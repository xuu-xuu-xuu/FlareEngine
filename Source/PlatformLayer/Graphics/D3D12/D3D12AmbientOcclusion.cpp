#include "D3D12AmbientOcclusion.h"
#include "D3D12Presentation.h"
#include "D3D12PipelineCache.h"
#include "D3D12Memory.h"
#include "D3D12Shader.h"
#include "D3D12Result.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <d3d12shader.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
namespace rhi=se::rhi;
using Microsoft::WRL::ComPtr;
namespace
{
using Json=nlohmann::json;
constexpr const char* entries[]{"GtaoHalf","ResolveOcclusion"};
std::string BlobHash(ID3DBlob& blob){return se::Sha256(std::span(static_cast<const std::uint8_t*>(blob.GetBufferPointer()),blob.GetBufferSize()));}
bool ReadFile(const std::filesystem::path& path,std::vector<std::uint8_t>& bytes)
{
    std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)return false;
    const auto count=input.tellg();if(count<=0||count>8*1024*1024)return false;bytes.resize(static_cast<size_t>(count));input.seekg(0);
    return bool(input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(count)));
}
Json Identity(const std::filesystem::path& source,const char* entry)
{
    std::vector<std::uint8_t> main,bindings;
    if(!ReadFile(source,main)||!ReadFile(source.parent_path()/"AmbientOcclusionBindings.hlsli",bindings))return {};
    return {{"schema",1},{"effect","RG104-08"},{"entry",entry},{"profile","cs_5_1"},{"constants",272},
        {"flags",D3D12ShaderCompileFlags()|D3DCOMPILE_IEEE_STRICTNESS},{"geometry_layout",rhi::DepthNormalLayoutVersion},
        {"source_sha256",se::Sha256(main)},{"bindings_sha256",se::Sha256(bindings)}};
}
bool ValidProgram(ID3DBlob& code,UINT stage)
{
    ComPtr<ID3D12ShaderReflection> reflection;
    if(FAILED(D3DReflect(code.GetBufferPointer(),code.GetBufferSize(),IID_PPV_ARGS(reflection.GetAddressOf()))))return false;
    D3D12_SHADER_DESC description{};UINT x=0,y=0,z=0;
    if(FAILED(reflection->GetDesc(&description))||D3D12_SHVER_GET_TYPE(description.Version)!=D3D12_SHVER_COMPUTE_SHADER
        ||D3D12_SHVER_GET_MAJOR(description.Version)!=5||D3D12_SHVER_GET_MINOR(description.Version)!=1)return false;
    reflection->GetThreadGroupSize(&x,&y,&z);if(x!=8||y!=8||z!=1)return false;
    auto* cb=reflection->GetConstantBufferByName("AmbientOcclusionConstants");D3D12_SHADER_BUFFER_DESC buffer{};
    if(!cb||FAILED(cb->GetDesc(&buffer))||buffer.Size!=272||buffer.Variables!=8)return false;
    const char* fields[]{"aoClipFromView","aoViewFromClip","aoViewFromWorld","aoViewport","aoParameters","contactParameters","aoLightDirection","aoModes"};
    const UINT offsets[]{0,64,128,192,208,224,240,256};
    for(UINT i=0;i<8;++i)
    {
        auto* field=cb->GetVariableByName(fields[i]);D3D12_SHADER_VARIABLE_DESC value{};D3D12_SHADER_TYPE_DESC type{};
        if(!field||FAILED(field->GetDesc(&value))||FAILED(field->GetType()->GetDesc(&type))||value.StartOffset!=offsets[i]
            ||value.Size!=(i<3?64u:16u)||type.Type!=D3D_SVT_FLOAT||type.Elements||type.Members
            ||type.Class!=(i<3?D3D_SVC_MATRIX_COLUMNS:D3D_SVC_VECTOR)||type.Rows!=(i<3?4u:1u)||type.Columns!=4)return false;
    }
    UINT resources=0;
    for(UINT i=0;i<description.BoundResources;++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC binding{};if(FAILED(reflection->GetResourceBindingDesc(i,&binding))||binding.Space||binding.BindCount!=1)return false;
        if(binding.Type==D3D_SIT_CBUFFER&&binding.BindPoint==0)resources|=1;
        else if(binding.Type==D3D_SIT_TEXTURE&&binding.Dimension==D3D_SRV_DIMENSION_TEXTURE2D&&binding.BindPoint<3)resources|=2u<<binding.BindPoint;
        else if(binding.Type==D3D_SIT_UAV_RWTYPED&&binding.Dimension==D3D_SRV_DIMENSION_TEXTURE2D&&binding.BindPoint==stage)resources|=16;
        else return false;
    }
    return resources==(stage?31u:23u);
}
void U32(std::vector<std::uint8_t>& bytes,UINT value){for(UINT shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>(value>>shift));}
UINT U32(const std::uint8_t* bytes){return UINT(bytes[0])|(UINT(bytes[1])<<8)|(UINT(bytes[2])<<16)|(UINT(bytes[3])<<24);}
std::filesystem::path Artifact(const std::filesystem::path& base,UINT stage)
{auto result=base;result+="."+std::string(entries[stage])+".seao";return result;}
bool Atomic(const std::filesystem::path& path,std::span<const std::uint8_t> bytes)
{
    std::error_code error;std::filesystem::create_directories(path.parent_path(),error);if(error||bytes.size()>MAXDWORD)return false;
    static std::atomic<UINT64> sequence{1};auto temporary=path;temporary+=L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(sequence++);
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;const bool success=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);
    const bool closed=CloseHandle(file)!=FALSE;
    if(!success||!closed||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temporary.c_str());return false;}return true;
}
bool ReadArtifact(const std::filesystem::path& artifact,const Json& identity,UINT stage,ComPtr<ID3DBlob>& output)
{
    std::vector<std::uint8_t> bytes;if(!ReadFile(artifact,bytes)||bytes.size()<16||std::memcmp(bytes.data(),"SEAO1\0\0",8))return false;
    const auto jsonBytes=U32(bytes.data()+8),codeBytes=U32(bytes.data()+12);
    if(!jsonBytes||!codeBytes||jsonBytes>65536||UINT64(16)+jsonBytes+codeBytes!=bytes.size())return false;
    try
    {
        const auto metadata=Json::parse(bytes.begin()+16,bytes.begin()+16+jsonBytes,
            [](int depth,Json::parse_event_t,Json&){if(depth>32)throw std::invalid_argument("AO metadata depth");return true;});
        if(metadata.at("identity")!=identity||FAILED(D3DCreateBlob(codeBytes,output.GetAddressOf())))return false;
        std::memcpy(output->GetBufferPointer(),bytes.data()+16+jsonBytes,codeBytes);
        return metadata.at("bytecode_sha256")==BlobHash(*output.Get())&&ValidProgram(*output.Get(),stage);
    }
    catch(const std::exception&){return false;}
}
D3D12_CPU_DESCRIPTOR_HANDLE Cpu(D3D12AmbientOcclusionOutput& output,UINT index)
{auto handle=output.descriptors->GetCPUDescriptorHandleForHeapStart();handle.ptr+=SIZE_T(index)*output.increment;return handle;}
D3D12_GPU_DESCRIPTOR_HANDLE Gpu(D3D12AmbientOcclusionOutput& output,UINT index)
{auto handle=output.descriptors->GetGPUDescriptorHandleForHeapStart();handle.ptr+=UINT64(index)*output.increment;return handle;}
}
bool WriteD3D12AmbientOcclusionShaders(const std::filesystem::path& source,const std::filesystem::path& outputBase,const Localization& text)
{
    for(UINT stage=0;stage<2;++stage)
    {
        const auto identity=Identity(source,entries[stage]);ComPtr<ID3DBlob> code;if(identity.is_null()
            ||!CompileD3D12Shader(source,entries[stage],"cs_5_1",code,text,D3DCOMPILE_IEEE_STRICTNESS)||!ValidProgram(*code.Get(),stage))return false;
        const auto metadata=Json{{"identity",identity},{"bytecode_sha256",BlobHash(*code.Get())}}.dump(2);
        std::vector<std::uint8_t> bytes{'S','E','A','O','1',0,0,0};U32(bytes,static_cast<UINT>(metadata.size()));U32(bytes,static_cast<UINT>(code->GetBufferSize()));
        bytes.insert(bytes.end(),metadata.begin(),metadata.end());const auto* data=static_cast<const std::uint8_t*>(code->GetBufferPointer());bytes.insert(bytes.end(),data,data+code->GetBufferSize());
        const auto artifact=Artifact(outputBase,stage);auto manifest=artifact;manifest+=".json";
        if(!Atomic(manifest,std::span(reinterpret_cast<const std::uint8_t*>(metadata.data()),metadata.size()))||!Atomic(artifact,bytes))return false;
    }
    return true;
}
HRESULT D3D12AmbientOcclusion::Initialize(ID3D12Device& device,const std::filesystem::path& shaders,const Localization& text)
{
    if(device_)return E_UNEXPECTED;std::array<ComPtr<ID3DBlob>,2> code;
    const auto source=shaders/"AmbientOcclusion.hlsl";
    for(UINT stage=0;stage<2;++stage)
    {
        const auto identity=Identity(source,entries[stage]);if(identity.is_null())return E_INVALIDARG;
        const auto artifact=Artifact(source,stage);std::error_code error;
        if(std::filesystem::exists(artifact,error)&&!error)
        {
            if(ReadArtifact(artifact,identity,stage,code[stage]))++statistics_.artifactLoads;
            else {code[stage].Reset();++statistics_.artifactRejections;Log(LogLevel::Warning,text.GetText("Shader.ArtifactRejected"));}
        }
        if(!code[stage])
        {
            ++statistics_.compilations;
            if(!CompileD3D12Shader(source,entries[stage],"cs_5_1",code[stage],text,D3DCOMPILE_IEEE_STRICTNESS)||!ValidProgram(*code[stage].Get(),stage))return E_FAIL;
        }
    }
    D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},uav{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER parameters[3]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[0].Descriptor={0,0};
    parameters[1].ParameterType=parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable={1,&srv};parameters[2].DescriptorTable={1,&uav};D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=3;root.pParameters=parameters;
    ComPtr<ID3DBlob> bytes,error;auto result=D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,bytes.GetAddressOf(),error.GetAddressOf());
    if(SUCCEEDED(result))result=device.CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(root_.GetAddressOf()));if(FAILED(result))return result;
    for(UINT stage=0;stage<2;++stage)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};description.pRootSignature=root_.Get();description.CS={code[stage]->GetBufferPointer(),code[stage]->GetBufferSize()};
        result=device.CreateComputePipelineState(&description,IID_PPV_ARGS(stages_[stage].GetAddressOf()));if(FAILED(result))return result;
    }
    device_=&device;return S_OK;
}
bool D3D12AmbientOcclusion::CreateOutput(D3D12Presentation& p,std::shared_ptr<D3D12DepthNormalTarget> inputs,bool half,
    std::shared_ptr<D3D12AmbientOcclusionOutput>& output,const Localization& text)const
{
    if(!device_||!inputs)return false;auto candidate=std::make_shared<D3D12AmbientOcclusionOutput>();candidate->inputs=std::move(inputs);
    candidate->width=candidate->inputs->width;candidate->height=candidate->inputs->height;candidate->halfWidth=(candidate->width+1)/2;candidate->halfHeight=(candidate->height+1)/2;candidate->hasHalf=half;
    const auto create=[&](UINT width,UINT height,ComPtr<ID3D12Resource>& resource,const char* name)
    {
        D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;description.Width=width;description.Height=height;description.DepthOrArraySize=description.MipLevels=1;
        description.Format=DXGI_FORMAT_R16G16_FLOAT;description.SampleDesc={1,0};description.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if(FAILED(p.memory->Create(description,D3D12_RESOURCE_STATE_COMMON,nullptr,resource,name)))return false;
        const auto allocation=p.memory->Allocation(*resource.Get());if(!allocation)return false;candidate->allocatedBytes+=allocation->bytes;return true;
    };
    if(!create(candidate->width,candidate->height,candidate->visibility,"ScreenSpace.Visibility")
        ||(half&&!create(candidate->halfWidth,candidate->halfHeight,candidate->half,"ScreenSpace.HalfAo")))return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,5,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(candidate->descriptors.GetAddressOf())))
        ||!CheckGraphicsResult(p.srvPool->Allocate(1,candidate->graphicsView),text,"D3D12.DescriptorCapacityExceeded"))return false;
    candidate->increment=device_->GetDescriptorHandleIncrementSize(heap.Type);
    device_->CopyDescriptorsSimple(1,Cpu(*candidate,0),candidate->inputs->views->Cpu(),heap.Type);
    device_->CopyDescriptorsSimple(1,Cpu(*candidate,1),candidate->inputs->views->Cpu(1),heap.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R16G16_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    device_->CreateShaderResourceView(half?candidate->half.Get():candidate->inputs->normal.Get(),&srv,Cpu(*candidate,2));
    device_->CreateShaderResourceView(candidate->visibility.Get(),&srv,candidate->graphicsView->Cpu());
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R16G16_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    device_->CreateUnorderedAccessView(candidate->half.Get(),nullptr,&uav,Cpu(*candidate,3));device_->CreateUnorderedAccessView(candidate->visibility.Get(),nullptr,&uav,Cpu(*candidate,4));
    output=std::move(candidate);return true;
}
bool ValidD3D12ScreenSpaceSettings(const rhi::ScreenSpaceOcclusionSettings& s)
{
    const auto inverse=se::Inverse(s.clipFromView);
    if(!se::IsFinite(s.view)||!se::IsFinite(s.clipFromView)||!inverse||!se::IsFinite(*inverse)||!se::IsFinite(s.radius)||s.radius<=0||s.radius>=100
        ||!se::IsFinite(s.intensity)||s.intensity<0||s.intensity>4||!se::IsFinite(s.bias)||s.bias<0||s.bias>=1
        ||!se::IsFinite(s.contactLength)||s.contactLength<=0||s.contactLength>=100||!se::IsFinite(s.contactBias)||s.contactBias<0||s.contactBias>=1
        ||!se::IsFinite(s.contactThickness)||s.contactThickness<=0||s.contactThickness>=100||!se::IsFinite(s.contactFade)||s.contactFade<0||s.contactFade>1)return false;
    if(std::abs(s.view(3,0))>1e-5f||std::abs(s.view(3,1))>1e-5f||std::abs(s.view(3,2))>1e-5f||std::abs(s.view(3,3)-1)>1e-5f)return false;
    const se::Vec3 x{s.view(0,0),s.view(0,1),s.view(0,2)},y{s.view(1,0),s.view(1,1),s.view(1,2)},z{s.view(2,0),s.view(2,1),s.view(2,2)};
    return std::abs(se::LengthSquared(x)-1)<.001f&&std::abs(se::LengthSquared(y)-1)<.001f&&std::abs(se::LengthSquared(z)-1)<.001f
        &&std::abs(se::Dot(x,y))<.001f&&std::abs(se::Dot(x,z))<.001f&&std::abs(se::Dot(y,z))<.001f&&se::Dot(se::Cross(x,y),z)>.999f;
}
bool PrepareD3D12AmbientOcclusion(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,UINT width,UINT height,const Localization& text)
{
    frame.ambientOcclusion.reset();p.screenSpaceStatistics={};const auto* settings=content.screenSpaceOcclusion;
    if(!settings||(!settings->gtao&&!settings->contactShadows)){p.ambientOcclusionOutput.reset();return true;}
    auto candidate=std::make_shared<D3D12AmbientOcclusionFrame>();auto& stats=candidate->statistics;
    stats.requested=true;stats.width=width;stats.height=height;stats.halfWidth=(width+1)/2;stats.halfHeight=(height+1)/2;
    const auto fallback=[&](rhi::ScreenSpaceOcclusionFallback reason)
    {candidate->output.reset();p.ambientOcclusionOutput.reset();stats.allocatedBytes=0;stats.fallback=reason;p.screenSpaceStatistics=stats;frame.ambientOcclusion=std::move(candidate);return true;};
    if(!ValidD3D12ScreenSpaceSettings(*settings))return fallback(rhi::ScreenSpaceOcclusionFallback::InvalidCamera);
    if(!content.forward||!content.forward->shading.enabled)return fallback(rhi::ScreenSpaceOcclusionFallback::UnsupportedPipeline);
    if(!frame.depthNormals||!frame.depthNormals->statistics.valid)return fallback(rhi::ScreenSpaceOcclusionFallback::MissingDepthNormals);
    if(frame.depthNormals->target->width!=width||frame.depthNormals->target->height!=height)return fallback(rhi::ScreenSpaceOcclusionFallback::MissingDepthNormals);
    if(!frame.depthNormals->statistics.complete)return fallback(rhi::ScreenSpaceOcclusionFallback::IncompleteGeometry);
    const bool ao=settings->gtao,contact=settings->contactShadows&&content.forward&&content.forward->directionalEnabled;
    if(!ao&&!contact)return fallback(rhi::ScreenSpaceOcclusionFallback::None);
    if(!content.meshPipeline||!content.forward)return fallback(rhi::ScreenSpaceOcclusionFallback::UnsupportedPipeline);
    if(!p.ambientOcclusionBuilder)
    {
        auto builder=std::make_shared<D3D12AmbientOcclusion>();ComPtr<ID3D12Device> device;
        if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))||FAILED(builder->Initialize(*device.Get(),content.meshPipeline->prepared->program->source.path.parent_path(),text)))
            return fallback(rhi::ScreenSpaceOcclusionFallback::InitializationFailed);
        p.ambientOcclusionBuilder=std::move(builder);
    }
    if(!p.ambientOcclusionOutput||p.ambientOcclusionOutput->inputs!=frame.depthNormals->target||p.ambientOcclusionOutput->hasHalf!=ao)
    {
        std::shared_ptr<D3D12AmbientOcclusionOutput> next;
        if(!p.ambientOcclusionBuilder->CreateOutput(p,frame.depthNormals->target,ao,next,text))return fallback(rhi::ScreenSpaceOcclusionFallback::ResourceFailure);
        p.ambientOcclusionOutput=std::move(next);
    }
    candidate->output=p.ambientOcclusionOutput;const auto inverse=se::Inverse(settings->clipFromView);if(!inverse||!se::IsFinite(*inverse))return fallback(rhi::ScreenSpaceOcclusionFallback::InvalidCamera);
    const auto direction=se::Normalize(content.forward->directionToLight);const auto transformed=settings->view*se::Vec4{direction.x,direction.y,direction.z,0};
    const D3D12AmbientOcclusionConstants constants{settings->clipFromView,*inverse,settings->view,{float(width),float(height),float(stats.halfWidth),float(stats.halfHeight)},
        {settings->radius,settings->intensity,settings->bias,0},{settings->contactLength,settings->contactBias,settings->contactThickness,settings->contactFade},
        {transformed.x,transformed.y,transformed.z,contact?1.f:0.f},{ao?1.f:0.f,contact?1.f:0.f,0,0}};
    if(FAILED(frame.uploads.Allocate(512,256,candidate->constants)))return fallback(rhi::ScreenSpaceOcclusionFallback::ResourceFailure);
    std::memset(candidate->constants.cpu,0,512);std::memcpy(candidate->constants.cpu,&constants,sizeof(constants));
    stats.valid=true;stats.aoValid=ao;stats.contactValid=contact;stats.allocatedBytes=candidate->output->allocatedBytes;
    p.screenSpaceStatistics=stats;frame.ambientOcclusion=std::move(candidate);return true;
}
bool D3D12AmbientOcclusion::Record(D3D12Presentation& p,D3D12FrameContext& frame,D3D12StateRecording& states,const Localization& text)const
{
    if(!frame.ambientOcclusion||!frame.ambientOcclusion->statistics.valid)return true;
    auto& data=*frame.ambientOcclusion;auto& output=*data.output;auto& commands=*p.commandList.Get();
    const auto use=[&](ID3D12Resource& resource,D3D12_RESOURCE_STATES state,const char* name){return CheckGraphicsResult(states.Use(commands,resource,state,name),text,"D3D12.StateTrackingFailed");};
    if(!use(*output.inputs->depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"ScreenSpace.Depth")
        ||!use(*output.inputs->normal.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"ScreenSpace.Normal")
        ||!use(*data.constants.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"ScreenSpace.Constants"))return false;
    ID3D12DescriptorHeap* heaps[]{output.descriptors.Get()};commands.SetDescriptorHeaps(1,heaps);commands.SetComputeRootSignature(root_.Get());
    commands.SetComputeRootConstantBufferView(0,data.constants.Gpu());commands.SetComputeRootDescriptorTable(1,Gpu(output,0));commands.SetComputeRootDescriptorTable(2,Gpu(output,3));
    if(output.hasHalf)
    {
        if(p.timingEnabled)commands.EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,25);
        if(!use(*output.half.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"ScreenSpace.HalfAo"))return false;
        commands.SetPipelineState(stages_[0].Get());commands.Dispatch((output.halfWidth+7)/8,(output.halfHeight+7)/8,1);
        if(!CheckGraphicsResult(states.UavBarrier(commands,*output.half.Get(),"ScreenSpace.HalfAoComplete"),text,"D3D12.StateTrackingFailed")
            ||!use(*output.half.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"ScreenSpace.BilateralInput"))return false;
        if(p.timingEnabled)commands.EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,26);
    }
    if(p.timingEnabled)commands.EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,27);
    if(!use(*output.visibility.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"ScreenSpace.Resolve"))return false;
    commands.SetPipelineState(stages_[1].Get());commands.Dispatch((output.width+7)/8,(output.height+7)/8,1);
    if(!CheckGraphicsResult(states.UavBarrier(commands,*output.visibility.Get(),"ScreenSpace.ResolveComplete"),text,"D3D12.StateTrackingFailed")
        ||!use(*output.visibility.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"ScreenSpace.LightingInput"))return false;
    if(p.timingEnabled)commands.EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,28);
    return true;
}
bool RecordD3D12AmbientOcclusion(D3D12Presentation& p,D3D12FrameContext& frame,D3D12StateRecording& states,const Localization& text)
{return frame.ambientOcclusion&&frame.ambientOcclusion->statistics.valid
    ?p.ambientOcclusionBuilder&&p.ambientOcclusionBuilder->Record(p,frame,states,text):true;}
D3D12_CPU_DESCRIPTOR_HANDLE GetD3D12AmbientOcclusionView(const D3D12FrameContext& frame)
{return frame.ambientOcclusion&&frame.ambientOcclusion->statistics.valid?frame.ambientOcclusion->output->graphicsView->Cpu():D3D12_CPU_DESCRIPTOR_HANDLE{};}
