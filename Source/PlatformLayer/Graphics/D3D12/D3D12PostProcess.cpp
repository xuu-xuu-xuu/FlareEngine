#include "D3D12PostProcess.h"
#include "D3D12Presentation.h"
#include "D3D12Memory.h"
#include "D3D12PipelineCache.h"
#include "D3D12Result.h"
#include "D3D12Gui.h"
#include <algorithm>
#include <cmath>

bool PrepareD3D12PostProcess(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,const Localization& text)
{
    const auto hdrResource=content.sceneHdr?content.sceneHdr->color.resource.Get():p.hdr.Get();
    const auto depthResource=content.sceneHdr?content.sceneHdr->depth.Get():p.depth.Get();
    const UINT width=content.sceneHdr?content.sceneHdr->color.width:p.width,height=content.sceneHdr?content.sceneHdr->color.height:p.height;
    if(!content.post||!hdrResource||!depthResource||std::any_of(content.postPipelines.begin(),content.postPipelines.end(),[](const auto* pipeline){return !pipeline||!pipeline->prepared;}))return false;
    Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))return false;
    const bool wantTransientAlias=content.post->transientAliasing&&p.memory&&p.memory->IsPlacedMode();
    if(!p.post||p.post->width!=width||p.post->height!=height||p.post->hdrSource.Get()!=hdrResource||p.post->depthSource.Get()!=depthResource
        ||p.post->transientAliased!=wantTransientAlias)
    {
        auto candidate=std::make_shared<D3D12PostTargets>();candidate->hdrSource=hdrResource;candidate->depthSource=depthResource;candidate->width=width;candidate->height=height;candidate->bloomWidth=(width+1)/2;candidate->bloomHeight=(height+1)/2;candidate->transientAliased=wantTransientAlias;
        const auto makeDesc=[](UINT w,UINT h,DXGI_FORMAT format){
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;desc.DepthOrArraySize=desc.MipLevels=1;
            desc.Format=format;desc.SampleDesc={1,0};desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;return desc;
        };
        const auto finishViews=[&](const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT format,D3D12PostTexture& target){
            if(!CheckGraphicsResult(p.rtvPool->Allocate(1,target.rtv),text,"D3D12.DescriptorCapacityExceeded")||!CheckGraphicsResult(p.srvPool->Allocate(1,target.srv),text,"D3D12.DescriptorCapacityExceeded"))return false;
            device->CreateRenderTargetView(target.resource.Get(),nullptr,target.rtv->Cpu());D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;device->CreateShaderResourceView(target.resource.Get(),&view,target.srv->Cpu());
            candidate->allocatedBytes+=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;return true;
        };
        const auto create=[&](UINT w,UINT h,DXGI_FORMAT format,const char* name,D3D12PostTexture& target){
            const auto desc=makeDesc(w,h,format);
            const auto result=p.memory->Create(desc,D3D12_RESOURCE_STATE_COMMON,nullptr,target.resource,name);
            if(!CheckGraphicsResult(result,text,result==D3D12MemoryBudgetExceeded?"D3D12.GpuMemoryBudgetExceeded":"D3D12.PostTargetsFailed"))return false;
            return finishViews(desc,format,target);
        };
        if(!create(candidate->bloomWidth,candidate->bloomHeight,DXGI_FORMAT_R16G16B16A16_FLOAT,"Bloom.Ping",candidate->bloom[0]))return false;
        if(wantTransientAlias)
        {
            const auto pongDesc=makeDesc(candidate->bloomWidth,candidate->bloomHeight,DXGI_FORMAT_R16G16B16A16_FLOAT);
            const auto ldrDesc=makeDesc(width,height,DXGI_FORMAT_R8G8B8A8_UNORM);
            const auto pongAlloc=device->GetResourceAllocationInfo(0,1,&pongDesc);
            const auto ldrAlloc=device->GetResourceAllocationInfo(0,1,&ldrDesc);
            const auto slotResult=p.memory->CreateTransientSlot((std::max)(pongAlloc.SizeInBytes,ldrAlloc.SizeInBytes),
                (std::max)(pongAlloc.Alignment,ldrAlloc.Alignment),D3D12MemoryCategory::RenderTarget,
                candidate->transientSlot,"PostProcess.TransientSlot0");
            if(!CheckGraphicsResult(slotResult,text,slotResult==D3D12MemoryBudgetExceeded?"D3D12.GpuMemoryBudgetExceeded":"D3D12.PostTargetsFailed"))return false;
            const auto pongRes=p.memory->CreateAliasedResource(candidate->transientSlot,0,pongDesc,D3D12_RESOURCE_STATE_COMMON,nullptr,candidate->bloom[1].resource,"Bloom.Pong");
            if(!CheckGraphicsResult(pongRes,text,"D3D12.PostTargetsFailed")||!finishViews(pongDesc,DXGI_FORMAT_R16G16B16A16_FLOAT,candidate->bloom[1]))return false;
            const auto ldrRes=p.memory->CreateAliasedResource(candidate->transientSlot,0,ldrDesc,D3D12_RESOURCE_STATE_COMMON,nullptr,candidate->ldr.resource,"ToneMap.Ldr");
            if(!CheckGraphicsResult(ldrRes,text,"D3D12.PostTargetsFailed")||!finishViews(ldrDesc,DXGI_FORMAT_R8G8B8A8_UNORM,candidate->ldr))return false;
        }
        else if(!create(candidate->bloomWidth,candidate->bloomHeight,DXGI_FORMAT_R16G16B16A16_FLOAT,"Bloom.Pong",candidate->bloom[1])
            ||!create(width,height,DXGI_FORMAT_R8G8B8A8_UNORM,"ToneMap.Ldr",candidate->ldr))return false;
        if(!CheckGraphicsResult(p.rtvPool->Allocate(D3D12Presentation::bufferCount,candidate->outputRtvs),text,"D3D12.DescriptorCapacityExceeded"))return false;
        D3D12_RENDER_TARGET_VIEW_DESC view{};view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;view.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        for(UINT index=0;index<D3D12Presentation::bufferCount;++index)device->CreateRenderTargetView(p.backBuffers[index].Get(),&view,candidate->outputRtvs->Cpu(index));
        // G15：曝光直方图/适应与 LUT 与后处理目标同寿命；ToneMap 27 布局始终有 t2/t3 可用。
        auto kernel=std::make_shared<D3D12Exposure>();
        const auto shaders=content.postPipelines[2]->prepared->program->source.path.parent_path();
        if(FAILED(kernel->Initialize(*device.Get(),*p.memory,shaders,text))
            ||FAILED(kernel->CreateOutput(*hdrResource,width,height,candidate->exposure)))
        {CheckGraphicsResult(E_FAIL,text,"D3D12.ExposureInputsInvalid");return false;}
        candidate->exposureKernel=std::move(kernel);
        const auto addBytes=[&](ID3D12Resource* resource)
        {
            if(!resource)return;
            const auto description=resource->GetDesc();
            candidate->allocatedBytes+=device->GetResourceAllocationInfo(0,1,&description).SizeInBytes;
        };
        addBytes(candidate->exposure.histogram.Get());addBytes(candidate->exposure.exposure[0].Get());
        addBytes(candidate->exposure.exposure[1].Get());addBytes(candidate->exposure.lut.Get());
        // ToneMap 的 t2/t3 走持久 SRV 池，与 Bloom/LDR 使用同一条副本路径。
        if(!CheckGraphicsResult(p.srvPool->Allocate(3,candidate->exposureSrvs),text,"D3D12.DescriptorCapacityExceeded"))return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC exposureView{};
        exposureView.Format=DXGI_FORMAT_R32_FLOAT;exposureView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        exposureView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;exposureView.Texture2D={0,1,0,0};
        device->CreateShaderResourceView(candidate->exposure.exposure[0].Get(),&exposureView,candidate->exposureSrvs->Cpu(0));
        device->CreateShaderResourceView(candidate->exposure.exposure[1].Get(),&exposureView,candidate->exposureSrvs->Cpu(1));
        D3D12_SHADER_RESOURCE_VIEW_DESC lutView{};
        lutView.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;lutView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;
        lutView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;lutView.Texture3D={0,1,0};
        device->CreateShaderResourceView(candidate->exposure.lut.Get(),&lutView,candidate->exposureSrvs->Cpu(2));
        p.post=std::move(candidate);++p.postCreations;
    }
    frame.post=p.post;
    auto& targets=*p.post;
    if(frame.postDescriptors.Maximum()!=20&&!frame.postDescriptors.Configure(20,20))return false;
    const auto& settings=*content.post;
    // 透明帧在真实几何尺寸上先解析纯不透明历史；普通旧帧保留原后处理阶段的历史路径。
    const bool hasTransparent=content.opaqueDrawCount<content.meshes.size();
    if(settings.taa)
    {
        if(!targets.taaKernel)
        {
            auto kernel=std::make_shared<D3D12Taa>();const auto shaders=content.postPipelines[2]->prepared->program->source.path.parent_path();
            if(FAILED(kernel->Initialize(*device.Get(),*p.memory,shaders,text)))return CheckGraphicsResult(E_FAIL,text,"D3D12.TaaInputsInvalid");
            targets.taaKernel=std::move(kernel);
        }
        if(!hasTransparent&&!targets.taa.velocity)
        {
            if(FAILED(targets.taaKernel->CreateOutput(*hdrResource,*depthResource,width,height,targets.taa)))return CheckGraphicsResult(E_FAIL,text,"D3D12.TaaInputsInvalid");
            if(!CheckGraphicsResult(p.srvPool->Allocate(2,targets.taaSrvs),text,"D3D12.DescriptorCapacityExceeded"))return false;
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D={0,1,0,0};
            for(UINT index=0;index<2;++index)device->CreateShaderResourceView(targets.taa.historyColor[index].Get(),&view,targets.taaSrvs->Cpu(index));
        }
    }
    bool lutChanged=false;
    for(size_t index=0;index<3;++index)
        lutChanged|=targets.lutLift[index]!=settings.lutLift[index]||targets.lutGamma[index]!=settings.lutGamma[index]
            ||targets.lutGain[index]!=settings.lutGain[index];
    if(lutChanged)
    {
        for(size_t index=0;index<3;++index)
        {
            targets.lutLift[index]=settings.lutLift[index];targets.lutGamma[index]=settings.lutGamma[index];
            targets.lutGain[index]=settings.lutGain[index];
        }
        if(!targets.lutAssetActive)targets.lutDirty=true;
    }
    // G15：外部 `.selut` LUT 与 LGG 生成互斥。资产生效时不再标记 LUT 重建；
    // 资产被关闭后必须重新生成一次 LGG LUT，避免 ToneMap 采样到旧风格。
    const bool lutAsset=settings.lutAsset&&settings.lutAsset->size()==std::size_t{D3D12Exposure::lutSize}*D3D12Exposure::lutSize*D3D12Exposure::lutSize;
    if(lutAsset!=targets.lutAssetActive)
    {
        targets.lutAssetActive=lutAsset;
        targets.lutAssetUploaded=false;
        if(!lutAsset)targets.lutDirty=true;
    }
    const auto hdr=content.sceneHdr?content.sceneHdr->color.views->Cpu(1):p.hdrSrv->Cpu();
    const auto a=targets.bloom[0].srv->Cpu(),b=targets.bloom[1].srv->Cpu(),ldr=targets.ldr.srv->Cpu();
    const UINT nextExposure=1-targets.exposure.exposureIndex;
    const auto exposureSrv=targets.exposureSrvs->Cpu(nextExposure),lutSrv=targets.exposureSrvs->Cpu(2);
    const bool taa=settings.taa&&targets.taaKernel&&!hasTransparent;
    const auto sceneColor=taa?targets.taaSrvs->Cpu(1-targets.taa.historyIndex):hdr;
    // 每个后处理 Pass 的绑定表固定 4 槽：前两个沿用旧语义，t2/t3 只由 ToneMap 采样。
    const D3D12_CPU_DESCRIPTOR_HANDLE sources[]{
        sceneColor,sceneColor,sceneColor,sceneColor,
        a,a,a,a,
        b,b,b,b,
        sceneColor,settings.bloom?a:sceneColor,exposureSrv,lutSrv,
        ldr,ldr,ldr,ldr};
    return CheckGraphicsResult(frame.postDescriptors.Prepare(*device.Get(),sources,4),text,"D3D12.DescriptorCapacityExceeded");
}
bool RecordD3D12PostProcess(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,D3D12StateRecording& states,UINT backBufferIndex,const Localization& text)
{
    auto& commands=*p.commandList.Get();auto& targets=*p.post;const auto& settings=*content.post;const UINT first=content.shadow?8:6;
    const auto hdrResource=targets.hdrSource.Get(),depthResource=targets.depthSource.Get();
    const auto width=targets.width,height=targets.height;
    const auto use=[&](ID3D12Resource& resource,D3D12_RESOURCE_STATES state,const char* name){return CheckGraphicsResult(states.Use(commands,resource,state,name),text,"D3D12.StateTrackingFailed");};
    const auto stamp=[&](UINT stage,bool end){if(p.timingEnabled)commands.EndQuery(frame.timestampQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,first+stage*2+(end?1:0));};
    const auto fullscreen=[&](UINT pipeline,UINT table,UINT width,UINT height,D3D12_CPU_DESCRIPTOR_HANDLE rtv,se::Vec4 options){
        const auto& program=*content.postPipelines[pipeline];commands.SetPipelineState(program.pipeline.Get());commands.SetGraphicsRootSignature(program.rootSignature.Get());
        const se::rhi::PostProcessConstants constants{{content.forward->exposure,settings.bloomThreshold,settings.bloom?settings.bloomStrength:0,settings.toneMapping?1.0f:0.0f},
            {float(width),float(height),1.0f/float(width),1.0f/float(height)},options};commands.SetGraphicsRoot32BitConstants(0,12,&constants,0);
        ID3D12DescriptorHeap* heaps[]{frame.postDescriptors.Heap()};commands.SetDescriptorHeaps(1,heaps);commands.SetGraphicsRootDescriptorTable(1,frame.postDescriptors.Draw(table));
        commands.OMSetRenderTargets(1,&rtv,FALSE,nullptr);const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
        commands.RSSetViewports(1,&viewport);commands.RSSetScissorRects(1,&scissor);commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands.DrawInstanced(3,1,0,0);
    };
    // G14：速度 Pass 复用主场景深度；解析写历史 ping-pong，ToneMap/Bloom 改读解析结果。
    ID3D12Resource* sceneColor=hdrResource;
    if(settings.taa&&targets.taaKernel&&!frame.opaqueTemporalResolved)
    {
        if(!CheckGraphicsResult(targets.taaKernel->RecordVelocity(commands,states,targets.taa,content.meshes,
            content.viewProjection,settings.taaCurrentClipFromWorld,settings.taaPreviousClipFromWorld,*depthResource),text,"D3D12.TaaRecordFailed"))return false;
        D3D12TaaParameters parameters;
        parameters.staticBlend=settings.taaStaticBlend;parameters.dynamicBlend=settings.taaDynamicBlend;
        parameters.depthThreshold=settings.taaDepthThreshold;parameters.velocityThreshold=settings.taaVelocityThreshold;
        parameters.sharpen=settings.taaSharpen;
        parameters.historyValid=(settings.taaHistoryValid&&targets.taa.frames>0&&targets.taaLastSubmission==p.submittedFrames)?1.0f:0.0f;
        if(!CheckGraphicsResult(targets.taaKernel->Resolve(commands,states,targets.taa,*hdrResource,*depthResource,parameters),
            text,"D3D12.TaaRecordFailed"))return false;
        sceneColor=targets.taa.historyColor[targets.taa.historyIndex].Get();
        targets.taaLastSubmission=p.submittedFrames+1;
    }
    // G15：直方图只读一次 HDR；适应写 1×1 曝光；LUT 只在参数变化时重建。
    // 此处结束时本次写入的曝光纹理已处于 PIXEL_SHADER_RESOURCE，供下面的 ToneMap 采样。
    const bool lutAsset=targets.lutAssetActive&&settings.lutAsset;
    if(lutAsset&&(!targets.lutAssetUploaded||targets.lutAssetRevision!=settings.lutRevision))
    {
        if(!CheckGraphicsResult(targets.exposureKernel->UploadLut(commands,states,targets.exposure,*settings.lutAsset),
            text,"D3D12.ExposureLutUploadFailed"))return false;
        targets.lutAssetUploaded=true;
        targets.lutAssetRevision=settings.lutRevision;
        targets.lutDirty=false;
    }
    const bool rebuildLut=targets.lutDirty&&!lutAsset;
    if(settings.autoExposure||rebuildLut)
    {
        D3D12ExposureParameters parameters;
        parameters.percentiles={-12.0f,4.0f,settings.exposureLowPercentile,settings.exposureHighPercentile};
        parameters.range={settings.exposureKey,settings.exposureMinimum,settings.exposureMaximum,settings.exposureDeltaSeconds};
        parameters.adaptation={settings.exposureBrightAdaptation,settings.exposureDarkAdaptation,0,0};
        parameters.lutLift={settings.lutLift[0],settings.lutLift[1],settings.lutLift[2],float(D3D12Exposure::lutSize)};
        parameters.lutGamma={settings.lutGamma[0],settings.lutGamma[1],settings.lutGamma[2],0};
        parameters.lutGain={settings.lutGain[0],settings.lutGain[1],settings.lutGain[2],0};
        const bool initialize=!targets.exposureHistoryValid||(settings.autoExposure&&!targets.exposureWasEnabled);
        if(!CheckGraphicsResult(targets.exposureKernel->Record(commands,states,targets.exposure,parameters,
            settings.autoExposure,initialize,rebuildLut),text,"D3D12.ExposureRecordFailed"))return false;
        if(settings.autoExposure)targets.exposureHistoryValid=true;
        targets.exposureWasEnabled=settings.autoExposure;
        targets.lutDirty=false;
    }
    for(UINT stage=0;stage<3;++stage)
    {
        stamp(stage,false);
        if(settings.bloom)
        {
            auto& target=targets.bloom[stage==1?1:0];auto* source=stage==0?sceneColor:targets.bloom[stage==1?0:1].resource.Get();
            if(!use(*source,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"BloomInput")||!use(*target.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"BloomTarget"))return false;
            fullscreen(stage==0?0:1,stage,targets.bloomWidth,targets.bloomHeight,target.rtv->Cpu(),{0,stage==1?1.0f:0.0f,stage==2?1.0f:0.0f,0});
        }
        stamp(stage,true);
    }
    if(!use(*sceneColor,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"ToneMapInput")
        ||(settings.bloom&&!use(*targets.bloom[0].resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"BloomComposite"))
        ||!use(*targets.ldr.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"ToneMapTarget"))return false;
    // G15：ToneMap 预算只统计采样+全屏绘制本身；资源屏障属于帧级提交成本，单独由帧时间覆盖。
    stamp(3,false);
    fullscreen(2,3,width,height,targets.ldr.rtv->Cpu(),{settings.autoExposure?1.0f:0.0f,settings.lutEnabled?1.0f:0.0f,0,0});stamp(3,true);
    stamp(4,false);
    const auto destination=content.sceneLdr?content.sceneLdr->color.resource.Get():p.backBuffers[backBufferIndex].Get();
    if(!use(*targets.ldr.resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"FxaaInput")||!use(*destination,D3D12_RESOURCE_STATE_RENDER_TARGET,"FxaaTarget"))return false;
    const auto output=content.sceneLdr?content.sceneLdr->srgbRtv->Cpu():targets.outputRtvs->Cpu(backBufferIndex);fullscreen(3,4,width,height,output,{settings.fxaa?1.0f:0.0f,0,0,0});stamp(4,true);
    stamp(5,false);
    if(!content.overlays.empty())
    {
        const auto& pipeline=*content.postPipelines[4];commands.SetPipelineState(pipeline.pipeline.Get());commands.SetGraphicsRootSignature(pipeline.rootSignature.Get());
        for(const auto& quad:content.overlays)
        {
            const D3D12_RECT scissor{LONG(std::clamp(std::floor(quad.clip.x),0.0f,float(width))),LONG(std::clamp(std::floor(quad.clip.y),0.0f,float(height))),
                LONG(std::clamp(std::ceil(quad.clip.x+quad.clip.z),0.0f,float(width))),LONG(std::clamp(std::ceil(quad.clip.y+quad.clip.w),0.0f,float(height)))};
            if(scissor.right<=scissor.left||scissor.bottom<=scissor.top||quad.rectangle.z<=0||quad.rectangle.w<=0||quad.color.w<=0)continue;
            const se::rhi::OverlayConstants constants{quad.rectangle,quad.color,{float(width),float(height),0,0}};commands.SetGraphicsRoot32BitConstants(0,12,&constants,0);commands.RSSetScissorRects(1,&scissor);commands.DrawInstanced(6,1,0,0);
        }
    }
    stamp(5,true);return true;
}

