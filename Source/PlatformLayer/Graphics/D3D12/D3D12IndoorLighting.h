#pragma once
#include "D3D12Presentation.h"
#include <array>
#include <cstdint>
#include <filesystem>

// 持久跨帧 SSGI 历史缓冲：保存乒乓累积漫反射辐照度、矩/法线历史及 À-Trous 滤波临时目标。
struct D3D12IndoorHistory
{
    UINT width=0,height=0,bounceWidth=0,bounceHeight=0,historyIndex=0;
    UINT64 frames=0,lastSubmission=0;
    std::uint64_t lastSceneToken=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> rawBounce;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> historyIrradiance;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> historyMoments;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> atrous;
};
// 每个在途帧保存自己的 HDR 快照和可写描述符；只有不可变根签名/PSO 跨帧共享。
struct D3D12IndoorFrame
{
    Microsoft::WRL::ComPtr<ID3D12Resource> radiance;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors;
    UINT width=0,height=0;
};
struct D3D12IndoorLighting
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> computeRoot;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> samplePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> temporalPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> atrousPipeline;
    bool Initialize(ID3D12Device&,const std::filesystem::path&,const Localization&);
};
bool RecordD3D12IndoorLighting(D3D12Presentation&,D3D12FrameContext&,const D3D12FrameContent&,D3D12StateRecording&,
    ID3D12Resource& color,D3D12_CPU_DESCRIPTOR_HANDLE rtv,UINT width,UINT height,const Localization&);
