# 字体图集、文本编辑与 DPI

Flare 用同一套基于 DirectWrite 的动态字形图集 `FontCache` 支撑编辑器和游戏 UI 的文字测量、命中、光标定位与渲染，并通过逻辑/物理坐标解耦保证拖动到 125%–200% 高分屏时文字依然清晰锐利。

## 文本测量、图集与 IME 数据流

```text
                        ┌─► UiStubTextService（离线测试/无平台回退，固定字宽）
UiLayoutEngine ──┐      │
UiTextEdit     ──┼──► UiTextService（逻辑像素接口）
UiGeometryBuilder┘      │
                        └─► UiDirectWriteTextService（512 项 LRU 布局缓存）
                                 │  SetRasterScale(dpiScale)
                                 ▼
                            FontCache（DirectWrite 整形 + 数字 run 单字形拆分）
                                 │  1024×1024 只追加多页图集（上限 64 页）
                                 ▼
                            FontPage{alpha, revision} ──► GuiCompositor 按 revision 增量上传 GPU

Win32 HWND (WM_CHAR / WM_IME_* / CF_UNICODETEXT)
   │
   ├─► EditorInputAdapter::Translate()（物理坐标 ÷ dpiScale，还原非文本 VK_PROCESSKEY）
   │        └─► UiInputEvent ──► UiDispatcher ──► UiTextEdit（码点光标/选区/Undo/IME 组合串）
   │
   └─► Win32UiPlatformServices（实现 UiPlatformServices）
            ├─ ReadClipboard / WriteClipboard（UTF-8 ◄─► UTF-16 CF_UNICODETEXT）
            ├─ DpiScale（GetDpiForWindow / 96.0f）
            └─ SetImeCandidateAnchor / CancelIme（IMM32 候选窗定位与取消）
```

## UiTextService

