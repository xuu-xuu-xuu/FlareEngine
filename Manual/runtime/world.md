# World 与组件管理接口

这篇文档介绍 Flare 运行时最核心的容器 `flare::World`：它如何统一管理实体、层级、组件、对象堆与安全边界，并详细说明灯光组件（`GetLight` / `SetLight` / `RemoveLight`）的读写与校验契约。

## 一句话定位

`flare::World` 是场景数据的**唯一权威所有者**，持有所有实体槽位、内置与扩展组件池、场景环境设置、空间修订号以及逻辑对象堆（`ObjectHeap`），并支持安全的深拷贝快照与原子场景提交（`CommitScene`）。

## 架构总览

```text
                       ┌──────────────────────────────────────┐
                       │             flare::World             │
                       ├──────────────────────────────────────┤
  EntityHandle ───────▶│ slots_ (deque<Slot>)  + freeSlots_   │
  StableEntityId ─────▶│ stableIds_ / usedStableIds_          │
                       ├──────────────────────────────────────┤
                       │ 内置池: transforms_ / cameras_ /     │
                       │         meshes_ / lights_ / scripts_ │
                       │ 扩展池: registeredPools_[TypeId]     │
                       ├──────────────────────────────────────┤
  帧内 / 回调修改 ────▶│ commands_ (本帧阶段边界)             │
                       │ nextFrameCommands_ (下一帧 Begin)    │
                       └──────────────────────────────────────┘
                                 │                  │
                         深拷贝 (Play 模式)    CommitScene (场景切换)
```

## World

