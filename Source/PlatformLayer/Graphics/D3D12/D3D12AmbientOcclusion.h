#pragma once
#include "D3D12DepthNormals.h"
#include "D3D12UploadArena.h"
struct alignas(16) D3D12AmbientOcclusionConstants
{
    flare::Mat4 clipFromView,viewFromClip,viewFromWorld;
    flare::Vec4 viewport,ao,contact,lightDirection,modes;
};
static_assert(sizeof(D3D12AmbientOcclusionConstants)==272&&offsetof(D3D12AmbientOcclusionConstants,viewport)==192);
struct D3D12AmbientOcclusionOutput
{
    std::shared_ptr<D3D12DepthNormalTarget> inputs;
    Microsoft::WRL::ComPtr<ID3D12Resource> half,visibility;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    std::shared_ptr<D3D12DescriptorRange> graphicsView;
    UINT width=0,height=0,halfWidth=0,halfHeight=0,increment=0;
    UINT64 allocatedBytes=0;
    bool hasHalf=false;
};
struct D3D12AmbientOcclusionFrame
{
    std::shared_ptr<D3D12AmbientOcclusionOutput> output;
    D3D12UploadSlice constants;
    flare::rhi::ScreenSpaceOcclusionStatistics statistics;
};
class D3D12AmbientOcclusion
{
public:
    HRESULT Initialize(ID3D12Device& device,const std::filesystem::path& shaders,const Localization& text);
    bool CreateOutput(D3D12Presentation& presentation,std::shared_ptr<D3D12DepthNormalTarget> inputs,bool half,
        std::shared_ptr<D3D12AmbientOcclusionOutput>& output,const Localization& text)const;
    bool Record(D3D12Presentation& presentation,D3D12FrameContext& frame,D3D12StateRecording& states,const Localization& text)const;
    D3D12ShaderLoadStatistics ShaderStatistics()const{return statistics_;}
private:
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,2> stages_;
    D3D12ShaderLoadStatistics statistics_;
};
// ShaderBuilder 的纯 CPU 产物入口；运行时不发布文件，缺失/过期才回源编译。
bool WriteD3D12AmbientOcclusionShaders(const std::filesystem::path& source,const std::filesystem::path& outputBase,const Localization& text);
bool ValidD3D12ScreenSpaceSettings(const flare::rhi::ScreenSpaceOcclusionSettings& settings);
bool PrepareD3D12AmbientOcclusion(D3D12Presentation& presentation,D3D12FrameContext& frame,const D3D12FrameContent& content,
    UINT width,UINT height,const Localization& text);
bool RecordD3D12AmbientOcclusion(D3D12Presentation& presentation,D3D12FrameContext& frame,D3D12StateRecording& states,const Localization& text);
D3D12_CPU_DESCRIPTOR_HANDLE GetD3D12AmbientOcclusionView(const D3D12FrameContext& frame);
