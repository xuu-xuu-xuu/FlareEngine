# 自研 UI 单栈架构、文档、样式与布局

Flare 编辑器与游戏界面统一建立在自研 UI Toolkit 之上：核心树、样式、布局与事件不依赖平台 API 或第三方 IMGUI，渲染只通过 `UiPaintSink` 输出只读几何与纹理命令。编辑器停靠外壳在 `ToolLayer` 中消费同一套控件与绘制链。

## UiDocument

`UiDocument` 持有带代际句柄的元素树，负责父子关系、属性存取、样式类与数据绑定挂载。[LoadUiDocumentJson()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/UI/Core/UiDocumentLoader.cpp#L172) 将 JSON 文档解析为 `UiDocument`，[UiBindingTable](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/UI/Core/UiBinding.h#L23) 驱动文字、可见性与启用状态同步。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/UI/Core/UiDocument.h#L26)

## UiStyleEngine

`UiStyleEngine` 按选择器优先级与状态伪类计算每个元素的最终样式，输入 `UiDocument` 与样式表，输出每节点解析样式供布局与绘制消费。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/UI/Core/UiStyle.h#L383)

## UiLayoutEngine

`UiLayoutEngine` 依据父子关系、流向、弹性比例与绝对定位计算每个元素的逻辑像素矩形，并与 DPI 缩放因子解耦。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/UI/Core/UiLayout.h#L31)

## UiDispatcher

`UiDispatcher` 依赖 [UiHitTest](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/UI/Core/UiInput.h#L88) 命中布局矩形，统一调度指针悬停/捕获、键盘焦点与 IME 组合文本。基础控件见 [控件状态、列表与叠层](widgets.md)。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/UI/Core/UiInput.h#L106)

## UiDisplayList

`UiDisplayList` 继承并实现 [UiPaintSink](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/UI/Interfaces/UiPaintSink.h#L14)，记录裁剪栈、文字、图像、线条与矩形；[UiGeometryBuilder](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/UiPaint.h#L138) 通过 [UiImageProvider](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/UI/Interfaces/UiImageProvider.h#L17) 解析纹理并输出只读 `GuiGeometry` 供 D3D12 绘制。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/UiPaint.h#L69)

## EditorShell

`EditorShell` 是默认编辑器的单栈界面外壳，持有 [EditorDock](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ToolLayer/Editor/EditorUi/EditorDock.h#L66) 多停靠区与浮动窗口状态、命令路由与属性面板，仅依赖 `EngineUiCore`、`EngineUiWidgets` 与 `EngineCore`，实现文件按面板与菜单职责拆分（层级/资产/检查器/视口与主菜单）。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ToolLayer/Editor/EditorUi/EditorShell.h#L53)

### Initialize

`bool Initialize(float width, float height)` 按初始视口尺寸构建编辑器根文档与停靠布局；尺寸改变时调用 `Resize`，DPI 变更时调用 `SetDpiScale` 重建排版与字形密度。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ToolLayer/Editor/EditorUi/EditorShell.h#L64)

### Frame

`void Frame()` 推进单帧编辑器控件状态、延迟销毁、停靠组重建、列表同步与 `UiLayoutEngine::Layout()` 布局更新；随后由外层宿主通过 `UiPainter` 与 `UiGeometryBuilder` 生成只读 `GuiGeometry` 交由渲染层合成。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ToolLayer/Editor/EditorUi/EditorShell.h#L68)
