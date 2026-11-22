# 场景加载、相机与空间查询

这篇文档串联起场景文件从磁盘加载到画面渲染前的三件关键事：**场景序列化与异步提交（`SceneSerializer` / `SceneManager`）**、**动静分离空间索引（`SceneSpatialIndex`）**，以及**主相机选择与视口拾取射线（`Camera`）**。

## 一句话定位

`SceneSerializer` 负责把 `World` 无损持久化为 `FlareEngine.Scene` JSON（并向后兼容旧格式）；`SceneManager` 把后台读盘与主线程原子切换解耦；`SceneSpatialIndex` 与 `Camera` 则为渲染、拾取和编辑器交互提供增量更新的广相加速与投影矩阵快照。

## 场景异步加载与提交数据流

```text
  工作线程 (TaskQueue)                      主线程 (FrameBegin 安全边界)
  ───────────────────                      ────────────────────────────
  RequestFile(scene, path, mode)
        │
        ▼
  ReadSceneDocument()
  ParseSceneDocument() ──▶ 不可变 SceneDocument ──▶ 放入 asynchronous_->ready
  (校验深度<=64/防重键/大小<=16MB)                       │ (下一帧 FrameBegin 复验 request 代数)
                                                         ▼
                                               SceneManager::Apply()
                                                 ├─ 在 World(StagingTag) 中 InstantiateScene()
                                                 ├─ 校验器 / TODProfile / StableId 冲突检查
                                                 ├─ 扫描并清零跨场景悬空 EntityReference
                                                 ├─ World::CommitScene() 原子转移池节点
                                                 ├─ ActivateStagedComponents() 触发 OnAttach/OnEnable
                                                 └─ EventBus 发布 16(激活)/17(卸载)/18(失败)
```

## 核心源码拆解

### 1. 场景格式与原子写盘（`SceneSerializer`）

