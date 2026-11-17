#pragma once
#include "PlatformLayer/Graphics/Rhi.h"

inline constexpr std::uint32_t D3D12LocalShadowResolution=512;
struct alignas(16) D3D12LocalLightConstants
{
    se::Vec4 info;
    std::array<se::Vec4,se::rhi::MaximumLocalLights> positionRange,radianceSpot,directionOuter,coneShadow;
    std::array<se::Mat4,se::rhi::MaximumLocalLights*6> clipFromWorld;
};
static_assert(offsetof(D3D12LocalLightConstants,clipFromWorld)==528&&sizeof(D3D12LocalLightConstants)==3600);
bool ValidD3D12LocalLight(const se::rhi::LocalLight& light);
bool PrepareD3D12LocalLightConstants(std::span<const se::rhi::LocalLight> lights,D3D12LocalLightConstants& output);
