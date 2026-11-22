# 输入、手柄、动作映射与窗口适配

平台层统一采集键鼠、中文 IME 与最多 4 路 XInput 手柄状态到纯值结构 `InputState`；功能层 `EngineInput` 依据项目 JSON 配置把多设备原始输入映射为命名动作与轴向，并结合 UI 消费标志屏蔽误触，最后通过 C# `F_InputActions` / `InputActions` 提供给玩法脚本。

## 输入采集与动作求值管线

```text
Win32 窗口消息 (WM_KEY* / WM_MOUSE* / WM_CHAR / WM_IME_*)
   │
   ├─► UI 先行消费：EditorInputAdapter（编辑器） / WindowsUiInput（游戏 UI）
   │      ├─ EditorInputAdapter 读取 lParam bit 30 (KF_REPEAT) 标记 isRepeat
   │      └─ 产出 keyboardConsumed / pointerConsumed 屏蔽标志
   │
   └─► ProcessWindowsInput()（始终更新原始状态，keysDown 状态翻转去重）
          │
          ├──► InputState { keysDown[256], mouseButtons[3], mouseDelta,
          │                 keyEvents, pointerEvents, textEvents, gamepads[4] }
          ▲
XInput DLL (xinput1_4 / xinput9_1_0) ──► PollWindowsGamepads()（断连 500ms 退避）
          │
          ▼  InputActionMap::Update(inputState, keyboardConsumed, pointerConsumed)
     InputActionMap（EngineInput：标量/径向死区 + 边沿判定 + 2D 单位圆归一化）
          │
          ▼  Native C ABI (flare_input_*)
     C# F_InputActions / InputActions（Session.Actions / F_Behaviour.Actions）
```

## InputState

