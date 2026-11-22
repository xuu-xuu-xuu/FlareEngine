# 预制体格式、实例化与覆盖同步

这篇文档讲清楚 Flare 预制体（`.prefab.json`）如何把一棵带组件和 C# 脚本的实体子树存成独立资产、如何原子实例化、如何追踪属性级/组件级覆写（Override），以及 `Revert / Apply` 与拖拽自动建预制体背后的完整事务流程。使用者操作步骤见[预制体与玩法系统](../user/gameplay-systems.md)。

## 一句话定位

预制体由三层协作完成：**数据格式层**（`PrefabDocument`）负责纯数据序列化与校验；**运行时层**（`PrefabInstantiator`）负责在 `World` 中原子展开子树、重映射内部引用并同步同源实例；**编辑器工作流层**（`PrefabWorkflow`）负责自动覆写追踪、`Revert / Apply` 事务回滚以及拖放建预制体。

## 完整工作流与数据流

```text
  [场景实体子树] ──(拖入 Project 目录 / 创建预制体)──▶ CreatePrefabFromSelection()
       ▲                                                  ├─ BuildPrefabDocumentFromEntity() (内部引用 → localNodeId)
       │ 自动挂上 PrefabLink                               ├─ WritePrefabDocument() (.prefab.json 原子写盘)
       │                                                  └─ RegisterSource() (.prefab.json.asset.json + project.json)
       │                                                                    │
  编辑实例属性 ──▶ RecordOverrideAfterPropertyEdit()                         │ 拖入视口 / 运行时生成
  (diff 原型，自动增删 PrefabLink::overrides)                                ▼
       │                                                       InstantiatePrefab()
       ├── Revert (字段 / 组件 / 整实例) ──▶ 从母版恢复并清覆盖    ├─ 拓扑序建实体 (保留 Tag/Layer/StaticFlags)
       └── Apply  (字段 / 组件 / 整实例) ──▶ 回写母版 + 同步全场景 └─ 失败时 CleanupCreatedEntities 零污染回滚
```

## 核心源码拆解

### 1. 预制体资产格式（`PrefabDocument`）

