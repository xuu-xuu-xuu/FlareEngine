# 自研 UI 单栈架构、文档、样式与布局

Flare 的编辑器和游戏界面都没有引入第三方 IMGUI，而是统一跑在同一套自研 UI 渲染与交互栈上：核心层只维护元素树、样式、布局和输入分发，完全不碰 Win32 或 D3D12；平台能力与最终绘制全部通过轻量接口注入，由渲染层统一输出只读 `GuiGeometry` 上屏。

## 为什么做零 IMGUI 自研单栈

编辑器如果用一套第三方 IMGUI、游戏再维护另一套自研 UI，很快就会出现两套字体图集、两套 DPI 换算、两套 IME/剪贴板适配以及相互打架的渲染提交。Flare 通过硬分层把边界切干净：

- **`EngineUiCore`**（`Source/FunctionLayer/UI/Core`、`Interfaces`）：只依赖 `EngineCore`，禁止 `#include <Windows.h>`、`d3d12.h`、`imgui.h` 或 `EditorSession.h`。
- **`EngineUiWidgets`**（`Source/FunctionLayer/UI/Widgets`）：只依赖 `EngineUiCore` 和 `EngineCore`，提供无平台依赖的控件状态机。
- **`EngineUiRender`**（`Source/FunctionLayer/Rendering/UiPaint.*`）：把 `UiDisplayList` 展平为只读 `GuiGeometry`，纹理与字体通过不透明 `uint64_t` token 和抽象接口解耦。
- **`EngineEditorUi`**（`Source/ToolLayer/Editor/EditorUi`）：用上面的核心与控件搭建多停靠区、浮动窗口与检查器，再由外层宿主接入 Win32 与 D3D12。

