#include "EditorViewportCamera.h"
#include "FunctionLayer/Runtime/SceneSpatialIndex.h"
#include "ResourceLayer/Assets/StaticModelPreparation.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Spatial/BoundsIndex.h"
#include <unordered_map>
namespace flare
{
std::optional<CameraSnapshot> EditorViewportCamera::Snapshot(CameraViewport viewport,std::uint64_t frame) const
{
    CameraSnapshot snapshot;CameraError error;
    if(!BuildCameraSnapshot({UINT64_MAX,FreeCameraTransform(state).Matrix(),{},true},viewport,frame,snapshot,error))return {};
    return snapshot;
}
std::optional<Aabb> EditorEntityBounds(const World& world,AssetStore& assets,EntityHandle entity)
{
    if(!world.IsAlive(entity)||!world.IsEnabled(entity))return {};
    const auto meshComp=world.FindComponent(entity,ComponentTypes::Mesh);
    const auto* mesh=(!meshComp||(world.GetComponent(meshComp)&&world.GetComponent(meshComp)->Enabled()))?world.GetMesh(entity):nullptr;
    Aabb local=mesh?Aabb{{-.5f,-.5f,-.5f},{.5f,.5f,.5f}}:Aabb{{-.2f,-.2f,-.2f},{.2f,.2f,.2f}};
    if(mesh&&!IsBuiltinMesh(mesh->meshAsset))
    {
        std::vector<AssetDiagnostic> diagnostics;const auto model=assets.LoadModel(mesh->meshAsset,diagnostics);StaticModelLayout layout;
        if(!model||!BuildStaticModelLayout(*model,layout,diagnostics))return {};
        local={layout.minimum,layout.maximum};
    }
    return TransformBounds(local,world.GetWorldMatrix(entity));
}
namespace
{
struct PickModel
{
    std::shared_ptr<const StaticModel> model;
    StaticModelLayout layout;
};

CameraRay LocalRay(const CameraRay& world, const Mat4& localFromWorld)
{
    // 保留方向长度，使每个实例的 t 都能直接按世界距离比较，包括非均匀缩放。
    return {TransformPoint(localFromWorld, world.origin), TransformVector(localFromWorld, world.direction)};
}

bool HitsBounds(const CameraRay& ray, const Aabb& box, double limit)
{
    return spatial::Intersect(box, {{ray.origin.x, ray.origin.y, ray.origin.z},
        {ray.direction.x, ray.direction.y, ray.direction.z}, limit}).has_value();
}

struct TriangleHit
{
    double distance = 0.0;
    Vec3 normal{0.0f, 1.0f, 0.0f};
};

std::optional<TriangleHit> MeshDistance(const MeshData& mesh, const CameraRay& ray,
                                        const Mat4& worldFromMesh, double limit)
{
    const auto position = [&](std::uint32_t index) {
        const auto& p = mesh.vertices[index].position;
        return Vec3{p[0], p[1], p[2]};
    };
    std::optional<TriangleHit> nearest;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        const Vec3 a = position(mesh.indices[i]);
        const Vec3 b = position(mesh.indices[i + 1]);
        const Vec3 c = position(mesh.indices[i + 2]);
        const Vec3 ab = b - a, ac = c - a;
        const Vec3 h = Cross(ray.direction, ac);
        const double determinant = Dot(ab, h);
        if (determinant == 0) continue;
        const Vec3 offset = ray.origin - a;
        const double u = Dot(offset, h) / determinant;
        if (u < 0 || u > 1) continue;
        const Vec3 q = Cross(offset, ab);
        const double v = Dot(ray.direction, q) / determinant;
        if (v < 0 || u + v > 1) continue;
        const double t = Dot(ac, q) / determinant;
        if (t >= 0 && t <= limit)
        {
            limit = t;
            const Vec3 wa = TransformPoint(worldFromMesh, a);
            const Vec3 wb = TransformPoint(worldFromMesh, b);
            const Vec3 wc = TransformPoint(worldFromMesh, c);
            const Vec3 cross = Cross(wb - wa, wc - wa);
            const float lenSq = LengthSquared(cross);
            const Vec3 normal = lenSq > 1e-12f ? (cross / std::sqrt(lenSq)) : Vec3{0.0f, 1.0f, 0.0f};
            nearest = TriangleHit{t, normal};
        }
    }
    return nearest;
}
}

