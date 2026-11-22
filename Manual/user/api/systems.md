# 音频、粒子、导航、UI、事件、昼夜与画质 API

本页整理 `FlareEngine` 命名空间下 6 套运行时玩法系统的完整 C# API：**3D 空间音频（`F_Audio`）、粒子控制（`F_Particles`）、导航寻路（`F_Navigation`）、游戏界面（`F_GameUi` / `F_UiElement` / `UiActionBindings`）、跨脚本事件与命令（`ScriptEvents` / `ScriptCommands`），以及昼夜时间与玩家画质（`TimeOfDayService` / `EngineSession.PlayerQuality`）**。所有签名、默认值与约束均已与现行源码逐行核对，可直接照抄使用。

## 快速入口：在 `F_Behaviour` 中如何访问

| 系统 | 在 `F_Behaviour` 里的访问方式 | 实际类型（兼容无前缀旧名） | 典型用途 |
| :--- | :--- | :--- | :--- |
| 音频 | `Audio`（即 `this.Audio`） | `F_Audio`（旧名 `Audio`） | 播放 2D/3D 音效、触发实体音源、控制音量与暂停 |
| 粒子 | `Particles`（即 `this.Particles`） | `F_Particles`（旧名 `Particles`） | 瞬时迸发粒子、启停发射器、清场与统计存活数 |
| 导航 | `Navigation`（即 `this.Navigation`） | `F_Navigation`（旧名 `Navigation`） | 离线寻路、驱动 `NavigationAgent` 寻路与停步 |
| 游戏 UI | `Ui`（即 `this.Ui`） | `F_GameUi`（旧名 `GameUi`） | 查找/创建 UI 节点、更新血条与文本、持久化文档 |
| UI 动作绑定 | `Scripts.UiActions` | `UiActionBindings` | 注册热重载友好的按钮点击/滑条变更回调 |
| 跨脚本事件 | `Scripts.Events`（订阅可直接调 `Subscribe<T>(...)`） | `ScriptEvents` | 按帧阶段广播解耦事件（如拾取金币、日出日落） |
| 单播命令 | `Scripts.Commands` | `ScriptCommands` | 同步发给单一接收者并立即获得 `bool` 处理结果 |
| 昼夜时间 | `Scripts.Session.TimeOfDay` | `TimeOfDayService` | 读取太阳/天空/时间快照、跳转时刻、调整流速 |
| 玩家画质 | `Scripts.Session.PlayerQuality` | `F_QualityLevel`（旧名 `QualityLevel`） | 查询或切换并持久化玩家画质档位 |

::: tip 命名兼容说明
`F_Audio`、`F_Particles`、`F_Navigation`、`F_GameUi`、`F_UiElement` 等类同时保留无前缀子类/同名结构体（`Audio`、`Particles`、`Navigation`、`GameUi`、`UiElement`、`AudioPlayDescription`、`AudioVoiceHandle`、`NavPath`、`NavPathStatus`、`QualityLevel`）。多数值类型之间提供隐式双向转换，但枚举 `F_QualityLevel` 与 `QualityLevel`、`F_NavPathStatus` 与 `NavPathStatus` 在互相赋值时需显式强转，下文会在对应位置明确标注。
:::

---

## 1. 音频系统：`F_Audio`

