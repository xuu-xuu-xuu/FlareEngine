#pragma once
#include "FunctionLayer/Navigation/NavMeshData.h"
#include <cstdint>
#include <span>
#include <string>

namespace se
{
/// 从纯三角形汤烘焙凸多边形导航网格：体素化、可行走净高过滤、半径腐蚀、矩形/T型接缝闭合与邻接构建。
bool BakeNavMesh(
    std::span<const Vec3> vertices,
    std::span<const std::uint32_t> indices,
    const NavBakeSettings& settings,
    NavMeshData& output,
    std::string& errorKey);
} // namespace se
