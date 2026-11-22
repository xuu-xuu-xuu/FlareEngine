# 物理、射线、碰撞与角色控制

本页是 `FlareEngine` 命名空间下**物理与角色控制全部公开 C# API** 的参考字典，涵盖全局射线检测、真实流形碰撞回调、动态/静态刚体施力与瞬移，以及胶囊体角色控制器。

## 开始前先记住这 4 条物理约定

1. **单位与默认重力**：距离与尺寸为**米 (`m`)**，时间为**秒 (`s`)**，质量为**千克 (`kg`)**，力为**牛顿 (`N`)**，冲量为**牛顿·秒 (`N·s`)**，角速度为**弧度/秒 (`rad/s`)**；默认重力为 `(0, -9.81f, 0)`。
2. **用 `Mass` 区分动态与静态刚体**：Flare 没有单独的刚体运动类型枚举——**`Mass > 0` 为动态刚体**（受重力、碰撞推力与施力影响，静止一段时间自动休眠），**`Mass == 0` 为静态刚体**（固定不动的地面/墙壁/触发区，不可对其施力或设置速度）。
3. **碰撞层与 32 位位掩码**：实体图层 `Entity.Layer` 为 `0..31` 的整数索引；物理检测与过滤使用 **32 位位掩码（bitmask）**。编辑器挂载的物理体会自动映射为 `1u << entity.Layer`；脚本动态创建刚体时，`Layer` 与 `Mask` 字段直接填写位掩码（默认 `Layer = 1u` 即图层 0，`Mask = uint.MaxValue` 即全部图层）。
4. **第 0 帧 `F_Awake` / `F_Start` 即可立即查询**：调用 `F_Physics.Raycast`、`GetComponent<F_Rigidbody>()` 或 `GetComponent<F_CharacterController>()` 时，原生桥接会先自动将当前场景里的物理组件同步到物理世界，**不需要等待第一帧 `F_FixedUpdate` 跑完**即可直接命中地面、墙体与实体。

### 固定物理步执行流程

```text
  F_Update()（渲染帧）           读取键鼠输入（如保存跳跃请求）、发起射线检测
         │
         ▼
  F_FixedUpdate(fixedDelta)     按固定步长（默认 60Hz）调用刚体 AddForce / 角色 Move
         │
         ▼
  原生 PhysicsWorld::Step       同步变换 ─▶ 积分速度与重力 ─▶ 流形碰撞求解 ─▶ 回写实体位姿
         │
         ▼
  AfterPhysics 回调派发         触发 F_CollisionEnter/Stay/Exit 与 F_TriggerEnter/Stay/Exit
```

### 公开类型与向下兼容名速查

新脚本推荐统一使用 **`F_` 前缀**，代码补全更集中；旧版无前缀类型保持完全兼容（通过继承或隐式转换互通）：

| 推荐类型（`F_` 前缀） | 向下兼容名 / 别名 | 定位 |
| :--- | :--- | :--- |
| `F_Physics` | `Physics`（`F_Behaviour.Physics` 实例属性） | 射线检测、动态创建/查找刚体、调试线框开关 |
| `F_RaycastHit` / `F_RayHit` | `RayHit` | 射线检测最近命中结果（实体、刚体、交点、法线、距离） |
| `F_Contact` | `PhysicsContact`（底层事件总线快照） | 真实物理流形碰撞信息（对方实体、接触点、法线、冲量） |
| `F_Rigidbody` / `F_RigidBody` | `RigidBody` | 刚体运行时组件代理（速度、施力、冲量、瞬移、触发器） |
| `F_BoxBodyDescription` | `BoxBodyDescription` | 盒体刚体创建参数描述 |
| `F_SphereBodyDescription` | `SphereBodyDescription` | 球体刚体 / 触发器创建参数描述 |
| `F_RigidBodyState` | `RigidBodyState` | 刚体动力学瞬时状态快照 |
| `F_CharacterController` | `CharacterController` | 运动学直立胶囊角色控制器（斜坡滑移、跳跃、推箱子） |
| `F_CharacterDescription` | `CharacterDescription` | 角色胶囊体尺寸、步速、跳跃与推力上限参数 |
| `F_CharacterState` | `CharacterState` | 角色瞬时位置、竖直速度与着地状态快照 |

---

## F_Physics

静态门面类，提供全局射线检测、刚体创建/查找与调试线框控制（在任意 `F_Behaviour` 回调中可直接调用）。底层委托给当前活动会话的 `Physics` 实例服务。

