# 输入、光标、相机、时间、数学与异步

本页涵盖 `FlareEngine` 命名空间下处理**玩家输入、鼠标锁定、屏幕射线投影、帧时钟、常用数学/四元数、日志与主线程异步等待**的全部公开类型与方法，可直接作为日常开发字典查阅与复制示例。

> 返回 [API 参考总览](index.md) · 关联章节：[核心（实体与场景）](core.md) · [物理与角色控制](physics.md) · [音频、粒子、导航与 UI](systems.md)

## 本页类型速查

| 类型 | 分类 | 常见入口 / 兼容别名 | 一句话用途 |
| :--- | :--- | :--- | :--- |
| [`F_Input`](#f-input) | 静态类 | 直接调用（底层快照 `this.Input`） | 读取键盘、鼠标按键、坐标、位移、滚轮与 WASD 移动轴 |
| [`F_Key`](#f-key) | 枚举 | 直接使用 | 键盘虚拟键码枚举（字母、数字、方向键、修饰键、功能键） |
| [`F_MouseButton`](#f-mousebutton) | 枚举 | 兼容别名 `MouseButton` | 鼠标左/右/中键枚举 |
| [`F_InputActions`](#f-inputactions) | 服务类 | `this.Actions`（兼容子类 `InputActions`） | 键鼠 + 手柄统一动作映射与运行时 JSON 改键 |
| [`F_Cursor`](#f-cursor) | 静态类 | 直接调用 | 鼠标锁定到窗口中心与光标显隐控制（自带 `Esc` 释放） |
| [`F_Camera`](#f-camera) / [`F_Ray`](#f-ray) | 静态类 / 结构体 | `F_Camera.ScreenPointToRay` / `entity.ScreenPointToRay` | 将屏幕像素坐标反投影为世界空间三维射线 |
| [`F_Time`](#f-time) | 静态类 | 直接调用 | 帧间隔、固定步长、累计时间、帧数与全局时间缩放 |
| [`F_Math`](#f-math) | 静态类 | 直接调用 | 夹取、线性插值、角度最短差、`SmoothDamp` 弹簧阻尼 |
| [`F_Quat`](#f-quat) | 静态类 | 直接调用 | 右手系欧拉角构造/还原、`LookRotation` 与球面插值 |
| [`F_Log`](#f-log) | 静态类 | 直接调用 | 输出带源文件名与行号的日志（控制台双击直达代码） |
| [`F_Await`](#f-await) | 静态类 | `await F_Await.Seconds(...)` 等 | 主线程确定性异步延时与帧同步，销毁/停止预览自动取消 |

---

## `F_Input`、`F_Key` 与 `F_MouseButton` {#f-input}

`F_Input` 是面向日常游戏逻辑的全局静态输入查询入口，通常在 `F_Update`（点选、镜头旋转、瞬时触发）或 `F_FixedUpdate`（角色跳跃、刚体推动）中调用。

源码位置：[FlareErgonomics.cs#L86-L148](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L86) · [Behaviour.cs#L221-L258](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L221)

### `F_Input` 属性与方法

| 成员签名 | 参数说明 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `F_Input.Key(F_Key key)` | `key`：键盘按键枚举 | `bool` | 按键在当前帧**持续按住**时返回 `true` |
| `F_Input.KeyDown(F_Key key)` | `key`：键盘按键枚举 | `bool` | 按键在**本帧刚刚按下**（上升沿）时返回 `true` |
| `F_Input.KeyUp(F_Key key)` | `key`：键盘按键枚举 | `bool` | 按键在**本帧刚刚松开**（下降沿）时返回 `true` |
| `F_Input.Mouse(F_MouseButton button)`<br>`F_Input.Mouse(MouseButton button = MouseButton.Left)` | `button`：鼠标按键（不传参数默认左键） | `bool` | 鼠标按键在当前帧**持续按住**时返回 `true` |
| `F_Input.MouseDown(F_MouseButton button)`<br>`F_Input.MouseDown(MouseButton button = MouseButton.Left)` | `button`：鼠标按键（不传参数默认左键） | `bool` | 鼠标按键在**本帧刚刚按下**时返回 `true` |
| `F_Input.MouseUp(F_MouseButton button)`<br>`F_Input.MouseUp(MouseButton button = MouseButton.Left)` | `button`：鼠标按键（不传参数默认左键） | `bool` | 鼠标按键在**本帧刚刚松开**时返回 `true` |
| `F_Input.MousePosition` | 只读属性 | `Vector2` | 鼠标在游戏视口内的像素坐标，**左上角 `(0, 0)` 为原点，向右 `+X`、向下 `+Y`** |
| `F_Input.MouseDelta` | 只读属性 | `Vector2` | 本帧鼠标相对位移（像素，向右 `+X`、向下 `+Y`）；适合第一/第三人称自由视角 |
| `F_Input.MouseWheel` | 只读属性 | `float` | 本帧鼠标滚轮增量（向前滚动为正、向后为负，无滚动为 `0f`） |
| `F_Input.Axis(F_Key negative, F_Key positive)` | `negative`：负向键；`positive`：正向键 | `float` | 返回 `-1f`、`0f` 或 `+1f`；两键同时按住时相互抵消为 `0f` |
| `F_Input.Horizontal` | 只读属性 | `float` | 水平移动轴（`-1..+1`）：优先读取 `A / D`，结果为 `0f` 时回退 `Left / Right` |
| `F_Input.Vertical` | 只读属性 | `float` | 前后移动轴（`-1..+1`）：优先读取 `S / W`，结果为 `0f` 时回退 `Down / Up` |
| `F_Input.MoveVector` | 只读属性 | `Vector2` | 由 `(Horizontal, Vertical)` 组成的平面输入向量；斜向同时按住时自动归一化（模长不超过 `1`） |

::: info 坐标系、UI 消费屏蔽与固定步边沿规则
1. **像素坐标系**：无论是在独立运行窗口还是编辑器运行预览（Play Mode）中，`MousePosition` 都已经转换到**游戏视口内部坐标系**（左上角 `(0, 0)`），可直接传入 `F_Camera.ScreenPointToRay(F_Input.MousePosition)`。
2. **UI 消费屏蔽规则**（防止点按钮时误开枪、打字时误移动）：
   - **键盘被 UI 消费**（GameUI 文本框获焦、弹出暂停/画质菜单，或编辑器中点击了属性栏等非视口区域）：`Key`、`KeyDown`、`KeyUp`、`Axis`、`Horizontal`、`Vertical`、`MoveVector` 全部返回 `false` / `0f` / `Vector2.Zero`。
   - **指针被 UI 消费**（鼠标悬停或点击在可交互 GameUI 控件上、弹出菜单打开，或指针离开编辑器游戏视口）：`Mouse`、`MouseDown`、`MouseUp` 返回 `false`，`MouseWheel` 返回 `0f`，`MouseDelta` 返回 `Vector2.Zero`（`MousePosition` 仍保留最新坐标）。
3. **固定步不会漏键**：在 `F_FixedUpdate` 中调用 `KeyDown` / `MouseDown` 时，`ScriptSystem` 会合并渲染帧内的按键边沿并在固定步结束后统一清除，高帧率下也不会丢跳跃触发。
:::

::: tip 需要判断是否被 UI 消费或查询未枚举的虚拟键码？
在 `F_Behaviour` 子类中可以通过 `this.Input`（类型 `ScriptInput`）访问当前帧原始快照：
- `Input.KeyboardConsumed` / `Input.Pointer.Consumed`：查看本帧键盘或鼠标是否已被 UI 消费。
- `Input.Down(int vk)` / `Input.Pressed(int vk)` / `Input.Released(int vk)`：按 `0..255` 虚拟键码直接查询。
:::

#### 示例：WASD 移动、滚轮缩放与自定义按键轴

```csharp
using System.Numerics;
using FlareEngine;

public class SimpleInputDemo : F_Behaviour
{
    [F_Expose] public float MoveSpeed = 5f;
    [F_Expose] public float RollSpeed = 90f;

    public override void F_Update(float delta)
    {
        // 1. 归一化平面移动（右手系：+X 向右，-Z 向前）
        Vector2 move = F_Input.MoveVector;
        if (move.LengthSquared() > 1e-4f)
        {
            var worldDelta = new Vector3(move.X, 0f, -move.Y) * (MoveSpeed * delta);
            Translate(worldDelta, localSpace: false);
        }

        // 2. 用 Q / E 组合自定义一维轴控制滚转
        float roll = F_Input.Axis(F_Key.Q, F_Key.E);
        if (roll != 0f)
            Rotate(new Vector3(0f, 0f, -roll * RollSpeed * delta));

        // 3. 鼠标左键瞬时点击与滚轮读取
        if (F_Input.MouseDown(F_MouseButton.Left))
            F_Log.Info($"左键点击像素坐标: {F_Input.MousePosition}");

        if (MathF.Abs(F_Input.MouseWheel) > 0.01f)
            F_Log.Info($"滚轮增量: {F_Input.MouseWheel:F2}");
    }
}
```

### `F_Key` 枚举完整对照表 {#f-key}

`F_Key` 数值与标准 Windows 虚拟键码（Virtual-Key Codes）对齐，源码位置：[FlareErgonomics.cs#L9-L38](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L9)。

| 分组 | 枚举成员与数值 |
| :--- | :--- |
| **字母键（26）** | `A = 65`, `B = 66`, `C = 67`, `D = 68`, `E = 69`, `F = 70`, `G = 71`, `H = 72`, `I = 73`, `J = 74`, `K = 75`, `L = 76`, `M = 77`, `N = 78`, `O = 79`, `P = 80`, `Q = 81`, `R = 82`, `S = 83`, `T = 84`, `U = 85`, `V = 86`, `W = 87`, `X = 88`, `Y = 89`, `Z = 90` |
| **主键盘数字键（10）** | `D0 = 48`, `D1 = 49`, `D2 = 50`, `D3 = 51`, `D4 = 52`, `D5 = 53`, `D6 = 54`, `D7 = 55`, `D8 = 56`, `D9 = 57` |
| **方向键（4）** | `Left = 37`, `Up = 38`, `Right = 39`, `Down = 40` |
| **修饰键（3）** | `Shift = 16`, `Control = 17`, `Alt = 18` |
| **功能键（12）** | `F1 = 112`, `F2 = 113`, `F3 = 114`, `F4 = 115`, `F5 = 116`, `F6 = 117`, `F7 = 118`, `F8 = 119`, `F9 = 120`, `F10 = 121`, `F11 = 122`, `F12 = 123` |
| **常用控制与编辑键（12）** | `None = 0`, `Backspace = 8`, `Tab = 9`, `Enter = 13`, `Escape = 27`, `Space = 32`, `PageUp = 33`, `PageDown = 34`, `End = 35`, `Home = 36`, `Insert = 45`, `Delete = 46` |

### `F_MouseButton` 枚举 {#f-mousebutton}

兼容无前缀别名：`MouseButton`（两者数值完全一致，均可直接传给 `F_Input.Mouse / MouseDown / MouseUp`）。源码位置：[Behaviour.cs#L222-L223](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L222)。

| 成员 | 数值 | 说明 |
| :--- | :---: | :--- |
| `F_MouseButton.Left` | `0` | 鼠标左键（`F_Input.Mouse()` 缺省值） |
| `F_MouseButton.Right` | `1` | 鼠标右键 |
| `F_MouseButton.Middle` | `2` | 鼠标中键（滚轮按下） |

---

## `F_InputActions`（动作映射输入） {#f-inputactions}

`F_InputActions` 将具体硬件输入（键盘、鼠标、最多 4 个手柄）解耦为语义化的“动作名”（如 `"Move"`、`"Jump"`、`"Fire"`），让同一份玩法代码同时支持键鼠与手柄，并支持运行时通过 JSON 热加载改键。

- **获取入口**：在 `F_Behaviour` 子类中直接通过内置属性 **`this.Actions`**（等价于 `Scripts.Session.Actions`）访问。
- **兼容子类**：`InputActions` 继承自 `F_InputActions`，两者 API 完全一致。
- **源码位置**：[InputActions.cs#L7-L73](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/InputActions.cs#L7) · [Behaviour.cs#L132](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Behaviour.cs#L132)

::: warning 注意属性名是 `Actions`
在 `F_Behaviour` 脚本基类中，动作映射服务的属性名是 **`Actions`**（例如 `Actions.IsActionPressed("Jump")`），而不是 `InputActions`（`InputActions` 是兼容类名）。
:::

### 方法一览

| 方法签名 | 参数说明 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `LoadJson(string json)` | `json`：UTF-8 JSON 配置文本（`format: "FlareEngine.InputActions"`, `version: 1`，上限 1 MB） | `void` | 运行时解析并替换当前动作映射表；格式非法抛出 `EngineException` |
| `IsActionDown(string action)` | `action`：动作名称（区分大小写，非空，上限 128 字节） | `bool` | 任一绑定的按键/鼠标/手柄输入当前帧持续触发时返回 `true`；未定义动作返回 `false` |
| `IsActionPressed(string action)` | `action`：动作名称 | `bool` | 任一绑定在本帧刚刚触发上升沿时返回 `true`；未定义动作返回 `false` |
| `GetAxis1D(string action)` | `action`：动作名称 | `float` | 汇总该动作的一维连续输入（键盘/鼠标滚轮/扳机/摇杆单轴），自动应用死区并夹取在 `[-maxScale, +maxScale]`（默认 `[-1, 1]`） |
| `GetAxis2D(string action)` | `action`：动作名称 | `Vector2` | 汇总二维输入（WASD / 方向键 / 手柄摇杆），自动对摇杆做径向死区并限制向量模长不超过 `maxScale`（默认 `1`） |

### 自动加载路径、内置默认动作与屏蔽规则

进入运行预览时，引擎会自动尝试读取项目下的 **`Settings/Input.input.json`**；若项目尚未创建该文件，引擎会启用内置默认动作表（源码：[InputActionAsset.cpp#L194-L252](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Input/InputActionAsset.cpp#L194)）：

| 默认动作名 | 类型（`kind`） | 默认死区 | 默认绑定（键鼠 + 手柄） |
| :--- | :--- | :---: | :--- |
| `Move` | `axis2d` | `0.15` | `W/A/S/D`、方向键、手柄左摇杆（`LeftStick`） |
| `Look` | `axis2d` | `0.15` | 手柄右摇杆（`RightStick`） |
| `Jump` | `button` | `0.15` | `Space`、手柄 `A` |
| `Fire` | `button` | `0.20` | 鼠标左键（`left`）、手柄右扳机（`RightTrigger`） |
| `Interact` | `button` | `0.15` | `E`、手柄 `X` |

- **按设备的 UI 屏蔽规则**（源码：[InputActionMap.cpp#L127-L136](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Input/InputActionMap.cpp#L127)）：
  - `keyboard` 绑定在 `keyboardConsumed` 时屏蔽；
  - `mouse` 绑定在 `pointerConsumed` 时屏蔽；
  - `gamepad` 绑定仅在 `keyboardConsumed && pointerConsumed` 同时为真时屏蔽。

### 示例：统一读取键鼠与手柄动作 + 运行时加载自定义动作表

```csharp
using System.Numerics;
using FlareEngine;

public class ActionControlledHero : F_Behaviour
{
    [F_Expose] public float MoveSpeed = 6f;

    public override void F_Start()
    {
        // 可选：在运行时加载自定义动作映射（通常直接用项目 Settings/Input.input.json 即可）
        const string customJson = """
        {
          "format": "FlareEngine.InputActions",
          "version": 1,
          "actions": [
            {
              "name": "Move",
              "kind": "axis2d",
              "deadZone": 0.15,
              "bindings": [
                { "device": "keyboard", "key": 65, "axisX": -1, "axisY": 0, "scale": 1 },
                { "device": "keyboard", "key": 68, "axisX":  1, "axisY": 0, "scale": 1 },
                { "device": "keyboard", "key": 83, "axisX":  0, "axisY": -1, "scale": 1 },
                { "device": "keyboard", "key": 87, "axisX":  0, "axisY":  1, "scale": 1 },
                { "device": "gamepad", "control": "LeftStick", "gamepadIndex": 0, "axisX": 1, "axisY": 1, "scale": 1 }
              ]
            },
            {
              "name": "Throttle",
              "kind": "axis1d",
              "deadZone": 0.1,
              "bindings": [
                { "device": "keyboard", "key": 16, "axisX": 1, "axisY": 0, "scale": 1 },
                { "device": "gamepad", "control": "RightTrigger", "gamepadIndex": 0, "axisX": 1, "axisY": 0, "scale": 1 }
              ]
            },
            {
              "name": "Jump",
              "kind": "button",
              "deadZone": 0.15,
              "bindings": [
                { "device": "keyboard", "key": 32, "axisX": 1, "axisY": 0, "scale": 1 },
                { "device": "gamepad", "control": "A", "gamepadIndex": 0, "axisX": 1, "axisY": 0, "scale": 1 }
              ]
            },
            {
              "name": "Fire",
              "kind": "button",
              "deadZone": 0.2,
              "bindings": [
                { "device": "mouse", "control": "left", "axisX": 1, "axisY": 0, "scale": 1 },
                { "device": "gamepad", "control": "RightShoulder", "gamepadIndex": 0, "axisX": 1, "axisY": 0, "scale": 1 }
              ]
            }
          ]
        }
        """;
        Actions.LoadJson(customJson);
    }

    public override void F_Update(float delta)
    {
        // 读取二维移动向量（WASD 或手柄左摇杆）
        Vector2 move = Actions.GetAxis2D("Move");
        // 读取一维加速轴（Shift 或手柄 RT 扳机，0..1）
        float boost = 1f + Actions.GetAxis1D("Throttle") * 0.8f;

        if (move.LengthSquared() > 1e-4f)
        {
            var dir = Entity.Forward * move.Y + Entity.Right * move.X;
            dir.Y = 0f;
            Translate(dir * (MoveSpeed * boost * delta), localSpace: false);
        }

        // 瞬时触发动作（Space 或手柄 A）
        if (Actions.IsActionPressed("Jump"))
            F_Log.Info("触发跳跃动作！");

        // 持续按住动作（左键或手柄 RB）
        if (Actions.IsActionDown("Fire"))
            F_Log.Info("持续开火中...");
    }
}
```

---

## `F_Cursor`（鼠标光标锁定与显隐） {#f-cursor}

`F_Cursor` 用于控制鼠标光标的可见性与中心锁定状态，是第一人称/第三人称自由视角的核心入口。

源码位置：[FlareErgonomics.cs#L151-L174](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L151) · [NativeInputExports.inl#L80-L109](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativeInputExports.inl#L80) · [EngineNative.cpp#L357-L437](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L357)

### 属性一览

| 属性签名 | 读写 | 默认值 | 说明 |
| :--- | :---: | :---: | :--- |
| `F_Cursor.Locked` | 读 / 写 | `false` | 设为 `true` 时锁定鼠标到视口中心并**自动隐藏光标**；从 `true` 设为 `false` 时自动恢复光标可见。<br>读取时返回**当前实际锁定生效状态**（玩家按 `Esc` 临时释放后会立刻返回 `false`） |
| `F_Cursor.Visible` | 读 / 写 | `true` | 获取或设置系统光标是否显示（仅改变显隐，不改变 `Locked` 状态）。处于锁定生效态时系统光标强制隐藏 |

### 运行态 `Esc` 临时释放与防吞机制

```text
  脚本设 F_Cursor.Locked = true
              │
              ▼
  [锁定生效] 光标隐藏并居中，F_Cursor.Locked == true，F_Input.MouseDelta 输出视角位移
              │
      玩家按 Esc 键
              ▼
  [Esc 临时暂停] 光标立即可见且自由移动，F_Cursor.Locked == false（方便点停止预览或调参数）
              │
      玩家在游戏视口内点击鼠标（或脚本先设 false 再设 true）
              ▼
  [自动重锁] 恢复居中隐藏，F_Cursor.Locked == true
```

::: tip 为什么每帧写 `F_Cursor.Locked = true` 也不会吞掉 `Esc`？
引擎底层（[NativeInputExports.inl#L92-L93](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativeInputExports.inl#L92)）仅在目标锁态发生**从 `false` 到 `true` 的边沿跳变**时才会重置 `Esc` 暂停标记。因此即使你的脚本在 `F_Update` 里每帧重复赋值 `F_Cursor.Locked = true`，玩家按 `Esc` 后依然能顺畅解放鼠标去点击编辑器按钮，直到主动点回视口才会重新锁定。

不过更推荐的标准模式是在 `F_Start` 开启锁定、在 `F_Update` 开头用 `if (!F_Cursor.Locked) return;` 跳过视角转动、在 `F_Destroy` 还原为 `false`。
:::

### 示例：第一人称视角（FPS Look）标准写法

```csharp
using System.Numerics;
using FlareEngine;

public class FpsCameraLook : F_Behaviour
{
    [F_Expose] public F_Entity? CameraPivot;
    [F_Expose, F_Range(0.05f, 1.0f)] public float Sensitivity = 0.2f;

    private float _yaw;
    private float _pitch;

    public override void F_Start()
    {
        // 启动试玩自动锁鼠标并隐藏光标；按 Esc 可随时释放，点回视口自动重锁
        F_Cursor.Locked = true;
        _yaw = Entity.EulerAngles.Y;
        _pitch = CameraPivot is { IsValid: true } ? CameraPivot.EulerAngles.X : 0f;
    }

    public override void F_Update(float delta)
    {
        // 按 Esc 释放光标后，F_Cursor.Locked 自动变为 false，立刻停止旋转视角
        if (!F_Cursor.Locked) return;

        // 注意：MouseDelta 已经是像素位移，不要再乘 delta！
        Vector2 look = F_Input.MouseDelta;
        _yaw -= look.X * Sensitivity;
        _pitch = F_Math.Clamp(_pitch - look.Y * Sensitivity, -80f, 80f);

        Entity.EulerAngles = new Vector3(0f, _yaw, 0f);
        if (CameraPivot is { IsValid: true })
            CameraPivot.EulerAngles = new Vector3(_pitch, 0f, 0f);
    }

    public override void F_Destroy()
    {
        // 离开场景或停止运行时主动释放光标
        F_Cursor.Locked = false;
    }
}
```

---

## `F_Camera` 与 `F_Ray`（相机屏幕射线） {#f-camera}

`F_Camera` 负责把二维屏幕像素点反投影成世界空间的三维射线 `F_Ray`，常与 [`F_Physics.Raycast`](physics.md) 配合实现鼠标点击拾取、建造放置与准星瞄准。

源码位置：[FlareErgonomics.cs#L177-L194](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L177) · [EngineSession.cs#L269-L275](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L269) · [NativeWorldExports.inl#L199-L219](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativeWorldExports.inl#L199)

### `F_Ray` 结构体 {#f-ray}

`public readonly record struct F_Ray(Vector3 Origin, Vector3 Direction)`

| 成员 | 类型 | 说明 |
| :--- | :--- | :--- |
| `Origin` | `Vector3` | 射线起点（世界空间米，位于摄像机近裁剪平面对应像素的反投影位置） |
| `Direction` | `Vector3` | 射线方向（已归一化的世界空间单位向量） |
| `GetPoint(float distance)` | `Vector3` | 返回沿射线前进 `distance` 米处的世界坐标：`Origin + Direction * distance` |

### `F_Camera.ScreenPointToRay`

```csharp
public static F_Ray? ScreenPointToRay(Vector2 screenPixel, F_Entity? camera = null)
```

- **实体快捷方法**：如果已持有指定摄像机实体 `camEntity`，也可以直接调用 `camEntity.ScreenPointToRay(screenPixel)`（定义于 [FlareErgonomics.cs#L513](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L513)）。

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :---: | :--- |
| `screenPixel` | `Vector2` | 无 | 游戏视口内像素坐标（左上角原点，直接传 `F_Input.MousePosition`） |
| `camera` | `F_Entity?` | `null` | 指定投影源摄像机实体；为 `null` 时自动使用当前场景的主摄像机 |

- **返回值**：`F_Ray?`（可空射线）。以下情况会返回 `null`，调用时请务必判空：
  1. 当前不在活跃脚本会话内（`ScriptSystem.Current` 为空且未传入有效 `camera`）；
  2. 场景中没有主摄像机，或传入的 `camera` 实体未挂载摄像机组件 / 已被销毁；
  3. `screenPixel` 含有 `NaN` / `Infinity`，或摄像机矩阵/视口参数异常导致无法反投影。

### 示例：鼠标点选 3D 物体并对命中刚体施加冲量

```csharp
using System.Numerics;
using FlareEngine;

public class ClickPicker : F_Behaviour
{
    [F_Expose] public float MaxPickDistance = 120f;
    [F_Expose] public float KickImpulse = 8f;

    public override void F_Update(float delta)
    {
        // 左键点选 3D 物理物体
        if (F_Input.MouseDown(F_MouseButton.Left))
        {
            if (F_Camera.ScreenPointToRay(F_Input.MousePosition) is not { } ray)
                return;

            if (F_Physics.Raycast(ray.Origin, ray.Direction, MaxPickDistance, out F_RaycastHit hit))
            {
                F_Log.Info($"点中物体: {hit.Entity.Name} | 距离: {hit.Distance:F2}m | 交点: {hit.Point}");

                // 若命中物体带动态刚体，沿射线方向踢一脚
                if (hit.Entity.TryGetComponent<F_Rigidbody>(out var body))
                    body.AddImpulse(ray.Direction * KickImpulse);
            }
            else
            {
                // 未命中任何碰撞体时，可取射线前方 10 米处作为落点参考
                Vector3 fallbackPoint = ray.GetPoint(10f);
                F_Log.Info($"未命中物体，前方 10m 坐标: {fallbackPoint}");
            }
        }
    }
}
```

---

## `F_Time`（帧时钟与时间缩放） {#f-time}

`F_Time` 提供全局帧间隔、固定物理步长、累计时间与时间倍率控制。每次进入运行态时由 `ScriptSystem.Start()` 自动重置（`TimeScale = 1f`，累计时间与帧数归零），每帧 `FrameBegin` 自动推进（编辑器暂停时冻结）。

源码位置：[FlareErgonomics.cs#L41-L83](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L41) · [ScriptSystem.cs#L154-L253](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L154)

### 静态属性一览

| 属性名 | 类型 | 读写权限 | 边界与默认值 | 典型使用场景 |
| :--- | :--- | :---: | :--- | :--- |
| `F_Time.DeltaTime` | `float` | 只读 | `UnscaledDeltaTime * TimeScale`（秒） | 常规角色移动、技能冷却、受暂停和慢放影响的玩法逻辑 |
| `F_Time.UnscaledDeltaTime` | `float` | 只读 | 真实帧间隔秒数（非有限值或负值自动归零） | **暂停菜单动画**、子弹时间期间不受减速影响的相机/UI 动效 |
| `F_Time.FixedDeltaTime` | `float` | 只读 | 默认 `1f / 60f`（约 `0.01667s`），与固定步时钟同步 | 在 `F_FixedUpdate` 中做刚体积分或胶囊角色移动 |
| `F_Time.Time` | `float` | 只读 | 从 `0f` 起累计缩放秒数 | 受暂停与慢放影响的游戏内计时（如关卡倒计时、攻击间隔） |
| `F_Time.UnscaledTime` | `float` | 只读 | 从 `0f` 起累计真实秒数 | 不受 `TimeScale` 影响的真实运行时长、UI 周期闪烁 |
| `F_Time.FrameCount` | `ulong` | 只读 | 从 `0` 起每渲染帧递增 `1` | 节流采样（如每 10 帧刷新一次导航）、诊断日志打标 |
| `F_Time.TimeScale` | `float` | **读 / 写** | 默认 `1f`；允许 `[0f, 100f]`；超过 `100f` 夹取为 `100f`；**负数、`NaN`、`Infinity` 自动重置回 `1f`** | `0f` 暂停玩法逻辑、`0.25f` 子弹时间慢放、`2f` 加速回放 |

::: warning 暂停游戏与编辑器暂停的区别
- 将 `F_Time.TimeScale = 0f` 时，渲染循环与 `F_Update` 仍会照常执行，只是 `F_Time.DeltaTime` 变为 `0f`（`F_Await.Seconds` 也会同步暂停计时），因此你的暂停菜单脚本依然可以读取 `F_Input` 并用 `F_Time.UnscaledDeltaTime` 播放界面淡入动画。
- 编辑器工具栏点击「暂停」会冻结整个模拟步进，此时只有重写了 `public override bool UpdateWhenPaused => true;` 的脚本还会继续运行。
:::

### 示例：按键切换暂停 / 子弹时间，并用 `UnscaledDeltaTime` 驱动不受减速影响的过渡

```csharp
using FlareEngine;

public class TimeControlDemo : F_Behaviour
{
    private bool _paused;
    private float _menuAlpha;

    public override void F_Update(float delta)
    {
        // 按 P 暂停/恢复游戏逻辑时间
        if (F_Input.KeyDown(F_Key.P))
        {
            _paused = !_paused;
            F_Time.TimeScale = _paused ? 0f : 1f;
            F_Log.Info(_paused ? "游戏已暂停 (TimeScale = 0)" : "游戏已恢复 (TimeScale = 1)");
        }

        // 按住 Tab 进入 0.25 倍速子弹时间（未暂停时生效）
        if (!_paused)
            F_Time.TimeScale = F_Input.Key(F_Key.Tab) ? 0.25f : 1f;

        // 即使 TimeScale == 0，菜单淡入动画依然使用 UnscaledDeltaTime 顺滑过渡
        float targetAlpha = _paused ? 1f : 0f;
        _menuAlpha = F_Math.MoveTowards(_menuAlpha, targetAlpha, F_Time.UnscaledDeltaTime * 4f);
    }

    public override void F_Destroy()
    {
        F_Time.TimeScale = 1f;
    }
}
```

---

## `F_Math` 与 `F_Quat`（数学与四元数） {#f-math}

源码位置：[FlareErgonomics.cs#L197-L359](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L197)

### `F_Math` 常量与常用方法

#### 常量

| 常量名 | 类型 | 数值 | 说明 |
| :--- | :--- | :--- | :--- |
| `F_Math.PI` | `float` | `3.1415927f` | 圆周率 `MathF.PI` |
| `F_Math.Deg2Rad` | `float` | `PI / 180f` | 角度转弧度乘子（如 `60f * F_Math.Deg2Rad` 传给 `Entity.SetCameraFieldOfView`） |
| `F_Math.Rad2Deg` | `float` | `180f / PI` | 弧度转角度乘子 |

#### 方法一览

| 方法签名 | 参数说明 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `Clamp(float value, float min, float max)`<br>`Clamp(int value, int min, int max)` | `value`：待限制值；`min`/`max`：闭区间边界 | `float` / `int` | 将数值限制在 `[min, max]` 区间内 |
| `Clamp01(float value)` | `value`：待限制值 | `float` | 将浮点数限制在 `[0, 1]` 区间内 |
| `Lerp(float a, float b, float t)`<br>`Lerp(Vector3 a, Vector3 b, float t)` | `a`：起点；`b`：终点；`t`：权重（自动夹取到 `[0,1]`） | `float` / `Vector3` | 有界线性插值 |
| `LerpUnclamped(float a, float b, float t)` | `a`：起点；`b`：终点；`t`：不夹取的权重 | `float` | 无界线性插值（`t` 超出 `[0,1]` 时会向外延拓） |
| `InverseLerp(float a, float b, float value)` | `a`、`b`：区间端点；`value`：输入值 | `float` | 计算 `value` 在 `[a, b]` 中的归一化比例 `0..1`；`a` 与 `b` 几乎相等时返回 `0f` |
| `MoveTowards(float current, float target, float maxDelta)`<br>`MoveTowards(Vector3 current, Vector3 target, float maxDistanceDelta)` | `current`：当前值；`target`：目标值；`maxDelta`：单步最大变化量 | `float` / `Vector3` | 以不超过给定步幅匀速推向目标，到达后精准停在 `target` 不越界 |
| `Repeat(float t, float length)` | `t`：输入值；`length`：周期长度（`<= 1e-6f` 时返回 `0f`） | `float` | 将 `t` 循环折叠到 `[0, length]` 区间内（类似对浮点数做非负取模） |
| `PingPong(float t, float length)` | `t`：输入值（常传 `F_Time.Time`）；`length`：折返上限 | `float` | 让数值在 `0` 与 `length` 之间做匀速往返运动（适合平台巡逻、呼吸灯） |
| `DeltaAngle(float current, float target)` | `current`、`target`：两个角度值（度） | `float` | 计算从 `current` 到 `target` 的最短有符号角差，范围 `(-180, 180]` |
| `LerpAngle(float a, float b, float t)` | `a`、`b`：角度（度）；`t`：夹取到 `[0,1]` 的权重 | `float` | 沿最短圆弧在两角度间插值，自动正确跨越 `360° / 0°` 边界 |
| `Approximately(float a, float b, float epsilon = 1e-5f)` | `a`、`b`：浮点数；`epsilon`：容差（默认 `1e-5f`） | `bool` | 当 `MathF.Abs(a - b) <= epsilon` 时返回 `true` |
| `SmoothDamp(float current, float target, ref float currentVelocity, float smoothTime, float maxSpeed = float.PositiveInfinity, float? deltaTime = null)` | 见下方详表 | `float` | 临界阻尼弹簧平滑过渡标量（帧率无关、不振荡、不超调） |
| `SmoothDamp(Vector3 current, Vector3 target, ref Vector3 currentVelocity, float smoothTime, float maxSpeed = float.PositiveInfinity, float? deltaTime = null)` | 见下方详表 | `Vector3` | 临界阻尼弹簧平滑过渡三维坐标（相机跟随首选） |

#### `SmoothDamp` 详细参数表

| 参数 | 类型 | 默认值 | 说明 |
| :--- | :--- | :---: | :--- |
| `current` | `float` / `Vector3` | 无 | 当前帧位置或数值 |
| `target` | `float` / `Vector3` | 无 | 期望趋近的目标位置或数值 |
| `currentVelocity` | `ref float` / `ref Vector3` | 无 | **当前速度状态变量**：必须声明为脚本成员变量（不能是每帧重新初始化的局部变量），引擎会在每帧内部读写它以保持惯性连续 |
| `smoothTime` | `float` | 无 | 大致到达目标所需时间（秒，越小越跟手，最小自动夹取到 `0.0001f`） |
| `maxSpeed` | `float` | `float.PositiveInfinity` | 允许的最大移动速率上限（米/秒或单位/秒） |
| `deltaTime` | `float?` | `null` | 步长时间（秒）；传 `null` 时自动取 `F_Time.DeltaTime`，若其 `<= 0f` 则回退 `1f / 60f`。若暂停菜单期间也要平滑，请显式传入 `F_Time.UnscaledDeltaTime` |

#### 示例：第三人称相机平滑跟随（`F_Math.SmoothDamp`）

```csharp
using System.Numerics;
using FlareEngine;

public class SmoothFollowCamera : F_Behaviour
{
    [F_Expose] public F_Entity? Target;
    [F_Expose] public Vector3 Offset = new(0f, 3.2f, 6.5f);
    [F_Expose, F_Range(0.05f, 1.5f)] public float SmoothTime = 0.22f;
    [F_Expose] public float MaxFollowSpeed = 30f;

    // 必须作为字段跨帧保存，供 SmoothDamp 持续更新速度状态
    private Vector3 _velocity;

    // 相机跟随推荐放在 F_LateUpdate，确保目标在本帧 F_Update 已完成移动
    public override void F_LateUpdate(float delta)
    {
        if (Target is not { IsValid: true }) return;

        Vector3 desiredPos = Target.WorldPosition + Offset;
        Entity.WorldPosition = F_Math.SmoothDamp(
            Entity.WorldPosition,
            desiredPos,
            ref _velocity,
            SmoothTime,
            MaxFollowSpeed,
            delta);

        LookAt(Target.WorldPosition + new Vector3(0f, 1.2f, 0f));
    }
}
```

### `F_Quat`（四元数与欧拉角工具） {#f-quat}

::: info 坐标系与旋转顺序约定
- Flare Engine 采用**右手坐标系**：`+X` 向右、`+Y` 向上、**`-Z` 为物体局部正前方**（`Entity.Forward`）。
- 所有涉及欧拉角与夹角的接口（`Euler`、`ToEuler`、`Angle`、`RotateTowards`）均采用**角度制（度）**。
- `F_Quat.Euler(x, y, z)` 的旋转合成顺序固定为 **`Z → X → Y`**（对应四元数乘积 `qy * qx * qz`，先滚转 Z、再俯仰 X、最后偏航 Y），与编辑器属性面板完全一致。
:::

#### 常量与方法一览

| 成员签名 | 参数说明 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `F_Quat.Identity` | 只读静态属性 | `Quaternion` | 无旋转的单位四元数 `(0, 0, 0, 1)` |
| `F_Quat.Euler(float xDegrees, float yDegrees, float zDegrees)`<br>`F_Quat.Euler(Vector3 eulerDegrees)` | 绕 X（俯仰）、Y（偏航）、Z（滚转）的欧拉角（角度制） | `Quaternion` | 按 `Z → X → Y` 顺序构造单位四元数 |
| `F_Quat.ToEuler(Quaternion rotation)` | `rotation`：四元数（内部自动归一化） | `Vector3` | 还原为 `(X, Y, Z)` 角度制欧拉角，自动处理万向锁奇异情况 |
| `F_Quat.LookRotation(Vector3 forward, Vector3? worldUp = null)` | `forward`：目标世界朝向；`worldUp`：上方向（缺省 `Vector3.UnitY`） | `Quaternion` | 构造让局部 **`-Z`** 对准 `forward` 的单位四元数；`forward` 近零返回 `Identity`，与 `worldUp` 共线时自动切换安全备用上轴 |
| `F_Quat.Slerp(Quaternion a, Quaternion b, float t)` | `a`、`b`：起止四元数；`t`：权重（自动夹取到 `[0,1]`） | `Quaternion` | 球面线性插值（等角速度旋转过渡） |
| `F_Quat.Angle(Quaternion a, Quaternion b)` | `a`、`b`：两个四元数 | `float` | 返回两个旋转之间的最小夹角（角度制，范围 `0..180`） |
| `F_Quat.RotateTowards(Quaternion from, Quaternion to, float maxDegreesDelta)` | `from`、`to`：起止四元数；`maxDegreesDelta`：本步最大旋转角（度） | `Quaternion` | 以恒定角速度朝目标旋转推进，到达后精确对齐 `to` |

#### 示例：炮塔恒速瞄准目标（`LookRotation` + `RotateTowards` + `Angle`）

```csharp
using System.Numerics;
using FlareEngine;

public class AutoTurret : F_Behaviour
{
    [F_Expose] public F_Entity? Enemy;
    [F_Expose, F_Range(10f, 360f)] public float TurnSpeedDegPerSec = 90f;

    public override void F_Update(float delta)
    {
        if (Enemy is not { IsValid: true }) return;

        Vector3 toEnemy = Enemy.WorldPosition - Entity.WorldPosition;
        toEnemy.Y = 0f; // 仅在水平面旋转炮台
        if (toEnemy.LengthSquared() < 1e-4f) return;

        Quaternion targetRot = F_Quat.LookRotation(toEnemy, Vector3.UnitY);
        Entity.WorldRotation = F_Quat.RotateTowards(
            Entity.WorldRotation,
            targetRot,
            TurnSpeedDegPerSec * delta);

        // 夹角小于 5 度时视为锁定完成
        if (F_Quat.Angle(Entity.WorldRotation, targetRot) < 5f)
        {
            // 可在此触发开火逻辑
        }
    }
}
```

---

## `F_Log` 与 `F_Await`（控制台日志与主线程确定性异步） {#f-log}

### `F_Log`（编辑器控制台日志）

`F_Log` 将脚本运行日志输出到标准流并推入编辑器左下角 **控制台** 面板。调用时只需传入消息内容，C# 编译器会通过 `[CallerFilePath]` 与 `[CallerLineNumber]` 自动注入调用处的源文件名与行号，格式化为 `[Info] MyScript.cs(42,1): 消息内容`——**在编辑器控制台双击该日志行，外部 IDE 会直接跳转定位到对应代码行**。

源码位置：[FlareErgonomics.cs#L709-L764](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L709)

| 成员签名 | 参数说明 | 返回值 | 说明 |
| :--- | :--- | :--- | :--- |
| `F_Log.Info(object? message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)` | `message`：任意可格式化对象（`file`/`line` 由编译器自动填充，勿手动传） | `void` | 输出 `[Info]` 普通信息日志到控制台 |
| `F_Log.Warn(object? message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)` | 同上 | `void` | 输出 `[Warning]` 黄色警告日志 |
| `F_Log.Error(object? message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)` | 同上 | `void` | 输出 `[Error]` 红色错误日志（并同步写入 `stderr`） |
| `F_Log.Logged` | `event Action<string>?` | 事件 | 每次产生新日志行时同步触发，可用于自定义游戏内调试控制台 |
| `F_Log.Drain()` | 无 | `IReadOnlyList<string>` | 取出并清空当前队列中缓存的日志（内部最多缓存 512 条，主要供编辑器宿主消费） |

```csharp
F_Log.Info($"玩家出生坐标: {Entity.WorldPosition}");
F_Log.Warn("弹药不足，仅剩 3 发");
F_Log.Error("缺少目标 SpawnPoint 引用！");
```

### `F_Await`（主线程确定性异步等待） {#f-await}

`F_Await` 是 FlareEngine 专为游戏脚本设计的轻量级协程/异步等待工具。它在脚本的 `async void`（或 `async Task`）方法中与 C# 原生 `await` 搭配使用，实现延时技能、分波刷怪、过场序列等逻辑，**彻底替代容易造成跨线程崩溃的 `Task.Delay`**。

源码位置：[FlareErgonomics.cs#L767-L792](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/FlareErgonomics.cs#L767) · [ScriptSystem.cs#L635-L710](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/ScriptSystem.cs#L635)

#### 方法一览

| 方法签名 | 参数说明 | 返回值 | 唤醒时机与行为 |
| :--- | :--- | :--- | :--- |
| `F_Await.Seconds(float seconds)` | `seconds`：等待的缩放秒数（受 `F_Time.TimeScale` 影响） | `F_Await.StepAwaitable` | 在每帧主循环 `FrameBegin` 扣除 `F_Time.DeltaTime`，归零后立即在主线程恢复执行；若传入 `<= 0f` 则当场同步继续 |
| `F_Await.NextFrame()` | 无 | `F_Await.StepAwaitable` | 挂起到**下一渲染帧**的 `FrameBegin` 再恢复执行（不会在同一帧被立刻唤醒） |
| `F_Await.FixedUpdate()` | 无 | `F_Await.StepAwaitable` | 挂起到**下一次固定物理步**（`FixedPreTick`）再恢复执行，适合等待物理状态生效后再读速度/位置 |

#### 主线程确定性调度与自动取消机制

```text
  脚本内调用 await F_Await.Seconds(2f)
              │
              ▼
  记录当前所属脚本实例 Owner，加入 ScriptSystem.pendingAwaits（不开启后台线程）
              │
              ├── 若所属实体被销毁 / 脚本被移除 ──▶ Remove() 立即清除续体（静默终止，不崩）
              ├── 若退出运行预览（Stop Play）    ──▶ Stop() 清空全部等待（静默终止，不崩）
              ├── 若编辑器暂停或 TimeScale == 0  ──▶ 自动暂停倒计时
              │
              ▼  （计时到达，实体仍存活）
  在主线程同步上下文（ScriptSynchronizationContext）恢复执行后续代码
```

| 对比项 | `F_Await.Seconds(...)`（推荐） | `Task.Delay(...)`（不推荐） |
| :--- | :--- | :--- |
| **执行线程** | 严格在引擎**主线程**恢复，安全操作 `Entity` / `F_Physics` / `F_Scene` | 在 .NET 线程池恢复，直接访问引擎对象会抛 `Bridge.WrongThread` |
| **暂停与时间缩放** | 自动遵循 `F_Time.TimeScale` 与编辑器暂停 | 按真实墙钟计时，暂停游戏期间仍在倒数 |
| **销毁与停止预览** | 所属实体销毁或退出预览时**自动取消**，后续代码不会再跑 | 停止预览后仍可能继续执行，导致空引用或泄漏 |
| **异常安全** | 续体抛异常会被捕获并记录到控制台，不会闪退编辑器 | `async void` 未捕获异常可能导致进程崩溃 |

::: warning 编写 `async void` 异步脚本的 2 个好习惯
1. **防止重复触发**：如果技能按钮或交互可能连续按下，用布尔锁字段（如 `private bool _casting;`）在进入 `async void` 时加锁、在 `finally` 块里解锁。
2. **检查外部实体引用**：`F_Await` 会保证**当前脚本所属实体**在唤醒时仍然存活，但不会替你检查其他外部实体——在 `await` 之后使用外部 `F_Entity?`（例如 `SpawnTemplate` 或克隆体 `clone`）前，请检查 `if (other is { IsValid: true })`。
:::

#### 示例：技能定时释放 + 分波刷怪

```csharp
using System.Numerics;
using FlareEngine;

public class SkillAndWaveSpawner : F_Behaviour
{
    [F_Header("刷怪配置")]
    [F_Expose] public F_Entity? EnemyPrefab;
    [F_Expose, F_Range(1, 10)] public int EnemiesPerWave = 3;
    [F_Expose, F_Range(0.2f, 5f)] public float SpawnInterval = 0.8f;

    [F_Header("技能冷却")]
    [F_Expose, F_Range(0.5f, 10f)] public float SkillCooldown = 3f;

    private bool _skillReady = true;

    public override void F_Start()
    {
        // 启动分波刷怪协程（退出运行预览或销毁本物体时自动安全停止）
        RunWaveLoopAsync();
    }

    public override void F_Update(float delta)
    {
        // 按 F 键释放定时技能
        if (F_Input.KeyDown(F_Key.F) && _skillReady)
            CastSkillAsync();
    }

    private async void CastSkillAsync()
    {
        _skillReady = false;
        try
        {
            F_Log.Info("开始蓄力（0.5 秒）...");
            await F_Await.Seconds(0.5f);

            F_Log.Info("释放冲击波！");
            // 等待下一次物理步，确保物理世界状态已同步
            await F_Await.FixedUpdate();

            if (TryGetComponent<F_Rigidbody>(out var body))
                body.AddImpulse(Vector3.UnitY * 6f);

            F_Log.Info($"技能进入冷却（{SkillCooldown:F1} 秒）...");
            await F_Await.Seconds(SkillCooldown);
            F_Log.Info("技能冷却完毕！");
        }
        finally
        {
            _skillReady = true;
        }
    }

    private async void RunWaveLoopAsync()
    {
        // 等待 1 帧，确保场景内所有实体已完成 F_Start
        await F_Await.NextFrame();

        for (int wave = 1; wave <= 3; wave++)
        {
            F_Log.Info($"第 {wave} 波敌人来袭！");
            for (int i = 0; i < EnemiesPerWave; i++)
            {
                if (EnemyPrefab is { IsValid: true })
                {
                    var offset = new Vector3((i - 1) * 2.5f, 0f, -4f);
                    Instantiate(EnemyPrefab, Entity.WorldPosition + offset, F_Quat.Identity);
                }
                await F_Await.Seconds(SpawnInterval);
            }

            // 每波之间间隔 4 秒（受 TimeScale 影响，暂停游戏时会自动冻结计时）
            await F_Await.Seconds(4f);
        }

        F_Log.Info("全部波次刷新完成！");
    }
}
```
