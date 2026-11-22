#pragma once
#include "FunctionLayer/Navigation/NavMeshData.h"
#include "FunctionLayer/Navigation/NavPathfinder.h"
#include "FunctionLayer/Physics/ScenePhysics.h"
#include "FunctionLayer/Runtime/World.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace flare
{
/// 从静态世界实体（网格及静态非触发碰撞体）提取世界空间三角网供导航烘焙使用。
void CollectNavigationSceneGeometry(
    const World& world,
    AssetStore* assets,
    std::vector<Vec3>& outVertices,
    std::vector<std::uint32_t>& outIndices);

/// 校验场景内 NavigationAgentComponent 的参数合法性（用于 Play 前校验）。
bool ValidateSceneNavigation(const World& world, std::string& reasonKey);

/// 场景导航协调器：缓存 .senav 资产、维护每个代理的运行时寻路状态并驱动底层角色移动或变换推进。
class SceneNavigation
{
public:
    void Clear() noexcept;
    void RemoveAgent(StableEntityId entityId) noexcept;
    void SetOverrideNavMesh(std::shared_ptr<const NavMeshData> navMesh) noexcept;

    [[nodiscard]] std::shared_ptr<const NavMeshData> ResolveNavMesh(
        AssetStore* assets, const AssetId& assetId, std::string& errorKey);

    bool FindPath(
        AssetStore* assets,
        const AssetId& navAssetId,
        Vec3 start,
        Vec3 goal,
        NavPathResult& outResult,
        std::string& errorKey);

    /// 为实体设置运行时导航目标（不直接写只读 World 组件，可在脚本回调与挂起命令期间调用）。
    bool SetDestination(const World& world, AssetStore* assets, StableEntityId entityId, Vec3 target, std::string& errorKey);
    bool StopAgent(const World& world, StableEntityId entityId, std::string& errorKey);

    /// 推进所有挂载了活跃 NavigationAgentComponent 的代理（优先复用 ScenePhysics::MoveCharacter）。
    void StepAgents(World& world, PhysicsWorld& physics, ScenePhysics& scenePhysics, AssetStore* assets, float deltaSeconds);

    [[nodiscard]] float RemainingDistanceMeters(const World& world, StableEntityId entityId) const noexcept;
    [[nodiscard]] NavPathStatus AgentStatus(StableEntityId entityId) const noexcept;
    [[nodiscard]] std::size_t TrackedAgentCount() const noexcept { return agents_.size(); }

private:
    struct AgentConfig
    {
        AssetId asset;
        float speed = 3.5f;
        float stoppingDistance = 0.2f;
        bool autoRepath = true;
        bool authoredHasTarget = false;
        Vec3 authoredTarget{};
    };

    struct AgentRuntime
    {
        AgentConfig authored{};
        bool runtimeHasOverride = false;
        bool activeTarget = false;
        Vec3 target{};
        Vec3 plannedGoal{};
        AssetId plannedAsset;
        bool pathPlanned = false;
        bool arrived = false;
        NavPathStatus status = NavPathStatus::Unreachable;
        std::vector<Vec3> waypoints;
        std::size_t waypointIndex = 0u;
    };

    struct CachedNavMesh
    {
        std::string contentHash;
        std::shared_ptr<const NavMeshData> data;
    };

    static bool TryReadAgentConfig(const World& world, EntityHandle entity, AgentConfig& outConfig) noexcept;
    AgentRuntime* EnsureAgent(const World& world, EntityHandle entity, StableEntityId stableId);
    void PlanAgentPath(AgentRuntime& runtime, Vec3 startPos, AssetStore* assets);

    std::shared_ptr<const NavMeshData> overrideNavMesh_;
    std::map<AssetId, CachedNavMesh> cache_;
    std::map<StableEntityId, AgentRuntime> agents_;
};
} // namespace flare
