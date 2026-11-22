# FlareEngine C# API 参考手册

本手册按功能模块整理了 `FlareEngine` 命名空间下的**全部公开脚本 API**——包含类与结构体字段、方法完整签名、参数说明、返回值、枚举对照表与可直接复制的示例代码，风格与 Unity Scripting API 保持一致，方便写游戏逻辑时随时查阅。

::: tip 如果刚接触 Flare 脚本编写
建议先花 3 分钟读 [编写与挂载 C# 脚本](../scripting.md)，了解脚本创建流程、零重启热重载和 C# 基础语法结构，再把本手册当作日常字典使用。
:::

## 通用约定（查 API 前先看这 4 条）

1. **命名空间**：脚本顶部引入 `using FlareEngine;` 与 `using System.Numerics;`（提供 `Vector2` / `Vector3` / `Quaternion`）。
2. **统一 `F_` 前缀与向下兼容**：所有引擎高频脚本 API 统一采用 `F_` 前缀（如 `F_Behaviour`、`F_Entity`、`F_Physics`、`F_Rigidbody`、`F_Audio`、`F_GameUi`）。旧版无前缀写法（如 `Entity`、`RigidBody`、`CharacterController`、`Audio`）仍保持完全兼容，但**新脚本推荐统一写 `F_` 前缀**，代码补全更集中。
3. **坐标系与物理单位**：
   - **右手坐标系**：`+X` 向右、`+Y` 向上、**`-Z` 为物体局部正前方**（即 `Entity.Forward`）。
   - **单位**：距离为**米 (`m`)**，时间为**秒 (`s`)**，旋转角（`EulerAngles`、`F_Quat.Euler`、`F_Math.DeltaAngle`）为**角度制 (`°`)**，欧拉角旋转顺序固定为 **`Z → X → Y`**（与编辑器属性面板一致）。
4. **中文 IDE 智能提示**：编辑器会自动向项目同步 `FlareEngine.Managed.xml`。在 VS Code / Cursor / Visual Studio / Rider 里把鼠标悬停在任意 `F_` 类型或方法上，也能直接看到中文注释；若搬移过引擎目录，点击 **编辑 → 偏好设置 → 同步 C# 解决方案** 即可一键刷新。

## 模块分类导航

| 章节 | 涵盖的核心类、结构体与枚举 | 适合查询 |
| :--- | :--- | :--- |
| [**核心：行为、实体、场景与特性**](core.md) | `F_Behaviour`、`F_Entity`、`F_Transform`、`F_EntityReference`、`F_Color`、`F_Scene`、`[F_Expose]` 等 8 项特性 | 生命周期回调、移动旋转朝向、父子层级、查找/克隆/销毁实体、面板暴露字段 |
| [**输入、光标、相机、时间、数学与异步**](input-math.md) | `F_Input`、`F_Key`、`F_MouseButton`、`F_InputActions`、`F_Cursor`、`F_Camera`、`F_Ray`、`F_Time`、`F_Math`、`F_Quat`、`F_Log`、`F_Await` | 键鼠与手柄输入、FPS 锁鼠标、屏幕点选射线、帧时间与慢放、平滑阻尼、主线程延时 |
| [**物理、射线、碰撞流形与角色控制**](physics.md) | `F_Physics`、`F_RaycastHit`、`F_Contact`、`F_Rigidbody`、`F_BoxBodyDescription`、`F_SphereBodyDescription`、`F_RigidBodyState`、`F_CharacterController`、`F_CharacterDescription`、`F_CharacterState` | 射线检测与碰撞层掩码、真实撞击点/法线/冲量、刚体施力与推力、胶囊角色移动跳跃 |
| [**音频、粒子、导航、UI、事件、昼夜与画质**](systems.md) | `F_Audio`、`F_AudioPlayDescription`、`F_AudioVoiceHandle`、`F_Particles`、`F_Navigation`、`F_NavPath`、`F_NavPathStatus`、`F_GameUi`、`F_UiElement`、`ScriptEvents`、`ScriptCommands`、`TimeOfDay`、`F_QualityLevel` | 3D 空间音效、粒子爆发控制、NavMesh 寻路、HUD 与按钮绑定、跨脚本事件、昼夜时间与画质档位 |

## 常用“从哪里拿到它”速查

| 你想调用的系统 | 在 `F_Behaviour` 里怎么拿到 | 所属章节 |
| :--- | :--- | :--- |
| 当前挂载的物体 | `Entity`（或直接调用 `Translate` / `Rotate` / `LookAt`） | [核心篇](core.md) |
| 同物体上的刚体 / 角色 / 其他脚本 | `GetComponent<F_Rigidbody>()` / `TryGetComponent<T>(out var comp)` | [核心篇](core.md) · [物理篇](physics.md) |
| 场景查找 / 克隆 / 重载 | `F_Scene.Find(...)` / `F_Scene.Instantiate(...)` / `F_Scene.Reload()` | [核心篇](core.md) |
| 键鼠 / 光标 / 主相机射线 | `F_Input` / `F_Cursor` / `F_Camera`（静态类，直接调用） | [输入与数学篇](input-math.md) |
| 物理射线与全局物理查询 | `F_Physics.Raycast(...)`（静态门面，直接调用） | [物理篇](physics.md) |
| 音频 / 粒子 / 导航 / 动作输入 / UI | `Audio` / `Particles` / `Navigation` / `Actions` / `Ui`（基类内置属性） | [输入与数学篇](input-math.md) · [系统篇](systems.md) |
| 事件总线 / 单播命令 / UI 动作路由 | `Scripts.Events`（或基类 `Subscribe<T>`） / `Scripts.Commands` / `Scripts.UiActions` | [系统篇](systems.md) |
| 昼夜系统 / 玩家画质档位 | `Scripts.Session.TimeOfDay` / `Scripts.Session.PlayerQuality` | [系统篇](systems.md) |
| 时间 / 数学 / 四元数 / 日志 / 异步 | `F_Time` / `F_Math` / `F_Quat` / `F_Log` / `F_Await`（静态类） | [输入与数学篇](input-math.md) |
