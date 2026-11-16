# EmptyGame 项目模板

`build.json` 连接 C# 项目、默认 Spin 行为绑定与入口 UI。先用 ProjectTool create 复制新项目；在仓库根目录运行 `powershell -NoProfile -File 新项目/Build.ps1 -SelfEngineRoot 引擎源码目录 -Name 新包名`，会编译、预检并导出到引擎 out/packages。具体阶段与验证状态见 [游戏导出说明](../../../Docs/GameExport.md)。复制到其他位置后，该说明链接需按引擎源码位置查阅。

可通过编辑器脚本 Inspector 或 ScriptBindings 的同一元数据协议修改、增删并保存 `Scripts/Main.bindings.json`；默认绑定实体 3 的旋转速度为每秒 1 弧度。`Run-ProjectPreview.ps1` 支持本模板的 SelfEngineRoot 参数传递，停止并关闭后重新编译/启动。下文原生无窗口回放仍是独立诊断入口，不代替玩家窗口。

`project.json` 声明入口场景、UI 文档、模型/图片 ID，以及固定步物理回放输入。场景含地面、落箱、模型引用和相机数据；UI 含图片节点。`Content/Models` 的三角形和棋盘图来自 SelfEngine 自有小样，不附带第三方素材许可负担。

从模板复制新项目后，在仓库根目录使用 `ProjectTool create Resources/Templates/EmptyGame <目标目录>`、`ProjectTool validate <目标目录>/project.json`，以及 `ProjectTool replay <目标目录>/project.json <输出目录>`。工具完整路径由构建配置决定。`dependencies` 命令输出带 SHA-256 的场景、UI、声明资源及模型外部依赖清单。

在 Visual Studio 2022 开发者 PowerShell 中进入新项目目录配置构建；配置和构建使用同一终端：

```powershell
cmake -S . -B out/build -DSELF_ENGINE_ROOT="<SelfEngine源码目录>"
cmake --build out/build --config Debug --target EmptyGameHeadless
```

运行生成的 `EmptyGameHeadless`，第一个参数为 `project.json` 路径，第二个为尚不存在的回放结果目录。重复对比使用不同目录，避免覆盖上次测量。全部文件写完后才发布目录，不把半成品当成完成结果。它输出最终场景、CPU 原始/统计 CSV 和回放摘要。固定 1/60 秒执行，不创建窗口、不运行 GPU，也不把运行耗时解释为渲染性能。

可独立编译脚本模板：

```powershell
dotnet build Scripts/EmptyGame.Scripts.csproj -p:SelfEngineRoot="<SelfEngine源码目录>"
```

`Spin` 为后续挂在无刚体模型实体上的行为示例。当前原生 headless 回放不加载 .NET 行为；脚本运行由引擎的托管宿主接入。项目校验不替代脚本编译、视觉验收、正式发布包或无开发环境验收。
