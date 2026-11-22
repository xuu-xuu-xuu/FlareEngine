# C# 行为、零重启热重载与事件

本文讲清楚两件事：一是 `F_Behaviour` 脚本组件在一帧内按什么顺序执行、有哪些最新的生命周期安全保证；二是编辑器如何在不重启进程的情况下原位热重载 C# 脚本。日常移动、物理、输入、数学与异步 API 请见[高频游戏 API](api.md)。

## 整体架构与数据流

编辑器工作进程始终通过托管宿主 `FlareEngine.Bootstrap` 启动，脚本执行（`ScriptSystem`）与编辑态绑定文档（`ScriptBindings`）各自独立，共同挂接在原生世界的调度周期上：

```text
【运行态单帧调度：ScriptSystem】
  实体挂载/激活 ──▶ F_Awake() ──▶ F_Enable() ──▶ F_Start()
                                                    │
                    ┌───────────────────────────────┘
                    ▼
  ┌─ 固定物理步（默认 60Hz，可跑 0..MaxFixedSteps 次）────────────┐
  │  TickTimersAndAwaits(fixed) ─▶ F_FixedUpdate(fixedDelta)     │
  │  ─▶ Native PhysicsStep ─▶ F_CollisionEnter/Stay/Exit         │
  │                        ─▶ F_TriggerEnter/Stay/Exit           │
  └──────────────────────────────────────────────────────────────┘
                    │
                    ▼
  ┌─ 渲染帧主循环 ───────────────────────────────────────────────┐
  │  FrameBegin: F_Time 推进 ─▶ UI 动作派发 ─▶ F_Await / 延迟销毁  │
  │  PreTick   : F_PreUpdate(delta)                              │
  │  Tick      : F_Update(delta)                                 │
  │  PostTick  : F_LateUpdate(delta)                             │
  └──────────────────────────────────────────────────────────────┘
                    │
         禁用 / 销毁 / 停止 Play Mode
                    ▼
            F_Disable() ──▶ F_Destroy() ──▶ 原生实体销毁

【编辑态零重启热重载】
  改码保存 / 点击编译 / 新建脚本
       │
       ▼
  EditorScriptWorkflow::StartInProcessCompile()   (后台线程无窗口 dotnet build)
       │  增量 obj: out/Editor/HotReload/obj/
       │  隔离输出: out/Editor/HotReload/bin-<seq>/
       ▼
  EditorScriptWorkflow::PollInProcessCompile()    (主循环轮询完成)
       │  JSON 命令: { "ReloadAssembly": "<dll>", "AttachScript"?: "<id>" }
       ▼
  ScriptProject.RunEditor ──▶ ScriptBindings.ReloadAssembly()
       │  可回收 ProjectContext + LoadFromStream (磁盘 DLL 零文件锁)
       ▼
  原地刷新 Inspector 字段表 / 迁移重命名脚本 ID / 自动挂载待挂脚本
```

## F_Behaviour

[`F_Behaviour`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L88) 是所有 C# 游戏组件的抽象基类。[`ScriptRegistry.Discover()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L212) 扫描程序集中的公开、非抽象、带无参构造函数的派生类；若类标注了 `[F_Script("id")]` 则使用该 ID，否则直接以完整类型名 `type.FullName` 作为持久化脚本 ID（[`ScriptRegistry.IdOf`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L217)）。

### 14 个生命周期与物理回调速查

`ScriptSystem` 在组件挂载时通过反射记录子类实际重写了哪些虚方法（[`ScriptSystem.Entry`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L17)），只向原生调度器注册真正用到的阶段，零空调用开销：

| 阶段 | 回调签名（均支持重写） | 触发时机与说明 |
| :--- | :--- | :--- |
| 初始化与启用 | `F_Awake()` / `F_Enable()` / `F_Start()` | 实体首次处于层级启用状态时调一次 `F_Awake`；每次启用调 `F_Enable`；首次参与更新前调一次 `F_Start`。 |
| 固定物理步 | `F_FixedUpdate()` / `F_FixedUpdate(float fixedDelta)` | 按固定步长（默认 `1/60` 秒）在 `FixedPreTick` 调度，适合刚体施力与角色控制。 |
| 逐帧更新 | `F_PreUpdate([float delta])`<br>`F_Update([float delta])`<br>`F_LateUpdate([float delta])` | 每渲染帧按 `PreTick → Tick → PostTick` 顺序执行；`delta` 经 `F_Time.TimeScale` 缩放。相机跟随放在 `F_LateUpdate`。 |
| 刚体碰撞 | `F_CollisionEnter(in F_Contact)`<br>`F_CollisionStay(in F_Contact)`<br>`F_CollisionExit(in F_Contact)` | 在物理步后（`AfterPhysics`）派发；`F_Contact` 包含对方实体、世界接触点、指向自身的法线与法向冲量。 |
| 触发器重叠 | `F_TriggerEnter(F_Entity other)`<br>`F_TriggerStay(F_Entity other)`<br>`F_TriggerExit(F_Entity other)` | 刚体触发器或胶囊角色控制器进出触发器体积时在 `AfterPhysics` 派发。 |
| 禁用与销毁 | `F_Disable()` / `F_Destroy()` | 组件/实体禁用、销毁或停止 Play Mode 时逆序执行清理。 |

