# 编辑会话、运行态实时调参与撤销

本文拆解 `EditorSession` 的双世界隔离（`EditWorld` / `PlayWorld`）、多选集合与 `@rel:` 相对增量、连续编辑事务合并、`Ctrl+D` 深克隆（含 `PrefabLink` 与脚本绑定同步）以及 `play.keepEntity` 运行期改参写回编辑态的完整实现，位于 **ToolLayer / Editor** 核心会话层。

## 双世界架构与生命周期

```text
                    EditorSession (统一持有 StableEntityId 选择与撤销栈)
        ┌────────────────────────────────────────────────────────────────────┐
        │                                                                    │
        ▼                                                                    ▼
 EditWorld (editEnvironment_)                                  PlayWorld (play_ · 按需创建)
  • 长期驻留，暂停态，承载场景文档与撤销快照                      • StartPlay() 深拷贝 EditWorld
  • 仅在非 Play 时允许事务提交 / Undo / Redo                     • 独立 Environment、物理、导航、音频、脚本
  • 任意修改通过 Snapshot 推入 undo_ / 清空 redo_                • 运行期间 Hierarchy & Inspector 实时切换至此
        ▲                                                                    │
        │  StopPlay() 释放 PlayWorld：                                        │
        │  ├─ 未勾选 Keep Changes ──► 临时改动自动丢弃，EditWorld 完好无损      │
        └──┴─ 已勾选 play.keepEntity ──► StopPlay 前抓取组件与运行期脚本状态 ──┘
                                         退出后合并为单条撤销写回 EditWorld + .bindings.json
```

