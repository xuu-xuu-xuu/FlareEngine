#pragma once
#include "D3D12Descriptors.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include <d3d12.h>
#include <wrl/client.h>

struct D3D12Presentation;
struct D3D12FrameContext;
struct D3D12FrameContent;
class D3D12StateRecording;
class Localization;
struct D3D12ShadowTarget
{
    Microsoft::WRL::ComPtr<ID3D12Resource> depth;
    std::shared_ptr<D3D12DescriptorRange> dsv,srv;
    UINT resolution=0;
    UINT layers=1;
};
bool PrepareD3D12Shadow(D3D12Presentation& presentation,D3D12FrameContext& frame,
    const D3D12FrameContent& content,const Localization& text);
bool RecordD3D12Shadow(D3D12Presentation& presentation,D3D12FrameContext& frame,
    const D3D12FrameContent& content,D3D12StateRecording& states,const Localization& text);

// 方向光正交矩阵的真实深度跨度和 texel 世界尺度；非法过滤参数/矩阵保持 output 不变。
bool PrepareD3D12ShadowFilterConstants(const se::rhi::ShadowSettings& settings,se::rhi::ShadowFilterConstants& output);