::: tip 无参 vs 带参重载与暂停运行
- `F_FixedUpdate` / `F_PreUpdate` / `F_Update` / `F_LateUpdate` 均提供**带 `float delta` 参数**和**无参**两个虚方法（带参版本默认转发给无参版本，见 [`Behaviour.cs#L159-L173`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L159)），重写其一即可。
- 重写 `public override bool UpdateWhenPaused => true;` 后，即使模拟暂停，该脚本的 `F_PreUpdate` / `F_Update` / `F_LateUpdate` 仍会在编辑器/游戏暂停期间继续运行（其中 `F_Update` 会注册到 `TickPhase.FrameBegin`，见 [`RegisterTicks`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L430)）。
:::

### 宿主快捷成员

`F_Behaviour` 直接暴露常用成员与子系统入口（[`Behaviour.cs#L93-L143`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L93)）：

- **实体与变换**：`Entity`、`Transform`、`Translate(...)`、`Rotate(...)`、`LookAt(...)`、`Enabled`
- **组件与对象操作**：`GetComponent<T>()`、`TryGetComponent<T>(out var c)`、`GetComponents<T>()`、`Instantiate(source, pos, rot)`、`Destroy(target, delaySeconds = 0f)`、`DestroySelf()`
- **子系统服务**：`Input`（底层快照，日常优先用静态 `F_Input`）、`Actions`、`Physics`、`Audio`、`Particles`、`Navigation`、`Ui`、`Scripts`

## ScriptSystem 的六大运行时行为保证

[`ScriptSystem`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L6) 刻意处理了游戏脚本最容易踩坑的 6 类时序边界问题：

### 1. 初始禁用实体延迟 `F_Awake`

如果实体或其任意祖先节点在启动时处于禁用状态（`Enabled == false`），`Create()` 会直接跳过初始化；直到该整条父子链首次全部启用时，才在 `RefreshActivation()` 中补跑 `F_Awake()` → `F_Enable()` → `RegisterTicks()`。

```csharp
// Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L497-L512
private static bool IsHierarchyEnabled(F_Entity entity)
{
    for (F_Entity? c = entity; c is not null; c = c.Parent)
        if (!c.Enabled) return false;
    return true;
}

private void Create(Entry entry)
{
    if (entry.Created || !entry.Value.Entity.IsAlive || !entry.Native.IsAlive ||
        destroying.Contains(entry.Value.Entity.Handle) || !IsHierarchyEnabled(entry.Value.Entity)) return;
    entry.Created = true;
    Invoke(entry, entry.Value.F_Awake, phase: nameof(F_Behaviour.F_Awake));
    RefreshActivation(entry);
    RegisterTicks(entry);
}
```

### 2. `Instantiate` 同帧立即可 `GetComponent<T>()`

在回调（`dispatchDepth > 0`）中克隆实体时，真正的原生组件挂接需要进入延迟变更队列 `mutations` 以保护迭代安全。为了让调用者在 `var clone = Instantiate(prefab);` 的**同一行之后立刻拿到脚本引用**，`Attach()` 在入队前就把新实例放进 `pendingAttachments`：

```csharp
// Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L128-L146 & #L781-L783
public void Attach(Entity entity, F_Behaviour value)
{
    // ...校验省略
    value.Entity = entity;
    value.Owner = this;
    pendingAttachments.Add(value); // 立即对 GetComponent<T>() 可见
    Mutate(() =>
    {
        pendingAttachments.Remove(value);
        if ((!entity.IsAlive && !entity.IsPending) || destroying.Contains(entity.Handle)) { value.Owner = null; return; }
        // ...绑定原生 ScriptComponentBinding 并转入 entries
    });
}

internal IReadOnlyList<F_Behaviour> AllAttachedBehaviours => pendingAttachments.Count == 0
    ? Attachments
    : entries.Select(e => e.Value).Concat(pendingAttachments).ToArray();
```

