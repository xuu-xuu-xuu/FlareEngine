#pragma once
#include "D3D12Presentation.h"
struct D3D12ShadingDefaults
{
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,5> textures; // 0:White, 1:FlatNormal, 2:Black, 3:Gray, 4:FarDepth(1.0)
    std::shared_ptr<D3D12DescriptorRange> views; // 0..3:RGBA16F, 4:NullTex2DArray, 5:FarDepth(R32F)
    bool initialized=false;
};
struct D3D12OpaqueBackdrop
{
    Microsoft::WRL::ComPtr<ID3D12Resource> colorCopy;
    Microsoft::WRL::ComPtr<ID3D12Resource> trackedDepth;
    std::shared_ptr<D3D12DescriptorRange> views; // 0:SceneDepth(R32F), 1:SceneColor(RGBA16F)
    UINT width=0,height=0;
};
struct D3D12ShadingFrame
{
    std::shared_ptr<D3D12ShadingDefaults> defaults;
    std::shared_ptr<D3D12OpaqueBackdrop> backdrop;
    D3D12UploadSlice defaultUpload;
    D3D12UploadSlice localLightConstants;
    std::shared_ptr<D3D12ShadowTarget> localShadow;
    std::vector<flare::Mat4> localShadowMatrices;
    std::vector<std::array<D3D12_CPU_DESCRIPTOR_HANDLE,D3D12ShadingTextureCount>> views;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> preResources;
    Microsoft::WRL::ComPtr<ID3D12Resource> occlusionResource;
    std::vector<std::shared_ptr<D3D12DescriptorRange>> leases;
};
bool PrepareD3D12ShadingExtensions(D3D12Presentation& presentation,D3D12FrameContext& frame,const D3D12FrameContent& content,
    ID3D12Resource* rasterDepth,UINT rasterWidth,UINT rasterHeight,const Localization& text);
bool RecordD3D12ShadingExtensionsPreDepthNormals(D3D12Presentation& presentation,D3D12FrameContext& frame,D3D12StateRecording& states,const Localization& text);
bool RecordD3D12ShadingExtensions(D3D12Presentation& presentation,D3D12FrameContext& frame,D3D12StateRecording& states,const Localization& text);
bool RecordD3D12ShadingExtensionsPreTransparent(D3D12Presentation& presentation,D3D12FrameContext& frame,
    ID3D12Resource& opaqueColor,D3D12StateRecording& states,const Localization& text);
