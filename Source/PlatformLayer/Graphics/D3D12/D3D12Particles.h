#pragma once
#include "D3D12Presentation.h"
#include <filesystem>

bool WriteD3D12ParticleShader(const std::filesystem::path& source, const std::filesystem::path& destination, const Localization& text);

/// D3D12 粒子广告牌渲染通道：独立持有 Root Signature 与 Alpha/Additive 混合 PSO，
/// 通过每帧上传缓冲绑定实例 SRV 并绑定只读场景深度执行单次实例化绘制。
struct D3D12Particles
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> alphaPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> additivePipeline;

    bool Initialize(ID3D12Device& device, const std::filesystem::path& shader, const Localization& text);
};

bool RecordD3D12Particles(D3D12Presentation& presentation, D3D12FrameContext& frame, const D3D12FrameContent& content,
    D3D12StateRecording& states, ID3D12Resource& color, ID3D12Resource& depth,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_CPU_DESCRIPTOR_HANDLE readOnlyDsv,
    UINT width, UINT height, const Localization& text);