`F_Entity.GetComponent<T>()` 查的是 `AllAttachedBehaviours`（[`FlareErgonomics.cs#L528`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L528)），因此 `Instantiate` 后无需等下一帧就能拿到并初始化克隆体上的脚本。

### 3. 延迟原生销毁，保障 `F_Destroy` 可读 `Entity.WorldPosition`

销毁实体时，`ScriptSystem.Destroy()` 先执行所有脚本的 `F_Disable()` 和 `F_Destroy()`，**最后才调用 `entity.Destroy()`** 回收原生实体，因此在 `F_Destroy()` 中读取 `Entity.WorldPosition` 生成死亡特效或掉落物完全安全：

```csharp
// Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L174-L186
public void Destroy(Entity entity)
{
    if (entity.Session != Session) throw new ArgumentException("Script.InvalidAttachment");
    destroying.Add(entity.Handle);
    Mutate(() =>
    {
        pendingAttachments.RemoveAll(b => b.Entity.Handle == entity.Handle);
        // 先逐个触发 F_Disable() -> F_Destroy()，此时原生变换仍然可读
        foreach (var entry in entries.Where(e => e.Value.Entity.Handle == entity.Handle).ToArray()) Remove(entry);
        // 最后才真正销毁原生实体
        if (entity.IsAlive || entity.IsPending) entity.Destroy();
        destroying.Remove(entity.Handle);
        SweepDead();
    });
}
```

### 4. 实体销毁自动取消挂起的 `F_Await`

当行为实例进入 `Remove()` 时，引擎自动清除它登记的所有 `pendingAwaits`；`TickTimersAndAwaits()` 在恢复延续前还会二次检查 `owner.Owner == this && owner.Entity.IsAlive`（[`ScriptSystem.cs#L537`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L537)、[`#L662`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L662)），杜绝“物体已销毁，协程 2 秒后醒来访问悬空句柄”的经典崩溃。

### 5. `ScriptSynchronizationContext` 捕获 `async void` 异常

C# 中 `async void` 方法内抛出的异常默认会逃逸到线程顶层导致进程直接崩溃。`ScriptSystem` 在 `Invoke()` 与 `TickTimersAndAwaits()` 期间安装自定义同步上下文（[`ScriptSystem.cs#L61-L70`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L61)）：

```csharp
private sealed class ScriptSynchronizationContext(ScriptSystem owner) : SynchronizationContext
{
    public override void Post(SendOrPostCallback d, object? state)
    {
        if (owner.disposed || owner.stopping) return;
        try { d(state); }
        catch (Exception ex) { owner.Report(null, "Script.CallbackFailed", ex.ToString(), "async"); }
    }
    public override void Send(SendOrPostCallback d, object? state) => Post(d, state);
}
```

同时，普通同步回调中若脚本抛出异常，`Invoke()` 会将其标记为 `Faulted = true`、自动禁用该组件并调一次 `F_Disable()`，再将错误转发到编辑器控制台，其他脚本不受影响（[`ScriptSystem.cs#L605-L619`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L605)）。

### 6. 零重启热重载与运行态实时调参

