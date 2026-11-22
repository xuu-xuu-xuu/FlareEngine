# 项目入口、启动进度与编辑器外壳

本文讲清 `FlareEditor.exe` 从双击启动到进入主编辑循环的完整进程链，以及 Play Mode 指针解锁与外部 IDE 工作区同步的实现机制，位于引擎的 **ToolLayer / Editor** 入口层。

## 启动与托管宿主链路

编辑器采用「**启动器 → 监督工作进程 → `.NET 10` 托管宿主**」三级进程模型，避免项目切换与脚本编译相互污染，并让零脚本工程首次创建 `.cs` 后无需重启即可热重载：

```text
FlareEditor.exe (wWinMain -> RunEditor)
  │
  ├─ 无 --worker ──► RunEditorLauncher()
  │                   ├─ 传 --project：Preflight 预检 ─► 直接拉起 --worker
  │                   └─ 无 --project：显示启动页（新建/打开/示例/最近项目）
  │                                     └─ 选中项目 ─► CreateProcess("--worker --project ...")
  │
  └─ 带 --worker ──► 路径归一（若传入 project.json 则取 parent_path()）
                      └─ RunEditorWorker()  [持有唯一 ProjectLease 独占锁]
                          ├─ 创建独立线程 EditorLoadingScreen + StartupProgressChannel
                          ├─ 交互态直启托管宿主：FlareEngine.Bootstrap.exe --editor
                          │   ├─ 复用最新热重载 DLL（若 Scripts/*.cs 未更新）
                          │   └─ 未编译时标 FLARE_EDITOR_UNCOMPILED_SCRIPTS=1，进编辑器后异步编译
                          ├─ 烟测/强制构建路径：RunManagedEditorWorker()
                          │   └─ 执行 Scripts/Build-EditorScriptCandidate.ps1 -> 校验 candidate.json
                          └─ 托管/原生回退均进入 RunEditorApp() -> RunSandboxApp()
                              └─ 退出时若写出 restart.json(action="compile")，监督循环自动重拉
```

## 入口分发与路径归一

