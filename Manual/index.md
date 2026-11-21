# Flare 开发者文档

新增类型、C# 脚本特性或资产字段时，先查阅[命名规范](development/命名规范.md)。如果希望直接学习如何使用编辑器制作项目与编写脚本，可切换至右上角[使用者文档](user/index.md)。

一帧画面从[窗口与主循环](platform/frame.md)开始。运行时提取场景与昼夜/动画数据，渲染器编排[前向 Pass、帧图](render/pipeline.md)与可选的[DXR 1.1 混合硬件光追](render/ray-tracing.md)，最后由[D3D12 命令提交](render/device.md)送到 GPU。编辑器和 `.NET 10` C# 宿主都复用同一套引擎底层接口。

**渲染与光照**

[灯光组件](runtime/light.md) · [前向管线与帧图](render/pipeline.md) · [光照着色、材质接口与 Shader 包](render/materials.md) · [阴影、分簇灯光与环境遮蔽](render/shadows.md) · [环境光照、大气与反射](render/environment.md) · [DXR 1.1 混合硬件光线追踪与 SVGF 降噪](render/ray-tracing.md) · [后处理、抗锯齿与调色](render/post.md) · [可见性、Hi-Z 与间接绘制](render/visibility.md) · [Mesh Shader 双轨管线与渲染优化](render/mesh-shader.md) · [确定性粒子模拟与 GPU 广告牌绘制](render/particles.md) · [LOD、HLOD 与代理几何](render/detail.md) · [D3D12 设备、命令与 GPU 资源寿命](render/device.md) · [交换链、绘制与读回](render/presentation.md)

**世界与运行时**

[骨骼动画与 GPU 蒙皮](runtime/skeletal-animation.md)：从角色组件到姿态采样，以及 `F_Behaviour` 如何播放片段和读取关节世界坐标。

[实体、层级与组件存储](runtime/entities.md) · [预制体格式、实例化与覆盖同步](runtime/prefab.md) · [组件基类与生命周期](runtime/component-base.md) · [World 的灯光接口](runtime/world.md) · [原生组件反射与属性序列化](runtime/metadata.md) · [场景加载、相机与空间查询](runtime/scenes.md) · [主循环、任务顺序与事件](runtime/scheduling.md) · [昼夜、天空与场景环境](runtime/daylight.md) · [刚体、碰撞与角色控制](runtime/physics.md) · [3D 空间音频与 XAudio2 后端](runtime/audio.md) · [导航网格烘焙与角色寻路](runtime/navigation.md)

**资产与资源**

[FBX 导入](assets/import.md) · [模型、纹理与材质资产](assets/models.md) · [文件解码、引用与派生缓存](assets/storage.md) · [异步加载、上传与纹理驻留](assets/loading.md)

**C# 脚本与宿主**

[C# 行为、零重启热重载与事件](scripts/behaviours.md) · [高频游戏 API：变换、时间、输入、数学与异步](scripts/api.md) · [脚本反射、特性元数据、程序集卸载与 GC](scripts/lifetime.md) · [托管宿主与原生 ABI](scripts/bridge.md)

**UI 系统**

[自研 UI 单栈架构、文档与布局](ui/toolkit.md) · [字体图集、文本编辑与 DPI](ui/text.md) · [控件状态、列表与叠层](ui/widgets.md) · [游戏 UI、绑定与合成](ui/game.md)

**编辑器**

[功能与默认编辑器的连接](editor/features.md)：组件、物理代理、资源任务及 UI/昼夜编辑的数据流与源码头文件定义。

[项目入口、启动进度与编辑器外壳](editor/workspace.md) · [面板模型、默认布局与界面合成](editor/panels.md) · [面板拖出与吸附](editor/docking.md) · [场景视口、变换手柄与选中描边](editor/viewport.md) · [编辑会话、运行态实时调参与撤销](editor/session.md) · [资产操作、卡片式属性面板与项目文档](editor/documents.md)

**平台与基础服务**

[从窗口消息到一帧画面](platform/frame.md) · [输入、手柄、动作映射与窗口适配](platform/input.md) · [基础服务、任务与诊断](platform/services.md)

**构建与分发**

[构建工具、预览与导出](tools/delivery.md) · [构建环境、资源复制与包内启动](tools/package.md) · [固定步连续抓帧与 MP4 编码](tools/frame-capture.md)

基础章节的源码基线为 [e7ca801](https://github.com/xuu-xuu-xuu/FlareEngine/tree/e7ca801292037b33172d094cc4a434682ac83c9c)；骨骼动画、抓帧与编码章节对应 [542afe8](https://github.com/xuu-xuu-xuu/FlareEngine/tree/542afe82dcdf65eb987477b3862dd3b5d82b43e5)；渲染工作室光照/材质/GPU 可见性接口对应 [34bad2e](https://github.com/xuu-xuu-xuu/FlareEngine/tree/34bad2e)；自研 UI 单栈、预制体、输入/音频/粒子/导航与阶段四 Mesh Shader/渲染优化对应 [6f1fea5](https://github.com/xuu-xuu-xuu/FlareEngine/tree/6f1fea505082d4a90a24b4d1779c45fbf8c6152d)；DXR 1.1 混合硬件光追与黑曜金 2×3 编辑器工作流对应 [87d7567](https://github.com/xuu-xuu-xuu/FlareEngine/tree/87d756767974810c1b53d683919c9af108e6c967) 及当前主线零重启热重载、`F_` 高频 API 增量。

维护文档时，在仓库根目录依次运行 `npm.cmd --prefix Manual ci` 和 `npm.cmd --prefix Manual run dev`，打开终端显示的本地地址即可预览侧栏、搜索和源码链接。