[`InputState`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/InputState.h#L9) 是不依赖 `<Windows.h>` 的纯数据结构，把输入拆成两类不同生命周期的数据：

- **持续状态**：`keysDown[256]`、`mouseButtons[3]`、`mouseX/Y`、`hasFocus` 与 4 路手柄快照 `gamepads[4]`，跨帧保留，失焦（`WM_KILLFOCUS`）时统一清零并发送 `KeyEventType::Reset` / `PointerEventType::Cancel`。
- **单帧脉冲队列**：`keyEvents`、`textEvents`、`pointerEvents` 与累积位移 `mouseDeltaX/Y`；每帧所有消费者读取完后，由主循环统一调用 [`ClearEvents()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/InputState.h#L27) 清空。

### 重复按键过滤与两种分工

长按键盘按键时，Windows 会持续投递重复的 `WM_KEYDOWN` 消息。Flare 在两条路径上按需求分别处理：

1. **游戏与原始输入（状态翻转去重）**：[`ProcessWindowsInput()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/WindowsInput.cpp#L11) 直接比对当前按键状态 `if (input.keysDown[wParam] == down) break;`，因此系统长按连发不会重复写入 `keyEvents`，确保跳跃/开火等 `Pressed` 边沿事件每按一次只触发一帧。
2. **编辑器 UI 输入（`KF_REPEAT` 标志透传）**：[`EditorInputAdapter::Translate()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInputAdapter.cpp#L166) 检查 `lParam` 第 30 位（对应 Win32 `KF_REPEAT` 标志）设置 `UiInputEvent::isRepeat`，从而允许 `UiTextEdit` 响应长按退格和方向键移动，同时让视口工具快捷键（[`EditorViewportController`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorViewportController.cpp#L244)）显式过滤掉重复触发。

### PollWindowsGamepads

[`PollWindowsGamepads()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/WindowsGamepad.cpp#L74) 负责轮询 0..3 号手柄，并解决了 Windows 下 XInput 的两个经典工程问题：

```cpp
// Source/PlatformLayer/Windows/WindowsGamepad.cpp
// 1. 运行时动态探测 DLL，不产生静态链接依赖；缺失时安全置零返回。
HMODULE module = LoadLibraryW(L"xinput1_4.dll");
if (!module) module = LoadLibraryW(L"xinput9_1_0.dll");

// 2. 未连接的槽位采用 500 ms 退避重试，避免每帧查询空槽位引发系统调用卡顿。
if (!outStates[i].connected && nowTick < nextDisconnectedRetryTick[i]) continue;
if (getState(i, &raw) == ERROR_SUCCESS) {
    nextDisconnectedRetryTick[i] = 0;
    outStates[i] = DecodeXInputGamepad(/* ... */);
} else {
    nextDisconnectedRetryTick[i] = nowTick + 500;
    outStates[i] = GamepadState{};
}
```

解码后的 [`GamepadState`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/GamepadState.h#L26) 将扳机归一化到 `[0, 1]`、摇杆轴量经 [`NormalizeXInputThumbAxis()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/WindowsGamepad.cpp#L49) 归一化到 `[-1, 1]`，按键掩码采用无平台头依赖的 `GamepadButton::*` 常量。

## InputActionMap

[`InputActionMap`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Input/InputActionMap.h#L12) 位于独立静态库 `EngineInput`（仅依赖 `EngineCore` 与值头 `InputState.h`），根据项目输入配置 [`InputActionDocument`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Input/InputActionAsset.h#L84)（默认路径 `Settings/Input.input.json`）把键盘、鼠标与手柄绑定映射为命名 `Button`、`Axis1D` 和 `Axis2D`。

核心行为细节（[`InputActionMap.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Input/InputActionMap.cpp#L10)）：

- **死区整形**：单轴与扳机走标量死区 `ApplyScalarDeadZone()`，摇杆走径向死区 `ApplyRadialDeadZone()`，平滑重映射到 `[0, 1]` 而不在边界处跳变。
- **UI 消费屏蔽**：`IsBindingBlocked()` 按设备粒度屏蔽——UI 占用键盘时屏蔽键盘绑定、占用指针时屏蔽鼠标绑定，仅当两者都被占用时屏蔽手柄绑定，避免点击 HUD 按钮时误开枪，同时不误伤手柄操作。
- **双模式求值**：既支持按帧 `Update()` 缓存快照的有状态查询，也提供传入 `InputState` 与 `previousGamepads` 的纯函数重载，方便无副作用单测。

### Update

`void Update(const InputState& current, bool keyboardConsumed = false, bool pointerConsumed = false)` 将上一帧手柄快照移入 `previousGamepads_` 再保存当前帧，使手柄按钮和扳机/摇杆阈值也能精确检测“首帧按下”边沿；退出 Play 模式或重置会话时调用 [`Reset()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Input/InputActionMap.cpp#L155) 清除残留状态，保留已加载动作表。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Input/InputActionMap.cpp#L164)

### IsActionPressed

[`IsActionPressed(std::string_view name)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Input/InputActionMap.cpp#L180) 仅在动作由松开转为按下的首帧返回 `true`（键盘检查 `keyEvents` 中的 `Pressed`、鼠标检查 `PointerEventType::Down`、手柄对比当前与上一帧状态）；持续按住使用 `IsActionDown()`，一维与二维轴向使用 `GetAxis1D()` 与 `GetAxis2D()`。其中 [`GetAxis2D()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Input/InputActionMap.cpp#L361) 会对超过最大幅值的对角线合成输入（如同时按 `W + D`）自动做单位圆归一化，避免斜向移动超速。

## InputActions

C# 脚本通过 `Session.Actions`（在 `F_Behaviour` 中可直接写 `Actions`，类型为 [`F_InputActions`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/InputActions.cs#L7)，兼容子类 [`InputActions`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/InputActions.cs#L70)）查询命名动作与轴向，无需在玩法代码里硬编码键码或手柄槽位。

```csharp
using System.Numerics;
using FlareEngine;

public sealed class PlayerInputDemo : F_Behaviour
{
    public override void OnUpdate(float deltaSeconds)
    {
        Vector2 move = Actions.GetAxis2D("Move");      // WASD + 左摇杆统一归一化向量
        float zoom   = Actions.GetAxis1D("Zoom");      // 滚轮或扳机 1D 轴

        if (Actions.IsActionPressed("Jump"))           // 仅按下首帧触发
        {
            // 执行跳跃
        }
    }
}
```

### LoadJson

`void LoadJson(string json)` 在运行时热替换当前会话的动作映射表；`IsActionDown`、`IsActionPressed`、`GetAxis1D` 与 `GetAxis2D` 均经原生 C ABI 直查底层 `InputActionMap`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/InputActions.cs#L14)

## 游戏控件、窗口适配与数学基础

- **游戏 UI 输入桥接**：[`WindowsUiInput`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/WindowsUiInput.h#L23) 负责将 Win32 键鼠、剪贴板与 IMM32 组合消息先送入 `UiTree`，并向宿主汇报 `KeyboardConsumed()` / `PointerConsumed()`；在编辑器内嵌 Play 视口时通过 `SetViewport()` 自动扣除客户区偏移。
- **计时与观察器**：[`UpdateFrameTimer()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/FrameTimer.cpp#L21) 与只读日志观察器 [`SandboxDiagnostics`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/SandboxDiagnostics.h#L8) 负责采集帧间隔与输入诊断（观察器绝不修改或清理 `InputState`）。
- **共用值类型**：变换与界面几何共用 [`Quat`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Math/Math.h#L34)、[`Mat4`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Math/Math.h#L68) 与 [`GuiGeometry`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/GuiGeometry.h#L18)。

## 常用输入符号速查

| 符号 | 位置 | 作用 |
| --- | --- | --- |
| `InputState` / `ClearEvents()` | [`InputState.h#L9`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/InputState.h#L9) | 跨层键鼠/手柄状态与单帧事件队列 |
| `ProcessWindowsInput()` | [`WindowsInput.cpp#L5`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/WindowsInput.cpp#L5) | Win32 消息转 `InputState` 与按键状态去重 |
| `PollWindowsGamepads()` | [`WindowsGamepad.h#L23`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/WindowsGamepad.h#L23) | 动态 XInput 4 路手柄采样（带 500 ms 断连退避） |
| `InputActionMap` | [`InputActionMap.h#L12`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Input/InputActionMap.h#L12) | 动作/轴求值、死区整形、边沿检测与 UI 消费屏蔽 |
| `F_InputActions` / `InputActions` | [`InputActions.cs#L7`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/InputActions.cs#L7) | C# 玩法脚本动作表加载与查询入口 |

::: tip 失焦时为什么不会出现“卡键一直往前跑”
`ProcessWindowsInput()` 在收到 `WM_KILLFOCUS` 时（[`WindowsInput.cpp#L53`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/WindowsInput.cpp#L53)）立即清空 `keysDown`、`mouseButtons`、`mouseDelta`，释放鼠标捕获并推入 `KeyEventType::Reset` 与 `PointerEventType::Cancel`；`InputActionMap` 检测到当帧存在 `Reset / Cancel` 时也会立刻把对应键鼠动作评估为未触发。
:::
