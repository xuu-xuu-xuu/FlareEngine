#include "NavMeshData.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace flare
{
namespace
{
constexpr char NavMagic[8]{'S', 'E', 'N', 'A', 'V', '0', '0', '1'};
constexpr std::uint32_t NavFormatVersion = 1u;
constexpr std::uint32_t MaximumVertices = 2u * 1024u * 1024u;
constexpr std::uint32_t MaximumPolygons = 512u * 1024u;
constexpr std::uint32_t MaximumPolySides = 64u;
constexpr std::uint32_t BvhLeafThreshold = 4u;

template <typename T>
void AppendPod(std::vector<std::uint8_t>& out, const T& value)
{
    const auto* ptr = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), ptr, ptr + sizeof(T));
}

template <typename T>
bool ReadPod(std::span<const std::uint8_t> bytes, std::size_t& cursor, T& value) noexcept
{
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(T)) return false;
    std::memcpy(&value, bytes.data() + cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

Aabb MergeAabb(const Aabb& a, const Aabb& b) noexcept
{
    return {
        {(std::min)(a.minimum.x, b.minimum.x), (std::min)(a.minimum.y, b.minimum.y), (std::min)(a.minimum.z, b.minimum.z)},
        {(std::max)(a.maximum.x, b.maximum.x), (std::max)(a.maximum.y, b.maximum.y), (std::max)(a.maximum.z, b.maximum.z)}
    };
}

bool AabbIntersects(const Aabb& a, const Aabb& b) noexcept
{
    return a.minimum.x <= b.maximum.x && a.maximum.x >= b.minimum.x
        && a.minimum.y <= b.maximum.y && a.maximum.y >= b.minimum.y
        && a.minimum.z <= b.maximum.z && a.maximum.z >= b.minimum.z;
}

float DistanceSqPointAabb(Vec3 p, const Aabb& box) noexcept
{
    float distSq = 0.0f;
    const float pArr[3]{p.x, p.y, p.z};
    const float minArr[3]{box.minimum.x, box.minimum.y, box.minimum.z};
    const float maxArr[3]{box.maximum.x, box.maximum.y, box.maximum.z};
    for (int axis = 0; axis < 3; ++axis)
    {
        if (pArr[axis] < minArr[axis])
        {
            const float d = minArr[axis] - pArr[axis];
            distSq += d * d;
        }
        else if (pArr[axis] > maxArr[axis])
        {
            const float d = pArr[axis] - maxArr[axis];
            distSq += d * d;
        }
    }
    return distSq;
}
} // namespace

void NavMeshData::Clear() noexcept
{
    settings_ = {};
    vertices_.clear();
    polygons_.clear();
    bvhIndices_.clear();
    bvhNodes_.clear();
    worldBounds_ = {};
}

void NavMeshData::SetData(NavBakeSettings settings, std::vector<Vec3> vertices, std::vector<NavPolygon> polygons)
{
    settings_ = settings;
    vertices_ = std::move(vertices);
    polygons_ = std::move(polygons);
    RebuildBvh();
}

void NavMeshData::BuildBvhNode(std::uint32_t start, std::uint32_t count)
{
    const std::uint32_t nodeIdx = static_cast<std::uint32_t>(bvhNodes_.size());
    bvhNodes_.push_back({});

    Aabb bounds = polygons_[bvhIndices_[start]].bounds;
    Aabb centroidBounds{polygons_[bvhIndices_[start]].center, polygons_[bvhIndices_[start]].center};
    for (std::uint32_t i = 1; i < count; ++i)
    {
        const auto& poly = polygons_[bvhIndices_[start + i]];
        bounds = MergeAabb(bounds, poly.bounds);
        centroidBounds = MergeAabb(centroidBounds, {poly.center, poly.center});
    }
    bvhNodes_[nodeIdx].bounds = bounds;

    if (count <= BvhLeafThreshold)
    {
        bvhNodes_[nodeIdx].rightOrFirst = start;
        bvhNodes_[nodeIdx].count = count;
        return;
    }

    const Vec3 extent = centroidBounds.maximum - centroidBounds.minimum;
    int axis = 0;
    if (extent.z > extent.x && extent.z >= extent.y) axis = 2;
    else if (extent.y > extent.x) axis = 1;

    const auto coord = [axis](Vec3 v) noexcept { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; };
    const std::uint32_t mid = count / 2u;
    std::nth_element(
        bvhIndices_.begin() + start,
        bvhIndices_.begin() + start + mid,
        bvhIndices_.begin() + start + count,
        [&](std::uint32_t a, std::uint32_t b) {
            return coord(polygons_[a].center) < coord(polygons_[b].center);
        });

    bvhNodes_[nodeIdx].count = 0u;
    BuildBvhNode(start, mid);
    const std::uint32_t rightIdx = static_cast<std::uint32_t>(bvhNodes_.size());
    bvhNodes_[nodeIdx].rightOrFirst = rightIdx;
    BuildBvhNode(start + mid, count - mid);
}

