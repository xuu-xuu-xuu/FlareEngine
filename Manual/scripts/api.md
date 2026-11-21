# 高频游戏 API：变换、时间、输入、数学与异步

`FlareEngine` 命名空间在 `Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs` 与 `Behaviour.cs` 中提供面向日常玩法编写的 `F_` 静态门面与实体扩展，所有空间运算与引擎右手系（局部前方向为 `-Z`、上方向为 `+Y`、右方向为 `+X`，欧拉角为角度制 `Z -> X -> Y`）严格一致。

## F_Entity

`F_Entity`（及其运行时派生类 `Entity`）除底层 `Transform` 值结构外，直接提供可就地读写的空间属性与组件查询接口：

- **方位与父子层级属性**：`Position`、`Rotation`、`LocalScale`、`EulerAngles`（角度制局部读写）、`WorldPosition`、`WorldRotation`（世界空间读写）、`Parent` 与 `SetParent(F_Entity? parent, bool keepWorld = true)`，以及世界空间单位方向向量 `Forward`（`-Z`）、`Right`（`+X`）、`Up`（`+Y`）。当实体挂载有 `RigidBody` 或 `CharacterController` 时，修改 `Position / Rotation / EulerAngles` 会自动路由至底层物理 `Teleport`，无需手动规避变换所有权异常；同帧刚创建的待激活实体亦可立即读写名称、稳定 ID 与空间方位。

### Translate

`void Translate(Vector3 delta, bool localSpace = true)` 按局部空间（默认，随当前朝向旋转）或世界空间平移实体。`Rotate(Vector3 eulerDegrees, bool localSpace = true)` 与 `LookAt(Vector3 worldTarget, Vector3? worldUp = null)` 分别执行增量旋转与世界空间朝向目标点；这三个方法同样暴露在 `F_Behaviour` 上。

### GetComponent

`T? GetComponent<T>() where T : class` 在同一实体上查找已挂载的其他 `F_Behaviour` 脚本实例、`RigidBody` 或 `CharacterController`。配套提供 `bool TryGetComponent<T>([NotNullWhen(true)] out T? component)` 与 `IReadOnlyList<T> GetComponents<T>()`。

## F_Scene

`F_Scene` 提供运行期实体查找、脚本服务定位、深度克隆与场景重载入口：

- `F_Scene.Find(ulong stableId)` / `F_Scene.Find(string name)`：按稳定十进制 ID 或精确实体名称查找存活实体。
- `F_Scene.FindObjectOfType<T>()` / `F_Scene.FindObjectsOfType<T>()`：查找当前激活的 `F_Behaviour`（或接口）实例。
- `F_Scene.Reload()`：登记当前场景重载请求，在当前帧步进安全结束后还原启动快照并重新初始化 `ScriptSystem`。

### Instantiate

`Entity Instantiate(F_Entity source, Vector3? position = null, Quaternion? rotation = null)` 对源实体执行**完整子树深度克隆**：原生侧同步复制整棵父子层级、`MeshComponent`（含全部材质槽）、灯光、相机与全部 9 类特性组件（物理刚体、碰撞体、角色控制、骨骼、音频、粒子、导航代理），托管侧克隆子树内所有 `F_Behaviour` 脚本并自动将子树内部的 `EntityReference` 重映射到对应克隆体。`F_Behaviour.Destroy(F_Entity target, float delaySeconds = 0f)` 支持立即销毁或按模拟时钟延迟若干秒后安全移除实体。

## F_Time

`F_Time` 由 `ScriptSystem` 在每帧与固定步自动更新：

| 属性 | 类型 | 含义 |
| :--- | :--- | :--- |
| `DeltaTime` | `float` | 乘以 `TimeScale` 后的本帧步长（秒） |
| `UnscaledDeltaTime` | `float` | 不受 `TimeScale` 影响的真实帧间隔（秒） |
| `FixedDeltaTime` | `float` | 物理固定步长（默认 `1/60` 秒） |
| `Time` / `UnscaledTime` | `float` | 自进入运行态以来的累计缩放时间 / 真实时间（秒） |
| `TimeScale` | `float` | 全局时间倍率（默认 `1f`，支持 `0f` 慢放/定格，上限 `100f`） |
| `FrameCount` | `ulong` | 运行态已推进的总帧数 |

## F_Input、F_Cursor 与 F_Camera

`F_Input` 配合 `F_Key` 枚举（字母 `A`–`Z`、数字 `D0`–`D9`、方向键、功能键 `F1`–`F12`、`Space`、`Shift`、`Control`、`Escape` 等）读取当前帧未被编辑器 UI 消费的键鼠输入：

