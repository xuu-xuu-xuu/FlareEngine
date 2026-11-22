# 功能与默认编辑器的连接

本文说明运行时扩展功能组件（物理、角色、渲染、环境、骨骼、矩形光、音频、粒子、导航）、游戏 UI 设计画布、昼夜曲线预览与离线资源工具如何接入默认编辑器并向渲染/物理后端输出数据，横跨 **FunctionLayer** 与 **ToolLayer / Editor**。

## 端到端数据流

```text
组件反射定义表 (FeatureComponents.h · 11 类扩展组件共用 ComponentBase)
  │
  ├─► 编辑器属性展示：FeatureInspector::AppendSingleFeatureComponent()
  │     • 物理单位 · 条件显隐 · 内联校验与 fix.* 一键自愈 · add.actor 自动补齐 Mesh
  │     └─► 提交修改：ApplyFeatureInspector() ──► EditorSession::SetProperty() / PlayWorld
  │
  ├─► 运行帧提取：ApplyFeatureFrameData() + ApplyTimeOfDayFrameData() + PopulateParticleFrame()
  │     └─► 写入纯 CPU FrameData（光照/阴影/GTAO/SSGI/SSR/雾/DXR/粒子/TOD）
  │
  ├─► 物理代理同步：ScenePhysics::Synchronize() / ApplyTransforms() / AdvanceCharacters()
  │     └─► C# F_Behaviour 通过 GetComponent<F_Rigidbody>() / TryGetComponent<F_CharacterController>() 共用同一代理
  │
  ├─► 游戏 UI 编辑：UICanvasEditor (缩放/拾取/锚点/拖拽) + UiInspector (属性提交) ──► UiTree
  ├─► 昼夜独立预览：TODPreview (独立 Environment 时钟/撤销) + TodInspector ──► .tod.json
  └─► 离线烘焙与参考预览：ExecuteFeatureTool() [后台线程] ──► EditorFeatureTools::Poll() [主线程发布]
```

## 核心连接入口速查

