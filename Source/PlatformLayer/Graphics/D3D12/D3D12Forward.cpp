#include "D3D12Forward.h"
#include "D3D12Presentation.h"
#include "D3D12Gui.h"
#include "D3D12Memory.h"
#include "D3D12PipelineCache.h"
#include "D3D12Result.h"
#include "D3D12Shadow.h"
#include "D3D12LightClustering.h"
#include <algorithm>
#include <cmath>
#include <cstring>

bool PrepareD3D12Forward(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,const Localization& text)
{
    if(!content.forward||!content.outputPipeline)return false;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))return false;
    if(!content.sceneHdr&&!p.hdr)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT format{DXGI_FORMAT_R16G16B16A16_FLOAT};
        constexpr auto required=D3D12_FORMAT_SUPPORT1_TEXTURE2D|D3D12_FORMAT_SUPPORT1_SHADER_LOAD|D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
        if(!CheckGraphicsResult(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&format,sizeof(format)),text,"D3D12.HdrTargetsFailed"))return false;
        if((format.Support1&required)!=required)return CheckGraphicsResult(E_NOTIMPL,text,"D3D12.HdrTargetsFailed");
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=p.width;desc.Height=p.height;
        desc.DepthOrArraySize=desc.MipLevels=1;desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.SampleDesc={1,0};desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        Microsoft::WRL::ComPtr<ID3D12Resource> hdr;std::shared_ptr<D3D12DescriptorRange> rtv,srv;
        D3D12_CLEAR_VALUE clear{};clear.Format=desc.Format;clear.Color[3]=1;
        auto result=p.memory->Create(desc,D3D12_RESOURCE_STATE_COMMON,&clear,hdr,"HdrScene");
        if(!CheckGraphicsResult(result,text,result==D3D12MemoryBudgetExceeded?"D3D12.GpuMemoryBudgetExceeded":"D3D12.HdrTargetsFailed"))return false;
        // 使用已有同类型池；每个 Range 仍由实际 owner 控制寿命。
        if(!CheckGraphicsResult(p.rtvPool->Allocate(1,rtv),text,"D3D12.DescriptorCapacityExceeded")
            ||!CheckGraphicsResult(p.srvPool->Allocate(1,srv),text,"D3D12.DescriptorCapacityExceeded"))return false;
        device->CreateRenderTargetView(hdr.Get(),nullptr,rtv->Cpu());
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=desc.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(hdr.Get(),&view,srv->Cpu());
        p.hdr=std::move(hdr);p.hdrRtv=std::move(rtv);p.hdrSrv=std::move(srv);++p.hdrCreations;
    }
    const auto& lighting=*content.forward;flare::rhi::ForwardFrameConstants constants{};
    constants.directionAndEnabled={lighting.directionToLight.x,lighting.directionToLight.y,lighting.directionToLight.z,lighting.directionalEnabled?1.0f:0.0f};
    constants.directionalRadiance={lighting.directionalRadiance.x,lighting.directionalRadiance.y,lighting.directionalRadiance.z,0};
    constants.skyAndExposure={lighting.sky.x,lighting.sky.y,lighting.sky.z,lighting.exposure};constants.pointCount.x=static_cast<float>(lighting.pointCount);
    for(UINT index=0;index<lighting.pointCount;++index)
    {
        const auto& point=lighting.points[index];constants.pointPositionRadius[index]={point.position.x,point.position.y,point.position.z,point.range};
        constants.pointRadiance[index]={point.radiance.x,point.radiance.y,point.radiance.z,0};
    }
    const bool cascaded=content.shadow&&content.shadow->cascadeCount>0;
    const bool clustered=lighting.cluster.enabled;
    const UINT frameBytes=clustered?1024:cascaded?768:512;
    if(!CheckGraphicsResult(frame.uploads.Allocate(frameBytes,256,frame.forwardConstants),text,"D3D12.UploadCapacityExceeded"))return false;
    std::memset(frame.forwardConstants.cpu,0,frameBytes);
    if(cascaded)
    {
        const auto& shadow=*content.shadow;
        constants.pointCount.y=shadow.enabled?1.f:0.f;constants.pointCount.z=shadow.pcf?1.f:0.f;
        const flare::rhi::CascadedShadowFrameConstants csm{constants,shadow.cascadeClipFromWorld,
            {shadow.cascadeSplits[0],shadow.cascadeSplits[1],shadow.cascadeSplits[2],shadow.cascadeSplits[3]},
            shadow.cascadeViewDepth,{1.f/float(frame.shadow->resolution),shadow.depthBias,shadow.slopeBias,shadow.normalBias},
            {float(shadow.cascadeCount),shadow.cascadeBlend,shadow.cascadeDebug?1.f:0.f,shadow.cascadeNear},{}};
        std::memcpy(frame.forwardConstants.cpu,&csm,sizeof(csm));
    }
    else if(content.shadow)
    {
        constants.pointCount.y=content.shadow->enabled?1.0f:0.0f;constants.pointCount.z=content.shadow->pcf?1.0f:0.0f;
        const flare::rhi::ShadowFrameConstants shadow{constants,content.shadow->clipFromWorld,
            {1.0f/float(frame.shadow->resolution),content.shadow->depthBias,content.shadow->slopeBias,content.shadow->normalBias}};
        std::memcpy(frame.forwardConstants.cpu,&shadow,sizeof(shadow));
    }
    else std::memcpy(frame.forwardConstants.cpu,&constants,sizeof(constants));
    if(clustered)
    {
        flare::rhi::ClusteredFrameConstants clusteredFrame{};
        auto& base=clusteredFrame.shadow;base.lights=constants;
        if(cascaded)std::memcpy(&base,frame.forwardConstants.cpu,sizeof(base));
        else if(content.shadow)
        {
            base.clipFromWorld[0]=content.shadow->clipFromWorld;
            base.parameters={1.f/float(frame.shadow->resolution),content.shadow->depthBias,content.shadow->slopeBias,content.shadow->normalBias};
        }
        const auto& config=lighting.cluster;
        base.viewDepth={-config.view(2,0),-config.view(2,1),-config.view(2,2),-config.view(2,3)};
        clusteredFrame.view=config.view;
        // 直接使用 ConfigureForward 已校验的相机投影矩阵，避免 viewProjection * Inverse(view) 产生大远近比浮点抵消误差。
        clusteredFrame.projection=config.clipFromView;
        if(!ValidD3D12ClusterProjection(clusteredFrame.projection,config.nearPlane,config.farPlane))return CheckGraphicsResult(E_INVALIDARG,text,"Rhi.BackendFailure");
        const auto inverseProjection=flare::Inverse(clusteredFrame.projection);if(!inverseProjection)return false;
        clusteredFrame.inverseProjection=*inverseProjection;
        clusteredFrame.clusterParameters={config.nearPlane,config.farPlane,float(config.lights.size()),float(config.capacity)};
        const UINT scale=content.post&&content.post->supersample4x?2:1;
        const UINT width=(content.sceneHdr?content.sceneHdr->color.width:content.gui?content.gui->scene->color.width:p.width)*scale;
        const UINT height=(content.sceneHdr?content.sceneHdr->color.height:content.gui?content.gui->scene->color.height:p.height)*scale;
        clusteredFrame.clusterViewport={float(width),float(height),1.f/width,1.f/height};
        clusteredFrame.clusterModes={config.debug?1.f:0.f,config.bruteForce?1.f:0.f,0,0};
        std::memcpy(frame.forwardConstants.cpu,&clusteredFrame,sizeof(clusteredFrame));
        if(!p.clusterBuilder)
        {
            auto builder=std::make_shared<D3D12LightClustering>();
            const auto shaders=content.outputPipeline->prepared->program->source.path.parent_path();
            if(!CheckGraphicsResult(builder->Initialize(*device.Get(),p.memory,shaders,text),text,"Rhi.BackendFailure"))return false;
            p.clusterBuilder=std::move(builder);
        }
        if(!frame.clusters&&!CheckGraphicsResult(p.clusterBuilder->CreateOutput(frame.clusters),text,"Rhi.BackendFailure"))return false;
        if(!CheckGraphicsResult(p.clusterBuilder->Prepare(*frame.clusters,config),text,"Rhi.BackendFailure"))return false;
        frame.clusterDescriptorIncrement=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if(content.meshPipeline&&flare::rhi::IsShadowFilterLayout(content.meshPipeline->prepared->program->source.layoutVersion))
    {
        flare::rhi::ShadowFilterConstants filter;
        if(!content.shadow||!PrepareD3D12ShadowFilterConstants(*content.shadow,filter))return CheckGraphicsResult(E_INVALIDARG,text,"Rhi.BackendFailure");
        if(!CheckGraphicsResult(frame.uploads.Allocate(256,256,frame.shadowFilterConstants),text,"D3D12.UploadCapacityExceeded"))return false;
        std::memset(frame.shadowFilterConstants.cpu,0,256);std::memcpy(frame.shadowFilterConstants.cpu,&filter,sizeof(filter));
    }
    if(content.environment)
    {
        const auto& environment=*content.environment;
        const flare::Vec4 parameters{environment.intensity,std::cos(environment.yaw),std::sin(environment.yaw),float(content.environmentTextures[1]->mipLevels-1)};
        const auto environmentOffset=(clustered||cascaded)?offsetof(flare::rhi::CascadedShadowFrameConstants,environment):offsetof(flare::rhi::EnvironmentFrameConstants,environment);
        std::memcpy(frame.forwardConstants.cpu+environmentOffset,&parameters,sizeof(parameters));
        if(frame.environmentDescriptors.Maximum()!=1&&!frame.environmentDescriptors.Configure(1,1))return false;
        const auto source=content.environmentTextures[1]->views->Cpu(1);
        if(!CheckGraphicsResult(frame.environmentDescriptors.Prepare(*device.Get(),std::span(&source,1)),text,"D3D12.DescriptorCapacityExceeded"))return false;
    }
    if(content.post)return true;
    // G15：输出 LUT 布局在 t2 追加 strip。ShadowOutput 的表为场景/阴影预览/LUT；
    // FinalOutput 不读 t1，但描述符表必须覆盖 t0..t2，因此 t1 绑定同一 LUT 仅作占位。
    const bool outputLut=content.outputPipeline->prepared->program->source.layoutVersion==flare::rhi::OutputLutLayoutVersion;
    if(outputLut&&!content.colorLut)return false;
    const UINT count=outputLut?3:(content.shadow?2:1);
    if(frame.outputDescriptors.Maximum()!=count&&!frame.outputDescriptors.Configure(count,count))return false;
    const auto lutView=outputLut?content.colorLut->views->Cpu(1):D3D12_CPU_DESCRIPTOR_HANDLE{};
    const D3D12_CPU_DESCRIPTOR_HANDLE sources[]{
        content.sceneHdr?content.sceneHdr->color.views->Cpu(0):p.hdrSrv->Cpu(),
        content.shadow?frame.shadow->srv->Cpu():lutView,
        lutView};
    return CheckGraphicsResult(frame.outputDescriptors.Prepare(*device.Get(),std::span(sources,count),count),text,"D3D12.DescriptorCapacityExceeded");
}
void RecordD3D12ForwardOutput(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,UINT width,UINT height,D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    const auto& pipeline=*content.outputPipeline;auto& commands=*p.commandList.Get();
    (void)rtv; // 输出目标已由 D3D12Presentation 绑定，此处只记录全屏三角形。
    commands.SetPipelineState(pipeline.pipeline.Get());commands.SetGraphicsRootSignature(pipeline.rootSignature.Get());
    const flare::Vec4 exposure{content.forward->exposure,content.shadow&&content.shadow->preview?1.0f:0.0f,float(width),float(height)};commands.SetGraphicsRoot32BitConstants(0,4,&exposure,0);
    ID3D12DescriptorHeap* heaps[]{frame.outputDescriptors.Heap()};commands.SetDescriptorHeaps(1,heaps);commands.SetGraphicsRootDescriptorTable(1,frame.outputDescriptors.Draw(0));
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    commands.RSSetViewports(1,&viewport);commands.RSSetScissorRects(1,&scissor);commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands.DrawInstanced(3,1,0,0);
}
void RecordD3D12ForwardSky(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,UINT width,UINT height)
{
    auto& commands=*p.commandList.Get();const auto& pipeline=*content.skyPipeline;
    commands.SetPipelineState(pipeline.pipeline.Get());commands.SetGraphicsRootSignature(pipeline.rootSignature.Get());
    if(pipeline.prepared->program->source.kind==flare::rhi::PipelineKind::EnvironmentSky)
    {
        const auto& environment=*content.environment;
        const flare::rhi::EnvironmentSkyConstants constants{environment.skyFromClip,{environment.intensity,std::cos(environment.yaw),std::sin(environment.yaw),0}};
        commands.SetGraphicsRoot32BitConstants(0,20,&constants,0);
        ID3D12DescriptorHeap* heaps[]{frame.environmentDescriptors.Heap()};commands.SetDescriptorHeaps(1,heaps);
        commands.SetGraphicsRootDescriptorTable(1,frame.environmentDescriptors.Draw(0));
    }
    else
    {
        const auto& sky=content.forward->skySettings;
        flare::rhi::SkyConstants constants{};
        const auto inverse=flare::Inverse(content.viewProjection);
        constants.skyFromClip=inverse.value_or(flare::Mat4{});
        // 逆 VP 含透视除法，直接清最后一列会同时抹掉前向基，导致天空朝地面。
        // Shader 以远近两点的齐次差消除平移，保持正确的旋转和 FOV。
        const auto clampHdr=[](float value){return std::isfinite(value)?std::clamp(value,0.0f,16.0f):0.0f;};
        constants.zenithColor={clampHdr(sky.zenithColor.x),clampHdr(sky.zenithColor.y),clampHdr(sky.zenithColor.z),1};
        constants.horizonColor={clampHdr(sky.horizonColor.x),clampHdr(sky.horizonColor.y),clampHdr(sky.horizonColor.z),1};
        constants.groundColor={clampHdr(sky.groundColor.x),clampHdr(sky.groundColor.y),clampHdr(sky.groundColor.z),1};
        constants.sunColor={clampHdr(sky.sunColor.x),clampHdr(sky.sunColor.y),clampHdr(sky.sunColor.z),1};
        const auto direction=flare::Normalize(content.forward->directionToLight);
        const auto radius=std::isfinite(sky.sunAngularRadius)?std::clamp(sky.sunAngularRadius,0.001f,0.10f):0.012f;
        constants.sunDirectionIntensity={direction.x,direction.y,direction.z,
            std::isfinite(sky.sunIntensity)?std::clamp(sky.sunIntensity,0.0f,1000.0f):12.0f};
        constants.parameters={float(width),std::cos(radius*1.15f),std::cos(radius*0.85f),float(height)};
        constants.cloudParameters={
            std::isfinite(sky.cloudCoverage)?std::clamp(sky.cloudCoverage,0.0f,1.0f):0.0f,
            std::isfinite(sky.cloudDensity)?std::clamp(sky.cloudDensity,0.0f,8.0f):1.0f,
            std::isfinite(sky.cloudSpeed)?std::clamp(sky.cloudSpeed,-100.0f,100.0f):1.0f,
            std::isfinite(sky.timeSeconds)?sky.timeSeconds:0.0f};
        commands.SetGraphicsRoot32BitConstants(0,44,&constants,0);
    }
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    commands.RSSetViewports(1,&viewport);commands.RSSetScissorRects(1,&scissor);commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands.DrawInstanced(3,1,0,0);
}