void NavMeshData::RebuildBvh()
{
    bvhNodes_.clear();
    bvhIndices_.clear();
    worldBounds_ = {};
    if (polygons_.empty() || vertices_.empty()) return;

    for (auto& poly : polygons_)
    {
        if (poly.vertices.empty()) continue;
        Vec3 sum{};
        Aabb polyBox{vertices_[poly.vertices[0]], vertices_[poly.vertices[0]]};
        for (const std::uint32_t vi : poly.vertices)
        {
            const Vec3 v = vertices_[vi];
            sum += v;
            polyBox = MergeAabb(polyBox, {v, v});
        }
        poly.center = sum * (1.0f / static_cast<float>(poly.vertices.size()));
        poly.bounds = polyBox;
    }

    bvhIndices_.resize(polygons_.size());
    std::iota(bvhIndices_.begin(), bvhIndices_.end(), 0u);
    bvhNodes_.reserve(polygons_.size() * 2u);
    BuildBvhNode(0u, static_cast<std::uint32_t>(polygons_.size()));
    worldBounds_ = bvhNodes_.empty() ? Aabb{} : bvhNodes_.front().bounds;
}

bool NavMeshData::Valid() const noexcept
{
    if (vertices_.empty() || polygons_.empty() || bvhNodes_.empty()) return false;
    for (const Vec3& v : vertices_)
    {
        if (!IsFinite(v)) return false;
    }
    for (const NavPolygon& poly : polygons_)
    {
        if (poly.vertices.size() < 3u || poly.vertices.size() != poly.neighbors.size()) return false;
        for (std::size_t i = 0; i < poly.vertices.size(); ++i)
        {
            if (poly.vertices[i] >= vertices_.size()) return false;
            const std::int32_t nb = poly.neighbors[i];
            if (nb != -1 && (nb < 0 || static_cast<std::size_t>(nb) >= polygons_.size())) return false;
        }
    }
    return true;
}

