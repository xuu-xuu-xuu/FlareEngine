# 资产操作、卡片式属性面板与项目文档

本文梳理编辑器多文档注册与外部宿主桥接、资产引用级移动/删除、卡片式 Inspector 与算式求值、材质/预制体专用工作流以及项目设置文档的协同关系，位于 **ToolLayer / Editor** 文档与工作流层。

## 多文档工作流总览

```text
                       EditorWorkspaceHost (非 UI 宿主文档总管)
     ┌──────────────┬───────────────┬──────────────┬─────────────────┬─────────────────┐
     ▼              ▼               ▼              ▼                 ▼                 ▼
SceneFileWorkflow  EditorMaterial  UiDocument     EditorSettings    EditorExternal    EditorDocument
 (场景 .scene)     Workflow        Workflow       Document          Documents         Registry
                   (.material)     (.ui.json)     (project/build)   (托管脚本绑定等)   (修订/脏/外部时间戳)
     │              │               │              │                 │                 │
     └──────────────┴───────────────┴──────────────┴─────────────────┴─────────────────┘
                                                   │
                 ┌─────────────────────────────────┴──────────────────────────────────┐
                 ▼                                                                    ▼
   SaveAllDocuments() (Ctrl+S 统一保存)                              WriteRecoveryGroup() (崩溃恢复组)
```

## 文档注册表与外部文档协议

### `EditorDocumentRegistry`

