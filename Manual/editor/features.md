# 功能与默认编辑器的连接

组件属性由运行时描述表定义，编辑器只负责展示和提交。配置随场景保存，帧提取和运行服务分别消费渲染与物理部分。

| 入口 | 负责的类型或接口 |
| --- | --- |
| 刚体、角色、场景渲染和环境配置 | [`FeatureComponent`](#feature-components)、[`AppendEngineFeatureMetadata()`](#feature-components)；继承 [`ComponentBase`](../runtime/component-base.md)，复用组件池和属性校验。 |
| 编辑器属性与撤销 | [`FeatureInspector`](#feature-inspector) 调用 [`EditorSession::SetProperty()`](session.md)；运行帧由 [`ApplyFeatureFrameData()`](#feature-inspector) 提取纯 CPU 数据。 |
| 物理代理 | [`ScenePhysics::Synchronize()`](#scene-physics) 按组件创建或移除代理；[`ApplyTransforms()`](#scene-physics) 转回父节点局部坐标，`Clear()` 负责释放。 |
| 派生资源和参考预览 | [`ExecuteFeatureTool()`](#feature-tools) 执行已有算法；[`EditorFeatureTools::Poll()`](#feature-tools) 在主线程发布和登记结果。 |
| 游戏 UI 与昼夜资源 | [`UiInspector`](#ui-inspector) 使用既有 UI 文档历史；[`TODPreview`](#tod-preview) 独立管理时钟、配置和撤销，避免改变 Play 世界。 |

C# 通过 `Physics.GetBody(entity)` 和 `CharacterController.FromComponent(entity)` 绑定组件生成的物理实例。原生桥接和默认编辑器共用 `ScenePhysics`，不重复创建代理。

资源工具把成功产物写入内容目录并登记项目依赖。用户 Shader 使用同目录的 PBR、Forward、Shadowed、Environment 包；渲染器按实际 Pass 选择兼容管线。只存在旧 PBR 包时，前向路径给出诊断并回退，避免向不兼容的根签名提交绘制。

大气、雾、反射探针、SSR 和 AO 参考接口仍只表示已有 CPU 算法。工具中的预览不会把它们变为场景 GPU 特效。

## 定义

下面的定义直接从源码包含，随文档构建更新。

<a id="feature-components"></a>
<details><summary>功能组件与描述表接口</summary>

<<< @/../Source/FunctionLayer/Runtime/FeatureComponents.h{cpp}

</details>

<a id="feature-inspector"></a>
<details><summary>属性适配与帧提取</summary>

<<< @/../Source/ToolLayer/Editor/FeatureInspector.h{cpp}

<<< @/../Source/FunctionLayer/Application/FeatureFrameData.h{cpp}

</details>

<a id="ui-inspector"></a>
<details><summary>UI 属性适配</summary>

<<< @/../Source/ToolLayer/Editor/UiInspector.h{cpp}

</details>

<a id="scene-physics"></a>
<details><summary>ScenePhysics</summary>

<<< @/../Source/FunctionLayer/Physics/ScenePhysics.h{cpp}

</details>

<a id="feature-tools"></a>
<details><summary>资源任务与编辑器控制器</summary>

<<< @/../Source/ToolLayer/Editor/EditorFeatureTools.h{cpp}

</details>

<a id="tod-preview"></a>
<details><summary>TODPreview</summary>

<<< @/../Source/ToolLayer/Editor/TODPreview.h{cpp}

</details>
