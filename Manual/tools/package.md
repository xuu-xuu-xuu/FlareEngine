# 构建环境、便携编辑器包与包内启动

Flare 把“开发者在源码仓库里编译”和“用户拿着解压即用的便携编辑器包做项目与导出”两条路径统一到了同一组脚本和契约之下：便携包内完整携带 `FlareEditor.exe`、离线工具、自包含玩家运行时、带 XML 注释文档的托管 SDK 与预编译模板脚本，且通过 `editor-package.json` 全量 SHA-256 清单与 MSBuild 目录配置隔离防串扰。

## 便携编辑器包目录布局

运行 [`Scripts/Build-EditorPackage.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Build-EditorPackage.ps1#L1) 后，产出的便携包目录结构如下：

```text
FlareEditor-<Name>/
├── FlareEditor.exe                 # 编辑器主程序（附同级 app-local VC143 CRT DLL）
├── Start-Editor.ps1                # 包内快捷启动脚本（传 --project / --language / --safe-mode）
├── editor-package.json             # 清单：export_contract=1 + source_sha256 + 全文件 SHA-256
├── NuGet.Config                    # 离线/工作区隔离 NuGet 配置
├── README.md                       # 来自 Docs/EditorPackageReadme.md
├── Resources/
│   ├── Shaders/ Textures/ Lighting/ Environments/ Localization/
│   └── Templates/                  # 从仓库干净文件复制的模板（不含 .cache / bin / obj / 日志）
│       ├── RenderingStudio/Scripts/
│       │   ├── FlareGame.Scripts.csproj      # 已自动改写为引用 $(FlareEngineRoot)/SDK/Managed/...
│       │   ├── Directory.Build.props         # 隔离 IDE obj/bin 与外层配置继承
│       │   └── Directory.Build.targets       # 配对阻断向上层目录继续查找 MSBuild 配置
│       └── IndoorGallery/...
├── Tools/                          # ProjectTool / ShaderBuilder / VideoEncoder / 4 个 Cooker + CRT
├── Player/                         # 预发布自包含 win-x64 玩家宿主（FlareEngine.Bootstrap.exe + EngineNative.dll）
├── SDK/
│   ├── FlareEngine.Scripts.props   # 统一 net10.0 / x64 / ImplicitUsings / 全局 Using FlareEngine
│   ├── FlareEngine.Scripts.targets # 构建期将引擎 ProjectReference 替换为包内 FlareEngine.Managed.dll
│   ├── Managed/
│   │   ├── FlareEngine.Managed.dll # 托管脚本 API 程序集
│   │   └── FlareEngine.Managed.xml # XML 文档注释，供 VS / Rider 提供完整中文 IntelliSense
│   └── Precompiled/FlareGame/      # 预编译模板脚本产物 + manifest.json（源码未改时免 SDK 秒导出）
├── Scripts/                        # Export-Game.ps1 / EditorPackageRuntime.ps1 等 9 个运行脚本
├── Runtime/                        # VC143 CRT 拷贝源（导出游戏时复制到游戏包）
└── ThirdParty/                     # 全部第三方库 LICENSE / NOTICE
```

## Build-EditorPackage.ps1 打包与分发守卫

[`Build-EditorPackage.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Build-EditorPackage.ps1#L1) 强制要求 `Release` 配置，并在打包过程中执行多层质量闸门：

1. **干净模板收集**：若处于 Git 仓库则用 `git ls-files --cached --others --exclude-standard -- Resources/Templates` 收集文件，避免把开发者试跑留下的 `bin/obj/.cache/*.log` 误打进分发包（[`Build-EditorPackage.ps1#L53`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Build-EditorPackage.ps1#L53)）。
2. **托管 SDK 与 XML 文档分发**：先用 `Publish-Managed.ps1` 生成 `Player/`，随后同时复制 `FlareEngine.Managed.dll` 与 **`FlareEngine.Managed.xml`** 到 `SDK/Managed/`（[`Build-EditorPackage.ps1#L88`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Build-EditorPackage.ps1#L88)），让脱离源码树的用户在 Visual Studio / Rider 里写 `F_` 脚本时依然有完整的 API 中文智能提示。
3. **模板预编译与派生清理**：在打包时为模板预编译 `SDK/Precompiled/FlareGame/Files/` 并签发源文件 SHA-256 清单，完成后立即删掉模板目录里 `dotnet publish` 产生的 `Scripts/bin` 和 `Scripts/obj`（[`Build-EditorPackage.ps1#L159`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Build-EditorPackage.ps1#L159)）。
4. **隐私与路径泄露扫描**：拒绝任何 `out/Editor/User`、`.git`、`.cache`、`Source/`、`Tests/` 或 `CMakeLists.txt` 进入包内，并扫描所有 `.txt / .ps1 / .json / .md / .cfg` 确保不残留打包机的绝对工作区路径（[`Build-EditorPackage.ps1#L209`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Build-EditorPackage.ps1#L209)）。
5. **脱离工作区实测自检**：将当前工作目录切到 `$env:TEMP` 后运行 `FlareEditor.exe --smoke-launcher 400 --no-ui-errors`，确认便携包不依赖源码目录即可干净启动（[`Build-EditorPackage.ps1#L220`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Build-EditorPackage.ps1#L220)）。
6. **清单双检与原子发布**：生成 `editor-package.json` 后立即反向遍历核对磁盘文件集合与每一个 SHA-256，全部通过才 `[IO.Directory]::Move($stage, $final)`（[`Build-EditorPackage.ps1#L240`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Build-EditorPackage.ps1#L240)）。

## Directory.Build.props 隔离与预编译复用

为了让用户的 IDE（Visual Studio / Rider / VS Code）与编辑器后台构建/离线导出互不干扰，项目脚本目录与运行脚本配合做了两层隔离：

```xml
<!-- Resources/Templates/RenderingStudio/Scripts/Directory.Build.props -->
<Project>
  <!-- 模板独立配置；位于引擎仓库内或复制到其他目录时均不继承外层配置。 -->
  <PropertyGroup>
    <DefaultItemExcludes>$(DefaultItemExcludes);obj/**;bin/**</DefaultItemExcludes>
  </PropertyGroup>
  <Import Project="../out/Editor/FlareEngine.Ide.props"
          Condition="'$(FlareEngineRoot)' == '' And Exists('../out/Editor/FlareEngine.Ide.props')" />
</Project>
```

- **隔绝上层配置与 IDE 中间文件**：[`Directory.Build.props`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Templates/RenderingStudio/Scripts/Directory.Build.props#L1) 配对同目录空文件 `Directory.Build.targets`，既阻断 MSBuild 向上递归加载仓库外层的构建脚本，又把 `obj/**;bin/**` 排除在默认编译项外；当离线导出编译自定义脚本时，[`Get-EnginePackageScriptProperties()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/EditorPackageRuntime.ps1#L71) 进一步通过 `-p:BaseIntermediateOutputPath` 与 `-p:MSBuildProjectExtensionsPath` 把中间产物重定向到本次导出的隔离目录（`script-obj/`），并用 `-p:CustomAfterMicrosoftCommonTargets=.../SDK/FlareEngine.Scripts.targets` 将源码的 `ProjectReference` 无缝切换为指向包内 `SDK/Managed/FlareEngine.Managed.dll`。
- **包完整性与预编译秒导出**：[`EditorPackageRuntime.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/EditorPackageRuntime.ps1#L17) 提供两个核心运行函数：
  - `Read-EngineEditorPackage()`：导出前校验 `editor-package.json` 中每一个文件的长度、重解析点（拒绝符号链接逃逸）与 SHA-256，以及包内多出来的未登记文件。
  - `Find-EnginePrecompiledScripts()`：比对项目脚本文件哈希是否与 `SDK/Precompiled/FlareGame/manifest.json` 逐一吻合（仅允许运行时字段文档 `Main.bindings.json` 单独改动）；命中时直接复制预编译程序集，无需在目标机安装 .NET 10 SDK 即可完成导出。

## 构建环境、资源复制与启动入口脚本

| 脚本 | 源码位置 | 职责与关键实现 |
| --- | --- | --- |
| `Import-MsvcEnvironment.ps1` | [`Import-MsvcEnvironment.ps1#L3`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Import-MsvcEnvironment.ps1#L3) | 通过 `vswhere.exe` 定位 VS 2022 并运行 `VsDevCmd.bat -arch=amd64`，以 UTF-16 Base64 传回全部环境变量，避免控制台代码页破坏中文路径或含等号/换行的变量值 |
| `Copy-RendererResources.ps1` | [`Copy-RendererResources.ps1#L1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Copy-RendererResources.ps1#L1) | 校验并复制着色器源码/预编译 `.sebin`、`checker.png`、BRDF/LTC/Sheen/Skin 查表纹理与默认环境贴图 `soft.seenv` |
| `Generate-FlareBranding.ps1` | [`Generate-FlareBranding.ps1#L1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Generate-FlareBranding.ps1#L1) | 从 `Resources/Branding/FlareMark.png` 生成包含 16..256 九种尺寸的 Windows 多帧 `.ico` 图标资源 |
| `Start-Editor.ps1` | [`Start-Editor.ps1#L1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Start-Editor.ps1#L1) | 源码工作区按 `-Configuration Debug|Release` 启动 `FlareEditor.exe`，转交 `-Project`、`-Language`、`-SafeMode`；便携包根目录有同参数独立版本 |
| `Export-Package.ps1` | [`Export-Package.ps1#L1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Export-Package.ps1#L1) | 兼容旧入口，直接透传参数到 [`Export-Game.ps1`](delivery.md) |
| `Start-ExportedGame.ps1` | [`Start-ExportedGame.ps1#L1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Start-ExportedGame.ps1#L1) | 复制到游戏包内作为 `Start-Game.ps1`，读取 `launch.json` 校验路径不越界后调用 `FlareEngine.Bootstrap.exe`，支持 `-Headless / -Hidden / -Resolution / -FixedStep / -Capture / -CaptureSequence / -Profile` |

::: tip 为什么 `Start-ExportedGame.ps1` 调用可执行文件前要把 `$ErrorActionPreference` 切为 `Continue`
Windows PowerShell 5.1 在 `$ErrorActionPreference = 'Stop'` 下，会把被重定向的原生 `stderr` 诊断行（例如着色器或图形设备提示）误判成终止异常。[`Start-ExportedGame.ps1#L32`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Start-ExportedGame.ps1#L32) 在拉起 `FlareEngine.Bootstrap.exe` 前放宽该策略，并严格以 `$LASTEXITCODE` 作为最终成败依据。
:::

::: warning 便携包目录应视为只读分发物
`Read-EngineEditorPackage()` 会拒绝便携包目录内任何未登记或哈希不符的文件。请勿把用户项目或输出目录建在便携包根目录里面（`Export-Game.ps1` 也会显式拦截落在包目录内的 `-OutputDirectory`）。
:::
