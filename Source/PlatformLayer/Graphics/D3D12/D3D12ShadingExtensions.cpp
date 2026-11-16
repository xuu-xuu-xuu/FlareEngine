#include "D3D12ShadingExtensions.h"
#include "D3D12AmbientOcclusion.h"
#include "D3D12Gui.h"
#include "D3D12SceneCoverage.h"
#include "D3D12Memory.h"
#include "D3D12Result.h"
#include <cstring>
bool PrepareD3D12ShadingExtensions(D3D12Presentation& p,D3D12FrameContext& frame,const D3D12FrameContent& content,const Localization& text)
{
    frame.shading.reset();frame.shadingConstants={};frame.shadingMaterials.clear();frame.shadingDescriptorIncrement=0;
    if(!content.forward||!content.forward->shading.enabled)return true;
    Microsoft::WRL::ComPtr<ID3D12Device> device;if(FAILED(p.queue->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))))return false;
    if(!p.shadingDefaults)
    {
        auto defaults=std::make_shared<D3D12ShadingDefaults>();D3D12_RESOURCE_DESC description{};
        description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;description.Width=description.Height=description.DepthOrArraySize=description.MipLevels=1;
        description.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;description.SampleDesc={1,0};
        for(UINT i=0;i<2;++i)
            if(!CheckGraphicsResult(p.memory->Create(description,D3D12_RESOURCE_STATE_COMMON,nullptr,defaults->textures[i],i?"Shading.FlatNormal":"Shading.White"),text,"D3D12.CreateTextureFailed"))return false;
        if(!CheckGraphicsResult(p.srvPool->Allocate(2,defaults->views),text,"D3D12.DescriptorCapacityExceeded"))return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=description.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        for(UINT i=0;i<2;++i)device->CreateShaderResourceView(defaults->textures[i].Get(),&view,defaults->views->Cpu(i));p.shadingDefaults=std::move(defaults);
    }
    auto candidate=std::make_shared<D3D12ShadingFrame>();candidate->defaults=p.shadingDefaults;
    if(!candidate->defaults->initialized)
    {
        if(!CheckGraphicsResult(frame.uploads.Allocate(1024,512,candidate->defaultUpload),text,"D3D12.UploadCapacityExceeded"))return false;
        std::memset(candidate->defaultUpload.cpu,0,1024);
        const std::uint16_t white[]{0x3c00,0x3c00,0x3c00,0x3c00},normal[]{0x3800,0x3800,0x3c00,0x3c00};
        std::memcpy(candidate->defaultUpload.cpu,white,sizeof(white));std::memcpy(candidate->defaultUpload.cpu+512,normal,sizeof(normal));
    }
    const UINT width=frame.coverage?frame.coverage->samples.color.width:content.sceneHdr?content.sceneHdr->color.width:p.width;
    const UINT height=frame.coverage?frame.coverage->samples.color.height:content.sceneHdr?content.sceneHdr->color.height:p.height;
    auto constants=content.forward->shading.constants;
    constants.viewport={float(width),float(height),frame.ambientOcclusion&&frame.ambientOcclusion->statistics.aoValid?1.f:0.f,frame.ambientOcclusion&&frame.ambientOcclusion->statistics.contactValid?1.f:0.f};
    if(!CheckGraphicsResult(frame.uploads.Allocate(512,256,frame.shadingConstants),text,"D3D12.UploadCapacityExceeded"))return false;
    std::memset(frame.shadingConstants.cpu,0,512);std::memcpy(frame.shadingConstants.cpu,&constants,sizeof(constants));
    frame.shadingMaterials.resize(content.meshes.size());candidate->views.resize(content.meshes.size());
    auto occlusion=GetD3D12AmbientOcclusionView(frame);
    if(occlusion.ptr)candidate->resources.push_back(frame.ambientOcclusion->output->visibility);
    for(size_t index=0;index<content.meshes.size();++index)
    {
        const auto& draw=content.meshes[index];auto& table=candidate->views[index];table.fill(candidate->defaults->views->Cpu());table[8]=candidate->defaults->views->Cpu(1);
        if(occlusion.ptr)table[0]=occlusion;
        const auto store=[&](const D3D12Texture* texture,UINT slot,bool srgb)
        {
            if(!texture)return;table[slot]=texture->views->Cpu(srgb?0:1);candidate->resources.push_back(texture->resource);candidate->leases.push_back(texture->views);
        };
        for(UINT slot=0;slot<5;++slot)store(content.shadingTextures[slot],slot+1,false);
        for(UINT slot=0;slot<6;++slot)store(draw.layerTextures[slot],slot+6,slot==3);
        if(!CheckGraphicsResult(frame.uploads.Allocate(256,256,frame.shadingMaterials[index]),text,"D3D12.UploadCapacityExceeded"))return false;
        std::memset(frame.shadingMaterials[index].cpu,0,256);std::memcpy(frame.shadingMaterials[index].cpu,&draw.layers,sizeof(draw.layers));
    }
    frame.shadingDescriptorIncrement=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    frame.shading=std::move(candidate);return true;
}
bool RecordD3D12ShadingExtensions(D3D12Presentation& p,D3D12FrameContext& frame,D3D12StateRecording& states,const Localization& text)
{
    if(!frame.shading)return true;auto& data=*frame.shading;auto& commands=*p.commandList.Get();
    const auto use=[&](ID3D12Resource& resource,D3D12_RESOURCE_STATES state,const char* name){return CheckGraphicsResult(states.Use(commands,resource,state,name),text,"D3D12.StateTrackingFailed");};
    if(!data.defaults->initialized)
    {
        if(!data.defaultUpload.resource||!use(*data.defaultUpload.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,"Shading.DefaultUpload"))return false;
        for(UINT i=0;i<2;++i)
        {
            auto& texture=*data.defaults->textures[i].Get();if(!use(texture,D3D12_RESOURCE_STATE_COPY_DEST,"Shading.DefaultTexture"))return false;
            D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=data.defaultUpload.resource;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint.Offset=data.defaultUpload.offset+UINT64(i)*512;source.PlacedFootprint.Footprint={DXGI_FORMAT_R16G16B16A16_FLOAT,1,1,1,256};
            D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=&texture;destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;commands.CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        }
        data.defaults->initialized=true;
    }
    for(const auto& texture:data.defaults->textures)if(!use(*texture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Shading.DefaultRead"))return false;
    for(const auto& resource:data.resources)if(!use(*resource.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,"Shading.TextureRead"))return false;
    if(!use(*frame.shadingConstants.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Shading.FrameConstants"))return false;
    for(const auto& slice:frame.shadingMaterials)if(!use(*slice.resource,D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,"Shading.MaterialConstants"))return false;
    return true;
}
