#pragma once

#include "EditorSession.h"
#include "FunctionLayer/Runtime/PrefabInstantiator.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <filesystem>
#include <string>
#include <string_view>

namespace se
{
struct EditorApplicationHooks;

// 将检视器别名（如 position.x、light.color.r、light.type）规范化为组件反射全键。
[[nodiscard]] std::string CanonicalPrefabPropertyKey(std::string_view inspectorOrPropertyKey);

// 向上查找所属预制体实例化根节点的稳定 ID；若未关联预制体则返回 0。
[[nodiscard]] StableEntityId FindEnclosingPrefabRoot(const World& world, StableEntityId entityId);

// 在单次属性编辑后比对原型，自动登记或清除 PropertyOverride（根实体的世界摆位 position/rotation 不计入覆盖）。
void RecordOverrideAfterPropertyEdit(World& world, AssetStore* assets, EntityHandle entity,
                                     std::string_view inspectorOrPropertyKey);

// 仅加载该 Prefab 根节点作者设定的缩放。
[[nodiscard]] Vec3 PrefabRootAuthoredScale(const PrefabDocument& document);

// 从选中实体子树创建 .prefab.json（含 .asset.json 与 project.json 同步）并把原实体子树链接到新预制体。
[[nodiscard]] bool CreatePrefabFromSelection(
    EditorSession& session,
    AssetStore& assets,
    const std::filesystem::path& projectRoot,
    StableEntityId rootEntityId,
    const std::filesystem::path& requestedPath,
    std::filesystem::path& createdPath,
    AssetId& createdAssetId,
    std::string& error,
    EditorApplicationHooks* hooks = nullptr);

[[nodiscard]] bool InstantiatePrefabIntoScene(
    EditorSession& session,
    AssetStore& assets,
    const AssetId& prefabId,
    const Transform& rootTransform,
    StableEntityId& createdRootId,
    std::string& error,
    EditorApplicationHooks* hooks = nullptr,
    StableEntityId rootParent = 0);

[[nodiscard]] bool RevertPropertyOverride(
    EditorSession& session,
    AssetStore& assets,
    StableEntityId entityId,
    std::string_view inspectorOrPropertyKey,
    std::string& error);

[[nodiscard]] bool RevertAllOverrides(
    EditorSession& session,
    AssetStore& assets,
    StableEntityId entityOrRootId,
    std::string& error);

[[nodiscard]] bool ApplyInstanceToPrefab(
    EditorSession& session,
    AssetStore& assets,
    const std::filesystem::path& projectRoot,
    StableEntityId entityOrRootId,
    std::string& error,
    EditorApplicationHooks* hooks = nullptr);

[[nodiscard]] bool SyncOpenScenePrefabInstances(
    EditorSession& session,
    AssetStore& assets,
    const AssetId& prefabId,
    std::string& error,
    EditorApplicationHooks* hooks = nullptr);
} // namespace se
