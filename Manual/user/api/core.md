# 核心 API：行为、实体、场景与特性

本页是 `FlareEngine` 命名空间核心脚手架的完整 API 参考，涵盖脚本组件基类 **`F_Behaviour`**、场景实体 **`F_Entity`**、核心值结构体（**`F_Transform`** / **`F_EntityReference`** / **`F_Color`**）、场景查询与克隆 **`F_Scene`**（含进阶多场景接口），以及全部 8 个编辑器序列化与面板特性。

::: tip 使用前确认两件事
- 脚本顶部引入 `using FlareEngine;` 与 `using System.Numerics;`（提供 `Vector2`、`Vector3`、`Quaternion`）。
- 引擎采用**右手坐标系**（`+X` 右、`+Y` 上、**`-Z` 为物体正前方**），距离单位为**米 (`m`)**，时间为**秒 (`s`)**，欧拉角均为**角度制 (`°`)** 且旋转顺序为 `Z → X → Y`。
:::

## 本页类型速查

| 推荐类型 (`F_` 命名) | 兼容别名 | 一句话定位 | 源码定位 |
| :--- | :--- | :--- | :--- |
| [`F_Behaviour`](#f_behaviour) | — | 游戏脚本组件基类，承接生命周期与物理回调 | [Behaviour.cs#L88](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L88) |
| [`F_Entity`](#f_entity) | `Entity` | 场景物体句柄，读写变换、标签、层级、组件与动画 | [EngineSession.cs#L453](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L453) · [FlareErgonomics.cs#L362](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L362) |
| [`F_Transform`](#f_transform) | `Transform` | 三维空间变换结构体（位置、四元数旋转、缩放） | [EngineSession.cs#L15](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L15) |
| [`F_EntityReference`](#f_entityreference) | `EntityReference` | 持久化 64 位 `StableId` 实体引用槽位 | [Behaviour.cs#L55](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L55) |
| [`F_Color`](#f_color) | — | 线性空间 RGBA 浮点颜色结构体与 7 个预设色 | [Behaviour.cs#L39](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L39) |
| [`F_Scene`](#f_scene) | — | 静态场景查询、深克隆实例化与场景重载工具 | [FlareErgonomics.cs#L629](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L629) |
| [`SceneService` / `Scene`](#scene-streaming) | — | 进阶多场景异步加载、替换、叠加与卸载管理 | [Scenes.cs#L17](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Scenes.cs#L17) |
| [面板与序列化特性](#attributes) | — | `[F_Expose]`、`[F_Header]`、`[F_Range]`、`[F_Tooltip]`、`[F_Space]`、`[F_ReadOnly]`、`[F_Script]`、`[F_Event]` | [Behaviour.cs#L8](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L8) · [ScriptFields.cs#L15](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L15) · [ScriptEvents.cs#L17](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L17) |

---

## 1. `F_Behaviour` {#f_behaviour}

所有挂载到场景实体的 C# 脚本都必须继承 `public abstract class F_Behaviour`，且子类必须是公开、非抽象并带有公开无参构造函数的类（不手写构造函数时 C# 自动提供）。

源码：[Behaviour.cs#L88-L195](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L88-L195)

### 公开属性

| 属性名 | 类型 | 权限 | 说明 |
| :--- | :--- | :--- | :--- |
| `Entity` | `Entity` | 只读 | 当前脚本挂载的场景实体（`Entity` 继承自 `F_Entity`，可直接赋值给 `F_Entity` 变量）。 |
| `Transform` | `Transform` | 读写 | 当前实体相对父节点的局部变换快捷方式，等价于 `Entity.Transform`。 |
| `Enabled` | `bool` | 读写 | 脚本组件启用状态（默认 `true`）。运行期置为 `false` 会触发 `F_Disable()` 并停止帧更新；重新置为 `true` 会触发 `F_Enable()`。 |
| `UpdateWhenPaused` | `bool` (`virtual`) | 只读（可重写） | 默认 `false`。重写为 `=> true` 时，即使编辑器或逻辑时钟处于暂停状态，该脚本的 `F_PreUpdate` / `F_Update` / `F_LateUpdate` 仍会继续执行（适合暂停菜单 UI）。 |
| `Scripts` | `ScriptSystem` | 只读 | 所属脚本调度系统，用于访问会话、事件总线、命令和 UI 动作绑定（未挂载时访问会抛出 `InvalidOperationException`）。 |
| `Input` | `ScriptInput` | 只读 | 当前帧底层键鼠输入快照（常规游戏开发推荐直接使用静态类 `F_Input`，详见 [输入与数学篇](input-math.md)）。 |
| `Actions` | `InputActions` | 只读 | 可配置动作映射输入服务（`.inputactions.json`，继承自 `F_InputActions`），等价于 `Scripts.Session.Actions`。 |
| `Audio` | `Audio` | 只读 | 音频与 3D 空间音源服务（继承自 `F_Audio`），等价于 `Scripts.Session.Audio`。 |
| `Particles` | `Particles` | 只读 | 粒子发射器控制服务（继承自 `F_Particles`），等价于 `Scripts.Session.Particles`。 |
| `Navigation` | `Navigation` | 只读 | 导航网格寻路服务（继承自 `F_Navigation`），等价于 `Scripts.Session.Navigation`。 |
| `Physics` | `Physics` | 只读 | 物理世界查询与刚体管理服务，等价于 `Scripts.Session.Physics`（也可直接调用静态门面 `F_Physics`）。 |
| `Ui` | `GameUi` | 只读 | 游戏界面文档树（GameUI）服务（继承自 `F_GameUi`），等价于 `Scripts.Session.Ui`。 |

::: tip 如何访问 `Session`、事件总线 `Events`、单播命令 `Commands` 与 `UiActions`？
这些服务挂在 `Scripts`（`ScriptSystem`）与 `Entity` 上，在任意 `F_Behaviour` 子类里直接这样取：
- **底层引擎会话**：`Scripts.Session` 或 `Entity.Session`
- **强类型事件总线**：`Scripts.Events.Publish(...)`；订阅事件可直接用基类内置保护方法 `Subscribe<T>(handler)`（销毁时自动解绑）
- **单播控制命令**：`Scripts.Commands.Register(...)` / `Scripts.Commands.Execute(...)`
- **持久化 UI 动作绑定**：`Scripts.UiActions.Register(this, "OnStart", ev => ...)`
:::

### 生命周期与物理回调

所有生命周期方法都是 `public virtual void`，按需 `override` 即可。调度顺序如下：

```text
挂载并激活
   │
   ▼
F_Awake() ──▶ F_Enable() ──▶ F_Start()    (首次更新前各执行一次；重新启用再次触发 F_Enable)
                                 │
   ┌─────────────────────────────┴──────────────────────────────┐
   │  每渲染帧主循环                                             │
   │    1. F_PreUpdate(delta)                                   │
   │    2. 固定物理步循环（默认 60 Hz，每步 0..N 次）：            │
   │         ├─ F_FixedUpdate(fixedDelta)                       │
   │         ├─ 物理世界步进                                     │
   │         └─ 碰撞/触发回调：                                  │
   │              F_CollisionEnter / Stay / Exit(in F_Contact)  │
   │              F_TriggerEnter   / Stay / Exit(F_Entity)      │
   │    3. F_Update(delta)                                      │
   │    4. F_LateUpdate(delta)                                  │
   └─────────────────────────────┬──────────────────────────────┘
                                 │  禁用 / 实体销毁 / 场景卸载 / 停止预览
                                 ▼
                         F_Disable() ──▶ F_Destroy()
```

| 回调签名 | 触发时机 | 参数与常见用途 |
| :--- | :--- | :--- |
| `void F_Awake()` | 实体及其父级层级均处于激活态、脚本首次就绪时调用一次（早于 `F_Enable` 和 `F_Start`）。 | 无参数。初始化本脚本内部字段、容器或非依赖其他脚本的状态。 |
| `void F_Enable()` | 每次脚本从非激活变为激活态时调用（首次紧接 `F_Awake` 之后；之后切换 `Enabled` 或 `Entity.Enabled` 为 `true` 时再次触发）。 | 无参数。注册临时监听、重置激活期状态。 |
| `void F_Start()` | 在所有已挂载组件完成 `F_Awake` / `F_Enable` 之后、首次参与帧或物理更新前调用一次。 | 无参数。安全调用 `GetComponent<T>()` 获取同物体刚体/角色控制器，或用 `F_Scene.Find` 建立跨实体引用。 |
| `void F_PreUpdate()`<br>`void F_PreUpdate(float delta)` | 每渲染帧在常规 `F_Update` 之前调用（`TickPhase.PreTick`）。 | `delta`：经时间缩放的帧间隔秒数。适合先于业务脚本采样状态或清理逐帧缓冲。 |
| `void F_FixedUpdate()`<br>`void F_FixedUpdate(float fixedDelta)` | 按固定物理步长（默认 `1/60 s`，`TickPhase.FixedPreTick`）在物理求解前调用。 | `fixedDelta`：固定步长秒数。适合刚体 `AddForce`、`SetVelocity` 与 `F_CharacterController.Move`。 |
| `void F_Update()`<br>`void F_Update(float delta)` | 每渲染帧主逻辑阶段调用一次（`TickPhase.Tick`；若 `UpdateWhenPaused => true` 则在 `FrameBegin` 调度）。 | `delta`：经时间缩放的帧间隔秒数（等价于 `F_Time.DeltaTime`）。适合键鼠输入、旋转移动、射线检测与计时。 |
| `void F_LateUpdate()`<br>`void F_LateUpdate(float delta)` | 每渲染帧在所有脚本完成 `F_Update` 之后调用（`TickPhase.PostTick`）。 | `delta`：帧间隔秒数。适合第三人称相机跟随、武器对齐等后置位姿修正。 |
| `void F_CollisionEnter(in F_Contact contact)` | 物理步进后（`AfterPhysics`），当前实体非触发器刚体与另一刚体**开始接触**时触发。 | `contact`：真实碰撞流形（含对方 `Other`、接触点 `Point`、法线 `Normal`、冲量 `Impulse`）。 |
| `void F_CollisionStay(in F_Contact contact)` | 两个非触发器刚体**持续接触**期间，除刚触发 Enter/Exit 的步外每个物理步触发。 | `contact`：当前接触对的流形信息。 |
| `void F_CollisionExit(in F_Contact contact)` | 两个非触发器刚体**分离**时触发。 | `contact`：分离前的最后接触信息。 |
| `void F_TriggerEnter(F_Entity other)` | 另一实体**进入**当前触发器（或当前实体进入另一触发器）时触发。 | `other`：与当前实体发生重叠的对方实体。适合拾取道具、进入检查点或伤害区。 |
| `void F_TriggerStay(F_Entity other)` | 触发器**持续重叠**期间每个物理步触发（除刚进入/离开的物理步）。 | `other`：持续重叠的对方实体。 |
| `void F_TriggerExit(F_Entity other)` | 触发器重叠**结束分离**时触发。 | `other`：离开重叠区的对方实体。 |
| `void F_Disable()` | 脚本禁用、实体层级停用、发生异常熔断或即将销毁前调用。 | 无参数。暂停音效、撤销临时效果。 |
| `void F_Destroy()` | 脚本移除、实体销毁、场景卸载或停止运行预览时调用一次（若此前激活，先调 `F_Disable` 再调 `F_Destroy`）。 | 无参数。释放光标锁（`F_Cursor.Locked = false`）或外部非托管资源；事件订阅与未完成的 `F_Await` 会自动清理。 |

::: tip 更新回调的两条重要细节
1. **无参与带参二选一重写**：引擎调度时调用的是带参版本（如 `F_Update(float delta)`），基类默认实现仅做 `=> F_Update()` 转发。因此只需重写你习惯的那一个版本，**不要同时重写两个**。
2. **零开销按需注册与异常熔断**：`ScriptSystem` 在挂载时检查实际重写的回调——未重写的更新/碰撞回调不会注册原生调度任务，零额外开销；若某次回调抛出未捕获异常，引擎会在控制台记录完整堆栈并将该脚本自动标记熔断禁用（触发 `F_Disable()`），不会拖垮整帧渲染或其他脚本。
:::

### 常用实例方法

#### `GetComponent<T>`

```csharp
public T? GetComponent<T>() where T : class
```

获取当前实体上挂载的指定类型组件。支持查询同一实体上的其他 `F_Behaviour` 脚本子类、`F_RigidBody` / `F_Rigidbody` / `RigidBody` 物理刚体，以及 `F_CharacterController` / `CharacterController` 角色控制器。

- **类型参数**：`T` — 目标组件类型（引用类型）。
- **返回值**：`T?` — 找到的第一个匹配组件；未挂载时返回 `null`。

```csharp
private F_Rigidbody? _body;

public override void F_Start()
{
    _body = GetComponent<F_Rigidbody>();
}
```

#### `TryGetComponent<T>`

```csharp
public bool TryGetComponent<T>(out T? component) where T : class
```

尝试获取当前实体上的指定类型组件，并配合 C# 空状态分析（`[NotNullWhen(true)]`）省去二次判空。

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `component` | `out T?` | 找到时输出组件实例；未找到时输出 `null`。 |

- **返回值**：`bool` — 找到时返回 `true`，否则返回 `false`。

```csharp
if (TryGetComponent<F_CharacterController>(out var cc))
{
    cc.Move(Entity.Forward, jumpPressed: false, F_Time.FixedDeltaTime);
}
```

#### `GetComponents<T>`

```csharp
public IReadOnlyList<T> GetComponents<T>() where T : class
```

获取当前实体上挂载的全部指定类型组件列表（适合查询同实体上的多个脚本或按接口检索）。

- **返回值**：`IReadOnlyList<T>` — 所有匹配组件的只读列表；无匹配时返回空列表（不为 `null`）。

```csharp
foreach (var behaviour in GetComponents<F_Behaviour>())
{
    F_Log.Info($"挂载组件: {behaviour.GetType().Name}");
}
```

#### `Translate`

```csharp
public void Translate(Vector3 delta, bool localSpace = true)
```

平移当前实体（内部委托 `Entity.Translate`，若挂有刚体或角色控制器会自动安全瞬移）。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `delta` | `Vector3` | — | 位移向量（米）。局部空间下 `-Z` 为正前方、`+X` 为右侧、`+Y` 为上方。 |
| `localSpace` | `bool` | `true` | `true` 按物体自身局部朝向平移；`false` 按世界坐标系固定轴平移。 |

```csharp
// 沿物体自身正前方（局部 -Z）以 5 m/s 前进
Translate(new Vector3(0f, 0f, -5f * F_Time.DeltaTime));

// 沿世界坐标 +Y 缓慢上升
Translate(Vector3.UnitY * (1.5f * F_Time.DeltaTime), localSpace: false);
```

#### `Rotate`

```csharp
public void Rotate(Vector3 eulerDegrees, bool localSpace = true)
```

按增量欧拉角（角度制）旋转当前实体（内部委托 `Entity.Rotate`）。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `eulerDegrees` | `Vector3` | — | 绕 `X`（俯仰）、`Y`（偏航）、`Z`（翻滚）轴的增量角度（°）。 |
| `localSpace` | `bool` | `true` | `true` 绕物体自身局部轴右乘旋转；`false` 绕世界轴左乘旋转。 |

```csharp
// 每秒绕自身 Y 轴旋转 90°
Rotate(new Vector3(0f, 90f * F_Time.DeltaTime, 0f));
```

#### `LookAt`

```csharp
public void LookAt(Vector3 worldTarget, Vector3? worldUp = null)
```

旋转当前实体，使其局部前方向（`-Z`）对准给定世界空间目标点（内部委托 `Entity.LookAt`）。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `worldTarget` | `Vector3` | — | 目标点在世界空间中的坐标（米）。与自身重合时不改变朝向。 |
| `worldUp` | `Vector3?` | `null` | 参考上方向；传 `null` 时默认使用世界 `+Y`（`Vector3.UnitY`）。 |

```csharp
if (Target is { IsValid: true })
{
    LookAt(Target.WorldPosition);
}
```

#### `Instantiate`

```csharp
public Entity Instantiate(F_Entity source, Vector3? position = null, Quaternion? rotation = null)
```

深克隆给定实体及其完整子层级（包含网格、材质、灯光、物理组件以及全部挂载的脚本与 `[F_Expose]` 字段），并自动将子树内部的实体引用重映射到新克隆节点。等价于 `F_Scene.Instantiate(...)`。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `source` | `F_Entity` | — | 作为模板的源实体（不可为 `null`）。 |
| `position` | `Vector3?` | `null` | 覆盖克隆根实体的初始位置；传 `null` 保留源实体位置。 |
| `rotation` | `Quaternion?` | `null` | 覆盖克隆根实体的初始旋转四元数；传 `null` 保留源实体旋转。 |

- **返回值**：`Entity` — 新生成的克隆根实体。

```csharp
if (BulletTemplate is { IsValid: true })
{
    var muzzlePos = Entity.WorldPosition + Entity.Forward * 1.2f;
    var bullet = Instantiate(BulletTemplate, muzzlePos, Entity.WorldRotation);
    Destroy(bullet, 3.0f); // 3 秒后自动销毁
}
```

#### `Destroy`

```csharp
public void Destroy(F_Entity target, float delaySeconds = 0f)
```

通过脚本系统安全销毁指定实体及其子树，支持可选延迟销毁。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `target` | `F_Entity` | — | 要销毁的目标实体（不可为 `null`）。 |
| `delaySeconds` | `float` | `0f` | 延迟销毁秒数（受 `F_Time.TimeScale` 影响）。`<= 0f` 表示进入安全变更队列立即销毁。 |

```csharp
// 立即销毁命中目标
Destroy(other);

// 2.5 秒后自动清理特效物体
Destroy(effectClone, delaySeconds: 2.5f);
```

#### 受保护快捷方法（仅 `F_Behaviour` 子类内部可用）

| 方法签名 | 返回值 | 说明 |
| :--- | :--- | :--- |
| `protected void DestroySelf()` | `void` | 立即安全销毁当前脚本所属的实体（等价于 `Scripts.Destroy(Entity)`）。 |
| `protected IDisposable Subscribe<T>(Action<T> handler)` | `IDisposable` | 订阅事件总线中类型为 `T` 的事件；返回的令牌可提前 `.Dispose()` 退订，脚本销毁时也会自动退订。 |

```csharp
public override void F_Enable()
{
    Subscribe<ScoreChanged>(ev => F_Log.Info($"得分变动: +{ev.Delta}"));
}
```

---

## 2. `F_Entity`（兼容类 `Entity`） {#f_entity}

`F_Entity` 是场景中物体的托管代理基类；运行时实例化返回的 `Entity` 是继承自 `F_Entity` 的封闭子类（`public sealed class Entity : F_Entity`）。**新脚本暴露字段或声明变量时推荐使用 `F_Entity?`**，与引擎返回的 `Entity` 无缝兼容。

源码：[EngineSession.cs#L453-L526](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L453-L526) · [FlareErgonomics.cs#L362-L554](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L362-L554)

### 公开属性

#### 身份、生命周期与元数据

| 属性名 | 类型 | 权限 | 说明 |
| :--- | :--- | :--- | :--- |
| `StableId` | `ulong` | 只读 | 跨保存、加载与热重载保持不变的 64 位稳定唯一标识符，用于实体比对、持久化与 `F_Scene.Find(ulong)`。 |
| `Handle` | `ulong` | 只读 | 当前运行会话内的底层原生实体句柄（重启或重载场景后会变化，持久化请用 `StableId`）。 |
| `Name` | `string` | 只读 | 实体在场景层级面板中的名称。 |
| `IsValid` | `bool` | 只读 | 实体句柄当前是否可用：`!Session.IsDisposed && (IsAlive || IsPending)`。**脚本中判空判活首选 `if (target is { IsValid: true })`**。 |
| `IsAlive` | `bool` | 只读 | 实体是否已在当前世界中处于存活状态（刚在本帧延迟创建尚未提交时为 `false`，此时 `IsValid` 为 `true`）。 |
| `Enabled` | `bool` | 读写 | 实体的激活开关（对应属性面板顶部复选框）。设为 `false` 会隐藏渲染并停用本实体及子层级脚本（触发 `F_Disable`），不会销毁实体。 |
| `Tag` | `string` | 读写 | 语义标签字符串（如 `"Untagged"`、`"Player"`、`"MainCamera"`）。写入空字符串或 `null` 时自动回退为 `"Untagged"`。 |
| `Layer` | `int` | 读写 | 实体所在的物理/系统路由图层索引（`0..31`）。 |
| `Parent` | `Entity?` | 只读 | 当前父实体；位于场景根层级时返回 `null`。修改父子关系请调用 `SetParent(...)`。 |
| `Session` | `EngineSession` | 只读 | 该实体所属的原生引擎会话实例。 |

#### 空间变换与方向向量

| 属性名 | 类型 | 权限 | 单位 / 空间 | 自动刚体路由 | 说明 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `Position` | `Vector3` | 读写 | 米 / 局部 | **是** | 相对父节点的局部坐标（根实体即世界坐标）。 |
| `Rotation` | `Quaternion` | 读写 | 四元数 / 局部 | **是** | 相对父节点的局部旋转（写入时自动归一化）。 |
| `EulerAngles` | `Vector3` | 读写 | 角度 (`°`) / 局部 | **是** | 局部欧拉角（`Z → X → Y` 顺序，与属性面板一致）。 |
| `LocalScale` | `Vector3` | 读写 | 倍率 / 局部 | 否 | 相对父节点的三轴缩放倍率（默认 `1, 1, 1`）。 |
| `WorldPosition` | `Vector3` | 读写 | 米 / 世界 | **是** | 世界空间位置。有父节点时自动经父世界矩阵逆变换写入 `Position`。 |
| `WorldRotation` | `Quaternion` | 读写 | 四元数 / 世界 | **是** | 世界空间旋转四元数。有父节点时自动扣除父级世界旋转后写入 `Rotation`。 |
| `Transform` | `Transform` | 读写 | 局部完整变换 | 否 | 直接读写底层局部 `Transform`（位置、旋转、缩放）。 |
| `WorldTransform` | `Transform` | 只读 | 世界完整变换 | — | 合成全部父级层级后的世界空间完整变换。 |
| `Forward` | `Vector3` | 只读 | 单位向量 / 世界 | — | 物体局部正前方（**`-Z`**）在世界空间中的单位方向向量。 |
| `Right` | `Vector3` | 只读 | 单位向量 / 世界 | — | 物体局部右侧（**`+X`**）在世界空间中的单位方向向量。 |
| `Up` | `Vector3` | 只读 | 单位向量 / 世界 | — | 物体局部上方（**`+Y`**）在世界空间中的单位方向向量。 |

::: info 自动刚体与角色控制器安全瞬移机制
修改 `Position`、`Rotation`、`EulerAngles`、`WorldPosition`、`WorldRotation`（以及基于它们实现的 `Translate`、`Rotate`、`LookAt`）时，底层 `WriteLocalPose`（[FlareErgonomics.cs#L366](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L366)）会自动检测：
- 若实体挂有刚体（`F_RigidBody`），自动路由调用 `body.Teleport(nextTransform, clearVelocity: false)`，既同步物理位姿又保留现有速度；
- 若实体挂有角色控制器（`F_CharacterController`）且修改了位置，自动路由调用 `cc.Teleport(nextPos)`；
- 只有直接赋值 `Entity.Transform = ...` 或 `Entity.LocalScale = ...` 才会直接写底层变换组件，不经过物理 `Teleport`。
:::

### 公开方法

#### `SetParent`

```csharp
public void SetParent(F_Entity? parent, bool keepWorld = true)
```

重新挂载实体的父节点，或解绑回场景根层级。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `parent` | `F_Entity?` | — | 新父实体；传入 `null` 时将当前实体移至场景根层级。 |
| `keepWorld` | `bool` | `true` | `true` 保持实体的世界坐标/旋转/缩放不变（自动换算局部变换）；`false` 保留原局部变换数值。 |

```csharp
// 把武器挂到角色手部锚点下，并对齐锚点局部坐标
weapon.SetParent(handSocket, keepWorld: false);
weapon.Position = Vector3.Zero;

// 投掷后脱离父物体，保持当前世界位置继续飞行
weapon.SetParent(null, keepWorld: true);
```

#### `Destroy`

```csharp
public void Destroy()
```

立即通知原生世界销毁该实体及其全部子层级。

::: warning 在脚本中优先用 `F_Behaviour.Destroy(target)`
在脚本回调内部推荐调用 `F_Behaviour.Destroy(entity)` 或 `DestroySelf()`：它们会通过脚本系统变更队列有序触发 `F_Disable` / `F_Destroy` 回调并支持延时销毁；直接调用 `entity.Destroy()` 会立刻销毁原生实体，脚本清理由当帧 `SweepDead` 兜底完成。
:::

#### `CompareTag`

```csharp
public bool CompareTag(string tag)
```

按精确序数比较（区分大小写）判断当前实体的 `Tag` 是否等于指定标签（`tag` 为 `null` 时按 `"Untagged"` 处理）。

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `tag` | `string` | 目标标签名，例如 `"Player"`、`"Enemy"`。 |

- **返回值**：`bool` — 标签一致时返回 `true`。

```csharp
public override void F_TriggerEnter(F_Entity other)
{
    if (other.CompareTag("Player"))
        F_Log.Info("玩家进入触发区域");
}
```

#### `Translate` / `Rotate` / `LookAt`

```csharp
public void Translate(Vector3 delta, bool localSpace = true)
public void Rotate(Vector3 eulerDegrees, bool localSpace = true)
public void LookAt(Vector3 worldTarget, Vector3? worldUp = null)
```

与 `F_Behaviour` 上的同名方法完全一致，用于在持有其他 `F_Entity` 引用时直接驱动目标物体移动、旋转与朝向。

```csharp
if (CameraRig is { IsValid: true })
{
    CameraRig.Translate(new Vector3(0f, 0f, -2f * F_Time.DeltaTime));
    CameraRig.LookAt(Entity.WorldPosition);
}
```

#### `GetComponent<T>` / `TryGetComponent<T>` / `GetComponents<T>`

```csharp
public T? GetComponent<T>() where T : class
public bool TryGetComponent<T>(out T? component) where T : class
public IReadOnlyList<T> GetComponents<T>() where T : class
```

在**指定实体**上检索挂载的组件（支持 `F_Behaviour` 子类、`F_RigidBody` / `F_Rigidbody`、`F_CharacterController`）。

```csharp
public override void F_CollisionEnter(in F_Contact contact)
{
    if (contact.Other.TryGetComponent<HealthScript>(out var hp))
    {
        hp.TakeDamage(10);
    }
}
```

#### `ScreenPointToRay`

```csharp
public F_Ray? ScreenPointToRay(Vector2 screenPixel)
```

以**当前摄像机实体**为投影源，将视口屏幕像素坐标（左上角为 `(0, 0)`）转换为世界空间射线（委托 `F_Camera.ScreenPointToRay(screenPixel, this)`）。

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `screenPixel` | `Vector2` | 客户区像素坐标（可直接传 `F_Input.MousePosition`）。 |

- **返回值**：`F_Ray?` — 包含 `Origin` 和单位方向 `Direction` 的射线结构体；若当前实体不是有效摄像机或视口尚未就绪则返回 `null`。

```csharp
if (F_Scene.FindWithTag("MainCamera") is { } cam &&
    cam.ScreenPointToRay(F_Input.MousePosition) is { } ray)
{
    if (F_Physics.Raycast(ray.Origin, ray.Direction, 100f, out F_RaycastHit hit))
        F_Log.Info($"点中: {hit.Entity.Name}");
}
```

#### `ProjectDirection`

```csharp
public Vector2? ProjectDirection(Vector3 direction)
```

将世界空间方向向量投影到当前摄像机的归一化屏幕 UV 坐标（左上角 `(0, 0)` 到右下角 `(1, 1)`，适合将太阳或远景方向标定到 HUD）。

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `direction` | `Vector3` | 世界空间方向向量。 |

- **返回值**：`Vector2?` — 位于摄像机前方时返回归一化屏幕 UV；位于摄像机背面时返回 `null`。

#### `SetCameraFieldOfView`

```csharp
public void SetCameraFieldOfView(float radians)
```

动态修改当前摄像机实体的垂直视场角（适合开镜缩放、冲刺 FOV 拉宽）。

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `radians` | `float` | 垂直视场角（**弧度制**，可用角度乘 `F_Math.Deg2Rad` 转换）。 |

```csharp
// 将主相机垂直 FOV 设为 60°
cameraEntity.SetCameraFieldOfView(60f * F_Math.Deg2Rad);
```

#### `SetMesh`

```csharp
public void SetMesh(string assetId, bool castsShadow = true, bool receivesShadow = true)
```

在运行期为实体挂载或替换静态网格资产。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `assetId` | `string` | — | 已导入网格资产的逻辑 ID（可在 `Scripts.Session.AssetIds` 或内容浏览器查看）。 |
| `castsShadow` | `bool` | `true` | 是否向阴影贴图投射阴影。 |
| `receivesShadow` | `bool` | `true` | 表面是否接收其他物体投射的阴影。 |

#### `PlayAnimation`

```csharp
public void PlayAnimation(string clip, float speed = 1f, bool loop = false)
```

在挂载了骨骼动画模型的角色实体上播放指定动画片段。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `clip` | `string` | — | 导入模型资产（FBX / GLB）内的动画片段名称。 |
| `speed` | `float` | `1f` | 播放倍率（`1f` 为原速，`0f` 可定格暂停当前动画）。 |
| `loop` | `bool` | `false` | `true` 循环播放；`false` 播放一遍停在尾帧。 |

```csharp
Entity.PlayAnimation("Run", speed: 1.2f, loop: true);
```

::: info 如何停止或切换骨骼动画？
当前版本 `F_Entity` 没有独立的 `StopAnimation` 方法：需要停止动作时，可调用 `PlayAnimation("Idle", speed: 1f, loop: true)` 切回待机片段，或传入 `speed: 0f` 将当前片段定格在当前姿态。
:::

#### `JointPosition`

```csharp
public Vector3? JointPosition(string joint)
```

查询角色骨骼中指定名称关节在当前帧的世界空间坐标（常用于武器挂点、脚步特效或射线起点）。

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `joint` | `string` | 骨骼节点名称（如 `"mixamorig:RightHand"`）。 |

- **返回值**：`Vector3?` — 找到关节时返回其世界坐标（米）；未挂骨骼或关节名不存在时返回 `null`。

```csharp
if (Entity.JointPosition("RightHand") is { } handPos && MuzzleEffect is { IsValid: true })
{
    MuzzleEffect.WorldPosition = handPos;
}
```

---

## 3. 核心值结构体

### `F_Transform`（兼容 `Transform`） {#f_transform}

按顺序布局（`[StructLayout(LayoutKind.Sequential)]`）的三维空间变换结构体，与原生 C++ 变换内存布局一一对应。`F_Transform` 与旧版 `Transform` 支持**双向隐式转换**，赋值无需强转。

源码：[EngineSession.cs#L15-L41](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L15-L41)

| 成员名 | 类型 | `Identity` 值 | 说明 |
| :--- | :--- | :--- | :--- |
| `Position` | `Vector3` | `(0, 0, 0)` | 三维位置坐标（米）。 |
| `Rotation` | `Quaternion` | `(0, 0, 0, 1)` | 单位四元数旋转（`Quaternion.Identity`）。 |
| `Scale` | `Vector3` | `(1, 1, 1)` | 三轴缩放倍率。 |
| `Identity` (`static`) | `F_Transform` | 见上 | 原点、无旋转、1 倍缩放的标准单位变换。 |

::: warning 不要使用 `new F_Transform()` / `default`
C# 结构体零初始化会让 `Rotation` 变成无效的全零四元数 `(0,0,0,0)`、让 `Scale` 变成 `(0,0,0)`（模型缩成不可见的一点）。构造变换时务必从 **`F_Transform.Identity`** 起步：
:::

```csharp
var pose = F_Transform.Identity;
pose.Position = new Vector3(0f, 1.5f, -3f);
pose.Rotation = F_Quat.Euler(0f, 45f, 0f);
pose.Scale = new Vector3(1.2f, 1.2f, 1.2f);
Entity.Transform = pose; // 隐式转换为 Transform
```

---

### `F_EntityReference`（兼容 `EntityReference`） {#f_entityreference}

封装 64 位持久化 `StableId` 的只读值类型（`public readonly record struct F_EntityReference(ulong StableId)`），与 `EntityReference` 支持双向隐式转换。

源码：[Behaviour.cs#L55-L80](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L55-L80)

| 成员签名 | 类型 / 返回值 | 默认值 / 说明 |
| :--- | :--- | :--- |
| `ulong StableId` | `ulong` | 默认 `0`（空引用）。绑定的场景实体持久化 ID。 |
| `bool IsValid` | `bool` | 等价于 `StableId != 0`（仅判断槽位是否已赋值，不代表目标此刻一定存活）。 |
| `Entity? Resolve()` | `Entity?` | 在当前活动 `ScriptSystem` 会话中解析运行时实体；未赋值或目标已销毁返回 `null`。 |
| `Entity? Resolve(EngineSession session)` | `Entity?` | 在指定 `EngineSession` 中解析运行时实体。 |
| `static F_EntityReference From(F_Entity? entity)` | `F_EntityReference` | 从实体构造稳定引用；传入 `null` 返回 `StableId = 0`。 |

```csharp
[F_Expose] public F_EntityReference SpawnPointRef;

public override void F_Start()
{
    if (SpawnPointRef.Resolve() is { IsValid: true } spawnPoint)
    {
        Entity.WorldPosition = spawnPoint.WorldPosition;
    }

    // 从实体构造引用
    SpawnPointRef = F_EntityReference.From(Entity);
}
```

::: tip `F_Entity?` vs `F_EntityReference` 怎么选？
- **常规脚本暴露字段推荐直接写 `[F_Expose] public F_Entity? Target;`**——编辑器同样渲染实体拖拽槽与 `⊙` 选择器，保存时自动存为 `StableId`，加载时自动解析为运行时对象，省去每次手写 `.Resolve()`。
- **在以下场景使用 `F_EntityReference` / `EntityReference`**：需要在 `[F_Event]` 载荷结构体里传递实体标识（注意 `[F_Event]` 载荷属性需声明为 `EntityReference`）、需要把 ID 放进字典键、或需要跨场景保留原始 `StableId` 数值时。
:::

---

### `F_Color` {#f_color}

线性空间 RGBA 浮点颜色记录结构体（`public record struct F_Color(float R, float G, float B, float A = 1f)`）。在脚本字段上加 `[F_Expose]` 会在编辑器属性面板自动显示颜色色块与 HSV 拾色器。

源码：[Behaviour.cs#L39-L52](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L39-L52)

| 成员签名 | 类型 | 默认值 / 范围 | 说明 |
| :--- | :--- | :--- | :--- |
| `float R` | `float` | 常规 `0..1`，HDR 可 `> 1` | 线性红色分量。 |
| `float G` | `float` | 常规 `0..1`，HDR 可 `> 1` | 线性绿色分量。 |
| `float B` | `float` | 常规 `0..1`，HDR 可 `> 1` | 线性蓝色分量。 |
| `float A` | `float` | 构造默认 `1f`（范围 `0..1`） | 不透明度 Alpha。 |
| `Vector3 ToVector3()` | `Vector3` | — | 提取 `new Vector3(R, G, B)`。 |
| `Vector4 ToVector4()` | `Vector4` | — | 提取 `new Vector4(R, G, B, A)`（可直接传给 `F_UiElement.SetColor`）。 |

#### 内置静态预设色

| 静态属性 | `(R, G, B, A)` 精确值 | 颜色 |
| :--- | :--- | :--- |
| `F_Color.White` | `(1f, 1f, 1f, 1f)` | 纯白 |
| `F_Color.Black` | `(0f, 0f, 0f, 1f)` | 纯黑 |
| `F_Color.Red` | `(1f, 0f, 0f, 1f)` | 纯红 |
| `F_Color.Green` | `(0f, 1f, 0f, 1f)` | 纯绿 |
| `F_Color.Blue` | `(0f, 0f, 1f, 1f)` | 纯蓝 |
| `F_Color.Yellow` | `(1f, 0.92f, 0.016f, 1f)` | 亮黄 |
| `F_Color.Cyan` | `(0f, 1f, 1f, 1f)` | 青色 |

```csharp
[F_Expose] public F_Color Tint = F_Color.Cyan;
[F_Expose] public F_Color CustomOrange = new(1f, 0.45f, 0.08f, 1f);

public override void F_Start()
{
    Vector4 rgba = Tint.ToVector4();
    Vector3 rgb = CustomOrange.ToVector3();
}
```

::: info 碰撞接触结构体 `F_Contact` 速览
`F_CollisionEnter / Stay / Exit(in F_Contact contact)` 的参数 `F_Contact` 定义于 [Behaviour.cs#L83](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L83)，包含 4 个只读字段：`F_Entity Other`（对方实体）、`Vector3 Point`（世界接触点）、`Vector3 Normal`（指向当前实体的单位接触法线）、`float Impulse`（法向冲量大小）。完整物理碰撞与射线查询详见 [物理篇](physics.md)。
:::

---

## 4. `F_Scene`（场景查询、克隆与重载） {#f_scene}

静态工具类，封装当前活动运行场景中最高频的实体查找、组件查找、深克隆实例化与场景重载操作。

源码：[FlareErgonomics.cs#L629-L706](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L629-L706)

### `F_Scene.Find(ulong)`

```csharp
public static Entity? Find(ulong stableId)
```

按 64 位持久化 `StableId` 查找当前场景中的存活实体。

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `stableId` | `ulong` | 目标实体的 `StableId`。 |

- **返回值**：`Entity?` — 找到时返回实体；不存在或未进入运行态时返回 `null`。

### `F_Scene.Find(string)`

```csharp
public static Entity? Find(string name)
```

按实体名称查找当前场景中第一个匹配的存活实体。

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `name` | `string` | 实体名称（空字符串或 `null` 直接返回 `null`）。 |

- **返回值**：`Entity?` — 首个同名存活实体；未找到返回 `null`。

```csharp
if (F_Scene.Find("Boss") is { IsValid: true } boss)
{
    LookAt(boss.WorldPosition);
}
```

### `F_Scene.FindWithTag`

```csharp
public static Entity? FindWithTag(string tag)
```

按语义标签（`Tag`）查找当前场景中第一个匹配的存活实体。

| 参数 | 类型 | 说明 |
| :--- | :--- | :--- |
| `tag` | `string` | 目标标签（如 `"Player"`、`"MainCamera"`；空字符串直接返回 `null`）。 |

- **返回值**：`Entity?` — 首个匹配标签的存活实体；未找到返回 `null`。

```csharp
var player = F_Scene.FindWithTag("Player");
```

### `F_Scene.FindObjectOfType<T>`

```csharp
public static T? FindObjectOfType<T>() where T : class
```

查找当前场景中第一个**同时满足脚本已启用、实体存活且实体已启用**的指定类型脚本实例（仅扫描已挂载的 `F_Behaviour`）。

- **类型参数**：`T` — 脚本类或脚本实现的接口类型。
- **返回值**：`T?` — 找到的首个活跃脚本实例；未找到返回 `null`。

```csharp
var gameManager = F_Scene.FindObjectOfType<GameManager>();
gameManager?.AddScore(100);
```

### `F_Scene.FindObjectsOfType<T>`

```csharp
public static IReadOnlyList<T> FindObjectsOfType<T>() where T : class
```

查找当前场景中所有**处于活跃启用状态**的指定类型脚本实例列表。

- **返回值**：`IReadOnlyList<T>` — 全部匹配的活跃脚本只读列表；无匹配时返回空列表。

::: tip 性能提示
`Find(string)`、`FindWithTag`、`FindObjectOfType<T>` 和 `FindObjectsOfType<T>` 都会遍历场景实体或已挂载脚本，建议在 `F_Start()` 里缓存结果，避免在 `F_Update` 中高频调用。
:::

### `F_Scene.Instantiate`

```csharp
public static Entity Instantiate(F_Entity source, Vector3? position = null, Quaternion? rotation = null)
```

深克隆指定实体及其完整子层级，并按以下规则自动处理脚本与引用：
1. 复制原生实体树（网格、材质、灯光、物理组件等）；
2. 为克隆出的每个节点重新实例化源节点上的所有 `F_Behaviour` 脚本，保留各自的 `Enabled` 状态；
3. 复制全部 `[F_Expose]` 字段；**如果某个 `F_Entity?` / `F_EntityReference` 字段指向被克隆子树内部的其他节点，会自动重映射到新克隆树中的对应节点**（指向子树外部的引用保持原目标）。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `source` | `F_Entity` | — | 源实体模板（不可为 `null`）。 |
| `position` | `Vector3?` | `null` | 覆盖克隆根实体的初始位置；`null` 保留源位置。 |
| `rotation` | `Quaternion?` | `null` | 覆盖克隆根实体的初始旋转；`null` 保留源旋转。 |

- **返回值**：`Entity` — 克隆得到的根实体。若源实体无效会抛出 `EngineException("Bridge.InvalidEntity")`。

```csharp
var enemy = F_Scene.Instantiate(enemyPrefab, new Vector3(10f, 0f, -5f), F_Quat.Euler(0f, 180f, 0f));
```

### `F_Scene.Reload`

```csharp
public static void Reload()
```

请求引擎在**当前帧结束时**重新加载当前活动场景（常用于玩家阵亡后“一键重开本关”）。

```csharp
if (F_Input.KeyDown(F_Key.R))
{
    F_Scene.Reload();
}
```

---

### 进阶：多场景流式管理（`SceneService` 与 `Scene`） {#scene-streaming}

当项目需要按文件或 JSON 字符串异步加载多关卡、实现替换加载或叠加（Additive）分区流式加载时，可通过 `Scripts.Session.Scenes`（`SceneService`）操作底层场景句柄。

源码：[Scenes.cs#L8-L59](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Scenes.cs#L8-L59)

#### 核心类型速查

| 类型 | 类别 | 关键成员 | 说明 |
| :--- | :--- | :--- | :--- |
| `SceneService` | 类 | `Active` (`Scene?`)、`Create(string name)`、`Get(SceneHandle)`、`Of(Entity)`、`Statistics` (`SceneStatistics`) | 挂在 `EngineSession.Scenes`，管理所有场景槽位与统计。 |
| `Scene` | 类 | `Handle`、`Scope`、`Snapshot` (`SceneSnapshot`)、`RequestLoad(path, mode, replacing)`、`RequestJson(json, mode, replacing)`、`Cancel()`、`Unload()`、`CreateEntity(name)` | 单个场景槽位的控制句柄。 |
| `SceneHandle` | `readonly record struct` | `ulong Value` | 场景槽位唯一句柄。 |
| `SceneLoadMode` | 枚举 | `Replace`、`Additive` | 替换现有场景，或以叠加模式并存加载。 |
| `SceneState` | 枚举 | `Unloaded`、`Preparing`、`ReadyToActivate`、`Active`、`Unloading`、`Failed`、`Cancelled` | 场景异步加载生命周期状态。 |
| `SceneSnapshot` | `record` | `Id`、`State`、`Active`、`Request`、`Revision`、`Scope`、`Name`、`Source`、`RequestedSource`、`Error`、`Members` (`ulong[]`) | 场景当前完整状态与成员实体句柄快照。 |
| `SceneStatistics` | `record` | `Requested`、`Activated`、`Unloaded`、`Failed`、`Cancelled`、`StaleResults`、`CallbackFaults`、`NotificationFailures`、`PendingJobs`、`Records`、`Members` | 场景流式加载诊断统计。 |
| `SceneActivated`<br>`SceneUnloaded`<br>`SceneLoadFailed` | 事件结构体 | `Scene` (`SceneHandle`)、`Request` (`ulong`)（失败事件另含 `Error`） | 可通过 `Subscribe<SceneActivated>(...)` 订阅的场景切换事件。 |

```csharp
// 叠加加载一个子区域场景，并在激活后收到事件通知
public override void F_Start()
{
    Subscribe<SceneActivated>(ev => F_Log.Info($"场景已激活: #{ev.Scene.Value}"));

    var scenes = Scripts.Session.Scenes;
    var subScene = scenes.Create("SubArea");
    subScene.RequestLoad("Scenes/SubArea.flare-scene", SceneLoadMode.Additive);
}
```

---

## 5. 编辑器序列化与面板特性（Attributes） {#attributes}

FlareEngine 提供 8 个特性（Attribute），用于控制字段在右侧 **属性（Inspector）** 面板中的显示、校验与持久化序列化，以及稳定脚本 ID 和跨脚本事件载荷声明。

源码：[Behaviour.cs#L6-L36](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L6-L36) · [ScriptFields.cs#L49-L124](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L49-L124) · [ScriptEvents.cs#L16-L18](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L16-L18)

### 8 项特性总览

| 特性名 | 构造函数签名 | 允许修饰目标 | 面板呈现 / 核心作用 |
| :--- | :--- | :--- | :--- |
| `[F_Expose]` | `F_ExposeAttribute()` | 字段 (`Field`) | 将实例字段暴露到属性面板并随场景自动保存（支持运行期热调参）。 |
| `[F_Header]` | `F_HeaderAttribute(string title)` | 字段 (`Field`) | 在当前字段上方绘制分组小标题（`Subheader`）。 |
| `[F_Range]` | `F_RangeAttribute(float min, float max)` | 字段 (`Field`) | 对标量数值字段限制闭区间 `[min, max]`；`max > min` 时渲染为可拖拽滑条。 |
| `[F_Tooltip]` | `F_TooltipAttribute(string text)` | 字段 (`Field`) | 鼠标悬停在该字段上时弹出中文/英文提示说明气泡。 |
| `[F_Space]` | `F_SpaceAttribute(float pixels = 8f)` | 字段 (`Field`) | 在字段上方留出垂直间距（像素值夹取在 `0..96`）。 |
| `[F_ReadOnly]` | `F_ReadOnlyAttribute()` | 字段 (`Field`) | 在属性面板中锁定为只读显示（禁止手动编辑，运行态仍实时刷新数值）。 |
| `[F_Script]` | `F_ScriptAttribute(string id)` | 类 (`Class`) | 显式指定脚本的稳定持久化类型 ID，重命名类名时不丢失场景绑定。 |
| `[F_Event]` | `F_EventAttribute(string key, uint version = 1)` | 结构体 (`Struct`) | 标记值类型结构体为可在 `Scripts.Events` 发布与订阅的强类型事件载荷。 |

---

### `[F_Expose]` 与支持的字段类型清单

```csharp
[AttributeUsage(AttributeTargets.Field)]
public class F_ExposeAttribute : Attribute
```

- **修饰规则**（核对自 [ScriptFields.cs#L49-L62](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L49-L62)）：
  - `public` 和 `private` 实例字段**均支持**暴露（不加 `[F_Expose]` 的 `public` 字段默认**不会**出现在属性面板）。
  - 会自动忽略 `static` 字段与 `readonly`（`init-only`）字段。
  - **继承链上不允许定义同名暴露字段**，否则反射描述时会抛出 `InvalidOperationException("Script.FieldTypeChanged")`。
  - 面板中字段默认按字段名字典序稳定排列。

#### 支持暴露的完整数据类型表（10 类）

核对自 [ScriptFields.cs#L63-L107](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L63-L107) 与 [EditorScriptWorkflow.cpp#L1142-L1227](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptWorkflow.cpp#L1142-L1227)：

| C# 字段类型 | 序列化类型键 (`Kind`) | 属性面板控件 | 校验与限制 |
| :--- | :--- | :--- | :--- |
| `int` | `"i32"` | 整数输入框（配合 `[F_Range]` 变整数滑条，拖拽步长 `1`） | 32 位有符号整数。 |
| `float` | `"f32"` | 浮点拖拽框（配合 `[F_Range]` 变连续滑条） | 须为有限值，拒绝 `NaN` / `Infinity`。 |
| `double` | `"f64"` | 双精度浮点拖拽框（配合 `[F_Range]` 变连续滑条） | 须为有限值，拒绝 `NaN` / `Infinity`。 |
| `bool` | `"bool"` | 复选框（Checkbox） | `true` / `false`。 |
| `string` | `"string"` | 文本输入框 | 允许 `null`，最大长度 **65536** 字符。 |
| `Vector2` | `"vec2"` | `X` / `Y` 双轴拖拽数字栏（步长 `0.05`） | 各分量须为有限浮点数。 |
| `Vector3` | `"vec3"` | `X` / `Y` / `Z` 三轴拖拽数字栏（步长 `0.05`） | 各分量须为有限浮点数。 |
| `F_Color` | `"color"` | 颜色色块 + HSV 拾色器 | 存储线性 `R/G/B/A`（缺省 `A = 1f`），各分量须有限。 |
| 任意 `enum` | `"enum"` | 下拉选择框（Combo，列出全部枚举名） | 按枚举成员名字符串序列化，严格区分大小写。 |
| `F_Entity?` / `Entity?` / `F_EntityReference` / `EntityReference` | `"entity"` | 实体引用槽（支持从场景层级直接拖拽，或点 `⊙` 选择器搜索） | 底层存 64 位 `StableId`；所引用目标随场景卸载时自动清空置零，避免悬空引用。 |

::: warning 不在清单内的类型会被忽略
例如 `List<T>`、数组、`Quaternion`、自定义 `class` 或嵌套 `struct` 目前不参与 `[F_Expose]` 序列化，不会显示在属性面板中。
:::

---

### `[F_Header]`、`[F_Range]`、`[F_Tooltip]`、`[F_Space]`、`[F_ReadOnly]`

这 5 个修饰特性需与 `[F_Expose]` **配合使用**（单独贴在未暴露的字段上不会生效）：

- **`[F_Header(string title)]`**（属性：`Title`）
  - 在当前字段上方插入一行分组小标题。若同时贴了 `[F_Space]`，空白间距会优先加在小标题上方，视觉分组更自然。
- **`[F_Range(float min, float max)]`**（属性：`Min`、`Max`）
  - 适用于标量数值字段（`int`、`float`、`double`）。
  - 当 `max > min` 时，面板自动把数字框切换为滑条控件；浮点滑条拖拽灵敏度自动按 `max(0.001, (max - min) * 0.005)` 计算，整数步长固定为 `1`。
- **`[F_Tooltip(string text)]`**（属性：`Text`）
  - 鼠标悬停在属性行上时弹出提示说明；若未指定，数值型字段默认显示左右拖拽调参提示。
- **`[F_Space(float pixels = 8f)]`**（属性：`Pixels`，默认 `8f`）
  - 在当前字段（或它上方的 `[F_Header]`）之前插入垂直空白间距；渲染时自动夹取在 `0..96` 像素区间内。
- **`[F_ReadOnly]`**
  - 将字段在面板中设为不可编辑的只读状态，但运行预览期间仍实时刷新该字段的当前值，非常适合直接观察血量、冷却倒计时、命中次数等运行时状态。

---

### `[F_Script]`

```csharp
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public class F_ScriptAttribute(string id) : Attribute
```

| 参数 | 类型 | 必填 | 说明 |
| :--- | :--- | :--- | :--- |
| `id` | `string` | 是 | 持久化稳定脚本类型标识符。长度 `1..256`，不得为空白或含控制字符，全项目唯一。 |

- **默认行为**：不贴 `[F_Script]` 时，引擎自动使用 `type.FullName ?? type.Name` 作为绑定 ID（见 [Behaviour.cs#L217](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L217)）。
- **最佳实践**：日常新建脚本无需手写 `[F_Script]`；**只有当你要把已有脚本改名或挪进新命名空间时**，在类上补贴 `[F_Script("旧类型完整名")]`，即可保证所有已保存场景中的组件挂载与字段配置无损迁移。

```csharp
// 原类名叫 PlayerMover，重构改名为 HeroController 但保留原场景绑定：
[F_Script("PlayerMover")]
public class HeroController : F_Behaviour
{
    [F_Expose] public float MoveSpeed = 6f;
}
```

---

### `[F_Event]`

```csharp
[AttributeUsage(AttributeTargets.Struct, Inherited = false)]
public class F_EventAttribute(string key, uint version = 1) : Attribute
```

标记一个值类型结构体为可通过 `Scripts.Events.Publish<T>(...)` 与 `Subscribe<T>(...)` 跨脚本收发的强类型事件载荷。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `key` | `string` | — | 全局唯一事件类型键。长度 `1..128`，仅允许字符 `a-z`、`A-Z`、`0-9`、`.`、`_`、`:`、`-`。 |
| `version` | `uint` | `1` | 载荷协议版本号（须 `>= 1`），用于拒绝不兼容版本的数据包。 |

::: warning 自定义事件结构体的 3 条硬性规则（核对自 [ScriptEvents.cs#L148-L180](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L148-L180)）
违反任一条在首次发布/订阅时会抛出 `ArgumentException("Event.UnsupportedPayload")`：
1. **必须是值类型 `struct`**（不能是 `class`）。
2. **禁止任何 `public` 实例字段**——所有载荷成员必须写成带 `get` 和 `set`（或 `init`）的公开属性（且不能是索引器）。
3. **属性类型仅限标量白名单**：`bool`、`int`、`uint`、`long`、`ulong`、`float`、`double`、`string`、任意 `enum`、`EntityReference`（注意：传递实体请用 `EntityReference`，不能直接放 `F_Entity` 或 `Vector3`）。
:::

```csharp
[F_Event("game.damage_dealt", version: 1)]
public struct DamageDealtEvent
{
    public EntityReference Attacker { get; set; }
    public EntityReference Victim { get; set; }
    public int Amount { get; set; }
}

public class CombatListener : F_Behaviour
{
    public override void F_Enable()
    {
        Subscribe<DamageDealtEvent>(ev =>
        {
            var victimName = ev.Victim.Resolve()?.Name ?? "未知目标";
            F_Log.Info($"{victimName} 受到 {ev.Amount} 点伤害");
        });
    }

    public void DealHit(F_Entity victim, int dmg)
    {
        Scripts.Events.Publish(new DamageDealtEvent
        {
            Attacker = F_EntityReference.From(Entity),
            Victim = F_EntityReference.From(victim),
            Amount = dmg
        });
    }
}
```

---

## 综合示例：可直接复制运行的核心 API 模板

下面这份脚本一次性展示了 `F_Behaviour` 生命周期、`F_Entity` 变换与标签、组件检索、`F_Scene` 查找/克隆/重载、核心结构体和全部面板特性：

```csharp
using System.Numerics;
using FlareEngine;

public enum TurretMode { Idle, Tracking, Firing }

[F_Event("turret.fired", version: 1)]
public struct TurretFiredEvent
{
    public EntityReference Turret { get; set; }
    public int ShotIndex { get; set; }
}

public class AutoTurret : F_Behaviour
{
    [F_Header("基础参数")]
    [F_Expose, F_Range(1f, 30f), F_Tooltip("索敌最大距离（米）")]
    public float DetectRange = 12f;

    [F_Expose, F_Range(0.1f, 5f)]
    public float FireInterval = 0.8f;

    [F_Expose] public TurretMode Mode = TurretMode.Tracking;
    [F_Expose] public F_Color LaserColor = F_Color.Red;

    [F_Space(10f)]
    [F_Header("场景对象引用（直接从场景层级拖入）")]
    [F_Expose, F_Tooltip("拖入子弹模板实体")]
    public F_Entity? ProjectileTemplate;

    [F_Expose, F_ReadOnly, F_Tooltip("运行态累计开火次数（只读）")]
    public int ShotCount = 0;

    private F_Entity? _player;
    private float _cooldown;

    public override void F_Start()
    {
        _player = F_Scene.FindWithTag("Player");
        Subscribe<TurretFiredEvent>(ev => F_Log.Info($"炮塔第 {ev.ShotIndex} 次开火"));
    }

    public override void F_Update(float delta)
    {
        if (Mode == TurretMode.Idle || _player is not { IsValid: true })
            return;

        float dist = Vector3.Distance(Entity.WorldPosition, _player.WorldPosition);
        if (dist > DetectRange) return;

        LookAt(_player.WorldPosition);

        _cooldown -= delta;
        if (Mode == TurretMode.Firing && _cooldown <= 0f && ProjectileTemplate is { IsValid: true })
        {
            _cooldown = FireInterval;
            ShotCount++;

            var spawnPos = Entity.WorldPosition + Entity.Forward * 1.1f;
            var shot = Instantiate(ProjectileTemplate, spawnPos, Entity.WorldRotation);
            if (shot.TryGetComponent<F_Rigidbody>(out var rb))
                rb.Velocity = Entity.Forward * 18f;

            Destroy(shot, delaySeconds: 3f);

            Scripts.Events.Publish(new TurretFiredEvent
            {
                Turret = F_EntityReference.From(Entity),
                ShotIndex = ShotCount
            });
        }
    }
}
```

---

## 继续查阅其他章节

- [API 总览与速查导航](index.md)
- [输入、光标、相机、时间、数学与异步](input-math.md)
- [物理、射线、碰撞流形与角色控制](physics.md)
- [音频、粒子、导航、UI、事件、昼夜与画质](systems.md)
