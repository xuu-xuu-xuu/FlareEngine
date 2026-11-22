# 控件状态、列表与叠层

`EngineUiWidgets` 提供了一套不依赖 Win32 或渲染器的基础控件。每个控件只持有自己的状态与目标 `UiElementHandle`，不直接读全局输入，也不耦合编辑器业务；外壳按帧消费控件产出的变化事件再执行命令或提交属性。

## 控件分类与协作关系

```text
UiDocument（元素树） + UiDispatcher（事件）
   │
   ├── 单值交互控件 ──► UiButton / UiCheckbox / UiCombo
   │                     （状态机 + ConsumeClick() / ConsumeChanged()）
   │
   ├── 虚拟化数据容器 ─► UiScrollView / UiListView / UiTreeView / UiTableView
   │                     （按视口高度 + 上下缓冲行，只生成可见节点）
   │
   └── 布局与全局叠层 ─► UiOverlay（Tooltip/Menu/Dropdown/Modal）
                         UiDragDrop / UiCanvas / UiSplitter / UiTabBar
```

所有基础控件声明均集中在 [`Source/FunctionLayer/UI/Widgets/UiWidgets.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L1)（文本编辑控件 `UiTextEdit` 单独拆分在 [字体图集、文本编辑与 DPI](text.md#uitextedit-与-uiplatformservices)）。

## 单值控件

单值控件把底层指针/键盘动作收束成“可读一次即清除”的语义信号，且所有交互都先检查目标节点是否有效、可见、已启用（`Usable()`）：

- **`UiButton`**（[`UiWidgets.h#L18`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L18)）：维护 `hovered_`、`pressed_`、`clicked_`；仅当按下并在元素内部抬起（`PointerUp(true)`）时置位点击，外壳通过 `ConsumeClick()` 取走。
- **`UiCheckbox`**（[`UiWidgets.h#L38`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L38)）：维护勾选值 `Checked()` 与变化标志 `ConsumeChanged()`。
- **`UiCombo`**（[`UiWidgets.h#L56`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L56)）：维护选项列表、展开状态与选中索引；内置 [`HandleKey()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.cpp#L161) 支持 `Enter/Space/Down` 展开、`Up/Down` 切换选项、`Enter` 确认与 `Escape` 关闭。

## 列表、树与表格虚拟化

编辑器里的层级树、资产目录、属性检查器和控制台动辄包含成百上千行；如果为每一行都在 `UiDocument` 里建元素，布局和绘制开销会随数据规模线性膨胀。`EngineUiWidgets` 用四个轻量类把工作量钉死在**当前可见视口大小**：

- **`UiScrollView`**（[`UiWidgets.h#L81`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L81)）：约束滚动偏移 `[0, max(0, contentHeight - viewportHeight)]`，并直接算出滚动条滑块位置 `ThumbY()` 与高度 `ThumbHeight()`。
- **`UiListView`**（[`UiWidgets.h#L107`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L107)）：根据 `offset_`、`rowHeight_` 与 `viewportHeight_` 计算可见行区间（上下各多留 `bufferRows_ = 2` 行缓冲），仅在 `dirty_` 为真时销毁旧行并重建可见区间内的元素；支持纯文本行（`SetRowTextProvider`）和复合子控件行（`SetRowBuilder`）。

```cpp
// Source/FunctionLayer/UI/Widgets/UiWidgets.cpp
void UiListView::Sync()
{
    if (!dirty_) return;
    dirty_ = false;
    for (const UiElementHandle row : rows_)
        if (document_->Element(row)) document_->DestroyElement(row);
    rows_.clear();

    const UiVisibleRange range = VisibleRange(); // [floor(offset/rowH) - 2, ceil((offset+viewH)/rowH) + 2]
    for (int index = range.first; index < range.last; ++index)
    {
        const UiElementHandle row = document_->CreateElement(rowBuilder_ ? UiElementKind::Panel : UiElementKind::Label);
        document_->SetParent(row, content_);
        UiStyle style;
        style.position = UiPosition::Absolute;
        style.top      = UiLength::Px(static_cast<float>(index) * rowHeight_ - offset_);
        style.width    = UiLength::Percent(100.0f);
        style.height   = UiLength::Px(rowHeight_);
        // UiStyle 默认 background 是不透明黑；列表行必须显式透明，否则整行会被画成黑块。
        style.background = rowBackground_ ? rowBackground_(index) : UiColor{0.0f, 0.0f, 0.0f, 0.0f};
        document_->SetStyle(row, style);
        if (rowBuilder_) rowBuilder_(index, row);
        else document_->SetText(row, rowText_ ? rowText_(index) : "item " + std::to_string(index));
        rows_.push_back(row);
    }
}
```

- **`UiTreeView`**（[`UiWidgets.h#L155`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L155)）：存储树节点与 `expanded` 展开状态，每次增删或折叠时展平为 `VisibleFlat()` 索引数组，直接喂给 `UiListView` 做虚拟化渲染。
- **`UiTableView`**（[`UiWidgets.h#L176`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L176)）：按行列维度和行高计算 `VisibleRange()`（同样带上下 2 行缓冲）。

## 叠层、拖放、页签与画布

| 控件 | 源码位置 | 职责与核心行为 |
| --- | --- | --- |
| `UiOverlay` | [`UiWidgets.h#L196`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L196) | 维护 `Tooltip / Menu / Dropdown / Modal` 四级叠层栈；[`PointerDown()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.cpp#L565) 点击叠层外部时自动关闭菜单/下拉，但 `Modal` 永远拦截输入；`Place()` 自动把弹窗钳位在屏幕可见范围内 |
| `UiDragDrop` | [`UiWidgets.h#L228`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L228) | 跟踪 `None -> Dragging -> Over` 拖放状态机，保存负载 `{type, id}`、幽灵预览框 `Ghost()`、目标元素与接受状态 `Accepted()` |
| `UiCanvas` | [`UiWidgets.h#L262`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L262) | 收集自定义 2D 矢量图元命令（`Line / Rect / Circle / Text`），供视口 Gizmo、曲线或调试面板直接提交到 `UiPaintSink` |
| `UiSplitter` | [`UiWidgets.h#L301`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L301) | 维护双区分割比例 `ratio_` 与两侧最小像素下限，拖拽 `Drag(deltaPixels, totalPixels)` 时自动夹紧 |
| `UiTabBar` | [`UiWidgets.h#L320`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L320) | 维护页签标题、当前选中项以及 `Select / Close / Move` 重排逻辑 |

这些基础部件在 [编辑器工作区外壳](../editor/workspace.md) 中被组合成层级、资产、检查器、控制台与多停靠页签，而跨区拆合与浮动窗口的拓扑变更则交给 [EditorDock 布局树](../editor/docking.md#editordock) 处理。

::: tip 列表数据变了但行数没变时，记得显式调 `Invalidate()`
`UiListView::Sync()` 为了避免每帧重建 DOM，只在 `itemCount_`、`rowHeight_`、`viewportHeight_`、`offset_` 或回调变更时置 `dirty_ = true`。如果你只修改了某几行的文字或高亮色但总行数不变，请手动调用 `listView.Invalidate()` 触发下一次同步。
:::

::: warning 注意 `UiStyle` 的默认背景色
`UiStyle::background` 默认值为 `{0, 0, 0, 1}`（不透明黑），而不是全透明。动态创建自定义容器或列表子项时，如果不希望遮挡父背景，记得显式把 `background.a` 设为 `0.0f`。
:::
