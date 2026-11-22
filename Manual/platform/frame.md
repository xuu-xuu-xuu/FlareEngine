# 从窗口消息到一帧画面

Flare 的每一帧都从 Win32 `PeekMessageW` 出发，按固定顺序走完“窗口消息与手柄采样 → 帧计时 → UI 与世界更新 → 提取只读 `FrameData` 快照 → 渲染器提交 GPU → 清理单帧事件”。应用层与渲染层只通过值快照和三个虚函数交界，退出时严格先排空 GPU 再销毁 `HWND`。

## 一帧主循环全链路

```text
CreateEngineWindow()（注册 FlareEngineWindowClass + DPI 感知 + 客户区尺寸）
   │
   ▼
┌────────────────────────────── RunSandboxApp 主循环 ──────────────────────────────┐
│                                                                                   │
│  1. PumpWindowMessages()                                                          │
│     └─ while (PeekMessageW(..., PM_REMOVE))                                       │
│          ├─ WM_QUIT ──► 退出循环                                                  │
│          ├─ VK_PROCESSKEY（非文本框）──► 捕获原始键码 + 取消 IME 组合             │
│          └─ DispatchMessageW() ──► WindowProc                                     │
│               ├─ state->messageHandler（编辑器/游戏 UI 优先消费）                 │
│               └─ ProcessWindowsInput()（写入 window.input 持续态与事件队列）      │
│                                                                                   │
│  2. PollWindowsGamepads(window.input.gamepads)（动态 XInput 0..3，断连 500ms 退避）│
│                                                                                   │
│  3. drawFrame()                                                                   │
│     ├─ UpdateFrameTimer()（QPC 计时；固定步覆盖或钳位 ≤ 0.1s）                    │
│     ├─ [CPU.Update]                                                               │
│     │    ├─ GameUI Layout + Update                                                │
│     │    ├─ services->input(input, keyboardConsumed, pointerConsumed)             │
│     │    ├─ services->update / runEnvironmentFrame(dt)（脚本 + 物理固定步）       │
│     │    └─ GameUI Layout + SynchronizeCapture + PositionIme                      │
│     ├─ [CPU.Extract]                                                              │
│     │    └─ BuildFrameData() ──► 栈上只读 FrameData 快照（不含可变 World 指针）   │
│     ├─ renderer.Render(RenderFrame{surface, dt, &data, profiler, ...})            │
│     └─ window.input.ClearEvents()（清空脉冲事件与 mouseDelta，保留 keysDown）     │
│                                                                                   │
│  4. 休眠节流：最小化 / Occluded 时 Sleep(16)；拖边重绘时 Sleep(0)                 │
└───────────────────────────────────────────────────────────────────────────────────┘
   │  退出循环
   ▼
SandboxLifetime::Shutdown()
   1. renderer.Shutdown()（排空 GPU、销毁交换链与所有 RHI 对象）
   2. 释放 uiInput & 调用 windowClosing 回调
   3. 关闭 postQuitOnDestroy 并 DestroyEngineWindow()（避免遗留 WM_QUIT）
```

## 窗口创建、消息泵与拖边重绘

