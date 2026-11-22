# Flare 开发者文档

欢迎来到 **Flare Engine 开发者文档**。这份文档面向希望**阅读源码、理解渲染与运行时机制、扩展编辑器或参与引擎开发**的读者。

::: tip 如果你想直接做游戏
切换到右上角的 [**使用者文档**](user/index.md)，里面有五步上手教程、编辑器快捷键与可直接复制运行的 C# 脚本示例。
:::

## 30 秒看懂引擎整体架构

Flare Engine 严格遵循自底向上的**五层单向依赖架构**（高层可依赖底层，底层绝不反向引用高层）：

```text
+-------------------------------------------------------------------------+
| ToolLayer      FlareEditor 黑曜金编辑器 · 离线烘焙器 · H.264 视频编码器     |
+-------------------------------------------------------------------------+
| FunctionLayer  World / 预制体 · Forward+/DXR 渲染器 · 物理 · 导航 · 音频   |
|                粒子 · 自研 UI 单栈 · .NET 10 C# 宿主与 F_ 游戏 SDK         |
+-------------------------------------------------------------------------+
| ResourceLayer  FBX / glTF 骨骼导入 · HDR 烘焙 · BC 纹理 · DirectWrite 字体 |
+-------------------------------------------------------------------------+
| PlatformLayer  Win32 窗口/输入/手柄 · XAudio2 音频 · DirectX 12 RHI/DXR   |
+-------------------------------------------------------------------------+
| CoreLayer      数学与包围盒 · 任务队列 · 双语本地化 · 哈希 · 日志与计时     |
+-------------------------------------------------------------------------+
```

一帧画面从 [窗口消息与主循环](platform/frame.md) 出发：依次完成输入采样、C# 脚本与物理固定步进、世界数据快照提取，再由渲染器编排 [前向 Pass / FrameGraph](render/pipeline.md) 与可选的 [DXR 1.1 混合硬件光追](render/ray-tracing.md)，最终经 [D3D12 命令提交](render/device.md) 与 [交换链呈现](render/presentation.md) 输出到屏幕。

## 按角色快速切入

| 你想了解什么 | 推荐阅读顺序 |
| :--- | :--- |
| **第一次读源码 / 准备提 PR** | [命名规范](development/命名规范.md) → [从窗口消息到一帧画面](platform/frame.md) → [实体、层级与组件存储](runtime/entities.md) |
| **研究 D3D12 渲染与软阴影 / 光追** | [前向管线与帧图](render/pipeline.md) → [光照着色与多 Pass / ASE Shader](render/materials.md) → [阴影、分簇灯光与环境遮蔽](render/shadows.md) → [DXR 1.1 混合硬件光追](render/ray-tracing.md) → [Mesh Shader 双轨管线](render/mesh-shader.md) |
| **扩展 C# 游戏 API 或热重载宿主** | [C# 行为与零重启热重载](scripts/behaviours.md) → [统一 F_ 游戏 API](scripts/api.md) → [反射、卸载与 IDE 智能提示](scripts/lifetime.md) → [托管宿主与原生 ABI](scripts/bridge.md) |
| **定制编辑器面板、检视器或视口工具** | [编辑器入口与外壳](editor/workspace.md) → [五层属性面板与布局](editor/panels.md) → [编辑会话与运行态隔离](editor/session.md) → [场景视口与 3D 控制柄](editor/viewport.md) |

## 模块目录总览

### 渲染与光照

- [灯光组件](runtime/light.md) · [前向管线与帧图](render/pipeline.md) · [光照着色、材质接口与多 Pass / ASE Shader](render/materials.md)
- [阴影、分簇灯光与环境遮蔽](render/shadows.md)（PSSM、三次 B 样条 PCF、16-Tap Vogel PCSS 与 12-Tap 局部光阴影）
- [环境光照、大气与反射](render/environment.md) · [DXR 1.1 混合硬件光线追踪与 SVGF 降噪](render/ray-tracing.md) · [后处理、抗锯齿与调色](render/post.md)
- [可见性、Hi-Z 与间接绘制](render/visibility.md) · [Mesh Shader 双轨管线与渲染优化](render/mesh-shader.md) · [确定性粒子模拟与 GPU 绘制](render/particles.md)
- [LOD、HLOD 与代理几何](render/detail.md) · [D3D12 设备、命令与 GPU 资源寿命](render/device.md) · [交换链、绘制与读回](render/presentation.md)

### 世界与运行时

- [实体、层级与组件存储](runtime/entities.md) · [预制体格式、实例化与覆盖同步](runtime/prefab.md) · [组件基类与生命周期](runtime/component-base.md)
- [World 的灯光接口](runtime/world.md) · [原生组件反射与属性序列化](runtime/metadata.md) · [场景加载、相机与空间查询](runtime/scenes.md)
- [主循环、任务顺序与事件](runtime/scheduling.md) · [昼夜、天空与场景环境](runtime/daylight.md)
- [刚体、碰撞与角色控制](runtime/physics.md) · [3D 空间音频与 XAudio2 后端](runtime/audio.md) · [导航网格烘焙与角色寻路](runtime/navigation.md) · [骨骼动画与 GPU 蒙皮](runtime/skeletal-animation.md)

### 资产与资源

- [FBX 导入](assets/import.md) · [模型、纹理与材质资产](assets/models.md) · [文件解码、引用与派生缓存](assets/storage.md) · [异步加载、上传与纹理驻留](assets/loading.md)

### C# 脚本与宿主

- [C# 行为、零重启热重载与事件](scripts/behaviours.md) · [高频游戏 API：变换、时间、输入、数学与异步](scripts/api.md)
- [脚本反射、特性元数据、程序集卸载与 GC](scripts/lifetime.md) · [托管宿主与原生 ABI](scripts/bridge.md)

### UI 系统

- [自研 UI 单栈架构、文档与布局](ui/toolkit.md) · [字体图集、文本编辑与 DPI](ui/text.md) · [控件状态、列表与叠层](ui/widgets.md) · [游戏 UI、绑定与合成](ui/game.md)

### 编辑器

- [项目入口、启动进度与编辑器外壳](editor/workspace.md) · [面板模型、默认布局与界面合成](editor/panels.md) · [面板拖出与吸附](editor/docking.md)
- [场景视口、变换手柄与选中描边](editor/viewport.md) · [编辑会话、运行态实时调参与撤销](editor/session.md) · [资产操作、卡片式属性面板与项目文档](editor/documents.md) · [功能与默认编辑器的连接](editor/features.md)

### 平台与分发

- [从窗口消息到一帧画面](platform/frame.md) · [输入、手柄、动作映射与窗口适配](platform/input.md) · [基础服务、任务与诊断](platform/services.md)
- [构建工具、预览与导出](tools/delivery.md) · [构建环境、资源复制与包内启动](tools/package.md) · [固定步连续抓帧与 MP4 编码](tools/frame-capture.md)

---

::: info 文档源码基线与本地预览
本版开发者文档与使用者文档对照仓库主线源码基线（含 commit [**`2318549`**](https://github.com/xuu-xuu-xuu/FlareEngine/tree/2318549) 的 Unity 风格多 Pass 表面着色器与 138 节点 ASE 可视化连线系统）核对实现、行号与界面交互。

在仓库根目录执行以下命令即可在本地实时预览双手册：

```powershell
npm.cmd --prefix Manual ci
npm.cmd --prefix Manual run dev
```
:::
