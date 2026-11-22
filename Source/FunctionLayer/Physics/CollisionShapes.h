#pragma once

#include "CoreLayer/Math/Bounds.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace flare
{
/// 刚体/碰撞体支持的几何形状种类（与 ColliderSettingsComponent 属性值一致）。
enum class ColliderShapeKind : std::uint8_t
{
    Box = 0,
    Sphere = 1
};

struct BoxShape
{
    Vec3 center{};
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    Quat rotation{};
};

struct SphereShape
{
    Vec3 center{};
    float radius = 0.5f;
};

struct ContactPoint
{
    Vec3 position{};
    float penetration = 0.0f;
    std::uint64_t feature = 0;
};

enum class ContactKind
{
    Face,
    Edge
};

struct ContactManifold
{
    Vec3 normal{};
    std::vector<ContactPoint> points;
    // 沿 normal 移动 B 至所选轴刚好分离的距离；逐点 penetration 用于求解。
    float separationDistance = 0.0f;
    ContactKind kind = ContactKind::Face;
};

/// 角色直立胶囊体与静态/动态障碍物的单点推离接触结果；normal 从障碍物指向胶囊。
struct CapsuleContact
{
    Vec3 normal{};
    Vec3 point{};
    float depth = 0.0f;
};

[[nodiscard]] bool ValidBoxShape(const BoxShape& shape) noexcept;
[[nodiscard]] bool ValidSphereShape(const SphereShape& shape) noexcept;

[[nodiscard]] Aabb ComputeAabb(const BoxShape& box);
[[nodiscard]] Aabb ComputeAabb(const SphereShape& sphere);

[[nodiscard]] std::optional<ContactManifold> CollideBoxes(const BoxShape& a, const BoxShape& b, float margin = 0.005f);
[[nodiscard]] std::optional<ContactManifold> CollideSpheres(const SphereShape& a, const SphereShape& b, float margin = 0.005f);
[[nodiscard]] std::optional<ContactManifold> CollideSphereBox(const SphereShape& sphere, const BoxShape& box, float margin = 0.005f);
[[nodiscard]] std::optional<ContactManifold> CollideBoxSphere(const BoxShape& box, const SphereShape& sphere, float margin = 0.005f);

[[nodiscard]] std::optional<CapsuleContact> CollideCapsuleBox(Vec3 center, float segmentHalfHeight, float radius, const BoxShape& box);
[[nodiscard]] std::optional<CapsuleContact> CollideCapsuleSphere(Vec3 center, float segmentHalfHeight, float radius, const SphereShape& sphere);
} // namespace flare
