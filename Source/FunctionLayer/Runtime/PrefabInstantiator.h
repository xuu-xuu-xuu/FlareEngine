#pragma once

#include "ResourceLayer/Scenes/PrefabDocument.h"
#include "FunctionLayer/Runtime/World.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace se
{
struct InstantiatedPrefabScript
{
    StableEntityId entity = 0;
    PrefabScriptRecord script;
};

struct PrefabInstanceReport
{
    StableEntityId rootEntity = 0;
    std::unordered_map<std::uint64_t, StableEntityId> nodeToEntity;
    std::vector<InstantiatedPrefabScript> scripts;
};

// 从 PrefabDocument 实例化子树：创建组件、一次性重映射内部实体/脚本引用，最后按覆盖键应用覆盖值。
// 若失败，已创建的子树实体及稳定 ID 预留会全部清理，不留半成品。
StableEntityId InstantiatePrefab(
    World& world,
    const PrefabDocument& document,
    const Transform& rootTransform,
    std::string* error = nullptr,
    const std::string& prefabId = {},
    StableEntityId rootParent = 0,
    PrefabInstanceReport* report = nullptr,
    const std::unordered_map<std::uint64_t, std::vector<PropertyOverride>>* overridesByNode = nullptr);

[[nodiscard]] bool ReadEntityOverrideValue(const World& world, EntityHandle entity, std::string_view key,
                                           PropertyValue& outValue, std::string& error);
[[nodiscard]] bool ApplyPropertyOverride(World& world, EntityHandle entity,
                                         const PropertyOverride& overrideValue, std::string& error);

// 读取任意 PrefabNodeRecord 上某个完整属性键对应的默认值（支持 "name" / "enabled" / 组件属性全键）。
[[nodiscard]] bool ReadPrefabNodePropertyValue(const PrefabNodeRecord& node, std::string_view key,
                                               PropertyValue& outValue, bool& outIsLocalRef);

// 从世界中的选中实体子树构建 PrefabDocument，内部引用自动转为 localNodeId 引用。
[[nodiscard]] bool BuildPrefabDocumentFromEntity(
    const World& world,
    EntityHandle rootEntity,
    PrefabDocument& outDocument,
    std::unordered_map<StableEntityId, std::uint64_t>& outEntityToLocal,
    std::string& error,
    const std::vector<InstantiatedPrefabScript>& attachedScripts = {});

// 将更新后的 PrefabDocument 同步至当前世界中所有同 prefabId 的实例，保留实例位置及已声明覆盖项。
[[nodiscard]] bool SyncPrefabInstancesInWorld(
    World& world,
    const std::string& prefabId,
    const PrefabDocument& document,
    std::string& error,
    std::vector<InstantiatedPrefabScript>* outScripts = nullptr);
} // namespace se
