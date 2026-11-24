#include "D3D12ShadingExtensions.h"
#include "D3D12AmbientOcclusion.h"
#include "D3D12RayTracing.h"
#include "D3D12Gui.h"
#include "D3D12SceneCoverage.h"
#include "D3D12Memory.h"
#include "D3D12Result.h"
#include "D3D12Shadow.h"
#include "D3D12LocalLighting.h"
#include <cstring>
bool PrepareD3D12ShadingExtensions(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,
    ID3D12Resource* rasterDepth,UINT rasterWidth,UINT rasterHeight,const Localization& text)
{
    frame.shading.reset();frame.shadingConstants={};frame.shadingMaterials.clear();frame.shadingDescriptorIncrement=0;
    if(!content.forward||!content.forward->shading.enabled)return true;
    Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))return false;
    if(!p.shadingDefaults)
    {
        auto defaults=std::make_shared<D3D12ShadingDefaults>();D3D12_RESOURCE_DESC description{};
        description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;description.Width=description.Height=description.DepthOrArraySize=description.MipLevels=1;
        description.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;description.SampleDesc={1,0};
        const char* defaultNames[]{"Shading.White","Shading.FlatNormal","Shading.Black","Shading.Gray"};
        for(UINT i=0;i<4;++i)
            if(!CheckGraphicsResult(p.memory->Create(description,D3D12_RESOURCE_STATE_COMMON,nullptr,defaults->textures[i],defaultNames[i]),text,"D3D12.CreateTextureFailed"))return false;
        description.Format=DXGI_FORMAT_R32_FLOAT;
        if(!CheckGraphicsResult(p.memory->Create(description,D3D12_RESOURCE_STATE_COMMON,nullptr,defaults->textures[4],"Shading.FarDepth"),text,"D3D12.CreateTextureFailed"))return false;
        if(!CheckGraphicsResult(p.srvPool->Allocate(6,defaults->views),text,"D3D12.DescriptorCapacityExceeded"))return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        for(UINT i=0;i<4;++i)device->CreateShaderResourceView(defaults->textures[i].Get(),&view,defaults->views->Cpu(i));
        D3D12_SHADER_RESOURCE_VIEW_DESC empty{};empty.Format=DXGI_FORMAT_R32_FLOAT;
        empty.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;empty.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        empty.Texture2DArray.ArraySize=empty.Texture2DArray.MipLevels=1;
        device->CreateShaderResourceView(nullptr,&empty,defaults->views->Cpu(4));
        view.Format=DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(defaults->textures[4].Get(),&view,defaults->views->Cpu(5));
        p.shadingDefaults=std::move(defaults);
    }
    auto candidate=std::make_shared<D3D12ShadingFrame>();candidate->defaults=p.shadingDefaults;
    const auto opaqueCount=std::min(content.opaqueDrawCount,content.meshes.size());
    const bool hasTransparent=opaqueCount<content.meshes.size();
    if(hasTransparent&&rasterDepth&&rasterWidth&&rasterHeight)
    {
        if(!p.opaqueBackdrop||p.opaqueBackdrop->width!=rasterWidth||p.opaqueBackdrop->height!=rasterHeight||p.opaqueBackdrop->trackedDepth.Get()!=rasterDepth)
        {
            auto backdrop=std::make_shared<D3D12OpaqueBackdrop>();
            backdrop->width=rasterWidth;backdrop->height=rasterHeight;backdrop->trackedDepth=rasterDepth;
            D3D12_RESOURCE_DESC copyDesc{};copyDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            copyDesc.Width=rasterWidth;copyDesc.Height=rasterHeight;copyDesc.DepthOrArraySize=copyDesc.MipLevels=1;
            copyDesc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;copyDesc.SampleDesc={1,0};
            if(!CheckGraphicsResult(p.memory->Create(copyDesc,D3D12_RESOURCE_STATE_COMMON,nullptr,backdrop->colorCopy,"Shading.OpaqueSceneCopy"),text,"D3D12.HdrTargetsFailed"))return false;
            if(!CheckGraphicsResult(p.srvPool->Allocate(2,backdrop->views),text,"D3D12.DescriptorCapacityExceeded"))return false;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
            srv.Format=DXGI_FORMAT_R32_FLOAT;
            device->CreateShaderResourceView(rasterDepth,&srv,backdrop->views->Cpu(0));
            srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            device->CreateShaderResourceView(backdrop->colorCopy.Get(),&srv,backdrop->views->Cpu(1));
            p.opaqueBackdrop=std::move(backdrop);
        }
        candidate->backdrop=p.opaqueBackdrop;
    }
    D3D12LocalLightConstants local{};
    if(!PrepareD3D12LocalLightConstants(content.forward->localLights,local))return false;
    if(!CheckGraphicsResult(frame.uploads.Allocate(sizeof(local),256,candidate->localLightConstants),text,"D3D12.UploadCapacityExceeded"))return false;
    std::memcpy(candidate->localLightConstants.cpu,&local,sizeof(local));
    const UINT layers=static_cast<UINT>(local.info.z);
    if(layers)
    {
        if(!content.shadow||!content.shadowPipeline||!AcquireD3D12LocalShadow(p,layers,candidate->localShadow,text))return false;
        candidate->localShadowMatrices.assign(local.clipFromWorld.begin(),local.clipFromWorld.begin()+layers);
        candidate->preResources.push_back(candidate->localShadow->depth);
    }
    if(!candidate->defaults->initialized)
    {
        if(!CheckGraphicsResult(frame.uploads.Allocate(2560,512,candidate->defaultUpload),text,"D3D12.UploadCapacityExceeded"))return false;
        std::memset(candidate->defaultUpload.cpu,0,2560);
        const std::uint16_t white[]{0x3c00,0x3c00,0x3c00,0x3c00},normal[]{0x3800,0x3800,0x3c00,0x3c00};
        const std::uint16_t black[]{0,0,0,0x3c00},gray[]{0x3800,0x3800,0x3800,0x3c00};
        const float farDepth=1.0f;
        std::memcpy(candidate->defaultUpload.cpu,white,sizeof(white));
        std::memcpy(candidate->defaultUpload.cpu+512,normal,sizeof(normal));
        std::memcpy(candidate->defaultUpload.cpu+1024,black,sizeof(black));
        std::memcpy(candidate->defaultUpload.cpu+1536,gray,sizeof(gray));
        std::memcpy(candidate->defaultUpload.cpu+2048,&farDepth,sizeof(farDepth));
    }
    const UINT width=rasterWidth?rasterWidth:(frame.coverage?frame.coverage->samples.color.width:content.sceneHdr?content.sceneHdr->color.width:p.width);
    const UINT height=rasterHeight?rasterHeight:(frame.coverage?frame.coverage->samples.color.height:content.sceneHdr?content.sceneHdr->color.height:p.height);
    auto constants=content.forward->shading.constants;
    const bool rtVis=frame.rayTracing&&frame.rayTracing->visibilityValid;
    const bool aoActive=rtVis?frame.rayTracing->aoValid:(frame.ambientOcclusion&&frame.ambientOcclusion->statistics.aoValid);
    const bool shadowActive=rtVis?frame.rayTracing->shadowsValid:(frame.ambientOcclusion&&frame.ambientOcclusion->statistics.contactValid);
    constants.viewport={float(width),float(height),aoActive?1.f:0.f,shadowActive?1.f:0.f};
    if(!CheckGraphicsResult(frame.uploads.Allocate(512,256,frame.shadingConstants),text,"D3D12.UploadCapacityExceeded"))return false;
    std::memset(frame.shadingConstants.cpu,0,512);std::memcpy(frame.shadingConstants.cpu,&constants,sizeof(constants));
    frame.shadingMaterials.resize(content.meshes.size());candidate->views.resize(content.meshes.size());
    auto occlusion=rtVis?GetD3D12RayTracingVisibilityView(frame):GetD3D12AmbientOcclusionView(frame);
    if(rtVis&&occlusion.ptr)candidate->occlusionResource=frame.rayTracing->targets->visibility;
    else if(occlusion.ptr)candidate->occlusionResource=frame.ambientOcclusion->output->visibility;
    const auto defaultUserCpu=[&](std::uint8_t defKind)->D3D12_CPU_DESCRIPTOR_HANDLE
    {
        if(defKind==3u)return candidate->defaults->views->Cpu(1); // Bump
        if(defKind==1u)return candidate->defaults->views->Cpu(2); // Black
        if(defKind==2u)return candidate->defaults->views->Cpu(3); // Gray
        return candidate->defaults->views->Cpu(0);                // White
    };
    for(size_t index=0;index<content.meshes.size();++index)
    {
        const auto& draw=content.meshes[index];auto& table=candidate->views[index];
        table.fill(candidate->defaults->views->Cpu(0));
        table[8]=candidate->defaults->views->Cpu(1);
        table[12]=candidate->localShadow?candidate->localShadow->srv->Cpu():candidate->defaults->views->Cpu(4);
        const bool isTransparent=(index>=opaqueCount)&&(draw.alphaMode==flare::MaterialAlphaMode::Blend)&&bool(candidate->backdrop);
        table[13]=isTransparent?candidate->backdrop->views->Cpu(0):candidate->defaults->views->Cpu(5); // t26 sceneDepthMap
        table[14]=isTransparent?candidate->backdrop->views->Cpu(1):candidate->defaults->views->Cpu(2); // t27 sceneColorMap
        for(UINT u=0;u<4;++u)table[15+u]=defaultUserCpu(draw.userTextureDefaults[u]);                  // t28..t31 userTex0..3
        if(occlusion.ptr)table[0]=occlusion;
        const auto store=[&](const D3D12Texture* texture,UINT slot,bool srgb)
        {
            if(!texture)return;table[slot]=texture->views->Cpu(srgb?0:1);candidate->preResources.push_back(texture->resource);candidate->leases.push_back(texture->views);
        };
        for(UINT slot=0;slot<5;++slot)store(content.shadingTextures[slot],slot+1,false);
        for(UINT slot=0;slot<6;++slot)store(draw.layerTextures[slot],slot+6,slot==3);
        for(UINT slot=0;slot<4;++slot)store(draw.userTextures[slot],slot+15,!draw.userTextureLinear[slot]);
        if(!CheckGraphicsResult(frame.uploads.Allocate(256,256,frame.shadingMaterials[index]),text,"D3D12.UploadCapacityExceeded"))return false;
        std::memset(frame.shadingMaterials[index].cpu,0,256);std::memcpy(frame.shadingMaterials[index].cpu,&draw.layers,sizeof(draw.layers));
    }
    frame.shadingDescriptorIncrement=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    frame.shading=std::move(candidate);return true;
}
bool RecordD3D12ShadingExtensionsPreDepthNormals(D3D12Presentation& p,D3D12FrameContext& frame,D3D12StateRecording& states,const Localization& text)
{
    if(!frame.shading)return true;auto& data=*frame.shading;auto& commands=*p.commandList.Get();
    const auto use=[&](ID3D12Resource& resource,D3D12_RESOURCE_STATES state,const char* name){return CheckGraphicsResult(states.Use(commands,resource,state,name),text,"D3D12.StateTrackingFailed");};
    if(!data.defaults->initialized)
    {
        if(!data.defaultUpload.resource||!use(*data.defaultUpload.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,"Shading.DefaultUpload"))return false;
        for(UINT i=0;i<5;++i)
        {
            auto& texture=*data.defaults->textures[i].Get();if(!use(texture,D3D12_RESOURCE_STATE_COPY_DEST,"Shading.DefaultTexture"))return false;
            D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=data.defaultUpload.resource;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Offset=data.defaultUpload.offset+UINT64(i)*512;
            source.PlacedFootprint.Footprint={i==4?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R16G16B16A16_FLOAT,1,1,1,256};
            D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=&texture;destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;commands.CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        }
        data.defaults->initialized=true;
    }
    for(const auto& texture:data.defaults->textures)if(!use(*texture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Shading.DefaultRead"))return false;
    for(const auto& resource:data.preResources)if(!use(*resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Shading.TextureRead"))return false;
    if(!use(*frame.shadingConstants.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Shading.FrameConstants"))return false;
    if(!use(*data.localLightConstants.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Shading.LocalLightConstants"))return false;
    for(const auto& slice:frame.shadingMaterials)if(!use(*slice.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Shading.MaterialConstants"))return false;
    return true;
}
bool RecordD3D12ShadingExtensions(D3D12Presentation& p,D3D12FrameContext& frame,D3D12StateRecording& states,const Localization& text)
{
    if(!frame.shading||!frame.shading->occlusionResource)return true;
    return CheckGraphicsResult(states.Use(*p.commandList.Get(),*frame.shading->occlusionResource.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Shading.OcclusionRead"),text,"D3D12.StateTrackingFailed");
}
bool RecordD3D12ShadingExtensionsPreTransparent(D3D12Presentation& p,D3D12FrameContext& frame,
    ID3D12Resource& opaqueColor,D3D12StateRecording& states,const Localization& text)
{
    if(!frame.shading||!frame.shading->backdrop)return true;
    auto& copy=*frame.shading->backdrop->colorCopy.Get();auto& commands=*p.commandList.Get();
    if(!CheckGraphicsResult(states.Use(commands,opaqueColor,D3D12_RESOURCE_STATE_COPY_SOURCE,"Transparent.SceneColorSource"),text,"D3D12.StateTrackingFailed")
        ||!CheckGraphicsResult(states.Use(commands,copy,D3D12_RESOURCE_STATE_COPY_DEST,"Transparent.SceneColorDest"),text,"D3D12.StateTrackingFailed"))return false;
    commands.CopyResource(&copy,&opaqueColor);
    return CheckGraphicsResult(states.Use(commands,copy,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Transparent.SceneColorRead"),text,"D3D12.StateTrackingFailed");
}