[`flare::EditorDocumentRegistry`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorCommand.h#L59) 统一追踪所有已打开文档的身份、编辑修订号（`editRevision`）、已保存修订号（`savedRevision`）与磁盘文件时间戳，不介入各领域模型内部的撤销实现：

- `Dirty(id)` 判定为 `editRevision != savedRevision`；撤销回到保存点调用 `RevertTo(id, revision)` 后可自动恢复干净。
- `HasExternalChange(id)` 比对 `fileSize` 与 `fileTime`，提供 `MarkReloaded()`、`AcceptExternal()`、`SaveAs()` 三种外部冲突出口。
- `SaveAll(callback)` 逐一调用各文档保存器，成功者清脏、失败者保持脏并返回逐项 `EditorDocumentSaveResult`。

### `EditorExternalDocuments`

[`flare::EditorExternalDocuments`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorExternalDocuments.h#L13) 为托管脚本绑定（`.bindings.json`）等领域文档提供三个回调槽，接入 [`EditorWorkspaceHost`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorWorkspaceHost.h#L31) 的统一保存、关闭保护弹窗与崩溃恢复组：

```cpp
// Source/ToolLayer/Editor/EditorExternalDocuments.h#L13-L19
struct EditorExternalDocuments
{
    std::function<bool(std::vector<EditorExternalDocument>&, std::string&)> snapshot;
    std::function<bool(const std::string&, std::string&)> save;
    // apply=false 只校验候选不改当前文档；apply=true 仅恢复内存不覆盖磁盘原文件
    std::function<bool(const RecoveryDocument&, bool apply, std::string&)> restore;
};
```

## 资产引用分析与原子操作

[导入服务](../assets/import.md#assetimportservice)将外部模型、贴图与音频带入项目后，[`EditorAssetActions`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorAssetActions.h#L54) 负责所有磁盘结构变更的安全性：

| 接口 | 源码位置 | 职责说明 |
| --- | --- | --- |
| `Plan()` | [`EditorAssetActions.h#L57`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorAssetActions.h#L57) | 移动或删除资产前生成 `EditorAssetActionPlan`，扫描磁盘及内存中的 `Scene`、`Material`、`Ui`、`Bindings`、`Prefab` 五类文档并列出全部引用、sidecar 与待重写 `project.json` |
| `Execute()` | [`EditorAssetActions.h#L58`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorAssetActions.h#L58) | 校验摘要未漂移后原子移动/删除资产文件并同步 `project.json` 与 `AssetStore` |
| `ValidatePlayAssets()` | [`EditorAssetActions.h#L61`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorAssetActions.h#L61) | 进入 Play Mode 前校验场景与 UI 所引用资产（优先使用内存编辑草稿）的完整性 |
| `SyncProjectAssets()` | [`EditorAssetActions.h#L64`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorAssetActions.h#L64) | 导入或复制资产后单独同步项目目录清单声明 |

## 卡片式 Inspector、算式求值与跨面板拖拽

[`EditorInspector.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L539) 将每个组件与 C# 脚本渲染为带缩进圆角背景、按前缀区分色条（脚本绿、变换蓝、灯光/相机金、网格青、预制体钴蓝、扩展紫）与折叠开关的独立卡片：

- **递归下降算式求值器（[`MathExprParser`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L108) / [`EvaluateNumericInputIfFormula()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L284)）**：所有数值框原生支持 `+ - * / % ^`、括号、常量 `pi / tau / e`、函数 `sin/cos/tan/asin/acos/atan/atan2/sqrt/abs/floor/ceil/round/deg/rad/pow/min/max`，以及 `+= -= *= /=` 相对增量（如 `360/8`、`deg(pi/3)`、`+=2.5`）。
- **富属性控件**：根据 `EditorProperty` 自动渲染 RGB/XYZ 轴向数值框（支持标签 `↔` 左右拖拽精调）、`[F_Header]` 分组小标题、`[F_Range(min, max)]` 填充滑条、HSV/RGB 选色浮层、可读实体槽（`◈ 名称 (#ID)` + `Ping` + `⊙` + `×`）和类型校验资产槽。
- **搜索式 `+ 添加组件` 浮层**：点击按钮弹出即时模糊过滤列表（C# 脚本置顶）；输入尚不存在的类名回车即可调用 [`EditorScriptTools::CreateBehaviour()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L355) 生成 `.cs` 文件，并在后台零重启热重载完成后自动挂载到当前实体。
- **跨面板直觉拖拽**：层级拖实体到 Inspector 的 `EntityRef` 槽完成引用绑定；资源拖材质/模型/脚本到 Inspector 槽、层级实体行或场景视口完成赋材质/落场/挂脚本；层级拖实体回资源面板直接触发 `CreatePrefabFromSelection()` 生成 `.prefab.json`。

## 材质编辑工作流（`MaterialEditSession`）

材质编辑采用“纯内存会话 + 工作流包装 + 离线缩略球”三层拆分：

1. **[`MaterialEditSession`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/MaterialEditSession.h#L8)**：持有可变 `MaterialDocument`、已保存编码快照与独立 `undo_` / `redo_` 栈，支持 `BeginEdit()` / `CommitEdit()` / `CancelEdit()` 合并连续调参。
2. **[`EditorMaterialWorkflow`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorMaterialWorkflow.h#L17)**：在 `MaterialEditSession` 外层叠加磁盘路径、资产 ID、`EditorFileStamp` 外部冲突检测与实时渲染热预览 `Preview()`。
3. **[`RenderMaterialThumbnail()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/MaterialThumbnail.h#L16)**：按当前 `baseColor / metallic / roughness` 在 CPU 侧实时渲染 PBR 球体缩略图，供资源列表与材质表单预览。

## 预制体工作流（`PrefabWorkflow`）

[`PrefabWorkflow.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/PrefabWorkflow.h#L1) 承接编辑器内全部 `.prefab.json` 创作与双向覆盖同步（运行时格式详见[预制体格式、实例化与覆盖同步](../runtime/prefab.md)）：

```text
层级实体子树 ──► CreatePrefabFromSelection() ──► 写 .prefab.json + .asset.json + project.json
                                                 并将原实体子树挂接 PrefabLink
                                                           │
拖放/点击实例化 ◄── InstantiatePrefabIntoScene() ◄─────────┘
        │
        ▼ 在场景中修改实例属性
RecordOverrideAfterPropertyEdit()
  ├─ CanonicalPrefabPropertyKey() 规范化别名（如 position.x -> transform.position）
  ├─ 忽略根节点摆位 (position / rotation)，其余与原型比对后登记或自动清除 PropertyOverride
  └─ Inspector 标蓝显示 overridden，支持：
       • 单属性级：RevertPropertyOverride() / ApplyPropertyOverrideToPrefab()
       • 单组件级：RevertComponentOverrides() / ApplyComponentOverridesToPrefab()
       • 整实例级：RevertAllOverrides() / ApplyInstanceToPrefab()
                                                  └─► SyncOpenScenePrefabInstances() 广播刷新同场景所有实例
```

## 项目设置、输入动作与外部 IDE 打开

- **[`ProjectSettingsService`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/ProjectSettingsService.h#L33)**：读写并校验 `project.json`、`build.json`（含视频录制分辨率/帧率/时长与画质档位 `QualityProfiles`）以及输入动作映射表 `Content/Settings/Input.input.json`（`ReadInputActions` / `WriteInputActions` / `SetInputActionBinding`）。
- **[`EditorSettingsDocument`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSettingsDocuments.h#L10)**：为 `project.json` 和 `build.json` 保留原始 JSON 中的未知字段，允许保存尚未全部填完的草稿，在显式 `Save()` 时通过 `CheckSource()` 防止静默覆盖外部修改，并支持 `Snapshot()` / `Restore()` 接入崩溃恢复组。
- **外部代码编辑器**：[`EditorScriptTools::PrepareOpenSource()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorScriptTools.cpp#L699) 与 `OpenSource()` 在打开 `.cs` 前自动同步 `.sln` 与 `FlareEngine.Ide.props`，支持自动探测已安装的 IDE 或自定义参数模板跳转报错行列（脚本反射详见[脚本反射与程序集卸载](../scripts/lifetime.md)）。

::: tip 共享材质一键独立
在资源面板或实体材质槽点开一个被多物体共用的 `.material.json` 时，Inspector 顶部会显示当前场景引用计数并提供 `asset.mat.duplicateAssign` 按钮，一键复制 `*_Instance1.material.json`、同步项目清单并替换当前选中实体的材质槽，避免误改全局共享材质。
:::

::: warning 跨文件保存并非分布式事务
`EditorDocumentRegistry::SaveAll()` 按文档顺序逐一原子替换写入：任一文档（如存在外部冲突的 `project.json`）保存失败时，已成功的其他文档会变为干净状态，失败文档保持脏并把诊断键回报状态栏与控制台。
:::
