#include "D3D12IndoorLighting.h"
#include "D3D12DepthNormals.h"
#include "D3D12LocalLighting.h"
#include "D3D12Memory.h"
#include "D3D12PipelineCache.h"
#include "D3D12Result.h"
#include "D3D12Shader.h"
#include "D3D12ShadingExtensions.h"
#include "D3D12Shadow.h"
#include "D3D12Texture.h"
#include "PlatformLayer/Graphics/MaterialLayout.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace
{
// 0..10 为图形槽位；11..35 为 5 个计算阶段的独立 (t8..t10, u0..u1) 描述符表切片。
constexpr UINT TableSize=36,LocalShadowSlot=8,EmptyBounceSlot=9,BounceSlot=10;
constexpr UINT SampleSrvSlot=11,SampleUavSlot=14;
constexpr UINT TemporalSrvSlot=16,TemporalUavSlot=19;
constexpr UINT ATrous0SrvSlot=21,ATrous0UavSlot=24;
constexpr UINT ATrous1SrvSlot=26,ATrous1UavSlot=29;
constexpr UINT ATrous2SrvSlot=31,ATrous2UavSlot=34;

bool CreateFrame(D3D12Presentation& p,ID3D12Device& device,ID3D12Resource& color,UINT width,UINT height,
    std::shared_ptr<D3D12IndoorFrame>& output,const Localization& text)
{
    auto frame=std::make_shared<D3D12IndoorFrame>();frame->width=width;frame->height=height;
    auto desc=color.GetDesc();
    if(desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT||desc.SampleDesc.Count!=1||desc.Width!=width||desc.Height!=height)
        return CheckGraphicsResult(E_INVALIDARG,text,"D3D12.PostTargetsFailed");
    desc.Flags=D3D12_RESOURCE_FLAG_NONE;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,TableSize,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(!CheckGraphicsResult(p.memory->Create(desc,D3D12_RESOURCE_STATE_COMMON,nullptr,frame->radiance,"Indoor.SourceHDR"),text,"D3D12.PostTargetsFailed")
        ||!CheckGraphicsResult(device.CreateDescriptorHeap(&heap,IID_PPV_ARGS(frame->descriptors.GetAddressOf())),text,"D3D12.DescriptorCapacityExceeded"))return false;
    output=std::move(frame);return true;
}

bool CreateHistory(D3D12Presentation& p,UINT width,UINT height,UINT bounceWidth,UINT bounceHeight,
    std::shared_ptr<D3D12IndoorHistory>& output,const Localization& text)
{
    auto candidate=std::make_shared<D3D12IndoorHistory>();
    candidate->width=width;candidate->height=height;
    candidate->bounceWidth=bounceWidth;candidate->bounceHeight=bounceHeight;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=bounceWidth;desc.Height=bounceHeight;
    desc.DepthOrArraySize=desc.MipLevels=1;
    desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc={1,0};
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto alloc=[&](ComPtr<ID3D12Resource>& target,const char* name){
        return CheckGraphicsResult(p.memory->Create(desc,D3D12_RESOURCE_STATE_COMMON,nullptr,target,name),text,"D3D12.PostTargetsFailed");
    };
    if(!alloc(candidate->rawBounce,"Indoor.RawBounce")
        ||!alloc(candidate->historyIrradiance[0],"Indoor.HistoryIrradianceA")
        ||!alloc(candidate->historyIrradiance[1],"Indoor.HistoryIrradianceB")
        ||!alloc(candidate->historyMoments[0],"Indoor.HistoryMomentsA")
        ||!alloc(candidate->historyMoments[1],"Indoor.HistoryMomentsB")
        ||!alloc(candidate->atrous[0],"Indoor.ATrousA")
        ||!alloc(candidate->atrous[1],"Indoor.ATrousB"))return false;
    output=std::move(candidate);
    return true;
}

se::Vec4 Vector(se::Vec3 value,float w=0){return {value.x,value.y,value.z,w};}
}

