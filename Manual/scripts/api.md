# 高频游戏 API：统一 `F_` SDK 全景

`FlareEngine` 命名空间为日常玩法开发提供了统一的 **`F_` 前缀 SDK 体系**。所有空间运算与引擎右手坐标系严格对齐：**局部前方向 `-Z`、上方向 `+Y`、右方向 `+X`，欧拉角为角度制 `Z → X → Y`**。

## 零分配继承链与向下兼容设计

为了让新代码统一使用 `F_` 前缀、同时保持既有无前缀脚本零修改可运行，SDK 对引用类型与值类型分别采用了两种零开销策略：

```text
【引用类型：零分配单实例继承链】
  F_Entity ◀────────────────────── Entity               (运行期只分配 1 个 Entity 实例)
  F_RigidBody ◀── F_Rigidbody ◀─── RigidBody            (GetComponent<三者任一> 返回同一对象)
  F_CharacterController ◀───────── CharacterController
  F_Audio ◀─────────────────────── Audio
  F_Particles ◀─────────────────── Particles
  F_Navigation ◀────────────────── Navigation
  F_InputActions ◀──────────────── InputActions
  F_GameUi ◀────────────────────── GameUi
  F_UiElement ◀─────────────────── UiElement

【值类型：同布局 struct + 双向 implicit operator】
  F_Transform ◀─────────▶ Transform
  F_EntityReference ◀───▶ EntityReference
  F_RayHit ◀──▶ F_RaycastHit ◀──▶ RayHit
  F_BoxBodyDescription ◀─▶ BoxBodyDescription
  F_SphereBodyDescription ◀─▶ SphereBodyDescription
  F_RigidBodyState ◀────▶ RigidBodyState
  F_CharacterDescription ◀─▶ CharacterDescription
  F_CharacterState ◀────▶ CharacterState
  F_AudioPlayDescription ◀─▶ AudioPlayDescription
  F_AudioVoiceHandle ◀──▶ AudioVoiceHandle
  F_NavPath ◀───────────▶ NavPath
```