Vec3 NavMeshData::ProjectPointToPolygon(std::uint32_t polyIndex, Vec3 point) const noexcept
{
    if (polyIndex >= polygons_.size()) return point;
    const auto& poly = polygons_[polyIndex];
    const std::size_t n = poly.vertices.size();
    if (n < 3u) return point;

    bool inside = true;
    for (std::size_t i = 0; i < n; ++i)
    {
        const Vec3& a = vertices_[poly.vertices[i]];
        const Vec3& b = vertices_[poly.vertices[(i + 1u) % n]];
        if (NavTriArea2XZ(a, b, point) < -1e-4f)
        {
            inside = false;
            break;
        }
    }

    if (inside)
    {
        // 采用反距离加权插值求平滑表面高度
        double weightSum = 0.0;
        double ySum = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const Vec3& v = vertices_[poly.vertices[i]];
            const double dx = static_cast<double>(point.x - v.x);
            const double dz = static_cast<double>(point.z - v.z);
            const double dSq = dx * dx + dz * dz;
            if (dSq <= 1e-8) return {point.x, v.y, point.z};
            const double w = 1.0 / dSq;
            weightSum += w;
            ySum += w * static_cast<double>(v.y);
        }
        const float y = weightSum > 0.0 ? static_cast<float>(ySum / weightSum) : poly.center.y;
        return {point.x, y, point.z};
    }

    float bestDistSq = (std::numeric_limits<float>::max)();
    Vec3 bestPoint = poly.center;
    for (std::size_t i = 0; i < n; ++i)
    {
        const Vec3 a = vertices_[poly.vertices[i]];
        const Vec3 b = vertices_[poly.vertices[(i + 1u) % n]];
        const float abx = b.x - a.x;
        const float abz = b.z - a.z;
        const float apx = point.x - a.x;
        const float apz = point.z - a.z;
        const float lenSq = abx * abx + abz * abz;
        const float t = lenSq > 1e-8f ? std::clamp((apx * abx + apz * abz) / lenSq, 0.0f, 1.0f) : 0.0f;
        const Vec3 closest{a.x + abx * t, a.y + (b.y - a.y) * t, a.z + abz * t};
        const float dSq = LengthSquared(closest - point);
        if (dSq < bestDistSq)
        {
            bestDistSq = dSq;
            bestPoint = closest;
        }
    }
    return bestPoint;
}

std::optional<std::uint32_t> NavMeshData::FindNearestPolygon(
    Vec3 point, Vec3 searchHalfExtents, Vec3* projectedPoint) const noexcept
{
    if (bvhNodes_.empty() || !IsFinite(point) || !IsFinite(searchHalfExtents)) return std::nullopt;
    const Vec3 absExt{std::abs(searchHalfExtents.x), std::abs(searchHalfExtents.y), std::abs(searchHalfExtents.z)};
    const Aabb queryBox{point - absExt, point + absExt};

    std::optional<std::uint32_t> bestPoly;
    float bestDistSq = (std::numeric_limits<float>::max)();
    Vec3 bestProj{};

    std::array<std::uint32_t, 64> stack{};
    std::size_t sp = 0;
    stack[sp++] = 0u;

    while (sp > 0u)
    {
        const auto& node = bvhNodes_[stack[--sp]];
        if (!AabbIntersects(node.bounds, queryBox)) continue;
        if (DistanceSqPointAabb(point, node.bounds) > bestDistSq) continue;

        if (node.count > 0u)
        {
            for (std::uint32_t i = 0; i < node.count; ++i)
            {
                const std::uint32_t polyIdx = bvhIndices_[node.rightOrFirst + i];
                const auto& poly = polygons_[polyIdx];
                if (!AabbIntersects(poly.bounds, queryBox)) continue;
                const Vec3 candidate = ProjectPointToPolygon(polyIdx, point);
                if (std::abs(candidate.x - point.x) > absExt.x + 1e-3f
                    || std::abs(candidate.y - point.y) > absExt.y + 1e-3f
                    || std::abs(candidate.z - point.z) > absExt.z + 1e-3f)
                {
                    continue;
                }
                const float dSq = LengthSquared(candidate - point);
                if (!bestPoly || dSq < bestDistSq || (dSq == bestDistSq && polyIdx < *bestPoly))
                {
                    bestPoly = polyIdx;
                    bestDistSq = dSq;
                    bestProj = candidate;
                }
            }
        }
        else
        {
            if (sp + 2u <= stack.size())
            {
                stack[sp++] = node.rightOrFirst;
                stack[sp++] = &node - bvhNodes_.data() + 1u;
            }
        }
    }

    if (bestPoly && projectedPoint) *projectedPoint = bestProj;
    return bestPoly;
}