- **按键与鼠标**：`Key(F_Key)`（按住）、`KeyDown(F_Key)`（本帧按下）、`KeyUp(F_Key)`（本帧抬起）、`Mouse(MouseButton)`、`MouseDown(MouseButton)`、`MouseUp(MouseButton)`、`MousePosition`、`MouseDelta`（逐帧鼠标像素位移，适配 FPS/TPS 自由视角）、`MouseWheel`。
- **光标锁定（`F_Cursor`）**：`F_Cursor.Locked` 与 `F_Cursor.Visible` 控制游戏视口内的原生光标捕获与隐藏（编辑器运行态按 `Esc` 可临时释放光标，点击游戏视口恢复锁定；退出运行或窗口失焦时 RAII 自动释放）。
- **屏幕拾取射线（`F_Camera`）**：`F_Camera.ScreenPointToRay(Vector2 screenPixel, F_Entity? camera = null)`（或相机实体上的 `cameraEntity.ScreenPointToRay(...)`）将屏幕像素坐标转换为主相机世界空间 `F_Ray(Origin, Direction)`，可直接传入 `Physics.Raycast` 完成 3D 鼠标点选。
- **复合轴向**：`Horizontal`（`A/D` 或左右方向键）、`Vertical`（`S/W` 或上下方向键）与自动归一化的二维移动向量 `MoveVector`。需要键鼠与手柄统一映射时可结合 [InputActions](../platform/input.md#inputactions)。

## F_Math

`F_Math` 提供游戏常用标量与三维向量运算：`PI`、`Deg2Rad`、`Rad2Deg`、`Clamp`、`Clamp01`、`Lerp`、`LerpUnclamped`、`InverseLerp`、`MoveTowards`、`Repeat`、`PingPong`、`DeltaAngle`、`LerpAngle` 与 `Approximately`。

### SmoothDamp

`float SmoothDamp(float current, float target, ref float currentVelocity, float smoothTime, float maxSpeed = float.PositiveInfinity, float? deltaTime = null)`（含 `Vector3` 重载）采用临界阻尼弹簧模型平滑追踪目标值，`deltaTime` 省略时默认读取 `F_Time.DeltaTime`。

## F_Quat

`F_Quat` 封装四元数与角度制欧拉角互转及球面插值：`Identity`、`Slerp(a, b, t)`、`Angle(a, b)` 与 `RotateTowards(from, to, maxDegreesDelta)`。

### Euler

`Quaternion Euler(float xDegrees, float yDegrees, float zDegrees)`（及 `Vector3` 重载）按与属性面板 `SceneInspector` 完全一致的 `Z -> X -> Y` 顺序构造单位四元数；`Vector3 ToEuler(Quaternion)` 逆向解算角度制欧拉角。

### LookRotation

`Quaternion LookRotation(Vector3 forward, Vector3? worldUp = null)` 将实体局部 `-Z` 轴对齐到给定世界前方向，共线退化时自动选取正交上方向，避免产生 `NaN`。

## F_Log

`F_Log.Info(message)`、`F_Log.Warn(message)` 与 `F_Log.Error(message)` 利用 `[CallerFilePath]` 与 `[CallerLineNumber]` 自动附带调用点 `文件名.cs(行号,1)` 前缀并写入无锁队列；编辑器每帧排空队列并实时显示在 **控制台** 面板，双击日志行可直接跳到外部编辑器对应源码行。

## F_Await

`F_Await` 实现零后台线程、与主循环帧序完全一致的轻量化 `INotifyCompletion` 等待器：

```csharp
public async void SpawnWave()
{
    await F_Await.NextFrame();
    await F_Await.Seconds(1.5f);
    await F_Await.FixedUpdate();
}
```

### Seconds

`StepAwaitable Seconds(float seconds)` 按缩放后的模拟时间等待指定秒数；`NextFrame()` 等待下一渲染帧 `F_Update` 阶段，`FixedUpdate()` 等待下一物理固定步。所有挂起延续由当前 `ScriptSystem` 托管，退出运行预览或卸载程序集时自动清空，不会在停止后继续触发已销毁对象。

## RigidBody

`RigidBody` 除完整状态查询与指定作用点冲量接口外，提供直接读写线性/角速度的 `Velocity`、`AngularVelocity` 属性，以及作用于质心的 `AddForce(Vector3)` 与 `AddImpulse(Vector3)` 快捷方法。
