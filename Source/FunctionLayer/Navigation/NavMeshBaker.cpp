#include "NavMeshBaker.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace flare
{
namespace
{
constexpr std::uint32_t MaxGridCells = 512u * 512u;

struct ColumnSpan
{
    int minV = 0;
    int maxV = 0;
    bool walkable = false;
};

struct WalkNode
{
    int gx = 0;
    int gz = 0;
    int maxV = 0;
    float y = 0.0f;
    std::int32_t neighbor[4]{-1, -1, -1, -1}; // +X, -X, +Z, -Z
    bool keep = false;
    int regionId = -1;
};

struct RegionRect
{
    int x0 = 0, z0 = 0;
    int x1 = 0, z1 = 0; // exclusive grid coords
    std::vector<int> gridIndices; // node index at (z * w + x)
    std::vector<std::uint32_t> polygonVertices;
};

void InsertSpan(std::vector<ColumnSpan>& spans, int minV, int maxV, bool walkable)
{
    if (minV > maxV) std::swap(minV, maxV);
    ColumnSpan incoming{minV, maxV, walkable};

    for (std::size_t i = 0; i < spans.size();)
    {
        auto& s = spans[i];
        if (s.maxV + 1 < incoming.minV)
        {
            ++i;
            continue;
        }
        if (incoming.maxV + 1 < s.minV)
        {
            spans.insert(spans.begin() + static_cast<std::ptrdiff_t>(i), incoming);
            return;
        }
        if (s.maxV > incoming.maxV) incoming.walkable = s.walkable;
        else if (s.maxV == incoming.maxV) incoming.walkable = incoming.walkable || s.walkable;
        incoming.minV = (std::min)(incoming.minV, s.minV);
        incoming.maxV = (std::max)(incoming.maxV, s.maxV);
        spans.erase(spans.begin() + static_cast<std::ptrdiff_t>(i));
    }
    spans.push_back(incoming);
}

bool ValidateSettings(const NavBakeSettings& settings) noexcept
{
    return IsFinite(settings.cellSizeMeters) && settings.cellSizeMeters >= 0.05f && settings.cellSizeMeters <= 10.0f
        && IsFinite(settings.agentRadiusMeters) && settings.agentRadiusMeters >= 0.0f && settings.agentRadiusMeters <= 25.0f
        && IsFinite(settings.agentHeightMeters) && settings.agentHeightMeters >= 0.1f && settings.agentHeightMeters <= 25.0f
        && IsFinite(settings.maxSlopeDegrees) && settings.maxSlopeDegrees >= 0.0f && settings.maxSlopeDegrees <= 89.0f
        && IsFinite(settings.maxStepMeters) && settings.maxStepMeters >= 0.0f && settings.maxStepMeters <= 10.0f;
}

bool ClipTriangleYToCellXZ(Vec3 a, Vec3 b, Vec3 c, float cellMinX, float cellMaxX, float cellMinZ, float cellMaxZ,
    float& outMinY, float& outMaxY) noexcept
{
    // 使用 Sutherland-Hodgman 裁剪三角形到单元格正方形 [cellMinX..cellMaxX, cellMinZ..cellMaxZ]
    Vec3 polyA[12]{a, b, c};
    Vec3 polyB[12]{};
    int count = 3;

    const auto clipPlane = [&](const Vec3* in, int inCount, Vec3* out, int& outCount,
        int axis, bool keepGreater, float threshold) {
        outCount = 0;
        if (inCount <= 0) return;
        const auto val = [axis](Vec3 v) noexcept { return axis == 0 ? v.x : v.z; };
        const auto inside = [&](Vec3 v) noexcept {
            const float cval = val(v);
            return keepGreater ? (cval >= threshold) : (cval <= threshold);
        };
        Vec3 prev = in[inCount - 1];
        bool prevIn = inside(prev);
        for (int i = 0; i < inCount; ++i)
        {
            Vec3 curr = in[i];
            bool currIn = inside(curr);
            if (currIn != prevIn)
            {
                const float d = val(curr) - val(prev);
                const float t = std::abs(d) > 1e-8f ? std::clamp((threshold - val(prev)) / d, 0.0f, 1.0f) : 0.0f;
                out[outCount++] = prev + (curr - prev) * t;
            }
            if (currIn) out[outCount++] = curr;
            prev = curr;
            prevIn = currIn;
        }
    };

    int nextCount = 0;
    clipPlane(polyA, count, polyB, nextCount, 0, true, cellMinX);
    clipPlane(polyB, nextCount, polyA, count, 0, false, cellMaxX);
    clipPlane(polyA, count, polyB, nextCount, 2, true, cellMinZ);
    clipPlane(polyB, nextCount, polyA, count, 2, false, cellMaxZ);
    if (count == 0) return false;

    float minY = polyA[0].y;
    float maxY = polyA[0].y;
    for (int i = 1; i < count; ++i)
    {
        minY = (std::min)(minY, polyA[i].y);
        maxY = (std::max)(maxY, polyA[i].y);
    }
    outMinY = minY;
    outMaxY = maxY;
    return true;
}

struct EdgeKey
{
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    bool operator==(const EdgeKey&) const = default;
};

struct EdgeKeyHash
{
    std::size_t operator()(const EdgeKey& e) const noexcept
    {
        return (static_cast<std::size_t>(e.a) << 32u) | static_cast<std::size_t>(e.b);
    }
};
} // namespace

bool BakeNavMesh(
    std::span<const Vec3> vertices,
    std::span<const std::uint32_t> indices,
    const NavBakeSettings& settings,
    NavMeshData& output,
    std::string& errorKey)
{
    errorKey.clear();
    output.Clear();

    if (!ValidateSettings(settings))
    {
        errorKey = "Navigation.InvalidSettings";
        return false;
    }
    if (vertices.empty() || indices.size() < 3u || (indices.size() % 3u) != 0u)
    {
        errorKey = "Navigation.EmptyGeometry";
        return false;
    }

    Aabb bounds{};
    bool hasValidTri = false;
    for (std::size_t i = 0; i < indices.size(); i += 3u)
    {
        const std::uint32_t i0 = indices[i], i1 = indices[i + 1u], i2 = indices[i + 2u];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
        {
            errorKey = "Navigation.EmptyGeometry";
            return false;
        }
        const Vec3& v0 = vertices[i0];
        const Vec3& v1 = vertices[i1];
        const Vec3& v2 = vertices[i2];
        if (!IsFinite(v0) || !IsFinite(v1) || !IsFinite(v2)) continue;
        const Vec3 n = Cross(v1 - v0, v2 - v0);
        if (LengthSquared(n) <= 1e-12f) continue;

        for (const Vec3& v : {v0, v1, v2})
        {
            if (!hasValidTri)
            {
                bounds = {v, v};
                hasValidTri = true;
            }
            else
            {
                bounds.minimum = {(std::min)(bounds.minimum.x, v.x), (std::min)(bounds.minimum.y, v.y), (std::min)(bounds.minimum.z, v.z)};
                bounds.maximum = {(std::max)(bounds.maximum.x, v.x), (std::max)(bounds.maximum.y, v.y), (std::max)(bounds.maximum.z, v.z)};
            }
        }
    }
    if (!hasValidTri)
    {
        errorKey = "Navigation.EmptyGeometry";
        return false;
    }

    const float cs = settings.cellSizeMeters;
    const float ch = (std::max)(0.02f, (std::min)(settings.maxStepMeters > 0.0f ? settings.maxStepMeters * 0.25f : cs * 0.25f, cs * 0.5f));
    const float originX = bounds.minimum.x - cs;
    const float originY = bounds.minimum.y - ch;
    const float originZ = bounds.minimum.z - cs;
    const int gridW = static_cast<int>(std::ceil((bounds.maximum.x - originX) / cs)) + 1;
    const int gridD = static_cast<int>(std::ceil((bounds.maximum.z - originZ) / cs)) + 1;
    if (gridW <= 2 || gridD <= 2 || static_cast<std::uint64_t>(gridW) * static_cast<std::uint64_t>(gridD) > MaxGridCells)
    {
        errorKey = "Navigation.BakeBoundsTooLarge";
        return false;
    }

    std::vector<std::vector<ColumnSpan>> columns(static_cast<std::size_t>(gridW) * static_cast<std::size_t>(gridD));
    const float minSlopeCos = std::cos(settings.maxSlopeDegrees * (Pi / 180.0f));
    const int minCeilingVoxels = static_cast<int>(std::ceil(settings.agentHeightMeters / ch));

    for (std::size_t i = 0; i < indices.size(); i += 3u)
    {
        const Vec3 a = vertices[indices[i]];
        const Vec3 b = vertices[indices[i + 1u]];
        const Vec3 c = vertices[indices[i + 2u]];
        if (!IsFinite(a) || !IsFinite(b) || !IsFinite(c)) continue;
        Vec3 n = Cross(b - a, c - a);
        const float nLenSq = LengthSquared(n);
        if (nLenSq <= 1e-12f) continue;
        n = n * (1.0f / std::sqrt(nLenSq));
        const bool walkable = n.y >= minSlopeCos;

        const float triMinX = (std::min)({a.x, b.x, c.x});
        const float triMaxX = (std::max)({a.x, b.x, c.x});
        const float triMinZ = (std::min)({a.z, b.z, c.z});
        const float triMaxZ = (std::max)({a.z, b.z, c.z});
        const int x0 = std::clamp(static_cast<int>(std::floor((triMinX - originX) / cs)), 0, gridW - 1);
        const int x1 = std::clamp(static_cast<int>(std::floor((triMaxX - originX) / cs)), 0, gridW - 1);
        const int z0 = std::clamp(static_cast<int>(std::floor((triMinZ - originZ) / cs)), 0, gridD - 1);
        const int z1 = std::clamp(static_cast<int>(std::floor((triMaxZ - originZ) / cs)), 0, gridD - 1);

        for (int gz = z0; gz <= z1; ++gz)
        {
            const float cellMinZ = originZ + static_cast<float>(gz) * cs;
            const float cellMaxZ = cellMinZ + cs;
            for (int gx = x0; gx <= x1; ++gx)
            {
                const float cellMinX = originX + static_cast<float>(gx) * cs;
                const float cellMaxX = cellMinX + cs;
                float cMinY = 0.0f, cMaxY = 0.0f;
                if (!ClipTriangleYToCellXZ(a, b, c, cellMinX, cellMaxX, cellMinZ, cellMaxZ, cMinY, cMaxY)) continue;

                const int sMin = static_cast<int>(std::floor((cMinY - originY) / ch));
                const int sMax = (std::max)(sMin, static_cast<int>(std::ceil((cMaxY - originY) / ch)));
                InsertSpan(columns[static_cast<std::size_t>(gz) * gridW + gx], sMin, sMax, walkable);
            }
        }
    }

    // 过滤头顶净空不足与创建可行走节点图
    std::vector<WalkNode> nodes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> columnNodeRange(columns.size(), {0u, 0u});
    for (int gz = 0; gz < gridD; ++gz)
    {
        for (int gx = 0; gx < gridW; ++gx)
        {
            const std::size_t colIdx = static_cast<std::size_t>(gz) * gridW + gx;
            auto& spans = columns[colIdx];
            const std::uint32_t beginIdx = static_cast<std::uint32_t>(nodes.size());
            for (std::size_t s = 0; s < spans.size(); ++s)
            {
                if (!spans[s].walkable) continue;
                if (s + 1u < spans.size() && (spans[s + 1u].minV - spans[s].maxV) < minCeilingVoxels)
                {
                    continue;
                }
                WalkNode node{};
                node.gx = gx;
                node.gz = gz;
                node.maxV = spans[s].maxV;
                node.y = originY + static_cast<float>(spans[s].maxV) * ch;
                nodes.push_back(node);
            }
            columnNodeRange[colIdx] = {beginIdx, static_cast<std::uint32_t>(nodes.size())};
        }
    }
    if (nodes.empty())
    {
        errorKey = "Navigation.NoWalkableSurface";
        return false;
    }

    // 连接 4 邻域高度差 <= maxStepMeters 的可行走节点
    constexpr int dx[4]{1, -1, 0, 0};
    constexpr int dz[4]{0, 0, 1, -1};
    for (std::size_t idx = 0; idx < nodes.size(); ++idx)
    {
        WalkNode& cur = nodes[idx];
        for (int dir = 0; dir < 4; ++dir)
        {
            const int nx = cur.gx + dx[dir];
            const int nz = cur.gz + dz[dir];
            if (nx < 0 || nx >= gridW || nz < 0 || nz >= gridD) continue;
            const auto [beginIdx, endIdx] = columnNodeRange[static_cast<std::size_t>(nz) * gridW + nx];
            std::int32_t bestNb = -1;
            float bestDiff = settings.maxStepMeters + 1e-4f;
            for (std::uint32_t cand = beginIdx; cand < endIdx; ++cand)
            {
                const float diff = std::abs(nodes[cand].y - cur.y);
                if (diff <= bestDiff)
                {
                    bestDiff = diff;
                    bestNb = static_cast<std::int32_t>(cand);
                }
            }
            cur.neighbor[dir] = bestNb;
        }
    }

    // 半径腐蚀（距离墙/悬崖小于 agentRadiusMeters 的表面移除）
    std::vector<int> cliffDist(nodes.size(), -1);
    std::queue<std::uint32_t> bfs;
    for (std::size_t idx = 0; idx < nodes.size(); ++idx)
    {
        const auto& n = nodes[idx];
        bool boundary = false;
        for (int dir = 0; dir < 4; ++dir)
        {
            if (n.neighbor[dir] < 0) { boundary = true; break; }
        }
        if (boundary)
        {
            cliffDist[idx] = 0;
            bfs.push(static_cast<std::uint32_t>(idx));
        }
    }
    while (!bfs.empty())
    {
        const std::uint32_t u = bfs.front();
        bfs.pop();
        const int nextDist = cliffDist[u] + 1;
        for (int dir = 0; dir < 4; ++dir)
        {
            const std::int32_t nb = nodes[u].neighbor[dir];
            if (nb >= 0 && cliffDist[static_cast<std::size_t>(nb)] == -1)
            {
                cliffDist[static_cast<std::size_t>(nb)] = nextDist;
                bfs.push(static_cast<std::uint32_t>(nb));
            }
        }
    }

    bool anyKept = false;
    for (std::size_t idx = 0; idx < nodes.size(); ++idx)
    {
        const float distFromEdgeMeters = (static_cast<float>(cliffDist[idx]) + 0.5f) * cs;
        const bool keep = settings.agentRadiusMeters <= 1e-4f || cliffDist[idx] < 0 || distFromEdgeMeters >= settings.agentRadiusMeters;
        nodes[idx].keep = keep;
        if (keep) anyKept = true;
    }
    if (!anyKept)
    {
        errorKey = "Navigation.NoWalkableSurface";
        return false;
    }

    // 贪心矩形区域合并（保证每个多边形天然凸且角点落在网格整格线，易于平滑接缝闭合）
    std::vector<std::uint8_t> visited(nodes.size(), 0u);
    std::vector<RegionRect> regions;
    const auto canMergeAt = [&](int baseNode, int tx, int tz, int& outNode) -> bool {
        if (tx < 0 || tx >= gridW || tz < 0 || tz >= gridD) return false;
        const auto [beginIdx, endIdx] = columnNodeRange[static_cast<std::size_t>(tz) * gridW + tx];
        for (std::uint32_t cand = beginIdx; cand < endIdx; ++cand)
        {
            if (!visited[cand] && nodes[cand].keep && std::abs(nodes[cand].y - nodes[baseNode].y) <= settings.maxStepMeters)
            {
                outNode = static_cast<int>(cand);
                return true;
            }
        }
        return false;
    };

    for (std::size_t idx = 0; idx < nodes.size(); ++idx)
    {
        if (!nodes[idx].keep || visited[idx]) continue;

        const int x0 = nodes[idx].gx;
        const int z0 = nodes[idx].gz;
        int x1 = x0 + 1;
        int prevRowRight = static_cast<int>(idx);
        while (x1 < gridW)
        {
            const std::int32_t nb = nodes[static_cast<std::size_t>(prevRowRight)].neighbor[0]; // +X
            if (nb < 0 || visited[static_cast<std::size_t>(nb)] || !nodes[static_cast<std::size_t>(nb)].keep
                || std::abs(nodes[static_cast<std::size_t>(nb)].y - nodes[idx].y) > settings.maxStepMeters)
            {
                break;
            }
            prevRowRight = nb;
            ++x1;
        }

        RegionRect rect;
        rect.x0 = x0;
        rect.z0 = z0;
        rect.x1 = x1;
        rect.z1 = z0 + 1;

        for (int gz = z0; gz < gridD; ++gz)
        {
            if (gz > z0)
            {
                bool rowOk = true;
                const int w = x1 - x0;
                const std::size_t prevRowOffset = rect.gridIndices.size() - static_cast<std::size_t>(w);
                for (int gx = x0; gx < x1; ++gx)
                {
                    int dummy = -1;
                    if (!canMergeAt(static_cast<int>(idx), gx, gz, dummy))
                    {
                        rowOk = false;
                        break;
                    }
                    // 确保与上一行的对应列相连（跨越边界悬崖时不可强行合并）
                    const int prevNode = rect.gridIndices[prevRowOffset + static_cast<std::size_t>(gx - x0)];
                    if (nodes[static_cast<std::size_t>(prevNode)].neighbor[2] != dummy)
                    {
                        rowOk = false;
                        break;
                    }
                }
                if (!rowOk) break;
                rect.z1 = gz + 1;
            }

            for (int gx = x0; gx < x1; ++gx)
            {
                int nodeId = -1;
                canMergeAt(static_cast<int>(idx), gx, gz, nodeId);
                visited[static_cast<std::size_t>(nodeId)] = 1u;
                nodes[static_cast<std::size_t>(nodeId)].regionId = static_cast<int>(regions.size());
                rect.gridIndices.push_back(nodeId);
            }
        }
        regions.push_back(std::move(rect));
    }

    // 按格网交点周边 4 格内的局部 WalkNode::neighbor 连通分量构建 CornerCluster，避免跨不可通行断崖误焊顶点，
    // 并在切分共线 T 型接缝与跨区连通变化点后基于真实 neighbor 校验建立邻接传送门。
    struct LatticeKey
    {
        int ix = 0;
        int iz = 0;
        bool operator==(const LatticeKey&) const = default;
    };
    struct LatticeKeyHash
    {
        std::size_t operator()(const LatticeKey& k) const noexcept
        {
            return (static_cast<std::size_t>(k.ix) * 73856093u) ^ (static_cast<std::size_t>(k.iz) * 19349663u);
        }
    };
    struct CornerCluster
    {
        double sumY = 0.0;
        int count = 0;
        std::uint32_t vertexIndex = UINT32_MAX;
        std::vector<int> members;
    };
    struct PolyEdgeInfo
    {
        int outDir = -1; // 0:+X, 1:-X, 2:+Z, 3:-Z
        int spanBegin = 0;
        int spanEnd = 0;
    };

    std::unordered_map<LatticeKey, std::vector<CornerCluster>, LatticeKeyHash> cornerClusters;
    const auto buildCornerIfNeeded = [&](int ix, int iz) {
        const LatticeKey key{ix, iz};
        auto [it, inserted] = cornerClusters.try_emplace(key);
        if (!inserted) return;

        std::vector<int> incident;
        incident.reserve(4);
        for (int dz = -1; dz <= 0; ++dz)
        {
            const int cz = iz + dz;
            if (cz < 0 || cz >= gridD) continue;
            for (int dx = -1; dx <= 0; ++dx)
            {
                const int cx = ix + dx;
                if (cx < 0 || cx >= gridW) continue;
                const auto [beginIdx, endIdx] = columnNodeRange[static_cast<std::size_t>(cz) * gridW + cx];
                for (std::uint32_t cand = beginIdx; cand < endIdx; ++cand)
                {
                    if (nodes[cand].keep) incident.push_back(static_cast<int>(cand));
                }
            }
        }
        if (incident.empty()) return;

        std::vector<std::uint8_t> compSeen(incident.size(), 0u);
        std::queue<int> q;
        for (std::size_t i = 0; i < incident.size(); ++i)
        {
            if (compSeen[i]) continue;
            CornerCluster cluster;
            compSeen[i] = 1u;
            q.push(incident[i]);
            while (!q.empty())
            {
                const int u = q.front();
                q.pop();
                cluster.sumY += static_cast<double>(nodes[static_cast<std::size_t>(u)].y);
                cluster.count += 1;
                cluster.members.push_back(u);
                for (int dir = 0; dir < 4; ++dir)
                {
                    const std::int32_t nb = nodes[static_cast<std::size_t>(u)].neighbor[dir];
                    if (nb < 0 || !nodes[static_cast<std::size_t>(nb)].keep) continue;
                    const auto itInc = std::find(incident.begin(), incident.end(), nb);
                    if (itInc == incident.end()) continue;
                    const std::size_t incIdx = static_cast<std::size_t>(itInc - incident.begin());
                    if (!compSeen[incIdx])
                    {
                        compSeen[incIdx] = 1u;
                        q.push(nb);
                    }
                }
            }
            it->second.push_back(std::move(cluster));
        }
    };

    const auto regionCellNode = [&](const RegionRect& r, int gx, int gz) noexcept -> int {
        const int lx = std::clamp(gx, r.x0, r.x1 - 1) - r.x0;
        const int lz = std::clamp(gz, r.z0, r.z1 - 1) - r.z0;
        return r.gridIndices[static_cast<std::size_t>(lz) * (r.x1 - r.x0) + lx];
    };

    // 收集共线分割坐标（包含每个区块角点以及侧边对外跨区连通性发生变化的格网交点）
    std::map<int, std::vector<int>> splitXAtZ;
    std::map<int, std::vector<int>> splitZAtX;
    for (const auto& r : regions)
    {
        splitXAtZ[r.z0].push_back(r.x0);
        splitXAtZ[r.z0].push_back(r.x1);
        splitXAtZ[r.z1].push_back(r.x0);
        splitXAtZ[r.z1].push_back(r.x1);
        splitZAtX[r.x0].push_back(r.z0);
        splitZAtX[r.x0].push_back(r.z1);
        splitZAtX[r.x1].push_back(r.z0);
        splitZAtX[r.x1].push_back(r.z1);

        const auto recordSideSplits = [&](int fixed, int begin, int end, int outDir, bool horiz) {
            int prevRegion = -2;
            for (int k = begin; k < end; ++k)
            {
                const int nodeId = horiz ? regionCellNode(r, k, fixed) : regionCellNode(r, fixed, k);
                const std::int32_t nb = nodes[static_cast<std::size_t>(nodeId)].neighbor[outDir];
                const int targetReg = (nb >= 0 && nodes[static_cast<std::size_t>(nb)].keep
                    && nodes[static_cast<std::size_t>(nb)].neighbor[outDir ^ 1] == nodeId)
                    ? nodes[static_cast<std::size_t>(nb)].regionId
                    : -1;
                if (k > begin && targetReg != prevRegion)
                {
                    if (horiz) splitXAtZ[horiz ? (outDir == 3 ? r.z0 : r.z1) : fixed].push_back(k);
                    else splitZAtX[outDir == 1 ? r.x0 : r.x1].push_back(k);
                }
                prevRegion = targetReg;
            }
        };
        recordSideSplits(r.z0, r.x0, r.x1, 3, true);
        recordSideSplits(r.z1 - 1, r.x0, r.x1, 2, true);
        recordSideSplits(r.x0, r.z0, r.z1, 1, false);
        recordSideSplits(r.x1 - 1, r.z0, r.z1, 0, false);

        const int w = r.x1 - r.x0;
        const int h = r.z1 - r.z0;
        for (int lz = 0; lz < h; ++lz)
        {
            for (int lx = 0; lx < w; ++lx)
            {
                const int gx = r.x0 + lx;
                const int gz = r.z0 + lz;
                buildCornerIfNeeded(gx, gz);
                buildCornerIfNeeded(gx + 1, gz);
                buildCornerIfNeeded(gx + 1, gz + 1);
                buildCornerIfNeeded(gx, gz + 1);
            }
        }
    }
    for (auto& [_, xs] : splitXAtZ)
    {
        std::sort(xs.begin(), xs.end());
        xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    }
    for (auto& [_, zs] : splitZAtX)
    {
        std::sort(zs.begin(), zs.end());
        zs.erase(std::unique(zs.begin(), zs.end()), zs.end());
    }

    std::vector<Vec3> navVertices;
    const auto resolveVertex = [&](int ix, int iz, int refNodeId) -> std::uint32_t {
        buildCornerIfNeeded(ix, iz);
        auto& clusters = cornerClusters[{ix, iz}];
        CornerCluster* target = nullptr;
        for (auto& c : clusters)
        {
            if (std::find(c.members.begin(), c.members.end(), refNodeId) != c.members.end())
            {
                target = &c;
                break;
            }
        }
        if (target && target->vertexIndex != UINT32_MAX) return target->vertexIndex;
        const float finalY = target && target->count > 0
            ? static_cast<float>(target->sumY / static_cast<double>(target->count))
            : nodes[static_cast<std::size_t>(refNodeId)].y;
        const std::uint32_t idx = static_cast<std::uint32_t>(navVertices.size());
        navVertices.push_back({originX + static_cast<float>(ix) * cs, finalY, originZ + static_cast<float>(iz) * cs});
        if (target) target->vertexIndex = idx;
        return idx;
    };

    // 构建闭合多边形顶点环与精确的侧边源信息，避免跨断崖误连接与 T-Junction 裂缝
    std::vector<NavPolygon> navPolygons(regions.size());
    std::vector<std::vector<PolyEdgeInfo>> polyEdges(regions.size());
    std::unordered_map<EdgeKey, std::uint32_t, EdgeKeyHash> halfEdgeOwner;

    for (std::uint32_t rIdx = 0; rIdx < regions.size(); ++rIdx)
    {
        const auto& r = regions[rIdx];
        auto& verts = navPolygons[rIdx].vertices;
        auto& edges = polyEdges[rIdx];

        std::vector<int> bottomXs, topXs, rightZs, leftZs;
        for (int ix : splitXAtZ[r.z0]) if (ix >= r.x0 && ix <= r.x1) bottomXs.push_back(ix);
        for (int iz : splitZAtX[r.x1]) if (iz >= r.z0 && iz <= r.z1) rightZs.push_back(iz);
        const auto& allTopXs = splitXAtZ[r.z1];
        for (auto it = allTopXs.rbegin(); it != allTopXs.rend(); ++it) if (*it >= r.x0 && *it <= r.x1) topXs.push_back(*it);
        const auto& allLeftZs = splitZAtX[r.x0];
        for (auto it = allLeftZs.rbegin(); it != allLeftZs.rend(); ++it) if (*it >= r.z0 && *it <= r.z1) leftZs.push_back(*it);

        // 底边：z = z0, x0 -> x1 (外法线 -Z = 3)
        for (std::size_t k = 0; k + 1u < bottomXs.size(); ++k)
        {
            const int xA = bottomXs[k];
            const int xB = bottomXs[k + 1u];
            verts.push_back(resolveVertex(xA, r.z0, regionCellNode(r, xA, r.z0)));
            edges.push_back({3, xA, xB});
        }
        // 右边：x = x1, z0 -> z1 (外法线 +X = 0)
        for (std::size_t k = 0; k + 1u < rightZs.size(); ++k)
        {
            const int zA = rightZs[k];
            const int zB = rightZs[k + 1u];
            verts.push_back(resolveVertex(r.x1, zA, regionCellNode(r, r.x1 - 1, zA)));
            edges.push_back({0, zA, zB});
        }
        // 顶边：z = z1, x1 -> x0 (外法线 +Z = 2)
        for (std::size_t k = 0; k + 1u < topXs.size(); ++k)
        {
            const int xA = topXs[k];
            const int xB = topXs[k + 1u];
            verts.push_back(resolveVertex(xA, r.z1, regionCellNode(r, xA - 1, r.z1 - 1)));
            edges.push_back({2, xB, xA});
        }
        // 左边：x = x0, z1 -> z0 (外法线 -X = 1)
        for (std::size_t k = 0; k + 1u < leftZs.size(); ++k)
        {
            const int zA = leftZs[k];
            const int zB = leftZs[k + 1u];
            verts.push_back(resolveVertex(r.x0, zA, regionCellNode(r, r.x0, zA - 1)));
            edges.push_back({1, zB, zA});
        }

        navPolygons[rIdx].neighbors.assign(verts.size(), -1);
        for (std::size_t e = 0; e < verts.size(); ++e)
        {
            const std::uint32_t a = verts[e];
            const std::uint32_t b = verts[(e + 1u) % verts.size()];
            halfEdgeOwner.emplace(EdgeKey{a, b}, rIdx);
        }
    }

    // 匹配反向半边且显式校验对应侧边网格存在真实 WalkNode 双向可达连通
    for (std::uint32_t rIdx = 0; rIdx < navPolygons.size(); ++rIdx)
    {
        const auto& r = regions[rIdx];
        auto& poly = navPolygons[rIdx];
        const auto& edges = polyEdges[rIdx];
        for (std::size_t e = 0; e < poly.vertices.size(); ++e)
        {
            const std::uint32_t a = poly.vertices[e];
            const std::uint32_t b = poly.vertices[(e + 1u) % poly.vertices.size()];
            const auto it = halfEdgeOwner.find(EdgeKey{b, a});
            if (it == halfEdgeOwner.end() || it->second == rIdx) continue;

            const PolyEdgeInfo& edgeInfo = edges[e];
            bool traversable = edgeInfo.spanEnd > edgeInfo.spanBegin;
            for (int k = edgeInfo.spanBegin; k < edgeInfo.spanEnd; ++k)
            {
                int cellNode = -1;
                if (edgeInfo.outDir == 3) cellNode = regionCellNode(r, k, r.z0);
                else if (edgeInfo.outDir == 2) cellNode = regionCellNode(r, k, r.z1 - 1);
                else if (edgeInfo.outDir == 0) cellNode = regionCellNode(r, r.x1 - 1, k);
                else if (edgeInfo.outDir == 1) cellNode = regionCellNode(r, r.x0, k);
                if (cellNode < 0)
                {
                    traversable = false;
                    break;
                }
                const std::int32_t nb = nodes[static_cast<std::size_t>(cellNode)].neighbor[edgeInfo.outDir];
                if (nb < 0 || !nodes[static_cast<std::size_t>(nb)].keep
                    || nodes[static_cast<std::size_t>(nb)].neighbor[edgeInfo.outDir ^ 1] != cellNode
                    || nodes[static_cast<std::size_t>(nb)].regionId != static_cast<int>(it->second))
                {
                    traversable = false;
                    break;
                }
            }
            if (traversable)
            {
                poly.neighbors[e] = static_cast<std::int32_t>(it->second);
            }
        }
    }

    output.SetData(settings, std::move(navVertices), std::move(navPolygons));
    if (!output.Valid())
    {
        errorKey = "Navigation.BakeFailed";
        return false;
    }
    return true;
}
} // namespace flare
