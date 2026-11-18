#pragma once
#include "CoreLayer/Math/Bounds.h"
#include "CoreLayer/Math/Math.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace se
{
struct NavBakeSettings
{
    float cellSizeMeters = 0.25f;
    float agentRadiusMeters = 0.35f;
    float agentHeightMeters = 1.8f;
    float maxSlopeDegrees = 45.0f;
    float maxStepMeters = 0.4f;

    bool operator==(const NavBakeSettings&) const = default;
};

struct NavPolygon
{
    std::vector<std::uint32_t> vertices; // 顶点索引序列（在 XZ 平面围绕中心按正向环绕）
    std::vector<std::int32_t> neighbors; // 与 vertices[i]->vertices[(i+1)%n] 共享的邻接多边形索引，-1 表示实心边界
    Vec3 center{};
    Aabb bounds{};
};

/// XZ 平面有向面积的两倍：(b.x - a.x)*(c.z - a.z) - (b.z - a.z)*(c.x - a.x)。
inline float NavTriArea2XZ(Vec3 a, Vec3 b, Vec3 c) noexcept
{
    return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

/// 纯 CPU 导航网格资产：存储多边形凸网、烘焙参数、平坦 BVH 与 .senav 二进制序列化。
class NavMeshData
{
public:
    void Clear() noexcept;
    void SetData(NavBakeSettings settings, std::vector<Vec3> vertices, std::vector<NavPolygon> polygons);
    void RebuildBvh();

    [[nodiscard]] bool Valid() const noexcept;
    [[nodiscard]] const NavBakeSettings& Settings() const noexcept { return settings_; }
    [[nodiscard]] const std::vector<Vec3>& Vertices() const noexcept { return vertices_; }
    [[nodiscard]] const std::vector<NavPolygon>& Polygons() const noexcept { return polygons_; }
    [[nodiscard]] Aabb WorldBounds() const noexcept { return worldBounds_; }

    /// 计算点在指定多边形 XZ 区域内的最近投影点（含线性/重心中高度插值）。
    [[nodiscard]] Vec3 ProjectPointToPolygon(std::uint32_t polyIndex, Vec3 point) const noexcept;

    /// 利用平坦 BVH 定位与查询包围盒相交且距 point 最近的凸多边形。
    [[nodiscard]] std::optional<std::uint32_t> FindNearestPolygon(
        Vec3 point, Vec3 searchHalfExtents, Vec3* projectedPoint = nullptr) const noexcept;

    bool Serialize(std::vector<std::uint8_t>& outBytes) const;
    static bool Deserialize(std::span<const std::uint8_t> bytes, NavMeshData& outData, std::string& outErrorKey);

private:
    struct BvhNode
    {
        Aabb bounds{};
        std::uint32_t rightOrFirst = 0; // count == 0 时为右子节点索引；count > 0 时为 bvhIndices_ 起始偏移
        std::uint32_t count = 0;
    };

    void BuildBvhNode(std::uint32_t start, std::uint32_t count);

    NavBakeSettings settings_{};
    std::vector<Vec3> vertices_;
    std::vector<NavPolygon> polygons_;
    std::vector<std::uint32_t> bvhIndices_;
    std::vector<BvhNode> bvhNodes_;
    Aabb worldBounds_{};
};
} // namespace se
