#include "ViewportDropWorkflow.h"
#include "EditorApplication.h"
#include "EditorCreation.h"
#include "EditorSceneGizmos.h"
#include "EditorViewportCamera.h"
#include "PrefabWorkflow.h"
#include "FunctionLayer/Runtime/PrefabInstantiator.h"
#include "FunctionLayer/Runtime/SceneSpatialIndex.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/StaticModelPreparation.h"
#include "ResourceLayer/Scenes/PrefabDocument.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace flare
{
namespace
{
std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool SamePathEquivalent(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ec;
    return !a.empty() && !b.empty() && std::filesystem::equivalent(a, b, ec) && !ec;
}

Aabb MergeBounds(const Aabb& a, const Aabb& b)
{
    return {
        {std::min(a.minimum.x, b.minimum.x), std::min(a.minimum.y, b.minimum.y), std::min(a.minimum.z, b.minimum.z)},
        {std::max(a.maximum.x, b.maximum.x), std::max(a.maximum.y, b.maximum.y), std::max(a.maximum.z, b.maximum.z)}
    };
}

void ExpandBoundsWithPoint(Aabb& bounds, const Vec3& point, bool& hasPoint)
{
    if (!hasPoint)
    {
        bounds = {point, point};
        hasPoint = true;
        return;
    }
    bounds = MergeBounds(bounds, {point, point});
}

Aabb ComputeMeshAssetLocalBounds(AssetStore& assets, const AssetId& assetId)
{
    if (assetId == BuiltinCubeAssetId || assetId == BuiltinSphereAssetId)
    {
        return {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    }
    std::vector<AssetDiagnostic> diagnostics;
    const auto model = assets.LoadModel(assetId, diagnostics);
    StaticModelLayout layout;
    if (model && BuildStaticModelLayout(*model, layout, diagnostics))
    {
        return {layout.minimum, layout.maximum};
    }
    return {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
}

bool ValidateModelDependencies(AssetStore& assets, const AssetId& modelId, std::string& error)
{
    std::vector<AssetDiagnostic> diagnostics;
    const auto model = assets.LoadModel(modelId, diagnostics);
    if (!model)
    {
        error = diagnostics.empty() ? "Assets.InvalidModel" : diagnostics.back().key;
        return false;
    }
    const auto source = assets.Resolve(modelId);
    for (const std::string& dependency : model->dependencies)
    {
        std::filesystem::path resolved;
        std::string relative;
        if (!asset_io::ResolveRelative(source, assets.SourceRoot(), dependency, resolved, relative) ||
            !std::filesystem::is_regular_file(resolved))
        {
            error = "Assets.ImportDependencyMissing";
            return false;
        }
    }
    error.clear();
    return true;
}

bool ValidatePrefabAssetDependencies(AssetStore& assets, const PrefabDocument& document, std::string& error)
{
    for (const PrefabNodeRecord& node : document.nodes)
    {
        for (const PrefabComponentRecord& comp : node.components)
        {
            for (const PrefabPropertyRecord& prop : comp.properties)
            {
                if (prop.type == PropertyType::AssetId)
                {
                    const auto* id = std::get_if<std::string>(&prop.value);
                    if (!id || id->empty() || IsBuiltinMesh(*id)) continue;
                    if (assets.Resolve(*id).empty())
                    {
                        error = "Prefab.DependencyMissing";
                        return false;
                    }
                    if (comp.type == ComponentTypes::Mesh && prop.key == "asset" &&
                        !ValidateModelDependencies(assets, *id, error))
                    {
                        return false;
                    }
                }
                else if (prop.type == PropertyType::AssetList)
                {
                    const auto* ids = std::get_if<std::vector<std::string>>(&prop.value);
                    if (!ids) continue;
                    for (const std::string& id : *ids)
                    {
                        if (!id.empty() && assets.Resolve(id).empty())
                        {
                            error = "Prefab.DependencyMissing";
                            return false;
                        }
                    }
                }
            }
        }
    }
    error.clear();
    return true;
}
} // namespace

bool ResolveViewportDropPayload(const AssetStore& assets, const std::string& pathOrAssetId,
                                ViewportDropPayload& outPayload, std::string& error)
{
    outPayload = {};
    if (pathOrAssetId.empty())
    {
        error = "Drop.UnsupportedAsset";
        return false;
    }
    if (pathOrAssetId == BuiltinCubeAssetId)
    {
        outPayload = {ViewportDropAssetKind::BuiltinCube, BuiltinCubeAssetId, {}, "Cube"};
        error.clear();
        return true;
    }
    if (pathOrAssetId == BuiltinSphereAssetId)
    {
        outPayload = {ViewportDropAssetKind::BuiltinSphere, BuiltinSphereAssetId, {}, "Sphere"};
        error.clear();
        return true;
    }

    AssetId resolvedId;
    std::filesystem::path resolvedPath;
    if (IsValidAssetIdString(pathOrAssetId))
    {
        resolvedPath = assets.Resolve(pathOrAssetId);
        if (!resolvedPath.empty()) resolvedId = pathOrAssetId;
    }
    if (resolvedPath.empty())
    {
        const std::filesystem::path requested = asset_io::FromUtf8(pathOrAssetId);
        const std::filesystem::path candidate = requested.is_absolute() ? requested : assets.SourceRoot() / requested;
        for (const AssetId& id : assets.ListIds())
        {
            const std::filesystem::path assetFile = assets.Resolve(id);
            if (assetFile == candidate || SamePathEquivalent(assetFile, candidate))
            {
                resolvedId = id;
                resolvedPath = assetFile;
                break;
            }
        }
    }

    if (resolvedId.empty() || resolvedPath.empty() || !std::filesystem::is_regular_file(resolvedPath))
    {
        error = "Drop.UnsupportedAsset";
        return false;
    }

    if (IsPrefabDocumentPath(resolvedPath))
    {
        outPayload = {ViewportDropAssetKind::Prefab, resolvedId, resolvedPath, asset_io::Utf8(resolvedPath.stem())};
        error.clear();
        return true;
    }

    const std::string ext = LowerAscii(asset_io::Utf8(resolvedPath.extension()));
    if (ext == ".fbx" || ext == ".glb" || ext == ".gltf")
    {
        outPayload = {ViewportDropAssetKind::Model, resolvedId, resolvedPath, asset_io::Utf8(resolvedPath.stem())};
        error.clear();
        return true;
    }

    error = "Drop.UnsupportedAsset";
    return false;
}

Aabb ComputeDropPayloadLocalBounds(AssetStore& assets, const ViewportDropPayload& payload)
{
    if (payload.kind == ViewportDropAssetKind::BuiltinCube || payload.kind == ViewportDropAssetKind::BuiltinSphere)
    {
        return {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    }
    if (payload.kind == ViewportDropAssetKind::Model)
    {
        return ComputeMeshAssetLocalBounds(assets, payload.assetId);
    }
    if (payload.kind == ViewportDropAssetKind::Prefab)
    {
        PrefabDocument document;
        std::string error;
        if (!ReadPrefabDocument(payload.assetPath, document, error))
        {
            return {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
        }
        std::unordered_map<std::uint64_t, const PrefabNodeRecord*> byId;
        for (const PrefabNodeRecord& node : document.nodes) byId.emplace(node.localNodeId, &node);

        std::function<Mat4(std::uint64_t)> nodeMatrix = [&](std::uint64_t id) -> Mat4 {
            const auto it = byId.find(id);
            if (it == byId.end()) return Mat4{};
            Transform t{};
            for (const PrefabComponentRecord& comp : it->second->components)
            {
                if (comp.type == ComponentTypes::Transform)
                {
                    ExtractTransformFromPrefabComponent(comp, t);
                    break;
                }
            }
            if (id == document.rootLocalNodeId) { t.position = {}; t.rotation = {}; }
            const Mat4 localMat = t.Matrix();
            return it->second->localParentId == 0 ? localMat : nodeMatrix(it->second->localParentId) * localMat;
        };

        Aabb total{{-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, 0.2f}};
        bool hasBounds = false;
        for (const PrefabNodeRecord& node : document.nodes)
        {
            Aabb local{{-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, 0.2f}};
            for (const PrefabComponentRecord& comp : node.components)
            {
                if (comp.type != ComponentTypes::Mesh) continue;
                for (const PrefabPropertyRecord& prop : comp.properties)
                {
                    if (prop.key == "asset")
                    {
                        if (const auto* meshId = std::get_if<std::string>(&prop.value))
                        {
                            local = ComputeMeshAssetLocalBounds(assets, *meshId);
                        }
                    }
                }
            }
            if (const auto transformed = TransformBounds(local, nodeMatrix(node.localNodeId)))
            {
                ExpandBoundsWithPoint(total, transformed->minimum, hasBounds);
                ExpandBoundsWithPoint(total, transformed->maximum, hasBounds);
            }
        }
        return total;
    }
    return {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
}

bool ValidateDropPayloadDependencies(AssetStore& assets, const ViewportDropPayload& payload, std::string& error)
{
    switch (payload.kind)
    {
    case ViewportDropAssetKind::BuiltinCube:
    case ViewportDropAssetKind::BuiltinSphere:
        error.clear();
        return true;
    case ViewportDropAssetKind::Model:
        return ValidateModelDependencies(assets, payload.assetId, error);
    case ViewportDropAssetKind::Prefab:
    {
        PrefabDocument document;
        if (!ReadPrefabDocument(payload.assetPath, document, error)) return false;
        return ValidatePrefabAssetDependencies(assets, document, error);
    }
    case ViewportDropAssetKind::None:
        break;
    }
    error = "Drop.UnsupportedAsset";
    return false;
}

DropPlacement ComputeDropPlacement(
    const CameraSnapshot& camera,
    Vec2 normalizedViewportUv,
    const World& world,
    AssetStore& assets,
    const Aabb& localAssetBounds,
    SceneSpatialIndex* spatial)
{
    DropPlacement placement;
    const auto& viewport = camera.viewport;
    if (viewport.width <= 0.0f || viewport.height <= 0.0f) return placement;

    const auto ray = ViewportPointToRay(
        camera, {viewport.x + normalizedViewportUv.x * viewport.width,
                 viewport.y + normalizedViewportUv.y * viewport.height},
        false);
    if (!ray) return placement;

    const float viewDepthPerMetre = -TransformVector(camera.view, ray->direction).z;
    if (viewDepthPerMetre <= 0.0f) return placement;
    const double maxDistance = (camera.lens.farPlane - camera.lens.nearPlane) / viewDepthPerMetre;

    Vec3 contact{};
    bool hitValid = false;
    if (const auto hit = IntersectEditorSceneSurface(world, assets, camera, *ray, spatial))
    {
        contact = hit->point;
        hitValid = true;
    }
    else if (std::abs(ray->direction.y) > 1e-6f)
    {
        const double planeT = -static_cast<double>(ray->origin.y) / static_cast<double>(ray->direction.y);
        if (std::isfinite(planeT) && planeT > 0.0 && planeT <= maxDistance)
        {
            contact = ray->origin + ray->direction * static_cast<float>(planeT);
            contact.y = 0.0f;
            hitValid = true;
        }
    }

    if (!hitValid || !IsFinite(contact) || !IsFinite(localAssetBounds.minimum) || !IsFinite(localAssetBounds.maximum))
    {
        return placement;
    }

    const Vec3 placementPos{contact.x, contact.y - localAssetBounds.minimum.y, contact.z};
    if (!IsFinite(placementPos)) return placement;

    placement.position = placementPos;
    placement.rotation = Quat{0.0f, 0.0f, 0.0f, 1.0f};
    placement.worldBounds = {localAssetBounds.minimum + placementPos, localAssetBounds.maximum + placementPos};
    placement.valid = true;
    return placement;
}

bool ViewportDropWorkflow::BeginOrUpdateHover(
    const std::string& pathOrAssetId,
    const CameraSnapshot& camera,
    Vec2 normalizedViewportUv,
    const World& world,
    AssetStore& assets,
    SceneSpatialIndex* spatial)
{
    ViewportDropPayload nextPayload;
    if (!ResolveViewportDropPayload(assets, pathOrAssetId, nextPayload, error_))
    {
        ClearHover();
        return false;
    }
    const Aabb localBounds = ComputeDropPayloadLocalBounds(assets, nextPayload);
    payload_ = std::move(nextPayload);
    placement_ = ComputeDropPlacement(camera, normalizedViewportUv, world, assets, localBounds, spatial);
    active_ = true;
    error_.clear();
    return placement_.valid;
}

void ViewportDropWorkflow::ClearHover() noexcept
{
    active_ = false;
    payload_ = {};
    placement_ = {};
}

void ViewportDropWorkflow::AppendPreviewLines(std::vector<rhi::SceneLine>& lines) const
{
    if (!active_) return;
    AppendDropPreviewGizmo(placement_.worldBounds, placement_.valid, lines);
}

bool ViewportDropWorkflow::CommitDrop(
    const std::string& pathOrAssetId,
    const CameraSnapshot& camera,
    Vec2 normalizedViewportUv,
    EditorSession& session,
    AssetStore& assets,
    std::string& error,
    EditorApplicationHooks* hooks,
    StableEntityId* outCreatedRoot)
{
    ClearHover();
    if (outCreatedRoot) *outCreatedRoot = 0;
    if (session.IsPlaying())
    {
        error = "Editor.EditWhilePlaying";
        error_ = error;
        return false;
    }

    ViewportDropPayload payload;
    if (!ResolveViewportDropPayload(assets, pathOrAssetId, payload, error) ||
        !ValidateDropPayloadDependencies(assets, payload, error))
    {
        error_ = error;
        return false;
    }

    const Aabb localBounds = ComputeDropPayloadLocalBounds(assets, payload);
    const DropPlacement drop = ComputeDropPlacement(
        camera, normalizedViewportUv, session.EditWorld(), assets, localBounds,
        &session.EditEnvironment().Spatial());
    if (!drop.valid)
    {
        error = "Drop.InvalidRay";
        error_ = error;
        return false;
    }

    const Transform dropTransform{drop.position, drop.rotation, {1.0f, 1.0f, 1.0f}};
    if (payload.kind == ViewportDropAssetKind::Prefab)
    {
        PrefabDocument prefabDoc;
        if (!ReadPrefabDocument(payload.assetPath, prefabDoc, error))
        {
            error_ = error;
            return false;
        }
        const Transform prefabTransform{drop.position, drop.rotation, PrefabRootAuthoredScale(prefabDoc)};
        StableEntityId rootId = 0;
        if (!InstantiatePrefabIntoScene(session, assets, payload.assetId, prefabTransform, rootId, error, hooks))
        {
            error_ = error;
            return false;
        }
        if (outCreatedRoot) *outCreatedRoot = rootId;
        error_.clear();
        return true;
    }

    if (!session.BeginEdit("Editor.DropAsset"))
    {
        error = session.LastError();
        error_ = error;
        return false;
    }

    StableEntityId createdId = 0;
    const bool ok = session.Apply("Editor.DropAsset", [&](World& world) {
        if (payload.kind == ViewportDropAssetKind::Model)
        {
            return SpawnModelEntity(world, assets, payload.assetId, payload.displayName, dropTransform, createdId, error);
        }
        const EntityHandle entity = world.Create(payload.displayName);
        if (!entity || !world.SetTransform(entity, dropTransform) ||
            !world.SetMesh(entity, {payload.assetId, {}}))
        {
            error = world.LastError();
            return false;
        }
        if (!world.FlushCommands())
        {
            error = world.LastError();
            return false;
        }
        createdId = world.Get(entity)->stableId;
        return true;
    });

    if (ok && createdId != 0 && session.Select(createdId) && session.CommitEdit())
    {
        if (outCreatedRoot) *outCreatedRoot = createdId;
        error_.clear();
        return true;
    }

    if (error.empty()) error = session.LastError();
    error_ = error;
    session.CancelEdit();
    return false;
}
} // namespace flare
