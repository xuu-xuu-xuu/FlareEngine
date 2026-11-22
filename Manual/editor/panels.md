# 面板模型、默认布局与界面合成

本文拆解自研编辑器外壳 `EditorShell` 的 2×3 布局、五层 GameObject `Inspector` 架构、行内重命名状态机、跨面板精准拖拽高亮、Win32 剪贴板桥接以及 `FormPanel` 提交流程，位于 **ToolLayer / EditorUi** 层。

## 默认 2×3 布局与只读上下文

[`DefaultEditorDockLayout()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L57) 采用贴合创作直觉的 **2×3** 分屏结构——左侧占 `46%` 宽为上下双视窗，右侧占 `54%` 宽为三列通高工作区：

```text
+-------------------------------+------------+----------------+--------------------+
|  Viewport | UiDesigner        |            |                |                    |
|  (场景视口 · 62% 高)           | Hierarchy  |    Assets      |     Inspector      |
+-------------------------------+  (场景层级) |    (资源)      |     (属性面板)     |
|  Game | Console (+ShaderGraph)|  右侧 27%  |  余下 43%      |     余下 57%       |
|  (摄像机/日志/按需停靠连线图)   |            |                |                    |
+-------------------------------+------------+----------------+--------------------+
           左侧 46%                                   右侧 54%
```

- **数据隔离**：UI 层不直接持有 `EditorSession*`、`World*` 或 `AssetStore*`。只读工作区信息通过 [`EditorContext`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorContext.h#L52) 暴露，各面板数据通过纯值模型同步：[`EditorHierarchyModel`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorPanels.h#L28)、[`EditorAssetsModel`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorPanels.h#L140)、[`EditorInspectorModel`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorPanels.h#L106) 与 `EditorShaderGraphViewState`。
- **摄像机视口（`Game`）**：顶栏提供 **窗口预览** 与 **全屏预览** 两种模式切换，底层映射到 [`EditorShell::MaximizeOnPlay()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShell.h#L179) 与 [`SetGameMaximized()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellChrome.cpp#L877)。Play Mode 在 `Game` 视口独立呈现，不劫持编辑用 `Viewport` 镜头。
- **控制台面板（`Console`）**：顶栏集成 **清空**、**折叠重复**、**运行清空**（`ConsoleClearOnPlay()`）及右侧 **信息 / 警告 / 错误** 计数过滤按钮；双击含 `File.cs(line,col)` 的日志行通过 `consoleActivateHandler_` 调起外部 IDE 精准跳转行列。
- **资源面板（`Assets`）**：支持列表/网格双视图、目录树与面包屑导航、类型过滤与关键词过滤；右上角布局菜单可将自定义分屏树持久化至 `.editor-ui.custom.json`（见[面板拖出与吸附](docking.md)）。
- **外观与保存**：顶部菜单、播放控制与状态栏由 [`EditorChrome`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorChrome.h#L1) 组织，配色取自黑曜金 `FlarePalette`。新场景由 [`CreateDefaultScene()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/SceneDefaults.cpp#L21) 创建默认主相机、方向光、平面与立方体；`Ctrl+S`（`SaveAll`）统一保存场景、脚本绑定与外部文档。

## 五层 GameObject `Inspector` 架构

[`SceneInspector.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/SceneInspector.cpp#L139) 与 [`EditorInspector.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L405) 通过序列化代理 [`EditorProperty`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorPanels.h#L66) 将选中实体严格拆分为五层，并彻底隔离场景全局环境与共享资产：

```text
target == 0 (未选中实体)                     target != 0 (选中 GameObject)
┌──────────────────────────────────┐        ┌───────────────────────────────────────────┐
│ Scene Environment (TOD 资产/时钟) │        │ [Play 绿条] Play Mode 提示 + Keep Changes │
│ (绝不渲染 "+ 添加组件" 按钮)       │        ├───────────────────────────────────────────┤
└──────────────────────────────────┘        │ Layer 1 · 实体头                          │
                                            │   [✓] Name · StaticFlags (#StableId)      │
                                            │   Tag · Layer (0..31)                     │
                                            │   [◆ Prefab 定位 | 还原 | 应用] (若是实例)  │
                                            ├───────────────────────────────────────────┤
                                            │ Layer 2 · Transform 空间锚点               │
                                            │   Space: Local | World + 连续欧拉角缓存    │
                                            ├───────────────────────────────────────────┤
                                            │ Layer 3/4 · 组件栈（按实体槽位顺序渲染）    │
                                            │   [✓] 组件卡片 + ⌖ 视口聚焦 + ⋮ 菜单       │
                                            │   条件显隐 · 物理单位 · 色彩空间 · fix.*   │
                                            ├───────────────────────────────────────────┤
                                            │ [ ＋ 添加组件 ]  (模糊搜索 / 回车新建 .cs) │
                                            ├───────────────────────────────────────────┤
                                            │ Layer 5 · Runtime & Derived Diagnostics   │
                                            │   (默认折叠 · 只读世界矩阵/包围盒/拓扑)     │
                                            └───────────────────────────────────────────┘
```

### 1. 实体头与全局环境隔离

- 选中实体（`id != 0`）时，Layer 1 仅暴露跨子系统世界级元信息：`entity.enabled`、`name`、`#StableId`、`entity.tag`（8 个预置标签并兼容自定义值）、`entity.layer`（`0..31`）与 `entity.staticFlags`（支持 `Auto` 自动推导、`Nothing`、`Everything` 及 `Batching / Navigation / GI / Occluder` 逐位切换）。
- 未选中实体（`id == 0`）时，[`SceneInspector.cpp#L625`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/SceneInspector.cpp#L625) 才展示全局 `SceneEnvironment`（`environment.todProfile` / `environment.preserveTime`），且 [`RefreshInspectorRows()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L439) 明确守卫：`if (inspectorModel_.Target() != 0)` 才生成 `+ 添加组件` 行，避免未选中对象时出现无效按钮。
- 检视 `.material.json` 时，[`EditorToolkitForms.cpp#L460`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorToolkitForms.cpp#L460) 会统计当前编辑世界中引用该材质的实体数量，展示共享提示横幅并提供 `asset.mat.duplicateAssign` 一键克隆独立实例材质并赋给当前选中实体。

### 2. 空间锚点 `Transform` 与连续欧拉角

Layer 2 锁定首位渲染 `Transform`，提供 `Local | World` 坐标系切换（`transform.space`）。当切换为 `World` 且存在父节点时，调用 `TryDecomposeTrs()` 分解世界矩阵；若父级非均匀缩放导致切变不可正交分解，自动锁定旋转/缩放并弹出只读诊断提示 `EditorUi.WorldShearReadOnly`。同时借助 [`EditorSession::CachedEulerDegrees()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSession.cpp#L490) 分别缓存 Local/World 四元数对应的连续欧拉角，消除万向节翻转与 `360°` 跳变。

### 3. 组件栈原语与强依赖守卫

Layer 3 按 `world.Components(entity)` 的真实槽位顺序遍历渲染全部内建组件与扩展 `FeatureComponent`（详见[功能与默认编辑器的连接](features.md)），每张卡片统一提供：

- 标题栏左侧启停复选框（`sectionEnabled`）、右侧 `⌖` 空间手柄聚焦（`spatialGizmoKey`）与 `⋮` 更多菜单（`headerMenuAction`）；
- `Reset` 语义重置、`MoveUp` / `MoveDown` 持久化排序、`Copy` / `Paste Values` / `Paste As New` 组件剪贴板（由 [`EditorSession`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSession.h#L105) 持有）；
- **组件依赖守卫**：添加骨骼角色 `add.actor` 时，[`FeatureInspector.cpp#L503`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/FeatureInspector.cpp#L503) 自动补齐默认 `MeshComponent` 并在状态栏/控制台提示 `Editor.DependencyAutoAdded`；移除 `remove.mesh` 时，[`SceneInspector.cpp#L1008`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/SceneInspector.cpp#L1008) 检测到实体仍挂有 `ActorSettingsComponent` 则前置拦截并报 `Editor.DependencyRequired`。

### 4. 条件显隐、物理单位与内联 `fix.*` 自愈

属性字段通过 `unit`（`m`、`°`、`kg`、`m/s`、`lx`、`lm`、`cd/m²`）与 `colorSpace`（`Linear`）明示物理语义，并根据当前状态动态隐藏无意义字段（例如方向光隐藏阴影/锥角、2D 音频隐藏距离衰减、球形碰撞体隐藏半长轴、渲染关闭总开关时折叠子项）。

发现配置冲突时，直接在该组件卡片内插入 `diagnosticLevel = 2` 告警行并附带一键自愈动作（[`SceneInspector.cpp#L821`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/SceneInspector.cpp#L821)）：

| 自愈动作键 | 触发条件 | 修复行为 |
| --- | --- | --- |
| `fix.solePrimaryCamera` | 无主相机或多个启用主相机 | 将当前相机设为唯一主相机并启用 |
| `fix.meshUseCube` | 网格资产缺失或无法解析 | 替换为内置立方体 `BuiltinCubeAssetId` |
| `fix.meshClearInvalidMaterial` | 材质槽引用了不存在的资产 | 清空失效槽位恢复默认材质 |
| `fix.uniformScale` | 球形碰撞体或角色控制器存在非均匀/零世界缩放 | 取三轴平均缩放写回 `Transform` |
| `fix.audioDistance` | 3D 音频 `minDistanceMeters > maxDistanceMeters` | 自动抬高最大距离为 `min + 1m` |
| `fix.actorClip` | 骨骼动画片段名不存在于模型动画列表 | 切换到模型第一个有效动画片段 |

### 5. 只读诊断区与跨面板桥接（`🔒` / `Ping` / `⊙`）

- **Layer 5 只读诊断**：[`SceneInspector.cpp#L561`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/SceneInspector.cpp#L561) 默认折叠输出 `#StableId`、层级激活态、深度/子数、有效静态态、世界坐标/缩放、世界包围盒、网格子网/顶点/三角数与有效物理模式。
- **`🔒/🔓` 检视锁定**：Inspector 页签栏右侧提供锁按钮（[`SetInspectorLocked()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L2387)），锁定后切选其他实体或资产不会冲掉当前检视目标，方便跨对象拖引用。
- **引用槽 `Ping` 与 `⊙` 作用域选择器**：点击已绑定的实体/资产槽调用 [`PingEntity()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L2419) / [`PingAsset()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L2444) 展开父目录、滚动居中并闪烁高亮 `1400ms`；点击槽右侧 `⊙` 打开 [`OpenScopedPickerPopup()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L1283) 弹出按 `acceptedAssetKind` 过滤的模糊搜索浮层。

## 行内重命名状态机

层级面板（`Hierarchy`）与资源面板（`Assets`）共用一套防误触的慢双击/F2 行内重命名状态机，避免与快速双击（聚焦/打开）或跨面板切焦点冲突：

```text
[空闲 Idle]
   │ 1. 根 Capture 阶段先记录 panelWasActiveBeforePointerDown_ = activePanel_
   │    再调用 FocusClickedContent() 切换焦点
   │ 2. 面板 Bubble PointerDown：
   │    - 若 Δt <= 340ms (kFastDoubleClickMs) ──► 判定为快速双击（聚焦实体 / 打开资产），取消重命名
   │    - 仅当 panelWasActive == true 且单选已选中该项且点在文字区：
   │        wasSelectedOnDown = true，记录 downTick
   ▼
[等待抬起 Armed]
   │ PointerUp 且未发生拖拽 ──► 写入 pendingRename + upTick
   ▼
[防抖计时 Pending]  (EditorShell::Frame() 每帧轮询)
   │ 若拖拽/切面板/改选则立即取消
   │ 当 now - downTick >= 340ms 且 now - upTick >= 80ms：
   ▼
[行内编辑 Editing]  (BeginHierarchyRename / BeginAssetRename)
   │ - 资产重命名用 StemCodepointsForAsset() 仅预选文件名主干，保护后缀
   │ - PaintInlineTextEdit() 维护水平视窗滚动、选区块与 1.5px 光标线
   │ - PlaceInlineTextCaret() 通过 DirectWrite HitTestCaret 精准定位/拖选
   ├─ Enter 或点击框外 ──► End*Rename(true) 提交属性/磁盘重命名
   └─ Esc              ──► End*Rename(false) 放弃修改
```

```cpp
// Source/ToolLayer/Editor/EditorUi/EditorShellPanels.cpp#L44-L75
constexpr std::uint64_t kFastDoubleClickMs = 340;

std::uint32_t StemCodepointsForAsset(std::string_view displayName, bool isFolder) noexcept
{
    if (isFolder) return Utf8CodepointCount(displayName);
    const std::size_t dot = displayName.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return Utf8CodepointCount(displayName);
    return Utf8CodepointCount(displayName.substr(0, dot));
}
```

```cpp
// Source/ToolLayer/Editor/EditorUi/EditorShell.cpp#L444-L452
else if (nowTick >= pendingHierarchyRenameDownTick_ &&
         nowTick - pendingHierarchyRenameDownTick_ >= 340 &&
         nowTick >= pendingHierarchyRenameTick_ &&
         nowTick - pendingHierarchyRenameTick_ >= 80)
{
    const std::uint64_t targetId = pendingHierarchyRenameId_;
    pendingHierarchyRenameId_ = 0;
    (void)BeginHierarchyRename(targetId);
}
```

核心辅助实现位于 [`EditorShellPanels.cpp#L78`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellPanels.cpp#L78)（`PaintInlineTextEdit`）、[`#L135`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellPanels.cpp#L135)（`PlaceInlineTextCaret`）与 [`EditorShellChrome.cpp#L464`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellChrome.cpp#L464)（`panelWasActiveBeforePointerDown_` 快照）。

## 精准单行拖拽高亮与 Win32 剪贴板桥接

### 资产拖到层级面板的单行高亮与空白区禁投

在 [`EditorShellPanels.cpp#L1838-L1865`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellPanels.cpp#L1838) 中，从资源面板拖出不同类型资产到 **场景层级** 时，高亮与鼠标光标严格区分「挂载到实体」和「实例化到场景」两类语义：

```cpp
// Source/ToolLayer/Editor/EditorUi/EditorShellPanels.cpp#L1840-L1864
const bool isEntityAttachAsset = IsAssetPathCompatible(assetDrag_->assetPath, "script") ||
                                 IsAssetPathCompatible(assetDrag_->assetPath, "material");
const bool isSceneCreateAsset = IsAssetPathCompatible(assetDrag_->assetPath, "model") ||
                                IsAssetPathCompatible(assetDrag_->assetPath, "prefab");
if (hoveredEntityRow >= 0 && (isEntityAttachAsset || isSceneCreateAsset))
{
    // 仅裁剪并高亮指针命中的单个实体行，不糊成一整片面板
    const float rowTop = hierContent.y + static_cast<float>(hoveredEntityRow) * kHierarchyRowHeight - hOffset;
    const float clipTop = std::max(hierContent.y, rowTop);
    const float clipBottom = std::min(hierContent.y + hierContent.height, rowTop + kHierarchyRowHeight);
    if (clipBottom > clipTop)
    {
        highlightRect = UiRect{hierContent.x + 1.0f, clipTop,
                               std::max(0.0f, hierContent.width - 2.0f), clipBottom - clipTop};
        showHighlight = true;
        compatible = true;
    }
}
else if (overHierarchy && isSceneCreateAsset)
{
    highlightRect = hierContent; // 仅模型/预制体允许投放到空白区（仅绘制外框）
    showHighlight = true;
    compatible = true;
    outlineOnly = true;
}
assetDragCursor_ = compatible ? EditorDragCursor::Allow : EditorDragCursor::Forbid;
```

拖入 Inspector 的具体资产槽位时，还会按 `IsAssetPathCompatible(path, slotProp.acceptedAssetKind)` 显示绿框允许或红框拒绝，避免类型错配。

### Win32 UTF-8 系统剪贴板桥接（全 11 处 `UiTextEdit`）

[`EditorClipboardPlatformServices`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShell.cpp#L90) 实现 `UiPlatformServices` 的 `ReadClipboard` / `WriteClipboard`：读取 `CF_UNICODETEXT` 转 UTF-8 并剔除 `\r`，写入时转为 UTF-16 `GMEM_MOVEABLE` 放回系统剪贴板，同时保留进程内 `fallback_` 兜底。`EditorShell::PlatformServices()` 统一注入编辑器全部 **11 处** `UiTextEdit`：

| # | 文本输入场景 | 源码位置 |
| --- | --- | --- |
| 1 | 拾色器 R/G/B/Hex 通道编辑框 | [`EditorColorPicker.cpp#L86`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorColorPicker.cpp#L86) |
| 2 | Inspector 属性行内编辑框 | [`EditorInspector.cpp#L360`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L360) |
| 3 | `+ 添加组件` 弹窗搜索框 | [`EditorInspector.cpp#L1080`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L1080) |
| 4 | `⊙` 作用域选择器搜索框 | [`EditorInspector.cpp#L1348`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInspector.cpp#L1348) |
| 5 | 通用 `FormPanel` 行内编辑框 | [`EditorShellForms.cpp#L326`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellForms.cpp#L326) |
| 6 | 场景层级行内重命名框 | [`EditorShellPanels.cpp#L873`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellPanels.cpp#L873) |
| 7 | 资源面板关键词过滤框 | [`EditorShellPanels.cpp#L1325`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellPanels.cpp#L1325) |
| 8 | 资源面板行内重命名框 | [`EditorShellPanels.cpp#L1416`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellPanels.cpp#L1416) |
| 9 | 着色器连线暴露参数名编辑框 | [`EditorShaderGraphSurface.cpp#L652`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/ToolLayer/Editor/EditorUi/EditorShaderGraphSurface.cpp#L652) (`sg.paramNameEdit`) |
| 10 | 着色器连线节点默认值编辑框 | [`EditorShaderGraphSurface.cpp#L653`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/ToolLayer/Editor/EditorUi/EditorShaderGraphSurface.cpp#L653) (`sg.valueEdit`) |
| 11 | 着色器连线 138 节点模糊搜索框 | [`EditorShaderGraphSurface.cpp#L743`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/ToolLayer/Editor/EditorUi/EditorShaderGraphSurface.cpp#L743) (`sg.searchEdit`) |

## ASE 风格可视化着色器面板（`Shader Graph`）

`EditorPanelId::ShaderGraph` 的 `openNear` 默认指向 `EditorPanelId::Console`，在新建/打开 `.fshader.json` 或点击菜单打开时自动停靠在左下方页签组（与 `Game`、`Console` 并列），确保创作时左上方 `Viewport` 里的真实场景模型不被遮挡；同时支持拖出为独立浮动副屏窗口：

- **UI 与文档双层解耦**：画布渲染与手势交互位于 [`EditorShaderGraphSurface.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/ToolLayer/Editor/EditorUi/EditorShaderGraphSurface.cpp)，通过纯值视图 `EditorShaderGraphViewState` 与动作回调 `EditorShell::SetShaderGraphActionHandler` 通信；文档读写、撤销重做栈、拓扑热编译和预览球更新由 [`EditorShaderGraphController.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/ToolLayer/Editor/EditorShaderGraphController.cpp#L276) 承接。
- **平滑连线与交互手感**：连线使用 `UiPaintSink::CubicBezier`（解析法线平滑条带 + 顶点 Alpha 抗锯齿羽化）和抗锯齿 `CanvasCircle` 端口；支持约 `20px` 合法端口磁吸、点击已连输入口拔插改连、`Alt + 左键` / 右键点击连线一键秒断、拖线至空白边缘自动平移、以及松手弹出带 9 大分类筛选标签与滚轮翻页的 **138 节点搜索菜单**并自动接线。
- **双路实时反馈**：每次编辑后自动落盘并推进 `shaderReloadGeneration` 触发主视口真实模型热编译，同时驱动右下角 `112×112` 可拖拽旋转光源的迷你动态预览球，顶部支持一键 **新建配套材质** 与 **打开生成的 HLSL**。

## `FormPanel` 表单提交闭环

材质、项目设置、构建导出、偏好设置与资源工具 5 个面板共用 [`EditorShellForms.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellForms.cpp#L44) 的 `FormPanel`。为防止用户在输入框填完参数后直接滚轮滚动或点按钮导致编辑丢失，[`commitFormInlineEdit`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellForms.cpp#L153) 在四条路径上统一自动提交：

1. **按 `Enter`**（`EditorShellForms.cpp#L334`）：提交并清焦点；按 `Esc` 则丢弃草稿。
2. **失焦 `FocusLost`**（`EditorShellForms.cpp#L327`）：点击表单其他行、保存按钮或切去别的面板时自动提交。
3. **滚轮 `PointerWheel`**（`EditorShellForms.cpp#L178`）：滚动列表前先提交并收起浮置编辑框，避免行错位。
4. **点击动作按钮 `actionButton`**（`EditorShellForms.cpp#L283`）：先提交未落盘的文本修改，再执行导出/烘焙等操作。

::: tip 扩展新的 Inspector 属性行
给自定义或内置组件新增属性时，优先在 `SceneInspector.cpp` / `FeatureInspector.cpp` 中通过 `EditorProperty` 的声明式字段（`section`、`axis`、`unit`、`slider`、`acceptedAssetKind`、`diagnosticLevel`、`fixActionKey`）描述意图，无需修改 `EditorInspector.cpp` 的布局代码。
:::

::: warning 延迟销毁与事件安全
面板重建菜单、下拉选项或停靠页签栏时，不得在 `UiDispatcher` 事件回调中直接 `DestroyElement()`，而应推入 `EditorShell::pendingDestroy_`，由 `EditorShell::Frame()` 在帧边界统一回收。
:::
