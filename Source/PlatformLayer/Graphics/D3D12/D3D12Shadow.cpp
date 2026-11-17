#include "D3D12Shadow.h"
#include <cmath>
#include "D3D12Presentation.h"
#include "D3D12Memory.h"
#include "D3D12Result.h"
#include "D3D12PipelineCache.h"
#include "D3D12LocalLighting.h"
#include "D3D12ShadingExtensions.h"

namespace
{
bool AcquireTarget(D3D12Presentation& p,UINT resolution,UINT layers,bool array,bool local,
    std::shared_ptr<D3D12ShadowTarget>& output,const Localization& text)
{
    const UINT targetKey=resolution|(layers<<16)|(array?1u<<24:0u)|(local?1u<<25:0u);
    auto found=p.shadowTargets.find(targetKey);auto target=found==p.shadowTargets.end()?std::shared_ptr<D3D12ShadowTarget>{}:found->second.lock();
    if(!target)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))return false;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT format{DXGI_FORMAT_R32_FLOAT};
        if(!CheckGraphicsResult(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&format,sizeof(format)),text,"D3D12.ShadowTargetsFailed"))return false;
        if(!(format.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON))return CheckGraphicsResult(E_NOTIMPL,text,"D3D12.ShadowTargetsFailed");
        target=std::make_shared<D3D12ShadowTarget>();target->resolution=resolution;target->layers=layers;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=resolution;
        desc.DepthOrArraySize=static_cast<UINT16>(layers);desc.MipLevels=1;desc.Format=DXGI_FORMAT_R32_TYPELESS;desc.SampleDesc={1,0};desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_D32_FLOAT;clear.DepthStencil.Depth=1;
        const auto created=p.memory->Create(desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,target->depth,
            std::string(local?"LocalShadow[":"DirectionalShadow[")+std::to_string(resolution)+"]");
        if(!CheckGraphicsResult(created,text,created==D3D12MemoryBudgetExceeded?"D3D12.GpuMemoryBudgetExceeded":"D3D12.ShadowTargetsFailed")
            ||!CheckGraphicsResult(p.dsvPool->Allocate(layers,target->dsv),text,"D3D12.DescriptorCapacityExceeded")
            ||!CheckGraphicsResult(p.srvPool->Allocate(1,target->srv),text,"D3D12.DescriptorCapacityExceeded"))return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC depth{};depth.Format=DXGI_FORMAT_D32_FLOAT;depth.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        if(array)
        {
            depth.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DARRAY;depth.Texture2DArray.ArraySize=1;
            for(UINT slice=0;slice<layers;++slice)
            {depth.Texture2DArray.FirstArraySlice=slice;device->CreateDepthStencilView(target->depth.Get(),&depth,target->dsv->Cpu(slice));}
        }
        else device->CreateDepthStencilView(target->depth.Get(),&depth,target->dsv->Cpu());
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_FLOAT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        if(array){view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;view.Texture2DArray.MipLevels=1;view.Texture2DArray.ArraySize=layers;}
        device->CreateShaderResourceView(target->depth.Get(),&view,target->srv->Cpu());
        p.shadowTargets.insert_or_assign(targetKey,target);++p.shadowCreations;
    }
    output=std::move(target);return true;
}
bool RecordLayers(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,
    D3D12ShadowTarget& target,std::span<const se::Mat4> matrices,bool enabled,D3D12StateRecording& states,const Localization& text)
{
    auto& commands=*p.commandList.Get();const auto& pipeline=*content.shadowPipeline;
    if(!CheckGraphicsResult(states.Use(commands,*target.depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,"Shadow"),text,"D3D12.StateTrackingFailed"))return false;
    commands.SetPipelineState(pipeline.pipeline.Get());commands.SetGraphicsRootSignature(pipeline.rootSignature.Get());
    // W03：蒙皮投射使用各自的 ShadowDepth 变体（b0 仍是 16 个 root32 常量，t8 调色板在参数 1）。
    // 静态投射继续共享默认 Pipeline，PSO/根签名只在切换时重设，未蒙皮路径的绑定次数不变。
    const D3D12MeshPipeline* activePipeline=&pipeline;
    const auto bind=[&](const D3D12MeshPipeline& program)
    {
        commands.SetPipelineState(program.pipeline.Get());commands.SetGraphicsRootSignature(program.rootSignature.Get());
        activePipeline=&program;
    };
    const D3D12_VIEWPORT viewport{0,0,float(target.resolution),float(target.resolution),0,1};const D3D12_RECT scissor{0,0,LONG(target.resolution),LONG(target.resolution)};
    commands.RSSetViewports(1,&viewport);commands.RSSetScissorRects(1,&scissor);commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for(UINT slice=0;slice<target.layers;++slice)
    {
    const auto dsv=target.dsv->Cpu(slice);commands.ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
    commands.OMSetRenderTargets(0,nullptr,FALSE,&dsv);
    const auto& shadowMatrix=matrices[slice];
    // 每级从完整世界候选重新提交，在光视锥中硬件裁剪，绝不读取主相机可见集。
    if(enabled)for(size_t index=0;index<content.shadowMeshes.size();++index)
    {
        const auto& draw=content.shadowMeshes[index];
        if(!draw.mesh||!draw.mesh->vertices||!draw.mesh->indices||!draw.mesh->indexCount||!se::IsFinite(draw.world)
            ||!se::IsFinite(shadowMatrix*draw.world))return CheckGraphicsResult(E_INVALIDARG,text,"D3D12.MeshDrawsInvalid");
        const auto& program=draw.pipeline?*draw.pipeline:pipeline;
        if(activePipeline!=&program)bind(program);
        const bool skinned=program.prepared->program->source.skinnedVertex;
        if(skinned!=(draw.mesh->vertexView.StrideInBytes==sizeof(se::rhi::SkinnedVertex)))
        {
            Log(LogLevel::Error,"阴影蒙皮校验: pipelineSkinned="+std::to_string(skinned)+" stride="+std::to_string(draw.mesh->vertexView.StrideInBytes)
                +" palette="+std::to_string(draw.skinPalette.size())+" joints="+std::to_string(draw.skinJointCount));
            return CheckGraphicsResult(E_INVALIDARG,text,"D3D12.MeshDrawsInvalid");
        }
        if(skinned)
        {
            const auto& skin=index<frame.shadowSkinSlices.size()?frame.shadowSkinSlices[index]:D3D12UploadSlice{};
            if(!skin.resource||draw.skinPalette.empty()||!draw.skinJointCount
                ||draw.skinPalette.size()-draw.skinJointOffset!=static_cast<std::size_t>(draw.skinJointCount)*48)
            {
                Log(LogLevel::Error,"阴影蒙皮调色板: slices="+std::to_string(frame.shadowSkinSlices.size())+" index="+std::to_string(index)
                    +" resource="+std::to_string(skin.resource?1:0)+" palette="+std::to_string(draw.skinPalette.size())
                    +" offset="+std::to_string(draw.skinJointOffset)+" joints="+std::to_string(draw.skinJointCount));
                return CheckGraphicsResult(E_INVALIDARG,text,"D3D12.MeshDrawsInvalid");
            }
            commands.SetGraphicsRootShaderResourceView(1,skin.Gpu());
        }
        if(!CheckGraphicsResult(states.Use(commands,*draw.mesh->vertices.Get(),D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"ShadowVertices"),text,"D3D12.StateTrackingFailed")
            ||!CheckGraphicsResult(states.Use(commands,*draw.mesh->indices.Get(),D3D12_RESOURCE_STATE_INDEX_BUFFER,"ShadowIndices"),text,"D3D12.StateTrackingFailed"))return false;
        if(program.prepared->materialRootParameter!=UINT_MAX)
        {
            if(draw.alphaMode!=se::MaterialAlphaMode::Mask||!draw.mesh->texture||!frame.shadowMaterialConstants[index].resource)return false;
            if(!CheckGraphicsResult(states.Use(commands,*draw.mesh->texture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Shadow.MaterialCoverage"),text,"D3D12.StateTrackingFailed"))return false;
            ID3D12DescriptorHeap* heaps[]{frame.shadowMaterialDescriptors.Heap()};commands.SetDescriptorHeaps(1,heaps);
            commands.SetGraphicsRootDescriptorTable(skinned?2u:1u,frame.shadowMaterialTables[index]);
            commands.SetGraphicsRootConstantBufferView(program.prepared->materialRootParameter,frame.shadowMaterialConstants[index].Gpu());
        }
        const auto clip=shadowMatrix*draw.world;commands.SetGraphicsRoot32BitConstants(0,16,clip.values.data(),0);
        commands.IASetVertexBuffers(0,1,&draw.mesh->vertexView);commands.IASetIndexBuffer(&draw.mesh->indexView);commands.DrawIndexedInstanced(draw.mesh->indexCount,1,0,0,0);
    }
    }
    return true;
}
}

bool PrepareD3D12Shadow(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,const Localization& text)
{
    if(!content.shadow||!content.shadowPipeline||!p.memory||!p.srvPool||!p.dsvPool)return CheckGraphicsResult(E_INVALIDARG,text,"D3D12.ShadowTargetsFailed");
    const auto resolution=content.shadow->resolution;const bool cascaded=content.shadow->cascadeCount>0;
    if(resolution<256||resolution>4096||((resolution&(resolution-1))&&!(cascaded&&resolution==1536))||!se::IsFinite(content.shadow->clipFromWorld))
        return CheckGraphicsResult(E_INVALIDARG,text,"D3D12.ShadowTargetsFailed");
    if(!AcquireTarget(p,resolution,cascaded?content.shadow->cascadeCount:1,cascaded,false,frame.shadow,text))return false;
    p.shadow=frame.shadow;return true;
}

bool AcquireD3D12LocalShadow(D3D12Presentation& p,UINT layers,std::shared_ptr<D3D12ShadowTarget>& output,const Localization& text)
{
    if(!layers||layers>se::rhi::MaximumLocalLights*6)return false;
    return AcquireTarget(p,D3D12LocalShadowResolution,layers,true,true,output,text);
}

bool RecordD3D12Shadow(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,D3D12StateRecording& states,const Localization& text)
{
    const auto& settings=*content.shadow;
    const auto matrices=settings.cascadeCount?std::span<const se::Mat4>(settings.cascadeClipFromWorld).first(settings.cascadeCount):
        std::span<const se::Mat4>(&settings.clipFromWorld,1);
    if(!RecordLayers(p,frame,content,*frame.shadow,matrices,settings.enabled,states,text))return false;
    if(frame.shading&&frame.shading->localShadow)
        return RecordLayers(p,frame,content,*frame.shading->localShadow,frame.shading->localShadowMatrices,true,states,text);
    return true;
}

bool PrepareD3D12ShadowFilterConstants(const se::rhi::ShadowSettings& settings,se::rhi::ShadowFilterConstants& output)
{
    const auto& filter=settings.filter;
    if(settings.cascadeCount<1||settings.cascadeCount>3||!settings.resolution
        ||!se::IsFinite(filter.angularRadius)||filter.angularRadius<0||filter.angularRadius>.2f
        ||!se::IsFinite(filter.blockerSearchDistance)||filter.blockerSearchDistance<.01f||filter.blockerSearchDistance>100
        ||!se::IsFinite(filter.maximumRadiusTexels)||filter.maximumRadiusTexels<1||filter.maximumRadiusTexels>32
        ||static_cast<UINT>(filter.debug)>2)return false;
    se::rhi::ShadowFilterConstants candidate{};
    candidate.parameters={std::tan(filter.angularRadius),filter.blockerSearchDistance,filter.maximumRadiusTexels,filter.pcss?1.f:0.f};
    candidate.options={float(filter.debug),0,0,0};
    for(UINT slice=0;slice<settings.cascadeCount;++slice)
    {
        const auto& matrix=settings.cascadeClipFromWorld[slice];
        if(!se::IsFinite(matrix)||std::abs(matrix(3,0))>1e-6f||std::abs(matrix(3,1))>1e-6f
            ||std::abs(matrix(3,2))>1e-6f||std::abs(matrix(3,3)-1)>1e-6f)return false;
        const se::Vec3 x{matrix(0,0),matrix(0,1),matrix(0,2)},y{matrix(1,0),matrix(1,1),matrix(1,2)},z{matrix(2,0),matrix(2,1),matrix(2,2)};
        const float a=se::Length(x),b=se::Length(y),c=se::Length(z);
        if(!se::IsFinite(a)||!se::IsFinite(b)||!se::IsFinite(c)||a<1e-8f||b<1e-8f||c<1e-8f
            ||std::abs(se::Dot(x,y))>a*b*.001f||std::abs(se::Dot(x,z))>a*c*.001f||std::abs(se::Dot(y,z))>b*c*.001f)return false;
        candidate.cascadeMetrics[slice]={1.f/c,2.f/(float(settings.resolution)*a),2.f/(float(settings.resolution)*b),0};
        if(!se::IsFinite(candidate.cascadeMetrics[slice]))return false;
    }
    output=candidate;return true;
}
