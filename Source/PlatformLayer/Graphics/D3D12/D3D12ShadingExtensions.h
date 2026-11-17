#pragma once
#include "D3D12Presentation.h"
struct D3D12ShadingDefaults
{
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> textures;
    std::shared_ptr<D3D12DescriptorRange> views;
    bool initialized=false;
};
struct D3D12ShadingFrame
{
    std::shared_ptr<D3D12ShadingDefaults> defaults;
    D3D12UploadSlice defaultUpload;
    D3D12UploadSlice localLightConstants;
    std::shared_ptr<D3D12ShadowTarget> localShadow;
    std::vector<se::Mat4> localShadowMatrices;
    std::vector<std::array<D3D12_CPU_DESCRIPTOR_HANDLE,D3D12ShadingTextureCount>> views;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources;
    std::vector<std::shared_ptr<D3D12DescriptorRange>> leases;
};
bool PrepareD3D12ShadingExtensions(D3D12Presentation& presentation,D3D12FrameContext& frame,const D3D12FrameContent& content,const Localization& text);
bool RecordD3D12ShadingExtensions(D3D12Presentation& presentation,D3D12FrameContext& frame,D3D12StateRecording& states,const Localization& text);
