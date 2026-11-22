# 构建工具、预览与导出

Flare 的离线交付链路把项目校验、资源烘焙、脚本编译、运行时打包和冒烟验证串成一条可重复流水线：无论是从源码仓库导出，还是从分发出去的便携编辑器包离线导出，最终都会原子生成一个自带运行时、可双击 `Play.exe` 直接运行的独立游戏目录。

## 离线游戏导出流水线

```text
project.json + build.json（可选）
   │
   ▼  Export-Game.ps1（加 ProjectBuildLease 租约 + 局部化 TEMP / NuGet 缓存）
1. content_validation ──► ProjectTool.exe dependencies（校验场景/资产/UI/输入并算出 SHA-256）
   │
2. publish_runtime    ──► 源码模式：Publish-Managed.ps1 发布自包含 .NET 10 + EngineNative.dll
   │                      便携包模式：直接复用包内 Player/
   │
3. publish_scripts    ──► 命中预编译模板（SDK/Precompiled/FlareGame）直接复用；
   │                      否则校验 .NET 10 SDK 并 dotnet restore + publish 项目脚本
   │
4. 组装暂存目录 (.Name.stage-<guid>)
   ├─ 按 dependencies.json 复制项目文件并逐一重算 SHA-256（防中途篡改）
   ├─ 复制 .cache/assets 下的已烘焙模型与 .seaud 音频缓存
   ├─ 复制 app-local MSVC CRT (VC143) 与第三方开源许可
   └─ 写入 launch.json、Start-Game.ps1 与独立入口 Play.exe
   │
5. 自动验收
   ├─ field_preflight  ──► FlareEngine.Bootstrap.exe --validate-bindings
   ├─ packaged_content ──► ProjectTool.exe validate
   ├─ headless_smoke   ──► Start-Game.ps1 -Headless -Frames 120
   ├─ window_smoke     ──► Start-Game.ps1 -Hidden -Frames 12 -Capture game-window.ppm
   └─ (可选 recordVideo) ► 固定步抓帧 + VideoEncoder.exe 压制 clip.mp4
   │
6. 最终核验 & 原子发布
   ├─ 复核源码/编辑器包与脚本输入摘要未变化
   ├─ 生成 package-manifest.json（全文件字节数与 SHA-256）
   └─ [IO.Directory]::Move($stage, $final)（同卷原子切换，失败则自动清理暂存目录）
```

## 原生工具与项目清单

[`CMakeLists.txt`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/CMakeLists.txt) 分别构建引擎核心库、`EngineNative.dll`、`FlareEditor.exe` 与多个独立离线工具：

