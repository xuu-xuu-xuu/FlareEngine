# 托管宿主与原生 ABI

## 从入口到会话

`FlareEngine.Bootstrap` 的 `Program.cs` 解析 `--editor`、项目清单与预览参数：编辑器工作进程无论当前工程是否已存在 `.cs` 脚本，统一以该托管宿主启动，使后续首次新建脚本与多次改码热重载都在同一进程内完成。宿主创建 `EngineSession`，通过 `NativeMethods` 的 `[LibraryImport]` 声明与原生 `se_get_abi_info()` 核对 ABI 版本，再经 `se_session_create()` 建立会话并注册每帧、物理事件、场景同步、脚本绑定 query/apply 以及原位热重载（`se_editor_set_script_reload_callback`）函数指针。

## 托管 API 的转发位置

| 子系统 | 实现如何跨边界 |
| --- | --- |
| 实体、场景与时间 | `F_Entity` / `Entity` 经句柄读写原生变换与动画；`SceneService` 管理场景加载，`F_Scene` 提供运行期实体查找与克隆；`TimeOfDayService` 与 `F_Time` 分别同步昼夜环境时钟与帧缩放时间。 |
| 物理、导航与音频 | `RigidBody` 与 `CharacterController` 绑定 `ScenePhysics` 代理；`Navigation` 转发 `SceneNavigation` 路径查询与寻路目标；`Audio` 与 `Particles` 控制 3D 声源和粒子发射器。 |
| 输入与游戏界面 | `ScriptInput` 与 `F_Input` 提供当前帧未被编辑器消费的键鼠边沿状态，`InputActions` 统一映射键鼠与 XInput 手柄；`GameUi` 向原生 UI 树提交控件读写。 |
| 编辑器热重载与日志 | 原生后台 `dotnet build` 完成后回调托管 `ReloadAssembly` 热换程序集；`F_Log` 输出与未捕获脚本异常经桥接轮询实时推入编辑器控制台面板。 |
| 性能采集 | `SetWindowProfile()` 把窗口帧耗时与 GPU 统计采集设置交给原生渲染器。 |

## 运行诊断

`BootstrapDiagnostics` 记录托管启动与会话阶段日志。显式预检通过 `RuntimePreflight.Verify()` 实际读写管道、序列化数据并校验程序集加载路径。独立预览目录由 `PreviewLease` 持有文件锁以隔离并发会话。

托管行为还可以用 `TickDependency` 声明阶段前置关系，由原生世界的[任务调度器](../runtime/scheduling.md#tickscheduler)按帧或固定模拟时钟拓扑排序执行。