| 子系统入口 | 负责的类型与接口 | 核心职责 |
| --- | --- | --- |
| 扩展组件描述表 | [`FeatureComponent`](#feature-components)、[`AppendEngineFeatureMetadata()`](#feature-components) | 继承 [`ComponentBase`](../runtime/component-base.md)，以固定 `ComponentTypeId`（`0x464C0001..0x464C000B`）统一承载刚体、碰撞体、角色、渲染设置、环境贴图、骨骼角色、矩形光、音频监听/音源、粒子发射器与导航代理共 **11 类**扩展组件 |
| 功能属性适配与帧提取 | [`FeatureInspector`](#feature-inspector)、[`ApplyFeatureFrameData()`](#feature-inspector) | 属性读写经 [`EditorSession::SetProperty()`](session.md) 进入撤销栈（Play 态直写 `PlayWorld`）；运行帧由纯 CPU 提取函数汇总到 `FrameData` |
| 场景物理代理 | [`ScenePhysics`](#scene-physics) | `Synchronize()` 按组件增量建/删物理代理，`ApplyTransforms()` 把世界刚体位姿转回父节点局部坐标，`Clear()` 统一回收 |
| 游戏 UI 设计器 | [`UICanvasEditor`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/UICanvasEditor.h#L10)、[`UiInspector`](#ui-inspector) | `UICanvasEditor` 负责预览缩放拟合、屏幕↔设计坐标换算、控件命中、锚点可视化与移动/缩放手势；`UiInspector` 读写 `UiTree` 样式与撤销历史 |
| 昼夜资产预览 | [`TODPreview`](#tod-preview)、[`TodInspector`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/TodInspector.h#L7) | 独占 `Environment` 时钟、配置与撤销栈，编辑与拖动时间轴绝不污染 Play 世界 |
| 离线资源与参考工具 | [`ExecuteFeatureTool()`](#feature-tools)、[`EditorFeatureTools::Poll()`](#feature-tools) | 后台线程消费只读模型/纹理快照执行烘焙或 CPU 参考预览，主线程 `Poll()` 登记产物并同步 `AssetStore` |

## C# `F_` SDK 与物理代理共用

C# 脚本通过 `GetComponent<F_Rigidbody>()`、`TryGetComponent<F_CharacterController>(out var cc)`（或静态工具 `F_Physics.GetBody(entity)`、`F_CharacterController.FromComponent(entity)`）直接获取组件生成的运行时物理代理。原生桥接与默认编辑器共用同一份 [`ScenePhysics`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/ScenePhysics.h#L12)，不会为同一实体重复创建刚体或胶囊控制器。

## `FeatureInspector` 语义适配细节

[`FeatureInspector.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/FeatureInspector.cpp#L95) 将冷冰冰的反射属性表转化为易读的创作界面：

- **物理单位与资产类型约束**：[`UnitForFeatureProperty()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/FeatureInspector.cpp#L38) 自动附加 `m / kg / s / ° / m/s / cd/m²`；[`AcceptedKindForFeatureProperty()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/FeatureInspector.cpp#L28) 约束 `actor.asset`（model）、`audio.clip`（audio）、`navagent.asset`（navmesh）、`environmentmap.asset`（env）与 `render.diffuseProbe`（probe）的拖拽和 `⊙` 弹窗范围。
- **状态驱动条件显隐**：[`FeatureInspector.cpp#L238`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/FeatureInspector.cpp#L238) 根据当前状态自动隐藏无物理意义的字段（如静态刚体 `mass <= 0` 隐藏阻尼、盒/球碰撞体互隐、2D 音频隐藏衰减距离、渲染设置总开关关闭时隐藏子参数）。
- **内联自愈与强依赖补齐**：检测角色/球体非均匀缩放（`fix.uniformScale`）、3D 音频距离倒挂（`fix.audioDistance`）、失效骨骼动画片段（`fix.actorClip`）；添加 `actor` 组件时自动补齐默认 `MeshComponent`（[`FeatureInspector.cpp#L503`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/FeatureInspector.cpp#L503)）。

## `UICanvasEditor` 画布交互与 `TodInspector` 昼夜编辑

- **[`UICanvasEditor`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/UICanvasEditor.cpp#L17)**：不拥有任何持久化 UI 数据，只管理视图变换与拖拽手势。`Prepare(ui, viewport)` 计算等比适配缩放 `scale_` 与居中画布 `content_`；`Hit()` 逆序遍历 `ui.Visuals()` 同时检查控件矩形与裁剪区；`Anchors()` 根据父级 padding、流式布局前序兄弟偏移与 `ScrollView` 滚动量计算真实锚框屏幕坐标；`Begin()` 在父节点采用非 `Absolute` 流式布局时拒绝移动手势（返回 `EditorUi.CanvasFlowPosition`），缩放手势会自动按 `pivot` 补偿 `offset`，并在视口或缩放突变时返回 `EditorUi.CanvasProjectionChanged` 防止拖歪。
- **[`TodInspector`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/TodInspector.cpp#L15)**：将 `TODPreview` 的日长、日出/日落、太阳方位/仰角以及最多 256 个关键帧（时间、太阳强度、雾密度、Sun/Sky/Fog RGB 颜色）映射为可拖拽数值行，新增关键帧自动插在当前相邻帧的最大时间空隙中点；底部实时只读展示当前预览时间采样出的 Sun/Sky/Fog 颜色。

## 资源烘焙工具与实时渲染开关

[`EditorFeatureTools`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/EditorFeatureTools.cpp#L37) 在「资源与参考工具」面板中按三大工具族组织离线任务：

| 工具族 | 支持操作 | 产物 / 行为 |
| --- | --- | --- |
| `geometry` | `lod`、`hlod`、`meshlets`、`impostor`、`vertices`、`navmesh` | 离线烘焙 `.selod`、HLOD、`.semeshlet`、`.seimp` 代理图集、顶点量化压缩分析报告或导航网格，并自动登记项目资产依赖 |
| `images` | `environment`、`bc1`、`bc4`、`bc5`、`shader` | 预过滤环境贴图、BC 压缩贴图或构建用户 Shader 包（同目录 PBR / Forward / Shadowed / Environment 变体） |
| `reference` | `atmosphere`、`fog`、`probes`、`ssr`、`occlusion` | 仅运行 CPU 参考算法生成 `256×128` 预览图，不替代场景实时 GPU 渲染 |

::: tip 如何开启实时 GPU 高级渲染效果
「资源与参考工具」中的参考预览仅用于离线算法验证；要在场景中启用实时 GPU **GTAO**、**接触阴影**、**SSGI**、**SSR**、**体积雾**、**SM 6.5 Mesh Shader** 或 [DXR 1.1 硬件光线追踪](../render/ray-tracing.md)，请直接选中带 **场景渲染设置**（`RenderSettingsComponent`）的实体勾选对应开关。
:::

::: warning 用户 Shader 包变体兼容性
渲染器按实际 Pass 自动挑选兼容的用户 Shader 变体。如果资产目录中只存在旧 PBR 包，前向管线会输出明确诊断并安全回退默认着色器，避免向不兼容的根签名提交绘制命令。
:::

## 源码接口定义（实时内嵌）

下面的头文件定义直接从当前仓库源码内嵌，随文档构建自动保持同步。

<a id="feature-components"></a>
<details><summary>功能组件与描述表接口（FeatureComponents.h）</summary>

<<< @/../Source/FunctionLayer/Runtime/FeatureComponents.h{cpp}

</details>

<a id="feature-inspector"></a>
<details><summary>属性适配与帧提取（FeatureInspector.h / FeatureFrameData.h）</summary>

<<< @/../Source/ToolLayer/Editor/FeatureInspector.h{cpp}

<<< @/../Source/FunctionLayer/Application/FeatureFrameData.h{cpp}

</details>

<a id="ui-inspector"></a>
<details><summary>UI 属性适配（UiInspector.h）</summary>

<<< @/../Source/ToolLayer/Editor/UiInspector.h{cpp}

</details>

<a id="scene-physics"></a>
<details><summary>场景物理代理（ScenePhysics.h）</summary>

<<< @/../Source/FunctionLayer/Physics/ScenePhysics.h{cpp}

</details>

<a id="feature-tools"></a>
<details><summary>资源任务与编辑器控制器（EditorFeatureTools.h）</summary>

<<< @/../Source/ToolLayer/Editor/EditorFeatureTools.h{cpp}

</details>

<a id="tod-preview"></a>
<details><summary>昼夜独立预览（TODPreview.h）</summary>

<<< @/../Source/ToolLayer/Editor/TODPreview.h{cpp}

</details>
