# 预制体格式、实例化与覆盖同步

预制体（`.prefab.json`，伴随 `.prefab.json.asset.json` 元数据）把一棵带组件与脚本绑定的实体子树序列化为独立资产；实例化后每个实体携带 `PrefabLink` 关联源资产与局部节点 ID，并支持逐属性覆盖写回与撤销。使用者操作步骤见[预制体与玩法系统](../user/gameplay-systems.md)。

## PrefabDocument

`PrefabDocument` 位于 `ResourceLayer/Scenes`，保存预制体版本号、根节点 ID `rootLocalNodeId` 与有序节点数组 `nodes`（名称、启用状态、父节点局部引用、组件记录列表 `components`（含局部变换与其余组件属性）以及脚本条目）。序列化与反序列化由 [SerializePrefabDocument()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ResourceLayer/Scenes/PrefabDocument.h#L82) 和 [ParsePrefabDocument()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ResourceLayer/Scenes/PrefabDocument.h#L83) 完成，只依赖 `EngineCore` 与 JSON 解析器。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ResourceLayer/Scenes/PrefabDocument.h#L70)

## PrefabLink

`PrefabLink` 挂载在场景实体的 [EntityRecord](entities.md) 上，记录来源预制体资产 ID、对应 `localNodeId` 及该实例相对母版改动过的属性覆写表（`overrides`）。场景保存时完整持久化，使母版更新不会冲掉实例级定制值。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ResourceLayer/Scenes/PrefabDocument.h#L22)

## PrefabInstantiator

`PrefabInstantiator` 位于 `FunctionLayer/Runtime`，在 [World](world.md#world) 中把 `PrefabDocument` 原子展开为实体子树，失败时自动回滚已创建的半成品实体。

### InstantiatePrefab

`StableEntityId InstantiatePrefab(World& world, const PrefabDocument& document, const Transform& rootTransform, std::string* error = nullptr, const std::string& prefabId = {}, ...)` 按父子拓扑以 `rootTransform` 重建全部节点、重映射内部实体引用、应用 `overrides` 并通过 `PrefabInstanceReport` 返回根实体稳定 ID、局部节点映射与待挂载脚本列表。返回 `0` 表示文档无效或创建失败且世界状态未受污染。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Runtime/PrefabInstantiator.h#L28)

### BuildPrefabDocumentFromEntity

`bool BuildPrefabDocumentFromEntity(const World&, EntityHandle root, ...)` 从场景中任意实体子树采集层级、组件属性与脚本绑定生成 `PrefabDocument`，用于编辑器“创建预制体”与“应用覆盖回预制体”。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Runtime/PrefabInstantiator.h#L48)

### SyncPrefabInstancesInWorld

`bool SyncPrefabInstancesInWorld(World&, const std::string& prefabId, const PrefabDocument&, ...)` 在母版 `.prefab.json` 变更后同步世界内所有同源实例：保留每个实例显式记录在 `PrefabLink::overrides` 中的差异属性，其余属性与增删子节点按最新母版刷新。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Runtime/PrefabInstantiator.h#L57)