- **后台增量编译**：[`EditorScriptWorkflow::StartInProcessCompile()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptWorkflow.cpp#L313) 在后台线程无窗口执行 `dotnet build`，共享 `out/Editor/HotReload/obj/` 增量缓存（有 `project.assets.json` 时优先走 `--no-restore` 快路径），将新 DLL 输出到递增的 `out/Editor/HotReload/bin-<seq>/` 目录（主循环完成后自动清理只保留最近 2 份）。
- **零文件锁流式加载**：[`EditorScriptWorkflow::PollInProcessCompile()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptWorkflow.cpp#L517) 轮询编译成功后，通过 `ReloadAssembly` 命令交给 [`ScriptProject.RunEditor`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptProject.cs#L150)，再调用 [`ScriptBindings.ReloadAssembly()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptBindings.cs#L172)。读取反射元数据时全程使用可回收 `ProjectContext` + `LoadFromStream`，绝不锁住磁盘 DLL。
- **Play Mode 实时调参**：在运行预览期间通过 Inspector 修改脚本暴露字段时，命令附带 `"Live": true`，经 [`ScriptFields.ApplyLive()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptFields.cs#L144) 直接写入存活实例；停止预览时自动丢弃运行态改动并还原编辑态绑定文档。

## ScriptBindings

[`ScriptBindings`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptBindings.cs#L12) 维护项目 `Scripts/*.bindings.json` 绑定文档（记录 `Version`、实体 `StableId`、脚本 `Type`、`Enabled` 与 `Fields`）。编辑态只保留 JSON/字符串元数据，不把项目 `Type` 长期驻留在宿主内存中。[`ScriptComponentBinding`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptComponentBinding.cs#L4) 负责将每个脚本实例桥接到原生世界的组件槽位。

### ReloadAssembly

[`void ReloadAssembly(string newAssemblyPath, IEnumerable<string> allowedScriptIds)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptBindings.cs#L172) 替换程序集路径与白名单后重新执行 [`LoadSchema()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptBindings.cs#L127)（通过临时 `ProjectContext` 流式加载提取字段描述并立即 `Unload()`）。若某个旧脚本因改命名空间/改 ID 在新程序集中找不到，但新程序集内存在**唯一**同简单类名的脚本，`ReloadAssembly` 会自动迁移旧绑定中的 `Type` 字段，避免丢失面板参数。

### Apply 与 `"CloneMap"` 深克隆重映射

[`ScriptBindings.Apply(ulong entityStableId, string request)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptBindings.cs#L232) 接收编辑器发来的 JSON 编辑指令，除常见的 `Add` / `Remove` / `Reset` / `MoveDelta` / `Field` / `Enabled` / `RenameFrom` / `PurgeType` 外，在编辑器内复制带子树的实体（`Ctrl+D`）时会使用 **`"CloneMap"`** 协议把整棵子树的脚本绑定连同内部实体引用一并重映射：

```csharp
// Source/FunctionLayer/Scripting/Managed/ScriptBindings.cs#L252-L287
if (change["CloneMap"] is JsonObject cloneMap && cloneMap.Count > 0)
{
    // cloneMap: { "<源实体 StableId>": "<克隆实体 StableId>", ... }
    foreach (var (srcId, dstId) in remap)
    {
        foreach (var srcEntry in snapshot)
        {
            if (srcEntry["Entity"]?.GetValue<string>() != srcId) continue;
            var copy = (JsonObject)srcEntry.DeepClone();
            copy["Entity"] = dstId;
            // 若脚本内有指向同子树成员的 entity 字段，自动改写为对应克隆体 ID
            if (copy["Fields"] is JsonObject copyFields)
                foreach (var fk in copyFields)
                    if (fk.Value is JsonObject fObj && fObj["Kind"]?.GetValue<string>() == "entity" &&
                        fObj["Value"]?.GetValue<string>() is { } refVal && remap.TryGetValue(refVal, out var mappedRef))
                        fObj["Value"] = mappedRef;
            list.Add(copy);
        }
    }
    if (clonedAny) ReplaceDocument(next);
    return;
}
```

## 事件、UI 动作与命令路由

| 机制 | 核心类 | 适用场景 | 生命周期保障 |
| :--- | :--- | :--- | :--- |
| 强类型事件总线 | [`ScriptEvents`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L61) | 跨脚本广播自定义 `[F_Event("key")]` 值类型载荷或内置物理/场景/昼夜事件，支持按 `EventPhase`（`NextFrame` / `AfterPhysics` / `AfterLogic`）派发 | `Subscribe<T>(handler)` 返回 `IDisposable`；脚本销毁时 `Events.Remove(owner)` 自动退订 |
| UI 动作绑定 | [`UiActionBindings`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/UiActionBindings.cs#L4) | 将 GameUI 控件的点击/值变更路由到 `(EntityStableId, ScriptId, Action)` 对应处理函数 | 持久化只存 ID 不存委托；脚本销毁时自动清理 |
| 单播命令路由 | [`ScriptCommands`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptCommands.cs#L4) | 需要立即返回 `bool` 受理结果的点对点控制命令（`Register<T>` / `Execute<T>`） | 每个命令键单一接收者，脚本销毁时自动注销 |

::: warning 扩展与调试注意事项
1. **自定义 `[F_Event]` 载荷必须是纯值类型结构体（`struct`）且仅含公开标量属性**：`ScriptEvents` 刻意不使用 `System.Text.Json` 全局反射缓存来编解码项目程序集类型（见 [`ScriptEvents.cs#L148-L153`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L148)），避免把可卸载的项目 `Type` 根死在默认上下文中。
2. **变更预算上限**：单次 `FlushMutations()` 最多处理 10,000 条级联变更（[`ScriptSystem.cs#L728`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L728)），防止在 `F_Awake` / `F_Destroy` 里递归无限生成或销毁实体导致死循环。
3. **热重载仅在编辑态生效**：在 Play Mode 下发送 `ReloadAssembly` 会返回 `Bridge.EditorReadOnly`；先停止预览，编辑器即可自动完成编译与热替换。
:::
