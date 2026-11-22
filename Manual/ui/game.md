# 游戏 UI、绑定与合成

游戏运行时的 HUD、暂停菜单、血条和输入框由 `FunctionLayer/GameUI`（`UiTree`）驱动，通过 C ABI（`flare_ui_*`）暴露给 C# `F_GameUi` / `F_UiElement`，最终经 `GameUIRenderer` 与 `GuiCompositor` 落到同一套 D3D12 GUI 绘制管线上，既能直接合成到独立游戏窗口，也能嵌入编辑器视口预览。

## 运行时游戏 UI 数据流

```text
C# 玩法脚本 (F_GameUi / F_UiElement / UiActionBindings)
   │  P/Invoke (flare_ui_create / set_text / fade / bind_action ...)
   ▼
UiTree（节点树 + 布局缓存 + 属性过渡 + 事件队列）◄── WindowsUiInput（键鼠/剪贴板/IME）
   │  Visuals() 导出只读快照
   ▼
std::vector<UiVisual>（纯值类型，不借用树指针）
   │
   ▼  GameUIRenderer::Prepare()（布局/文字 FontCache + AssetStore 纹理 + 批次合并）
GuiGeometry（20 字节顶点 + uint32 索引 + Scissor）
   │
   ├─ 独立玩家窗口 ──► GameUIRenderer::Configure() ──► RecordD3D12GuiLayer()
   └─ 编辑器视口   ──► EditorViewportTargetResource（离屏 HDR/LDR）
                          └─► GuiCompositor::Configure() ──► RecordD3D12Gui() ──► SwapChain
```

## UiTree 与 UiEventSystem