源码位置：[`Source/FunctionLayer/Scripting/Managed/Audio.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Audio.cs#L106)

`F_Audio` 负责播放 2D/3D 瞬时音效、动态调节活跃声部参数，以及控制场景实体上挂载的 `AudioSource` 组件。

::: info 3D 听者（Listener）如何同步？
`F_Audio` 无需在脚本里手动提交听者坐标。引擎每帧会自动从场景中挂载了 **音频监听器（`AudioListener`）** 组件的激活实体（通常为 `Main Camera`）读取其 `WorldTransform` 同步听者位置与朝向。
:::

### 1.1 `F_AudioPlayDescription`（旧名兼容：`AudioPlayDescription`）

源码位置：[`Audio.cs#L9`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Audio.cs#L9)

结构体描述单次音频播放的位置、音量、音高、3D 衰减与循环开关。`F_AudioPlayDescription` 与 `AudioPlayDescription` 可隐式互转。

| 字段 / 属性 | 类型 | 默认值（`Default`） | 说明与约束 |
| :--- | :--- | :--- | :--- |
| `Position` | `Vector3` | `Vector3.Zero` | 音源世界坐标（米），须为有限浮点数 |
| `Volume` | `float` | `1.0f` | 音量倍率，必须 `>= 0`（常规范围 `0.0f..1.0f`） |
| `Pitch` | `float` | `1.0f` | 音高与播放速度倍率，**必须 `> 0`**（`1.0f` 为原速） |
| `MinDistanceMeters` | `float` | `1.0f` | 3D 满音量最小衰减距离（米），必须 `>= 0` |
| `MaxDistanceMeters` | `float` | `30.0f` | 3D 衰减归零的最大距离（米），必须 `>= 0` |
| `SpatialBlend` | `float` | `0.0f` | 空间混合权重：`0.0f` 纯 2D，`1.0f` 纯 3D 衰减 |
| `Loop` | `bool` | `false` | 是否循环播放 |
| `Priority` | `int` | `0` | 声部优先级 |

**静态快捷构造：**

| 成员签名 | 说明 |
| :--- | :--- |
| `static F_AudioPlayDescription Default { get; }` | 返回 2D 默认参数（`SpatialBlend = 0f`、`Volume = 1f`、`Pitch = 1f`、`Loop = false`） |
| `static F_AudioPlayDescription Spatial(Vector3 position, float volume = 1.0f, float minDistance = 1.0f, float maxDistance = 30.0f, bool loop = false)` | 返回 3D 空间衰减参数（`SpatialBlend = 1.0f`、`Pitch = 1.0f`、`Priority = 0`） |

### 1.2 `F_AudioVoiceHandle`（旧名兼容：`AudioVoiceHandle`）

源码位置：[`Audio.cs#L93`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Audio.cs#L93)

只读值类型句柄，代表一个正在播放的底层声部，可与 `AudioVoiceHandle` 隐式互转。

| 成员 | 类型 | 说明 |
| :--- | :--- | :--- |
| `Handle` | `ulong` | 底层打包的 64 位声部 ID（`0` 为无效句柄） |
| `IsValid` | `bool` | 等价于 `Handle != 0`，用于判空校验 |

### 1.3 `F_Audio` 属性与方法

**属性：**

| 属性 | 类型 | 读写 | 说明 |
| :--- | :--- | :--- | :--- |
| `MasterVolume` | `float` | 读 / 写 | 全局主音量（常规 `0..1`，底层要求有限且 `>= 0`） |
| `ActiveVoiceCount` | `uint` | 只读 | 当前正在发声的活跃声部总数 |

**方法：**

| 方法签名 | 参数说明 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `AudioVoiceHandle Play(string assetId, AudioPlayDescription description)` | `assetId`：非空音频资产 ID；`description`：完整播放描述（可直接传入 `F_AudioPlayDescription`） | `AudioVoiceHandle`（可隐式赋给 `F_AudioVoiceHandle`） | 按指定参数播放音频资产并返回声部句柄 |
| `AudioVoiceHandle PlayOneShot(string assetId, float volume = 1.0f)` | `assetId`：音频资产 ID；`volume`：音量（默认 `1.0f`） | `AudioVoiceHandle` | 播放单次不循环 2D 音效（适合 UI 点击、拾取提示音） |
| `AudioVoiceHandle PlayOneShotAt(string assetId, Vector3 position, float volume = 1.0f, float minDistance = 1.0f, float maxDistance = 30.0f)` | `assetId`：音频资产 ID；`position`：世界坐标；`volume` / `minDistance` / `maxDistance`：衰减参数 | `AudioVoiceHandle` | 在世界坐标播放单次不循环 3D 空间衰减音效 |
| `void Stop(AudioVoiceHandle voice)` | `voice`：活跃声部句柄（可传 `F_AudioVoiceHandle`） | `void` | 立即停止指定声部；若句柄为 `0` 会抛 `EngineException("Audio.InvalidVoice")` |
| `bool IsPlaying(AudioVoiceHandle voice)` | `voice`：声部句柄 | `bool` | 查询声部是否仍在播放；`voice.IsValid == false` 时直接返回 `false` |
| `void UpdateVoice(AudioVoiceHandle voice, AudioPlayDescription description)` | `voice`：活跃声部句柄；`description`：新参数描述 | `void` | 动态更新运行中声部的 `Volume`、`Pitch`、`Position`、`SpatialBlend`、`MinDistanceMeters`、`MaxDistanceMeters` |
| `void PlaySource(F_Entity entity)` | `entity`：挂载了 `AudioSource` 组件的实体 | `void` | 触发实体上的 `AudioSource` 组件开始播放 |
| `void StopSource(F_Entity entity)` | `entity`：挂载了 `AudioSource` 组件的实体 | `void` | 停止实体上 `AudioSource` 组件的当前播放 |
| `bool IsSourcePlaying(F_Entity entity)` | `entity`：挂载了 `AudioSource` 组件的实体 | `bool` | 查询实体 `AudioSource` 组件是否正在播放 |
| `void SetPaused(bool paused)` | `paused`：`true` 暂停，`false` 恢复 | `void` | 全局暂停或恢复音频混合输出 |
| `void StopAll()` | 无 | `void` | 立即停止全部活跃声部及场景音源 |
| `void UseNullBackend(bool forceNull = true)` | `forceNull`：`true` 切静音空后端，`false` 重建 XAudio2 硬件后端 | `void` | 测试或无声卡环境下切换空后端；调用时会清空当前全部播放 |

::: warning 常见坑
- `UpdateVoice` 仅动态更新音量、音高、世界位置与空间衰减参数，**不会**修改已经启动声部的 `Loop` 或 `Priority`；若声部已自然播完失效，`UpdateVoice` 会抛出 `EngineException("Audio.InvalidVoice")`，建议先用 `Audio.IsPlaying(_voice)` 判断。
- `F_AudioPlayDescription` 默认构造 `default` 时 `Pitch` 为 `0f`，直接传给 `Play` 会因 `pitch <= 0` 被拒绝！请务必从 `F_AudioPlayDescription.Default` 或 `F_AudioPlayDescription.Spatial(...)` 起步再改字段。
:::

### 1.4 示例：移动载体循环 3D 引擎声 + 碰撞瞬时音效

```csharp
using System.Numerics;
using FlareEngine;

public class VehicleAudioController : F_Behaviour
{
    [F_Expose] public string EngineLoopAssetId = "audio/engine_loop.wav";
    [F_Expose] public string ImpactAssetId = "audio/metal_hit.wav";

    private F_AudioVoiceHandle _engineVoice;
    private F_AudioPlayDescription _engineDesc;

    public override void F_Start()
    {
        if (string.IsNullOrEmpty(EngineLoopAssetId)) return;

        // 从 Spatial 工厂初始化，避免 Pitch = 0 导致参数校验失败
        _engineDesc = F_AudioPlayDescription.Spatial(
            Entity.WorldPosition,
            volume: 0.7f,
            minDistance: 2.0f,
            maxDistance: 40.0f,
            loop: true);

        _engineVoice = Audio.Play(EngineLoopAssetId, _engineDesc);
    }

    public override void F_Update(float delta)
    {
        if (!_engineVoice.IsValid || !Audio.IsPlaying(_engineVoice)) return;

        // 实时同步 3D 声源位置，并按前进输入微调音高
        _engineDesc.Position = Entity.WorldPosition;
        _engineDesc.Pitch = 1.0f + MathF.Abs(F_Input.Vertical) * 0.45f;
        Audio.UpdateVoice(_engineVoice, _engineDesc);
    }

    public override void F_CollisionEnter(in F_Contact contact)
    {
        // 在真实碰撞点播放一次 3D 撞击音效，音量随冲量缩放
        if (!string.IsNullOrEmpty(ImpactAssetId) && contact.Impulse > 1.5f)
        {
            float vol = F_Math.Clamp01(contact.Impulse / 20f);
            Audio.PlayOneShotAt(ImpactAssetId, contact.Point, volume: vol, minDistance: 1.5f, maxDistance: 35f);
        }
    }

    public override void F_Destroy()
    {
        if (_engineVoice.IsValid && Audio.IsPlaying(_engineVoice))
            Audio.Stop(_engineVoice);
    }
}
```

---

## 2. 粒子系统：`F_Particles`

源码位置：[`Source/FunctionLayer/Scripting/Managed/Particles.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Particles.cs#L4)

`F_Particles`（兼容无前缀旧名 `Particles`）用于在运行时触发粒子迸发、开关实体上的 `ParticleEmitter` 持续发射、清空存活粒子以及查询数量。

### 2.1 `F_Particles` 属性与方法

**属性：**

| 属性 | 类型 | 读写 | 说明 |
| :--- | :--- | :--- | :--- |
| `EmitterCount` | `uint` | 只读 | 当前场景中已注册追踪的粒子发射器总数 |

**方法：**

| 方法签名 | 参数说明 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `uint EmitBurst(F_Entity entity, uint count)` | `entity`：挂有激活 `ParticleEmitter` 的实体；`count`：请求迸发粒子数 | `uint` | 瞬时生成一批粒子，返回**实际成功生成数量**（受发射器容量上限约束） |
| `void SetPlaying(F_Entity entity, bool playing)` | `entity`：目标实体；`playing`：`true` 开启持续发射，`false` 暂停发射 | `void` | 切换持续发射开关；暂停后已存活粒子会继续自然消亡 |
| `void Clear(F_Entity? entity = null)` | `entity`：指定实体，或传 `null`（缺省）表示清空全场 | `void` | 立即清除指定发射器或全场所有存活粒子 |
| `uint ActiveCount(F_Entity? entity = null)` | `entity`：指定实体，或传 `null`（缺省）表示统计全场 | `uint` | 返回指定发射器或全场当前存活粒子总数 |

::: warning 注意实体激活状态
对未挂载 `ParticleEmitter`、发射器组件未启用或所属实体被禁用的目标调用 `EmitBurst` / `SetPlaying`，底层会抛出 `EngineException("Particle.EmitterInactive")`。使用暴露引用时建议先判断 `entity is { IsValid: true, Enabled: true }`。
:::

### 2.2 示例：按需迸发火花与持续火焰开关

```csharp
using FlareEngine;

public class TorchParticles : F_Behaviour
{
    [F_Expose] public F_Entity? FlameEmitter;
    [F_Expose] public uint BurstCount = 48;

    private bool _lit = true;

    public override void F_Update(float delta)
    {
        F_Entity emitter = FlameEmitter is { IsValid: true } ? FlameEmitter : Entity;

        // 按 F 瞬时迸发一簇火星
        if (F_Input.KeyDown(F_Key.F))
        {
            uint spawned = Particles.EmitBurst(emitter, BurstCount);
            F_Log.Info($"迸发火星 {spawned} 颗，当前该发射器存活 {Particles.ActiveCount(emitter)} / 全场 {Particles.ActiveCount()}");
        }

        // 按 E 开关火把持续火焰；按 C 立即熄灭清空残余粒子
        if (F_Input.KeyDown(F_Key.E))
        {
            _lit = !_lit;
            Particles.SetPlaying(emitter, _lit);
        }
        else if (F_Input.KeyDown(F_Key.C))
        {
            _lit = false;
            Particles.SetPlaying(emitter, false);
            Particles.Clear(emitter);
        }
    }
}
```

---

## 3. 导航寻路：`F_Navigation`

源码位置：[`Source/FunctionLayer/Scripting/Managed/Navigation.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Navigation.cs#L33)

`F_Navigation`（兼容无前缀旧名 `Navigation`）提供两种寻路用法：
1. **代理自动寻路**：给挂有 `NavigationAgent` 组件的实体调用 `SetDestination` / `Stop`，引擎自动沿 NavMesh 移动。
2. **离线路径查询**：调用 `FindPath` 直接获得世界空间路点数组，用于绘制路径预览或自定义移动逻辑。

### 3.1 `F_NavPathStatus`（旧名兼容：`NavPathStatus`）

源码位置：[`Navigation.cs#L7`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Navigation.cs#L7)

| 枚举值 | 数值 | 说明 |
| :--- | :--- | :--- |
| `Complete` | `0` | 找到通往目标的完整可行走路径 |
| `Partial` | `1` | 目标不可直达，已规划至最近可达点的部分路径 |
| `Unreachable` | `2` | 起点或终点无法落到可行走区域，或两者不连通 |

### 3.2 `F_NavPath`（旧名兼容：`NavPath`）

源码位置：[`Navigation.cs#L23`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Navigation.cs#L23)

只读值类型记录，与 `NavPath` 之间支持隐式双向转换。

| 属性 | 类型 | 说明 |
| :--- | :--- | :--- |
| `Status` | `F_NavPathStatus` | 寻路可达状态（`Complete` / `Partial` / `Unreachable`） |
| `Waypoints` | `Vector3[]` | 世界空间折线路点数组（单位：米；不可达时可能为空数组） |
| `TotalLengthMeters` | `float` | 折线路径总长度（米） |

### 3.3 `F_Navigation` 方法

| 方法签名 | 参数说明 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `NavPath FindPath(string navAssetId, Vector3 start, Vector3 goal)` | `navAssetId`：烘焙后的导航网格资产 ID（传 `""` 使用场景默认导航网格，不可为 `null`）；`start` / `goal`：世界起终点 | `NavPath`（可隐式赋给 `F_NavPath`） | 计算起点到终点的折线路径，内部自动处理多边形吸附与路点扩容 |
| `void SetDestination(F_Entity entity, Vector3 target)` | `entity`：挂有激活 `NavigationAgent` 组件的实体；`target`：世界目标点 | `void` | 设置导航代理的目标点并开始沿 NavMesh 自动移动 |
| `void Stop(F_Entity entity)` | `entity`：导航代理实体 | `void` | 立即停止导航代理的当前移动与路径跟随 |
| `float RemainingDistanceMeters(F_Entity entity)` | `entity`：导航代理实体 | `float` | 返回代理当前距目标点的剩余路径长度（米） |
| `NavPathStatus AgentStatus(F_Entity entity)` | `entity`：导航代理实体 | `NavPathStatus` | 返回代理当前寻路状态（如需 `F_NavPathStatus` 可显式强转 `(F_NavPathStatus)Navigation.AgentStatus(Entity)`） |

::: info 寻路使用要点
- 当前版本不提供独立的 `SamplePosition` 接口；起终点吸附到最近可行走多边形的逻辑由 `FindPath` 与 `SetDestination` 内部自动完成。
- `SetDestination`、`Stop`、`RemainingDistanceMeters`、`AgentStatus` 均要求实体处于激活态且挂载了启用的 `NavigationAgent` 组件，否则会抛出 `EngineException("Navigation.AgentInactive")`。
:::

### 3.4 示例：敌人两点巡逻 + 发现玩家追击

```csharp
using System.Numerics;
using FlareEngine;

public class GuardPatrolAI : F_Behaviour
{
    [F_Expose] public F_Entity? PointA;
    [F_Expose] public F_Entity? PointB;
    [F_Expose, F_Range(2f, 30f)] public float ChaseRadius = 10f;
    [F_Expose, F_Range(0.2f, 3f)] public float ArriveDistance = 0.6f;

    private bool _headingToB = true;
    private float _repathTimer;

    public override void F_Start()
    {
        MoveToCurrentPatrolPoint();
    }

    public override void F_Update(float delta)
    {
        _repathTimer -= delta;

        // 1. 优先检测玩家是否在警戒范围内
        if (F_Scene.FindWithTag("Player") is { IsValid: true } player)
        {
            float dist = Vector3.Distance(Entity.WorldPosition, player.WorldPosition);
            if (dist <= ChaseRadius)
            {
                if (_repathTimer <= 0f)
                {
                    // 用离线 FindPath 先确认路径是否可达，再驱动代理追击
                    F_NavPath preview = Navigation.FindPath("", Entity.WorldPosition, player.WorldPosition);
                    if (preview.Status != F_NavPathStatus.Unreachable)
                        Navigation.SetDestination(Entity, player.WorldPosition);
                    else
                        Navigation.Stop(Entity);

                    _repathTimer = 0.25f;
                }
                return;
            }
        }

        // 2. 巡逻模式：接近巡逻点或当前路径不可达时切换下一个巡逻点
        var status = (F_NavPathStatus)Navigation.AgentStatus(Entity);
        float remaining = Navigation.RemainingDistanceMeters(Entity);
        if (status == F_NavPathStatus.Unreachable || remaining <= ArriveDistance)
        {
            _headingToB = !_headingToB;
            MoveToCurrentPatrolPoint();
        }
    }

    private void MoveToCurrentPatrolPoint()
    {
        F_Entity? next = _headingToB ? PointB : PointA;
        if (next is { IsValid: true })
            Navigation.SetDestination(Entity, next.WorldPosition);
    }
}
```

---

## 4. 游戏界面：`F_GameUi`、`F_UiElement` 与 `UiActionBindings`

源码位置：[`Source/FunctionLayer/Scripting/Managed/GameUi.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L42) · [`UiActionBindings.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/UiActionBindings.cs#L4)

FlareEngine 提供树形保留模式游戏界面（GameUI）：既可以在编辑器 UI 画布里可视化搭建、在脚本中通过 `Ui.Find(id)` 查找并更新，也可以直接用 `Ui.Create(...)` 运行时动态构建 HUD 与菜单。

### 4.1 枚举一览

**`UiKind`**（[`GameUi.cs#L9`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L9)）— 节点控件类型：

| 枚举值 | 说明 |
| :--- | :--- |
| `Panel` | 容器面板（可做背景底框与布局容器） |
| `Label` | 静态或动态文本标签 |
| `Image` | 图片/图标控件（配合 `SetImage`） |
| `Button` | 可点击按钮（触发 `Click`、`PointerEnter`、`PointerLeave`） |
| `Checkbox` | 复选框（`Value` 为 `0` / `1`，变更触发 `ValueChanged`） |
| `Slider` | 拖拽滑条（配合 `SetRange` / `SetStep` / `SetValue`，触发 `ValueChanged`） |
| `ProgressBar` | 进度条/血条（配合 `SetRange` / `SetValue`） |
| `ScrollView` | 可滚动裁剪容器 |
| `TextInput` | 文本输入框（支持 IME 输入法组合串，触发 `TextChanged` / `FocusChanged`） |

**`UiLayout`**（[`GameUi.cs#L10`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L10)）：`Absolute`（绝对锚点定位）、`Horizontal`（水平排布）、`Vertical`（垂直排布）。

**`UiEventKind`**（[`GameUi.cs#L11`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L11)）：`Click`、`ValueChanged`、`TextChanged`、`FocusChanged`、`PointerEnter`、`PointerLeave`。

**`UiPointerOperation`**（[`GameUi.cs#L12`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L12)）：`Move`、`Down`、`Up`、`Wheel`、`Cancel`。

### 4.2 数据结构：`UiStyle`、`UiBinding`、`UiEvent`、`UiVisual`

**`UiStyle`**（[`GameUi.cs#L14`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L14)）— 节点布局与锚点样式：

| 字段 / 属性 | 类型 | 默认值（`UiStyle.Default`） | 说明 |
| :--- | :--- | :--- | :--- |
| `AnchorMin` | `Vector2` | `(0, 0)` | 父区域左上锚点归一化比例（`0..1`） |
| `AnchorMax` | `Vector2` | `(0, 0)` | 父区域右下锚点归一化比例（`0..1`） |
| `Pivot` | `Vector2` | `(0, 0)` | 节点自身对齐枢轴（`0..1`） |
| `Offset` | `Vector2` | `(0, 0)` | 相对锚点的偏移量（逻辑像素） |
| `Size` | `Vector2` | `(120, 32)` | 节点宽高（逻辑像素） |
| `Padding` | `float` | `0` | 容器内边距 |
| `Spacing` | `float` | `0` | `Horizontal` / `Vertical` 子节点间距 |
| `Layout` | `UiLayout` | `UiLayout.Absolute` | 子节点布局方式 |
| `Clip` | `bool` | `false` | 是否裁剪超出自身矩形区域的子节点 |

**`UiBinding`**（[`GameUi.cs#L23`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L23)）：`record UiBinding(UiEventKind Event, ulong Entity, string Script, string Action)` — 持久化的 UI 动作描述，仅存实体 `StableId`、脚本 ID 与动作名，不存委托对象，因此支持序列化和脚本热重载。

**`UiEvent`**（[`GameUi.cs#L27`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L27)）：`record UiEvent(UiEventKind Kind, ulong Target, double Value, string Text)`，另含可选属性 `UiBinding? Binding { get; init; }`。

**`UiVisual`**（[`GameUi.cs#L32`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L32)）：由 `Ui.Visuals()` 返回的只读渲染快照，包含 `Id`、`Kind`、`X`/`Y`/`Width`/`Height`、`ClipX`/`ClipY`/`ClipWidth`/`ClipHeight`、`Text`、`ImageAsset`、`Composition`、`Value`、`Opacity`、`Hovered`、`Pressed`、`Focused`、`Enabled`，以及文本编辑光标属性 `Caret`、`SelectionAnchor`、`CompositionCaret`（均为 Unicode 码点索引）。

### 4.3 `F_GameUi`（旧名兼容：`GameUi`）

源码位置：[`GameUi.cs#L95`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L95)

| 成员签名 | 参数说明 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `bool ConsumesKeyboard { get; }` | — | `bool` | 当前是否有 `TextInput` 等控件持有焦点并独占键盘输入 |
| `UiElement? Find(ulong id)` | `id`：UI 节点 ID | `UiElement?`（继承自 `F_UiElement`） | 查找已存在的 UI 节点；不存在返回 `null` |
| `UiElement Create(UiKind kind, F_UiElement? parent = null)` | `kind`：控件类型；`parent`：父节点（`null` 挂到根节点） | `UiElement` | 运行时创建新 UI 节点并返回代理 |
| `void Layout(float width, float height)` | `width` / `height`：视口宽高（`>= 0`） | `void` | 手动按指定尺寸触发布局计算（常规游戏窗口会自动布局） |
| `bool Pointer(UiPointerOperation operation, Vector2 position, float wheel = 0)` | `operation`：指针动作；`position`：像素坐标；`wheel`：滚轮增量 | `bool` | 向 UI 树手动注入指针事件（多用于自定义宿主或测试），返回是否被 UI 消费 |
| `bool Key(uint virtualKey, bool shift = false, bool control = false)` | `virtualKey`：虚拟键码（`0..255`）；`shift` / `control`：修饰键 | `bool` | 手动向聚焦控件注入编辑按键，返回是否消费 |
| `bool InputText(string text, bool composition = false)` | `text`：UTF-8 文本；`composition`：`true` 表示 IME 预编辑串 | `bool` | 手动向聚焦 `TextInput` 注入文本，返回是否消费 |
| `IReadOnlyList<UiVisual> Visuals()` | 无 | `IReadOnlyList<UiVisual>` | 读取当前帧全部 UI 节点的布局与视觉状态快照 |
| `IReadOnlyList<UiEvent> DrainEvents()` | 无 | `IReadOnlyList<UiEvent>` | 取出并清空待派发 UI 事件队列（**普通脚本请勿手动调用**，见下方提示） |
| `string Save()` | 无 | `string` | 将整棵 UI 文档树序列化为 JSON 字符串 |
| `void Load(string json)` | `json`：由 `Save()` 或编辑器导出的 UI 文档 JSON | `void` | 载入 UI 文档并替换当前树（会令之前缓存的 `F_UiElement` 代理失效） |

::: warning 不要在 `F_Behaviour` 里手调 `Ui.DrainEvents()`
`ScriptSystem` 每帧会自动调用 `Session.Ui.DrainEvents()`，先派发到 `Scripts.UiActions`，再广播到 `Scripts.Events`。如果脚本自己提前调用了 `Ui.DrainEvents()`，事件会被直接消费清空，导致所有按钮的 `Bind` 回调和 `Subscribe` 监听都收不到通知。
:::

### 4.4 `F_UiElement`（旧名兼容：`UiElement`）

源码位置：[`GameUi.cs#L42`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L42)

**属性：**
- `ulong Id { get; }` — 节点唯一数字 ID。
- `IReadOnlyList<UiBinding> Bindings { get; }` — 当前节点保存的全部持久事件绑定。

**样式、内容与控件数值方法：**

| 方法签名 | 说明与参数范围 |
| :--- | :--- |
| `void SetStyle(UiStyle style)` | 设置锚点、尺寸、偏移、布局方向与裁剪开关 |
| `void SetText(string text)` | 更新文本内容（用于 `Label`、`Button`、`Checkbox`、`TextInput` 等，非 `null`） |
| `void SetImage(string assetId)` | 绑定图片资产 ID（用于 `Image`、`Button`、`Panel` 等） |
| `void SetColor(Vector4 color)` | 立即设置控件颜色（`X=R, Y=G, Z=B, W=A`，各分量必须在 **`0..1`**） |
| `void SetValue(double value)` | 设置数值状态（`ProgressBar` / `Slider` 进度值，`Checkbox` `0` 或 `1`，`ScrollView` 滚动位置） |
| `void SetRange(double minimum, double maximum)` | 设置 `Slider` 或 `ProgressBar` 的数值上下界（默认 `0..1`） |
| `void SetStep(double value)` | 设置 `Slider` 拖拽步进间隔（`0` 为连续无步进） |
| `void SetVisible(bool value)` | 显示或隐藏当前节点及其子树 |
| `void SetEnabled(bool value)` | 启用或禁用交互（禁用后不响应点击/编辑） |
| `void SetPointerBlocking(bool value)` | 设置节点是否阻挡鼠标点击穿透到场景 |
| `void Focus()` | 将键盘输入焦点切换到该节点（常用于 `TextInput`） |
| `void Remove()` | 从 UI 文档树中移除当前节点 |

**内置过渡动画方法：**

| 方法签名 | 说明 |
| :--- | :--- |
| `void FadeTo(float opacity, double seconds)` | 在 `seconds`（`>= 0`）秒内平滑过渡不透明度到 `opacity`（`0..1`） |
| `void MoveTo(Vector2 offset, double seconds)` | 在 `seconds` 秒内将 `UiStyle.Offset` 平滑移动到目标偏移 |
| `void ColorTo(Vector4 color, double seconds)` | 在 `seconds` 秒内平滑过渡颜色到 RGBA（各分量 `0..1`） |
| `void CancelTransitions()` | 立即取消当前节点上正在播放的全部过渡动画 |

**事件绑定与订阅方法：**

| 方法签名 | 说明 |
| :--- | :--- |
| `void Bind(UiEventKind kind, F_Behaviour target, string action)` | **推荐（热重载友好）**：将事件持久绑定到 `target` 脚本实例和具名 `action`；配合 `Scripts.UiActions.Register` 使用，可随 `Ui.Save()` 保存 |
| `void SetBinding(UiBinding binding)` | 直接写入底层 `UiBinding` 结构（每个 `UiEventKind` 在同一节点上最多保留一条绑定，重复设置覆盖旧值） |
| `void Unbind(UiEventKind kind)` | 移除节点上指定事件类型的持久绑定 |
| `IDisposable Subscribe(F_Behaviour ownerBehaviour, UiEventKind kind, Action<UiEvent> action)` | 临时监听该节点事件（不写入文档持久化，通过事件总线派发；脚本销毁时自动释放，也可提前 `Dispose()`） |

### 4.5 `UiActionBindings`（热重载友好动作注册表）

源码位置：[`UiActionBindings.cs#L4`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/UiActionBindings.cs#L4)

通过 `Scripts.UiActions` 访问。热重载或场景重开后，UI 文档中保留的是 `(Entity.StableId, ScriptId, Action)` 三元组；脚本在 `F_Start()`（或 `F_Enable()`）里再次调用 `Register` 即可无感接回所有按钮回调，不会把旧程序集委托残留进内存。

| 成员签名 | 说明与约束 |
| :--- | :--- |
| `int Count { get; }` | 当前已注册的 UI 动作处理器数量 |
| `IDisposable Register(F_Behaviour owner, string action, Action<UiEvent> handler)` | 为 `owner` 注册名为 `action` 的 UI 事件处理器。<br>- `action` 长度 1..128，仅允许字母、数字及 `.` `_` `:` `-`；<br>- 同一实体同脚本重复注册同名 `action` 会抛 `InvalidOperationException("Script.UiActionDuplicate")`；<br>- 脚本销毁时由引擎自动清理，也可提前调用返回的 `IDisposable.Dispose()` 注销。 |

### 4.6 示例：HUD 血条 + 热重载友好治疗按钮 + 输入框监听

```csharp
using System.Numerics;
using FlareEngine;

public class PlayerHudController : F_Behaviour
{
    [F_Expose] public float MaxHp = 100f;
    private float _hp = 100f;

    private F_UiElement? _rootPanel;
    private F_UiElement? _hpBar;
    private F_UiElement? _hpLabel;
    private F_UiElement? _healButton;
    private F_UiElement? _nameInput;
    private IDisposable? _inputSub;

    public override void F_Start()
    {
        // 1. 注册热重载友好的具名 UI 动作处理器
        Scripts.UiActions.Register(this, "hud.heal", OnHealClicked);

        // 2. 创建左上角 HUD 容器面板
        _rootPanel = Ui.Create(UiKind.Panel);
        var panelStyle = UiStyle.Default;
        panelStyle.Offset = new Vector2(24f, 24f);
        panelStyle.Size = new Vector2(260f, 148f);
        panelStyle.Layout = UiLayout.Vertical;
        panelStyle.Padding = 12f;
        panelStyle.Spacing = 8f;
        _rootPanel.SetStyle(panelStyle);
        _rootPanel.SetColor(new Vector4(0.08f, 0.10f, 0.14f, 0.82f));

        // 3. 创建血量文字与进度条
        _hpLabel = Ui.Create(UiKind.Label, _rootPanel);
        _hpLabel.SetStyle(new UiStyle { Size = new Vector2(236f, 24f) });

        _hpBar = Ui.Create(UiKind.ProgressBar, _rootPanel);
        _hpBar.SetStyle(new UiStyle { Size = new Vector2(236f, 20f) });
        _hpBar.SetRange(0.0, MaxHp);
        _hpBar.SetColor(new Vector4(0.18f, 0.82f, 0.35f, 1.0f));

        // 4. 创建“治疗 +20”按钮并持久绑定到 hud.heal
        _healButton = Ui.Create(UiKind.Button, _rootPanel);
        _healButton.SetStyle(new UiStyle { Size = new Vector2(236f, 32f) });
        _healButton.SetText("治疗 +20");
        _healButton.Bind(UiEventKind.Click, this, "hud.heal");

        // 5. 创建玩家昵称输入框并临时订阅文本变更
        _nameInput = Ui.Create(UiKind.TextInput, _rootPanel);
        _nameInput.SetStyle(new UiStyle { Size = new Vector2(236f, 28f) });
        _nameInput.SetText("Adventurer");
        _inputSub = _nameInput.Subscribe(this, UiEventKind.TextChanged, evt =>
        {
            F_Log.Info($"玩家昵称已修改为: {evt.Text}");
        });

        RefreshHpUi();
    }

    public override void F_Update(float delta)
    {
        // 输入框占用键盘时不触发快捷键掉血
        if (!Ui.ConsumesKeyboard && F_Input.KeyDown(F_Key.H))
        {
            _hp = F_Math.Clamp(_hp - 15f, 0f, MaxHp);
            RefreshHpUi();
        }
    }

    private void OnHealClicked(UiEvent evt)
    {
        _hp = F_Math.Clamp(_hp + 20f, 0f, MaxHp);
        RefreshHpUi();
        _hpBar?.ColorTo(new Vector4(0.20f, 0.90f, 0.45f, 1.0f), 0.2);
    }

    private void RefreshHpUi()
    {
        _hpBar?.SetValue(_hp);
        _hpLabel?.SetText($"HP: {_hp:0} / {MaxHp:0}");
        if (_hp < MaxHp * 0.3f)
            _hpBar?.ColorTo(new Vector4(0.92f, 0.22f, 0.20f, 1.0f), 0.25);
    }

    public override void F_Destroy()
    {
        _inputSub?.Dispose();
        _rootPanel?.Remove();
    }
}
```

::: tip 关于 UI 代理失效（Epoch 校验）
调用 `Ui.Load(...)` 或编辑器进出 Play 模式时，UI 树会递增 `Epoch`，之前缓存的 `F_UiElement` 会自动失效并在操作时抛 `EngineException("Bridge.InvalidUi")`。重载 UI 后请重新调用 `Ui.Find(id)` 获取有效代理。
:::

---

## 5. 事件总线与单播命令：`ScriptEvents` 与 `ScriptCommands`

源码位置：[`Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L61) · [`ScriptCommands.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptCommands.cs#L4)

两者定位互补：
- **`ScriptEvents`（广播事件总线）**：用于广播“已经发生的事实”（一对零/多），按指定 `EventPhase` 在主循环确定性派发，支持跨脚本彻底解耦与后台线程安全发布。
- **`ScriptCommands`（单播命令表）**：用于“向单一明确接收者发起同步控制并立刻得到 `bool` 接受/拒绝结果”（一对一），不产生广播队列。

```text
  发布端 (ScriptA / 引擎子系统)                 接收端 (ScriptB / HUD / 音频)
  ────────────────────────────                 ────────────────────────────
  Scripts.Events.Publish(evt, phase)  ──[原生队列]──▶  到达 EventPhase 统一派发给全部订阅者
  Scripts.Commands.Execute(key, arg)  ──[同步直调]──▶  命中唯一注册者，立刻返回 bool 结果
```

### 5.1 `EventPhase` 与 `EventPostResult`

**`EventPhase : uint`**（[`ScriptEvents.cs#L12`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L12)）— 事件投递派发时机：

| 枚举值 | 数值 | 派发时机 |
| :--- | :--- | :--- |
| `NextFrame` | `0` | 推迟到下一渲染帧开始阶段派发 |
| `AfterPhysics` | `1` | 固定物理步结束后派发 |
| `AfterLogic` | `2` | 逻辑更新结束后派发（`Publish` 默认值） |

**`EventPostResult : uint`**（[`ScriptEvents.cs#L14`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L14)）— 发布返回值状态码：

| 枚举值 | 含义 |
| :--- | :--- |
| `Accepted` | 已成功入队等待在指定阶段派发 |
| `Closed` | 事件总线或发布端已关闭（如退出 Play） |
| `UnknownType` | 事件类型未注册 |
| `UnsupportedVersion` | 事件版本号不匹配 |
| `InvalidScope` | 场景作用域无效 |
| `InvalidTarget` | 指定的 `target` 过滤目标无效 |
| `Full` | 事件队列达到上限 |
| `TooLarge` | 序列化后载荷超过单条事件字节上限（自定义事件上限 65536 字节） |
| `SequenceExhausted` | 序列号耗尽 |

### 5.2 `[F_Event]` 特性与自定义事件结构体硬性规则

源码位置：[`ScriptEvents.cs#L17`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L17)

```csharp
[AttributeUsage(AttributeTargets.Struct, Inherited = false)]
public class F_EventAttribute(string key, uint version = 1) : Attribute
{
    public string Key { get; }
    public uint Version { get; }
}
```

::: warning 定义自定义 `[F_Event]` 结构体必须遵守的 4 条硬规则（否则抛 `ArgumentException("Event.UnsupportedPayload")`）
1. **必须是值类型 `struct`**（推荐写 `public readonly record struct` 或带公开属性的 `public struct`）。
2. **必须标注 `[F_Event("稳定键名", version)]`**：`key` 长度为 1..128，仅允许字母、数字与 `.` `_` `:` `-`；`version` 必须 `>= 1`。
3. **禁止出现任何 `public` 字段**，所有需要传递的数据必须是**同时具有 `get` 与 `set`/`init` 的非索引 `public` 属性**（`readonly record struct MyEvent(int Value)` 自动满足此规则）。
4. **属性类型仅允许以下 10 种标量**：`bool`、`int`、`uint`、`long`、`ulong`、`float`、`double`、`string`、任意 `enum`、`EntityReference`。
   - **暂不支持**嵌套自定义结构体、数组/`List<T>`、`Vector3` 或 `F_EntityReference`；如需传三维坐标请拆成 `float X, float Y, float Z`，如需传实体请使用 `EntityReference`（通过 `EntityReference.From(entity)` 构造，`F_EntityReference` 也可隐式转为 `EntityReference`）。
:::

### 5.3 内置事件载荷一览

以下类型无需贴 `[F_Event]` 特性，已由 `ScriptEvents` 内置二进制/JSON 编解码器直接支持发布与订阅：

| 载荷类型 | 字段 / 属性 | 说明 |
| :--- | :--- | :--- |
| `int` | 原生 32 位整数 | 轻量整型消息 |
| `string` | UTF-8 字符串（最大 65536 字节） | 简单文本消息 |
| `PhysicsContact` | `(EntityReference EntityA, EntityReference EntityB, bool Began, bool Ended, Vector3 Point, Vector3 Normal, float Impulse)` | 物理碰撞接触事件（注意：经事件总线编码时仅保序传递 `EntityA`、`EntityB`、`Began`、`Ended` 24 字节；若要读取精确撞击点/冲量请重写 `F_Behaviour.F_CollisionEnter(in F_Contact)`） |
| `PhysicsTrigger` | `(EntityReference TriggerEntity, EntityReference OtherEntity, bool Entered)` | 物理触发器进出事件 |
| `UiEvent` | `(UiEventKind Kind, ulong Target, double Value, string Text)` + `UiBinding? Binding` | GameUI 控件交互事件 |
| `SceneStateChanged` | `(bool Active, uint FormatVersion)` | 场景激活态变更 |
| `SceneActivated` | `(SceneHandle Scene, ulong Request)` | 异步场景加载并激活完成 |
| `SceneUnloaded` | `(SceneHandle Scene, ulong Request)` | 场景卸载完成 |
| `SceneLoadFailed` | `(SceneHandle Scene, ulong Request, string Error)` | 场景加载失败及错误键 |
| `TimeChanged` | `(long Day, double Time, double SimulationSeconds)` | 昼夜时间跳转或切换 Profile 时触发 |
| `Sunrise` | `(long Day, double Time, double SimulationSeconds)` | 昼夜推进跨越日出阈值时触发 |
| `Sunset` | `(long Day, double Time, double SimulationSeconds)` | 昼夜推进跨越日落阈值时触发 |

### 5.4 `EventHeader` 与 `EventStatistics`

**`EventHeader`**（[`ScriptEvents.cs#L20`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L20)）— 带头信息的订阅回调收到的只读元数据：
`World` (`ulong`)、`Channel` (`ulong`)、`Type` (`ulong`)、`Source` (`ulong`)、`SourceStableId` (`ulong`)、`Target` (`ulong`)、`Scene` (`ulong`)、`Frame` (`ulong`)、`FixedStep` (`ulong`)、`Sequence` (`ulong`)、`Version` (`uint`)、`Phase` (`EventPhase`)。

**`EventStatistics`**（[`ScriptEvents.cs#L27`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L27)）— 通过 `Scripts.Events.Statistics` 读取的诊断计数器：
`Accepted`、`Delivered`、`Rejected`、`DroppedTargets`、`NoReceiver`、`CallbackFailures`、`InvalidPayload`、`Pending`、`Queued`、`PendingBytes`、`PeakPending`、`PeakBytes`（以上均为 `ulong`）与 `Closed` (`uint`)。

### 5.5 `ScriptEvents` 与 `ScriptEventPublisher<T>`

源码位置：[`ScriptEvents.cs#L50`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L50) · [`#L61`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptEvents.cs#L61)

**`ScriptEvents` 属性与方法：**

| 成员签名 | 说明 |
| :--- | :--- |
| `int Count { get; }` | 当前活跃订阅数 |
| `EventStatistics Statistics { get; }` | 读取原生事件总线的统计快照 |
| `string TraceJson { get; }` | 读取最近事件流转追踪 JSON（用于排障） |
| `EventPostResult Publish<T>(T value, EventPhase phase = EventPhase.AfterLogic, ulong source = 0, ulong sourceStableId = 0, ulong target = 0, ulong scene = 0) where T : notnull` | 发布强类型事件；可选填写发起者实体句柄/`StableId`、定向接收实体 `target`（`0` 表示广播）或限定场景 `scene`（`0` 表示全部场景） |
| `ScriptEventPublisher<T> CreatePublisher<T>()` | 创建一个仅持有弱原生句柄与编解码器的轻量发布端；**可安全传给后台线程**投递事件而不触碰主线程 `World`/`Session` |
| `IDisposable Subscribe<T>(F_Behaviour owner, Action<T> handler)` | 订阅类型 `T` 的事件；在 `F_Behaviour` 内可直接调用基类快捷方法 `Subscribe<T>(handler)`。脚本销毁时自动退订，也可调用返回的 `IDisposable.Dispose()` 提前退订 |
| `IDisposable Subscribe<T>(F_Behaviour owner, Action<EventHeader, T> handler, ulong target = 0, ulong scene = 0)` | 订阅类型 `T` 并接收完整 `EventHeader`，支持按 `target` 实体或 `scene` 作用域过滤 |

**`ScriptEventPublisher<T>` 方法：**
- `EventPostResult Publish(T value, EventPhase phase = EventPhase.AfterLogic, ulong source = 0, ulong sourceStableId = 0, ulong target = 0, ulong scene = 0)` — 线程安全地投递一条 `T` 事件；总线关闭后直接返回 `EventPostResult.Closed`。

### 5.6 `ScriptCommands`

源码位置：[`ScriptCommands.cs#L4`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptCommands.cs#L4)

| 成员签名 | 参数说明 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `int Count { get; }` | — | `int` | 当前已注册命令总数 |
| `IDisposable Register<T>(F_Behaviour owner, string key, Func<T, bool> handler) where T : notnull` | `owner`：接收脚本；`key`：命令名（1..128 字符，字母/数字/`. _ : -`）；`handler`：同步处理器，返回 `true` 表示接受执行 | `IDisposable` | 注册全局唯一单播命令；重名会抛 `InvalidOperationException("Script.CommandDuplicate")`；`owner` 销毁时自动注销 |
| `bool Execute<T>(string key, T value) where T : notnull` | `key`：命令名；`value`：参数（类型必须与注册时的 `T` 完全一致） | `bool` | 同步执行命令并返回处理器结果；命令不存在、参数类型不匹配或接收脚本已禁用时返回 `false` |

### 5.7 示例：跨脚本发布/订阅“拾取金币”事件 + 单播商店购买命令

```csharp
using FlareEngine;

// 1. 定义强类型事件载荷：必须是 struct + [F_Event] + 公开标量属性（无 public 字段）
[F_Event("game.coin_picked_up", version: 1)]
public readonly record struct CoinPickedUp(
    int Amount,
    string CollectorTag,
    EntityReference CoinEntity);

// 2. 金币实体脚本：玩家碰触触发器时广播事件并销毁自己
public class CoinPickup : F_Behaviour
{
    [F_Expose] public int Value = 10;
    private bool _collected;

    public override void F_TriggerEnter(F_Entity other)
    {
        if (_collected || !other.CompareTag("Player")) return;
        _collected = true;

        var payload = new CoinPickedUp(Value, other.Tag, EntityReference.From(Entity));
        EventPostResult res = Scripts.Events.Publish(
            payload,
            phase: EventPhase.AfterPhysics,
            source: Entity.Handle,
            sourceStableId: Entity.StableId);

        if (res == EventPostResult.Accepted)
            DestroySelf();
    }
}

// 3. 分数与商店管理脚本：订阅“拾取金币”事件，并对外注册同步扣款命令
public class WalletManager : F_Behaviour
{
    public int Coins { get; private set; }

    public override void F_Start()
    {
        // 使用基类快捷 Subscribe<T> 订阅广播事件（随脚本销毁自动退订）
        Subscribe<CoinPickedUp>(OnCoinPickedUp);

        // 注册单播命令：调用方立刻知道金币够不够
        Scripts.Commands.Register<int>(this, "wallet.spend", cost =>
        {
            if (cost <= 0 || Coins < cost) return false;
            Coins -= cost;
            F_Log.Info($"消费 {cost} 金币成功，剩余 {Coins}");
            return true;
        });
    }

    private void OnCoinPickedUp(CoinPickedUp evt)
    {
        Coins += evt.Amount;
        F_Log.Info($"[{evt.CollectorTag}] 拾取金币 +{evt.Amount}，当前总计 {Coins}");
    }
}

// 4. 商店交互脚本：需要同步拿结果时走 ScriptCommands
public class ShopPedestal : F_Behaviour
{
    [F_Expose] public int PotionPrice = 25;

    public override void F_Update(float delta)
    {
        if (F_Input.KeyDown(F_Key.B))
        {
            bool bought = Scripts.Commands.Execute("wallet.spend", PotionPrice);
            F_Log.Info(bought ? "购买药水成功！" : "金币不足，无法购买。");
        }
    }
}
```

---

## 6. 昼夜循环与运行时画质：`TimeOfDayService` 与 `EngineSession`

源码位置：[`Source/FunctionLayer/Scripting/Managed/TimeOfDay.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/TimeOfDay.cs#L14) · [`EngineSession.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L9)

### 6.1 `TimeOfDayService`（通过 `Scripts.Session.TimeOfDay` 访问）

昼夜系统根据主场景挂载的昼夜 Profile 持续推进日历天数与归一化一天时刻，并自动插值太阳方向、光照颜色、天空色与雾效，同时在跨越日出/日落边界时向 `ScriptEvents` 派发 `Sunrise` / `Sunset` 事件。

**`EnvironmentVector`**（[`TimeOfDay.cs#L6`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/TimeOfDay.cs#L6)）：`readonly record struct EnvironmentVector(float X, float Y, float Z)`。

**`EnvironmentSnapshot`**（[`TimeOfDay.cs#L7`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/TimeOfDay.cs#L7)）— 当前环境只读快照：

| 属性 | 类型 | 说明 |
| :--- | :--- | :--- |
| `Active` | `bool` | 当前场景是否已激活昼夜 Profile |
| `FrameIndex` | `ulong` | 采样对应的帧序号 |
| `Day` | `long` | 当前天数计数（从 `0` 起计，合法范围 `[-1_000_000_000, 1_000_000_000]`） |
| `Time` | `double` | 一天内的归一化时刻，范围 **`[0.0, 1.0)`**：`0.0` = 00:00 午夜、`0.25` = 06:00 清晨、`0.5` = 12:00 正午、`0.75` = 18:00 傍晚 |
| `SimulationSeconds` | `double` | 自运行开始累计的模拟秒数 |
| `Rate` | `double` | 当前时间流速倍率（`0` 暂停昼夜流动，`1` 正常流速） |
| `Daylight` | `bool` | 当前是否处于白昼区间（`sunrise <= Time < sunset`） |
| `SunDirection` | `EnvironmentVector` | 太阳方向向量 |
| `SunColor` | `EnvironmentVector` | 太阳光颜色（RGB） |
| `SunIntensity` | `float` | 太阳光强度 |
| `SkyColor` | `EnvironmentVector` | 天空主色（RGB） |
| `FogColor` | `EnvironmentVector` | 雾效颜色（RGB） |
| `FogDensity` | `float` | 雾效密度 |

**`TimeOfDayService` 属性与方法：**

| 成员签名 | 参数说明 | 说明 |
| :--- | :--- | :--- |
| `EnvironmentSnapshot Snapshot { get; }` | — | 读取当前帧的昼夜与环境光照快照 |
| `void SetTime(long day, double time, bool replayBoundaries = false)` | `day`：目标天数；`time`：归一化时刻 **`[0.0, 1.0)`**；`replayBoundaries`：`true` 时按顺序补发前进跨越的 `Sunrise`/`Sunset` 事件 | 立即跳转时间并触发一次 `TimeChanged` 事件（`EventPhase.AfterLogic`） |
| `void SetRate(double rate)` | `rate`：时间流速倍率（必须有限且 `>= 0`） | 调整昼夜流逝倍速；传 `0` 冻结太阳移动 |

::: warning 昼夜调用边界
- `time` 必须严格满足 `0.0 <= time < 1.0`（例如传 `1.0` 会抛出 `EngineException("TOD.InvalidTime")`）。
- 若当前场景未配置昼夜 Profile（`Snapshot.Active == false`），调用 `SetTime` 或 `SetRate` 会抛出 `EngineException("TOD.NoProfile")`。
- 当 `replayBoundaries: true` 且目标时间早于当前时间时，会抛出 `EngineException("TOD.ReplayBackwards")`。
:::

### 6.2 玩家画质：`F_QualityLevel` 与 `EngineSession.PlayerQuality`

源码位置：[`EngineSession.cs#L9`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L9) · [`#L134`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L134) · [`#L142`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L142)

**`F_QualityLevel`**（旧名兼容：`QualityLevel`）— 玩家运行时画质档位枚举：

| 枚举值 | 数值 | 含义 |
| :--- | :--- | :--- |
| `Project` | `0` | 跟随项目：完整沿用作者在场景渲染设置中调好的原画质 |
| `Performance` | `1` | 性能优先：按项目设置中的低开销档位覆盖阴影/泛光/抗锯齿 |
| `Balanced` | `2` | 均衡档位：在清晰度与帧率间折中 |
| `High` | `3` | 高画质档位：按项目设置启用更高质量抗锯齿与效果 |

**`EngineSession` 画质与运行时外观控制成员（通过 `Scripts.Session` 访问）：**

| 成员签名 | 参数说明 | 说明 |
| :--- | :--- | :--- |
| `QualityLevel PlayerQuality { get; set; }` | 写入合法档位值（可由 `(QualityLevel)F_QualityLevel.High` 转换） | 读取当前生效画质档位，或即时切换并持久化到玩家本地偏好文件 |
| `void ConfigurePlayerQuality(string gameId, bool showMenu = true, string? profilesJson = null)` | `gameId`：非空游戏唯一标识；`showMenu`：是否启用内置 `F10` 菜单；`profilesJson`：三档覆盖 JSON（`null` 表示 `"{}"`） | **仅在窗口运行前由独立游戏宿主调用**；将画质文件定位到 `%LOCALAPPDATA%\FlareEngine\Players\<SHA256(gameId)>\quality.txt` |
| `void SetLook(int mode, float blend = 1f)` | `mode`：`0` 基础色 / `1` 白模 / `2` Toon / `-1` 清除并恢复场景设置；`blend`：混合权重 | 运行时切换全局外观覆盖模式 |
| `void SetSky(SkyState? state)` | `state`：`SkyState` 结构体（含 `Zenith`/`Horizon`/`Ground`/`SunColor`、`SunIntensity`、`SunAngularRadius`，颜色为线性 `0..1`），传 `null` 恢复场景默认 | 运行时覆盖程序化天空颜色与太阳盘参数 |

::: warning 编辑器预览下读写 `PlayerQuality` 的注意事项
1. 导出后的独立游戏（`Play.exe`）启动时会自动配置玩家画质偏好；而在**编辑器内直接按 Play 预览**时未配置独立玩家存档，访问 `Scripts.Session.PlayerQuality` 会抛出 `EngineException("PlayerQuality.NotConfigured")`。制作自制设置界面时请用 `try / catch (EngineException)` 兜底。
2. `EngineSession.PlayerQuality` 的属性声明类型为兼容枚举 `QualityLevel`，与 `F_QualityLevel` 数值完全一致；脚本中若使用 `F_QualityLevel` 枚举，读写时请做显式转换：`(F_QualityLevel)Scripts.Session.PlayerQuality` 与 `Scripts.Session.PlayerQuality = (QualityLevel)level`。
:::

### 6.3 示例：监听日出日落自动开关街灯 + 快捷键切时间与画质

```csharp
using FlareEngine;

public class WorldTimeAndQualityController : F_Behaviour
{
    [F_Expose] public F_Entity? StreetLampLight;

    public override void F_Start()
    {
        // 监听昼夜边界事件，自动开关街灯
        Subscribe<Sunrise>(evt =>
        {
            if (StreetLampLight is { IsValid: true }) StreetLampLight.Enabled = false;
            F_Log.Info($"第 {evt.Day} 天日出（时刻 {evt.Time:F2}），关闭街灯");
        });

        Subscribe<Sunset>(evt =>
        {
            if (StreetLampLight is { IsValid: true }) StreetLampLight.Enabled = true;
            F_Log.Info($"第 {evt.Day} 天日落（时刻 {evt.Time:F2}），点亮街灯");
        });

        // 按初始白昼状态同步一次
        EnvironmentSnapshot snap = Scripts.Session.TimeOfDay.Snapshot;
        if (snap.Active && StreetLampLight is { IsValid: true })
            StreetLampLight.Enabled = !snap.Daylight;
    }

    public override void F_Update(float delta)
    {
        TimeOfDayService tod = Scripts.Session.TimeOfDay;

        // 按 1 跳到正午 12:00，按 2 跳到深夜 21:00（需场景配置了昼夜 Profile）
        if (tod.Snapshot.Active)
        {
            if (F_Input.KeyDown(F_Key.D1))
                tod.SetTime(tod.Snapshot.Day, 0.50, replayBoundaries: false);
            else if (F_Input.KeyDown(F_Key.D2))
                tod.SetTime(tod.Snapshot.Day, 0.875, replayBoundaries: false);
            else if (F_Input.KeyDown(F_Key.D3))
                tod.SetRate(tod.Snapshot.Rate > 0.0 ? 0.0 : 1.0); // 暂停 / 恢复时间流动
        }

        // 按 F8 循环切换玩家画质档位（在导出独立游戏包中生效并自动持久化）
        if (F_Input.KeyDown(F_Key.F8))
            CyclePlayerQuality();
    }

    private void CyclePlayerQuality()
    {
        try
        {
            var current = (F_QualityLevel)Scripts.Session.PlayerQuality;
            var next = (F_QualityLevel)(((int)current + 1) % 4);
            Scripts.Session.PlayerQuality = (QualityLevel)next;
            F_Log.Info($"已切换玩家画质档位: {current} -> {next}");
        }
        catch (EngineException ex) when (ex.Key == "PlayerQuality.NotConfigured")
        {
            F_Log.Warn("当前为编辑器预览会话，未启用独立玩家画质偏好；请在导出后的 Play.exe 中测试画质切换。");
        }
    }
}
```
