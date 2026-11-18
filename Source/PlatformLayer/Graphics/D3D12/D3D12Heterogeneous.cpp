#include "D3D12Heterogeneous.h"
#include "D3D12Context.h"
#include "D3D12Memory.h"
#include "D3D12PipelineCache.h"
#include "D3D12Shader.h"
#include "D3D12StateTracker.h"
#include "D3D12HiZ.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT commandBytes=sizeof(D3D12HeterogeneousCommand);
struct alignas(256) Parameters
{
    se::VisibilityFrustum frustum;
    UINT count=0,padding[3]{};
    se::Mat4 clipFromWorld;
    se::Vec4 screen;
    UINT occlusionEnabled=0,occlusionPadding[3]{};
};
static_assert(sizeof(Parameters)==256&&offsetof(Parameters,count)==96&&offsetof(Parameters,clipFromWorld)==112
    &&offsetof(Parameters,screen)==176&&offsetof(Parameters,occlusionEnabled)==192);
D3D12HeterogeneousFallback EligibleRangeOnly(const D3D12HeterogeneousInput& item)
{
    using Reason=D3D12HeterogeneousFallback;
    const auto& mesh=*item.draw->mesh;
    const UINT stride=mesh.indexView.Format==DXGI_FORMAT_R16_UINT?2:mesh.indexView.Format==DXGI_FORMAT_R32_UINT?4:0;
    const UINT count=item.indexCount?item.indexCount:mesh.indexCount;
    if(item.baseVertex!=0||!stride||!count||count%3||item.firstIndex%3||(!item.indexCount&&item.firstIndex)
        ||item.firstIndex>mesh.indexCount||count>mesh.indexCount-item.firstIndex
        ||item.firstIndex>mesh.indexView.SizeInBytes/stride||count>mesh.indexView.SizeInBytes/stride-item.firstIndex)
        return Reason::InvalidGeometry;
    if(!se::IsValid(item.worldBounds))return Reason::InvalidBounds;
    return Reason::None;
}
bool ValidOcclusion(ID3D12Device& device,const D3D12HeterogeneousOcclusion& value)
{
    const auto inverse=se::Inverse(value.clipFromWorld);
    if(!value.output||!se::IsFinite(value.clipFromWorld)||!inverse||!se::IsFinite(*inverse)
        ||!se::IsFinite(value.depthBias)||value.depthBias<1e-6f||value.depthBias>.01f)return false;
    const auto& input=*value.output;
    if(!input.depth||!input.texture||!input.descriptors||!input.hzbSrv.ptr||!input.width||!input.height
        ||D3D12HiZ::Levels(input.width,input.height)!=input.mipCount)return false;
    const auto texture=input.texture->GetDesc(),depth=input.depth->GetDesc();const auto heap=input.descriptors->GetDesc();
    if(texture.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||texture.Width!=input.width||texture.Height!=input.height
        ||texture.MipLevels!=input.mipCount||texture.Format!=DXGI_FORMAT_R32_FLOAT||texture.DepthOrArraySize!=1||texture.SampleDesc.Count!=1
        ||depth.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||depth.Width!=input.width||depth.Height!=input.height
        ||depth.MipLevels!=1||depth.DepthOrArraySize!=1||depth.SampleDesc.Count!=1
        ||(depth.Format!=DXGI_FORMAT_R32_FLOAT&&depth.Format!=DXGI_FORMAT_R32_TYPELESS&&depth.Format!=DXGI_FORMAT_D32_FLOAT)
        ||heap.Type!=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV||!(heap.Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)||heap.NumDescriptors<input.mipCount+2)return false;
    ComPtr<ID3D12Device> owner;if(FAILED(input.texture->GetDevice(IID_PPV_ARGS(owner.GetAddressOf())))||owner.Get()!=&device)return false;
    owner.Reset();if(FAILED(input.depth->GetDevice(IID_PPV_ARGS(owner.GetAddressOf())))||owner.Get()!=&device)return false;
    owner.Reset();if(FAILED(input.descriptors->GetDevice(IID_PPV_ARGS(owner.GetAddressOf())))||owner.Get()!=&device)return false;
    return input.hzbSrv.ptr==input.descriptors->GetGPUDescriptorHandleForHeapStart().ptr+device.GetDescriptorHandleIncrementSize(heap.Type);
}
bool BufferRange(ID3D12Resource* resource,UINT64 address,UINT64 bytes)
{
    if(!resource||!bytes||resource->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)return false;
    const auto begin=resource->GetGPUVirtualAddress(),size=resource->GetDesc().Width;
    return begin&&address>=begin&&address-begin<=size&&bytes<=size-(address-begin);
}
const D3D12MeshPipeline* Pipeline(const D3D12HeterogeneousInput& item,const D3D12MeshPipeline& fallback)
{return item.draw&&item.draw->pipeline?item.draw->pipeline:&fallback;}
bool TrustedGeometryProgram(const D3D12ShaderSource& source,const std::filesystem::path& builtinDirectory)
{
    // 反射相同不代表 VS 没有位移。首版只接受运行程序自带的确定内建源；
    // 项目内同名文件、自定义 artifact/未知宏都不能借 basename 进入 GPU AABB 裁剪。
    if(builtinDirectory.empty()||source.path.empty())return false;
    const wchar_t* file=nullptr;
    switch(source.kind)
    {
    case se::rhi::PipelineKind::PbrMesh:file=L"Pbr.hlsl";break;
    case se::rhi::PipelineKind::ForwardMesh:file=L"Forward.hlsl";break;
    case se::rhi::PipelineKind::ShadowedMesh:file=L"ShadowedForward.hlsl";break;
    case se::rhi::PipelineKind::EnvironmentMesh:file=L"EnvironmentForward.hlsl";break;
    case se::rhi::PipelineKind::BasicLitMesh:file=L"BasicLit.hlsl";break;
    default:return false;
    }
    const auto expected=(builtinDirectory/file).lexically_normal().native();
    std::error_code error;
    const auto actual=std::filesystem::weakly_canonical(source.path,error).native();
    if(error)return false;
    if(actual.size()>INT_MAX||expected.size()>INT_MAX
        ||CompareStringOrdinal(actual.data(),static_cast<int>(actual.size()),expected.data(),static_cast<int>(expected.size()),TRUE)!=CSTR_EQUAL)
        return false;
    bool bc5=false;
    for(const auto& macro:source.macros)
    {
        // 这个既有宏只影响 PS 的法线解码；其余宏未证明不会改变几何。
        if(macro.name!="SE_NORMAL_BC5"||macro.value!="1"||bc5)return false;
        bc5=true;
    }
    return true;
}
bool SameBindings(const D3D12HeterogeneousInput& a,const D3D12HeterogeneousInput& b,const D3D12MeshPipeline& fallback)
{
    const auto* pa=Pipeline(a,fallback);const auto* pb=Pipeline(b,fallback);
    if(pa->prepared!=pb->prepared||pa->pipeline.Get()!=pb->pipeline.Get()||pa->rootSignature.Get()!=pb->rootSignature.Get())return false;
    if(!a.draw||!b.draw||!a.draw->mesh||!b.draw->mesh)return false;
    const auto& ma=*a.draw->mesh;const auto& mb=*b.draw->mesh;
    if(a.draw->textureIsData!=b.draw->textureIsData||ma.texture.Get()!=mb.texture.Get()||ma.textureViews!=mb.textureViews)return false;
    for(size_t slot=0;slot<ma.pbrTextures.size();++slot)
        if(ma.pbrTextures[slot].resource.Get()!=mb.pbrTextures[slot].resource.Get()
            ||ma.pbrTextures[slot].views!=mb.pbrTextures[slot].views)return false;
    // table 的 shadow/environment 尾槽由同帧提供；不跨帧、不跨 scene target 合桶。
    return true;
}
D3D12HeterogeneousFallback Eligible(const D3D12HeterogeneousInput& item,const D3D12MeshPipeline& fallback,
    bool trustedGeometry)
{
    using Reason=D3D12HeterogeneousFallback;
    if(!item.draw||!item.draw->mesh)return Reason::InvalidGeometry;
    const auto& draw=*item.draw;const auto& mesh=*draw.mesh;const auto* pipeline=Pipeline(item,fallback);
    if(draw.alphaMode!=se::MaterialAlphaMode::Opaque)return Reason::Alpha;
    if(!draw.instances.empty())return Reason::Instances;
    if(!draw.skinPalette.empty()||draw.skinJointCount||draw.skinJointOffset)return Reason::Skin;
    if(draw.outline)return Reason::Outline;
    if(draw.impostorView!=0||draw.impostorNext!=0||draw.impostorBlend!=0)return Reason::Impostor;
    if(!pipeline->prepared||!pipeline->prepared->program||!pipeline->pipeline||!pipeline->rootSignature)return Reason::UnsupportedPipeline;
    const auto& source=pipeline->prepared->program->source;const auto kind=source.kind;
    if(source.materialLayoutVersion||source.shadingLayoutVersion
        ||pipeline->prepared->materialRootParameter!=UINT_MAX||source.depthNormals)
        return Reason::MaterialExtension;
    // 源 AABB 尚无量化 decode 膨胀证明；压缩顶点与蒙皮/实例一样先走完整 CPU 路径。
    if(source.skinnedVertex||source.compressedVertex||se::rhi::IsInstanced(kind))return Reason::UnsupportedPipeline;
    if(kind!=se::rhi::PipelineKind::PbrMesh&&kind!=se::rhi::PipelineKind::ForwardMesh
        &&kind!=se::rhi::PipelineKind::ShadowedMesh&&kind!=se::rhi::PipelineKind::EnvironmentMesh
        &&kind!=se::rhi::PipelineKind::BasicLitMesh)return Reason::UnsupportedPipeline;
    if(source.layoutVersion!=se::rhi::BindingLayout(kind)||pipeline->prepared->blend!=se::rhi::BlendMode::Opaque
        ||pipeline->prepared->alphaMode!=se::MaterialAlphaMode::Opaque)return Reason::UnsupportedPipeline;
    if(!trustedGeometry)return Reason::UnsupportedPipeline;
    const UINT expectedStride=static_cast<UINT>(sizeof(se::rhi::Vertex));
    if(!mesh.vertices||!mesh.indices||mesh.vertexView.StrideInBytes!=expectedStride||!item.materialTable.ptr
        ||!mesh.vertexView.SizeInBytes||mesh.vertexView.SizeInBytes%expectedStride
        ||!BufferRange(mesh.vertices.Get(),mesh.vertexView.BufferLocation,mesh.vertexView.SizeInBytes)
        ||!BufferRange(mesh.indices.Get(),mesh.indexView.BufferLocation,mesh.indexView.SizeInBytes)
        ||!se::IsFinite(draw.world)||item.baseVertex!=0)return Reason::InvalidGeometry;
    const UINT stride=mesh.indexView.Format==DXGI_FORMAT_R16_UINT?2:mesh.indexView.Format==DXGI_FORMAT_R32_UINT?4:0;
    const UINT count=item.indexCount?item.indexCount:mesh.indexCount;
    if(!stride||!count||count%3||item.firstIndex%3||(!item.indexCount&&item.firstIndex)
        ||mesh.indexView.BufferLocation%stride||mesh.indexView.SizeInBytes%stride
        ||item.firstIndex>mesh.indexCount||count>mesh.indexCount-item.firstIndex
        ||item.firstIndex>mesh.indexView.SizeInBytes/stride||count>mesh.indexView.SizeInBytes/stride-item.firstIndex)
        return Reason::InvalidGeometry;
    if(!se::IsValid(item.worldBounds))return Reason::InvalidBounds;
    if(!item.b0.resource||item.b0.offset%256||item.b0.size<sizeof(se::rhi::PbrDrawConstants)
        ||item.b0.offset>item.b0.resource->GetDesc().Width
        ||item.b0.size>item.b0.resource->GetDesc().Width-item.b0.offset
        ||!BufferRange(item.b0.resource,item.b0.Gpu(),item.b0.size)||item.b0.Gpu()%256)
        return Reason::InvalidConstants;
    return Reason::None;
}
}
HRESULT D3D12Heterogeneous::Initialize(D3D12Context& context,const std::filesystem::path& shader,const Localization& text)
{
    if(device_)return E_UNEXPECTED;
    if(!context.device||!context.states||!context.memory)return E_INVALIDARG;
    ComPtr<ID3DBlob> code;
    if(!CompileD3D12Shader(shader,"Cull","cs_5_1",code,text,D3DCOMPILE_IEEE_STRICTNESS))return E_FAIL;
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_DESCRIPTOR_RANGE occlusionRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,1,0,D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[3].DescriptorTable={1,&occlusionRange};
    D3D12_ROOT_SIGNATURE_DESC description{};description.NumParameters=4;description.pParameters=parameters;
    ComPtr<ID3DBlob> serialized,error;
    auto result=D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,serialized.GetAddressOf(),error.GetAddressOf());
    if(FAILED(result))return result;
    ComPtr<ID3D12RootSignature> root;
    result=context.device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(root.GetAddressOf()));
    if(FAILED(result))return result;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.Get();pipeline.CS={code->GetBufferPointer(),code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> cull;
    result=context.device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(cull.GetAddressOf()));
    if(FAILED(result))return result;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    ComPtr<ID3D12DescriptorHeap> nullHeap;result=context.device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(nullHeap.GetAddressOf()));if(FAILED(result))return result;
    D3D12_SHADER_RESOURCE_VIEW_DESC nullView{};nullView.Format=DXGI_FORMAT_R32_FLOAT;nullView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    nullView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;nullView.Texture2D.MipLevels=1;
    context.device->CreateShaderResourceView(nullptr,&nullView,nullHeap->GetCPUDescriptorHandleForHeapStart());
    device_=context.device;memory_=context.memory;computeRoot_=std::move(root);cull_=std::move(cull);nullOcclusionHeap_=std::move(nullHeap);return S_OK;
}
HRESULT D3D12Heterogeneous::GetSignature(ID3D12RootSignature& root,ComPtr<ID3D12CommandSignature>& output)
{
    for(const auto& found:signatures_)if(found.root.Get()==&root){output=found.command;return S_OK;}
    D3D12_INDIRECT_ARGUMENT_DESC arguments[4]{};
    arguments[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;arguments[0].VertexBuffer.Slot=0;
    arguments[1].Type=D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    arguments[2].Type=D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;arguments[2].ConstantBufferView.RootParameterIndex=0;
    arguments[3].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC description{};description.ByteStride=commandBytes;description.NumArgumentDescs=4;description.pArgumentDescs=arguments;
    Signature next;next.root=&root;
    const auto result=device_->CreateCommandSignature(&description,&root,IID_PPV_ARGS(next.command.GetAddressOf()));
    if(FAILED(result))return result;
    signatures_.push_back(next);output=std::move(next.command);return S_OK;
}
HRESULT D3D12Heterogeneous::CreateOutput(UINT capacity,D3D12HeterogeneousOutput& output) const
{
    if(!device_)return E_UNEXPECTED;
    if(capacity>se::MaximumVisibilityItems)return E_INVALIDARG;
    D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width=UINT64{std::max(1u,capacity)}*commandBytes;description.Height=1;
    description.DepthOrArraySize=description.MipLevels=1;description.SampleDesc={1,0};
    description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;description.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12HeterogeneousOutput next;next.capacity=capacity;
    const auto result=memory_->Create(description,D3D12_RESOURCE_STATE_COMMON,nullptr,next.commands,"Heterogeneous.Commands");
    if(FAILED(result))return result;output=std::move(next);return S_OK;
}
HRESULT D3D12Heterogeneous::Prepare(D3D12UploadArena& arena,const D3D12MeshPipeline& defaultPipeline,
    std::span<const D3D12HeterogeneousInput> input,const se::VisibilityFrustum& frustum,UINT capacity,D3D12HeterogeneousPrepared& output,const D3D12HeterogeneousOcclusion* occlusion)
{
    if(!device_)return E_UNEXPECTED;
    if(input.size()>se::MaximumVisibilityItems||capacity>se::MaximumVisibilityItems||!se::ValidVisibilityFrustum(frustum))return E_INVALIDARG;
    // 每批只解析一次引擎目录；同一 program 只核对一次完整源路径和宏。
    std::error_code pathError;const auto executable=GetExecutableDirectory();
    auto builtinDirectory=executable.empty()?std::filesystem::path{}:
        std::filesystem::weakly_canonical(executable/"Resources/Shaders",pathError);
    if(pathError)builtinDirectory.clear();
    std::unordered_map<const D3D12ShaderProgram*,bool> trustedPrograms;
    const auto trusted=[&](const D3D12HeterogeneousInput& item)
    {
        const auto* pipeline=Pipeline(item,defaultPipeline);
        if(!pipeline->prepared||!pipeline->prepared->program)return false;
        const auto* program=pipeline->prepared->program.get();
        const auto [entry,inserted]=trustedPrograms.try_emplace(program,false);
        if(inserted)entry->second=TrustedGeometryProgram(program->source,builtinDirectory);
        return entry->second;
    };
    if(occlusion&&!ValidOcclusion(*device_.Get(),*occlusion))return E_INVALIDARG;
    D3D12HeterogeneousPrepared next;next.statistics_.sourceDraws=static_cast<UINT>(input.size());
    if(occlusion)next.occlusion_=occlusion->output;
    std::vector<D3D12HeterogeneousCommand> records;records.reserve(input.size());
    std::vector<se::VisibilityItem> bounds;bounds.reserve(input.size());
    for(UINT first=0;first<input.size();)
    {
        UINT end=first+1;
        while(end<input.size()&&SameBindings(input[first],input[end],defaultPipeline))++end;
        D3D12HeterogeneousSegment segment;segment.sourceFirst=first;segment.sourceCount=end-first;
        segment.pipeline=Pipeline(input[first],defaultPipeline);segment.materialTable=input[first].materialTable;
        UINT unsupported=0;
        const D3D12MeshDraw* verifiedDraw=nullptr;
        D3D12UploadSlice verifiedB0{};
        UINT64 verifiedTable=0;
        for(UINT index=first;index<end;++index)
        {
            const auto& cur=input[index];
            D3D12HeterogeneousFallback reason;
            if(verifiedDraw&&cur.draw==verifiedDraw&&cur.b0.resource==verifiedB0.resource
                &&cur.b0.offset==verifiedB0.offset&&cur.b0.size==verifiedB0.size&&cur.materialTable.ptr==verifiedTable)
            {
                reason=EligibleRangeOnly(cur);
            }
            else
            {
                reason=Eligible(cur,defaultPipeline,trusted(cur));
                if(reason==D3D12HeterogeneousFallback::None)
                {verifiedDraw=cur.draw;verifiedB0=cur.b0;verifiedTable=cur.materialTable.ptr;}
            }
            if(reason!=D3D12HeterogeneousFallback::None)
            {++unsupported;++next.statistics_.reasons[static_cast<size_t>(reason)];}
        }
        if(unsupported)
        {
            next.statistics_.unsupportedDraws+=unsupported;next.statistics_.peerFallbackDraws+=end-first-unsupported;
            next.statistics_.cpuDraws+=end-first;++next.statistics_.cpuBuckets;
        }
        else
        {
            if(end-first>capacity||records.size()>capacity-(end-first))return E_INVALIDARG;
            segment.cpuFallback=false;segment.commandFirst=static_cast<UINT>(records.size());segment.commandCount=end-first;
            segment.root=segment.pipeline->rootSignature;segment.preparedPipeline=segment.pipeline->prepared;
            const auto result=GetSignature(*segment.root.Get(),segment.signature);if(FAILED(result))return result;
            const D3D12MeshDraw* leasedDraw=nullptr;
            ID3D12Resource* leasedB0=nullptr;
            for(UINT index=first;index<end;++index)
            {
                const auto& item=input[index];const auto& mesh=*item.draw->mesh;
                D3D12HeterogeneousCommand command;command.vertices=mesh.vertexView;command.indices=mesh.indexView;command.b0=item.b0.Gpu();
                command.draw={item.indexCount?item.indexCount:mesh.indexCount,1,item.firstIndex,item.baseVertex,0};
                records.push_back(command);bounds.push_back({item.worldBounds.minimum,index,item.worldBounds.maximum,0});
                if(item.draw!=leasedDraw)
                {
                    leasedDraw=item.draw;
                    next.vertexLeases_.push_back(mesh.vertices);next.indexLeases_.push_back(mesh.indices);
                    if(mesh.texture)next.textureLeases_.push_back(mesh.texture);
                    if(mesh.textureViews)next.viewLeases_.push_back(mesh.textureViews);
                    for(const auto& texture:mesh.pbrTextures)
                    {if(texture.resource)next.textureLeases_.push_back(texture.resource);if(texture.views)next.viewLeases_.push_back(texture.views);}
                }
                if(item.b0.resource!=leasedB0)
                {
                    leasedB0=item.b0.resource;
                    next.constantLeases_.emplace_back(item.b0.resource);
                }
            }
            ++next.statistics_.gpuBuckets;
        }
        next.segments_.push_back(std::move(segment));first=end;
    }
    next.count_=static_cast<UINT>(records.size());next.statistics_.candidateCommands=next.count_;
    if(next.count_)
    {
        auto result=arena.Allocate(UINT64{next.count_}*commandBytes,16,next.inputCommands_);if(FAILED(result))return result;
        result=arena.Allocate(bounds.size()*sizeof(se::VisibilityItem),16,next.bounds_);if(FAILED(result))return result;
        result=arena.Allocate(sizeof(Parameters),256,next.constants_);if(FAILED(result))return result;
        std::memcpy(next.inputCommands_.cpu,records.data(),records.size()*commandBytes);
        std::memcpy(next.bounds_.cpu,bounds.data(),bounds.size()*sizeof(se::VisibilityItem));
        Parameters parameters{};parameters.frustum=frustum;parameters.count=next.count_;
        if(occlusion){parameters.clipFromWorld=occlusion->clipFromWorld;parameters.screen={float(occlusion->output->width),float(occlusion->output->height),occlusion->depthBias,float(occlusion->output->mipCount)};parameters.occlusionEnabled=1;}
        std::memcpy(next.constants_.cpu,&parameters,sizeof(parameters));
    }
    output=std::move(next);return S_OK;
}
HRESULT D3D12Heterogeneous::RecordCull(ID3D12GraphicsCommandList& commands,D3D12StateRecording& states,
    const D3D12HeterogeneousPrepared& prepared,const D3D12HeterogeneousOutput& output) const
{
    if(!device_)return E_UNEXPECTED;
    if(!prepared.count_)return S_OK;
    if(prepared.count_>output.capacity||output.capacity>se::MaximumVisibilityItems||!output.commands
        ||!BufferRange(output.commands.Get(),output.commands->GetGPUVirtualAddress(),UINT64{output.capacity}*commandBytes)
        ||!prepared.inputCommands_.resource||!prepared.bounds_.resource||!prepared.constants_.resource)return E_INVALIDARG;
    auto result=states.Use(commands,*prepared.inputCommands_.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,"Heterogeneous.Template");
    if(SUCCEEDED(result))result=states.Use(commands,*output.commands.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Heterogeneous.Template");
    if(FAILED(result))return result;
    commands.CopyBufferRegion(output.commands.Get(),0,prepared.inputCommands_.resource,prepared.inputCommands_.offset,UINT64{prepared.count_}*commandBytes);
    result=states.Use(commands,*prepared.bounds_.resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Heterogeneous.Bounds");
    if(SUCCEEDED(result))result=states.Use(commands,*prepared.constants_.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Heterogeneous.Constants");
    if(SUCCEEDED(result))result=states.Use(commands,*output.commands.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Heterogeneous.Cull");
    if(FAILED(result))return result;
    for(const auto& resource:prepared.vertexLeases_)
    {result=states.Use(commands,*resource.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Heterogeneous.Vertices");if(FAILED(result))return result;}
    for(const auto& resource:prepared.indexLeases_)
    {result=states.Use(commands,*resource.Get(),D3D12_RESOURCE_STATE_INDEX_BUFFER,"Heterogeneous.Indices");if(FAILED(result))return result;}
    for(const auto& resource:prepared.constantLeases_)
    {result=states.Use(commands,*resource.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Heterogeneous.DrawConstants");if(FAILED(result))return result;}
    for(const auto& resource:prepared.textureLeases_)
    {result=states.Use(commands,*resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Heterogeneous.Material");if(FAILED(result))return result;}
    commands.SetComputeRootSignature(computeRoot_.Get());commands.SetPipelineState(cull_.Get());
    commands.SetComputeRootConstantBufferView(0,prepared.constants_.Gpu());
    commands.SetComputeRootShaderResourceView(1,prepared.bounds_.Gpu());
    commands.SetComputeRootUnorderedAccessView(2,output.commands->GetGPUVirtualAddress());
    if(prepared.occlusion_)
    {
        result=states.Use(commands,*prepared.occlusion_->texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Heterogeneous.CurrentFrameHiZ");if(FAILED(result))return result;
        ID3D12DescriptorHeap* heaps[]{prepared.occlusion_->descriptors.Get()};commands.SetDescriptorHeaps(1,heaps);commands.SetComputeRootDescriptorTable(3,prepared.occlusion_->hzbSrv);
    }
    else {ID3D12DescriptorHeap* heaps[]{nullOcclusionHeap_.Get()};commands.SetDescriptorHeaps(1,heaps);commands.SetComputeRootDescriptorTable(3,nullOcclusionHeap_->GetGPUDescriptorHandleForHeapStart());}
    commands.Dispatch((prepared.count_+63)/64,1,1);
    return states.Use(commands,*output.commands.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,"Heterogeneous.Execute");
}
HRESULT D3D12Heterogeneous::RecordBucket(ID3D12GraphicsCommandList& commands,const D3D12HeterogeneousPrepared& prepared,
    const D3D12HeterogeneousOutput& output,UINT segmentIndex) const
{
    if(!device_)return E_UNEXPECTED;
    if(segmentIndex>=prepared.segments_.size())return E_INVALIDARG;
    const auto& segment=prepared.segments_[segmentIndex];
    if(segment.cpuFallback||!segment.commandCount||!segment.signature||!segment.materialTable.ptr||!output.commands
        ||segment.commandFirst>prepared.count_||segment.commandCount>prepared.count_-segment.commandFirst
        ||prepared.count_>output.capacity
        ||!BufferRange(output.commands.Get(),output.commands->GetGPUVirtualAddress(),UINT64{prepared.count_}*commandBytes))return E_INVALIDARG;
    commands.SetGraphicsRootDescriptorTable(1,segment.materialTable);
    commands.ExecuteIndirect(segment.signature.Get(),segment.commandCount,output.commands.Get(),UINT64{segment.commandFirst}*commandBytes,nullptr,0);
    return S_OK;
}