bool D3D12IndoorLighting::Initialize(ID3D12Device& device,const std::filesystem::path& shader,const Localization& text)
{
    const auto check=[&](HRESULT hr){return CheckGraphicsResult(hr,text,"D3D12.CreatePipelineFailed");};
    // 图形根签名与 PSO
    D3D12_DESCRIPTOR_RANGE ranges[]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,8,0,0,0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,25,0,LocalShadowSlot}};
    D3D12_DESCRIPTOR_RANGE bounceRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,8,0,0};
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable={2,ranges};
    for(UINT i=1;i<=2;++i)
    {
        parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[i].Descriptor.ShaderRegister=i==1?0:7;
    }
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[3].DescriptorTable={1,&bounceRange};
    for(auto& parameter:parameters)parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxAnisotropy=1;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{4,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,errors,vertex,pixel;
    if(!check(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors))
        ||!check(device.CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(root.GetAddressOf())))
        ||!CompileD3D12Shader(shader,"VSMain","vs_5_1",vertex,text)
        ||!CompileD3D12Shader(shader,"PSMain","ps_5_1",pixel,text))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();
    desc.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};desc.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
    desc.SampleDesc.Count=1;desc.SampleMask=UINT_MAX;desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;desc.RasterizerState.DepthClipEnable=TRUE;
    desc.NumRenderTargets=1;desc.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    if(!check(device.CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(pipeline.GetAddressOf()))))return false;

    // 计算根签名与 PSO（单帧 SSGI 采样、时域重投影累积、3 趟 À-Trous 空域降噪）
    D3D12_DESCRIPTOR_RANGE sceneSrvs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0};
    D3D12_DESCRIPTOR_RANGE extraSrvs{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,3,8,0,0};
    D3D12_DESCRIPTOR_RANGE uavs{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,0};
    D3D12_ROOT_PARAMETER computeParameters[5]{};
    computeParameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;computeParameters[0].DescriptorTable={1,&sceneSrvs};
    computeParameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;computeParameters[1].Descriptor={0,0};
    computeParameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;computeParameters[2].Constants={1,0,4};
    computeParameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;computeParameters[3].DescriptorTable={1,&extraSrvs};
    computeParameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;computeParameters[4].DescriptorTable={1,&uavs};
    D3D12_ROOT_SIGNATURE_DESC computeSignature{5,computeParameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> computeBlob,computeErrors,sampleCs,temporalCs,atrousCs;
    if(!check(D3D12SerializeRootSignature(&computeSignature,D3D_ROOT_SIGNATURE_VERSION_1,&computeBlob,&computeErrors))
        ||!check(device.CreateRootSignature(0,computeBlob->GetBufferPointer(),computeBlob->GetBufferSize(),IID_PPV_ARGS(computeRoot.GetAddressOf())))
        ||!CompileD3D12Shader(shader,"CSSampleBounce","cs_5_1",sampleCs,text)
        ||!CompileD3D12Shader(shader,"CSTemporalAccumulate","cs_5_1",temporalCs,text)
        ||!CompileD3D12Shader(shader,"CSATrousFilter","cs_5_1",atrousCs,text))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc{};computeDesc.pRootSignature=computeRoot.Get();
    computeDesc.CS={sampleCs->GetBufferPointer(),sampleCs->GetBufferSize()};
    if(!check(device.CreateComputePipelineState(&computeDesc,IID_PPV_ARGS(samplePipeline.GetAddressOf()))))return false;
    computeDesc.CS={temporalCs->GetBufferPointer(),temporalCs->GetBufferSize()};
    if(!check(device.CreateComputePipelineState(&computeDesc,IID_PPV_ARGS(temporalPipeline.GetAddressOf()))))return false;
    computeDesc.CS={atrousCs->GetBufferPointer(),atrousCs->GetBufferSize()};
    return check(device.CreateComputePipelineState(&computeDesc,IID_PPV_ARGS(atrousPipeline.GetAddressOf())));
}