- **最新格式**：`SerializeScene()` 写出 `"format": "FlareEngine.Scene"`、`"version": 2`，并按 `stableId` 升序排列实体，保证内存槽位复用不会导致无意义的 Git diff 或脏状态误判。每个实体仅在值非默认时才写出 `tag`、`layer`、`staticFlags`、`prefab` 字段；已删除但保留身份的 ID 写入 `retiredIds`。
- **向后兼容**：[`ParseSceneDocument()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/SceneSerializer.cpp#L127) 同时接受 `"FlareEngine.Scene"` 与历史格式名 `"SelfEngine.Scene"`（只读兼容）；若遇到 `version == 1` 的老场景，[`InstantiateScene()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/SceneSerializer.cpp#L150) 会先经 [`MigrateV1()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/SceneSerializer.cpp#L57) 自动转换为 v2 组件数组结构再实例化。
- **防损坏原子写盘**：[`SaveScene()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/SceneSerializer.cpp#L247) 先写入同目录临时文件并 `FlushFileBuffers`，再通过 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` 原子替换目标文件，断电或崩溃时绝不会留下半截损坏的 `.scene.json`。

来源：[SceneSerializer.cpp#L94](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/SceneSerializer.cpp#L94) · [SceneSerializer.cpp#L140](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Scenes/SceneSerializer.cpp#L140)

```cpp
// Source/ResourceLayer/Scenes/SceneSerializer.cpp (SerializeScene 关键输出结构)
Json root{
    { "format",     "FlareEngine.Scene" },
    { "version",    2 },
    { "entities",   Json::array() },
    { "retiredIds", Json::array() }
};
// 非默认实体契约字段按需写出，保持 JSON 干净紧凑
if (!record.tag.empty() && record.tag != "Untagged") entityJson["tag"] = record.tag;
if (record.layer != 0)                               entityJson["layer"] = record.layer;
if (record.staticFlags != 0)                         entityJson["staticFlags"] = record.staticFlags;
if (record.prefabLink)                               entityJson["prefab"] = detail::WritePrefabLinkJson(*record.prefabLink);
```

### 2. 替换与叠加场景加载（`SceneManager`）

[`SceneManager`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SceneManager.h#L31) 管理多个逻辑场景记录（`SceneHandle` 全局不回绕，上限 1024 个记录），支持 `SceneLoadMode::Replace`（替换主场景或指定目标场景）和 `SceneLoadMode::Additive`（叠加子场景）：

- **防竞态与过期拦截**：每次请求都会分配新的单调 `request` 代数，并在 `Replace` 模式下登记 `replacementClaims_`。若同一场景又发起了更新的加载请求或被 `Cancel()`，先返回的旧后台结果会在 [`Apply()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SceneManager.cpp#L245) 入口被判定为过期（`staleResults++`）并直接丢弃。
- **零分配原子提交**：[`World::CommitScene()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/WorldSceneCommit.cpp#L7) 会先预分配全部槽位、校验跨场景父子约束与稳定 ID 冲突，再停用旧场景成员并通过 `std::unordered_map::extract` 零分配转移组件节点。所有留在世界里的跨场景 `EntityReference` 若指向已卸载实体，会在提交前自动清零。
- **场景事件广播**：通过 `EventBus` 在 `EventPhase::AfterLogic` 投递类型 ID `16`（`engine.scene_activated`）、`17`（`engine.scene_unloaded`）、`18`（`engine.scene_load_failed`）。

### 3. 动静分离空间索引（`SceneSpatialIndex`）

[`SceneSpatialIndex`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SceneSpatialIndex.h#L16) 负责把实体的局部包围盒随世界矩阵变换为世界 AABB，内部按 `SpatialClass::Static` / `Dynamic` 分别维护两棵 [`spatial::BoundsIndex`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Spatial/BoundsIndex.h#L20)（静态树按需整体 `Build` BVH，动态树增量 `Upsert`/`Remove`）：

来源：[SceneSpatialIndex.cpp#L46](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SceneSpatialIndex.cpp#L46)

```cpp
// Source/FunctionLayer/Runtime/SceneSpatialIndex.cpp
bool SceneSpatialIndex::Synchronize()
{
    const auto revision = world_->SpatialRevision();
    if (!localDirty_ && revision_ == revision && revision != UINT64_MAX) return true;

    for (auto it = entries_.begin(); it != entries_.end(); )
    {
        auto& entry = it->second;
        if (!world_->IsAlive(entry.entity)) { RemoveLeaf(entry); it = entries_.erase(it); continue; }
        // 仅当自身或祖先的 spatialRevision 变化、或启用状态变更时才重算世界包围盒
        const auto version = world_->EntitySpatialRevision(entry.entity);
        const bool enabled = world_->IsEnabled(entry.entity);
        if (entry.dirty || version != entry.revision || version == UINT64_MAX || enabled != entry.enabled)
        {
            entry.revision = version; entry.enabled = enabled;
            dirty.push_back(&entry);
        }
        ++it;
    }
    // ... 若某个对象祖先矩阵极大导致 TransformBounds 溢出，进入 conservative_ 列表保守返回，绝不静默裁掉
}
```

- [`QueryAabb()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SceneSpatialIndex.cpp#L89) 与 [`RaycastCandidates()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/SceneSpatialIndex.cpp#L101) 在查询前自动调用 `Synchronize()`，并支持传入 `SceneHandle` 按场景过滤，返回排序去重后的广相候选实体列表；需要三角形级精确命中时由调用方继续做窄相检测。

### 4. 相机快照与视口拾取射线（`Camera`）

- [`SelectCamera()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/Camera.cpp#L68)：按稳定 ID 升序确定性选取相机——若指定了 `binding` 则精确绑定；否则选择第一个有效且 `primary == true` 的相机（多个主相机时将 `multiplePrimary` 置位以供诊断）。
- [`BuildCameraSnapshot()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/Camera.cpp#L38)：要求相机整条祖先链满足单位缩放（`rigidHierarchy == true`，使用 `InverseRigid` 求 `view`），按右手系 `PerspectiveRH` 生成 `view`/`projection`/`viewProjection` 及其逆矩阵。
- [`ViewportPointToRay()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/Camera.cpp#L123)：把屏幕像素反投影为世界空间射线。实现中特意取深度 `0`（近平面）与 **`0.5f`（内部深度点）** 求方向而非直接取 `1.0f`（远平面），防止远平面非常大时 `float` 投影量化成无穷远。

```cpp
// Source/FunctionLayer/Runtime/Camera.cpp
const float x = 2 * ((pixel.x - viewport.x) / viewport.width) - 1;
const float y = 1 - 2 * ((pixel.y - viewport.y) / viewport.height);
const auto origin = Divide(camera.inverseViewProjection * Vec4{ x, y, 0, 1 });
// 用内部深度点求方向，避免很远的 far 在 float 投影中量化成无穷远。
const auto inside = Divide(camera.inverseViewProjection * Vec4{ x, y, 0.5f, 1 });
```

编辑器漫游视角复用同一模块中的 [`UpdateFreeCamera()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/Camera.cpp#L146)（限制俯仰角在 `±(Pi/2 - 0.01f)` 内，鼠标像素位移不乘 `dt`）。

## 常用接口速查

| 模块 | 接口 | 说明 |
| --- | --- | --- |
| `SceneSerializer` | `SaveScene` / `LoadScene` / `ParseSceneDocument` / `InstantiateScene` | JSON 场景读写、后台解析与主线程实例化（上限 16MB / 10 万实体） |
| `SceneManager` | `RequestFile` / `RequestDocument` / `Cancel` / `Unload` / `ActivateIdle` / `FrameBegin` | 异步或空闲态加载、取消与卸载场景 |
| `SceneSpatialIndex` | `Insert` / `UpdateBounds` / `SetClassification` / `QueryAabb` / `RaycastCandidates` | 动静分离 AABB 树广相查询 |
| `Camera` | `SelectCamera` / `BuildCameraSnapshot` / `ProjectWorldPoint` / `ViewportPointToRay` | 主相机裁决、投影矩阵快照与世界/屏幕坐标互转 |

::: tip 静态对象为什么不该频繁调用 `SetTransform`？
`SceneSpatialIndex` 对 `SpatialClass::Static` 的修改会把 `staticDirty_` 置位，下次查询时会整体重建静态 BVH。运行期需要移动的物体应保持默认的 `SpatialClass::Dynamic`，以便只触发局部动态树 `Upsert`。
:::

::: warning 相机祖先节点不能带有非单位缩放
`BuildFrameData` 提取相机时会沿父链逐个检查 `HasUnitScale(transform->scale)`，任意祖先缩放不为 `(1, 1, 1)` 都会把 `rigidHierarchy` 置为 `false`，导致 `BuildCameraSnapshot()` 返回 `CameraError::InvalidTransform`。挂载相机到角色或载具下时，请确保父链保持纯刚体变换。
:::
