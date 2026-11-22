#pragma once
#include "PlatformLayer/Graphics/Rhi.h"

inline constexpr std::uint32_t D3D12LocalShadowResolution=512;
struct alignas(16) D3D12LocalLightConstants
{
    flare::Vec4 info;
    std::array<flare::Vec4,flare::rhi::MaximumLocalLights> positionRange,radianceSpot,directionOuter,coneShadow;
    std::array<flare::Mat4,flare::rhi::MaximumLocalLights*6> clipFromWorld;
};
static_assert(offsetof(D3D12LocalLightConstants,clipFromWorld)==528&&sizeof(D3D12LocalLightConstants)==3600);
bool ValidD3D12LocalLight(const flare::rhi::LocalLight& light);
bool PrepareD3D12LocalLightConstants(std::span<const flare::rhi::LocalLight> lights,D3D12LocalLightConstants& output);