[`FlareEditor.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/FlareEditor.cpp#L61) 负责诊断初始化、便携包日志重定向与参数分流。便携包安装目录包含 `editor-package.json`，启动时会把 `FLARE_LOG_DIRECTORY` 临时指向用户数据目录下的 `Logs`，不污染受包清单校验的安装目录。

```cpp
// Source/ToolLayer/Tools/FlareEditor.cpp#L90-L109
if (options.worker)
{
    flare::EditorWorkerOptions worker;
    // 命令行既允许传项目目录，也允许直接传 project.json；统一归一到项目根目录
    worker.projectRoot = std::filesystem::is_regular_file(options.project, fileError)
        ? options.project.parent_path()
        : options.project;
    worker.scene = options.scene;
    worker.capture = options.capture;
    worker.locale = options.locale;
    worker.frames = options.frames;
    worker.hidden = options.hidden;
    worker.demo = options.demo;
    // ... 传递自动化烟测参数
    return flare::RunEditorWorker(worker);
}
return flare::RunEditorLauncher(options);
```

- 开发者若需在本地调试期附带独立控制台，可将 [`FlareEditor.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/FlareEditor.cpp#L17) 顶部的 `ShowDebugConsole` 改为 `true` 并重新构建；管道或文件重定向流会被 `ConnectConsoleStream()` 保留，避免干扰自动化测试。
- 启动页由 [`EditorLauncher.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorLauncher.cpp#L1514) 中的 `RunEditorLauncher()` 驱动，支持 **Blank Scene**、**Basic 3D**、**Rendering Studio**、**Indoor Gallery** 四种模板，并通过 [`ProjectWorkspaceSession`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/ProjectWorkspaceSession.h#L56) 完成只读预检（`Preflight`）、最近项目记录与便携包只读拦截（`RejectPackageProject`）。

## 独占租约、托管宿主与加载进度窗

[`RunEditorWorker()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorWorker.cpp#L330) 作为外层监督进程，生命周期覆盖整次项目编辑：

1. **项目独占锁**：通过 [`ProjectLease::Acquire()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorWorker.cpp#L362) 获取租约；若已被另一编辑器实例占用（`ERROR_SHARING_VIOLATION`），返回退出码 `6`，启动页展示占用提示。
2. **托管宿主优先直启**：在交互运行且存在 `FlareEngine.Bootstrap.exe` 时，[`EditorWorker.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorWorker.cpp#L411) 直接以 `--editor` 模式拉起 `.NET 10` 托管宿主；哪怕项目初始没有任何 `.cs` 脚本，也运行在托管宿主之上，确保后续新建脚本与热重载全程零重启。若存在已编译脚本且 `out/Editor/HotReload/bin-*` 中的热重载 DLL 不旧于最新 `.cs` 文件，直接复用启动；否则设置 `FLARE_EDITOR_UNCOMPILED_SCRIPTS=1` 交由编辑器进入后异步编译。
3. **跨进程真实加载进度**：[`StartupProgressChannel`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/StartupProgress.h#L20) 通过命名共享内存把子进程调用 `ReportStartup(StartupStage)` 发布的真实阶段（`Project` → `Scripts` → `Runtime` → `Assets` → `Scene` → `Interface` → `Renderer` → `FirstFrame` → `Ready`）同步到监督进程。
4. **独立线程加载窗**：[`EditorLoadingScreen`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorLoadingScreen.cpp#L36) 在独立 Win32 UI 线程以 GDI+ [`DrawLoadingRibbon()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/LoadingRibbon.h#L8) 绘制金色飘带进度，支持鼠标拖拽与点取消中止启动；直到主窗口首帧 D3D12 交换链呈现完成并报告 `StartupStage::Ready` 后自动销毁。所有窗口图标统一由 [`ApplyAppIcon()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/AppBranding.h#L8) 设置。

## 主应用装配与 Play Mode 指针释放闭环

[`RunEditorApp()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorApplication.cpp#L248) 通过 `RunSandboxApp()` 驱动原生 Win32 宿主，并以 [`EditorApplicationHooks`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorApplication.h#L25) 注入托管桥接回调。

### Play Mode 进入与退出还原

在 [`ConfigureSessionAndWorkspace()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorApplication.cpp#L107) 中注册的启停回调保证了编辑区与运行区互不干扰：

- **进入 Play**：克隆 `UiTree` 为 `playUi` 并挂接 `EditorPlayInput`，记录 `Game` 页签组原活跃页签 `prePlayGameGroupTab`，聚焦 `Game` 面板；若开启了 `MaximizeOnPlay()`，自动将 `Game` 面板全屏化（`SetGameMaximized(true)`）。
- **退出 Play**：清理导航、物理与音频运行态，自动恢复 `SetGameMaximized(false)` 并切回 `prePlayGameGroupTab`，最后释放 `playUi` 并通知托管层触发 `playStopping`。

### `Esc` 释放锁鼠后点击停止不重锁

FPS / 第三人称游戏在 Play Mode 中会通过 `F_Cursor.Lock()` 锁定光标。按下 `Esc` 临时释放鼠标后，如果玩家去点顶部工具栏的 **停止（Stop）** 按钮，必须防止该次点击又把光标锁回游戏视口。这一闭环由原生桥接与 `EditorApplication` 协作完成：

```text
1. Play 中按 Esc
   └─ EngineNative.cpp CaptureInput() ──► cursorLockPausedByEsc = true + ReleaseSessionCursor()

2. 下一帧鼠标移到 Game 视口外并点击工具栏「停止」
   └─ EditorApplication.cpp services.input()
       ├─ hooks->syncPlayCursor(hostWindow, screenRect) 返回 cursorCaptured = false
       ├─ 检测到 PointerEventType::Down 落在 activePlayRect 外 ──► outsideViewportPointer = true
       └─ effectivePointerConsumed = pointer || outsideViewportPointer  (= true)

3. CaptureInput() 收到 pointerConsumed == true
   └─ 跳过 PointerDown 恢复锁定分支 ──► cursorLockPausedByEsc 保持 true（绝不重锁）
   └─ 点击事件正常命中工具栏 Stop 按钮 ──► StopPlay()
```

```cpp
// Source/ToolLayer/Editor/EditorApplication.cpp#L423-L457
bool cursorCaptured = false;
if (state.hooks->syncPlayCursor && state.hostWindow != nullptr)
{
    // ... 把 activePlayRect 转为屏幕像素矩形 screenRect
    cursorCaptured = state.hooks->syncPlayCursor(state.hostWindow, screenRect);
    if (state.playInput) state.playInput->SetCursorCaptured(cursorCaptured);
}
bool outsideViewportPointer = false;
if (!cursorCaptured && !floatingInput)
{
    for (const auto& ev : gameplay.pointerEvents)
    {
        if (ev.type == PointerEventType::Down && !inPlayViewport(ev.x, ev.y))
        {
            outsideViewportPointer = true; // 视口外点击不得重锁指针
            break;
        }
    }
}
const bool effectivePointerConsumed = pointer || outsideViewportPointer;
```

## 外部 IDE 探测与 `.sln` 工作区同步

[`EditorScriptTools`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.h#L41) 让用户在外部代码编辑器中打开 `.cs` 时即刻获得完整的 `FlareEngine` 智能提示，无需手工配置 SDK 路径：

| 接口 | 源码位置 | 职责说明 |
| --- | --- | --- |
| `CreateBehaviour()` | [`EditorScriptTools.cpp#L355`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L355) | 校验 C# 标识符与 Windows 保留文件名，事务式创建 `F_Behaviour` 模板，若尚无脚本工程则同步生成 `FlareGame.Scripts.csproj`、`Directory.Build.props` 并更新 `build.json`（失败自动回滚） |
| `SyncIdeWorkspace()` | [`EditorScriptTools.cpp#L462`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L462) | 生成本地路径桥接文件 `out/Editor/FlareEngine.Ide.props`、项目根 `<ProjectName>.sln`（确定性 GUID）及 `.vscode/settings.json`，必要时执行静默 `dotnet restore` 生成设计时 `project.assets.json` |
| `DetectInstalledCodeEditor()` | [`EditorScriptTools.cpp#L621`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L621) | 先查 Windows `.cs` 文件关联（识别 `vslauncher.exe` 偏好 Visual Studio），再按 `VS Code / Cursor / Antigravity / Windsurf / VSCodium` → `Visual Studio 2022/18` → `JetBrains Rider` 顺序探测可用可执行文件 |
| `PrepareOpenSource()` / `OpenSource()` | [`EditorScriptTools.cpp#L699`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L699) | 打开 `.cs`/`.csproj`/`.sln` 前自动调用 `SyncIdeWorkspace()`，校验文件处于项目根目录内且拒绝 `cmd.exe`/`powershell.exe` 等危险壳程序，展开 `{file}`、`{line}`、`{column}`、`{project}`、`{sln}` 占位符并精准跳转行列号 |

## 核心入口与外壳接口速查

| 符号 | 头文件 | 作用 |
| --- | --- | --- |
| `flare::ParseEditorLaunchOptions()` | [`EditorLauncher.h#L33`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorLauncher.h#L33) | 解析 `--project`、`--language`、`--safe-mode`、`--worker` 等命令行选项 |
| `flare::RunEditorLauncher()` | [`EditorLauncher.h#L35`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorLauncher.h#L35) | 启动页或显式项目直启工作进程 |
| `flare::RunEditorWorker()` | [`EditorWorker.h#L26`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorWorker.h#L26) | 监督进程：租约锁、加载窗、托管宿主生命周期与受控重启 |
| `flare::RunEditorApp()` | [`EditorApplication.h#L51`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorApplication.h#L51) | 装配 `EditorShell`、`EditorSession`、`EditorFloatingWindows` 与 `RunSandboxApp` |
| `flare::ui::editor::EditorShell` | [`EditorShell.h#L54`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShell.h#L54) | 基于[自研 UI 单栈](../ui/toolkit.md)的编辑器外壳（零第三方 IMGUI 依赖） |
| `flare::ui::editor::EditorCommandRouter` | [`EditorCommandRouter.h#L21`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorCommandRouter.h#L21) | 汇总全局与面板级快捷键并投递到 `EditorCommandBus` |

::: tip 安全模式与会话恢复
启动时传入 `--safe-mode` 会设置 `FLARE_EDITOR_SAFE_MODE=1`，[`RunEditorApp()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorApplication.cpp#L283) 会跳过上次会话自动恢复与崩溃恢复弹窗，便于在损坏场景或异常布局下安全进入编辑器。停靠树持久化详见[面板拖出与吸附](docking.md)。
:::

::: warning 便携包只读约束
从带 `editor-package.json` 的便携包运行编辑器时，严禁直接打开包内示例项目（启动器与工作进程都会返回退出码 `3` 并提示 `Editor.PackageProjectReadOnly`），必须通过「打开示例」复制到包外目录再编辑。
:::
