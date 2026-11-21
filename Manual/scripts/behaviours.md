# C# 行为、零重启热重载与事件

Flare 的脚本系统将 `FlareEngine.F_Behaviour` 派生类发现为可挂载组件，并通过可回收程序集上下文在编辑器内原位热替换，无需退出或重启窗口。日常移动、输入、数学与异步接口见[高频游戏 API](api.md)。

## 零重启编辑器脚本工作流

编辑器工作进程始终通过托管宿主 `FlareEngine.Bootstrap.exe --editor` 启动（初始无 `.cs` 文件的项目传入空 `--project ""`）。用户在属性面板底部点击 **+ 添加组件** 并在搜索浮层输入新名称（或使用工具栏 **编译脚本**、从外部 IDE 修改保存 `.cs` 切回窗口触发 `WM_ACTIVATEAPP`）时，`EditorScriptWorkflow::StartAsyncHotReload()` 在后台无窗口执行 `dotnet build`（共享 `out/Editor/HotReload/obj/` 增量缓存，输出至独立 `bin-<seq>/` 目录）。

主循环 `PollHotReload()` 检测到编译成功后，调用托管回调 `ScriptBindings.ReloadAssembly()` 从内存流加载新 DLL，原地刷新脚本注册表并保留已有绑定字段值；若存在待挂载脚本则自动附加到选中实体。脚本绑定的增删改与场景修改共用 `Ctrl+S`（`EditorCommandId::SaveAll`）和统一撤销栈；运行预览（Play Mode）期间修改暴露字段会经 `ScriptBindings.TrySetLiveField()` 实时写入存活实例，停止预览时自动还原编辑态快照。

## F_Behaviour

`F_Behaviour` 是所有 C# 游戏脚本的抽象基类。`ScriptRegistry` 扫描程序集中的公开非抽象无参构造派生类，优先读取 `[F_Script(id)]` 特性，缺省时以类型全名作为脚本 ID。`ScriptSystem` 负责管理实例附件队列，并在主循环对应阶段依次调度以下 14 个虚方法：

| 阶段 | 回调签名 | 触发时机 |
| :--- | :--- | :--- |
| 初始化与启用 | `F_Awake()` / `F_Enable()` / `F_Start()` | 实例挂载时执行一次 `F_Awake`；启用时调 `F_Enable`；首次进入更新前调一次 `F_Start`。 |
| 固定物理步 | `F_FixedUpdate(float fixedDelta)` | 按固定物理步长推进，适合刚体施力与角色控制。 |
| 逐帧更新 | `F_PreUpdate(float delta)` / `F_Update(float delta)` / `F_LateUpdate(float delta)` | 每渲染帧依次执行（`delta` 受 `F_Time.TimeScale` 缩放）；相机跟随宜放在 `F_LateUpdate`。 |
| 物理碰撞与触发器 | `F_CollisionEnter/Stay/Exit(in F_Contact)` / `F_TriggerEnter/Stay/Exit(F_Entity)` | 实体刚体发生碰撞或进出触发器体积时派发。 |
| 禁用与销毁 | `F_Disable()` / `F_Destroy()` | 组件禁用、实体销毁或停止运行预览时逆序清理。 |

`F_Behaviour` 直接持有宿主实体 `Entity`、局部/世界变换读写属性 `Transform`，以及子系统入口 `Input`、`Actions`、`Physics`、`Audio`、`Particles`、`Navigation`，并提供转发快捷方法 `Translate`、`Rotate`、`LookAt`、`GetComponent<T>()`、`TryGetComponent<T>()`、`GetComponents<T>()`、`Instantiate(...)` 与支持延时秒数的 `Destroy(target, delaySeconds)`。

## ScriptBindings

`ScriptBindings` 维护项目 `Scripts/*.bindings.json` 绑定文档，记录稳定实体 ID、脚本类型 ID、`Enabled` 状态与暴露字段键值表。`ScriptComponentBinding` 将托管绑定桥接到原生世界生命周期，在实体重建或预制体刷新时按 `StableEntityId` 重新定位目标。

### ReloadAssembly

`bool ReloadAssembly(string assemblyPath, out string error)` 从磁盘将新编译的程序集与符号文件读入内存流（`LoadFromStream`），在全新的可回收 `ProjectContext` 中反射脚本类型与 `[F_Expose]` 字段描述，校验已有绑定兼容后替换旧上下文并释放旧程序集引用，使磁盘 DLL 全程零文件锁。

## 事件与 UI 动作

`ScriptEvents` 登记强类型事件载荷并在托管脚本与原生事件总线之间编解码；行为内调用 `Subscribe<T>(handler)` 返回 `IDisposable` 订阅令牌，实例销毁时自动解绑。`UiActionBindings` 按稳定实体 ID、脚本 ID 与动作名分发游戏 UI 点击/值变更事件，`ScriptCommands` 按命令键路由外部命令。