bool RecordD3D12IndoorLighting(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,
    D3D12StateRecording& states,ID3D12Resource& color,D3D12_CPU_DESCRIPTOR_HANDLE rtv,UINT width,UINT height,const Localization& text)
{
    // 屏内跟踪只读完整且与主颜色一致的几何。自定义/实例覆盖未知时整帧沿用原始 HDR。
    if(!frame.depthNormals||!frame.depthNormals->statistics.valid||!frame.depthNormals->statistics.complete||!content.meshPipeline)return true;
    const auto& target=*frame.depthNormals->target;const auto& lighting=*content.forward;const auto& indoor=lighting.indoor;
    if(target.width!=width||target.height!=height||(indoor.SurfaceEnabled()&&!target.material))return true;
    const auto inverseView=se::Inverse(indoor.view),inverseProjection=se::Inverse(indoor.clipFromView);
    if(!inverseView||!inverseProjection||!se::IsFinite(*inverseView)||!se::IsFinite(*inverseProjection))return false;
    ComPtr<ID3D12Device> device;if(!CheckGraphicsResult(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf())),text,"D3D12.CreatePipelineFailed"))return false;
    if(!p.indoorLighting)
    {
        auto next=std::make_shared<D3D12IndoorLighting>();
        const auto path=content.meshPipeline->prepared->program->source.path.parent_path()/"IndoorLighting.hlsl";
        if(!next->Initialize(*device.Get(),path,text))return false;
        p.indoorLighting=std::move(next);
    }
    if(!frame.indoor||frame.indoor->width!=width||frame.indoor->height!=height)
        if(!CreateFrame(p,*device.Get(),color,width,height,frame.indoor,text))return false;
    auto& output=*frame.indoor;
    // 4× 空间采样时栅格是输出两倍；稀疏漫反射按输出尺度取步长，避免重建核在输出上减半而放大采样噪声。
    const UINT stride=content.post&&content.post->supersample4x?8u:4u;
    const float shift=content.post&&content.post->supersample4x?3.f:2.f;
    const UINT bounceWidth=(width+stride-1)/stride,bounceHeight=(height+stride-1)/stride;
    const bool bounce=indoor.indirect&&indoor.indirectStrength>0;

    se::rhi::IndoorLightingConstants constants{};
    constants.view=indoor.view;constants.projection=indoor.clipFromView;
    constants.inverseView=*inverseView;constants.inverseProjection=*inverseProjection;
    constants.viewport={float(width),float(height),1.f/width,1.f/height};
    constants.sampling={shift,1.f/float(stride),float(bounceWidth),float(bounceHeight)};
    constants.strengths={indoor.indirect?indoor.indirectStrength:0.f,indoor.reflections?indoor.reflectionStrength:0.f,
        indoor.volumetricFog?indoor.fogDensity:0.f,indoor.fogDistance};
    constants.fogAlbedo=Vector(indoor.fogAlbedo);
    constants.sunDirection=Vector(lighting.directionToLight);if(lighting.directionalEnabled)constants.sunRadiance=Vector(lighting.directionalRadiance);
    if(content.environment)
    {
        const float yaw=content.environment->yaw;
        constants.environment={content.environment->intensity,std::cos(yaw),std::sin(yaw),float(content.environmentTextures[1]->mipLevels-1)};
    }
    const UINT count=std::min<UINT>(lighting.pointCount,static_cast<UINT>(constants.ordinaryPosition.size()));
    constants.ordinaryLightCount.x=float(count);
    for(UINT i=0;i<count;++i)
    {
        constants.ordinaryPosition[i]=Vector(lighting.points[i].position,lighting.points[i].range);
        constants.ordinaryRadiance[i]=Vector(lighting.points[i].radiance);
    }
    const bool sunShadow=lighting.directionalEnabled&&content.shadow&&content.shadow->enabled&&frame.shadow;
    if(sunShadow)
    {
        const auto& s=*content.shadow;const bool cascaded=s.cascadeCount>0;
        constants.sunClip=s.cascadeClipFromWorld;if(!cascaded)constants.sunClip[0]=s.clipFromWorld;
        constants.sunSplits={s.cascadeSplits[0],s.cascadeSplits[1],s.cascadeSplits[2],0};
        constants.sunShadowParameters={float(cascaded?s.cascadeCount:1),float(frame.shadow->resolution),cascaded?1.f:0.f,s.depthBias};
    }

    UINT prevHistory=0,currHistory=1;
    if(bounce)
    {
        if(!p.indoorHistory||p.indoorHistory->width!=width||p.indoorHistory->height!=height
            ||p.indoorHistory->bounceWidth!=bounceWidth||p.indoorHistory->bounceHeight!=bounceHeight)
        {
            std::shared_ptr<D3D12IndoorHistory> nextHistory;
            if(!CreateHistory(p,width,height,bounceWidth,bounceHeight,nextHistory,text))return false;
            p.indoorHistory=std::move(nextHistory);
        }
        frame.indoorHistory=p.indoorHistory;
        auto& history=*p.indoorHistory;
        prevHistory=history.historyIndex;
        currHistory=1u-prevHistory;
        const bool validHistory=indoor.historyValid&&history.frames>0
            &&history.lastSubmission==p.submittedFrames&&history.lastSceneToken==indoor.sceneToken
            &&se::IsFinite(indoor.previousClipFromWorld);
        constants.previousClipFromWorld=validHistory?indoor.previousClipFromWorld:se::Mat4::Identity();
        constants.frameIndex=indoor.frameIndex;
        constants.historyBlendWeight=std::isfinite(indoor.historyBlendWeight)?std::clamp(indoor.historyBlendWeight,0.f,0.96f):0.88f;
        constants.historyValid=validHistory?1.f:0.f;
        constants.atrousStep=1;
    }
    else
    {
        p.indoorHistory.reset();
        frame.indoorHistory.reset();
    }

    D3D12UploadSlice upload,emptyLocal;
    const UINT size=(sizeof(constants)+255)&~255u;
    if(!CheckGraphicsResult(frame.uploads.Allocate(size,256,upload),text,"D3D12.CreatePipelineFailed"))return false;
    std::memcpy(upload.cpu,&constants,sizeof(constants));
    const bool local=frame.shading&&frame.shading->localLightConstants.resource;
    if(!local)
    {
        // b7 数量为零时不会访问其灯与矩阵数组；仍绑定完整的零常量块。
        constexpr UINT localBytes=(sizeof(D3D12LocalLightConstants)+255)&~255u;
        if(!CheckGraphicsResult(frame.uploads.Allocate(localBytes,256,emptyLocal),text,"D3D12.CreatePipelineFailed"))return false;
        std::memset(emptyLocal.cpu,0,localBytes);
    }

    auto& commands=*p.commandList.Get();
    auto use=[&](ID3D12Resource& resource,D3D12_RESOURCE_STATES state,const char* name)
    {return CheckGraphicsResult(states.Use(commands,resource,state,name),text,"D3D12.StateTrackingFailed");};
    commands.OMSetRenderTargets(0,nullptr,FALSE,nullptr);
    if(!use(color,D3D12_RESOURCE_STATE_COPY_SOURCE,"Indoor.OpaqueHDR")
        ||!use(*output.radiance.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Indoor.SourceHDR"))return false;
    commands.CopyResource(output.radiance.Get(),&color);

    const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto at=[&](UINT slot){auto h=output.descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot)*increment;return h;};
    auto gpuAt=[&](UINT slot){auto h=output.descriptors->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(slot)*increment;return h;};
    auto copy=[&](UINT slot,D3D12_CPU_DESCRIPTOR_HANDLE srv){device->CopyDescriptorsSimple(1,at(slot),srv,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);};
    auto view=[&](UINT slot,ID3D12Resource* resource,DXGI_FORMAT format,D3D12_SRV_DIMENSION dimension)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;srv.ViewDimension=dimension;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if(dimension==D3D12_SRV_DIMENSION_TEXTURE2DARRAY){srv.Texture2DArray.MipLevels=1;srv.Texture2DArray.ArraySize=1;}
        else if(dimension==D3D12_SRV_DIMENSION_TEXTURECUBE)srv.TextureCube.MipLevels=1;
        else srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(resource,&srv,at(slot));
    };
    auto srv2d=[&](UINT slot,ID3D12Resource* resource){view(slot,resource,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_SRV_DIMENSION_TEXTURE2D);};
    auto uav2d=[&](UINT slot,ID3D12Resource* resource)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource,nullptr,&uav,at(slot));
    };

    view(0,output.radiance.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_SRV_DIMENSION_TEXTURE2D);
    copy(1,target.views->Cpu());copy(2,target.views->Cpu(1));
    if(target.material)copy(3,target.views->Cpu(2));else srv2d(3,nullptr);
    if(content.environment)
    {
        for(UINT i=1;i<=2;++i)
        {
            if(!use(*content.environmentTextures[i]->resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.Environment"))return false;
            copy(i+3,content.environmentTextures[i]->views->Cpu(1));
        }
    }
    else
    {
        view(4,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_SRV_DIMENSION_TEXTURECUBE);
        view(5,nullptr,DXGI_FORMAT_R16G16_FLOAT,D3D12_SRV_DIMENSION_TEXTURE2D);
    }
    view(6,nullptr,DXGI_FORMAT_R32_FLOAT,D3D12_SRV_DIMENSION_TEXTURE2D);
    view(7,nullptr,DXGI_FORMAT_R32_FLOAT,D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
    if(sunShadow)
    {
        if(!use(*frame.shadow->depth.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.SunShadow"))return false;
        copy(content.shadow->cascadeCount?7u:6u,frame.shadow->srv->Cpu());
    }
    if(local&&frame.shading->localShadow)
    {
        if(!use(*frame.shading->localShadow->depth.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.LocalShadow"))return false;
        copy(LocalShadowSlot,frame.shading->localShadow->srv->Cpu());
    }
    else view(LocalShadowSlot,nullptr,DXGI_FORMAT_R32_FLOAT,D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
    srv2d(EmptyBounceSlot,nullptr);

    ID3D12DescriptorHeap* heaps[]{output.descriptors.Get()};
    if(bounce)
    {
        auto& history=*p.indoorHistory;
        srv2d(BounceSlot,history.atrous[0].Get());

        srv2d(SampleSrvSlot+0,nullptr);srv2d(SampleSrvSlot+1,nullptr);srv2d(SampleSrvSlot+2,nullptr);
        uav2d(SampleUavSlot+0,history.rawBounce.Get());uav2d(SampleUavSlot+1,nullptr);

        srv2d(TemporalSrvSlot+0,history.rawBounce.Get());
        srv2d(TemporalSrvSlot+1,history.historyIrradiance[prevHistory].Get());
        srv2d(TemporalSrvSlot+2,history.historyMoments[prevHistory].Get());
        uav2d(TemporalUavSlot+0,history.historyIrradiance[currHistory].Get());
        uav2d(TemporalUavSlot+1,history.historyMoments[currHistory].Get());

        srv2d(ATrous0SrvSlot+0,history.historyIrradiance[currHistory].Get());
        srv2d(ATrous0SrvSlot+1,history.historyMoments[currHistory].Get());
        srv2d(ATrous0SrvSlot+2,nullptr);
        uav2d(ATrous0UavSlot+0,history.atrous[0].Get());uav2d(ATrous0UavSlot+1,nullptr);

        srv2d(ATrous1SrvSlot+0,history.atrous[0].Get());
        srv2d(ATrous1SrvSlot+1,history.historyMoments[currHistory].Get());
        srv2d(ATrous1SrvSlot+2,nullptr);
        uav2d(ATrous1UavSlot+0,history.atrous[1].Get());uav2d(ATrous1UavSlot+1,nullptr);

        srv2d(ATrous2SrvSlot+0,history.atrous[1].Get());
        srv2d(ATrous2SrvSlot+1,history.historyMoments[currHistory].Get());
        srv2d(ATrous2SrvSlot+2,nullptr);
        uav2d(ATrous2UavSlot+0,history.atrous[0].Get());uav2d(ATrous2UavSlot+1,nullptr);

        if(!use(*output.radiance.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indoor.ComputeSourceHDR")
            ||!use(*target.depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indoor.ComputeDepth")
            ||!use(*target.normal.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indoor.ComputeNormal"))return false;
        if(target.material&&!use(*target.material.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indoor.ComputeMaterial"))return false;

        commands.SetDescriptorHeaps(1,heaps);
        commands.SetComputeRootSignature(p.indoorLighting->computeRoot.Get());
        commands.SetComputeRootDescriptorTable(0,gpuAt(0));
        commands.SetComputeRootConstantBufferView(1,upload.Gpu());

        const UINT gx=(bounceWidth+7)/8,gy=(bounceHeight+7)/8;
        const auto dispatchCompute=[&](ID3D12PipelineState* pso,UINT stepSize,UINT passIdx,UINT srvSlot,UINT uavSlot){
            const UINT filterParams[4]{stepSize,bounceWidth,bounceHeight,passIdx};
            commands.SetComputeRoot32BitConstants(2,4,filterParams,0);
            commands.SetComputeRootDescriptorTable(3,gpuAt(srvSlot));
            commands.SetComputeRootDescriptorTable(4,gpuAt(uavSlot));
            commands.SetPipelineState(pso);
            commands.Dispatch(gx,gy,1);
        };

        // 1. 单次反弹采样（R2 低差异序列抖动）
        if(!use(*history.rawBounce.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Indoor.RawBounceWrite"))return false;
        dispatchCompute(p.indoorLighting->samplePipeline.Get(),1,0,SampleSrvSlot,SampleUavSlot);
        if(!CheckGraphicsResult(states.UavBarrier(commands,*history.rawBounce.Get(),"Indoor.RawBounceBarrier"),text,"D3D12.StateTrackingFailed")
            ||!use(*history.rawBounce.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indoor.RawBounceRead"))return false;

        // 2. 时域反向重投影 + 邻域方差夹取 + 矩更新
        if(!use(*history.historyIrradiance[prevHistory].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indoor.HistoryIrradianceRead")
            ||!use(*history.historyMoments[prevHistory].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indoor.HistoryMomentsRead")
            ||!use(*history.historyIrradiance[currHistory].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Indoor.HistoryIrradianceWrite")
            ||!use(*history.historyMoments[currHistory].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Indoor.HistoryMomentsWrite"))return false;
        dispatchCompute(p.indoorLighting->temporalPipeline.Get(),1,1,TemporalSrvSlot,TemporalUavSlot);
        if(!CheckGraphicsResult(states.UavBarrier(commands,*history.historyIrradiance[currHistory].Get(),"Indoor.TemporalIrrBarrier"),text,"D3D12.StateTrackingFailed")
            ||!CheckGraphicsResult(states.UavBarrier(commands,*history.historyMoments[currHistory].Get(),"Indoor.TemporalMomBarrier"),text,"D3D12.StateTrackingFailed")
            ||!use(*history.historyIrradiance[currHistory].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indoor.TemporalIrrRead")
            ||!use(*history.historyMoments[currHistory].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,"Indoor.TemporalMomRead"))return false;

        // 3. 三趟法线-深度-方差加权 À-Trous 空域降噪 (step = 1, 2, 4)
        const UINT atrousSteps[3]{1u,2u,4u};
        const UINT atrousSrvSlots[3]{ATrous0SrvSlot,ATrous1SrvSlot,ATrous2SrvSlot};
        const UINT atrousUavSlots[3]{ATrous0UavSlot,ATrous1UavSlot,ATrous2UavSlot};
        for(UINT pass=0;pass<3;++pass)
        {
            ID3D12Resource& outRes=*history.atrous[pass%2u].Get();
            if(!use(outRes,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,"Indoor.ATrousWrite"))return false;
            dispatchCompute(p.indoorLighting->atrousPipeline.Get(),atrousSteps[pass],pass,atrousSrvSlots[pass],atrousUavSlots[pass]);
            if(!CheckGraphicsResult(states.UavBarrier(commands,outRes,"Indoor.ATrousBarrier"),text,"D3D12.StateTrackingFailed"))return false;
            const auto nextState=(pass==2)?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if(!use(outRes,nextState,"Indoor.ATrousRead"))return false;
        }

        history.historyIndex=currHistory;
        ++history.frames;
        history.lastSubmission=p.submittedFrames+1;
        history.lastSceneToken=indoor.sceneToken;
    }
    else
    {
        srv2d(BounceSlot,nullptr);
    }

    if(!use(*output.radiance.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.SourceHDR")
        ||!use(*target.depth.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.Depth")
        ||!use(*target.normal.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.Normal")
        ||!use(color,D3D12_RESOURCE_STATE_RENDER_TARGET,"Indoor.HDR"))return false;
    if(target.material&&!use(*target.material.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.Material"))return false;

    commands.SetGraphicsRootSignature(p.indoorLighting->root.Get());
    commands.SetDescriptorHeaps(1,heaps);
    commands.SetGraphicsRootDescriptorTable(0,gpuAt(0));
    commands.SetGraphicsRootConstantBufferView(1,upload.Gpu());
    commands.SetGraphicsRootConstantBufferView(2,local?frame.shading->localLightConstants.Gpu():emptyLocal.Gpu());
    commands.SetGraphicsRootDescriptorTable(3,gpuAt(bounce?BounceSlot:EmptyBounceSlot));
    commands.SetPipelineState(p.indoorLighting->pipeline.Get());
    const D3D12_VIEWPORT vp{0,0,float(width),float(height),0,1};const D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    commands.RSSetViewports(1,&vp);commands.RSSetScissorRects(1,&scissor);commands.OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands.DrawInstanced(3,1,0,0);
    return true;
}