- **[`ProjectTool.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/ProjectTool.cpp#L5)**：提供 `create`、`validate`、`dependencies` 与 `replay` 四个命令行入口，底层对应 [`Project.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/Project.h#L31) 的五个公共接口。
- **[`ShaderBuilder.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/ShaderBuilder.cpp)** 与 **[`EnvironmentCooker.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/EnvironmentCooker.cpp)**：离线编译 HLSL 着色器产物（`.sebin`）和烘焙 IBL 环境贴图（`.seenv`），连同 `RenderingCooker`、`DiffuseProbeCooker`、`NavigationCooker` 一起分发到编辑器包的 `Tools/` 目录。

### 清单校验与玩家画质配置

[`ReadProject()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/Project.cpp#L121) 对 `project.json` 执行严格白名单字段检查（拒绝未知键、重复键与越界路径），除了基础资产/场景/UI 列表外，还支持玩家运行时画质配置字段：

```cpp
// Source/ToolLayer/Tools/Project.cpp
const std::set<std::string> supported{
    "format", "version", "name", "contentRoot", "entryScene",
    "scenes", "ui", "assets", "replay",
    "showPlayerQualityMenu", "playerSettingsId", "qualityProfiles"
};
```

- **`showPlayerQualityMenu`**（默认 `true`）：控制导出游戏窗口是否允许玩家呼出画质设置菜单。
- **`playerSettingsId`**：可选的游戏设置隔离 ID；留空时按项目名隔离，由 [`EngineSession.ConfigurePlayerQuality()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L134) 取 SHA-256 映射到 `%LOCALAPPDATA%\FlareEngine\Players\<hash>\quality.txt`，通过 `MoveFileExW` 原子落盘（[`PlayerQualityPreference::Select()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/PlayerQuality.cpp#L37)）。
- **`qualityProfiles`**：定义 `performance`、`balanced`、`high` 三档画质覆盖项（[`QualityProfiles.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/QualityProfiles.h#L13) 中的 `sampling`、`allowShadows`、`allowBloom`）。渲染每帧提取快照后，[`ApplyPlayerQuality()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/PlayerQuality.cpp#L13) 只在临时 `FrameData` 上覆写开关，切回 `project` 档无需逆向修改场景世界。

校验通过后，[`ValidateProject()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/Project.cpp#L178) 会加载每一个模型、贴图、材质、音频、NavMesh、TOD、Prefab、场景、输入配置和游戏 UI 文档，验证全部交叉引用有效；再由 [`ExportProjectDependencies()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/Project.cpp#L359) 导出含文件大小与 SHA-256 的 `dependencies.json`。

## 行为编译、预览与进程管理

- **[`Build-EditorScriptCandidate.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Build-EditorScriptCandidate.ps1#L1)**：扫描项目内 `.cs / .csproj / .props / .targets / build.json` 计算输入摘要，构建独立候选程序集与 `candidate.json`；在候选通过校验前绝不覆盖上一份可用版本。
- **[`Run-ProjectPreview.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Run-ProjectPreview.ps1#L1)**：校验项目与原生 DLL，编译 Bootstrap 与项目脚本，再启动 `Editor / Window / Headless` 预览并输出 `preview.json` 报告。
- **[`ProjectBuildLease.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/ProjectBuildLease.ps1#L2)**：通过 `out/preview/preview.lock` 的两字节区域锁协调并发——构建期间锁住字节 0 与字节 1；启动预览子进程前释放字节 1 交由子进程接管，防止构建与预览互相踩踏输出目录。
- **子进程隔离**：编辑器内部构建与导出统一走 [`RunEditorProcess()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorProcessRunner.cpp#L207)（绑定 Windows Job Object 支持随主进程清理与取消）；需要脱离内部 Job 的外部程序（如打开外部 IDE / 文件夹）则经 [`ExternalProcessBroker`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/ExternalProcessBroker.h#L10) 转发启动。

## 游戏导出服务与 Export-Game.ps1

编辑器界面与命令行共用同一份 [`Export-Game.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Export-Game.ps1#L1)，绝不维护两套打包逻辑：

- **UI 后台编排**：[`ExportOperationController`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/ExportOperation.h#L17) 维护 `Queued -> Running -> Succeeded / Failed / Cancelling -> Cancelled` 状态机并以 `workspaceGeneration` 拒绝过期回调；[`ExportService`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/ExportService.h#L31) 构造参数并实时轮询 `export-report.json`；[`ExportJob`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/ExportJob.h#L29) 在后台线程运行导出并持久化最近一次导出报告。
- **托管运行时发布**：源码工作区由 [`Publish-Managed.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Publish-Managed.ps1#L1) 以自包含多文件模式发布 `Bootstrap.csproj`、`EngineNative.dll` 与渲染资源，并通过 `-ManagedOutputRoot` 把中间目录隔离到本次导出专有路径，避免与运行中的编辑器冲突。
- **防中途篡改与原子交付**：导出开始前通过 [`SourceIdentity.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/SourceIdentity.ps1#L6) 或 `Read-EngineEditorPackage` 固定输入摘要；发布前复核所有已烘焙资产、引擎源码和脚本输入均未在导出过程中变动，生成 `package-manifest.json` 后由 [`[IO.Directory]::Move($stage, $final)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Export-Game.ps1#L269) 一次性切为最终目录。
- **内置视频录制集成**：若 `build.json` 启用 `recordVideo: true`，`Export-Game.ps1` 会在冒烟验证后自动调用包内 `Start-Game.ps1` 执行[固定步连续抓帧](frame-capture.md#setcapturesequence)，再用 [VideoEncoder](frame-capture.md#videoencoder) 在包内产出 `clip.mp4` 并清理临时帧目录。

## 项目回放（ReplayProject）

[`ReplayProject()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/Project.cpp#L378) 提供无需图形设备的确定性物理与 UI 无头回放：按固定 60 Hz 步长注入 `project.json` 中记录的刚体冲量 `impulses`，同步世界变换并推进 UI 布局，最终将 `final.scene.json`、`replay.json`（含 `stateSha256` 状态摘要）以及 `cpu-frames.csv` / `cpu-summary.csv` 写入临时目录，再经带重试退避的 [`RenameDirectoryWithRetry()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/Project.cpp#L39) 发布到目标目录（应对 Windows 杀毒软件或索引器短暂占用）。

## 工具链速查表

| 工具 / 脚本 / 类 | 源码位置 | 职责 |
| --- | --- | --- |
| `ProjectTool` / `Project.cpp` | [`Project.cpp#L121`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/Project.cpp#L121) | 清单解析、依赖收集、模板创建与 60 Hz 无头回放 |
| `ApplyPlayerQuality()` / `QualityProfiles` | [`PlayerQuality.cpp#L13`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/PlayerQuality.cpp#L13) | 玩家运行时三档画质覆盖与本地偏好持久化 |
| `Export-Game.ps1` | [`Export-Game.ps1#L1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Export-Game.ps1#L1) | 统一离线游戏导出、冒烟验证、可选录屏与清单签发 |
| `Publish-Managed.ps1` | [`Publish-Managed.ps1#L1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Publish-Managed.ps1#L1) | 发布自包含 win-x64 托管宿主与渲染资源 |
| `ExportJob` / `ExportService` | [`ExportJob.h#L29`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/ExportJob.h#L29) | 编辑器后台导出作业、分阶段报告与取消清理 |
| `ProjectBuildLease.ps1` | [`ProjectBuildLease.ps1#L2`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/ProjectBuildLease.ps1#L2) | 双字节文件锁协调构建、导出与预览并发 |

::: warning 导出前不要向输出目录手动放同名文件夹
`Export-Game.ps1` 和 `Build-EditorPackage.ps1` 均遵循**绝不静默覆盖既有产物**原则：目标路径已存在时立即报错 `Export.OutputExists`，请更换 `-Name` 或先移走旧导出目录。
:::
