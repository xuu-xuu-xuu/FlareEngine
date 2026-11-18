#pragma once
#include "FunctionLayer/Navigation/NavMeshData.h"
#include <cstdint>
#include <vector>

namespace se
{
enum class NavPathStatus : std::uint8_t
{
    Complete = 0,
    Partial = 1,
    Unreachable = 2
};

struct NavPathResult
{
    NavPathStatus status = NavPathStatus::Unreachable;
    std::vector<Vec3> waypoints;
    float totalLengthMeters = 0.0f;
};

/// 基于已烘焙的 NavMeshData 执行多边形 A* 与 Funnel 漏斗拉直，生成紧凑拐点路径。
[[nodiscard]] NavPathResult FindNavPath(
    const NavMeshData& navMesh,
    Vec3 start,
    Vec3 goal,
    Vec3 queryHalfExtents = {2.0f, 4.0f, 2.0f});
} // namespace se
