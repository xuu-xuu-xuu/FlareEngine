# 项目入口、启动进度与编辑器外壳

## 进程和项目归属

`RunEditorLauncher()` 管理启动器的项目选择、新建工程与示例副本创建，`ProjectWorkspaceSession` 处理模板复制、预检和最近项目列表。`ProjectLease` 获取项目编辑独占锁后，`RunEditorWorker()` 统一拉起 `.NET 10` 托管宿主 `FlareEngine.Bootstrap.exe --editor` 承载编辑器工作进程（即使工程初始不含任何 `.cs` 脚本也直接运行于托管宿主之上，使首次创建脚本与后续热重载无需切换进程或重启窗口）。Windows 图形入口的 `ShowDebugConsole` 开关控制开发期独立调试控制台。

## 真实进度如何到达窗口

`ReportStartup()` 发布各子系统启动阶段，`StartupProgressChannel` 在进程间传递阶段百分比和取消信号。`EditorLoadingScreen` 管理可拖动的独立加载窗口，`DrawLoadingRibbon()` 按真实阶段绘制金色飘带进度，直至主窗口首帧 D3D12 交换链呈现完成后无缝切换显示。窗口与任务栏图标统一通过 `ApplyAppIcon()` 设置。

## 自研 UI 单栈外壳与停靠持久化

`RunEditorApp()` 将编辑会话、输入与渲染器装配到原生窗口宿主。`EditorShell` 基于[自研 UI Toolkit 单栈](../ui/toolkit.md)构建全部菜单栏、工具栏、状态栏、停靠区与模态窗，零第三方 IMGUI 依赖。`EditorCommandRouter` 将全局与面板级快捷键送到 `EditorCommandBus`；主窗口及拖出的原生浮动面板树通过 `EditorDockState` 序列化为布局配置并支持一键重置，详见[面板拖出与吸附](docking.md)。
