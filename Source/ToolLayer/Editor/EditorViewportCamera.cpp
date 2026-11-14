#include "EditorViewportCamera.h"
#include "ResourceLayer/Assets/StaticModelPreparation.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Spatial/BoundsIndex.h"
#include <unordered_map>
namespace se
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
    const auto* mesh=world.GetMesh(entity);
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

std::optional<double> MeshDistance(const MeshData& mesh, const CameraRay& ray, double limit)
{
    const auto position = [&](std::uint32_t index) {
        const auto& p = mesh.vertices[index].position;
        return Vec3{p[0], p[1], p[2]};
    };
    std::optional<double> nearest;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        const Vec3 a = position(mesh.indices[i]);
        const Vec3 ab = position(mesh.indices[i + 1]) - a, ac = position(mesh.indices[i + 2]) - a;
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
            nearest = t;
        }
    }
    return nearest;
}
}

StableEntityId PickEditorBounds(const World& world, AssetStore& assets, const CameraSnapshot& camera, float x, float y)
{
    const auto& viewport = camera.viewport;
    const auto ray = ViewportPointToRay(camera, {viewport.x + x * viewport.width, viewport.y + y * viewport.height});
    if (!ray) return 0;
    const float viewDepthPerMetre = -TransformVector(camera.view, ray->direction).z;
    if (viewDepthPerMetre <= 0) return 0;
    double nearest = (camera.lens.farPlane - camera.lens.nearPlane) / viewDepthPerMetre;
    StableEntityId selected = 0;
    std::unordered_map<AssetId, PickModel> models;
    for (auto entity : world.Entities())
    {
        const auto* mesh = world.GetMesh(entity);
        if (!mesh || !world.IsEnabled(entity)) continue;
        const auto modelFromWorld = Inverse(world.GetWorldMatrix(entity));
        if (!modelFromWorld) continue;
        const auto local = LocalRay(*ray, *modelFromWorld);
        std::optional<double> distance;
        if (IsBuiltinMesh(mesh->meshAsset))
        {
            if (!HitsBounds(local, {{-.5f,-.5f,-.5f}, {.5f,.5f,.5f}}, nearest)) continue;
            static const MeshData cube = MakeUnitCube(), sphere = MakeUnitSphere();
            distance = MeshDistance(mesh->meshAsset == BuiltinCubeAssetId ? cube : sphere, local, nearest);
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
                const auto partDistance = MeshDistance(geometry.model->meshes[part.mesh],
                    LocalRay(local, *meshFromModel), distance.value_or(nearest));
                if (partDistance) distance = partDistance;
            }
        }
        const auto id = world.Get(entity)->stableId;
        if (distance && (*distance < nearest || (*distance == nearest && (!selected || id < selected))))
        {
            nearest = *distance;
            selected = id;
        }
    }
    return selected;
}
}