[`flare::World`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.h#L63) 负责维护实体句柄代数、父子层级、内置/扩展组件池以及帧边界命令队列。

### 核心设计特点

1. **严格的安全点约束**：
   - `BeginFrame()` 会把 `nextFrameCommands_` 合入 `commands_` 并执行，然后打开 `frameActive_ = true`。
   - `FinishFrame()` 提交剩余的 `commands_` 并关闭帧状态。
   - **深拷贝** `World(const World& other)`（例如编辑器点击 Play 时从编辑世界克隆运行世界）要求源世界处于安全静止点（不在帧内、不在遍历中、无未决命令），拷贝会保留所有 `StableEntityId`、`Tag`、`Layer`、`StaticFlags`、`PrefabLink`、层级及全部组件属性，但**重新发放运行句柄 `EntityHandle`**，防止旧世界的句柄误操作新世界。
2. **身份防重用保护**：
   - `usedStableIds_` 记录本世界历史出现过的所有稳定 ID（包括已删除实体的 ID，作为 `retiredIds` 持久化到场景 JSON），避免旧引用意外指向新创建实体。
   - 仅当撤销“本次会话刚新建、从未保存、随后又删除”的实体时，才允许调用 `ForgetStableId()` 回收预留。
3. **内置灯光组件封装**：
   - 内部使用 [`LightRuntimeComponent`](light.md#lightruntimecomponent) 保存组件身份和灯光参数，对外向调用方暴露纯值结构 [`LightComponent`](light.md#lightcomponent)，屏蔽组件槽位细节。

### 核心源码片段：实体查找与深拷贝

来源：[World.cpp#L41](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L41) · [World.cpp#L149](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L149)

```cpp
// Source/FunctionLayer/Runtime/World.cpp
World::World(const World& other)
{
    if (other.IsInFrame() || other.IsIterating() || other.HasPendingCommands())
        throw std::logic_error("World.SnapshotPending");
    for (const auto source : other.Entities())
    {
        const auto* record = other.Get(source);
        const auto target = Create(record->name, record->stableId);
        SetEnabled(target, record->enabled);
        if (record->tag != "Untagged") SetTag(target, record->tag);
        if (record->layer != 0)        SetLayer(target, record->layer);
        if (record->staticFlags != 0)  SetStaticFlags(target, record->staticFlags);
        if (record->prefabLink)        SetPrefabLink(target, record->prefabLink);
    }
    // 第二遍恢复父子关系，第三遍经 CloneComponentProperties 克隆全部组件属性
    // ...
    usedStableIds_ = other.usedStableIds_;
    sceneEnvironment_ = other.sceneEnvironment_;
}
```

### GetLight

[`const LightComponent* GetLight(EntityHandle entity) const`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.h#L122) 返回实体灯光参数的只读指针。当实体已失效、处于待销毁/未激活状态，或该实体未挂载灯光组件时，返回 `nullptr`。[查看实现](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L449)

### SetLight

[`bool SetLight(EntityHandle entity, const LightComponent& component)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.h#L123) 为实体新增或更新灯光参数。写入前会在入口处完成全量合法性校验，任何一项不满足都立即返回 `false`（设置 `LastError() == "World.InvalidComponent"`）而不污染世界状态。[查看实现](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L450)

```cpp
// Source/FunctionLayer/Runtime/World.cpp
bool World::SetLight(EntityHandle entity, const LightComponent& component)
{
    if (!IsKnown(entity)) return Fail("World.InvalidEntity");
    if (!IsFinite(component.color) || !IsFinite(component.intensity) || component.intensity < 0
        || component.color.x < 0 || component.color.y < 0 || component.color.z < 0
        || !IsFinite(component.range) || component.range <= 0
        || LightTypeName(component.type).empty()
        || !IsFinite(component.innerAngle) || !IsFinite(component.outerAngle)
        || component.innerAngle < 0 || component.innerAngle > component.outerAngle
        || component.outerAngle < .01f || component.outerAngle > 1.55f)
        return Fail("World.InvalidComponent");

    // 首次挂载属于结构变更（nextFrame=true），更新已有灯光参数则走当前阶段队列
    return DeferEntity(entity, [entity, component](World& world) {
        if (!world.IsAlive(entity)) return world.Fail("World.InvalidEntity");
        return world.Store(world.lights_, entity, component);
    }, lights_.Get(entity.index) == nullptr);
}
```

**校验条件一目了然**：
- 实体必须是已知有效实体（`IsKnown`）；
- `color` 各分量和 `intensity` 必须有限且 `>= 0`；
- `range` 必须有限且严格 `> 0`；
- `type` 必须是合法的 `LightType`（`Directional` / `Point` / `Spot`）；
- 聚光灯半角（弧度）须满足 `0 <= innerAngle <= outerAngle` 且 `0.01f <= outerAngle <= 1.55f`。

下面的小函数展示安全的“读-改-写”模式：

```cpp
bool SetIntensity(flare::World& world, flare::EntityHandle entity, float intensity)
{
    const auto* current = world.GetLight(entity);
    if (!current) return false;

    auto light = *current;
    light.intensity = intensity;
    return world.SetLight(entity, light);
}
```

### RemoveLight

[`bool RemoveLight(EntityHandle entity)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.h#L124) 移除指定实体的灯光组件。对有效但原本就没有灯光的实体调用同样会返回 `true`（幂等）；仅当传入未知/失效实体时返回 `false`。[查看实现](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L459)

## 常用组件读写接口速查

| 组件 | 读取 | 写入 / 新增 | 移除 |
| --- | --- | --- | --- |
| 变换 | `GetTransform(e)` / `GetKnownTransform(e)` | `SetTransform(e, t, mode)` | 不可移除（必选组件） |
| 相机 | `GetCamera(e)` | `SetCamera(e, cam)` | `RemoveCamera(e)` |
| 网格 | `GetMesh(e)` | `SetMesh(e, mesh)` | `RemoveMesh(e)` |
| 灯光 | `GetLight(e)` | `SetLight(e, light)` | `RemoveLight(e)` |
| 脚本 | `GetScriptComponent(ch)` | `AttachScriptComponent(e, ctrlId, cb, enabled)` | `RemoveComponent(ch)` |
| 扩展组件 | `GetRegisteredComponent<T>(ch)` | `AttachRegisteredComponent<T>` / `UpdateRegisteredComponent<T>` | `RemoveComponent(ch)` |

::: tip `SetTransform` 的 `TransformWriteMode`
普通编辑使用默认的 `TransformWriteMode::Normalize`，会自动规范化四元数；在加载场景或克隆快照时，引擎使用 `TransformWriteMode::PreserveUnit`，若四元数长度平方与 `1` 的误差不超过 `1e-6f` 则原样保留 float 位值，保证“保存-再打开”完全无浮点漂移。
:::

::: warning 首次添加组件在同帧内可能读不到
留意 `SetLight` / `SetCamera` / `SetMesh` 最后传给 `DeferEntity` 的 `nextFrame` 参数：当实体原本没有该组件时，`nextFrame` 为 `true`。如果在帧内刚调完 `SetLight` 就紧接着调 `GetLight`，返回仍可能是 `nullptr`——它会等到下一次 `BeginFrame()`（或空闲态 `FlushCommands()`）才真正入池。
:::
