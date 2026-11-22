#include "NavPathfinder.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace flare
{
namespace
{
struct PortalSegment
{
    Vec3 right{};
    Vec3 left{};
};

float DistSqXZ(Vec3 a, Vec3 b) noexcept
{
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}

std::vector<Vec3> StringPullFunnel(Vec3 start, Vec3 end, const std::vector<PortalSegment>& portals)
{
    std::vector<Vec3> path;
    path.push_back(start);

    if (portals.empty())
    {
        if (DistSqXZ(start, end) > 1e-6f || std::abs(start.y - end.y) > 1e-4f) path.push_back(end);
        return path;
    }

    std::vector<PortalSegment> chain = portals;
    chain.push_back({end, end});

    Vec3 apex = start;
    Vec3 left = start;
    Vec3 right = start;
    std::size_t apexIndex = 0;
    std::size_t leftIndex = 0;
    std::size_t rightIndex = 0;

    auto pushPoint = [&](Vec3 pt) {
        if (path.empty() || LengthSquared(path.back() - pt) > 1e-6f) path.push_back(pt);
    };

    std::size_t i = 0;
    while (i < chain.size())
    {
        const Vec3 newRight = chain[i].right;
        const Vec3 newLeft = chain[i].left;

        // 尝试收紧右边界
        if (NavTriArea2XZ(apex, right, newRight) >= -1e-5f)
        {
            if (DistSqXZ(apex, right) <= 1e-8f || NavTriArea2XZ(apex, left, newRight) <= 1e-5f)
            {
                right = newRight;
                rightIndex = i;
            }
            else
            {
                // 新右边界越过左边界：固定 left 为新 apex 并从其后继门重启
                pushPoint(left);
                apex = left;
                apexIndex = leftIndex;
                left = apex;
                right = apex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                i = apexIndex + 1u;
                continue;
            }
        }

        // 尝试收紧左边界
        if (NavTriArea2XZ(apex, left, newLeft) <= 1e-5f)
        {
            if (DistSqXZ(apex, left) <= 1e-8f || NavTriArea2XZ(apex, right, newLeft) >= -1e-5f)
            {
                left = newLeft;
                leftIndex = i;
            }
            else
            {
                // 新左边界越过右边界：固定 right 为新 apex 并从其后继门重启
                pushPoint(right);
                apex = right;
                apexIndex = rightIndex;
                left = apex;
                right = apex;
                leftIndex = apexIndex;
                rightIndex = apexIndex;
                i = apexIndex + 1u;
                continue;
            }
        }
        ++i;
    }

    pushPoint(end);
    return path;
}
} // namespace

NavPathResult FindNavPath(const NavMeshData& navMesh, Vec3 start, Vec3 goal, Vec3 queryHalfExtents)
{
    NavPathResult result;
    if (!navMesh.Valid() || !IsFinite(start) || !IsFinite(goal)) return result;

    Vec3 startProj{};
    const auto startPolyOpt = navMesh.FindNearestPolygon(start, queryHalfExtents, &startProj);
    if (!startPolyOpt) return result;
    const std::uint32_t startPoly = *startPolyOpt;

    Vec3 goalProj{};
    const auto goalPolyOpt = navMesh.FindNearestPolygon(goal, queryHalfExtents, &goalProj);
    if (goalPolyOpt && *goalPolyOpt == startPoly)
    {
        result.status = NavPathStatus::Complete;
        result.waypoints.push_back(startProj);
        if (LengthSquared(goalProj - startProj) > 1e-6f) result.waypoints.push_back(goalProj);
        result.totalLengthMeters = Length(goalProj - startProj);
        return result;
    }

    const auto& polygons = navMesh.Polygons();
    const auto& vertices = navMesh.Vertices();
    const std::size_t n = polygons.size();

    constexpr float Inf = (std::numeric_limits<float>::max)();
    std::vector<float> gScore(n, Inf);
    std::vector<std::int32_t> parentPoly(n, -1);
    std::vector<std::int32_t> parentEdge(n, -1);
    std::vector<std::uint8_t> closed(n, 0u);

    struct QueueEntry
    {
        float f = 0.0f;
        std::uint32_t poly = 0u;
        bool operator>(const QueueEntry& other) const noexcept
        {
            return f != other.f ? f > other.f : poly > other.poly;
        }
    };
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> open;

    const Vec3 heuristicTarget = goalPolyOpt ? goalProj : goal;
    gScore[startPoly] = 0.0f;
    open.push({Length(heuristicTarget - startProj), startPoly});

    std::uint32_t bestReachable = startPoly;
    float bestGoalDistSq = LengthSquared(polygons[startPoly].center - goal);
    bool reachedGoal = false;

    while (!open.empty())
    {
        const std::uint32_t cur = open.top().poly;
        open.pop();
        if (closed[cur]) continue;
        closed[cur] = 1u;

        if (goalPolyOpt && cur == *goalPolyOpt)
        {
            reachedGoal = true;
            break;
        }

        const float dGoalSq = LengthSquared(polygons[cur].center - goal);
        if (dGoalSq < bestGoalDistSq)
        {
            bestGoalDistSq = dGoalSq;
            bestReachable = cur;
        }

        const auto& poly = polygons[cur];
        for (std::size_t e = 0; e < poly.neighbors.size(); ++e)
        {
            const std::int32_t nbSigned = poly.neighbors[e];
            if (nbSigned < 0) continue;
            const std::uint32_t nb = static_cast<std::uint32_t>(nbSigned);
            if (closed[nb]) continue;

            const Vec3 curAnchor = cur == startPoly ? startProj : poly.center;
            const Vec3 nbAnchor = (goalPolyOpt && nb == *goalPolyOpt) ? goalProj : polygons[nb].center;
            const float stepCost = (std::max)(1e-4f, Length(nbAnchor - curAnchor));
            const float tentative = gScore[cur] + stepCost;
            if (tentative < gScore[nb])
            {
                gScore[nb] = tentative;
                parentPoly[nb] = static_cast<std::int32_t>(cur);
                parentEdge[nb] = static_cast<std::int32_t>(e);
                const float h = goalPolyOpt ? Length(heuristicTarget - nbAnchor) : 0.0f;
                open.push({tentative + h, nb});
            }
        }
    }

    const std::uint32_t endPoly = reachedGoal ? *goalPolyOpt : bestReachable;
    const Vec3 targetProj = reachedGoal ? goalProj : navMesh.ProjectPointToPolygon(endPoly, goal);
    result.status = reachedGoal ? NavPathStatus::Complete : NavPathStatus::Partial;

    std::vector<std::uint32_t> corridor;
    std::vector<std::int32_t> corridorEdges;
    for (std::int32_t cur = static_cast<std::int32_t>(endPoly); cur != -1;)
    {
        corridor.push_back(static_cast<std::uint32_t>(cur));
        corridorEdges.push_back(parentEdge[static_cast<std::size_t>(cur)]);
        cur = parentPoly[static_cast<std::size_t>(cur)];
    }
    std::reverse(corridor.begin(), corridor.end());
    std::reverse(corridorEdges.begin(), corridorEdges.end());

    std::vector<PortalSegment> portals;
    if (corridor.size() > 1u)
    {
        portals.reserve(corridor.size() - 1u);
        for (std::size_t i = 0; i + 1u < corridor.size(); ++i)
        {
            const auto& poly = polygons[corridor[i]];
            const std::size_t edgeIdx = static_cast<std::size_t>(corridorEdges[i + 1u]);
            const std::uint32_t va = poly.vertices[edgeIdx];
            const std::uint32_t vb = poly.vertices[(edgeIdx + 1u) % poly.vertices.size()];
            portals.push_back({vertices[va], vertices[vb]});
        }
    }

    result.waypoints = StringPullFunnel(startProj, targetProj, portals);
    float totalLen = 0.0f;
    for (std::size_t i = 1u; i < result.waypoints.size(); ++i)
    {
        totalLen += Length(result.waypoints[i] - result.waypoints[i - 1u]);
    }
    result.totalLengthMeters = totalLen;
    return result;
}
} // namespace flare