Win32 平台封装位于 [`Source/PlatformLayer/Windows/Window.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/Window.cpp#L1)：

- **DPI 感知与窗口创建**：[`EnableEngineDpiAwareness()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/Window.cpp#L206) 优先开启 `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`；[`CreateEngineWindow()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/Window.cpp#L220) 注册 `FlareEngineWindowClass`（`hbrBackground = nullptr` 避免拖边闪烁），并通过 `WM_NCCREATE` 把调用方持有的 `WindowState*` 绑定到 `GWLP_USERDATA`。若设置了环境变量 `FLARE_TEST_OFFSCREEN`，窗口自动创建在 `(-30000, -30000)`，供自动化 UI 测试在后台无打扰运行。
- **消息泵与中文输入法隔离**：[`PumpWindowMessages()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/Window.cpp#L339) 非阻塞拉空队列。遇到 `WM_KEYDOWN` 且 `wParam == VK_PROCESSKEY` 时，先用 `ImmGetVirtualKey()` 记录真实虚拟键码；若当前窗口不处于文本编辑态，直接取消 IME 组合并跳过 `TranslateMessage`，防止场景快捷键弹出拼音候选框。
- **拖边实时重绘（无黑边/冻结）**：Windows 拖拽窗口边框时会阻塞在系统内部模态循环里。`WindowProc` 监听 `WM_ENTERSIZEMOVE` / `WM_SIZE` / `WM_EXITSIZEMOVE`（[`Window.cpp#L79`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/Window.cpp#L79)），尺寸一变就同步发送自定义消息 `EngineWindowDrawResizeFrame` 回调 `state->drawResizeFrame()`；同时 `RunSandboxApp` 在编辑器模式下为交换链按显示器对齐预分配 `paddedSurfW × paddedSurfH` 并置位 `allowOversizedTarget`，避免每次像素级拖边都重建交换链与闪烁。

## 帧计时与 RunSandboxApp

[`FrameTimer.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/FrameTimer.cpp#L3) 使用 `QueryPerformanceFrequency` / `QueryPerformanceCounter` 计算高精度帧间隔（先做 64 位整数差再转 `double` 除法，保持长时运行精度）。

宿主 [`RunSandboxApp()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/SandboxApp.cpp#L76) 把窗口、本地化表、计时器、输入路由与渲染器组装在一起：

```cpp
// Source/FunctionLayer/Application/SandboxApp.cpp
const double updateDeltaSeconds = services && services->fixedDeltaSeconds > 0
    ? services->fixedDeltaSeconds
    : (timer.deltaSeconds > maxUpdateDeltaSeconds ? maxUpdateDeltaSeconds : timer.deltaSeconds);

// ...
for (;;)
{
    pumping = true;
    const bool running = PumpWindowMessages(exitCode);
    pumping = false;
    flare::PollWindowsGamepads(window.input.gamepads);
    if (!running || finished || window.closeRequested || !drawFrame()) break;
    if (!window.drawable) Sleep(16);
    else if (window.interactiveResize) Sleep(0);
    else if (result == RenderResult::Occluded) Sleep(16);
}
```

- **时间步长保护**：默认将真实 `deltaSeconds` 钳位在 `maxUpdateDeltaSeconds = 0.1` 秒，防止断点调试或切屏回来一帧跳变过大；离线录制与确定性测试可通过 `ApplicationServices::fixedDeltaSeconds` 强制覆盖每帧步长（`rawDeltaSeconds` 仍保留真实墙钟间隔供诊断）。
- **相机意图解耦**：[`FreeCameraInputRouter`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FreeCameraInput.h#L9) 只读取 `InputState` 与 UI 消费标志产出当帧相机意图，不修改原始输入状态。
- **RAII 安全关闭**：[`SandboxLifetime::Shutdown()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/SandboxApp.cpp#L41) 保证无论正常退出还是抛异常，都按 **渲染器关闭 → UI 输入释放 → 关闭回调 → 销毁 HWND** 的逆序清理。

## 应用与渲染的交界

应用层与渲染层之间只交换不可变的单帧值快照，渲染器绝不直接访问 `World` 或 `HWND` 的创建销毁：

- **[`FrameData`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FrameData.h#L54)**：由 [`BuildFrameData()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FrameData.h#L128) 从 `World` / `Environment`、`UiTree` 与 `PhysicsWorld` 提取网格实例、相机、灯光、阴影、昼夜大气、后处理、粒子和 UI 可视列表。不含可变世界指针或 GPU 句柄。
- **[`RenderFrame`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Renderer.h#L19)**：打包 `RenderSurface`、步长、帧序号、`const FrameData*` 与可选 `Profiler*`，`data` 仅在当次 `Render()` 调用期间有效。
- **[`Renderer`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Renderer.h#L41)**：仅暴露 `Initialize()`、`Render()`、`Shutdown()`（及编辑器视口查询 `QueryEditorView()`），由三类实现复用：
  - **[`SandboxRenderer`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/SandboxRenderer.h#L34)**：核心场景渲染实现，驱动阴影、前向/延迟特性、光照、后处理与 `GameUIRenderer`。
  - **[`EditorRenderer`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorRenderer.h#L9)**：同样继承 `Renderer` 并内部持有 `SandboxRenderer`，把 3D 场景渲染到 `EditorViewportTarget` 再交给 `GuiCompositor` 合成编辑器界面。
  - **[`WindowProfileRenderer`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/WindowProfile.h#L10)**：轻量装饰器，在诊断模式下包装真实 `SandboxRenderer`，记录最多 6,000 帧的提交号、耗时、DrawCall 与遮挡状态。

## 关键入口速查

| 符号 | 位置 | 作用 |
| --- | --- | --- |
| `EnableEngineDpiAwareness()` / `CreateEngineWindow()` | [`Window.cpp#L206`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/Window.cpp#L206) | 开启 Per-Monitor V2 DPI 感知并创建 Win32 主窗口 |
| `PumpWindowMessages()` | [`Window.cpp#L339`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/Window.cpp#L339) | 非阻塞 `PeekMessageW` 消息泵与 `VK_PROCESSKEY` 处理 |
| `InitializeFrameTimer()` / `UpdateFrameTimer()` | [`FrameTimer.cpp#L3`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/FrameTimer.cpp#L3) | 基于 QPC 的高精度帧计时 |
| `RunSandboxApp()` | [`SandboxApp.cpp#L76`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/SandboxApp.cpp#L76) | 引擎窗口应用主循环与生命周期协调器 |
| `ApplicationServices` | [`FrameData.h#L131`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FrameData.h#L131) | 注入宿主回调（输入、更新、提取、固定步长、拖边重绘） |
| `Renderer` / `RenderFrame` | [`Renderer.h#L19`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Renderer.h#L19) | 应用层与渲染实现的最小接口契约 |

::: warning 不要在渲染器里缓存 `RenderFrame::data` 指针
`FrameData` 是在 `RunSandboxApp` 的 `drawFrame()` 栈上按帧构造并销毁的；任何需要跨帧或异步引用的资源（如资产 `shared_ptr`、字体图集）必须由渲染器自行复制或提升所有权。
:::
