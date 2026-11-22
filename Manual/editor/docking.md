# 面板拖出与吸附

本文说明编辑器多窗口分屏树、五向贴边停靠预览、独立 Win32 浮窗管理与布局持久化（含 `.editor-ui.custom.json`）的实现方式，位于 **ToolLayer / EditorUi** 与 **ToolLayer / Editor** 交界层。把任意面板拖出主窗口后，它仍然共享同一份 `UiDocument` 与编辑器会话；布局树归 [EditorDock](#editordock) 记录，原生窗口与呈现资源由 [EditorFloatingWindows](#editorfloatingwindows) 负责。

## 整体数据流

```text
鼠标拖曳页签 (主窗或任意浮窗)
  │
  ▼
EditorFloatingWindows::RoutePointer()
  ├─ 按屏幕坐标找命中的主窗/浮窗 HWND，换算成目标客户区坐标
  ├─ 命中某窗 ──► EditorShell::SetInputSurface(surfaceId)
  └─ 拖出所有窗外 ──► SetInputSurface(UINT32_MAX)
  │
  ▼
EditorShell::HandleTabMove()
  ├─ 页签栏区 ──► 重排/合并 (DockZone::Merge + tabInsertMarker_)
  ├─ 内容区   ──► ContentDropZone() 五向判定 (Merge / Left / Right / Top / Bottom)
  └─ 复制预览树 EditorDock preview = dock_ -> ApplyTabDrop(preview) -> 精准计算 dropHighlight_
  │
  ▼ 鼠标松开 HandleTabUp()
  ├─ 窗外松手 ──► floatRequest_() -> EditorDock::FloatTab() -> 新建 FloatingDockWindow
  └─ 窗内松手 ──► ApplyTabDrop(dock_) -> EditorDock::MoveTab() / SplitTab() -> Normalize()
  │
  ▼ 帧同步
EditorFloatingWindows::Synchronize()  [创建/销毁/更新 Win32 浮窗 & D3D12 SandboxRenderer]
EditorDockState::Save()               [原子写入 .editor-ui.json (v4) / .editor-ui.custom.json]
```

## EditorDock

[`flare::ui::editor::EditorDock`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.h#L70) 是纯 CPU 分屏布局树，同时管理主窗口布局 `root_`（`surface == 0`）和全部浮动窗口数组 `windows_`（`surface == window.id`，最多 32 个）：

- **二叉分屏树 [`DockNode`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.h#L37)**：每个节点要么是分屏节点（`Kind::Split`，含 `DockAxis::Horizontal / Vertical`、`ratio` 与 2 个子节点），要么是页签组叶子（`Kind::Tabs`，含 `tabs` 列表与 `active` 活跃页签）。
- **空组自动折叠 [`Normalize()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L521)**：关闭或拖走某页签组最后一个面板时，自动把兄弟子树提升替代父分屏节点，并自动销毁已空的 `FloatingDockWindow`。
- **内置预设树**：提供 [`DefaultEditorDockLayout()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L57)（默认 2×3）、[`StandardShelfDockLayout()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L125)（标准下架）、[`WideViewportDockLayout()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L159)（宽视口）以及用于旧版迁移比对的 [`ClassicThreeColumnDockLayout()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L91)。

### 五向贴边停靠与预览一致性

[`EditorShell::ContentDropZone()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellDock.cpp#L728) 将目标页签组内容区划分为中心合并区与四向 22% 边缘分屏带（夹紧在 `24~64 DIP`），并引入 `8 DIP` 滞回余量 `slack` 防止指针在角落微抖时在左右/上下分屏间反复跳变：

```cpp
// Source/ToolLayer/Editor/EditorUi/EditorShellDock.cpp#L728-L757
EditorShell::DockZone EditorShell::ContentDropZone(const UiRect& group, float x, float y, DockZone previous)
{
    constexpr float slack = 8.0f;
    const float contentTop = group.y + kTabHeight;
    const float contentHeight = std::max(0.0f, group.height - kTabHeight);
    const float sideBand = std::clamp(group.width * 0.22f, 24.0f, 64.0f);
    const float endBand = std::clamp(contentHeight * 0.22f, 24.0f, 64.0f);
    const bool canSplitWidth = group.width >= 2 * kMinimumGroupSize;
    const bool canSplitHeight = group.height >= 2 * kMinimumGroupSize;
    struct Edge { DockZone zone; float distance, band; bool allowed; };
    const Edge edges[] = {
        {DockZone::Left,   x - group.x,                sideBand, canSplitWidth},
        {DockZone::Right,  group.x + group.width - x,  sideBand, canSplitWidth},
        {DockZone::Top,    y - contentTop,             endBand,  canSplitHeight},
        {DockZone::Bottom, group.y + group.height - y, endBand,  canSplitHeight},
    };
    const Edge* best = nullptr;
    float nearest = 0;
    for (const auto& edge : edges)
    {
        const float distance = edge.distance - (previous == edge.zone ? slack : 0.0f);
        if (edge.allowed && distance < edge.band && (!best || distance < nearest))
        {
            best = &edge;
            nearest = distance;
        }
    }
    return best ? best->zone : DockZone::Merge;
}
```

拖拽预览时，[`HandleTabMove()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellDock.cpp#L830) 直接拷贝一份临时 `EditorDock preview = dock_` 并执行与最终提交完全相同的 `ApplyTabDrop(preview)`，再用 `DockPanelRect()` 算真实高亮矩形——因此预览面积与松手后的实际落点（含空组折叠后的空间回收）100% 一致。

### `EditorDock` 核心变换接口

| 拖放/操作语义 | 对应接口 | 说明 |
| --- | --- | --- |
| 拖出所有窗口，变为独立浮窗 | [`FloatTab()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L541) | 从原组移除并新建 `FloatingDockWindow`，返回不复用的会话唯一窗口 ID |
| 拖到目标页签栏或内容中心 | [`MoveTab()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L394) | 按 `before` 插入目标页签前/后并激活，随后调用 `Normalize()` |
| 拖到内容区左/右/上/下边缘 | [`SplitTab()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L446) | 在目标叶子位置构造 `Kind::Split`（初始 `ratio = 0.5`）并按 `axis`/`before` 插入新组 |
| 拖拽分屏分隔线 | [`SetSplitRatio()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L585) / [`DragSplitter()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L613) | 按节点路径更新分屏比例并尊重两侧最小尺寸约束 |
| 打开/就近打开/关闭页签 | [`OpenTab()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L334) / [`OpenTabNear()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L378) / [`CloseTab()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L308) | 支持把工具面板就近打开在相邻页签组 |

## EditorFloatingWindows

[`flare::EditorFloatingWindows`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorFloatingWindows.h#L15) 将 `EditorDock::Windows()` 同步为真实 Win32 `WS_EX_TOOLWINDOW` 浮窗，每个浮窗独立持有自己的 `SandboxRenderer`、`UiDirectWriteTextService` 与 `EditorInputAdapter`，但渲染的是同一份 `EditorShell::Document()` 下的不同表面子树（`SurfaceRoot(entry->id)`）：

- **[`Synchronize()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorFloatingWindows.cpp#L79)**：每帧对比 `dock_.Windows()`，自动销毁已移除浮窗、为新增 ID 创建 Win32 窗口（校正可见屏幕边界 `VisibleBounds`、设置黑曜金标题栏与 Per-Monitor DPI），并按浮窗是否承载 `EditorPanelId::Viewport` 决定是否初始化 3D `BasicLit` 渲染场景。
- **[`RoutePointer()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorFloatingWindows.cpp#L191)**：拖拽页签期间把源窗口客户区坐标转换为屏幕坐标，通过 `WindowFromPoint()` 命中鼠标下方的目标编辑器窗口，再转回目标客户区物理像素，实现主窗与任意浮窗间的双向回吸。
- **平滑拖边缩放与 `Esc` 快捷关闭**：收到 `WM_ENTERSIZEMOVE` 时标记 `interactiveResize = true` 并设置窗口属性 `L"FlareInteractiveResize"`，渲染目标尺寸向上对齐到 `512px` 步进（[`EditorFloatingWindows.cpp#L152`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorFloatingWindows.cpp#L152)）避免频繁重建交换链；单页签工具浮窗（设置/材质/导出/关于等）在非文本输入状态下按 `Esc` 可直接关闭（[`#L238`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorFloatingWindows.cpp#L238)）。

## 布局持久化与 `.editor-ui.custom.json`

[`EditorDockState`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDockState.h#L12) 负责把主布局树、所有浮窗边界/内部树与 `maximizeOnPlay` 序列化为 **v4 JSON**（上限 64 KB、最大深度 32、最多 256 节点）：

```cpp
// Source/ToolLayer/Editor/EditorUi/EditorDockState.cpp#L305-L313
std::string EditorDockState::Serialize(const EditorDock& workspace)
{
    Json root;
    root["version"] = 4;
    if (workspace.MaximizeOnPlay()) root["maximizeOnPlay"] = true;
    root["layout"] = ToJson(workspace.Root());
    root["floating"] = Json::array();
    for (const auto& w : workspace.Windows())
        root["floating"].push_back({{"id", w.id}, {"x", w.x}, {"y", w.y},
                                    {"width", w.width}, {"height", w.height},
                                    {"layout", ToJson(w.layout)}});
    return root.dump(2);
}
```

- **原子写盘与备份兜底**：[`EditorDockState::Save()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDockState.cpp#L374) 先写 `.tmp`、复制旧文件为 `.bak`，再以 `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` 同卷原子替换；[`Load()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDockState.cpp#L391) 若主文件损坏自动回退读 `.bak`。
- **平滑版本迁移**：反序列化兼容 v1–v4——未改动的旧版三栏布局自动升级为 2×3 默认布局；v3 及以下若缺失 `Game` 面板，会自动把 `Game` 注入到 `Console` 所在页签组首位。
- **自定义布局预设（`.editor-ui.custom.json`）**：在右上角布局菜单执行保存/载入时，[`EditorShell::ApplyLayoutPreset()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShellChrome.cpp#L1016) 在 `layoutPath_`（`.editor-ui.json`）同目录读写 `.editor-ui.custom.json`，切换预设时会保留当前 `MaximizeOnPlay()` 偏好不丢失。

::: tip 浮窗 ID 单调递增
`EditorDock::nextWindowId_` 在同一编辑器会话内只增不复用（[`EditorDock.h#L113`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.h#L113)），防止在帧边界前关闭旧浮窗并立即新建浮窗时误命中尚未销毁的旧 `HWND`。
:::

::: warning 模态对话框期间冻结浮窗
当主窗口打开关闭保护弹窗或确认框（`CloseModalOpen() || ConfirmOpen()`）时，`EditorFloatingWindows::Synchronize()` 会调用 `EnableWindow(entry->window, FALSE)` 暂时禁用全部浮窗并把前台焦点交回主窗，避免绕过模态守卫修改场景状态。
:::
