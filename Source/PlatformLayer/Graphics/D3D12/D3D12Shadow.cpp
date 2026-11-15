#include "D3D12Shadow.h"
#include "D3D12Presentation.h"
#include "D3D12Memory.h"
#include "D3D12Result.h"
#include "D3D12PipelineCache.h"

bool PrepareD3D12Shadow(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,const Localization& text)
{
    if(!content.shadow||!content.shadowPipeline||!p.memory||!p.srvPool||!p.dsvPool)return CheckGraphicsResult(E_INVALIDARG,text,"D3D12.ShadowTargetsFailed");
    const auto resolution=content.shadow->resolution;
    if(resolution<256||resolution>4096||(resolution&(resolution-1))||!se::IsFinite(content.shadow->clipFromWorld))return CheckGraphicsResult(E_INVALIDARG,text,"D3D12.ShadowTargetsFailed");
    auto found=p.shadowTargets.find(resolution);auto target=found==p.shadowTargets.end()?std::shared_ptr<D3D12ShadowTarget>{}:found->second.lock();
    if(!target)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))return false;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT format{DXGI_FORMAT_R32_FLOAT};
        if(!CheckGraphicsResult(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&format,sizeof(format)),text,"D3D12.ShadowTargetsFailed"))return false;
        if(!(format.Support1&D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON))return CheckGraphicsResult(E_NOTIMPL,text,"D3D12.ShadowTargetsFailed");
        target=std::make_shared<D3D12ShadowTarget>();target->resolution=resolution;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=resolution;
        desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=DXGI_FORMAT_R32_TYPELESS;desc.SampleDesc={1,0};desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_D32_FLOAT;clear.DepthStencil.Depth=1;
        const auto created=p.memory->Create(desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,target->depth,"DirectionalShadow["+std::to_string(resolution)+"]");
        if(!CheckGraphicsResult(created,text,created==D3D12MemoryBudgetExceeded?"D3D12.GpuMemoryBudgetExceeded":"D3D12.ShadowTargetsFailed")
            ||!CheckGraphicsResult(p.dsvPool->Allocate(1,target->dsv),text,"D3D12.DescriptorCapacityExceeded")
            ||!CheckGraphicsResult(p.srvPool->Allocate(1,target->srv),text,"D3D12.DescriptorCapacityExceeded"))return false;
        D3D12_DEPTH_STENCIL_VIEW_DESC depth{};depth.Format=DXGI_FORMAT_D32_FLOAT;depth.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(target->depth.Get(),&depth,target->dsv->Cpu());
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_FLOAT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(target->depth.Get(),&view,target->srv->Cpu());
        p.shadowTargets.insert_or_assign(resolution,target);++p.shadowCreations;
    }
    p.shadow=target;frame.shadow=std::move(target);return true;
}
bool RecordD3D12Shadow(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,D3D12StateRecording& states,const Localization& text)
{
    auto& commands=*p.commandList.Get();auto& target=*frame.shadow;const auto& pipeline=*content.shadowPipeline;
    if(!CheckGraphicsResult(states.Use(commands,*target.depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,"Shadow"),text,"D3D12.StateTrackingFailed"))return false;
    const auto dsv=target.dsv->Cpu();commands.ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);commands.OMSetRenderTargets(0,nullptr,FALSE,&dsv);
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
    if(content.shadow->enabled)for(size_t index=0;index<content.shadowMeshes.size();++index)
    {
        const auto& draw=content.shadowMeshes[index];
        if(!draw.mesh||!draw.mesh->vertices||!draw.mesh->indices||!draw.mesh->indexCount||!se::IsFinite(draw.world)
            ||!se::IsFinite(content.shadow->clipFromWorld*draw.world))return CheckGraphicsResult(E_INVALIDARG,text,"D3D12.MeshDrawsInvalid");
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
        const auto clip=content.shadow->clipFromWorld*draw.world;commands.SetGraphicsRoot32BitConstants(0,16,clip.values.data(),0);
        commands.IASetVertexBuffers(0,1,&draw.mesh->vertexView);commands.IASetIndexBuffer(&draw.mesh->indexView);commands.DrawIndexedInstanced(draw.mesh->indexCount,1,0,0,0);
    }
    return true;
}
