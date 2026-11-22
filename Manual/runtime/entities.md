# 实体、层级与组件存储

这篇文档讲清楚 Flare 引擎里一个实体从创建到销毁是怎么存、怎么防悬空句柄、怎么算父子变换，以及新增的 `Tag` / `Layer` / `EntityStaticFlags` 契约字段如何流向物理、导航和渲染系统。

## 一句话定位

实体本身只是一个带代数校验的轻量槽位（`EntityRecord`），真正的数据按组件类型分池存放在 [`flare::World`](world.md#world) 中；所有结构修改在帧内或回调中自动排队，统一在安全边界提交，杜绝遍历时迭代器失效与悬空访问。

## 核心数据流与状态流转

```text
World::Create("Box")
  │  分配槽位 + 全局不回绕 generation + 分配/校验 StableEntityId
  ▼
[Reserved] ──(FlushCommands / 下一帧 BeginFrame)──▶ [Alive] (挂载 TransformComponent)
                                                      │
     EntityRecord: tag / layer(0..31) / staticFlags   │  World::Destroy(e)
       ├──▶ layer ──▶ ScenePhysics (1u << layer)      ▼
       ├──▶ staticFlags.Batching ──▶ FrameData     [DestroyPending] (立即逻辑失效)
       ├──▶ staticFlags.Navigation ──▶ NavMesh        │
       └──▶ staticFlags.GI ──▶ DiffuseProbe           ▼ (安全边界 DestroyNow)
                                                   [Free] (倒序卸载组件，槽位进 freeSlots_)
```

## 核心源码拆解

### 1. 运行句柄与稳定 ID

引擎同时维护两套身份：**运行期**用 `EntityHandle`（O(1) 槽位索引 + 代数校验），**持久化与跨会话**（场景文件、预制体、撤销栈、脚本引用）用 64 位 `StableEntityId`。槽位复用后 `generation` 必然递增且永不回绕，旧句柄会立刻被 `IsAlive()` 拦下。

来源：[RuntimeHandles.h#L6](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/RuntimeHandles.h#L6) · [World.cpp#L139](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L139)

```cpp
// Source/FunctionLayer/Runtime/RuntimeHandles.h
struct EntityHandle
{
    std::uint32_t index = UINT32_MAX, generation = 0;
    constexpr std::uint64_t Pack() const { return (std::uint64_t{generation} << 32) | index; }
    static constexpr EntityHandle FromPacked(std::uint64_t value)
    {
        return { static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32) };
    }
    explicit constexpr operator bool() const { return index != UINT32_MAX && generation != 0; }
};

// Source/FunctionLayer/Runtime/World.cpp
bool World::IsAllocated(EntityHandle entity) const
{
    return entity && entity.index < slots_.size()
        && slots_[entity.index].generation == entity.generation
        && slots_[entity.index].state != SlotState::Free;
}

EntityHandle World::FindByStableId(StableEntityId id) const
{
    const auto it = stableIds_.find(id);
    return it != stableIds_.end() && IsAlive(it->second) ? it->second : EntityHandle{};
}
```

### 2. 实体契约字段：`Tag`、`Layer` 与 `EntityStaticFlags`

除了名称、启用状态和父实体之外，`EntityRecord` 还承载三个统一契约字段以及可选的 `PrefabLink`。它们直接保存在槽位记录上，序列化进场景 JSON（仅非默认值写出）并自动跟随预制体同步与世界深拷贝。

来源：[World.h#L25](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.h#L25) · [World.cpp#L295](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L295)

```cpp
// Source/FunctionLayer/Runtime/World.h
namespace EntityStaticFlags
{
    constexpr std::uint32_t None               = 0u;
    constexpr std::uint32_t Batching           = 1u << 0; // 参与静态合批与静态阴影缓存
    constexpr std::uint32_t Navigation         = 1u << 1; // 参与 NavMesh 静态三角形采集
    constexpr std::uint32_t GlobalIllumination = 1u << 2; // 参与漫反射球谐探针烘焙
    constexpr std::uint32_t Occluder           = 1u << 3; // 标记为静态遮挡体
    constexpr std::uint32_t All                = Batching | Navigation | GlobalIllumination | Occluder;
    constexpr std::uint32_t Explicit           = 1u << 7; // 用户显式声明动静契约，覆盖默认启发式
}

struct EntityRecord
{
    StableEntityId stableId = 0;
    std::string name;
    EntityHandle parent{};
    bool enabled = true;
    std::string tag = "Untagged";     // 最长 256 字节；空串自动归一化为 "Untagged"
    std::uint32_t layer = 0;          // 合法范围 0..31，超出返回 Metadata.InvalidValue
    std::uint32_t staticFlags = 0;    // 写入时按 (All | Explicit) 掩码截断
    std::optional<PrefabLink> prefabLink;
};
```

#### `Explicit` 标志位如何协调“自动推导”与“显式声明”？

各下游子系统对 `staticFlags` 采用同一套约定：**当第 7 位 `Explicit` 为 0 时，引擎走默认动态组件启发式**（若实体或其祖先挂有活跃的 `Script`、`CharacterSettingsComponent`、`ActorSettingsComponent`、`NavigationAgentComponent` 或 `body.mass > 0` 的动态刚体，则判定为动态）；**一旦 `Explicit` 置位，则严格以对应功能位为准，完全覆盖启发式**。

| 实体字段 / 位 | 下游消费模块 | 具体行为 | 源码位置 |
| --- | --- | --- | --- |
| `layer` (`0..31`) | `ScenePhysics` / `PhysicsWorld` | 映射为 32 位碰撞层掩码 `1u << layer`，参与碰撞对过滤与射线检测掩码 | [ScenePhysics.cpp#L51](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/ScenePhysics.cpp#L51) |
| `staticFlags & Batching` | `BuildFrameData` | 沿祖先链判定 `mesh.staticGeometry`，控制静态合批与静态阴影缓存 | [FrameData.cpp#L31](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FrameData.cpp#L31) |
| `staticFlags & Navigation` | `SceneNavigation` | 过滤哪些网格的三角形进入导航网格（`.senav`）离线/编辑器烘焙 | [SceneNavigation.cpp#L86](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/SceneNavigation.cpp#L86) |
| `staticFlags & GlobalIllumination` | `DiffuseProbePreparation` | 过滤哪些静态几何体参与漫反射球谐探针（Diffuse SH）烘焙 | [DiffuseProbePreparation.cpp#L27](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/DiffuseProbePreparation.cpp#L27) |
| `tag` | 脚本 / 编辑器 / 预制体 | 逻辑分组标识（默认 `"Untagged"`），支持预制体属性级覆盖同步 | [World.cpp#L295](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L295) |

### 3. 层级变换与重挂接

`SetParent()` 只修改父指针并维持实体的局部变换不变；同时检查跨场景挂接和环路（`World.ParentCycle`），并在生效时调用 `TouchSpatial()` 递增空间版本号与 `RefreshComponents()` 同步祖先禁用状态。

来源：[World.cpp#L364](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L364) · [TransformDecomposition.h#L9](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Math/TransformDecomposition.h#L9)

```cpp
// Source/FunctionLayer/Runtime/World.cpp
bool World::ParentNow(EntityHandle entity, EntityHandle parent)
{
    if (!IsAlive(entity) || (parent && !IsAlive(parent))) return Fail("World.InvalidEntity");
    if (sceneParent_ && !sceneParent_(entity, parent))    return Fail("Scene.CrossSceneParent");
    for (auto cursor = parent; cursor; cursor = Get(cursor)->parent)
        if (cursor == entity) return Fail("World.ParentCycle");
    slots_[entity.index].record.parent = parent;
    TouchSpatial(entity);
    return RefreshComponents();
}

Mat4 World::GetWorldMatrix(EntityHandle entity) const
{
    Mat4 result;
    if (!IsAlive(entity)) { Fail("World.InvalidEntity"); return result; }
    for (auto cursor = entity; cursor; cursor = Get(cursor)->parent)
        result = GetTransform(cursor)->Matrix() * result;
    return result;
}
```

当编辑器需要“改变父节点但保持物体在世界中的位置/朝向/缩放不动”时，先用新父世界矩阵的逆乘上旧世界矩阵，再调用 [`TryDecomposeTrs()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Math/TransformDecomposition.h#L9) 分解回局部 TRS。`TryDecomposeTrs()` 会严格校验正交性与重建误差，若父节点非等比缩放导致剪切（shear），会直接拒绝而不静默丢弃形变。

### 4. 组件存储：内置池 vs 扩展注册池

`World` 把组件分为两类存储，都避免对象切片（slicing）：

- **内置组件池**：`Transform`、`Camera`、`Mesh`、`Light` 以实体槽位索引 `owner.index` 存入 `ComponentPool<T>`；`ScriptComponent` 支持单实体多挂，以组件槽位索引 `handle.index` 存入 `scripts_`。
- **扩展组件池**：所有 `TypeId >= 1024` 的组件（含 11 个引擎特性组件与用户自定义组件）通过 [`AttachRegisteredComponent<T>()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.h#L134) 存入以类型 ID 索引的 [`TypedRegisteredComponentPool<T>`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/RegisteredComponentPool.h#L23)，基类 [`RegisteredComponentPool`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/RegisteredComponentPool.h#L10) 只擦除池操作接口，池内保存真实派生对象。

组件身份与生命周期状态由 [`ComponentBase`](component-base.md#componentbase) 统一维护。

### 5. 安全修改、命令队列与两阶段销毁

帧循环期间（`BeginFrame()` ~ `FinishFrame()`）或处于 `ForEach()` / 生命周期回调中时，`Defer()` 会把结构变更推入 `commands_` 或 `nextFrameCommands_`：

- **新建与结构修改**（`Create`、`SetParent`、首次添加组件）默认进入 `nextFrameCommands_`，保证同一帧内的查询看到一致快照，直到下一次 `BeginFrame()` 或空闲时 `FlushCommands()` 才生效。
- **销毁子树**（[`World::Destroy()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/World.cpp#L250)）：第一步 `MarkDestroy()` 立刻沿父子关系找出整棵子树并置为 `DestroyPending`（此时 `IsAlive()` 立即返回 `false`，避免同帧逻辑继续访问已删对象）；第二步把 `DestroyNow()` 排入命令队列，待安全边界时**按子树逆序（先子后父）、组件逆序**依次触发 `OnDisable` / `OnDetach` 并归还槽位。

## 常用接口速查

| 目的 | 接口 | 说明 |
| --- | --- | --- |
| 创建 / 销毁实体 | `Create(name, stableId = 0)` / `Destroy(e)` | 传入 `stableId = 0` 自动分配全局唯一 ID；已退役的 ID 不能复用来创建 |
| 句柄校验与反查 | `IsAlive(e)` / `IsPending(e)` / `FindByStableId(id)` | 仅活跃实体对 `IsAlive` 返回 `true`；排队未激活实体用 `IsPending` |
| 修改实体契约 | `SetTag` / `SetLayer` / `SetStaticFlags` | `SetLayer` 和 `SetStaticFlags` 生效时会同步递增 `spatialRevision_` |
| 调整组件展示顺序 | `MoveComponent(owner, type, delta)` | 移动非 `Transform`/`Script` 组件在实体组件列表中的位置（Inspector 排序） |
| 挂载扩展组件 | `AttachRegisteredComponent<T>(owner, val, enabled, multiple)` | 编译期校验 `T : ComponentBase` 且 `T::TypeId >= 1024` |
| 提交延迟命令 | `FlushCommands()` / `FlushPhaseCommands()` | 帧外调用 `FlushCommands()` 提交全部队列；帧内阶段边界只提交 `commands_` |

::: tip 什么时候用 `GetKnown*` 而不是 `Get*`？
在刚刚调用 `Create()` 后、尚未到达 `FlushCommands()` 时，实体处于 `Reserved` 状态，`Get()` / `GetTransform()` 会返回 `nullptr`。若在同一批初始化代码里需要读取刚写入的预备状态，可使用 `GetKnown()`、`GetKnownTransform()` 或 `GetKnownWorldMatrix()`。
:::

::: warning 不要长期缓存 `EntityHandle` 到磁盘或跨场景
`World` 深拷贝（如编辑器进入 Play 模式克隆编辑世界）或场景重载时，实体的 `StableEntityId` 不变，但所有运行句柄 `EntityHandle` 都会重新分配。跨快照、撤销记录或资产引用请一律保存 `StableEntityId`，用时再经 `FindByStableId()` 解析。
:::