[`PrefabDocument`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/PrefabDocument.h#L73) 位于 `ResourceLayer/Scenes`，只依赖 `EngineCore` 与 JSON 解析器。它保存版本号（`1`）、根节点局部 ID `rootLocalNodeId` 和有序节点数组 `nodes`。每个节点完整记录名称、启用状态、实体契约字段（`tag`、`layer`、`staticFlags`）、组件属性与脚本字段，并把指向子树内部节点的实体引用转换为 `localNodeId`（组件侧标 `localEntityReference = true`，脚本侧存 `localNodeRef`）。

来源：[PrefabDocument.h#L16](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/PrefabDocument.h#L16) · [PrefabDocument.cpp#L217](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/PrefabDocument.cpp#L217)

```cpp
// Source/ResourceLayer/Scenes/PrefabDocument.h
struct PropertyOverride
{
    std::string key;      // 规范化反射全键，如 "light.intensity" / "tag" / "body.enabled"
    PropertyValue value;
};

struct PrefabLink
{
    std::string prefabId; // 32 位小写十六进制 AssetId
    std::uint64_t localNodeId = 0;
    std::vector<PropertyOverride> overrides;
};

struct PrefabNodeRecord
{
    std::uint64_t localNodeId = 0;
    std::uint64_t localParentId = 0;
    std::string name;
    bool enabled = true;
    std::string tag = "Untagged";
    std::uint32_t layer = 0;
    std::uint32_t staticFlags = 0;
    std::vector<PrefabComponentRecord> components;
    std::vector<PrefabScriptRecord> scripts;
};
```

- **序列化格式与向后兼容**：[`SerializePrefabDocument()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/PrefabDocument.cpp#L144) 写出 `"format": "FlareEngine.Prefab"` 并按 `localNodeId` 升序排列节点；[`ParsePrefabDocument()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/PrefabDocument.cpp#L235) 同时只读兼容旧格式名 `"SelfEngine.Prefab"`，并强制校验单根树、无环父链、必含 `Transform`、最大 16MB 与最大深度 64。
- **原子写盘**：[`WritePrefabDocument()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/PrefabDocument.cpp#L429) 先写临时文件并 `FlushFileBuffers`，再用 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` 原子替换。

### 2. 实例链接与原子展开（`PrefabInstantiator`）

场景中每个来自预制体的实体都在 [`EntityRecord::prefabLink`](entities.md) 上挂载 `PrefabLink`。[`InstantiatePrefab()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/PrefabInstantiator.cpp#L174) 按广度优先拓扑序重建整棵子树，并保证**全成或全撤**：

1. **建实体与层级**：为每个节点创建实体并应用 `enabled`、`tag`、`layer`、`staticFlags` 及父子关系，建立 `localNodeId -> StableEntityId` 映射表。
2. **建组件与重映射内部引用**：根节点采用传入的 `rootTransform`，其余子节点使用原型局部变换；若属性的 `localEntityReference == true`，自动查表替换为新实例对应的 `StableEntityId`。
3. **挂 `PrefabLink` 并应用 `overrides`**：同时将脚本字段的 `localNodeRef` 改写为新实体的十进制 ID 字符串，汇总进 `PrefabInstanceReport::scripts` 交由托管脚本宿主挂载。
4. **失败零污染回滚**：任一步骤失败，立刻调用 `CleanupCreatedEntities()` 销毁已建实体、`FlushCommands()` 并调用 `ForgetStableId()` 回收本批预留的稳定 ID，返回 `0`。

[`BuildPrefabDocumentFromEntity()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/PrefabInstantiator.cpp#L489) 执行逆向采集：会把根节点局部 `position`/`rotation` 归零（保留 `scale`），并把子树内部的组件/脚本实体引用自动转为 `localNodeId`。

### 3. 母版变更全场景同步（`SyncPrefabInstancesInWorld`）

当母版 `.prefab.json` 被修改时，[`SyncPrefabInstancesInWorld()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/PrefabInstantiator.cpp#L629) 会找出世界中所有同 `prefabId` 的实例根并逐一差量同步：

- 删除母版中已删去的非根节点，补建母版新增的子节点；
- 刷新所有**未出现在 `PrefabLink::overrides` 中**的实体头字段（`name`/`enabled`/`tag`/`layer`/`staticFlags`）、组件增删、组件启用状态（`<comp>.enabled`）与组件属性；
- **始终保护实例根节点的摆放 `position`/`rotation`**（仅同步未覆写的 `scale`），最后重放实例自己的 `overrides`，保证定制值绝不被冲掉。

### 4. 编辑器覆写追踪、`Revert / Apply` 与拖放建预制体（`PrefabWorkflow`）

编辑器侧在 [`PrefabWorkflow.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.cpp#L455) 完整实现了 Unity 风格的预制体工作流：

- **自动覆写记录**：每次属性编辑结束后，[`RecordOverrideAfterPropertyEdit()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.cpp#L455) 先经 `CanonicalPrefabPropertyKey()` 把 Inspector 别名规范化（如 `position.x` → `transform.position`、`entity.tag` → `tag`、`feature.enabled.body` → `body.enabled`），再读取母版值比对——**若值与母版相等则自动清除覆盖条目，不等则写入/更新 `overrides`**（根实体的世界摆位 `transform.position`/`transform.rotation` 刻意排除）。
- **三级 `Revert` 与 `Apply`**：
  - **字段级**：[`RevertPropertyOverride()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.cpp#L696) / [`ApplyPropertyOverrideToPrefab()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.cpp#L853)
  - **组件级**：[`RevertComponentOverrides()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.cpp#L924) / [`ApplyComponentOverridesToPrefab()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.cpp#L992)（若组件是实例额外新增、母版不存在的可选组件，Revert 会直接移除该组件）
  - **整实例级**：[`RevertAllOverrides()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.cpp#L1099) / [`ApplyInstanceToPrefab()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.cpp#L1218)
- **Apply 的 `localNodeId` 保活与事务安全**：`ApplyInstanceToPrefab()` 重新导出子树时，会优先复用实例原本的 `localNodeId`（防止场景里其他同源实例的覆盖键失配），并**先开启 `EditorSession` 内存事务校验并同步所有场景实例，确认无误后再写盘并同步脚本**；如果写盘或脚本同步失败，自动撤销内存事务并用 `originalDiskBytes` 恢复原 `.prefab.json` 文件。
- **拖放目录自动创建预制体**：在 Hierarchy 中把实体拖拽到 Project 资源浏览器的目录中时，[`SetHierarchyAssetDropHandler`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorToolkitSession.cpp#L1129) 解析当前选中目录并调用 [`CreatePrefabFromSelection()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.cpp#L528)：

```cpp
// Source/ToolLayer/Editor/PrefabWorkflow.cpp (目录拖放自动命名核心逻辑)
if (!autoDir.empty())
{
    const std::string stem = SanitizePrefabFileStem(rootRecord->name);
    for (unsigned index = 0; index < 10000; ++index)
    {
        const std::string suffix = index == 0 ? std::string{} : std::to_string(index);
        const std::filesystem::path candidate = autoDir / (stem + suffix + std::string(PrefabFileExtension));
        const std::filesystem::path meta      = autoDir / (stem + suffix + std::string(PrefabMetadataExtension));
        if (!std::filesystem::exists(candidate) && !std::filesystem::exists(meta))
        {
            targetPath = candidate;
            break;
        }
    }
}
```

写出 `.prefab.json` 后，流程会紧接着注册 `.prefab.json.asset.json`、同步 `project.json` 资产清单，并在一次撤销步内将原场景实体子树原地绑定上新建的 `PrefabLink`。反向拖放（从资产面板把 `.prefab.json` 拖入视口）则由 [`ViewportDropWorkflow.cpp#L441`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/ViewportDropWorkflow.cpp#L441) 调用 `InstantiatePrefabIntoScene()` 完成落地实例化。

## 核心接口速查

| 层级 | 函数 | 作用 | 源码位置 |
| --- | --- | --- | --- |
| `ResourceLayer` | `SerializePrefabDocument` / `ParsePrefabDocument` | `.prefab.json` 文本编解码与结构校验 | [PrefabDocument.h#L85](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/PrefabDocument.h#L85) |
| `ResourceLayer` | `ReadPrefabDocument` / `WritePrefabDocument` | 带 BOM 剥离读盘与临时文件原子写盘 | [PrefabDocument.h#L87](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/PrefabDocument.h#L87) |
| `Runtime` | `InstantiatePrefab` | 原子展开预制体子树、重映射内部引用与应用覆盖 | [PrefabInstantiator.h#L28](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/PrefabInstantiator.h#L28) |
| `Runtime` | `BuildPrefabDocumentFromEntity` | 从世界实体子树采集生成 `PrefabDocument` | [PrefabInstantiator.h#L48](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/PrefabInstantiator.h#L48) |
| `Runtime` | `SyncPrefabInstancesInWorld` | 按更新后的母版差量刷新世界内所有同源实例 | [PrefabInstantiator.h#L57](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/PrefabInstantiator.h#L57) |
| `Editor` | `CreatePrefabFromSelection` / `InstantiatePrefabIntoScene` | 编辑器创建/实例化预制体并同步脚本与资产库 | [PrefabWorkflow.h#L28](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.h#L28) |
| `Editor` | `Revert*` / `Apply*`（字段 / 组件 / 整实例） | 三级覆盖撤销与回写母版 | [PrefabWorkflow.h#L49](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.h#L49) |

::: tip 支持覆盖的属性键范围
除全部已注册组件反射属性键（如 `"mesh.materials"`、`"light.intensity"`、`"body.mass"`）外，`ReadEntityOverrideValue()` 与 `ApplyPropertyOverride()` 还支持 5 个实体头键（`"name"`、`"enabled"`、`"tag"`、`"layer"`、`"staticFlags"`）以及任意组件的启用状态键 `"<componentKey>.enabled"`。
:::

::: warning 为什么 Play 运行时禁止编辑预制体资产？
`PrefabWorkflow` 的全部创建、Revert 和 Apply 入口在第一行都会检查 `if (session.IsPlaying())` 并返回 `"Editor.EditWhilePlaying"`，防止运行期临时物理位移或脚本状态误污染磁盘上的母版 `.prefab.json`；运行期间只允许通过脚本或运行时接口对预制体进行实例化。
:::
