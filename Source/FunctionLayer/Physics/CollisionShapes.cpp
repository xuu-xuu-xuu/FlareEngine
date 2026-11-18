#include "FunctionLayer/Physics/CollisionShapes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace se
{
namespace
{
constexpr float kMinimumShapeExtent = 0.001f;

float Component(Vec3 value, int axis) noexcept
{
    return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

Vec3 ClampToBox(Vec3 point, Vec3 halfExtents) noexcept
{
    return {
        std::clamp(point.x, -halfExtents.x, halfExtents.x),
        std::clamp(point.y, -halfExtents.y, halfExtents.y),
        std::clamp(point.z, -halfExtents.z, halfExtents.z)};
}

std::array<Vec3, 3> Axes(const BoxShape& box) noexcept
{
    return {
        Rotate(box.rotation, {1.0f, 0.0f, 0.0f}),
        Rotate(box.rotation, {0.0f, 1.0f, 0.0f}),
        Rotate(box.rotation, {0.0f, 0.0f, 1.0f})};
}

Vec3 Support(const BoxShape& box, Vec3 direction) noexcept
{
    const auto axes = Axes(box);
    Vec3 result = box.center;
    for (int i = 0; i < 3; ++i)
    {
        const float extent = Component(box.halfExtents, i);
        result += axes[i] * (Dot(axes[i], direction) >= 0.0f ? extent : -extent);
    }
    return result;
}

std::uint64_t Feature(std::uint64_t a, std::uint64_t b, std::uint64_t plane) noexcept
{
    if (a > b)
    {
        std::swap(a, b);
    }
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto value : {a, b, plane})
    {
        for (unsigned shift = 0; shift < 64; shift += 8)
        {
            hash ^= (value >> shift) & 255ull;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

struct ClipVertex
{
    Vec3 position{};
    std::uint64_t feature = 0;
};

std::vector<ClipVertex> Clip(
    const std::vector<ClipVertex>& polygon,
    Vec3 center,
    Vec3 normal,
    float distance,
    std::uint64_t plane)
{
    std::vector<ClipVertex> result;
    if (polygon.empty())
    {
        return result;
    }
    auto previous = polygon.back();
    float previousDistance = Dot(previous.position - center, normal) - distance;
    for (const auto current : polygon)
    {
        const float currentDistance = Dot(current.position - center, normal) - distance;
        if ((currentDistance <= 0.0f) != (previousDistance <= 0.0f))
        {
            result.push_back({
                previous.position + (current.position - previous.position) * (previousDistance / (previousDistance - currentDistance)),
                Feature(previous.feature, current.feature, plane)});
        }
        if (currentDistance <= 0.0f)
        {
            result.push_back(current);
        }
        previous = current;
        previousDistance = currentDistance;
    }
    return result;
}

std::pair<Vec3, Vec3> ClosestSegments(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2) noexcept
{
    const Vec3 d1 = q1 - p1;
    const Vec3 d2 = q2 - p2;
    const Vec3 r = p1 - p2;
    const float a = Dot(d1, d1);
    const float e = Dot(d2, d2);
    const float f = Dot(d2, r);
    float s = 0.0f;
    float t = 0.0f;
    if (a <= 1e-10f && e <= 1e-10f)
    {
        return {p1, p2};
    }
    if (a <= 1e-10f)
    {
        t = std::clamp(f / e, 0.0f, 1.0f);
    }
    else
    {
        const float c = Dot(d1, r);
        if (e <= 1e-10f)
        {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        }
        else
        {
            const float b = Dot(d1, d2);
            const float denominator = a * e - b * b;
            if (denominator > 1e-10f)
            {
                s = std::clamp((b * f - c * e) / denominator, 0.0f, 1.0f);
            }
            t = (b * s + f) / e;
            if (t < 0.0f)
            {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            }
            else if (t > 1.0f)
            {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return {p1 + d1 * s, p2 + d2 * t};
}

Vec3 AxisUnitVector(int axis, float sign) noexcept
{
    return axis == 0 ? Vec3{sign, 0.0f, 0.0f} : (axis == 1 ? Vec3{0.0f, sign, 0.0f} : Vec3{0.0f, 0.0f, sign});
}
} // namespace

bool ValidBoxShape(const BoxShape& shape) noexcept
{
    const float rotLenSq = LengthSquared(shape.rotation);
    return IsFinite(shape.center) && IsFinite(shape.halfExtents) && IsFinite(shape.rotation) &&
           IsFinite(rotLenSq) && rotLenSq > 1e-10f &&
           shape.halfExtents.x > kMinimumShapeExtent &&
           shape.halfExtents.y > kMinimumShapeExtent &&
           shape.halfExtents.z > kMinimumShapeExtent;
}

bool ValidSphereShape(const SphereShape& shape) noexcept
{
    return IsFinite(shape.center) && IsFinite(shape.radius) && shape.radius > kMinimumShapeExtent;
}

Aabb ComputeAabb(const BoxShape& box)
{
    const auto axes = Axes(box);
    Vec3 radius{};
    for (int i = 0; i < 3; ++i)
    {
        const float h = Component(box.halfExtents, i);
        radius += {std::abs(axes[i].x) * h, std::abs(axes[i].y) * h, std::abs(axes[i].z) * h};
    }
    return {box.center - radius, box.center + radius};
}

Aabb ComputeAabb(const SphereShape& sphere)
{
    const Vec3 extent{sphere.radius, sphere.radius, sphere.radius};
    return {sphere.center - extent, sphere.center + extent};
}

std::optional<ContactManifold> CollideBoxes(const BoxShape& a, const BoxShape& b, float margin)
{
    if (!ValidBoxShape(a) || !ValidBoxShape(b) || !IsFinite(margin) || margin < 0.0f)
    {
        return std::nullopt;
    }
    const auto ax = Axes(a);
    const auto bx = Axes(b);
    const Vec3 delta = b.center - a.center;
    float minimum = std::numeric_limits<float>::max();
    int selected = -1;
    Vec3 normal{};
    for (int axisIndex = 0; axisIndex < 15; ++axisIndex)
    {
        Vec3 axis = axisIndex < 3
            ? ax[axisIndex]
            : (axisIndex < 6 ? bx[axisIndex - 3] : Cross(ax[(axisIndex - 6) / 3], bx[(axisIndex - 6) % 3]));
        const float length = Length(axis);
        if (length < 1e-5f)
        {
            continue;
        }
        axis = axis / length;
        float ra = 0.0f;
        float rb = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            ra += Component(a.halfExtents, i) * std::abs(Dot(ax[i], axis));
            rb += Component(b.halfExtents, i) * std::abs(Dot(bx[i], axis));
        }
        const float penetration = ra + rb - std::abs(Dot(delta, axis));
        if (penetration < -margin)
        {
            return std::nullopt;
        }
        // 近似相等时保留面轴，提高连续接触的稳定性。
        if (selected < 0 || penetration < minimum - (axisIndex >= 6 ? 0.001f : 0.00001f))
        {
            minimum = penetration;
            selected = axisIndex;
            normal = Dot(delta, axis) >= 0.0f ? axis : -axis;
        }
    }
    ContactManifold result;
    result.normal = normal;
    result.separationDistance = std::max(minimum, 0.0f);
    if (selected < 6)
    {
        const bool referenceA = selected < 3;
        const auto& reference = referenceA ? a : b;
        const auto& incident = referenceA ? b : a;
        const auto referenceAxes = referenceA ? ax : bx;
        const auto incidentAxes = referenceA ? bx : ax;
        const int referenceAxis = selected % 3;
        const Vec3 referenceNormal = referenceA ? normal : -normal;
        const Vec3 center = reference.center + referenceNormal * Component(reference.halfExtents, referenceAxis);
        int incidentAxis = 0;
        for (int i = 1; i < 3; ++i)
        {
            if (std::abs(Dot(incidentAxes[i], referenceNormal)) > std::abs(Dot(incidentAxes[incidentAxis], referenceNormal)))
            {
                incidentAxis = i;
            }
        }
        const float sign = Dot(incidentAxes[incidentAxis], referenceNormal) > 0.0f ? -1.0f : 1.0f;
        const Vec3 incidentCenter = incident.center + incidentAxes[incidentAxis] * (sign * Component(incident.halfExtents, incidentAxis));
        const int u = (incidentAxis + 1) % 3;
        const int v = (incidentAxis + 2) % 3;
        const Vec3 sideU = incidentAxes[u] * Component(incident.halfExtents, u);
        const Vec3 sideV = incidentAxes[v] * Component(incident.halfExtents, v);
        const auto faceFeature = Feature(
            static_cast<std::uint64_t>(selected * 2 + (Dot(referenceNormal, referenceAxes[referenceAxis]) > 0.0f)),
            static_cast<std::uint64_t>(16 + incidentAxis * 2 + (sign > 0.0f)),
            32);
        std::vector<ClipVertex> polygon{
            {incidentCenter - sideU - sideV, Feature(faceFeature, 0, 64)},
            {incidentCenter + sideU - sideV, Feature(faceFeature, 1, 64)},
            {incidentCenter + sideU + sideV, Feature(faceFeature, 2, 64)},
            {incidentCenter - sideU + sideV, Feature(faceFeature, 3, 64)}};
        for (int i = 0; i < 3; ++i)
        {
            if (i != referenceAxis)
            {
                polygon = Clip(polygon, center, referenceAxes[i], Component(reference.halfExtents, i), static_cast<std::uint64_t>(i * 2));
                polygon = Clip(polygon, center, -referenceAxes[i], Component(reference.halfExtents, i), static_cast<std::uint64_t>(i * 2 + 1));
            }
        }
        std::vector<ContactPoint> points;
        for (const auto& vertex : polygon)
        {
            const float depth = Dot(center - vertex.position, referenceNormal);
            if (depth >= -margin)
            {
                points.push_back({vertex.position + referenceNormal * (depth * 0.5f), std::max(depth, 0.0f), vertex.feature});
            }
        }
        if (points.size() <= 4)
        {
            result.points = std::move(points);
        }
        else
        {
            for (std::size_t i = 0; i < 4; ++i)
            {
                result.points.push_back(points[i * points.size() / 4]);
            }
        }
    }
    else
    {
        result.kind = ContactKind::Edge;
        const int ai = (selected - 6) / 3;
        const int bi = (selected - 6) % 3;
        Vec3 ac = a.center;
        Vec3 bc = b.center;
        std::uint64_t sideBits = 0;
        for (int i = 0; i < 3; ++i)
        {
            if (i != ai && Dot(ax[i], normal) >= 0.0f)
            {
                sideBits |= std::uint64_t{1} << i;
            }
            if (i != bi && Dot(bx[i], normal) >= 0.0f)
            {
                sideBits |= std::uint64_t{1} << (i + 3);
            }
            if (i != ai)
            {
                ac += ax[i] * (Dot(ax[i], normal) >= 0.0f ? Component(a.halfExtents, i) : -Component(a.halfExtents, i));
            }
            if (i != bi)
            {
                bc += bx[i] * (Dot(bx[i], normal) >= 0.0f ? -Component(b.halfExtents, i) : Component(b.halfExtents, i));
            }
        }
        const auto points = ClosestSegments(
            ac - ax[ai] * Component(a.halfExtents, ai),
            ac + ax[ai] * Component(a.halfExtents, ai),
            bc - bx[bi] * Component(b.halfExtents, bi),
            bc + bx[bi] * Component(b.halfExtents, bi));
        result.points.push_back({(points.first + points.second) * 0.5f, std::max(minimum, 0.0f), Feature(static_cast<std::uint64_t>(selected), sideBits, 128)});
    }
    if (result.points.empty())
    {
        result.points.push_back({(Support(a, normal) + Support(b, -normal)) * 0.5f, std::max(minimum, 0.0f), 0});
    }
    return result;
}

std::optional<ContactManifold> CollideSpheres(const SphereShape& a, const SphereShape& b, float margin)
{
    if (!ValidSphereShape(a) || !ValidSphereShape(b) || !IsFinite(margin) || margin < 0.0f)
    {
        return std::nullopt;
    }
    const Vec3 delta = b.center - a.center;
    const float distSq = LengthSquared(delta);
    const float sumRadii = a.radius + b.radius;
    const float limit = sumRadii + margin;
    if (distSq > limit * limit)
    {
        return std::nullopt;
    }

    const float dist = std::sqrt(distSq);
    const Vec3 normal = dist > 1e-6f ? (delta / dist) : Vec3{0.0f, 1.0f, 0.0f};
    const float penetration = sumRadii - dist;
    const float clampedDepth = std::max(penetration, 0.0f);
    // Contact midpoint between sphere A surface and sphere B surface along A->B normal.
    const Vec3 surfaceA = a.center + normal * a.radius;
    const Vec3 surfaceB = b.center - normal * b.radius;

    ContactManifold result;
    result.normal = normal;
    result.separationDistance = clampedDepth;
    result.kind = ContactKind::Face;
    result.points.push_back({(surfaceA + surfaceB) * 0.5f, clampedDepth, Feature(200, 1, 1)});
    return result;
}

std::optional<ContactManifold> CollideSphereBox(const SphereShape& sphere, const BoxShape& box, float margin)
{
    if (!ValidSphereShape(sphere) || !ValidBoxShape(box) || !IsFinite(margin) || margin < 0.0f)
    {
        return std::nullopt;
    }
    const Quat normalizedBoxRot = Normalize(box.rotation);
    const Quat inverseRot = Conjugate(normalizedBoxRot);
    const Vec3 localSphere = Rotate(inverseRot, sphere.center - box.center);
    const Vec3 localClosest = ClampToBox(localSphere, box.halfExtents);
    const Vec3 diff = localClosest - localSphere;
    const float distSq = LengthSquared(diff);

    ContactManifold result;
    result.kind = ContactKind::Face;

    if (distSq > 1e-12f)
    {
        const float dist = std::sqrt(distSq);
        const float penetration = sphere.radius - dist;
        if (penetration < -margin)
        {
            return std::nullopt;
        }
        const Vec3 localNormal = diff / dist; // From sphere center toward box surface (A -> B).
        const Vec3 worldNormal = Normalize(Rotate(normalizedBoxRot, localNormal));
        const Vec3 worldClosest = box.center + Rotate(normalizedBoxRot, localClosest);
        const Vec3 sphereSurface = sphere.center + worldNormal * sphere.radius;
        const float clampedDepth = std::max(penetration, 0.0f);

        result.normal = worldNormal;
        result.separationDistance = clampedDepth;
        result.points.push_back({(worldClosest + sphereSurface) * 0.5f, clampedDepth, Feature(300, 0, 1)});
        return result;
    }

    // Sphere center lies inside the box: choose the nearest exit face for A->B normal.
    float minFaceDist = std::numeric_limits<float>::max();
    Vec3 localExitNormal{0.0f, 1.0f, 0.0f};
    std::uint64_t faceId = 0;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float coord = Component(localSphere, axis);
        const float extent = Component(box.halfExtents, axis);
        const float distToFace = std::max(0.0f, extent - std::abs(coord));
        if (distToFace < minFaceDist)
        {
            minFaceDist = distToFace;
            const float sign = coord >= 0.0f ? 1.0f : -1.0f;
            localExitNormal = AxisUnitVector(axis, sign);
            faceId = static_cast<std::uint64_t>(axis * 2 + (sign > 0.0f ? 1 : 0));
        }
    }

    const Vec3 worldExit = Normalize(Rotate(normalizedBoxRot, localExitNormal));
    // Exit vector moves A out of B, so A->B normal is the opposite direction.
    const Vec3 worldNormal = -worldExit;
    const float penetration = sphere.radius + minFaceDist;
    const Vec3 worldFacePoint = box.center + Rotate(normalizedBoxRot, localSphere + localExitNormal * minFaceDist);
    const Vec3 sphereSurface = sphere.center + worldNormal * sphere.radius;

    result.normal = worldNormal;
    result.separationDistance = penetration;
    result.points.push_back({(worldFacePoint + sphereSurface) * 0.5f, penetration, Feature(301, faceId, 2)});
    return result;
}

std::optional<ContactManifold> CollideBoxSphere(const BoxShape& box, const SphereShape& sphere, float margin)
{
    auto manifold = CollideSphereBox(sphere, box, margin);
    if (!manifold)
    {
        return std::nullopt;
    }
    manifold->normal = -manifold->normal;
    return manifold;
}

std::optional<CapsuleContact> CollideCapsuleBox(Vec3 center, float segmentHalfHeight, float radius, const BoxShape& box)
{
    if (!IsFinite(center) || !IsFinite(segmentHalfHeight) || segmentHalfHeight < 0.0f ||
        !IsFinite(radius) || radius <= 0.0f || !ValidBoxShape(box))
    {
        return std::nullopt;
    }
    const Quat normalizedBoxRot = Normalize(box.rotation);
    const Quat inverse = Conjugate(normalizedBoxRot);
    const Vec3 a = Rotate(inverse, center - Vec3{0.0f, segmentHalfHeight, 0.0f} - box.center);
    const Vec3 b = Rotate(inverse, center + Vec3{0.0f, segmentHalfHeight, 0.0f} - box.center);
    const Vec3 d = b - a;
    const Vec3 h = box.halfExtents;

    // 点到 AABB 的距离在各坐标穿过面的位置处分段；每段为一元二次函数。
    std::vector<float> cuts{0.0f, 1.0f};
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(Component(d, axis)) > 1e-8f)
        {
            for (float sign : {-1.0f, 1.0f})
            {
                const float t = (sign * Component(h, axis) - Component(a, axis)) / Component(d, axis);
                if (t > 0.0f && t < 1.0f)
                {
                    cuts.push_back(t);
                }
            }
        }
    }
    std::sort(cuts.begin(), cuts.end());

    float best = std::numeric_limits<float>::max();
    Vec3 segmentPoint{};
    Vec3 boxPoint{};
    const auto sample = [&](float t)
    {
        const Vec3 p = a + d * t;
        const Vec3 q = ClampToBox(p, h);
        const float distance = LengthSquared(p - q);
        if (distance < best)
        {
            best = distance;
            segmentPoint = p;
            boxPoint = q;
        }
    };
    for (std::size_t i = 1; i < cuts.size(); ++i)
    {
        const float lo = cuts[i - 1];
        const float hi = cuts[i];
        const float mid = (lo + hi) * 0.5f;
        float numerator = 0.0f;
        float denominator = 0.0f;
        sample(lo);
        sample(hi);
        for (int axis = 0; axis < 3; ++axis)
        {
            const float value = Component(a + d * mid, axis);
            const float extent = Component(h, axis);
            if (value >= -extent && value <= extent)
            {
                continue;
            }
            const float start = Component(a, axis) - (value < 0.0f ? -extent : extent);
            const float slope = Component(d, axis);
            numerator += start * slope;
            denominator += slope * slope;
        }
        if (denominator > 1e-10f)
        {
            sample(std::clamp(-numerator / denominator, lo, hi));
        }
    }

    const float distance = std::sqrt(best);
    if (distance > radius + 0.001f)
    {
        return std::nullopt;
    }
    Vec3 normal{};
    float depth = radius - distance;
    if (distance > 1e-6f)
    {
        normal = (segmentPoint - boxPoint) / distance;
    }
    else
    {
        // 出生于箱内也有确定的退出方向；按整个胶囊线段的投影计算平移。
        depth = std::numeric_limits<float>::max();
        for (int axis = 0; axis < 3; ++axis)
        {
            for (float sign : {-1.0f, 1.0f})
            {
                const float low = std::min(Component(a, axis), Component(b, axis));
                const float high = std::max(Component(a, axis), Component(b, axis));
                const float candidate = Component(h, axis) + (sign > 0.0f ? -low : high) + radius;
                if (candidate < depth)
                {
                    depth = candidate;
                    normal = AxisUnitVector(axis, sign);
                }
            }
        }
    }
    return CapsuleContact{
        Rotate(normalizedBoxRot, normal),
        box.center + Rotate(normalizedBoxRot, boxPoint),
        depth};
}

std::optional<CapsuleContact> CollideCapsuleSphere(Vec3 center, float segmentHalfHeight, float radius, const SphereShape& sphere)
{
    if (!IsFinite(center) || !IsFinite(segmentHalfHeight) || segmentHalfHeight < 0.0f ||
        !IsFinite(radius) || radius <= 0.0f || !ValidSphereShape(sphere))
    {
        return std::nullopt;
    }
    const float clampedY = std::clamp(sphere.center.y, center.y - segmentHalfHeight, center.y + segmentHalfHeight);
    const Vec3 segmentPoint{center.x, clampedY, center.z};
    const Vec3 diff = segmentPoint - sphere.center;
    const float distSq = LengthSquared(diff);
    const float sumRadii = radius + sphere.radius;
    const float limit = sumRadii + 0.001f;
    if (distSq > limit * limit)
    {
        return std::nullopt;
    }

    const float dist = std::sqrt(distSq);
    const Vec3 normal = dist > 1e-6f ? (diff / dist) : Vec3{0.0f, 1.0f, 0.0f};
    const float depth = sumRadii - dist;
    const Vec3 contactPoint = sphere.center + normal * sphere.radius;
    return CapsuleContact{normal, contactPoint, depth};
}
} // namespace se
