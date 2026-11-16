#pragma once
#include "D3D12Gui.h"
#include <filesystem>

// 着色器与不可变描述符由使用它们的帧持有，窗口或视口尺寸变化无需等待整条队列。
struct D3D12SceneCoverage
{
    D3D12GuiTarget samples;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> views;

    bool Initialize(D3D12Presentation&,UINT width,UINT height,const std::filesystem::path& shader,const Localization&);
    bool Resolve(D3D12Presentation&,D3D12StateRecording&,ID3D12Resource& color,ID3D12Resource& depth,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,D3D12_CPU_DESCRIPTOR_HANDLE dsv,const Localization&) const;
};
