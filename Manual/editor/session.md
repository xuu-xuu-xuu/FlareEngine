# 编辑会话、运行态实时调参与撤销

## EditorSession

`EditorSession` 持有编辑世界（`EditWorld`）、当前选择和统一事务历史。层级视图通过 `BuildEditorHierarchy()` 展平父子关系。连续拖动数值框、`[F_Range]` 滑条或视口变换手柄时，用 `BeginEdit()` 与 `CommitEdit()` 将整段手势合并为单个历史项，`Undo()` 与 `Redo()` 同步覆盖原生场景变换、组件属性与 C# 脚本绑定变更。

## 播放世界与运行态实时调参

`EditorSession::StartPlay()` 创建独立的运行环境与 `PlayWorld`，深拷贝编辑世界并同步昼夜、物理、音频与脚本实例后启动运行预览。`EditorPlaybackController` 驱动播放、暂停、恢复、单步和停止；`EditorPlayInput` 依据 **摄像机视口**（窗口预览或全屏预览区域）的焦点和指针捕获状态向游戏转发键鼠输入。

在运行预览（Play Mode）期间，属性面板保持可编辑：修改实体变换、灯光/相机/刚体参数或存活 `F_Behaviour` 的 `[F_Expose]` 字段会实时作用于 `PlayWorld` 与运行期托管脚本实例，方便边玩边调手感与光影；点击 `StopPlay()` 释放播放状态后，所有临时改动自动还原为进入播放前的 `EditWorld` 快照，不会污染已保存场景。

## 场景文档与恢复

`SceneFileWorkflow` 管理场景打开、另存、未保存提示与外部覆盖决策。`ExternalChangeMonitor` 监视场景文件时间戳，而编辑器窗口重新激活（`WM_ACTIVATEAPP`）时同步扫描项目 `Scripts/` 目录变更以触发零重启 C# 增量编译；`EditorSessionStore` 与 `ApplyRecoveryScene()` 负责异常退出后的会话恢复。
