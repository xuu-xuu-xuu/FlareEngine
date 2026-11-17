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
#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT TableSize=11,LocalShadowSlot=8,EmptyBounceSlot=9,BounceSlot=10;
struct alignas(16) IndoorConstants
{
    se::Mat4 view,projection,inverseProjection,inverseView;
    se::Vec4 viewport,strengths,fogAlbedo,sunDirection,sunRadiance,environment,ordinaryLightCount;
    std::array<se::Vec4,8> ordinaryPosition{},ordinaryRadiance{};
    std::array<se::Mat4,3> sunClip{};
    se::Vec4 sunSplits{},sunShadowParameters{},sampling{};
};
static_assert(sizeof(IndoorConstants)==864&&offsetof(IndoorConstants,sunClip)==624);

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
se::Vec4 Vector(se::Vec3 value,float w=0){return {value.x,value.y,value.z,w};}
}
bool D3D12IndoorLighting::Initialize(ID3D12Device& device,const std::filesystem::path& shader,const Localization& text)
{
    const auto check=[&](HRESULT hr){return CheckGraphicsResult(hr,text,"D3D12.CreatePipelineFailed");};
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
    ComPtr<ID3DBlob> blob,errors,vertex,pixel,diffuse;
    if(!check(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors))
        ||!check(device.CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(root.GetAddressOf())))
        ||!CompileD3D12Shader(shader,"VSMain","vs_5_1",vertex,text)||!CompileD3D12Shader(shader,"PSMain","ps_5_1",pixel,text)
        ||!CompileD3D12Shader(shader,"PSDiffuse","ps_5_1",diffuse,text))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();
    desc.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};desc.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
    desc.SampleDesc.Count=1;desc.SampleMask=UINT_MAX;desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;desc.RasterizerState.DepthClipEnable=TRUE;
    desc.NumRenderTargets=1;desc.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    if(!check(device.CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(pipeline.GetAddressOf()))))return false;
    desc.PS={diffuse->GetBufferPointer(),diffuse->GetBufferSize()};
    return check(device.CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(diffusePipeline.GetAddressOf())));
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
    if(bounce&&!output.bounce)
    {
        auto desc=color.GetDesc();desc.Width=bounceWidth;desc.Height=bounceHeight;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> radiance;std::shared_ptr<D3D12DescriptorRange> view;
        if(!CheckGraphicsResult(p.memory->Create(desc,D3D12_RESOURCE_STATE_COMMON,nullptr,radiance,"Indoor.DiffuseIrradiance"),text,"D3D12.PostTargetsFailed")
            ||!CheckGraphicsResult(p.rtvPool->Allocate(1,view),text,"D3D12.DescriptorCapacityExceeded"))return false;
        device->CreateRenderTargetView(radiance.Get(),nullptr,view->Cpu());output.bounce=std::move(radiance);output.bounceRtv=std::move(view);
    }
    else if(!bounce){output.bounce.Reset();output.bounceRtv.reset();}
    IndoorConstants constants{};
    constants.view=indoor.view;constants.projection=indoor.clipFromView;
    constants.inverseView=*inverseView;constants.inverseProjection=*inverseProjection;
    constants.viewport={float(width),float(height),1.f/width,1.f/height};
    constants.sampling={shift,1.f/float(stride),0,0};
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
    if(!use(color,D3D12_RESOURCE_STATE_COPY_SOURCE,"Indoor.OpaqueHDR")||!use(*output.radiance.Get(),D3D12_RESOURCE_STATE_COPY_DEST,"Indoor.SourceHDR"))return false;
    commands.CopyResource(output.radiance.Get(),&color);
    if(!use(*output.radiance.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.SourceHDR")
        ||!use(*target.depth.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.Depth")
        ||!use(*target.normal.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.Normal")
        ||!use(color,D3D12_RESOURCE_STATE_RENDER_TARGET,"Indoor.HDR"))return false;
    if(target.material&&!use(*target.material.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.Material"))return false;
    const UINT increment=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto at=[&](UINT slot){auto h=output.descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot)*increment;return h;};
    auto copy=[&](UINT slot,D3D12_CPU_DESCRIPTOR_HANDLE srv){device->CopyDescriptorsSimple(1,at(slot),srv,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);};
    auto view=[&](UINT slot,ID3D12Resource* resource,DXGI_FORMAT format,D3D12_SRV_DIMENSION dimension)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;srv.ViewDimension=dimension;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if(dimension==D3D12_SRV_DIMENSION_TEXTURE2DARRAY){srv.Texture2DArray.MipLevels=1;srv.Texture2DArray.ArraySize=1;}
        else if(dimension==D3D12_SRV_DIMENSION_TEXTURECUBE)srv.TextureCube.MipLevels=1;
        else srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(resource,&srv,at(slot));
    };
    view(0,output.radiance.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_SRV_DIMENSION_TEXTURE2D);
    copy(1,target.views->Cpu());copy(2,target.views->Cpu(1));
    if(target.material)copy(3,target.views->Cpu(2));else view(3,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_SRV_DIMENSION_TEXTURE2D);
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
    view(EmptyBounceSlot,nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_SRV_DIMENSION_TEXTURE2D);
    view(BounceSlot,output.bounce.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_SRV_DIMENSION_TEXTURE2D);
    commands.SetGraphicsRootSignature(p.indoorLighting->root.Get());
    ID3D12DescriptorHeap* heaps[]{output.descriptors.Get()};commands.SetDescriptorHeaps(1,heaps);
    commands.SetGraphicsRootDescriptorTable(0,heaps[0]->GetGPUDescriptorHandleForHeapStart());
    commands.SetGraphicsRootConstantBufferView(1,upload.Gpu());commands.SetGraphicsRootConstantBufferView(2,local?frame.shading->localLightConstants.Gpu():emptyLocal.Gpu());
    auto draw=[&](ID3D12PipelineState* pipeline,D3D12_CPU_DESCRIPTOR_HANDLE targetView,UINT w,UINT h,UINT bounceSlot)
    {
        auto srv=heaps[0]->GetGPUDescriptorHandleForHeapStart();srv.ptr+=UINT64(bounceSlot)*increment;
        commands.SetGraphicsRootDescriptorTable(3,srv);commands.SetPipelineState(pipeline);
        const D3D12_VIEWPORT vp{0,0,float(w),float(h),0,1};const D3D12_RECT scissor{0,0,LONG(w),LONG(h)};
        commands.RSSetViewports(1,&vp);commands.RSSetScissorRects(1,&scissor);commands.OMSetRenderTargets(1,&targetView,FALSE,nullptr);
        commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands.DrawInstanced(3,1,0,0);
    };
    if(bounce)
    {
        if(!use(*output.bounce.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,"Indoor.DiffuseIrradiance"))return false;
        draw(p.indoorLighting->diffusePipeline.Get(),output.bounceRtv->Cpu(),bounceWidth,bounceHeight,EmptyBounceSlot);
        commands.OMSetRenderTargets(0,nullptr,FALSE,nullptr);
        if(!use(*output.bounce.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Indoor.DiffuseIrradiance"))return false;
    }
    draw(p.indoorLighting->pipeline.Get(),rtv,width,height,BounceSlot);return true;
}