[`UiTextService`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Interfaces/UiTextService.h#L57) 是核心层与布局层唯一可见的文本抽象接口，所有输入输出全部使用**逻辑像素**与 **UTF-8 字节偏移**：

- `Measure(text, fontSize, maxWidth, wrap)`：返回宽高与行数，供 `UiLayoutEngine` 计算文本节点尺寸。
- `LineHeight(fontSize)`：查询行高，用于单行省略、滚动定位与垂直居中。
- `HitTestCaret(...)` / `CaretRect(...)`：实现鼠标点击定位光标与绘制竖线光标/IME 候选窗锚点。
- `LayoutGlyphs(...)`：返回带图集页号与 UV 的 `UiGlyphQuad` 列表，供 `UiGeometryBuilder` 生成顶点。

无平台环境或单元测试使用同文件中的 [`UiStubTextService`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Interfaces/UiTextService.h#L95)（默认每字符 8×16 逻辑像素），无需初始化 DirectWrite 即可验证全部布局与编辑逻辑。

## UiDirectWriteTextService 与 FontCache

Windows 平台的 [`UiDirectWriteTextService`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/UiDirectWriteTextService.h#L17) 默认以 `Microsoft YaHei UI` 驱动底层 [`FontCache`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Fonts/FontCache.h#L33)（位于 `ResourceLayer/Fonts/`），不另起第二套字体栈：

1. **512 项 LRU 布局缓存**：[`UiDirectWriteTextService::Impl::GetLayout()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/UiDirectWriteTextService.cpp#L169) 以 `(textHash, textLength, fontSize, effectiveWidth, wrap, familyHash)` 为键缓存 `TextLayout`，命中时直接把节点移到 LRU 队头，省去每帧重复调用 DirectWrite 排版。
2. **数字 Run 单字形拆解**：在编辑器检查器里拖动数值滑条会产生海量不同数字串。[`FontCache::Impl::DrawGlyphRun()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Fonts/FontCache.cpp#L78) 自动识别由 `0-9 . - + e E` 组成的纯数字 run，把它们拆成单个已整形数字字形分别缓存；而普通中英文则保留整段 run 栅格化，不破坏连字、字体回退与双向文本。
3. **只追加多页图集**：图集每页固定 `1024×1024`（单通道 Alpha，扩展为 RGBA8 上传 GPU），最多扩展到 64 页；已写入的旧字形永不搬家，只递增对应页的 `revision`，[`GuiCompositor`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/GuiCompositor.cpp#L46) 比对 `revision` 后仅重传脏页。

### 高 DPI 切换与图集重建

文字的**布局、光标和命中始终用逻辑像素**，但字形光栅化必须按物理像素密度 `pixelsPerDip` 进行，否则在 150%/200% 缩放屏上放大采样会发糊。当窗口跨屏触发 DPI 变化时，宿主调用 [`UiDirectWriteTextService::SetRasterScale()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/UiDirectWriteTextService.cpp#L389)：

```cpp
// Source/PlatformLayer/Windows/UiDirectWriteTextService.cpp
bool UiDirectWriteTextService::SetRasterScale(float pixelsPerDip)
{
    if (!std::isfinite(pixelsPerDip) || pixelsPerDip < 0.5f || pixelsPerDip > 4.0f) return false;
    if (impl_->rasterScale != pixelsPerDip)
    {
        impl_->rasterScale = pixelsPerDip;
        ClearCache();
        // 以所有页最大修订号推进基线，确保新建图集页不会与旧 GPU 纹理 revision 碰撞。
        const auto& pages = impl_->fontCache.Pages();
        std::uint64_t maxRevision = 0;
        for (const FontPage& page : pages) maxRevision = std::max(maxRevision, page.revision);
        impl_->atlasRevisionBase += maxRevision + 1;
        impl_->fontCache.ClearAtlas();
        impl_->lastError.clear();
    }
    return true;
}
```

主窗口与浮动窗口分别在 [`EditorToolkitSession.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorToolkitSession.cpp#L2761) 和 [`EditorFloatingWindows.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorFloatingWindows.cpp#L264) 响应 DPI 变化并同步刷新 `EditorInputAdapter::SetDpiScale()` 与 `UiDirectWriteTextService::SetRasterScale()`。

## UiTextEdit 与 UiPlatformServices

[`UiTextEdit`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiTextEdit.h#L17) 是单行与多行输入框共用的编辑模型：

- **按 Unicode 码点索引**：内部光标 `caret` 与选区起点 `anchor` 均以码点计数，并在 UTF-8 字节边界安全切分，中英文与 Emoji 混排不会把多字节字符劈断。
- **完整编辑能力**：内置 `Ctrl+A/C/X/V/Z/Y`、方向键/`Home`/`End` 配合 `Shift` 选区、最多 100 步 Undo/Redo 栈，以及基于 `UiTextService` 的 `EnsureCaretVisible()` 自动滚动。
- **IME 组合串隔离**：拼音输入过程中的未提交组合文本放在 `composition_` 中，通过 `DisplayText()` 临时拼接在选区位置显示；调用 `CommitComposition()` 才正式写入文本并入 Undo 栈，失焦时由 `UiDispatcher` 自动触发 `ImeCancel`。
- **系统剪贴板与回退**：[`UiTextEdit::Impl::ReadClipboard()` / `WriteClipboard()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Widgets/UiTextEdit.cpp#L200) 优先调用注入的 [`UiPlatformServices`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/UI/Interfaces/UiPlatformServices.h#L12)，无平台服务时自动回退到进程内内存缓冲，保证纯逻辑单测可跑通复制粘贴。

Windows 端有两个轻量适配器负责桥接 Win32：

| 适配器 | 源码位置 | 职责 |
| --- | --- | --- |
| `Win32UiPlatformServices` | [`Win32UiPlatformServices.h#L13`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/Win32UiPlatformServices.h#L13) | 通过 `OpenClipboard` + `CF_UNICODETEXT` 读写系统剪贴板（UTF-8 ↔ UTF-16 转换）、查询 `GetDpiForWindow(hwnd)/96.0f`、调用 `ImmSetCandidateWindow` / `ImmNotifyIME(CPS_CANCEL)` 控制输入法候选框 |
| `EditorInputAdapter` | [`EditorInputAdapter.h#L15`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInputAdapter.h#L15) | 将 `WM_MOUSE*` 物理像素除以 `dpiScale_` 转为 UI 逻辑坐标；拼接 UTF-16 代理对生成 `TextInput`；解析 `WM_IME_STARTCOMPOSITION / WM_IME_COMPOSITION (GCS_COMPSTR / GCS_RESULTSTR) / WM_IME_ENDCOMPOSITION`；当焦点不在文本框但中文输入法拦截按键返回 `VK_PROCESSKEY (229)` 时，按扫描码还原 `A–Z` 快捷键 |

::: tip 为什么切换中文输入法后视口 WASD / QWER 快捷键依然生效
[`EditorInputAdapter::Translate()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInputAdapter.cpp#L153) 检查 `textInputActive_`：只有焦点真正落在文本框时才让 IME 接管组合；在视口或面板浏览时遇到 `VK_PROCESSKEY`，会从窗口属性或 `MapVirtualKeyExW` 还原原始字母键码，避免必须切回英文输入法才能移动相机。
:::

::: warning 字节偏移与码点索引不要混用
`UiTextService::HitTestCaret()` / `CaretRect()` 接收和返回 **UTF-8 字节偏移**，而 `UiTextEdit::Caret()` / `Select()` 与底层 `TextLayout::HitTest()` 使用 **Unicode 码点索引**。跨层对接时务必转换，避免中文字符光标偏移。
:::
