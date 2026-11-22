#pragma once
#include <cstdint>

namespace flare::rhi
{
inline constexpr std::uint32_t MaximumSceneTextureDimension=16384;

// 4× 是每轴 2 倍的几何采样；超出纹理尺寸上限时保留完整输出尺寸。
constexpr std::uint32_t SceneRasterScale(bool supersample4x,std::uint32_t width,std::uint32_t height)
{
    return supersample4x&&width<=MaximumSceneTextureDimension/2&&height<=MaximumSceneTextureDimension/2?2u:1u;
}
}
