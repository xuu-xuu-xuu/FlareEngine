#pragma once
#include "D3D12Presentation.h"
#include <filesystem>

// 每个在途帧保存自己的 HDR 快照和可写描述符；只有不可变根签名/PSO 跨帧共享。
struct D3D12IndoorFrame
{
    Microsoft::WRL::ComPtr<ID3D12Resource> radiance;
    Microsoft::WRL::ComPtr<ID3D12Resource> bounce;
    std::shared_ptr<D3D12DescriptorRange> bounceRtv;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    UINT width=0,height=0;
};
struct D3D12IndoorLighting
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> diffusePipeline;
    bool Initialize(ID3D12Device&,const std::filesystem::path&,const Localization&);
};
bool RecordD3D12IndoorLighting(D3D12Presentation&,D3D12FrameContext&,const D3D12FrameContent&,D3D12StateRecording&,
    ID3D12Resource& color,D3D12_CPU_DESCRIPTOR_HANDLE rtv,UINT width,UINT height,const Localization&);