std::optional<EditorSurfaceHit> IntersectEditorSceneSurface(
    const World& world,
    AssetStore& assets,
    const CameraSnapshot& camera,
    const CameraRay& ray,
    SceneSpatialIndex* spatial)
{
    const float viewDepthPerMetre = -TransformVector(camera.view, ray.direction).z;
    if (viewDepthPerMetre <= 0) return {};
    double nearest = (camera.lens.farPlane - camera.lens.nearPlane) / viewDepthPerMetre;
    if (!std::isfinite(nearest) || nearest <= 0) return {};

    std::vector<EntityHandle> candidates;
    if (spatial && spatial->Statistics().tracked > 0)
    {
        candidates = spatial->RaycastCandidates(
            {{ray.origin.x, ray.origin.y, ray.origin.z},
             {ray.direction.x, ray.direction.y, ray.direction.z},
             nearest});
    }
    else
    {
        candidates = world.Entities();
    }

    std::optional<EditorSurfaceHit> hit;
    std::unordered_map<AssetId, PickModel> models;
    for (const auto entity : candidates)
    {
        const auto meshComp = world.FindComponent(entity, ComponentTypes::Mesh);
        if (meshComp && (!world.GetComponent(meshComp) || !world.GetComponent(meshComp)->Enabled())) continue;
        const auto* mesh = world.GetMesh(entity);
        if (!mesh || !world.IsEnabled(entity)) continue;
        const Mat4 worldMatrix = world.GetWorldMatrix(entity);
        const auto modelFromWorld = Inverse(worldMatrix);
        if (!modelFromWorld) continue;
        const auto local = LocalRay(ray, *modelFromWorld);
        std::optional<TriangleHit> surfaceHit;
        if (IsBuiltinMesh(mesh->meshAsset))
        {
            if (!HitsBounds(local, {{-.5f,-.5f,-.5f}, {.5f,.5f,.5f}}, nearest)) continue;
            static const MeshData cube = MakeUnitCube(), sphere = MakeUnitSphere();
            surfaceHit = MeshDistance(mesh->meshAsset == BuiltinCubeAssetId ? cube : sphere,
                                      local, worldMatrix, nearest);
        }
        else
        {
            auto [entry, inserted] = models.try_emplace(mesh->meshAsset);
            auto& geometry = entry->second;
            if (inserted)
            {
                std::vector<AssetDiagnostic> diagnostics;
                geometry.model = assets.LoadModel(mesh->meshAsset, diagnostics);
                if (geometry.model && !BuildStaticModelLayout(*geometry.model, geometry.layout, diagnostics))
                    geometry.model.reset();
            }
            if (!geometry.model || !HitsBounds(local, {geometry.layout.minimum, geometry.layout.maximum}, nearest)) continue;
            for (const auto& part : geometry.layout.parts)
            {
                const auto meshFromModel = Inverse(part.modelFromMesh);
                if (!meshFromModel) continue;
                const auto partHit = MeshDistance(geometry.model->meshes[part.mesh],
                    LocalRay(local, *meshFromModel), worldMatrix * part.modelFromMesh,
                    surfaceHit ? surfaceHit->distance : nearest);
                if (partHit) surfaceHit = partHit;
            }
        }
        const auto id = world.Get(entity)->stableId;
        if (surfaceHit && (surfaceHit->distance < nearest ||
            (surfaceHit->distance == nearest && (!hit || id < hit->entity))))
        {
            nearest = surfaceHit->distance;
            Vec3 normal = surfaceHit->normal;
            if (Dot(normal, ray.direction) > 0.0f) normal = normal * -1.0f;
            hit = EditorSurfaceHit{
                id,
                nearest,
                ray.origin + ray.direction * static_cast<float>(nearest),
                normal
            };
        }
    }
    return hit;
}

StableEntityId PickEditorBounds(const World& world, AssetStore& assets, const CameraSnapshot& camera, float x, float y)
{
    const auto& viewport = camera.viewport;
    const auto ray = ViewportPointToRay(camera, {viewport.x + x * viewport.width, viewport.y + y * viewport.height});
    if (!ray) return 0;
    const auto hit = IntersectEditorSceneSurface(world, assets, camera, *ray, nullptr);
    return hit ? hit->entity : 0;
}
}