这意味着无论你在 `GetComponent<T>()` 中写 `F_RigidBody`、`F_Rigidbody` 还是 `RigidBody`，底层返回的都是同一个 `RigidBody` 实例，不会产生额外包装对象分配（[`Physics.cs#L153-L196`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Physics.cs#L153)）。

## F_Entity、F_Transform 与 F_EntityReference

[`F_Entity`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L453)（及其运行期派生类 `Entity`、扩展定义在 [`FlareErgonomics.cs#L362`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L362)）封装实体的身份、变换、层级、标签/图层、动画、网格与组件查询：

| 分类 | 成员 | 说明 |
| :--- | :--- | :--- |
| 身份与状态 | `Name`、`StableId`、`Handle`、`Enabled`、`IsAlive`、`IsValid` | `IsValid` 对本帧刚创建尚未刷入世界的待激活实体也返回 `true`。 |
| 标签与图层 | `Tag`、`Layer`（0..31）、`CompareTag(string)` | 语义分组与物理/系统路由过滤。 |
| 局部/世界变换 | `Transform`、`WorldTransform`、`Position`、`Rotation`、`LocalScale`、`EulerAngles`、`WorldPosition`、`WorldRotation` | 挂有物理刚体或角色控制器时，写 `Position` / `Rotation` / `EulerAngles` **自动路由到底层 `Teleport`**。 |
| 方向与层级 | `Forward`（`-Z`）、`Right`（`+X`）、`Up`（`+Y`）、`Parent`、`SetParent(F_Entity?, bool keepWorld = true)` | 均为世界空间单位向量。 |
| 网格/动画/相机 | `SetMesh(...)`、`PlayAnimation(clip, speed, loop)`、`JointPosition(joint)`、`SetCameraFieldOfView(rad)`、`ScreenPointToRay(pixel)`、`ProjectDirection(dir)` | 运行时换模、骨骼动画播放、关节挂点查询与相机投影。 |

[`F_Transform`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L15) 为 sequential 40 字节结构体（`Position` + `Rotation` + `Scale`，默认 `F_Transform.Identity`）。[`F_EntityReference`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L55) 保存 64 位 `StableId`，提供 `IsValid`、`Resolve()` 与 `From(F_Entity?)`；在脚本暴露字段中也可以直接声明 `public F_Entity? Target;`（详见[反射与寿命](lifetime.md)）。

```csharp
// Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L366-L392
private void WriteLocalPose(Vector3? position, Quaternion? rotation)
{
    var current = Transform;
    var nextPos = position ?? current.Position;
    var nextRot = rotation.HasValue ? Quaternion.Normalize(rotation.Value) : current.Rotation;
    if (Session.Physics.GetBody(AsEntity()) is { } body)
    {
        current.Position = nextPos; current.Rotation = nextRot;
        body.Teleport(current, clearVelocity: false); // 避免 Bridge.PhysicsOwnsTransform 报错
        return;
    }
    if (position.HasValue && TryGetComponent<CharacterController>(out var cc))
    {
        cc.Teleport(nextPos);
        if (rotation.HasValue) { current = Transform; current.Rotation = nextRot; Transform = current; }
        return;
    }
    current.Position = nextPos; current.Rotation = nextRot; Transform = current;
}
```

### Translate

- `void Translate(Vector3 delta, bool localSpace = true)`：默认沿自身朝向局部平移；传 `localSpace: false` 沿世界坐标平移（[`FlareErgonomics.cs#L486`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L486)）。
- `void Rotate(Vector3 eulerDegrees, bool localSpace = true)`：按角度制欧拉角增量旋转（[`#L495`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L495)）。
- `void LookAt(Vector3 worldTarget, Vector3? worldUp = null)`：旋转实体使局部 `-Z` 对准世界目标点（[`#L505`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L505)）。

三者在 `F_Behaviour` 上同样有直接转发版本。

### GetComponent

`T? GetComponent<T>() where T : class`（[`FlareErgonomics.cs#L516-L532`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L516)）支持在同一实体上查找 `F_RigidBody` / `F_Rigidbody`、`F_CharacterController` 或任意 `F_Behaviour` 子类/接口，并配套提供 `TryGetComponent<T>(out T?)` 与 `GetComponents<T>()`：

```csharp
public T? GetComponent<T>() where T : class
{
    if (typeof(F_RigidBody).IsAssignableFrom(typeof(T)))
        return Session.Physics.GetBody(AsEntity()) as T;
    if (typeof(F_CharacterController).IsAssignableFrom(typeof(T)))
    {
        try { return CharacterController.FromComponent(AsEntity()) as T; }
        catch (EngineException) { return null; }
    }
    if (Session.EnvironmentOwner is ScriptSystem sys)
    {
        var id = StableId;
        foreach (var b in sys.AllAttachedBehaviours) // 含同帧刚挂载的 pendingAttachments
            if (b.Entity.StableId == id && b is T typed) return typed;
    }
    return null;
}
```

## F_Scene

[`F_Scene`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L629) 提供运行期实体检索、脚本服务定位、子树深克隆与场景重载：

- `F_Scene.Find(ulong stableId)` / `F_Scene.Find(string name)` / `F_Scene.FindWithTag(string tag)`：查找存活实体。
- `F_Scene.FindObjectOfType<T>()` / `F_Scene.FindObjectsOfType<T>()`：查找场景中处于启用状态的脚本实例或接口实现。
- `F_Scene.Reload()`：登记重载请求，在当前帧步进结束后的 `PostStep` 安全点还原启动场景快照并重建 `ScriptSystem`（[`ScriptProject.cs#L43-L54`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptProject.cs#L43)）。

### Instantiate

[`Entity Instantiate(F_Entity source, Vector3? position = null, Quaternion? rotation = null)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L673) 对源实体执行**完整子树深度克隆**：原生侧复制整棵父子层级、网格/材质槽、灯光、相机与全部特性组件；托管侧克隆子树内所有 `F_Behaviour` 并把子树内部的实体引用字段自动重映射到对应克隆体：

```csharp
// Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L680-L705
var clonedSubtree = scripts.Session.DuplicateEntity(source, position, rotation);
Dictionary<ulong, ulong> remap = new(clonedSubtree.Count);
foreach (var (srcStableId, clonedEntity) in clonedSubtree)
    remap[srcStableId] = clonedEntity.StableId;

var existingAttachments = scripts.AllAttachedBehaviours;
foreach (var (srcStableId, clonedEntity) in clonedSubtree)
{
    foreach (var existing in existingAttachments)
    {
        if (existing.Entity.StableId != srcStableId) continue;
        var copy = scripts.Registry.Create(ScriptRegistry.IdOf(existing.GetType()))
                   ?? (F_Behaviour?)Activator.CreateInstance(existing.GetType());
        if (copy is null) continue;
        copy.EnabledValue = existing.Enabled;
        ScriptFields.CopyExposedFields(existing, copy, remap); // 自动重映射子树内部实体槽位
        scripts.Attach(clonedEntity, copy);
    }
}
return clonedSubtree[0].Clone;
```

## 物理与角色控制

### F_Physics、F_RayHit 与 F_RaycastHit

[`F_Physics`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L557) 是当前活动会话 `Physics` 服务的静态快捷门面，支持两种风格的射线检测与动态刚体操作：

| 方法 | 返回 / 输出 | 说明 |
| :--- | :--- | :--- |
| `F_Physics.Raycast(origin, dir, dist, mask)` | `F_RayHit?` | 返回最近命中（`Entity`、`Body`、`Point`、`Normal`、`Distance`），未命中返回 `null`。 |
| `F_Physics.Raycast(origin, dir, dist, out F_RaycastHit hit, mask)` | `bool` | Unity 风格 `out` 重载（同时提供 `out F_RayHit` 重载）。 |
| `F_Physics.CreateBox(entity, F_BoxBodyDescription)` | `F_RigidBody?` | 动态创建盒体刚体（含 `Dynamic` / `Static` 预设）。 |
| `F_Physics.CreateSphere(entity, F_SphereBodyDescription)` | `F_RigidBody?` | 动态创建球体刚体或触发器（含 `Dynamic` / `Static` / `Trigger` 预设）。 |
| `F_Physics.GetBody(entity)` / `SetDebugDrawing(bool)` | `F_RigidBody?` / `bool` | 查询实体刚体代理或开关视口碰撞线框绘制。 |

### RigidBody

[`F_RigidBody`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Physics.cs#L153)（别名 `F_Rigidbody` / `RigidBody`，扩展见 [`FlareErgonomics.cs#L606`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L606)）提供直接的动力学控制：

- **快捷属性与质心施力**：`Velocity`（线速度）、`AngularVelocity`（角速度）、`AddForce(Vector3)`（建议在 `F_FixedUpdate` 调用）、`AddImpulse(Vector3)`（瞬间冲量，适合跳跃/击退）。
- **完整控制**：`State`（含 `Transform`、`LinearVelocity`、`AngularVelocity`、`Mass`、`Sleeping`）、`IsTrigger`、`SetVelocity(linear, angular)`、`ApplyForce(force, worldPoint)`、`ApplyImpulse(impulse, worldPoint)`、`Teleport(transform, clearVelocity)`、`Destroy()`。
- **碰撞接触数据**：[`F_Contact(F_Entity Other, Vector3 Point, Vector3 Normal, float Impulse)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L83)，由 `F_CollisionEnter/Stay/Exit(in F_Contact)` 传入，法线已统一调整为指向当前实体。

### F_CharacterController

[`F_CharacterController`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/CharacterController.cs#L36) 封装运动学胶囊体角色控制（支持斜坡滑移、跳跃及对动态刚体的有限推力）：

- **获取或创建**：在编辑器挂好角色控制器组件后直接 `GetComponent<F_CharacterController>()`（或 `F_CharacterController.FromComponent(entity)`）；也可用 `new F_CharacterController(entity, F_CharacterDescription.Default)` 动态创建。
- **移动与状态**：在 `F_FixedUpdate(float fixedDelta)` 中调用 `Move(horizontalDirection, jumpPressed, fixedDelta)` 返回 `CharacterState(Position, VerticalVelocity, Grounded)`；支持 `Teleport(Vector3)` 与 `Destroy()`。

## 音频、粒子与导航

| 子系统 | 入口属性 / 类型 | 高频方法 | 源码位置 |
| :--- | :--- | :--- | :--- |
| 音频 | `Audio`（[`F_Audio`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Audio.cs#L106)） | `MasterVolume`、`ActiveVoiceCount`、`Play(assetId, F_AudioPlayDescription)`、`PlayOneShot(id, vol)`、`PlayOneShotAt(id, pos, vol, minD, maxD)`、`Stop(voice)`、`UpdateVoice(voice, desc)`、`PlaySource(entity)`、`StopSource(entity)`、`IsSourcePlaying(entity)`、`SetPaused(bool)`、`StopAll()` | `Audio.cs` |
| 粒子 | `Particles`（[`F_Particles`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Particles.cs#L4)） | `EmitterCount`、`EmitBurst(entity, count)`、`SetPlaying(entity, bool)`、`Clear(entity?)`、`ActiveCount(entity?)` | `Particles.cs` |
| 导航 | `Navigation`（[`F_Navigation`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Navigation.cs#L33)） | `FindPath(navAssetId, start, goal)`（返回 `F_NavPath(Status, Waypoints, TotalLengthMeters)`）、`SetDestination(entity, target)`、`Stop(entity)`、`RemainingDistanceMeters(entity)`、`AgentStatus(entity)` | `Navigation.cs` |

## F_Input、F_InputActions、F_Cursor 与 F_Camera

- **[`F_Input`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L86)**：读取当前帧未被编辑器/游戏 UI 消费的即时键鼠输入。
  - 键盘（配合 [`F_Key`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L9)）：`Key(k)`（按住）、`KeyDown(k)`（本帧按下）、`KeyUp(k)`（本帧松开）、`Axis(neg, pos)`、`Horizontal`（`A/D` 或左右键）、`Vertical`（`S/W` 或上下键）、`MoveVector`（斜向归一化二维向量）。
  - 鼠标（配合 `F_MouseButton` / `MouseButton`）：`Mouse(btn)`、`MouseDown(btn)`、`MouseUp(btn)`、`MousePosition`、`MouseDelta`（逐帧原始相对位移像素，适配 FPS/TPS 自由视角）、`MouseWheel`。
- **[`F_InputActions`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/InputActions.cs#L7)**（脚本内经 `Actions` 属性访问）：加载 `.inputactions.json` 并统一查询键鼠与手柄映射：`LoadJson(json)`、`IsActionDown(name)`、`IsActionPressed(name)`、`GetAxis1D(name)`、`GetAxis2D(name)`。
- **[`F_Cursor`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L151)**：`F_Cursor.Locked`（设为 `true` 时自动隐藏并锁定光标到游戏视口中央）与 `F_Cursor.Visible`。运行预览中按 `Esc` 可临时释放光标，点击游戏视口恢复；原生层具备上升沿保护，避免脚本每帧重写把 `Esc` 解锁覆盖（详见[原生 ABI](bridge.md#cursor-lock)）。
- **[`F_Camera`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L184)**：`F_Camera.ScreenPointToRay(Vector2 screenPixel, F_Entity? camera = null)` 返回 `F_Ray(Origin, Direction)`，配合 `F_Physics.Raycast` 即可实现 3D 鼠标拾取。

## F_GameUi 与 F_UiElement

脚本通过 `Ui` 属性访问 [`F_GameUi`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L95) 游戏界面文档树，操作 [`F_UiElement`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L42) 控件代理：

- **文档与节点**：`Ui.Create(UiKind, parent?)`、`Ui.Find(ulong id)`、`Ui.Load(json)` / `Ui.Save()`、`Ui.Visuals()`、`Ui.ConsumesKeyboard`。
- **控件属性与动画**：`SetStyle(UiStyle)`、`SetText(string)`、`SetImage(assetId)`、`SetColor(Vector4)`、`SetValue(double)`、`SetRange(min, max)`、`SetVisible(bool)`、`SetEnabled(bool)`、`SetPointerBlocking(bool)`、`Focus()`、`Remove()`，以及内置缓动过渡 `FadeTo(opacity, sec)`、`MoveTo(offset, sec)`、`ColorTo(color, sec)`、`CancelTransitions()`。
- **事件绑定**：`Bind(UiEventKind, targetBehaviour, actionName)`（持久化动作绑定）或 `Subscribe(ownerBehaviour, UiEventKind, handler)`（运行期直接订阅）。

## F_Time

[`F_Time`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L41) 由 `ScriptSystem` 在每帧 `FrameBegin` 与 `FixedPreTick` 自动同步：

| 属性 | 类型 | 含义 |
| :--- | :--- | :--- |
| `DeltaTime` | `float` | 经 `TimeScale` 缩放后的帧间隔（秒） |
| `UnscaledDeltaTime` | `float` | 不受 `TimeScale` 影响的真实帧间隔（秒） |
| `FixedDeltaTime` | `float` | 固定物理步长（秒，默认 `1/60`） |
| `Time` / `UnscaledTime` | `float` | 自运行态开始累计的缩放时间 / 真实时间（秒） |
| `TimeScale` | `float` | 全局时间倍率（默认 `1f`，支持 `0f` 慢放/定格，上限 `100f`） |
| `FrameCount` | `ulong` | 运行态已推进的总渲染帧数 |

## F_Math

[`F_Math`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L197) 提供常用标量与 `Vector3` 运算：`PI`、`Deg2Rad`、`Rad2Deg`、`Clamp`、`Clamp01`、`Lerp`、`LerpUnclamped`、`InverseLerp`、`MoveTowards`、`Repeat`、`PingPong`、`DeltaAngle`、`LerpAngle`、`Approximately`。

### SmoothDamp

[`SmoothDamp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L261)（支持 `float` 与 `Vector3` 重载）采用临界阻尼弹簧模型实现帧率无关平滑追踪，`deltaTime` 省略时自动使用 `F_Time.DeltaTime`：

```csharp
public static Vector3 SmoothDamp(
    Vector3 current, Vector3 target, ref Vector3 currentVelocity,
    float smoothTime, float maxSpeed = float.PositiveInfinity, float? deltaTime = null)
```

## F_Quat

[`F_Quat`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L300) 封装右手系四元数工具：`Identity`、`Slerp(a, b, t)`、`Angle(a, b)`、`RotateTowards(from, to, maxDegreesDelta)`。

### Euler

[`F_Quat.Euler(float xDegrees, float yDegrees, float zDegrees)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L306)（含 `Vector3` 重载）按与编辑器属性面板完全一致的 `qy * qx * qz`（即 `Z → X → Y`）顺序构造单位四元数；[`F_Quat.ToEuler(Quaternion)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L318) 逆向解算角度制欧拉角。

### LookRotation

[`F_Quat.LookRotation(Vector3 forward, Vector3? worldUp = null)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L332) 将实体局部 `-Z` 对齐到指定世界方向；当 `forward` 与 `worldUp` 近乎共线时自动切换正交参考轴，避免产生 `NaN`。

## F_Log

[`F_Log.Info(msg)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L719)、`F_Log.Warn(msg)` 与 `F_Log.Error(msg)` 借助 `[CallerFilePath]` 与 `[CallerLineNumber]` 自动拼接 `文件名.cs(行号,1): ` 前缀写入有界无锁队列（上限 512 条）；编辑器每帧排空并显示在 **控制台**，双击日志行可直接跳回外部 IDE 源码对应行。

## F_Await

[`F_Await`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L767) 是零后台线程、在游戏主循环确定点恢复的轻量化 `INotifyCompletion` 等待器，完全替代 `Task.Delay`：

### Seconds

- `F_Await.Seconds(float seconds)`：按经过 `F_Time.TimeScale` 缩放的模拟时间等待指定秒数。
- `F_Await.NextFrame()`：等待到下一渲染帧 `FrameBegin` 阶段（`SkipCurrentFrame` 保证不会在同帧立刻重入）。
- `F_Await.FixedUpdate()`：等待到下一次 `FixedPreTick` 固定物理步。

所有延续均归属当前正在回调的 `F_Behaviour`，实体销毁或退出 Play Mode 时自动丢弃。

## 综合示例：射击交互与协程冷却

```csharp
using System.Numerics;
using FlareEngine;

namespace Game;

public class RaycastShooter : F_Behaviour
{
    [F_Header("射击参数")]
    [F_Expose, F_Range(5f, 100f)] public float Range = 40f;
    [F_Expose, F_Range(0.1f, 2f)] public float Cooldown = 0.35f;
    [F_Expose, F_Tooltip("发射特效粒子实体")] public F_Entity? MuzzleVfx;

    private bool ready = true;

    public override void F_Enable() => F_Cursor.Locked = true;

    public override void F_Update(float delta)
    {
        if (ready && F_Input.MouseDown(F_MouseButton.Left))
            FireAsync();
    }

    private async void FireAsync()
    {
        ready = false;
        if (MuzzleVfx is { IsValid: true }) Particles.EmitBurst(MuzzleVfx, 24);

        if (F_Physics.Raycast(Entity.WorldPosition, Entity.Forward, Range, out F_RaycastHit hit))
        {
            F_Log.Info($"命中 {hit.Entity.Name}，距离 {hit.Distance:F1}m");
            hit.Body.AddImpulse(Entity.Forward * 12f);
        }

        await F_Await.Seconds(Cooldown);
        ready = true;
    }
}
```
