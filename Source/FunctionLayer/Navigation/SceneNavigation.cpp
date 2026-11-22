#include "SceneNavigation.h"
#include "CoreLayer/Math/TransformDecomposition.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/StaticModel.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace flare
{
namespace
{
void AppendTransformedMesh(
    const MeshData& mesh,
    const Mat4& world,
    std::vector<Vec3>& outVertices,
    std::vector<std::uint32_t>& outIndices)
{
    if (mesh.vertices.empty() || mesh.indices.size() < 3u) return;
    const std::uint32_t baseVertex = static_cast<std::uint32_t>(outVertices.size());
    outVertices.reserve(outVertices.size() + mesh.vertices.size());
    for (const auto& v : mesh.vertices)
    {
        outVertices.push_back(TransformPoint(world, {v.position[0], v.position[1], v.position[2]}));
    }
    outIndices.reserve(outIndices.size() + mesh.indices.size());
    for (const auto idx : mesh.indices)
    {
        outIndices.push_back(baseVertex + idx);
    }
}

Vec3 ReadEntityWorldPosition(const World& world, EntityHandle entity) noexcept
{
    const Mat4 m = world.GetWorldMatrix(entity);
    return {m(0, 3), m(1, 3), m(2, 3)};
}

float DistXZ(Vec3 a, Vec3 b) noexcept
{
    return std::hypot(a.x - b.x, a.z - b.z);
}

bool ApplyWorldPosition(World& world, EntityHandle entity, Vec3 worldPosition)
{
    const auto* local = world.GetTransform(entity);
    const auto* record = world.Get(entity);
    if (!local || !record) return false;
    Transform desired = *local;
    if (record->parent)
    {
        Transform worldTrs{};
        if (!TryDecomposeTrs(world.GetWorldMatrix(entity), worldTrs)) return false;
        worldTrs.position = worldPosition;
        const auto invParent = Inverse(world.GetWorldMatrix(record->parent));
        if (!invParent) return false;
        if (!TryDecomposeTrs(*invParent * worldTrs.Matrix(), desired)) return false;
    }
    else
    {
        desired.position = worldPosition;
    }
    return world.SetTransform(entity, desired);
}
} // namespace

void CollectNavigationSceneGeometry(
    const World& world,
    AssetStore* assets,
    std::vector<Vec3>& outVertices,
    std::vector<std::uint32_t>& outIndices)
{
    outVertices.clear();
    outIndices.clear();
    const MeshData unitCube = MakeUnitCube();
    const MeshData unitSphere = MakeUnitSphere(16u, 8u);
    std::map<AssetId, std::shared_ptr<const StaticModel>> modelCache;

    for (const EntityHandle entity : world.Entities())
    {
        if (!world.IsEnabled(entity)) continue;
        const EntityRecord* rec = world.Get(entity);
        if (rec && (rec->staticFlags & EntityStaticFlags::Explicit) != 0)
        {
            if ((rec->staticFlags & EntityStaticFlags::Navigation) == 0) continue;
        }
        else
        {
            // 排除动态角色与导航代理本身，仅将静态环境几何体与静态实心碰撞体计入烘焙源。
            if (world.FindComponent(entity, NavigationAgentComponent::TypeId)
                || world.FindComponent(entity, CharacterSettingsComponent::TypeId))
            {
                continue;
            }
            if (const auto bodyComp = world.FindComponent(entity, PhysicsBodyComponent::TypeId))
            {
                if (world.GetComponent(bodyComp)->Active())
                {
                    const float mass = std::get<float>(FeatureValue(world, entity, "body.mass"));
                    if (mass > 0.0f) continue;
                }
            }
        }

        const Mat4 entityWorld = world.GetWorldMatrix(entity);
        bool addedMesh = false;
        if (const auto* mesh = world.GetMesh(entity))
        {
            const auto meshComp = world.FindComponent(entity, ComponentTypes::Mesh);
            if (!meshComp || world.GetComponent(meshComp)->Active())
            {
                if (mesh->meshAsset == BuiltinCubeAssetId)
                {
                    AppendTransformedMesh(unitCube, entityWorld, outVertices, outIndices);
                    addedMesh = true;
                }
                else if (mesh->meshAsset == BuiltinSphereAssetId)
                {
                    AppendTransformedMesh(unitSphere, entityWorld, outVertices, outIndices);
                    addedMesh = true;
                }
                else if (assets && !mesh->meshAsset.empty())
                {
                    std::shared_ptr<const StaticModel> model;
                    if (const auto it = modelCache.find(mesh->meshAsset); it != modelCache.end())
                    {
                        model = it->second;
                    }
                    else
                    {
                        std::vector<AssetDiagnostic> diagnostics;
                        model = assets->LoadModel(mesh->meshAsset, diagnostics);
                        modelCache.emplace(mesh->meshAsset, model);
                    }
                    if (model)
                    {
                        std::vector<Mat4> nodeGlobals(model->nodes.size(), Mat4::Identity());
                        for (std::size_t ni = 0; ni < model->nodes.size(); ++ni)
                        {
                            const auto& node = model->nodes[ni];
                            Mat4 local{};
                            local.values = node.localMatrix;
                            nodeGlobals[ni] = node.parent >= 0 && static_cast<std::size_t>(node.parent) < ni
                                ? nodeGlobals[static_cast<std::size_t>(node.parent)] * local
                                : local;
                        }
                        for (const auto& subMesh : model->meshes)
                        {
                            const Mat4 meshWorld = subMesh.meshNode < nodeGlobals.size()
                                ? entityWorld * nodeGlobals[subMesh.meshNode]
                                : entityWorld;
                            AppendTransformedMesh(subMesh, meshWorld, outVertices, outIndices);
                        }
                        addedMesh = true;
                    }
                }
            }
        }

        // 若静态实心盒碰撞体没有可见 Mesh，则同样将其体积纳入静态可行走/阻挡体烘焙
        if (!addedMesh)
        {
            const auto bodyHandle = world.FindComponent(entity, PhysicsBodyComponent::TypeId);
            const auto colHandle = world.FindComponent(entity, ColliderSettingsComponent::TypeId);
            const bool activeBody = bodyHandle && world.GetComponent(bodyHandle)->Active();
            const bool activeCol = colHandle && world.GetComponent(colHandle)->Active();
            if (activeBody || activeCol)
            {
                const bool isTrigger = activeCol && std::get<bool>(FeatureValue(world, entity, "collider.isTrigger"));
                if (!isTrigger)
                {
                    Vec3 halfExtents{0.5f, 0.5f, 0.5f};
                    if (activeCol)
                    {
                        const int shapeCode = static_cast<int>(std::lround(std::get<float>(FeatureValue(world, entity, "collider.shape"))));
                        if (shapeCode == 1)
                        {
                            const float radius = std::max(0.01f, std::get<float>(FeatureValue(world, entity, "collider.radiusMeters")));
                            AppendTransformedMesh(unitSphere, entityWorld * Scaling({radius * 2.0f, radius * 2.0f, radius * 2.0f}), outVertices, outIndices);
                            continue;
                        }
                        halfExtents = std::get<Vec3>(FeatureValue(world, entity, "collider.halfExtentsMeters"));
                    }
                    else
                    {
                        halfExtents = std::get<Vec3>(FeatureValue(world, entity, "body.halfExtents"));
                    }
                    AppendTransformedMesh(unitCube, entityWorld * Scaling(halfExtents * 2.0f), outVertices, outIndices);
                }
            }
        }
    }
}

bool ValidateSceneNavigation(const World& world, std::string& reasonKey)
{
    reasonKey.clear();
    for (const auto entity : world.Entities())
    {
        if (!world.IsEnabled(entity)) continue;
        const auto handle = world.FindComponent(entity, NavigationAgentComponent::TypeId);
        const auto* component = world.GetComponent(handle);
        if (!component || !component->Active()) continue;
        const float speed = std::get<float>(FeatureValue(world, entity, "navagent.speed"));
        const float stop = std::get<float>(FeatureValue(world, entity, "navagent.stoppingDistance"));
        const Vec3 target = std::get<Vec3>(FeatureValue(world, entity, "navagent.target"));
        if (!IsFinite(speed) || speed < 0.0f || !IsFinite(stop) || stop <= 0.0f || !IsFinite(target))
        {
            reasonKey = "Navigation.InvalidAgentConfig";
            return false;
        }
    }
    return true;
}

void SceneNavigation::Clear() noexcept
{
    agents_.clear();
    cache_.clear();
}

void SceneNavigation::RemoveAgent(StableEntityId entityId) noexcept
{
    agents_.erase(entityId);
}

void SceneNavigation::SetOverrideNavMesh(std::shared_ptr<const NavMeshData> navMesh) noexcept
{
    overrideNavMesh_ = std::move(navMesh);
}

std::shared_ptr<const NavMeshData> SceneNavigation::ResolveNavMesh(
    AssetStore* assets, const AssetId& assetId, std::string& errorKey)
{
    errorKey.clear();
    if (assetId.empty())
    {
        if (overrideNavMesh_ && overrideNavMesh_->Valid()) return overrideNavMesh_;
        errorKey = "Navigation.NavMeshNotAssigned";
        return nullptr;
    }

    if (!assets)
    {
        if (const auto it = cache_.find(assetId); it != cache_.end()) return it->second.data;
        errorKey = "Bridge.AssetsNotConfigured";
        return nullptr;
    }

    const auto path = assets->Resolve(assetId);
    if (path.empty())
    {
        cache_.erase(assetId);
        errorKey = "Assets.UnknownId";
        return nullptr;
    }

    std::vector<std::uint8_t> bytes;
    if (!asset_io::Read(path, bytes))
    {
        cache_.erase(assetId);
        errorKey = "Assets.ReadFailed";
        return nullptr;
    }

    const std::string contentHash = asset_io::Hash(bytes);
    if (const auto it = cache_.find(assetId); it != cache_.end() && it->second.contentHash == contentHash)
    {
        return it->second.data;
    }

    auto data = std::make_shared<NavMeshData>();
    if (!NavMeshData::Deserialize(bytes, *data, errorKey))
    {
        cache_.erase(assetId);
        return nullptr;
    }

    cache_[assetId] = CachedNavMesh{contentHash, data};
    return data;
}

bool SceneNavigation::FindPath(
    AssetStore* assets,
    const AssetId& navAssetId,
    Vec3 start,
    Vec3 goal,
    NavPathResult& outResult,
    std::string& errorKey)
{
    outResult = {};
    const auto navMesh = ResolveNavMesh(assets, navAssetId, errorKey);
    if (!navMesh) return false;
    outResult = FindNavPath(*navMesh, start, goal);
    return true;
}

bool SceneNavigation::TryReadAgentConfig(const World& world, EntityHandle entity, AgentConfig& outConfig) noexcept
{
    if (!world.IsEnabled(entity)) return false;
    const auto* component = world.GetComponent(world.FindComponent(entity, NavigationAgentComponent::TypeId));
    if (!component || !component->Active()) return false;

    outConfig.asset = std::get<std::string>(FeatureValue(world, entity, "navagent.asset"));
    outConfig.speed = std::max(0.0f, std::get<float>(FeatureValue(world, entity, "navagent.speed")));
    outConfig.stoppingDistance = std::max(0.01f, std::get<float>(FeatureValue(world, entity, "navagent.stoppingDistance")));
    outConfig.autoRepath = std::get<bool>(FeatureValue(world, entity, "navagent.autoRepath"));
    outConfig.authoredHasTarget = std::get<bool>(FeatureValue(world, entity, "navagent.hasTarget"));
    outConfig.authoredTarget = std::get<Vec3>(FeatureValue(world, entity, "navagent.target"));
    return true;
}

SceneNavigation::AgentRuntime* SceneNavigation::EnsureAgent(
    const World& world, EntityHandle entity, StableEntityId stableId)
{
    AgentConfig cfg{};
    if (!TryReadAgentConfig(world, entity, cfg)) return nullptr;

    auto it = agents_.find(stableId);
    if (it == agents_.end())
    {
        AgentRuntime runtime;
        runtime.authored = cfg;
        runtime.activeTarget = cfg.authoredHasTarget;
        runtime.target = cfg.authoredTarget;
        auto [inserted, _] = agents_.emplace(stableId, std::move(runtime));
        return &inserted->second;
    }

    AgentRuntime& runtime = it->second;
    const bool authoredChanged = runtime.authored.authoredHasTarget != cfg.authoredHasTarget
        || LengthSquared(runtime.authored.authoredTarget - cfg.authoredTarget) > 1e-6f;
    if (authoredChanged)
    {
        runtime.runtimeHasOverride = false;
        runtime.activeTarget = cfg.authoredHasTarget;
        runtime.target = cfg.authoredTarget;
        runtime.pathPlanned = false;
        runtime.arrived = false;
    }
    if (runtime.authored.asset != cfg.asset)
    {
        runtime.pathPlanned = false;
    }
    runtime.authored = std::move(cfg);
    return &runtime;
}

void SceneNavigation::PlanAgentPath(AgentRuntime& runtime, Vec3 startPos, AssetStore* assets)
{
    runtime.waypoints.clear();
    runtime.waypointIndex = 0u;
    runtime.arrived = false;
    runtime.plannedGoal = runtime.target;
    runtime.plannedAsset = runtime.authored.asset;
    runtime.pathPlanned = true;

    std::string errorKey;
    const auto navMesh = ResolveNavMesh(assets, runtime.authored.asset, errorKey);
    if (!navMesh)
    {
        runtime.status = NavPathStatus::Unreachable;
        return;
    }

    NavPathResult result = FindNavPath(*navMesh, startPos, runtime.target);
    runtime.status = result.status;
    runtime.waypoints = std::move(result.waypoints);
    if (runtime.waypoints.size() > 1u && DistXZ(startPos, runtime.waypoints.front()) <= 0.05f)
    {
        runtime.waypointIndex = 1u;
    }
}

bool SceneNavigation::SetDestination(
    const World& world, AssetStore* assets, StableEntityId entityId, Vec3 target, std::string& errorKey)
{
    errorKey.clear();
    if (!IsFinite(target))
    {
        errorKey = "Bridge.InvalidArgument";
        return false;
    }
    const auto entity = world.FindByStableId(entityId);
    if (!entity)
    {
        errorKey = "Bridge.InvalidEntity";
        return false;
    }
    auto* runtime = EnsureAgent(world, entity, entityId);
    if (!runtime)
    {
        errorKey = "Navigation.AgentInactive";
        return false;
    }

    runtime->runtimeHasOverride = true;
    runtime->activeTarget = true;
    runtime->target = target;
    runtime->arrived = false;
    PlanAgentPath(*runtime, ReadEntityWorldPosition(world, entity), assets);
    return true;
}

bool SceneNavigation::StopAgent(const World& world, StableEntityId entityId, std::string& errorKey)
{
    errorKey.clear();
    const auto entity = world.FindByStableId(entityId);
    if (!entity)
    {
        errorKey = "Bridge.InvalidEntity";
        return false;
    }
    auto* runtime = EnsureAgent(world, entity, entityId);
    if (!runtime)
    {
        errorKey = "Navigation.AgentInactive";
        return false;
    }

    runtime->runtimeHasOverride = true;
    runtime->activeTarget = false;
    runtime->waypoints.clear();
    runtime->waypointIndex = 0u;
    runtime->pathPlanned = false;
    runtime->arrived = false;
    return true;
}

void SceneNavigation::StepAgents(
    World& world, PhysicsWorld& physics, ScenePhysics& scenePhysics, AssetStore* assets, float deltaSeconds)
{
    if (!IsFinite(deltaSeconds) || deltaSeconds <= 0.0f) return;

    std::vector<StableEntityId> seen;
    for (const auto entity : world.Entities())
    {
        const auto* record = world.Get(entity);
        if (!record) continue;
        AgentRuntime* runtime = EnsureAgent(world, entity, record->stableId);
        if (!runtime) continue;
        seen.push_back(record->stableId);

        if (!runtime->activeTarget) continue;

        const Vec3 currentPos = ReadEntityWorldPosition(world, entity);
        if (!runtime->pathPlanned
            || (runtime->authored.autoRepath && (LengthSquared(runtime->plannedGoal - runtime->target) > 1e-4f
                || runtime->plannedAsset != runtime->authored.asset)))
        {
            PlanAgentPath(*runtime, currentPos, assets);
        }

        if (runtime->status == NavPathStatus::Unreachable || runtime->waypoints.empty())
        {
            continue;
        }

        const float stopDist = runtime->authored.stoppingDistance;
        while (runtime->waypointIndex + 1u < runtime->waypoints.size()
            && DistXZ(currentPos, runtime->waypoints[runtime->waypointIndex]) <= 0.1f)
        {
            ++runtime->waypointIndex;
        }

        const Vec3 activeTarget = runtime->waypoints[runtime->waypointIndex];
        const bool isFinal = (runtime->waypointIndex + 1u == runtime->waypoints.size());
        const float distXZ = DistXZ(currentPos, activeTarget);
        if (isFinal && distXZ <= stopDist)
        {
            runtime->arrived = true;
            if (scenePhysics.Character(record->stableId))
            {
                scenePhysics.MoveCharacter(world, physics, record->stableId, {}, false, deltaSeconds);
            }
            continue;
        }

        runtime->arrived = false;
        const Vec3 delta{activeTarget.x - currentPos.x, 0.0f, activeTarget.z - currentPos.z};
        const float len = std::hypot(delta.x, delta.z);
        if (len <= 1e-5f) continue;

        const Vec3 dirXZ{delta.x / len, 0.0f, delta.z / len};
        if (const auto* character = scenePhysics.Character(record->stableId))
        {
            const float moveDt = std::min(deltaSeconds, 0.05f);
            const float walkSpeed = character->walkSpeed;
            float scale = walkSpeed > 1e-4f ? std::min(1.0f, runtime->authored.speed / walkSpeed) : 1.0f;
            if (isFinal && walkSpeed * moveDt > 1e-4f)
            {
                scale = std::min(scale, len / (walkSpeed * moveDt));
            }
            scenePhysics.MoveCharacter(world, physics, record->stableId, dirXZ * scale, false, moveDt);
        }
        else if (world.GetTransform(entity))
        {
            const float step = std::min(len, runtime->authored.speed * deltaSeconds);
            Vec3 nextWorld{currentPos.x + dirXZ.x * step, currentPos.y, currentPos.z + dirXZ.z * step};
            if (isFinal || len > step)
            {
                nextWorld.y = currentPos.y + (activeTarget.y - currentPos.y) * (step / std::max(len, 1e-4f));
            }
            ApplyWorldPosition(world, entity, nextWorld);
        }
    }

    for (auto it = agents_.begin(); it != agents_.end();)
    {
        if (std::find(seen.begin(), seen.end(), it->first) == seen.end()) it = agents_.erase(it);
        else ++it;
    }
}

float SceneNavigation::RemainingDistanceMeters(const World& world, StableEntityId entityId) const noexcept
{
    const auto it = agents_.find(entityId);
    if (it == agents_.end()) return 0.0f;
    const AgentRuntime& runtime = it->second;
    if (!runtime.activeTarget || runtime.arrived || runtime.waypoints.empty()
        || runtime.waypointIndex >= runtime.waypoints.size())
    {
        return 0.0f;
    }

    const auto entity = world.FindByStableId(entityId);
    if (!entity) return 0.0f;
    const Vec3 currentPos = ReadEntityWorldPosition(world, entity);
    float total = DistXZ(currentPos, runtime.waypoints[runtime.waypointIndex]);
    for (std::size_t i = runtime.waypointIndex + 1u; i < runtime.waypoints.size(); ++i)
    {
        total += DistXZ(runtime.waypoints[i - 1u], runtime.waypoints[i]);
    }
    return total;
}

NavPathStatus SceneNavigation::AgentStatus(StableEntityId entityId) const noexcept
{
    const auto it = agents_.find(entityId);
    return it != agents_.end() ? it->second.status : NavPathStatus::Unreachable;
}
} // namespace flare
