# 场景视口、变换手柄与选中描边

本文讲清场景视口如何把键鼠消息转化为相机漫游、射线拾取、W/E/R 变换手柄与 3D 组件范围控制柄，以及 GPU 侧如何绘制无限世界网格与无伪影可见轮廓描边，横跨 **ToolLayer / Editor** 与 **PlatformLayer / Graphics / D3D12** 两层。

## 视口交互与渲染数据流

```text
Win32 键鼠消息
  │
  ▼ EditorInputAdapter::Translate()  [DPI 归一 + 提取 lParam bit30 到 isRepeat]
  │
  ├─ Play 态 & 命中 Game 视口 ──► EditorPlayInput::Process()
  │                                └─ 放行 F5-F8/F11/Ctrl+P 给编辑器，其余转交 Game UI / Gameplay
  │
  └─ Scene 视口 ──► EditorViewportController::Handle() / Update()
                     ├─ 导航手势：Fly (RMB+WASD/QE) / Orbit (Alt+LMB) / Zoom (Alt+RMB) / Pan (MMB/Q)
                     ├─ 拾取：PickEditorBounds() + EditorSceneIconCenter() (同点重复点循环切选图标)
                     ├─ 变换：EditorGizmo (W/E/R + Ctrl 吸附 + 多选跟随) ──► EditorSession 事务
                     └─ 范围柄：ComponentHandleDot (光源/碰撞体/角色) ──► ApplySceneInspector()
  │
  ▼ Prepare() / Draw()
  ├─ 2D 矢量叠层 (overlay_)：相机/光源/音频图标 + 3D 组件拖拽圆点
  ├─ 3D 场景辅助线 (sceneLines_)：视锥、光锥、音频双球、Gizmo 轴线、拖放包围盒
  └─ D3D12SceneOverlay::Record()  [SceneOverlay.hlsl]
      ├─ Pass 1/2：选中模型双通道掩码 (R=完整投影, G=深度可见)
      ├─ Pass 3  ：PSGrid 无限 XZ 世界网格（参与场景深度遮挡）
      ├─ Pass 4  ：PSLine 场景辅助线（被遮挡段按 hiddenAlpha 透视弱显）
      └─ Pass 5  ：PSOutline 邻域采样仅给可见模型外缘描橙金轮廓
```

## 相机操控、`KF_REPEAT` 防误切与视角对齐