bool NavMeshData::Serialize(std::vector<std::uint8_t>& outBytes) const
{
    outBytes.clear();
    if (!Valid()) return false;
    outBytes.insert(outBytes.end(), NavMagic, NavMagic + 8);
    AppendPod(outBytes, NavFormatVersion);
    AppendPod(outBytes, settings_);
    const std::uint32_t vertexCount = static_cast<std::uint32_t>(vertices_.size());
    const std::uint32_t polyCount = static_cast<std::uint32_t>(polygons_.size());
    AppendPod(outBytes, vertexCount);
    AppendPod(outBytes, polyCount);
    for (const Vec3& v : vertices_) AppendPod(outBytes, v);
    for (const NavPolygon& poly : polygons_)
    {
        const std::uint32_t edgeCount = static_cast<std::uint32_t>(poly.vertices.size());
        AppendPod(outBytes, edgeCount);
        for (const std::uint32_t vi : poly.vertices) AppendPod(outBytes, vi);
        for (const std::int32_t nb : poly.neighbors) AppendPod(outBytes, nb);
    }
    return true;
}

bool NavMeshData::Deserialize(std::span<const std::uint8_t> bytes, NavMeshData& outData, std::string& outErrorKey)
{
    outErrorKey.clear();
    if (bytes.size() < 8u || std::memcmp(bytes.data(), NavMagic, 8u) != 0)
    {
        outErrorKey = "Navigation.InvalidAsset";
        return false;
    }

    std::size_t cursor = 8u;
    std::uint32_t version = 0u;
    NavBakeSettings settings{};
    std::uint32_t vertexCount = 0u;
    std::uint32_t polyCount = 0u;
    if (!ReadPod(bytes, cursor, version) || version != NavFormatVersion
        || !ReadPod(bytes, cursor, settings)
        || !ReadPod(bytes, cursor, vertexCount) || vertexCount == 0u || vertexCount > MaximumVertices
        || !ReadPod(bytes, cursor, polyCount) || polyCount == 0u || polyCount > MaximumPolygons)
    {
        outErrorKey = "Navigation.InvalidAsset";
        return false;
    }

    if (!IsFinite(settings.cellSizeMeters) || settings.cellSizeMeters <= 0.0f
        || !IsFinite(settings.agentRadiusMeters) || settings.agentRadiusMeters < 0.0f
        || !IsFinite(settings.agentHeightMeters) || settings.agentHeightMeters <= 0.0f
        || !IsFinite(settings.maxSlopeDegrees) || !IsFinite(settings.maxStepMeters))
    {
        outErrorKey = "Navigation.InvalidAsset";
        return false;
    }

    std::vector<Vec3> vertices(vertexCount);
    for (std::uint32_t i = 0; i < vertexCount; ++i)
    {
        if (!ReadPod(bytes, cursor, vertices[i]) || !IsFinite(vertices[i]))
        {
            outErrorKey = "Navigation.InvalidAsset";
            return false;
        }
    }

    std::vector<NavPolygon> polygons(polyCount);
    for (std::uint32_t p = 0; p < polyCount; ++p)
    {
        std::uint32_t edgeCount = 0u;
        if (!ReadPod(bytes, cursor, edgeCount) || edgeCount < 3u || edgeCount > MaximumPolySides)
        {
            outErrorKey = "Navigation.InvalidAsset";
            return false;
        }
        polygons[p].vertices.resize(edgeCount);
        polygons[p].neighbors.resize(edgeCount);
        for (std::uint32_t e = 0; e < edgeCount; ++e)
        {
            if (!ReadPod(bytes, cursor, polygons[p].vertices[e]) || polygons[p].vertices[e] >= vertexCount)
            {
                outErrorKey = "Navigation.InvalidAsset";
                return false;
            }
        }
        for (std::uint32_t e = 0; e < edgeCount; ++e)
        {
            std::int32_t nb = -1;
            if (!ReadPod(bytes, cursor, nb) || (nb != -1 && (nb < 0 || static_cast<std::uint32_t>(nb) >= polyCount)))
            {
                outErrorKey = "Navigation.InvalidAsset";
                return false;
            }
            polygons[p].neighbors[e] = nb;
        }
    }

    if (cursor != bytes.size())
    {
        outErrorKey = "Navigation.InvalidAsset";
        return false;
    }

    outData.SetData(settings, std::move(vertices), std::move(polygons));
    if (!outData.Valid())
    {
        outData.Clear();
        outErrorKey = "Navigation.InvalidAsset";
        return false;
    }
    return true;
}
} // namespace flare
