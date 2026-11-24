#include "D3D12DepthNormals.h"
#include "D3D12Presentation.h"
#include "D3D12PipelineCache.h"
#include "D3D12Memory.h"
#include "D3D12Result.h"
#include "D3D12Shader.h"
#include "CoreLayer/Hash.h"
#include <algorithm>
#include <filesystem>
namespace rhi=flare::rhi;
namespace
{
// 只认随产品发布的已知内建源文件；用户 Source/Artifact 不推测覆盖语义。
bool BuiltinCoverage(const D3D12ShaderSource& source,const std::filesystem::path& expectedShaderDir)
{
    if(expectedShaderDir.empty())return false;
    std::error_code error;const auto actual=std::filesystem::weakly_canonical(source.path.parent_path(),error);
    if(error||_wcsicmp(actual.c_str(),expectedShaderDir.c_str()))return false;
    const std::string name=source.path.filename().string();
    static constexpr const char* names[]{"Pbr.hlsl","Forward.hlsl","ShadowedForward.hlsl","EnvironmentForward.hlsl","BasicLit.hlsl","SkinnedForward.hlsl","SkinnedEnvironmentForward.hlsl",
        "CascadedShadowedForward.hlsl","CascadedEnvironmentForward.hlsl","CascadedBasicLit.hlsl","CascadedSkinnedForward.hlsl","CascadedSkinnedEnvironmentForward.hlsl",
        "ClusteredForward.hlsl","ClusteredShadowedForward.hlsl","ClusteredEnvironmentForward.hlsl","ClusteredBasicLit.hlsl","ClusteredSkinnedForward.hlsl","ClusteredSkinnedShadowedForward.hlsl","ClusteredSkinnedEnvironmentForward.hlsl",
        "ClusteredCascadedShadowedForward.hlsl","ClusteredCascadedEnvironmentForward.hlsl","ClusteredCascadedBasicLit.hlsl","ClusteredCascadedSkinnedForward.hlsl","ClusteredCascadedSkinnedEnvironmentForward.hlsl",
        "PcssCascadedShadowedForward.hlsl","PcssCascadedEnvironmentForward.hlsl","PcssCascadedBasicLit.hlsl","PcssCascadedSkinnedForward.hlsl","PcssCascadedSkinnedEnvironmentForward.hlsl",
        "PcssClusteredCascadedShadowedForward.hlsl","PcssClusteredCascadedEnvironmentForward.hlsl","PcssClusteredCascadedBasicLit.hlsl","PcssClusteredCascadedSkinnedForward.hlsl","PcssClusteredCascadedSkinnedEnvironmentForward.hlsl"};
    if(std::find(std::begin(names),std::end(names),name)==std::end(names))return false;
    return std::all_of(source.macros.begin(),source.macros.end(),[](const auto& macro)
    {
        if(macro.name=="SE_CSM_RECEIVER_PLANE")return macro.value=="0"||macro.value=="1";
        return macro.value=="1"&&(macro.name=="SE_MATERIAL_SURFACE"||macro.name=="SE_SKINNED"||macro.name=="SE_VERTEX_COMPRESSED"||macro.name=="SE_NORMAL_BC5"||macro.name=="SE_SHADING_EXTENSIONS");
    });
}
bool SupportedPrepared(const D3D12PreparedPipeline& prepared,const std::filesystem::path& expectedShaderDir)
{
    if(!prepared.program)return false;
    const auto& source=prepared.program->source;
    return rhi::UsesLitLayout(source.kind)&&!rhi::IsInstanced(source.kind)&&prepared.depth==rhi::DepthMode::ReadWriteLess
        &&prepared.blend==rhi::BlendMode::Opaque&&prepared.description.SampleDesc.Count==1&&prepared.description.DSVFormat==DXGI_FORMAT_D32_FLOAT
        &&(source.materialLayoutVersion==rhi::UserSurfaceMaterialLayoutVersion||BuiltinCoverage(source,expectedShaderDir));
}
bool CreateTarget(D3D12Presentation& p,ID3D12Device& device,UINT width,UINT height,bool material,std::shared_ptr<D3D12DepthNormalTarget>& output,const Localization& text)
{
    auto candidate=std::make_shared<D3D12DepthNormalTarget>();candidate->width=width;candidate->height=height;
    D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;description.Width=width;description.Height=height;
    description.DepthOrArraySize=description.MipLevels=1;description.SampleDesc={1,0};description.Format=DXGI_FORMAT_R32_TYPELESS;description.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_D32_FLOAT;clear.DepthStencil.Depth=1;
    if(!CheckGraphicsResult(p.memory->Create(description,D3D12_RESOURCE_STATE_COMMON,&clear,candidate->depth,"DepthNormals.Depth"),text,"D3D12.CreateDepthFailed"))return false;
    const auto depthAllocation=p.memory->Allocation(*candidate->depth.Get());if(!depthAllocation)return false;
    candidate->allocatedBytes=depthAllocation->bytes;
    description.Format=DXGI_FORMAT_R16G16_FLOAT;description.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clear={};clear.Format=DXGI_FORMAT_R16G16_FLOAT;clear.Color[0]=clear.Color[1]=2;
    if(!CheckGraphicsResult(p.memory->Create(description,D3D12_RESOURCE_STATE_COMMON,&clear,candidate->normal,"DepthNormals.WorldGeometricOct"),text,"D3D12.PostTargetsFailed"))return false;
    const auto normalAllocation=p.memory->Allocation(*candidate->normal.Get());if(!normalAllocation)return false;
    candidate->allocatedBytes+=normalAllocation->bytes;
    if(material)
    {
        description.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        clear={};clear.Format=description.Format;clear.Color[3]=-1;
        if(!CheckGraphicsResult(p.memory->Create(description,D3D12_RESOURCE_STATE_COMMON,&clear,candidate->material,"DepthNormals.Material"),text,"D3D12.PostTargetsFailed"))return false;
        const auto allocation=p.memory->Allocation(*candidate->material.Get());if(!allocation)return false;
        candidate->allocatedBytes+=allocation->bytes;
    }
    if(!CheckGraphicsResult(p.dsvPool->Allocate(1,candidate->dsv),text,"D3D12.DescriptorCapacityExceeded")
        ||!CheckGraphicsResult(p.rtvPool->Allocate(material?2:1,candidate->rtv),text,"D3D12.DescriptorCapacityExceeded")
        ||!CheckGraphicsResult(p.srvPool->Allocate(material?3:2,candidate->views),text,"D3D12.DescriptorCapacityExceeded"))return false;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    device.CreateDepthStencilView(candidate->depth.Get(),&dsv,candidate->dsv->Cpu());device.CreateRenderTargetView(candidate->normal.Get(),nullptr,candidate->rtv->Cpu());
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_FLOAT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
    device.CreateShaderResourceView(candidate->depth.Get(),&view,candidate->views->Cpu());view.Format=DXGI_FORMAT_R16G16_FLOAT;
    device.CreateShaderResourceView(candidate->normal.Get(),&view,candidate->views->Cpu(1));
    if(material)
    {
        device.CreateRenderTargetView(candidate->material.Get(),nullptr,candidate->rtv->Cpu(1));view.Format=description.Format;
        device.CreateShaderResourceView(candidate->material.Get(),&view,candidate->views->Cpu(2));
    }
    output=std::move(candidate);return true;
}
}
bool D3D12DepthNormals::Prepare(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,UINT width,UINT height,const Localization& text)
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))return false;
    const bool material=content.forward&&(content.forward->indoor.SurfaceEnabled()||content.forward->rayTracing.IndirectEnabled());
    if(!p.depthNormalTarget||p.depthNormalTarget->width!=width||p.depthNormalTarget->height!=height||bool(p.depthNormalTarget->material)!=material)
        if(!CreateTarget(p,*device.Get(),width,height,material,p.depthNormalTarget,text))return false;
    auto candidate=std::make_shared<D3D12DepthNormalFrame>();candidate->target=p.depthNormalTarget;
    auto& statistics=candidate->statistics;statistics.requested=statistics.valid=true;statistics.complete=true;statistics.allocatedBytes=candidate->target->allocatedBytes;
    const auto count=std::min(content.opaqueDrawCount,content.meshes.size());candidate->pipelines.resize(count);
    std::erase_if(pipelines_,[](const auto& entry){return entry.second.expired();});
    std::filesystem::path expectedShaderDir;
    {
        std::array<wchar_t,32768> executable{};
        if(GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size())))
        {
            std::error_code error;
            expectedShaderDir=std::filesystem::weakly_canonical(std::filesystem::path(executable.data()).parent_path()/"Resources/Shaders",error);
            if(error)expectedShaderDir.clear();
        }
    }
    struct CachedPreparedResult
    {
        bool checked=false,supported=false;
        std::shared_ptr<const D3D12DepthNormalPipeline> pair;
    };
    std::unordered_map<const D3D12PreparedPipeline*,CachedPreparedResult> framePreparedCache;
    for(size_t index=0;index<count;++index)
    {
        const auto& draw=content.meshes[index];const auto* main=draw.pipeline?draw.pipeline:content.meshPipeline;
        if(!main||!main->prepared||!main->prepared->program||!draw.instances.empty()||draw.alphaMode==flare::MaterialAlphaMode::Blend)
        {statistics.complete=false;++statistics.unsupportedDraws;continue;}
        const auto& prepared=*main->prepared;
        auto& cached=framePreparedCache[&prepared];
        if(!cached.checked)
        {
            cached.checked=true;
            cached.supported=SupportedPrepared(prepared,expectedShaderDir);
            if(cached.supported)
            {
                const auto& source=prepared.program->source;
                const bool basic=rhi::IsBasicLit(source.kind),surface=prepared.materialRootParameter!=UINT_MAX;
                const bool userSurface=source.materialLayoutVersion==rhi::UserSurfaceMaterialLayoutVersion&&!basic;
                const auto& builtinDir=expectedShaderDir.empty()?source.path.parent_path():expectedShaderDir;
                const UINT slot=(basic?2u:0u)+(surface?1u:0u);
                Microsoft::WRL::ComPtr<ID3DBlob> userPixelBlob;
                std::string pixelIdentity;
                if(userSurface)
                {
                    const bool forward=rhi::IsForward(source.kind),shading=prepared.shadingFrameParameter!=UINT_MAX;
                    const bool normalBc5=std::any_of(source.macros.begin(),source.macros.end(),[](const auto& m){return m.name=="SE_NORMAL_BC5"&&m.value=="1";});
                    const auto userKey=flare::Sha256(prepared.program->bytecodeKey+(material?":user-ps-indoor:v2":":user-ps-main:v2")
                        +(forward?":fwd":"")+(shading?":shd":"")+(normalBc5?":bc5":""));
                    auto foundBlob=userPixels_.find(userKey);
                    if(foundBlob!=userPixels_.end())userPixelBlob=foundBlob->second;
                    else
                    {
                        std::vector<D3D_SHADER_MACRO> defines{{"SE_DEPTH_NORMALS","1"},{"SE_MATERIAL_SURFACE","1"},{"SE_USER_SURFACE","1"}};
                        if(forward)defines.push_back({"SE_FORWARD","1"});
                        if(shading)defines.push_back({"SE_SHADING_EXTENSIONS","1"});
                        if(normalBc5)defines.push_back({"SE_NORMAL_BC5","1"});
                        defines.push_back({nullptr,nullptr});
                        const auto userPath=source.path.parent_path()/"DepthNormals.hlsl";
                        if(!CompileD3D12Shader(userPath,material?"PSIndoor":"PSMain","ps_5_1",userPixelBlob,text,0,defines.data()))return false;
                        userPixels_.emplace(userKey,userPixelBlob);
                    }
                    pixelIdentity=userKey;
                }
                else
                {
                    if(!pixels_[slot])
                    {
                        rhi::PipelineDescription description{rhi::PipelineKind::PbrMesh,builtinDir/(basic?"DepthNormalsBasic.hlsl":"DepthNormals.hlsl"),rhi::PbrMaterialLayoutVersion};
                        description.macros.push_back({"SE_DEPTH_NORMALS","1"});description.materialLayoutVersion=surface?rhi::MaterialSurfaceLayoutVersion:0;
                        D3D12ShaderSource builtinSource;if(!ReadD3D12ShaderSource(description,builtinSource,text))return false;
                        pixels_[slot]=LoadD3D12ShaderProgram(std::move(builtinSource),shaderStatistics_,text);if(!pixels_[slot])return false;
                    }
                    if(material&&!indoorPixels_[slot])
                    {
                        const D3D_SHADER_MACRO defines[]{{"SE_MATERIAL_SURFACE","1"},{nullptr,nullptr}};
                        const auto path=builtinDir/(basic?"DepthNormalsBasic.hlsl":"DepthNormals.hlsl");
                        if(!CompileD3D12Shader(path,"PSIndoor","ps_5_1",indoorPixels_[slot],text,0,surface?defines:nullptr))return false;
                    }
                    pixelIdentity=pixels_[slot]->bytecodeKey;
                }
                const auto key=flare::Sha256(prepared.key+":"+pixelIdentity+(material?":indoor-material:v2":":geometry39:independent-d32-rg16-less:v2"));
                auto found=pipelines_.find(key);std::shared_ptr<const D3D12DepthNormalPipeline> pair=found==pipelines_.end()?nullptr:found->second.lock();
                if(!pair)
                {
                    auto next=std::make_shared<D3D12DepthNormalPipeline>();next->main=main->prepared;if(!userSurface)next->pixel=pixels_[slot];
                    auto derived=std::make_shared<D3D12PreparedPipeline>(prepared);derived->key=key;
                    auto description=prepared.description;
                    if(userSurface)
                    {
                        description.PS={userPixelBlob->GetBufferPointer(),userPixelBlob->GetBufferSize()};
                    }
                    else
                    {
                        description.PS={next->pixel->pixel->GetBufferPointer(),next->pixel->pixel->GetBufferSize()};
                    }
                    description.NumRenderTargets=1;description.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;description.CachedPSO={};
                    description.BlendState.RenderTarget[0].BlendEnable=FALSE;description.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_RED|D3D12_COLOR_WRITE_ENABLE_GREEN;
                    if(material)
                    {
                        if(!userSurface)
                        {
                            auto* pixel=indoorPixels_[slot].Get();description.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
                        }
                        description.NumRenderTargets=2;description.RTVFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;
                        description.BlendState.IndependentBlendEnable=TRUE;
                        description.BlendState.RenderTarget[1]=description.BlendState.RenderTarget[0];
                        description.BlendState.RenderTarget[1].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
                    }
                    description.DepthStencilState.DepthEnable=TRUE;description.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;description.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;
                    derived->pipeline.Reset();if(!CheckGraphicsResult(device->CreateGraphicsPipelineState(&description,IID_PPV_ARGS(derived->pipeline.GetAddressOf())),text,"D3D12.CreatePipelineFailed"))return false;
                    derived->description=description;derived->description.InputLayout={derived->inputElements.data(),static_cast<UINT>(derived->inputElements.size())};
                    derived->pipeline->SetName(L"FlareEngine.DepthNormals.SameMainVertexShader");next->mesh={derived->rootSignature,derived->pipeline,derived};
                    pair=std::move(next);pipelines_.insert_or_assign(key,pair);++pipelineCreations_;
                }
                cached.pair=std::move(pair);
            }
        }
        if(!cached.supported){statistics.complete=false;++statistics.unsupportedDraws;continue;}
        candidate->pipelines[index]=cached.pair;++statistics.draws;
    }
    frame.depthNormals=std::move(candidate);p.depthNormalStatistics=statistics;return true;
}
bool D3D12DepthNormals::Record(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,D3D12StateRecording& states,const Localization& text) const
{
    if(!frame.depthNormals)return false;const auto& target=*frame.depthNormals->target;auto& commands=*p.commandList.Get();
    if(!CheckGraphicsResult(states.Use(commands,*target.depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,"DepthNormals.Depth"),text,"D3D12.StateTrackingFailed")
        ||!CheckGraphicsResult(states.Use(commands,*target.normal.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"DepthNormals.Normal"),text,"D3D12.StateTrackingFailed"))return false;
    const auto dsv=target.dsv->Cpu(),rtv=target.rtv->Cpu();const float invalidNormal[4]{2,2,0,0};
    commands.ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);commands.ClearRenderTargetView(rtv,invalidNormal,0,nullptr);
    if(target.material)
    {
        if(!CheckGraphicsResult(states.Use(commands,*target.material.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"DepthNormals.Material"),text,"D3D12.StateTrackingFailed"))return false;
        const float invalidMaterial[4]{0,0,0,-1};commands.ClearRenderTargetView(target.rtv->Cpu(1),invalidMaterial,0,nullptr);
    }
    commands.OMSetRenderTargets(target.material?2:1,&rtv,TRUE,&dsv);
    for(size_t index=0;index<frame.depthNormals->pipelines.size();++index)
    {
        const auto& pipeline=frame.depthNormals->pipelines[index];if(!pipeline||content.meshes[index].pbr.cullFront)continue;
        auto draw=content.meshes[index];draw.pipeline=&pipeline->mesh;
        RecordD3D12Meshes(pipeline->mesh,std::span(&draw,1),content.viewProjection,frame,commands,target.width,target.height,false,nullptr,{},static_cast<UINT>(index));
    }
    return true;
}