这条边界不仅写在文档里，还由 CTest 用例 `UiLayering`（[`Scripts/Check-UiLayering.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Check-UiLayering.ps1#L1)）在每次构建时自动扫描头文件引用与 CMake `target_link_libraries` 依赖方向，任何越界直接失败。

## 一帧 UI 数据流

```text
Win32 消息 (HWND / WPARAM / LPARAM)
   │
   ▼  EditorInputAdapter::Translate()（物理坐标 ÷ dpiScale，提取 isRepeat / IME）
UiInputEvent ──► UiDispatcher::Dispatch()
                    │  ├─ UiHitTest::Hit()（从后往前命中 + overflow 裁剪）
                    │  ├─ Capture ──► Target ──► Bubble 三阶段分发
                    │  └─ 管理 Focus / PointerCapture / Tab 顺序
                    ▼
               UiDocument（代际句柄元素树 + 脏标记）◄── UiBindingTable::Update()
                    │
                    ▼
               UiStyleEngine::Compute()（选择器级联 + (element, state) 缓存）
                    │
                    ▼
               UiLayoutEngine::Layout()（Flex 子集 measure + arrange，逻辑像素）
                    │
                    ▼
               UiPainter::Build() ──► UiDisplayList（实现 UiPaintSink）
                    │
                    ▼  UiGeometryBuilder::Build()（UiTextService + UiImageProvider）
               GuiGeometry（20 字节顶点 + uint32 索引 + Scissor 批次）
                    │
                    ▼
               GuiCompositor::Configure() ──► RecordD3D12GuiLayer() ──► SwapChain
```

## UiDocument

`UiDocument` 是 UI 元素树的所有者，采用**槽位 + 代际句柄（`UiElementHandle{index, generation}`）**管理节点生命周期，防止回调持有已销毁节点造成悬空指针。它刻意不做文件 IO、样式级联或布局，而是通过 `UiDirtyFlags` 与修订号通知下游按需重算。

```cpp
// Source/FunctionLayer/UI/Core/UiElement.h
struct UiElementHandle
{
    std::uint32_t index = 0;
    std::uint32_t generation = 0; // 槽位复用时递增，旧句柄自动失效
    [[nodiscard]] constexpr bool IsValid() const noexcept { return index != 0 && generation != 0; }
};

enum class UiDirtyFlags : std::uint8_t
{
    None      = 0,
    Structure = 1u << 0, // 父子关系或节点增删变化
    Style     = 1u << 1, // 样式、类名或文本变化
    Layout    = 1u << 2, // 触发下一帧重新 measure/arrange
    Paint     = 1u << 3, // 仅重绘，不改布局
    All       = 0x0F,
};
```

- **源码位置**：[`UiDocument`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiDocument.h#L26) · [`UiElementHandle` / `UiDirtyFlags`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiElement.h#L31)
- **JSON 加载**：[`LoadUiDocumentJson()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiDocumentLoader.cpp#L172) 负责把 `version: 1` 的 JSON 树解析到临时候选 `UiDocument`，全部校验通过后整体 `std::move` 替换目标文档，失败不污染旧树。
- **数据绑定**：[`UiBindingTable`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiBinding.h#L23) 支持显式绑定 `Text`、`Visible`、`Enabled` 三个属性；调用 `Update()` 时仅在 getter 返回值真正改变时写回文档并标脏，不做无意义的每帧轮询。

## UiStyleEngine

`UiStyleEngine` 按选择器优先级与交互伪类（`UiInteractionState`：`Hover` / `Active` / `Focus` / `Disabled`）计算每个元素的最终 `UiStyle`，并将结果按 `(UiElementHandle, UiInteractionState)` 缓存。

- 仅当主题切换（`SetTheme()`）或文档 `StyleRevision()` 变化时失效缓存，避免重复级联计算。
- 内联样式通过 64 位位掩码 [`UiStyleMask`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiStyle.h#L166) 和 [`DiffStyleMask()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiStyle.h#L207) 推导，只覆盖与默认值不同的字段。
- `UiTheme::Dark()` 只提供核心通用色板与间距 token；编辑器具体的 Obsidian & Solar Gold 配色放在 Tool 层映射，不污染核心。
- **源码位置**：[`UiStyle`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiStyle.h#L124) · [`UiStyleEngine`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiStyle.h#L383)

## UiLayoutEngine

`UiLayoutEngine` 实现了够用且可预测的 **Flex 子集布局**（`measure` + `arrange` 两阶段），全程在**逻辑像素**空间计算矩形，和显示器的 DPI 缩放因子完全解耦。

- 支持 `UiFlexDirection::Row / Column`、`UiJustifyContent`、`UiAlignItems`、`UiPosition::Relative / Absolute`、`gap`、`padding`、`margin`、`flexGrow`、`flexShrink` 与 `Px / Percent / Auto` 三种长度单位。
- 视口尺寸未变且文档没有 `Structure / Layout` 脏标记时，`Layout(viewportWidth, viewportHeight)` 直接返回 `false` 跳过整棵树遍历；未变子树还会命中内部的 `measureCache`。
- 文本测量通过构造函数注入的 `UiTextService*` 完成，不直接依赖 DirectWrite；离线单元测试可直接传入 `UiStubTextService`。
- **源码位置**：[`UiLayoutEngine`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiLayout.h#L31) · [`UiLayout.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiLayout.cpp#L1)

## UiDispatcher

`UiDispatcher` 负责事件路由、焦点管理（含 `Tab` / `Shift+Tab` 切换）与指针捕获，内部持有只读命中器 [`UiHitTest`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiInput.h#L89)。

```cpp
// Source/FunctionLayer/UI/Core/UiInput.h
struct UiInputEvent
{
    UiEventType type = UiEventType::PointerMove;
    UiEventPhase phase = UiEventPhase::Target;
    UiElementHandle target;
    UiElementHandle currentTarget;
    float x = 0.0f, y = 0.0f, wheel = 0.0f;
    std::uint8_t button = 0; // 0 左键、1 右键、2 中键
    UiKeyCode key = 0;
    bool shift = false, control = false, alt = false;
    bool isRepeat = false;   // 系统长按重复按键标志，过滤误触发的离散工具/切换命令
    std::uint32_t codepoint = 0;
    std::string_view text;
    bool propagationStopped = false;
    bool handled = false;
};
```

关键设计细节：

1. **三阶段事件传播**：[`DispatchPath()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiInput.cpp#L201) 先从根节点向目标执行 `Capture`，再在目标执行 `Target`，最后沿父链回溯 `Bubble`；任意监听器调用 `event.StopPropagation()` 即中止后续传递。
2. **回调期安全快照**：[`DispatchAt()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiInput.cpp#L182) 在派发前把当前节点的监听器拷贝为 `shared_ptr` 列表，并逐个检查节点是否仍在文档中，允许回调内打开菜单或增删控件而不破坏迭代器。
3. **滚轮不锁捕获**：[`Dispatch()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiInput.cpp#L326) 中只有 `PointerMove` 与 `PointerUp` 走指针捕获，`PointerWheel` 始终按当前指针位置重新命中，防止一次异常拖动把滚轮永久锁在旧元素。
4. **重复按键过滤（`isRepeat`）**：[`EditorInputAdapter::Translate()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInputAdapter.cpp#L166) 读取 Win32 `lparam` 第 30 位填充 `event.isRepeat`；下游视口快捷键等离散操作（如 [`EditorViewportController`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorViewportController.cpp#L244)）据此区分首帧按下与系统长按连发，同时文本编辑框仍保留长按退格/方向键连发。
5. **多窗口表面隔离**：多浮动窗口共用同一份 `UiDocument` 时，宿主通过 [`SetInputRoot()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiInput.cpp#L473) 限定当前命中与 `Tab` 焦点遍历的子树根，指针捕获仍可跨表面生效。

- **源码位置**：[`UiInputEvent`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiInput.h#L55) · [`UiHitTest`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiInput.h#L89) · [`UiDispatcher`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiInput.h#L107)

## UiDisplayList

绘制链路分为两步，中间彻底隔离文档指针与 GPU 对象：

1. **`UiPainter::Build()`** 遍历可见节点，调用 [`UiPaintSink`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Interfaces/UiPaintSink.h#L14) 录制值类型命令到 `UiDisplayList`（裁剪栈 `ClipPush/ClipPop`、矩形、圆角边框、九宫格 `NineSlice`、文本、图片、线条与画布图元）。
2. **`UiGeometryBuilder::Build()`** 消费 `UiDisplayList`，调用 `UiTextService::LayoutGlyphs()` 生成字形四边形、调用 [`UiImageProvider::Resolve()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Interfaces/UiImageProvider.h#L17) 把图片名映射为不透明纹理 token，最终产出只读 [`GuiGeometry`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/GuiGeometry.h#L18)（每顶点固定 20 字节：`position + uv + packed sRGB color`）。

```cpp
// Source/CoreLayer/GuiGeometry.h
struct GuiVertex { Vec2 position, uv; std::uint32_t color = 0xffffffff; };
static_assert(sizeof(GuiVertex) == 20);

struct GuiCommand
{
    Vec4 clip; // 左、上、右、下（displayPosition 坐标系）
    std::uint64_t texture = 0;
    std::uint32_t indexOffset = 0, indexCount = 0, vertexOffset = 0;
    bool textureIsData = false;
};
```

- **源码位置**：[`UiDisplayList`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/UiPaint.h#L69) · [`UiPainter`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/UiPaint.h#L97) · [`UiGeometryBuilder`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/UiPaint.h#L138) · [`UiFrameData`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiFrameData.h#L17)

## EditorShell

`EditorShell` 是默认编辑器界面的 CPU 组合根，位于 `flare::ui::editor` 命名空间。它持有 `UiDocument`、`UiLayoutEngine`、`UiStyleEngine`、`UiDispatcher`、停靠树 [`EditorDock`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorDock.h#L70)、菜单工具栏与命令总线，但不拥有任何 GPU 资源，所有实现文件按菜单、检查器、层级、资产和视口职责拆分。

- **源码位置**：[`EditorShell`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShell.h#L54)

### Initialize

`bool Initialize(float width, float height)` 按初始视口尺寸创建编辑器根表面、菜单栏、工具栏、状态栏与 15 个面板槽位，注册默认快捷键并构建初始停靠组。注意本地化表 `SetLocalization()` 必须在 `Initialize()` 之前调用，否则标签将回退英文默认值。窗口尺寸改变时调用 `Resize()`，DPI 变更时调用 `SetDpiScale()` 并由外层同步更新字体光栅缩放（详见 [字体图集、文本编辑与 DPI](text.md)）。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShell.cpp#L322)

### Frame

`void Frame()` 推进单帧编辑器 UI 状态，执行顺序刻意安排以消除事件期野指针和布局抖动：[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShell.cpp#L429)

1. 检查延迟重命名计时（层级/资产二次单击延迟触发内联 `UiTextEdit`）。
2. 统一释放上一帧回调放入 `pendingDestroy_` 的待销毁节点，避免在事件分发中途销毁当前目标。
3. 若 `dockRevision_` 变化则调用 `RebuildGroupViews()` 重建页签栏与分割线。
4. 同步 Console、Hierarchy、Assets、Inspector 四个虚拟化 `UiListView::Sync()`，更新面板内容区、取色器、状态栏与 Tooltip。
5. 调用 `layout_.Layout()` 计算布局，再据此更新滚动条滑块（`UpdateScrollThumbs()`）与 Game 视口工具栏，最后补跑一次 `layout_.Layout()` 消化滑块样式变更，确保空闲帧零脏节点。

## 核心接口速查

| 类型 / 接口 | 所属库 | 核心职责 | 源码链接 |
| --- | --- | --- | --- |
| `UiDocument` | `EngineUiCore` | 代际句柄元素树、父子关系、类名、脏标记 | [`UiDocument.h#L26`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiDocument.h#L26) |
| `UiStyleEngine` | `EngineUiCore` | 选择器级联、伪类状态计算与样式缓存 | [`UiStyle.h#L383`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiStyle.h#L383) |
| `UiLayoutEngine` | `EngineUiCore` | Flex 子集测量与排列、测量缓存 | [`UiLayout.h#L31`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiLayout.h#L31) |
| `UiHitTest` / `UiDispatcher` | `EngineUiCore` | 逆序裁剪命中、三阶段分发、焦点与指针捕获 | [`UiInput.h#L89`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Core/UiInput.h#L89) |
| `UiPaintSink` / `UiImageProvider` | `EngineUiCore` | 绘制命令录制与纹理 token 解析抽象接口 | [`UiPaintSink.h#L14`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Interfaces/UiPaintSink.h#L14) |
| `UiTextService` / `UiPlatformServices` | `EngineUiCore` | 文本测量/整形与剪贴板/IME/DPI 抽象接口 | [`UiPlatformServices.h#L12`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Interfaces/UiPlatformServices.h#L12) |
| `UiPainter` / `UiGeometryBuilder` | `EngineUiRender` | 元素树 → `UiDisplayList` → 只读 `GuiGeometry` | [`UiPaint.h#L97`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/UiPaint.h#L97) |
| `EditorShell` | `EngineEditorUi` | 编辑器多停靠区、面板与快捷键外壳 | [`EditorShell.h#L54`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorShell.h#L54) |

::: tip 扩展新控件或新后端时如何保持分层干净
新增纯 UI 逻辑放在 `Source/FunctionLayer/UI/` 并只依赖接口头；新增平台实现放在 `Source/PlatformLayer/`。改动后运行 `pwsh -File Scripts/Check-UiLayering.ps1`，确认 0 violation 再提交。
:::

::: warning 不要在事件回调里同步销毁正在接收事件的节点
事件回调可能仍在 `Capture / Target / Bubble` 路径中。若需要关闭弹窗或重建分组，参考 `EditorShell` 把待销毁句柄推入延迟列表，在下一帧 `Frame()` 开头统一 `DestroyElement()`。
:::
