#pragma once

#include "EditorSession.h"
#include "FunctionLayer/Runtime/Camera.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <filesystem>
#include <string>
#include <vector>

namespace se
{
struct EditorApplicationHooks;
class SceneSpatialIndex;

struct DropPlacement
{
    Vec3 position{};
    Quat rotation{};
    Aabb worldBounds{};
    bool valid = false;
};

enum class ViewportDropAssetKind
{
    None = 0,
    BuiltinCube,
    BuiltinSphere,
    Model,
    Prefab
};

struct ViewportDropPayload
{
    ViewportDropAssetKind kind = ViewportDropAssetKind::None;
    AssetId assetId;
    std::filesystem::path assetPath;
    std::string displayName;
};

[[nodiscard]] bool ResolveViewportDropPayload(const AssetStore& assets, const std::string& pathOrAssetId,
                                              ViewportDropPayload& outPayload, std::string& error);
[[nodiscard]] Aabb ComputeDropPayloadLocalBounds(AssetStore& assets, const ViewportDropPayload& payload);
[[nodiscard]] bool ValidateDropPayloadDependencies(AssetStore& assets, const ViewportDropPayload& payload,
                                                   std::string& error);

// 纯求交落场计算：优先按相机视口射线与场景静态网格三角形精确求交，未命中表面则回退到 y=0 地平面。
[[nodiscard]] DropPlacement ComputeDropPlacement(
    const CameraSnapshot& camera,
    Vec2 normalizedViewportUv,
    const World& world,
    AssetStore& assets,
    const Aabb& localAssetBounds,
    SceneSpatialIndex* spatial = nullptr);

class ViewportDropWorkflow
{
public:
    bool BeginOrUpdateHover(
        const std::string& pathOrAssetId,
        const CameraSnapshot& camera,
        Vec2 normalizedViewportUv,
        const World& world,
        AssetStore& assets,
        SceneSpatialIndex* spatial = nullptr);
    void ClearHover() noexcept;
    [[nodiscard]] bool Active() const noexcept { return active_; }
    [[nodiscard]] const DropPlacement& CurrentPlacement() const noexcept { return placement_; }
    [[nodiscard]] const ViewportDropPayload& CurrentPayload() const noexcept { return payload_; }
    [[nodiscard]] const std::string& LastError() const noexcept { return error_; }
    void AppendPreviewLines(std::vector<rhi::SceneLine>& lines) const;

    bool CommitDrop(
        const std::string& pathOrAssetId,
        const CameraSnapshot& camera,
        Vec2 normalizedViewportUv,
        EditorSession& session,
        AssetStore& assets,
        std::string& error,
        EditorApplicationHooks* hooks = nullptr,
        StableEntityId* outCreatedRoot = nullptr);

private:
    bool active_ = false;
    ViewportDropPayload payload_;
    DropPlacement placement_;
    std::string error_;
};
} // namespace se