[`EditorPlaybackController`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorPlayback.h#L10) 约束状态转移为严格有向图：`Editing → Preparing → Playing ↔ Paused → Stopping → Editing`。进入 Play 前，[`EditorSession::ValidatePlay()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSession.cpp#L374) 校验物理、音频、导航配置并确认场景中至少存在一台激活且世界矩阵有限的相机。

## `EditWorld` 与 `PlayWorld` 隔离及运行态实时调参

[`EditorSession`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSession.h#L40) 全程使用跨世界稳定的 `StableEntityId`（不直接持有单次世界内的 `EntityHandle`）：

- **启动播放 [`StartPlay()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSession.cpp#L404)**：新建独立 `Environment` 实例，直接值拷贝 `candidate->GetWorld() = edit_`，同步场景与 TOD，再执行 `playStarted_` 回调创建物理代理、导航与 C# 托管脚本实例。
- **运行态 `Hierarchy` 实时切到 `PlayWorld`**：[`SyncToolkitHierarchyAssetsAndInspector()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorToolkitSession.cpp#L598) 与 `SceneInspector`、`FeatureInspector` 均按同一规则取当前世界：
  ```cpp
  // Source/ToolLayer/Editor/EditorToolkitSession.cpp#L597-L599
  const World& editWorld = state.session.EditWorld();
  const World& hierarchyWorld = state.session.PlayWorld() ? *state.session.PlayWorld() : editWorld;
  for (const EditorHierarchyItem& item : BuildEditorHierarchy(hierarchyWorld)) { /* ... */ }
  ```
  因此运行期脚本通过 `F_Entity.Create` / `Destroy` 动态增删的实体会实时出现在 **场景层级**，点击即可在 Inspector 中实时改参数或热调存活 `F_Behaviour` 的 `[F_Expose]` 字段。
- **安全停止 [`StopPlay()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSession.cpp#L456)**：若在帧推进中收到停止请求，先挂起 `stopPlayRequested_ = true`，待 `CompletePlayFrame()` 离开帧回调后再执行 `play_->BeginShutdown()`、`playStopping_` 清理与 `play_.reset()`，最后修剪 `selection_` 中仅存在于运行期的临时实体 ID。

## 多选集合与 `@rel:` 相对增量协议

`EditorSession` 维护有序多选集合 `selection_` 与主选 `selected_`（`Select` / `SetSelection` / `ToggleSelection` / `IsSelected`）：

1. **多选交集与混合态**：[`AppendMultiSceneInspector()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/SceneInspector.cpp#L641) 提取所有选中实体的共有属性，剔除单实体专有诊断与组件排序按钮；值不一致的字段标记 `mixedValue = true`（界面显示 `—`）。
2. **`@rel:` 相对增量运算**：在任意多选或单选数值框中输入 `+=10`、`-=2.5`、`*=2`、`/=4`（支持右侧嵌套公式如 `+=360/8`），[`EvaluateNumericInputIfFormula()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L284) 会把输入编码为标准协议字符串 `@rel:<op>:<rhs>`，由 [`ParseSceneNumberOrRelative()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/SceneInspector.cpp#L57)、[`ApplyRelativeNumber()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/FeatureInspector.cpp#L61) 与 [`EditorScriptWorkflow.cpp#L1629`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptWorkflow.cpp#L1629) 分别以**每个实体各自的当前值**为基准独立运算——例如多选 5 个不同高度的物体输入 `+=2`，全部精确上移 2 米而不会被拉平到同一高度。

## 连续编辑事务合并（`BeginEdit` / `CommitEdit` / `CancelEdit`）

拖拽滑块、标签 Scrub、拾色器、视口 Gizmo 或 3D 范围控制柄时，每帧都会高频修改组件值。`EditorSession` 用显式事务把整段连续手势折叠为单条撤销记录：

```cpp
// Source/ToolLayer/Editor/EditorSession.cpp#L293-L311
bool EditorSession::BeginEdit(std::string label)
{
    if (transaction_ || play_) return Fail("Editor.TransactionActive");
    auto command = std::make_unique<Command>();
    command->label = std::move(label);
    if (!Capture(command->before)) return false; // 快照含 scene JSON、多选集合与 eulerCache
    transaction_ = std::move(command);
    return true;
}
bool EditorSession::CommitEdit()
{
    if (!transaction_) return Fail("Editor.NoTransaction");
    if (!Capture(transaction_->after)) return false;
    auto command = std::move(*transaction_);
    transaction_.reset();
    Push(std::move(command));
    return true;
}
bool EditorSession::CancelEdit()
{
    if (!transaction_) return Fail("Editor.NoTransaction");
    if (!Restore(transaction_->before)) return false; // 按 Esc 无损恢复手势开始前的快照
    transaction_.reset();
    return true;
}
```

- 事务激活期间，`Apply()` 仍会立即更新 `edit_` 以保证实时渲染与视口反馈，但跳过 `Push()`；
- `Snapshot` 同时记录序列化场景文本、`selected`、`selection` 与 `eulerCache`，因此 `Undo()` / `Redo()` 会连同多选状态和连续欧拉角缓存一起无损还原。

## `Ctrl+D` 深克隆（保留 `PrefabLink` 与 `"CloneMap"` 克隆脚本）

按下 `Ctrl+D`（[`EditorToolkitSession.cpp#L2074`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorToolkitSession.cpp#L2074)）同时克隆实体子树、内部引用、预制体链接与 C# 脚本绑定：

1. **多选顶层过滤**：多选时先过滤掉「祖先也在选中集合中」的子实体（`topRoots`），并用单次 `BeginEdit("Editor.Duplicate")` 包裹所有根的克隆，保证一次 `Ctrl+Z` 完整撤销。
2. **原生子树与 `PrefabLink` 克隆**：[`EditorSession::Duplicate()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSession.cpp#L219) 拷贝 `enabled / tag / layer / staticFlags` 与全部反射组件属性，自动把子树内部的 `PropertyEntityReference` 重映射为新稳定 ID；若被克隆根本身是完整预制体实例根（而非局部拆出的子节点），连同 `PrefabLink` 及其 `overrides` 中的实体引用一起复制，并通过 `outRemap` 返回 `旧 ID → 新 ID` 映射表。
3. **托管脚本绑定同步克隆**：拿到 `idRemap` 后立即调用 [`EditorScriptWorkflow::CloneEntityBindings()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptWorkflow.cpp#L1832)，向托管桥接发送 `{"CloneMap": {"<srcId>": "<dstId>", ...}}` 指令，在同一个脚本事务内把原实体的全部脚本实例与已序列化 `[F_Expose]` 字段（含内部实体引用重映射）克隆到新实体。

```cpp
// Source/ToolLayer/Editor/EditorScriptWorkflow.cpp#L1837-L1847
Json mapObj = Json::object();
for (const auto& [srcId, dstId] : idRemap)
{
    if (srcId != 0 && dstId != 0)
        mapObj[std::to_string(srcId)] = std::to_string(dstId);
}
const Json cmd{{"CloneMap", std::move(mapObj)}};
const bool ok = hooks_->scriptApply(0, cmd.dump(), err);
```

## `play.keepEntity` 运行期改参写回编辑态

Play Mode 顶部绿条右侧的 **保留改动（Keep Changes）** 按钮（键 `play.keepEntity`）允许开发者按实体登记“退出播放后保留本次调参结果”，避免边玩边调完光影/手感后还要手工重填：

```cpp
// Source/ToolLayer/Editor/EditorToolkitSession.cpp#L1859-L1896 (精简流程)
// 1. StopPlay() 销毁 PlayWorld 之前，先从存活的 PlayWorld 与托管运行态抓拍快照
for (const StableEntityId keepId : state.assetState.playKeepEntities)
{
    CapturedEntityPlayState snap;
    if (state.session.CaptureEntityPlayState(keepId, snap))
        capturedEntities.push_back(std::move(snap));
    std::string scriptDoc = state.scriptWorkflow.CaptureLiveEntityScripts(keepId);
    if (!scriptDoc.empty())
        capturedScripts.emplace_back(keepId, std::move(scriptDoc));
}
state.assetState.playKeepEntities.clear();
state.session.StopPlay(); // 2. 正常销毁运行世界

// 3. 回到 EditWorld 后，合并写回并各生成单条可撤销记录
if (!capturedEntities.empty())
    state.session.ApplyCapturedPlayStates(capturedEntities, &state.assets);
if (!capturedScripts.empty())
{
    const bool tx = state.scriptWorkflow.BeginEdit();
    for (const auto& [sid, doc] : capturedScripts)
        state.scriptWorkflow.ApplyCapturedEntityScripts(sid, doc);
    if (tx) (void)state.scriptWorkflow.CommitEdit();
}
```

[`ApplyCapturedPlayStates()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSession.cpp#L773) 在写回前会通过 `edit.CanResolveReference()` 清理指向运行期临时生成实体的悬空引用，防止污染 `EditWorld`。

## 场景文件、外部变更与崩溃恢复

| 模块 | 源码位置 | 职责说明 |
| --- | --- | --- |
| `SceneFileWorkflow` | [`SceneFileWorkflow.h#L9`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/SceneFileWorkflow.h#L9) | 管理场景打开、另存、未保存提示（Save / Discard / Cancel）与保存前外部冲突决策（Reload / SaveAs / Cancel） |
| `ExternalChangeMonitor` | [`ExternalChangeMonitor.h#L20`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/ExternalChangeMonitor.h#L20) | 仅观测当前打开文档的 `mtime / size` 基线（不轮询整仓）；窗口重新激活（`WM_ACTIVATEAPP`）时同步触发 `Scripts/` 变更扫描与零重启增量编译 |
| `EditorSessionStore` | [`EditorSessionStore.h#L31`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSessionStore.h#L31) | 维护进程 `CrashMarker`（检测 `ActiveInstance` / `UncleanExit`）、会话快照与多文档原子恢复组 `WriteRecoveryGroup()`（默认上限 200 MB / 5 修订，60 秒防抖） |

::: tip 新建后立即删除不会误判场景变脏
在 [`EditorSession::Delete()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSession.cpp#L181) 中，如果删除的是“尚未保存过的新建实体”，会自动调用 `edit_.ForgetStableId(id)` 抹去其 `retiredIds` 痕迹，避免撤销栈或删空后留下永久脏标记。
:::

::: warning 撤销栈与脚本撤销协同
场景属性编辑与 C# 脚本字段编辑各自拥有独立事务栈（`EditorSession` 与 `EditorScriptWorkflow`），由 `EditorToolkitSession` 按最近修改来源标志（`preferScriptUndo` / `preferMaterialUndo`）路由 `Ctrl+Z` / `Ctrl+Y`，防止不同文档的撤销顺序互相打架。
:::
