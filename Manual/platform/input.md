# 输入、手柄、动作映射与窗口适配

平台层统一采集键鼠、IME 组合文本与最多 4 路 XInput 手柄状态；功能层 `EngineInput` 将原始输入映射为命名动作与轴，再通过 C# `InputActions` 暴露给玩法脚本。

## InputState

`InputState` 同时保存持续按键、指针位置、焦点、4 路手柄快照 `gamepads` 与本帧事件队列。平台在收到消息时写入对应记录；所有消费者读取完毕后由主循环调用 [ClearEvents()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/InputState.h#L24) 清空单帧脉冲事件，持续按住状态保留至下一帧。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/InputState.h#L9)

### PollWindowsGamepads

`void PollWindowsGamepads(std::array<GamepadState, 4>& outStates)` 动态探测 `xinput1_4.dll` / `xinput9_1_0.dll`（无静态硬链接依赖），轮询 0..3 号手柄按键位、归一化扳机与经径向死区整形后的左右摇杆向量；缺失 DLL 时自动安全置零。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/Windows/WindowsGamepad.h#L23)

## InputActionMap

`InputActionMap` 位于独立静态库 `EngineInput`（仅依赖 `EngineCore` 与值类型头 `InputState.h`），按项目输入配置文档 `InputActionDocument`（JSON）将按键、鼠标按钮、手柄按键与摇杆组合映射为命名布尔动作、1D 轴与 2D 向量，并支持编辑器/UI 消费标志屏蔽（`keyboardConsumed`、`pointerConsumed`）。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Input/InputActionMap.h#L12)

### Update

`void Update(const InputState& current, bool keyboardConsumed = false, bool pointerConsumed = false)` 在每帧推进前沿触发状态（区分“本帧刚按下”与“持续按住”）并缓存上一帧手柄按键位；切出 Play 模式或重置会话时调用 `Reset()` 清除残留边沿状态。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Input/InputActionMap.h#L25)

### IsActionPressed

`bool IsActionPressed(std::string_view name)` 仅在动作由松开转为按下的首帧返回 `true`；持续按住查询使用 `IsActionDown()`，一维与二维轴查询使用 `GetAxis1D()` 与 `GetAxis2D()`（自动归一化超过单位圆的斜向键鼠合成输入）。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Input/InputActionMap.h#L27)

## InputActions

C# 脚本通过 `Session.Actions`（`F_Behaviour` 内亦可直接访问 `Actions`，类型为 `FlareEngine.InputActions`）加载动作表并查询动作/轴，不直接硬编码平台扫描码。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Scripting/Managed/InputActions.cs#L6)

```csharp
Vector2 move = Actions.GetAxis2D("Move");
if (Actions.IsActionPressed("Jump"))
{
    // 响应跳跃
}
```

### LoadJson

`void LoadJson(string json)` 在运行时热替换动作映射表；`IsActionDown`、`IsActionPressed`、`GetAxis1D` 与 `GetAxis2D` 通过原生桥直查 `InputActionMap`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Scripting/Managed/InputActions.cs#L12)

## 游戏控件、窗口适配与数学基础

[WindowsUiInput](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/Windows/WindowsUiInput.h#L23) 将 Win32 指针、键盘与 IME 组合消息送入 UI 树；[UpdateFrameTimer()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/Windows/FrameTimer.cpp#L21) 与 [SandboxDiagnostics](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Application/SandboxDiagnostics.h#L8) 负责帧计时与诊断。变换与界面几何共用 [Quat](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/CoreLayer/Math/Math.h#L34)、[Mat4](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/CoreLayer/Math/Math.h#L68) 与 [GuiGeometry](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/CoreLayer/GuiGeometry.h#L18)。