[`EditorViewportController`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorViewportController.h#L12) 协调编辑镜头 [`EditorViewportCamera`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorViewportCamera.h#L20)（不写入 `World`，不参与 Play 相机选择），通过 `Gesture` 枚举驱动全部交互：

| 操作 | 内部手势 / 动作 | 行为细节 |
| --- | --- | --- |
| 右键拖拽 + `WASD / QE` | `Gesture::Fly` | 自由视角漫游，`Shift` 加速，飞行时滚轮指数调节漫游速度 `speed_`（`0.05..1000`） |
| `Alt` + 左键拖拽 | `Gesture::Orbit` | 绕当前 `pivot_` 轨道环绕 |
| `Alt` + 右键拖拽 / 滚轮 | `Gesture::Zoom` / `Dolly()` | 沿镜头前向指数推拉；普通滚轮支持 `Shift` 2.5× 加速、`Ctrl` 0.3× 微调 |
| 中键拖拽 / `Q` 工具左键 | `Gesture::Pan` | 按当前景深与垂直 FOV 60° 换算屏幕像素到世界平移量 |
| `F` / `Shift+F` | `FrameSelection()` | 聚焦选中实体包围盒（无选中时聚焦全场景）；`Shift+F` 持续跟随选中体移动 |
| `Ctrl+Shift+F` | `Action("AlignWithView")` | 将编辑相机当前位姿写入选中实体世界变换（自动求父逆矩阵分解局部 TRS，保留原缩放）并入撤销栈 |

### `KF_REPEAT` 防松右键误切工具

右键漫游时玩家经常一直按着 `W / E / Q` 移动，Windows 会持续派发带重复标记（`lParam` 第 30 位 `KF_REPEAT`）的 `WM_KEYDOWN`。若松开右键瞬间尚未抬起 `W/E/Q`，紧随其后的自动重复键极易误把当前工具切回移动/旋转/手掌。Flare 通过两步彻底根除该问题：

1. [`EditorInputAdapter::Translate()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorUi/EditorInputAdapter.cpp#L166) 从 Win32 `lParam` 提取 bit 30 写入 `event.isRepeat`：
   ```cpp
   // Source/ToolLayer/Editor/EditorUi/EditorInputAdapter.cpp#L166
   event.isRepeat = (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
                    ((static_cast<DWORD_PTR>(lparam) & (1ull << 30)) != 0);
   ```
2. [`EditorViewportController::Handle()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorViewportController.cpp#L244) 合并本地按键表与 `ev.isRepeat`，只要是重复键或仍在 `Flying()` 就绝不触发 `Q/W/E/R/X/Z` 工具切换：
   ```cpp
   // Source/ToolLayer/Editor/EditorViewportController.cpp#L244-L250
   const bool repeated = keys_[ev.key] || ev.isRepeat;
   keys_[ev.key] = true;
   if (Flying()) { clickPending_ = false; return true; }
   if (Captured() || ev.control || ev.alt || repeated) return false;
   if (ev.key == 'F') return FrameSelection(s, assets, ev.shift);
   if (ev.key == 'Q' || ev.key == 'W' || ev.key == 'E' || ev.key == 'R' || ev.key == 'X' || ev.key == 'Z')
       return Action(std::string(1, static_cast<char>(ev.key)), s, assets);
   ```

## 变换 Gizmo、吸附与 3D 组件范围控制柄

### `EditorGizmo` 变换手柄与多选跟随

[`EditorGizmo`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorGizmo.h#L14) 负责单轴与等比缩放计算：

- **模式与空间**：`W`（平移）/ `E`（旋转）/ `R`（缩放，含中心白色等比缩放块 `GizmoAxis::Uniform`）；`X` 切换世界/局部空间（`GizmoSpace`），`Z` 切换轴心/包围盒几何中心（`center_`）。
- **网格吸附**：拖动时按住 `Ctrl` 传入 `snap = true`，[`EditorGizmo.cpp#L78`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorGizmo.cpp#L78) 按固定步长量化——**平移 `0.25 m`、旋转 `15°`、缩放 `0.1`**。
- **多选与事务合并**：开始拖拽时调用 `EditorSession::BeginEdit("Editor.Transform")`，若当前为多选，[`EditorViewportController.cpp#L301`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorViewportController.cpp#L301) 会过滤掉「祖先也在选中集合中」的子实体避免双重叠加；松手时顺带调用 `RecordOverrideAfterPropertyEdit()` 登记 Prefab 变换覆盖，再 `CommitEdit()` 合成单条撤销记录（中途按 `Esc` 则 `CancelEdit()` 无损回滚）。

### 3D 场景辅助线与可拖拽组件控制柄

- **场景辅助线（[`EditorSceneGizmos.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorSceneGizmos.cpp#L252)）**：为相机绘制视锥体（按真实近远平面反投影避免 NDC 量化误差），为点光/聚光/方向光绘制衰减球、内外光锥与照射箭头，为矩形光绘制发光面与朝向箭头，为 3D `AudioSource` 绘制最小/最大衰减双球（`DrawAudioSource`），并在图标上方保留 22px 偏移锚点方便拾取。
- **3D 交互式组件范围控制柄（[`AppendComponentHandles()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorViewportController.cpp#L493)）**：选中带以下组件的实体时，在 3D 范围边界绘制可拖拽高亮圆点，拖动期间按 `Alt` 进入 `0.1×` 精调、按 `Shift` `2.5×` 加速，松手合并为单条 `EditorUi.ViewportHandle` 撤销记录：

| 组件类型 | 可拖拽控制柄 | 实时驱动属性键 |
| --- | --- | --- |
| `Light`（Point） | 6 个轴向球面点 | `light.range` |
| `Light`（Spot） | 锥顶距离点 + 4 个外锥缘点 | `light.range`、`light.outerAngle` |
| `PhysicsBodyComponent` | 局部 +X/+Y/+Z 半长轴点 | `body.halfExtents.x / .y / .z` |
| `ColliderSettingsComponent` | 盒体三轴半长轴点 / 球体三轴半径点 | `collider.halfExtentsMeters.*` / `collider.radiusMeters` |
| `CharacterSettingsComponent` | 胶囊半径点 + 顶部半球高度点 | `character.radius`、`character.segmentHalfHeight` |

## `EditorPlayInput` 播放输入隔离

[`EditorPlayInput`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorPlayInput.h#L20) 守护 Play Mode 下的键鼠归属：仅当指针命中且激活 **摄像机视口（`Game`）** 且未被弹窗/菜单遮挡时，才将消息拦截给游戏 UI 与脚本；在 [`EditorPlayInput.cpp#L158`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorPlayInput.cpp#L158) 中显式放行 `F5 / F6 / F7 / F8 / F11` 与 `Ctrl+P` 给编辑器，确保游戏运行期间随时可用快捷键暂停、单步、停止或切换全屏。点击视口外或窗口失焦（`WM_KILLFOCUS`）立即 `Deactivate()` 释放捕获。

## 固定世界网格与双通道可见描边

GPU 叠层由 [`D3D12SceneOverlay::Record()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12SceneOverlay.cpp#L109) 配合 [`Resources/Shaders/SceneOverlay.hlsl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/SceneOverlay.hlsl#L1) 实现：

- **无限固定世界网格（[`PSGrid()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/SceneOverlay.hlsl#L41)）**：用 `worldFromClip` 把屏幕像素反投影为近/远裁剪面射线并与 `y = 0` 平面求交，利用 `fwidth(gridPoint.xz)` 做抗锯齿，固定绘出 **1 m 细格**、**10 m 主格** 与红 X / 蓝 Z 世界轴，输出交点 `SV_DEPTH` 与场景深度自然遮挡。
- **双通道模型轮廓（[`PSOutline()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/SceneOverlay.hlsl#L64)）**：在独立 `Rgba8` 掩码图（超采样时自动匹配 2× 网格）中分两遍渲染选中网格——**R 通道**不开启深度测试写完整投影，**G 通道**复用场景深度写实际可见像素。后处理描边仅在「当前像素 G 可见，且圆形邻域内存在 R 通道缺失」时输出橙金色轮廓，并跳过越出视口纹理边界的邻域采样：

```hlsl
// Resources/Shaders/SceneOverlay.hlsl#L74-L89
int radius = (int)ceil(options.y);
bool edge = false;
[loop] for (int y = -radius; y <= radius; ++y)
    [loop] for (int x = -radius; x <= radius; ++x)
    {
        if (x * x + y * y > radius * radius) continue;
        int2 qBase = (p + int2(x, y)) * scale;
        // 视口裁剪边不是模型轮廓，避免给屏幕四框描边
        if (any(qBase < 0) || qBase.x + scale - 1 >= (int)width || qBase.y + scale - 1 >= (int)height) continue;
        float maskSum = 0;
        [loop] for (int sy = 0; sy < scale; ++sy)
            [loop] for (int sx = 0; sx < scale; ++sx)
                maskSum += selectionMask.Load(int3(qBase + int2(sx, sy), 0)).r;
        if (maskSum < scale * scale - .5) edge = true;
    }
if (!edge) discard;
return float4(ToLinear(float3(1, .6, .08)), 1);
```

这样既不会在前景遮挡物交界处画出错误的内部描边，也不会在物体半出屏幕时沿着视口边缘画出伪影线。

::: tip 拖拽窗口边缘的平滑呈现
主窗口与浮窗在 `WM_ENTERSIZEMOVE` 期间会设置 `interactiveResize = true` 与窗口属性 `L"FlareInteractiveResize"`（[`Window.cpp#L79`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/Window.cpp#L79)、[`D3D12Presentation.cpp#L1497`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Presentation.cpp#L1497)），呈现层采用立即呈现与 512px 步进填充策略，消除拉拽窗口边框时的卡顿与闪烁。
:::

::: warning 投影变化自动取消手柄
如果正在拖拽变换手柄期间发生视口尺寸变更或相机投影突变，`EditorViewportController::Prepare()` 会通过 `gizmo_.ProjectionMatches(snapshot_)` 检测到失配并立即 `Cancel(s)` 回滚当前事务，防止射线解算发散把物体拖飞。
:::