> 源码位置：[FlareErgonomics.cs#L557](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L557) · [Physics.cs#L199](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Physics.cs#L199)

### 静态方法一览

| 方法签名 | 返回值 | 说明 |
| :--- | :--- | :--- |
| `Raycast(Vector3 origin, Vector3 direction, float distance, uint mask = uint.MaxValue)` | `F_RayHit?` | 发射射线并返回最近命中信息，未命中返回 `null` |
| `Raycast(Vector3 origin, Vector3 direction, float distance, out F_RaycastHit hit, uint mask = uint.MaxValue)` | `bool` | Unity 风格 `out` 重载，命中返回 `true` 并输出 `F_RaycastHit` |
| `Raycast(Vector3 origin, Vector3 direction, float distance, out F_RayHit hit, uint mask = uint.MaxValue)` | `bool` | `out` 重载，命中返回 `true` 并输出 `F_RayHit` |
| `CreateBox(F_Entity entity, F_BoxBodyDescription description)` | `F_RigidBody?` | 在指定根实体上动态创建盒体刚体 |
| `CreateSphere(F_Entity entity, F_SphereBodyDescription description)` | `F_RigidBody?` | 在指定根实体上动态创建球体刚体或球形触发器 |
| `GetBody(F_Entity entity)` | `F_RigidBody?` | 查找实体上已有的刚体代理（含编辑器组件），无刚体返回 `null` |
| `SetDebugDrawing(bool enabled)` | `bool` | 开关视口物理碰撞体与接触点线框绘制，返回修改前的旧状态 |

### Raycast

向物理世界发射一条有限长度射线，返回距离起点最近的**非触发器**刚体命中结果。

```csharp
public static F_RayHit? Raycast(
    Vector3 origin,
    Vector3 direction,
    float distance,
    uint mask = uint.MaxValue);

public static bool Raycast(
    Vector3 origin,
    Vector3 direction,
    float distance,
    out F_RaycastHit hit,
    uint mask = uint.MaxValue);

public static bool Raycast(
    Vector3 origin,
    Vector3 direction,
    float distance,
    out F_RayHit hit,
    uint mask = uint.MaxValue);
```

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `origin` | `Vector3` | 必填 | 射线起点的世界坐标（米）。 |
| `direction` | `Vector3` | 必填 | 射线方向（世界空间）。引擎内部会自动归一化，但**不能为零向量**（模长平方小于 `1e-10f` 时直接返回未命中）。 |
| `distance` | `float` | 必填 | 最大检测距离（米）。**必须严格大于 `0` 且为有限值**，传入 `<= 0`、`NaN` 或 `Infinity` 会抛出 `EngineException("Bridge.InvalidArgument")`。 |
| `hit` | `out F_RaycastHit` / `out F_RayHit` | — | 仅 `bool` 重载包含。命中时写入最近命中信息，未命中时为 `default`。 |
| `mask` | `uint` | `uint.MaxValue` | 32 位碰撞层位掩码，仅检测满足 `(mask & body.Layer) != 0` 的刚体。 |

**返回值**：
- `F_RayHit?` 重载：命中返回最近 `F_RayHit`，未命中或当前无活动脚本会话返回 `null`。
- `bool` 重载：命中返回 `true`，未命中返回 `false`。

::: tip 按实体图层 `Entity.Layer` 构造 `mask`
场景中每个实体的图层 `Entity.Layer` 是 `0..31` 的整数，映射到物理层位掩码为 `1u << entity.Layer`：
- **只检测图层 3**（例如 `Enemy` 层）：`uint mask = 1u << 3;`
- **同时检测图层 0（地面）与图层 3（敌人）**：`uint mask = (1u << 0) | (1u << 3);`
- **检测除图层 2（玩家自身）以外的所有层**：`uint mask = ~(1u << 2);`
- **忽略触发器**：`Raycast` 自动跳过所有 `IsTrigger == true` 的触发器，不会误命中触发区域。
:::

::: info 为什么第 0 帧 `F_Awake()` / `F_Start()` 也能直接命中？
原生接口 [`flare_physics_raycast`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativePhysicsExports.inl#L203) 在执行射线求交前，会先主动同步场景中全部已启用的 `PhysicsBody` 与 `ColliderSettings` 组件。因此你可以放心在 `F_Awake()` 或 `F_Start()` 里向下发射射线把角色或道具自动贴合到地面上。
:::

```csharp
using System.Numerics;
using FlareEngine;

public class GroundSnapper : F_Behaviour
{
    [F_Expose] public int GroundLayer = 0;
    [F_Expose] public float HalfHeight = 0.5f;

    public override void F_Start()
    {
        // 第 0 帧即可直接发射射线，只检测 GroundLayer 图层
        uint groundMask = 1u << GroundLayer;
        Vector3 start = Entity.WorldPosition + Vector3.UnitY * 0.2f;

        if (F_Physics.Raycast(start, -Vector3.UnitY, 50f, out F_RaycastHit hit, groundMask))
        {
            Entity.WorldPosition = hit.Point + Vector3.UnitY * HalfHeight;
            F_Log.Info($"已贴地到 {hit.Entity.Name}，高度 Y = {hit.Point.Y:F2}m");
        }
    }
}
```

### CreateBox / CreateSphere

在运行时动态为实体挂载盒体或球体碰撞刚体。

```csharp
public static F_RigidBody? CreateBox(F_Entity entity, F_BoxBodyDescription description);
public static F_RigidBody? CreateSphere(F_Entity entity, F_SphereBodyDescription description);
```

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `entity` | `F_Entity` | 目标场景实体（必须为存活的根实体，且尚未挂载任何刚体、碰撞体或角色控制器）。 |
| `description` | `F_BoxBodyDescription` / `F_SphereBodyDescription` | 形状尺寸、质量、摩擦、弹性、图层掩码及触发器参数。 |

**返回值**：新创建的 `F_RigidBody`（运行时实际类型为 `RigidBody`，可直接转换为 `F_Rigidbody`）；当前无活动会话时返回 `null`。

::: warning 动态创建刚体的前置条件
通过脚本 `CreateBox` / `CreateSphere` 创建刚体时，目标实体必须满足：
1. 是**根实体**（`entity.Parent is null`）；
2. 当前**没有**其他刚体、编辑器碰撞体或角色控制器，否则抛出 `EngineException("Bridge.InvalidBodyAttachment")`；
3. 实体的三轴缩放 `LocalScale` 绝对值会自动乘算到盒体 `HalfExtents`（逐轴相乘）或球体 `Radius`（乘最大轴缩放）。
:::

```csharp
// 在当前实体上创建一个弹性动态球体（半径 0.4m、质量 2kg、弹性 0.7）
var desc = F_SphereBodyDescription.Dynamic;
desc.Radius = 0.4f;
desc.Mass = 2f;
desc.Restitution = 0.7f;
desc.Layer = 1u << Entity.Layer;

F_RigidBody? ball = F_Physics.CreateSphere(Entity, desc);
ball?.AddImpulse(new Vector3(0f, 8f, -4f));
```

### GetBody

查找并返回指定实体上已存在的刚体代理（兼容编辑器挂载的 `PhysicsBody` / `ColliderSettings` 组件和脚本动态创建的刚体）。

```csharp
public static F_RigidBody? GetBody(F_Entity entity);
```

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `entity` | `F_Entity` | 要查询的场景实体。 |

**返回值**：找到时返回 `F_RigidBody`（可转换为 `F_Rigidbody`）；实体未挂载刚体、尚未提交或无活动会话时返回 `null`。

### SetDebugDrawing

实时开关视口中的物理碰撞线框调试显示，便于检查碰撞体尺寸、休眠状态与接触点。

```csharp
public static bool SetDebugDrawing(bool enabled);
```

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `enabled` | `bool` | `true` 开启碰撞线框显示，`false` 关闭。 |

**返回值**：修改**之前**的开关状态（`bool`）。

| 调试线框颜色 | 对应物理状态 |
| :--- | :--- |
| 绿色 | 活跃中的动态刚体（`Mass > 0` 且未休眠） |
| 蓝色 | 已休眠的动态刚体（`Sleeping == true`） |
| 灰色 | 静态刚体（`Mass == 0`） |
| 青色 | 触发器（`IsTrigger == true`） |
| 黄色 | 胶囊体角色控制器（`F_CharacterController`） |
| 红色法线 + 青色十字 | 当前物理步的实时流形接触点与接触法线 |

```csharp
// 按 F3 切换物理调试线框显示
private bool _debugDraw;

public override void F_Update()
{
    if (F_Input.KeyDown(F_Key.F3))
    {
        _debugDraw = !_debugDraw;
        F_Physics.SetDebugDrawing(_debugDraw);
    }
}
```

### Physics 实例服务扩展方法

在 `F_Behaviour` 子类中，还可通过基类属性 `Physics`（类型为 `FlareEngine.Physics`，即 `Scripts.Session.Physics`）访问与 `F_Physics` 对应的实例方法，以及以下底层专用接口：

| 成员签名（`Physics` 实例） | 说明 |
| :--- | :--- |
| `RigidBody CreateBox(F_Entity entity, BoxBodyDescription description, bool isTrigger)` | 一步创建**盒形触发器**的便捷重载（`F_BoxBodyDescription` 无 `IsTrigger` 字段时推荐用此方法，或在 `CreateBox` 后设置 `body.IsTrigger = true`）。 |
| `IReadOnlyList<PhysicsContact> DrainContacts()` | 取出并清空原生缓存的底层接触事件列表。常规游戏脚本**不要手动调用**，否则会截走本帧分发给 `F_CollisionEnter/Stay/Exit` 的事件。 |
| `IReadOnlyList<PhysicsTrigger> DrainTriggers()` | 取出并清空原生缓存的底层触发器事件列表。同上，仅在无 `ScriptSystem` 自动派发时使用。 |
| `void Step(double seconds)` | 手动推进单步物理模拟（仅在非托管时钟模式 `simulatePhysics: false` 下可用；正常运行时调用抛出 `InvalidOperationException("Script.PhysicsClockOwned")`）。 |
| `uint Advance(double frameSeconds)` | 按帧间隔推进固定步累计模拟（同样仅非托管时钟模式可用），返回实际执行的步数；超预算丢弃的时长记录在 `DroppedSeconds`（`double`）中。 |

---

## F_RaycastHit / F_RayHit

物理射线检测的最近命中结果只读值类型（`readonly record struct`）。`F_RaycastHit`（Unity 命名风格）、`F_RayHit`（简短命名）与 `RayHit`（向下兼容无前缀名）结构完全一致，三者支持隐式互转。

> 源码位置：[Physics.cs#L81](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Physics.cs#L81)

```csharp
public readonly record struct F_RaycastHit(
    Entity Entity,
    F_RigidBody Body,
    Vector3 Point,
    Vector3 Normal,
    float Distance);

public readonly record struct F_RayHit(
    Entity Entity,
    F_RigidBody Body,
    Vector3 Point,
    Vector3 Normal,
    float Distance);
```

### 属性表

| 属性名 | 类型 | 单位 / 范围 | 说明 |
| :--- | :--- | :--- | :--- |
| `Entity` | `Entity`（继承自 `F_Entity`） | 非空有效实体 | 被射线命中的场景实体，可直接读写 `Name`、`Tag`、`Layer` 或调用 `GetComponent<T>()`。 |
| `Body` | `F_RigidBody`（`RayHit` 中为 `RigidBody`） | 非空刚体代理 | 被命中的刚体代理，可直接对其调用 `ApplyImpulse(impulse, hit.Point)` 实现精准击打。 |
| `Point` | `Vector3` | 世界坐标（米 `m`） | 射线与碰撞体表面的精确交点。 |
| `Normal` | `Vector3` | 单位向量（长度 `1`） | 命中点处的世界空间表面外法线（朝向碰撞体外部）。 |
| `Distance` | `float` | 米（`m`），`[0, maxDistance]` | 从射线起点 `origin` 到交点 `Point` 的直线距离。 |

::: tip 射线起点已经在碰撞体内部时怎么办？
当射线起点位于盒体或球体内部时，射线求交会自动返回**穿出表面**的距离与法线，不会直接漏检。发射枪口或脚底射线时，仍建议沿发射方向稍微外移起点（如 `+ Vector3.UnitY * 0.05f`）或通过 `mask` 排除自身图层。
:::

### 示例：鼠标点选并沿表面法线贴面放置物体

结合 `F_Camera.ScreenPointToRay`、`F_Physics.Raycast` 与 `F_Scene.Instantiate`，左键点击任意墙面、斜坡或桌面即可把模板物体精准贴面摆放：

```csharp
using System.Numerics;
using FlareEngine;

public class SurfacePlacer : F_Behaviour
{
    [F_Header("放置设置")]
    [F_Expose] public F_Entity? PropTemplate;
    [F_Expose, F_Range(0f, 2f)] public float SurfaceOffset = 0.25f;
    [F_Expose, F_Range(1f, 200f)] public float MaxPlaceDistance = 80f;

    public override void F_Update()
    {
        if (!F_Input.MouseDown(F_MouseButton.Left)) return;
        if (PropTemplate is not { IsValid: true }) return;

        // 1. 将当前鼠标屏幕像素坐标转换为世界射线
        if (F_Camera.ScreenPointToRay(F_Input.MousePosition) is not { } ray) return;

        // 2. 发射射线检测表面
        if (F_Physics.Raycast(ray.Origin, ray.Direction, MaxPlaceDistance, out F_RaycastHit hit))
        {
            // 3. 沿表面法线外移一定偏移量，避免新物体嵌进墙面
            Vector3 spawnPos = hit.Point + hit.Normal * SurfaceOffset;

            // 4. 构造让物体正面（-Z）或顶部对齐表面法线的朝向
            Quaternion alignRot = F_Quat.LookRotation(hit.Normal);

            Entity placed = F_Scene.Instantiate(PropTemplate, spawnPos, alignRot);
            F_Log.Info($"已在 {hit.Entity.Name} 表面放置 {placed.Name}，距离 {hit.Distance:F2}m");
        }
    }
}
```

---

## F_Contact

真实物理流形碰撞信息只读值类型，作为参数传递给 `F_Behaviour` 的 `F_CollisionEnter`、`F_CollisionStay` 和 `F_CollisionExit` 回调。

> 源码位置：[Behaviour.cs#L83](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L83) · [ScriptSystem.cs#L272](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L272)

```csharp
public readonly record struct F_Contact(
    F_Entity Other,
    Vector3 Point,
    Vector3 Normal,
    float Impulse);
```

### 属性表

| 属性名 | 类型 | 单位 / 范围 | 说明 |
| :--- | :--- | :--- | :--- |
| `Other` | `F_Entity` | 有效场景实体 | 与当前脚本所属实体发生碰撞的**对方实体**。 |
| `Point` | `Vector3` | 世界坐标（米 `m`） | 世界空间接触点。多点流形碰撞开始时，按各接触点法向冲量加权平均（冲量为 0 时取算术平均）；分离时还原为最后缓存的接触点世界坐标。 |
| `Normal` | `Vector3` | 单位向量（长度 `1`） | **已针对当前实体校正方向的世界空间单位法线**：方向始终**从 `Other` 指向当前实体 `Entity`**。 |
| `Impulse` | `float` | 牛顿·秒（`N·s`，`>= 0`） | 此次碰撞求解产生的**法向总冲量标量**（接触流形所有接触点的法向冲量之和）。撞击速度越快、质量越大，该值越高；在 `F_CollisionExit` 中固定为 `0f`。 |

### 法线方向约定与触发时机

在 `ScriptSystem` 分发 `F_Contact` 时，已经自动根据收件实体是 A 还是 B 翻转了底层流形法线，因此**无论哪一方收到回调，`contact.Normal` 永远指向“把自己推离对方”的方向**：

```text
          ┌───────────────┐
          │   球 (Self)    │  球收到 F_CollisionEnter：
          └───────┬───────┘    contact.Other  = 地面
                  ▲            contact.Normal = (0, +1, 0)  ← 指向自己（向上）
        contact.Normal
   ═══════════════●═══════════════  ← contact.Point（真实接触点）
              地面 (Other)
                  ▼            地面收到 F_CollisionEnter：
        contact.Normal         contact.Other  = 球
                               contact.Normal = (0, -1, 0)  ← 指向自己（向下）
```

利用这一约定，你可以非常直观地写方向判定：
- 判定“**踩在对方头上 / 落到水平地面上**”：`contact.Normal.Y > 0.65f`
- 判定“**头顶撞到天花板**”：`contact.Normal.Y < -0.65f`
- 判定“**侧面撞墙**”：`MathF.Abs(contact.Normal.Y) < 0.3f`

### 碰撞与触发生命周期回调签名

在任意继承自 `F_Behaviour` 的脚本中直接 `override` 即可响应（未重写的回调零开销）：

```csharp
// 实体碰撞回调（双方均须为非触发器刚体，且至少一方为动态刚体）
public override void F_CollisionEnter(in F_Contact contact) { }
public override void F_CollisionStay(in F_Contact contact) { }
public override void F_CollisionExit(in F_Contact contact) { }

// 触发器重叠回调（至少一方刚体 IsTrigger == true）
public override void F_TriggerEnter(F_Entity other) { }
public override void F_TriggerStay(F_Entity other) { }
public override void F_TriggerExit(F_Entity other) { }
```

::: warning 使用碰撞与触发回调的 3 个注意点
1. **`in` 关键字不可省略**：`F_CollisionEnter / Stay / Exit` 的参数签名为 `in F_Contact contact`（只读引用传参），漏写 `in` 将无法重写基类回调。
2. **双方都会收到回调**：碰撞或触发发生时，只要实体上挂有重写了对应方法的脚本，**碰撞双方（以及触发器方与进入方）**都会分别收到回调，且参数里的 `contact.Other` / `other` 永远是对方。
3. **角色控制器不参与刚体触发器事件**：`F_CharacterController` 是独立于刚体列表的运动学胶囊求解器，不会自动触发 `F_TriggerEnter/Stay/Exit`。若需让角色踩进传送区或拾取金币，请给道具或玩家附加距离检测、射线检测，或在玩家实体上配合刚体逻辑处理。
:::

::: details 底层事件总线快照类型：PhysicsContact 与 PhysicsTrigger
若你通过 `Subscribe<PhysicsContact>(...)` 或 `Subscribe<PhysicsTrigger>(...)` 订阅全局物理事件总线，底层事件结构体定义在 [Physics.cs#L99](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Physics.cs#L99)：
- `public readonly record struct PhysicsContact(EntityReference EntityA, EntityReference EntityB, bool Began, bool Ended, Vector3 Point, Vector3 Normal, float Impulse)`（注意：此底层快照中的 `Normal` 固定由 `EntityA` 指向 `EntityB`，未经接收方方向翻转）。
- `public readonly record struct PhysicsTrigger(EntityReference TriggerEntity, EntityReference OtherEntity, bool Entered)`。
:::

### 示例：按法向冲量判定摔落伤害、撞击音效与触发区拾取

```csharp
using System;
using System.Numerics;
using FlareEngine;

public class FragileCrate : F_Behaviour
{
    [F_Header("耐摔与音效")]
    [F_Expose] public float Health = 100f;
    [F_Expose] public float SafeImpulse = 6f;
    [F_Expose] public float DamagePerImpulse = 8f;
    [F_Expose] public string ImpactSoundPath = "Assets/Audio/wood_hit.wav";

    public override void F_CollisionEnter(in F_Contact contact)
    {
        // 1. 按法向冲量线性映射撞击音量（0.15..1.0），在真实撞击点播放 3D 音效
        if (contact.Impulse > 1.5f && !string.IsNullOrEmpty(ImpactSoundPath))
        {
            float volume = F_Math.Clamp(contact.Impulse / 20f, 0.15f, 1f);
            Audio.PlayOneShotAt(ImpactSoundPath, contact.Point, volume);
        }

        // 2. 超过安全冲量阈值时按超出部分扣血
        if (contact.Impulse > SafeImpulse)
        {
            float damage = (contact.Impulse - SafeImpulse) * DamagePerImpulse;
            Health -= damage;
            F_Log.Info($"撞击 {contact.Other.Name}（法线 {contact.Normal}，冲量 {contact.Impulse:F1} N·s），扣血 {damage:F1}，剩余 {Health:F1}");

            if (Health <= 0f)
            {
                DestroySelf();
            }
        }
    }

    public override void F_TriggerEnter(F_Entity other)
    {
        if (other.CompareTag("HealZone"))
        {
            Health = 100f;
            F_Log.Info($"进入恢复区 {other.Name}，木箱已修复");
        }
    }
}
```

---

## F_Rigidbody / F_RigidBody

物理刚体运行时代理组件。继承链为 `F_RigidBody` ← `F_Rigidbody` ← `RigidBody`（向下兼容名），底层返回的实际实例均为 `RigidBody`，因此 **`F_Rigidbody`、`F_RigidBody` 与 `RigidBody` 三种类型名可完全等价互换**。

> 源码位置：[Physics.cs#L153](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Physics.cs#L153) · [FlareErgonomics.cs#L606](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L606)

### 如何获取

```csharp
// 方式 1：在 F_Start() 获取当前实体上挂载的刚体（推荐）
private F_Rigidbody? _body;
public override void F_Start() => _body = GetComponent<F_Rigidbody>();

// 方式 2：安全判断后直接操作
if (TryGetComponent<F_Rigidbody>(out var body))
{
    body.AddImpulse(Vector3.UnitY * 5f);
}

// 方式 3：获取其他实体上的刚体
F_RigidBody? targetBody = F_Physics.GetBody(otherEntity);
```

### 属性表

| 属性名 | 类型 | 读 / 写 | 说明 |
| :--- | :--- | :--- | :--- |
| `Handle` | `ulong` | 只读 | 底层物理世界的原生刚体句柄。 |
| `Entity` | `Entity` | 只读 | 该刚体所属的场景实体。 |
| `State` | `RigidBodyState`（可隐式转为 `F_RigidBodyState`） | 只读 | 读取刚体当前瞬时状态快照（`Transform`、`LinearVelocity`、`AngularVelocity`、`Mass`、`Sleeping`）。需要多个字段时建议先存局部变量，减少跨边界调用。 |
| `Velocity` | `Vector3` | 读 / 写 | 获取或设置世界空间线速度（米/秒 `m/s`）。写入时保留当前 `AngularVelocity` 并唤醒刚体；不可用于静态刚体，速率上限 `100 m/s`。 |
| `AngularVelocity` | `Vector3` | 读 / 写 | 获取或设置世界空间角速度（弧度/秒 `rad/s`）。写入时保留当前 `Velocity` 并唤醒刚体；不可用于静态刚体，角速率上限 `100 rad/s`。 |
| `IsTrigger` | `bool` | 读 / 写 | 获取或设置是否为触发器（`true` 时只检测重叠并派发 `F_Trigger*`，不产生物理碰撞阻挡，且被射线检测忽略）。 |

::: tip 如何读取或修改刚体的位置与旋转？
`F_Rigidbody` 不单独重复定义 `Position` / `Rotation` 属性：
- **读取位姿**：直接读取 `body.State.Transform.Position` / `body.State.Transform.Rotation`，或读取 `Entity.WorldPosition` / `Entity.WorldRotation`。
- **瞬间改变位置或旋转**：调用 `body.Teleport(transform, clearVelocity)`；若直接给 `Entity.Position` / `Entity.Rotation` 赋值，`F_Entity` 内部检测到刚体时会自动转为调用 `body.Teleport(..., clearVelocity: false)` 同步物理体并保留当前速度。
:::

### 方法详解

#### AddForce

在刚体质心处施加持续世界空间力（牛顿 `N`），不产生力矩。

```csharp
public void AddForce(Vector3 force);
```

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `force` | `Vector3` | 世界空间力向量（牛顿 `N`）。 |

- **行为与注意**：力累计器在每个物理步结束后自动清零，因此持续推力（如引擎推进、风力、磁力）应在 `F_FixedUpdate()` 中每步调用。会自动唤醒休眠刚体；对静态刚体（`Mass == 0`）调用会抛出 `EngineException("Bridge.PhysicsFailed")`。
- 内部等价于 `ApplyForce(force, Entity.Position)`；若刚体挂在**子实体**上，请改用 `ApplyForce(force, Entity.WorldPosition)` 确保作用点准确落在世界质心。

#### AddImpulse

在刚体质心处施加瞬时世界空间冲量（牛顿·秒 `N·s`），立刻改变刚体速度（`deltaV = impulse / Mass`）。

```csharp
public void AddImpulse(Vector3 impulse);
```

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `impulse` | `Vector3` | 世界空间冲量向量（`N·s`）。 |

- **行为与注意**：适合单帧爆发事件（跳跃、击退、炮弹发射），无需乘 `DeltaTime`。会自动唤醒休眠刚体；不可用于静态刚体。子实体建议用 `ApplyImpulse(impulse, Entity.WorldPosition)`。

#### ApplyForce

在指定**世界空间作用点**对刚体施加持续力（牛顿 `N`）；当作用点偏离质心时，同时产生旋转力矩。

```csharp
public void ApplyForce(Vector3 force, Vector3 worldPoint);
```

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `force` | `Vector3` | 世界空间力向量（牛顿 `N`）。 |
| `worldPoint` | `Vector3` | 力的世界空间作用点坐标（米）。 |

#### ApplyImpulse

在指定**世界空间作用点**对刚体施加瞬时冲量（`N·s`），同时改变线速度与角速度（例如击中木箱边角使其翻滚飞出）。

```csharp
public void ApplyImpulse(Vector3 impulse, Vector3 worldPoint);
```

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `impulse` | `Vector3` | 世界空间冲量向量（`N·s`）。 |
| `worldPoint` | `Vector3` | 冲量的世界空间作用点坐标（米，常直接传 `hit.Point`）。 |

#### SetVelocity

一次性直接设置刚体的世界空间线速度与角速度，并自动唤醒刚体。

```csharp
public void SetVelocity(Vector3 linear, Vector3 angular = default);
```

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `linear` | `Vector3` | 必填 | 目标世界空间线速度（`m/s`），模长不得超过 `100 m/s`。 |
| `angular` | `Vector3` | `default` (`Vector3.Zero`) | 目标世界空间角速度（`rad/s`），模长不得超过 `100 rad/s`。 |

- **行为与注意**：同时修改线速度与角速度时，比分别写 `Velocity` 与 `AngularVelocity` 少一次原生边界调用；对静态刚体调用或超速会抛出 `EngineException("Bridge.PhysicsFailed")`。

#### Teleport

将刚体与所属实体瞬移到指定空间变换，同时清理旧接触缓存并唤醒周围动态刚体。

```csharp
public void Teleport(Transform transform, bool clearVelocity = true);
```

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `transform` | `Transform`（可隐式传入 `F_Transform`） | 必填 | 目标位姿（位置、单位四元数旋转与缩放）。 |
| `clearVelocity` | `bool` | `true` | `true` 时清空线速度与角速度；`false` 时保留当前速度（如穿越传送门）。 |

::: warning Teleport 不可同时修改 Scale
原生物理体不支持通过 `Teleport` 缩放尺寸：传入的 `transform.Scale` **必须与实体当前 `Transform.Scale` 保持一致**，否则会抛出 `EngineException("Bridge.BodyScaleChanged")`。推荐写法：
```csharp
var pose = Entity.Transform;
pose.Position = new Vector3(0f, 3f, 0f);
pose.Rotation = F_Quat.Identity;
body.Teleport(pose, clearVelocity: true);
```
:::

#### Destroy

从物理世界中销毁并移除该刚体（若源自编辑器 `PhysicsBody` / `ColliderSettings` 组件，也会同步移除对应原生组件）。场景卸载或实体已销毁时重复调用会被安全忽略。

```csharp
public void Destroy();
```

### 示例：质心推进、射线命中点翻滚击飞与传送门保留速度

```csharp
using System.Numerics;
using FlareEngine;

public class PhysicsPlayground : F_Behaviour
{
    [F_Expose] public float ThrustForce = 25f;
    [F_Expose] public float ShotImpulse = 12f;
    [F_Expose] public F_Entity? PortalExit;

    private F_Rigidbody? _body;

    public override void F_Start()
    {
        _body = GetComponent<F_Rigidbody>();
    }

    public override void F_Update()
    {
        // 左键点射任意动态刚体，在真实命中点施加冲量使其翻滚飞出
        if (F_Input.MouseDown(F_MouseButton.Left) &&
            F_Camera.ScreenPointToRay(F_Input.MousePosition) is { } ray &&
            F_Physics.Raycast(ray.Origin, ray.Direction, 100f, out F_RaycastHit hit))
        {
            if (hit.Body.State.Mass > 0f)
            {
                hit.Body.ApplyImpulse(ray.Direction * ShotImpulse, hit.Point);
            }
        }

        // 按 R 键重置回原点并清零速度
        if (F_Input.KeyDown(F_Key.R) && _body is not null)
        {
            var pose = Entity.Transform;
            pose.Position = new Vector3(0f, 2f, 0f);
            pose.Rotation = F_Quat.Identity;
            _body.Teleport(pose, clearVelocity: true);
        }
    }

    public override void F_FixedUpdate(float fixedDelta)
    {
        if (_body is null) return;

        // WASD 在世界水平面施加持续推力（-Z 为前）
        Vector2 move = F_Input.MoveVector;
        if (move.LengthSquared() > 0.001f)
        {
            _body.AddForce(new Vector3(move.X, 0f, -move.Y) * ThrustForce);
        }
    }

    public override void F_TriggerEnter(F_Entity other)
    {
        // 进入传送门触发器：瞬移到出口并保留原有速度
        if (other.CompareTag("Portal") && _body is not null && PortalExit is { IsValid: true })
        {
            var pose = Entity.Transform;
            pose.Position = PortalExit.WorldPosition;
            _body.Teleport(pose, clearVelocity: false);
        }
    }
}
```

### F_BoxBodyDescription

盒体刚体创建参数结构体（兼容名 `BoxBodyDescription`，两者可隐式互转）。

> 源码位置：[Physics.cs#L8](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Physics.cs#L8)

| 字段 / 静态预设 | 类型 | `Dynamic` 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `HalfExtents` | `Vector3` | `(0.5, 0.5, 0.5)` | 盒体局部半长宽高（米），各轴须 `> 0.001`。实际尺寸为 `HalfExtents * 2 * |LocalScale|`。 |
| `Mass` | `float` | `1f` | 质量（`kg`）。**`> 0` 动态刚体，`0` 静态刚体**，不可为负。 |
| `Friction` | `float` | `0.6f` | 摩擦系数（`>= 0`）。两物体接触摩擦取几何平均 `sqrt(fA * fB)`。 |
| `Restitution` | `float` | `0f` | 弹性恢复系数，范围 `[0, 1]`（闭合速度 `< 1 m/s` 时自动抑制反弹防止微抖）。 |
| `Layer` | `uint` | `1u`（即 `1u << 0`） | 32 位碰撞层位掩码。 |
| `Mask` | `uint` | `uint.MaxValue` | 32 位碰撞过滤掩码。 |
| `Dynamic` *(静态属性)* | `F_BoxBodyDescription` | `HalfExtents=(0.5,0.5,0.5), Mass=1, Friction=0.6, Restitution=0, Layer=1, Mask=uint.MaxValue` | 1 米单位动态立方体预设。 |
| `Static` *(静态属性)* | `F_BoxBodyDescription` | 同 `Dynamic`，`Mass = 0` | 1 米单位静态立方体预设。 |

### F_SphereBodyDescription

球体刚体与球形触发器创建参数结构体（兼容名 `SphereBodyDescription`，两者可隐式互转）。

> 源码位置：[Physics.cs#L33](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Physics.cs#L33)

| 字段 / 属性 / 静态预设 | 类型 | `Dynamic` 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `Radius` | `float` | `0.5f` | 球体局部半径（米），须 `> 0.001`。实际半径乘以实体三轴缩放绝对值的最大值。 |
| `Mass` | `float` | `1f` | 质量（`kg`）。`> 0` 动态刚体，`0` 静态刚体。 |
| `Friction` | `float` | `0.6f` | 摩擦系数（`>= 0`）。 |
| `Restitution` | `float` | `0f` | 弹性恢复系数，范围 `[0, 1]`。 |
| `Layer` | `uint` | `1u` | 32 位碰撞层位掩码。 |
| `Mask` | `uint` | `uint.MaxValue` | 32 位碰撞过滤掩码。 |
| `IsTrigger` *(属性)* | `bool` | `false` | 设为 `true` 时作为触发器创建，仅检测重叠不产生物理排斥。 |
| `Dynamic` *(静态属性)* | `F_SphereBodyDescription` | `Radius=0.5, Mass=1, Friction=0.6, Restitution=0, Layer=1, Mask=uint.MaxValue, IsTrigger=false` | 半径 0.5 米动态球体预设。 |
| `Static` *(静态属性)* | `F_SphereBodyDescription` | 同 `Dynamic`，`Mass = 0` | 静态球体预设。 |
| `Trigger` *(静态属性)* | `F_SphereBodyDescription` | 同 `Static`，`IsTrigger = true` | 半径 0.5 米静态球形触发器预设。 |

### F_RigidBodyState

刚体动力学瞬时状态快照结构体（兼容名 `RigidBodyState`，两者可隐式互转；`F_Rigidbody.State` 返回 `RigidBodyState`）。

> 源码位置：[Physics.cs#L109](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Physics.cs#L109)

| 字段 / 属性 | 类型 | 单位 / 范围 | 说明 |
| :--- | :--- | :--- | :--- |
| `Transform` | `Transform` | 位姿结构体 | 当前刚体世界位置 `Position`、世界旋转 `Rotation` 与实体缩放 `Scale`。 |
| `LinearVelocity` | `Vector3` | 米/秒（`m/s`） | 当前世界空间线速度。 |
| `AngularVelocity` | `Vector3` | 弧度/秒（`rad/s`） | 当前世界空间角速度。 |
| `Mass` | `float` | 千克（`kg`） | 刚体质量（`0` 表示静态刚体）。 |
| `Sleeping` *(只读属性)* | `bool` | `true` / `false` | 是否处于物理休眠状态（低速接触持续超 `0.7 s` 自动休眠）。 |

---

## F_CharacterController

运动学直立胶囊体角色控制器（兼容名 `CharacterController`，继承自 `F_CharacterController`）。内置多子步防穿墙求解、斜坡滑移（约 `49.5°` 内可站立）、顶头清零上升速度、重力与着地跳跃，以及对动态刚体施加受上限约束的柔和推力。

> 源码位置：[CharacterController.cs#L36](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/CharacterController.cs#L36) · [CharacterController.cpp#L9](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/CharacterController.cpp#L9)

```text
          ╭───╮   ▲  Radius (默认 0.3m)
          │   │   ┼
          │ ● │   │  SegmentHalfHeight (默认 0.6m)   Position = 胶囊中心 ●
          │   │   │  总身高 = 2 * (SegmentHalfHeight + Radius) = 1.8m
          │   │   ┼
          ╰───╯   ▼  Radius (默认 0.3m)
     ═══════════════ 脚底位于 Position.Y - (SegmentHalfHeight + Radius)
```

### 如何获取与创建

```csharp
// 方式 1（推荐）：编辑器给角色挂好「角色控制器」组件后，在 F_Start() 获取
private F_CharacterController? _controller;
public override void F_Start() => _controller = GetComponent<F_CharacterController>();

// 方式 2：静态绑定编辑器组件（未挂载时会抛出 EngineException("Bridge.InvalidCharacter")）
CharacterController cc = F_CharacterController.FromComponent(Entity);

// 方式 3：纯代码在根实体上动态创建（要求实体无父节点且无冲突物理组件）
var desc = F_CharacterDescription.Default;
desc.WalkSpeed = 6f;
desc.JumpSpeed = 6.5f;
var dynamicCc = new F_CharacterController(Entity, desc);
```

### 属性与方法详解

#### 属性表

| 属性名 | 类型 | 读 / 写 | 说明 |
| :--- | :--- | :--- | :--- |
| `Entity` | `Entity` | 只读 | 所属场景实体。 |
| `State` | `CharacterState`（可隐式转为 `F_CharacterState`） | 只读 | 读取角色控制器最新位置 `Position`、竖直速度 `VerticalVelocity` 与是否着地 `Grounded`。 |

#### Move

在固定物理步 `F_FixedUpdate` 中按水平移动输入与跳跃请求推进角色运动，自动执行重力积分、斜坡/墙壁碰撞推离并更新实体世界坐标。

```csharp
public CharacterState Move(Vector3 horizontalDirection, bool jumpPressed, double seconds);
```

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `horizontalDirection` | `Vector3` | 世界空间水平移动方向与速率比例。<br>- **自动忽略 `Y` 分量**（内部会置零）。<br>- **无需强制手动归一化**：当长度 `> 1` 时底层会自动归一化到长度 `1`；长度在 `[0, 1]` 之间时按比例缩放步速（可直接传入摇杆模拟量，实现轻推慢走、满推疾跑）。<br>- 实际水平速度 = `clampedDirection * WalkSpeed`。 |
| `jumpPressed` | `bool` | 本步是否请求起跳。仅当 **`jumpPressed == true` 且上一步处于 `Grounded == true`** 时，底层才会把竖直速度置为 `JumpSpeed` 起跳，再叠加本步重力 `gravity.y * seconds`。 |
| `seconds` | `double` | 本步模拟时长（秒），**必须满足 `0 < seconds <= 0.1`**。请直接传入 `F_FixedUpdate(float fixedDelta)` 的 `fixedDelta` 或 `F_Time.FixedDeltaTime`。 |

**返回值**：`CharacterState`（可隐式赋给 `F_CharacterState`），包含本次移动解算后的最新 `Position`、`VerticalVelocity` 与 `Grounded`。

::: tip 编辑器组件的自动重力机制
对于在编辑器中挂载的 **角色控制器** 组件，如果某个物理步内没有任何脚本调用 `Move(...)`，引擎会自动以零水平输入推进一次重力与接地（防止角色悬空）；只要脚本在该物理步调用了 `Move(...)`，引擎不会再重复施加重力。脚本用 `new F_CharacterController(...)` 动态创建的实例则完全由脚本自己调用 `Move(...)` 驱动。
:::

#### Teleport

将角色控制器与所属实体瞬移到指定世界坐标，同时**清零 `VerticalVelocity` 并将 `Grounded` 设为 `false`**（下一次 `Move` 会重新检测脚下支撑）。

```csharp
public void Teleport(Vector3 position);
```

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `position` | `Vector3` | 胶囊体几何中心的新世界坐标（米）。 |

#### Destroy

销毁角色控制器组件代理（若源自编辑器组件也会移除对应原生组件），可安全重复调用。销毁后再调用 `Move` / `Teleport` 会抛出 `EngineException("Bridge.InvalidCharacter")`。

```csharp
public void Destroy();
```

### F_CharacterDescription

胶囊体角色控制器参数结构体（兼容名 `CharacterDescription`，两者可隐式互转）。

> 源码位置：[CharacterController.cs#L8](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/CharacterController.cs#L8)

| 字段 / 静态预设 | 类型 | `Default` 默认值 | 单位与合法范围 | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| `Radius` | `float` | `0.3f` | 米（`> 0.001`） | 胶囊体横截面半径。 |
| `SegmentHalfHeight` | `float` | `0.6f` | 米（`>= 0`） | 中间圆柱线段半高；总身高为 `2 * (SegmentHalfHeight + Radius)`（默认 `1.8 m`）。 |
| `WalkSpeed` | `float` | `4f` | `m/s`（`>= 0`） | 满额水平输入时的最大移动速度。 |
| `JumpSpeed` | `float` | `5f` | `m/s`（`>= 0`） | 着地起跳瞬间赋予的向上竖直初速度。 |
| `MaximumPushImpulse` | `float` | `0.5f` | `N·s`（`>= 0`） | 单次 `Move` 内允许施加给沿途动态刚体的总推力冲量上限；设为 `0` 时完全不推动箱子。 |
| `Default` *(静态属性)* | `F_CharacterDescription` | `0.3, 0.6, 4, 5, 0.5` | — | 标准 1.8 米人形角色预设。 |

### F_CharacterState

胶囊体角色控制器瞬时运动状态只读值类型（兼容名 `CharacterState`，两者可隐式互转）。

> 源码位置：[CharacterController.cs#L27](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/CharacterController.cs#L27)

| 属性名 | 类型 | 单位 / 范围 | 说明 |
| :--- | :--- | :--- | :--- |
| `Position` | `Vector3` | 世界坐标（米 `m`） | 胶囊体几何中心当前的世界坐标。 |
| `VerticalVelocity` | `float` | 米/秒（`m/s`） | 当前竖直速度（起跳为正、下落为负、着地静止时为 `0`）。 |
| `Grounded` | `bool` | `true` / `false` | 当前是否踩在可站立地面（接触推离法线 `Y > 0.65`）上。 |

### 示例一：相机朝向移动、跳跃缓冲与着地监测（`F_CharacterController`）

适合第一人称或第三人称探索/动作角色。演示如何在 `F_Update` 采集按键并做 **0.12 秒跳跃输入缓冲**（落地前提前按空格也能顺滑起跳），在 `F_FixedUpdate` 按相机朝向调用 `Move`：

```csharp
using System;
using System.Numerics;
using FlareEngine;

public class PlayerMotor : F_Behaviour
{
    [F_Header("视角与控制")]
    [F_Expose] public F_Entity? CameraEntity;
    [F_Expose, F_Range(0.05f, 0.5f)] public float MouseSensitivity = 0.16f;
    [F_Expose, F_Range(0f, 0.3f)] public float JumpBufferSeconds = 0.12f;

    [F_Header("运行态只读监视")]
    [F_Expose, F_ReadOnly] public bool IsGrounded;
    [F_Expose, F_ReadOnly] public float VerticalSpeed;

    private F_CharacterController? _cc;
    private float _yaw;
    private float _jumpBufferTimer;
    private bool _wasGrounded;

    public override void F_Start()
    {
        _cc = GetComponent<F_CharacterController>();
        _yaw = Entity.EulerAngles.Y;
        F_Cursor.Locked = true;
    }

    public override void F_Update(float delta)
    {
        // 按 Esc 切换光标锁定，方便调试
        if (F_Input.KeyDown(F_Key.Escape))
            F_Cursor.Locked = !F_Cursor.Locked;

        // 水平鼠标旋转角色朝向（右手系绕 +Y 轴）
        if (F_Cursor.Locked)
        {
            _yaw -= F_Input.MouseDelta.X * MouseSensitivity;
            Entity.EulerAngles = new Vector3(0f, _yaw, 0f);
        }

        // 在渲染帧捕获 KeyDown，写入跳跃缓冲计时器，避免 FixedUpdate 丢键
        if (F_Input.KeyDown(F_Key.Space))
            _jumpBufferTimer = JumpBufferSeconds;
        else if (_jumpBufferTimer > 0f)
            _jumpBufferTimer = MathF.Max(0f, _jumpBufferTimer - delta);
    }

    public override void F_FixedUpdate(float fixedDelta)
    {
        if (_cc is null) return;

        // 1. 按相机（或角色自身）水平前/右方向合成移动方向
        F_Entity basis = CameraEntity is { IsValid: true } ? CameraEntity : Entity;
        Vector3 forward = basis.Forward;
        forward.Y = 0f;
        if (forward.LengthSquared() > 1e-6f) forward = Vector3.Normalize(forward);

        Vector3 right = basis.Right;
        right.Y = 0f;
        if (right.LengthSquared() > 1e-6f) right = Vector3.Normalize(right);

        Vector2 input = F_Input.MoveVector;
        Vector3 moveDir = right * input.X + forward * input.Y;

        // 2. 只有当角色着地且缓冲期内按过空格时才发起起跳
        bool wantJump = _jumpBufferTimer > 0f && _cc.State.Grounded;
        if (wantJump) _jumpBufferTimer = 0f;

        // 3. 推进胶囊角色控制器
        F_CharacterState state = _cc.Move(moveDir, wantJump, fixedDelta);

        // 4. 监测落地瞬间与实时状态
        if (!_wasGrounded && state.Grounded)
        {
            F_Log.Info($"角色着地于 {state.Position}");
        }
        _wasGrounded = state.Grounded;
        IsGrounded = state.Grounded;
        VerticalSpeed = state.VerticalVelocity;
    }

    public override void F_Destroy()
    {
        F_Cursor.Locked = false;
    }
}
```

### 示例二：带二段跳与脚底射线检测的刚体角色控制（`F_Rigidbody` + `F_Physics.Raycast`）

::: warning 为什么二段跳推荐使用 `F_Rigidbody`？
`F_CharacterController.Move` 的原生跳跃逻辑严格检查 `jumpPressed && Grounded`（仅允许着地起跳，且未暴露直接覆盖内部 `VerticalVelocity` 的接口）。如果你的游戏需要**二段跳、空中冲刺、弹簧板弹射或火箭跳**等在空中自由改写竖直速度的玩法，推荐直接给角色挂动态 `F_Rigidbody` 并搭配 `F_Physics.Raycast` 做脚底接地检测：
:::

```csharp
using System;
using System.Numerics;
using FlareEngine;

public class DoubleJumpMover : F_Behaviour
{
    [F_Header("移动与二段跳")]
    [F_Expose] public float MoveSpeed = 6f;
    [F_Expose] public float JumpVelocity = 6.5f;
    [F_Expose] public int MaxJumps = 2;
    [F_Expose] public int PlayerLayer = 2;        // 独立图层，避免向下射线误命中自己（地面默认在图层 0）
    [F_Expose] public float GroundRayLength = 0.65f; // 略大于刚体半高（如 0.5m + 0.15m 容差）

    private F_Rigidbody? _body;
    private bool _jumpRequested;
    private int _jumpsRemaining;
    private bool _grounded;

    public override void F_Start()
    {
        Entity.Layer = PlayerLayer;
        _body = GetComponent<F_Rigidbody>();
        _jumpsRemaining = MaxJumps;
    }

    public override void F_Update()
    {
        if (F_Input.KeyDown(F_Key.Space))
            _jumpRequested = true;
    }

    public override void F_FixedUpdate(float fixedDelta)
    {
        if (_body is null) return;

        // 1. 排除自身图层向下发射射线判断是否着地
        uint mask = ~(1u << Entity.Layer);
        _grounded = F_Physics.Raycast(Entity.WorldPosition, -Vector3.UnitY, GroundRayLength, mask).HasValue;
        if (_grounded && _body.Velocity.Y <= 0.1f)
            _jumpsRemaining = MaxJumps;

        // 2. 合成目标水平速度，保留原有竖直速度
        Vector2 input = F_Input.MoveVector;
        Vector3 vel = _body.Velocity;
        Vector3 horizontal = (Entity.Right * input.X + Entity.Forward * input.Y) * MoveSpeed;
        vel.X = horizontal.X;
        vel.Z = horizontal.Z;

        // 3. 着地跳或空中二段跳：直接重写竖直速度实现干脆手感
        if (_jumpRequested)
        {
            _jumpRequested = false;
            if (_jumpsRemaining > 0)
            {
                vel.Y = JumpVelocity;
                _jumpsRemaining--;
                F_Log.Info(_grounded ? "一段跳！" : $"空中二段跳！（剩余 {_jumpsRemaining} 次）");
            }
        }

        // 锁定旋转（角速度清零），防止角色胶囊/方块翻倒
        _body.SetVelocity(vel, Vector3.Zero);
    }
}
```

---

## 常见物理排查与避坑指南

| 现象 | 常见原因 | 解决办法 |
| :--- | :--- | :--- |
| `F_Physics.Raycast` 总是返回 `null` | ① `direction` 是零向量；② 目标刚体勾选了 `IsTrigger`；③ `mask` 漏掉了目标的 `1u << target.Layer`。 | 开启 `F_Physics.SetDebugDrawing(true)` 确认目标不是青色触发器，并核对 `Entity.Layer` 与 `mask`。 |
| 调用 `AddForce` / `SetVelocity` 抛出 `Bridge.PhysicsFailed` | 目标是静态刚体（`Mass == 0`），或传入了 `NaN` / 超过 `100 m/s` 的速度。 | 静态刚体请改用 `Teleport` 移动，动态刚体请确保质量 `> 0` 且速度在上限以内。 |
| `F_Input.KeyDown` 放在 `F_FixedUpdate` 里偶尔没反应 | 渲染帧与固定物理步频率不同，高帧率或掉帧时可能出现某帧执行 0 次或多次 `F_FixedUpdate`。 | 在 `F_Update()` 捕获瞬时按键 `KeyDown` 存入布尔/计时器变量，再在 `F_FixedUpdate()` 里消费清零。 |
| `F_CollisionEnter` 从不触发 | ① 方法签名漏写了 `in`（应为 `in F_Contact contact`）；② 两物体都是静态刚体（`Mass == 0`）；③ 至少一方是触发器（应改写 `F_TriggerEnter`）。 | 核对回调签名、至少一方为动态刚体且未勾选触发器，检查 `Layer`/`Mask` 是否相交。 |
| `Teleport` 抛出 `Bridge.BodyScaleChanged` | 构造新 `Transform` 时没复制实体原有 `Scale`（导致 `Scale` 变成 `(0, 0, 0)` 或 `(1, 1, 1)` 与实体不一致）。 | 先读取 `var t = Entity.Transform;`，仅修改 `t.Position` 和 `t.Rotation` 后再传入 `body.Teleport(t)`。 |