bool PrepareD3D12OpaqueTemporal(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,
    ID3D12Resource& color,ID3D12Resource& depth,UINT width,UINT height,const Localization& text)
{
    if(!content.post||!content.post->taa||content.opaqueDrawCount>=content.meshes.size())return true;
    if(!frame.post||!frame.post->taaKernel)return false;
    if(!p.opaqueTemporal||p.opaqueTemporal->color.Get()!=&color||p.opaqueTemporal->depth.Get()!=&depth)
    {
        auto next=std::make_shared<D3D12OpaqueTemporal>();next->color=&color;next->depth=&depth;
        if(!CheckGraphicsResult(frame.post->taaKernel->CreateOutput(color,depth,width,height,next->history),text,"D3D12.TaaInputsInvalid"))return false;
        p.opaqueTemporal=std::move(next);
    }
    frame.opaqueTemporal=p.opaqueTemporal;return true;
}
bool RecordD3D12OpaqueTemporal(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,
    std::span<const D3D12MeshDraw> opaque,ID3D12Resource& color,ID3D12Resource& depth,D3D12StateRecording& states,const Localization& text)
{
    if(!content.post||!content.post->taa)return true;
    if(!frame.opaqueTemporal||!frame.post||!frame.post->taaKernel)return false;
    auto& temporal=*frame.opaqueTemporal;auto& commands=*p.commandList.Get();const auto& settings=*content.post;
    if(!CheckGraphicsResult(frame.post->taaKernel->RecordVelocity(commands,states,temporal.history,opaque,
        content.viewProjection,settings.taaCurrentClipFromWorld,settings.taaPreviousClipFromWorld,depth),text,"D3D12.TaaRecordFailed"))return false;
    D3D12TaaParameters parameters;parameters.staticBlend=settings.taaStaticBlend;parameters.dynamicBlend=settings.taaDynamicBlend;
    parameters.depthThreshold=settings.taaDepthThreshold;parameters.velocityThreshold=settings.taaVelocityThreshold;parameters.sharpen=settings.taaSharpen;
    parameters.historyValid=settings.taaHistoryValid&&temporal.history.frames>0&&temporal.lastSubmission==p.submittedFrames?1.f:0.f;
    if(!CheckGraphicsResult(frame.post->taaKernel->Resolve(commands,states,temporal.history,color,depth,parameters),text,"D3D12.TaaRecordFailed"))return false;
    auto& history=*temporal.history.historyColor[temporal.history.historyIndex].Get();
    if(!CheckGraphicsResult(states.Use(commands,history,D3D12_RESOURCE_STATE_COPY_SOURCE,"OpaqueTemporal.CopySource"),text,"D3D12.StateTrackingFailed")
        ||!CheckGraphicsResult(states.Use(commands,color,D3D12_RESOURCE_STATE_COPY_DEST,"OpaqueTemporal.CopyTarget"),text,"D3D12.StateTrackingFailed"))return false;
    // 历史保留纯不透明颜色；只把解析结果复制到当前几何目标，透明不会写回历史。
    commands.CopyResource(&color,&history);temporal.lastSubmission=p.submittedFrames+1;frame.opaqueTemporalResolved=true;return true;
}