[`UiTree`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/GameUI/UiTree.h#L76) 是游戏侧保留模式 UI 树（命名空间 `flare::`），内建 9 种常用游戏控件（`Panel / Label / Image / Button / Checkbox / Slider / ProgressBar / ScrollView / TextInput`）与三种排布方式（`Absolute / Horizontal / Vertical`）：

- **参考分辨率等比缩放**：调用 [`SetCanvasReference(width, height)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/GameUI/UiTree.h#L92)（例如 `1280×720`）后，节点样式仍用设计单位填写，布局时自动按 `canvasScale_` 等比映射到当前物理视口；传 `0, 0` 则直接使用物理像素。
- **只读命中路由**：[`UiEventSystem`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/GameUI/UiEventSystem.h#L7) 只读访问 `UiTree`，不维护第二份 `hover/capture/focus` 状态；[`HitBranch()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/GameUI/UiEventSystem.cpp#L8) 按绘制逆序先检查 `ScrollView` 滚动条轨道，再递归命中子节点，最后检查交互控件或显式开启 `blocksPointer` 的面板。
- **内建属性过渡**：[`UiTransition<Value>`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/GameUI/UiTransition.h#L7) 随 `UiTree::Update(seconds)` 线性推进 `FadeTo()`、`MoveTo()` 和 `ColorTo()`，支持 `CancelTransitions(id)` 原地停住。
- **内置样例**：[`BuildControlsGallery()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/GameUI/ControlsGallery.cpp#L6) 演示全部 9 种控件联动，[`BuildUiTransitionDemo()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/GameUI/UiTransitionDemo.cpp#L6) 演示带滑入/淡出过渡的菜单面板。

## F_GameUi 与脚本动作绑定

C# 侧通过 [`F_GameUi`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L95) 与元素代理 [`F_UiElement`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L42)（同时保留继承子类 `GameUi` / `UiElement` 兼容无前缀写法）操作原生 `UiTree`，底层全部经 [`NativeUiExports.inl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativeUiExports.inl#L1) 导出的 `flare_ui_*` C ABI 桥接：

- **代际防过期（`Epoch`）**：每次 `Load(json)` 重载整棵 UI 树时，`F_GameUi.Epoch` 自增，任何旧的 `F_UiElement` 代理再次调用都会立刻抛出 `EngineException("Bridge.InvalidUi")`，避免误改新树里恰好同号的节点。
- **热重载友好的持久绑定**：UI 节点上持久化的 [`UiEventBinding`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/GameUI/UiTree.h#L24) 只存稳定三元组 `(EntityStableId, ScriptId, ActionName)`，绝不序列化 C# 委托指针；脚本启动时在 [`UiActionBindings::Register()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/UiActionBindings.cs#L19) 登记动作名，热重载后即可无缝重新接管按钮点击。

```csharp
using System.Numerics;
using FlareEngine;

public sealed class HudController : F_Behaviour
{
    private F_UiElement? _button;
    private IDisposable? _actionToken;

    public override void OnStart()
    {
        // 创建按钮并绑定持久化动作名，脚本热重载后依然有效
        _button = Session.Ui.Create(UiKind.Button);
        _button.SetStyle(UiStyle.Default with { Offset = new Vector2(32, 32), Size = new Vector2(180, 44) });
        _button.SetText("开始游戏");

        _actionToken = Scripts.UiActions.Register(this, "StartGame", ev =>
        {
            _button?.FadeTo(0.25f, 0.3);
        });
        _button.Bind(UiEventKind.Click, this, "StartGame");
    }

    public override void OnDestroy() => _actionToken?.Dispose();
}
```

脚本侧动作转发与事件周期细节见 [脚本事件与行为生命周期](../scripts/behaviours.md)。

## GameUIRenderer、EditorViewportTarget 与 GuiCompositor

游戏 UI 与编辑器外壳虽然各自维护不同的节点树，但在进入 GPU 之前会汇流到相同的 `GuiGeometry` 与 D3D12 GUI 管线：

### GameUIRenderer

[`GameUIRenderer`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/GameUIRenderer.h#L17) 只消费纯值快照 `std::span<const UiVisual>`，不借用 `UiTree` 指针：

- **批次合并与容量护栏**：[`Prepare()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/GameUIRenderer.cpp#L60) 将相邻且 `texture / textureIsData / scissor` 完全一致的四边形自动合并为同一个 DrawCall；同时强制限制单帧最多 `65,536` 个 quad、最多 `256` 张图且图片总内存不超过 `128 MB`，超限直接报错 `UI.RenderCapacity` 而非撑爆上传堆。
- **共享字体与资产纹理**：文字通过同一套 [`FontCache`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Fonts/FontCache.h#L33) 光栅化并按页 `revision` 上传；图片节点通过 `AssetStore::LoadTextureImage()` 解析，缺失时自动回退到内置 `missing_` 棋盘纹理。
- **叠加层支持**：[`Configure()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/GameUIRenderer.h#L24) 允许追加额外的调试/性能 `overlays` 列表在场景 UI 之后绘制，不修改 `FrameData`。

### EditorViewportTarget 与 GuiCompositor

在编辑器中，3D 场景先渲染到离屏目标，再作为一张普通纹理嵌进编辑器 UI 面板：

- **[`EditorViewportTargetResource`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/EditorViewportTarget.h#L34)**：拥有离屏 HDR、LDR 与共享深度附件；`Resize()` 成功后才替换新句柄，失败则保留上一有效目标，旧纹理靠 RHI Fence lease 保活至 GPU 读完。
- **[`GuiCompositor`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/GuiCompositor.h#L12)**：统一把 `FrameData::editorGui` 中的不透明纹理 token 映射为真实 RHI 纹理句柄：
  - `FontToken = 1`（字形图集第 0 页）
  - `SceneToken = 2`（3D 视口离屏 LDR 目标）
  - `WhiteToken = 3`（1×1 纯白像素，画纯色矩形/线条）
  - `FontExtraPageBase = 0x200000000`（第 1..63 页扩展字形图集）
  - 其余自定义 token 对应资产预览图/实时图像源，并按 `revision` 增量上传更新。
- **D3D12 录制**：[`PrepareD3D12Gui()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Gui.cpp#L47) 将所有顶点与索引拷入帧上传堆并打包每个 DrawCall 的 SRV 描述符；[`RecordD3D12GuiLayer()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Gui.cpp#L60) 推送 4 个 float 正交投影常量（把逻辑坐标映射到 NDC），按 `framebufferScale` 换算每个命令的物理 `D3D12_RECT` Scissor 并调用 `DrawIndexedInstanced()`；最后 [`RecordD3D12Gui()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Gui.cpp#L88) 将合成结果 `CopyResource` 到当前交换链后缓冲。

## 关键接口与导出速查

| 组件 / C ABI | 位置 | 说明 |
| --- | --- | --- |
| `UiTree` / `UiEventSystem` | [`UiTree.h#L76`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/GameUI/UiTree.h#L76) · [`UiEventSystem.h#L7`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/GameUI/UiEventSystem.h#L7) | 游戏侧节点树、布局、过渡动画与命中路由 |
| `F_GameUi` / `F_UiElement` | [`GameUi.cs#L42`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L42) | C# 运行时 UI 句柄、样式、过渡与事件订阅 |
| `flare_ui_*`（20 个导出） | [`NativeUiExports.inl#L1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativeUiExports.inl#L1) | 跨托管/原生边界的 UI 创建、布局、输入、JSON 读写与动作绑定 |
| `WindowsUiInput` | [`WindowsUiInput.h#L23`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/WindowsUiInput.h#L23) | 游戏窗口键鼠/剪贴板/IMM32 桥接，支持内嵌编辑器 Play 视口偏移 `SetViewport()` |
| `GameUIRenderer` | [`GameUIRenderer.h#L17`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/GameUIRenderer.h#L17) | `UiVisual` → `GuiGeometry` 批处理与 GPU 纹理配置 |
| `EditorViewportTarget` / `GuiCompositor` | [`EditorViewportTarget.h#L11`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/EditorViewportTarget.h#L11) · [`GuiCompositor.h#L12`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/GuiCompositor.h#L12) | 3D 场景离屏目标与最终 GUI 合成 |

::: tip 编辑器内的 Game 视口如何正确接收游戏 UI 点击
在编辑器 Play 模式下，游戏画面被绘制在视口面板的子矩形内。宿主调用 [`WindowsUiInput::SetViewport(rect)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/WindowsUiInput.cpp#L126) 传入面板相对主窗口的客户区矩形，即可自动完成鼠标点击坐标扣偏与 IME 候选窗定位。
:::
