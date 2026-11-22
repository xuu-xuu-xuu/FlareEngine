# 前向管线与帧图

这篇文档说明一帧画面如何从逻辑世界快照出发，经过灯光分槽、模型/材质合批和 `FrameGraph` 依赖编译，最终变成有序提交给 RHI 的渲染 Pass。它位于 `FunctionLayer/Rendering` 纯逻辑编排层，不直接持有任何 D3D12 屏障或原生状态表。

## 一帧前向渲染的数据流

```text
World / Environment
   │  BuildFrameData()
   ▼
FrameData（只读帧快照：meshes / cameras / lights / rectangleLights / render settings）
   │
   ├─▶ PrepareForwardLighting()         ── 稳定实体 ID 排序 → 主方向光 / 8 局部灯 / 256 Forward+ 点光
   ├─▶ StaticModelRenderCache           ── 网格/纹理上传、压缩驻留、材质解析
   ├─▶ SortOpaqueDraws() / Batcher      ── 按管线/材质/网格键排序，合并可共享实例
   │
   ▼
FrameGraph（每帧 Reset → AddTexture/AddPass/Read/Write → Compile）
   │  拓扑排序 + 死 Pass 剔除 + 瞬态纹理生存期与 64 KiB 物理别名规划
   ▼
ExecuteFrameGraph() → rhi::CommandContext
   Clear → Shadow → Sky → [DepthNormals → AmbientOcclusion]
         → Opaque → [Transparent] → [Bloom → ToneMap → FXAA → UI] → Output
```

## 场景渲染路径（SceneRenderRequest）

编辑器视口、预览窗口与独立 Player 通过 [`flare::SceneRenderRequest`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/SceneRenderPath.h#L10) 选择场景渲染路径，将“画什么场景”与“怎么呈现编辑器 UI”解耦：

```cpp
// Source/FunctionLayer/Rendering/SceneRenderPath.h#L9
enum class SceneRenderPath { PbrPreview, ForwardShadowed, Environment };

struct SceneRenderRequest
{
    const FrameData* frame = nullptr;          // 只在本帧有效
    const rhi::SceneTargets* targets = nullptr; // 由调用方保活到 Render 返回
    SceneRenderPath path = SceneRenderPath::ForwardShadowed;
};
```

| 路径枚举 | 典型用途 | 包含的关键能力 |
| --- | --- | --- |
| `PbrPreview` | 材质球/资产预览 | 单主光 PBR 预览，无阴影与复杂环境Pass |
| `ForwardShadowed` | 基础受光与阴影场景 | 方向光阴影、多点光/局部灯、基础后处理 |
| `Environment` | 完整 HDR 场景（编辑器/游戏主路径） | 方向光 CSM/PCSS、IBL/探针、Forward+ 256 点光、DepthNormals、GTAO/SSGI/光追与完整后处理 |

## 从灯光组件到渲染槽位（PrepareForwardLighting）

[`PrepareForwardLighting()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/ForwardRenderer.cpp#L10) 消费本帧 `LightInstance` 快照：先按稳定实体 ID 排序以保证跨帧确定性，再把 `color * intensity` 限制在 `[0, 10000]`，并把不同灯型路由到对应槽位：

```cpp
// Source/FunctionLayer/Rendering/ForwardRenderer.cpp#L39
// 聚光灯和开启 castsShadow 的点光优先进入 localLights（最多 8 槽），不重复进入普通点光表
if (light.type == LightType::Spot || light.castsShadow)
{
    if (frame.localLights.size() < rhi::MaximumLocalLights)
    {
        rhi::LocalLight local;
        local.position = point.position;
        local.radiance = point.radiance;
        local.range = point.range;
        local.spot = (light.type == LightType::Spot);
        local.castsShadow = light.castsShadow;
        if (local.spot)
        {
            local.direction = Normalize(TransformVector(instance->world, {0, 0, -1}));
            local.innerCos = std::cos(light.innerAngle);
            local.outerCos = std::cos(light.outerAngle);
        }
        frame.localLights.push_back(local);
        continue; // 一盏投影点光只进一个灯表，防止双重着色
    }
    ++excessLocal;
    if (light.type == LightType::Spot) continue; // 超量聚光记诊断，不退化为点光
}
// 其余普通点光（或超量投影点光）进入普通点光槽或 256 灯 Forward+ 列表
```

| 灯槽 | 上限 | 溢出策略与诊断键 |
| --- | --- | --- |
| 主方向光 (`directional`) | 1 盏 | 按最小稳定实体 ID 取第 1 盏，超量记录 `Rendering.LightLimit` |
| 局部灯 (`localLights`) | 8 盏（`rhi::MaximumLocalLights`） | 聚光灯和投影点光共享；超量点光回退为无阴影普通点光，超量聚光丢弃并记录 `Rendering.LocalLightLimit` |
| 普通点光 / Forward+ | 旧路径 8 盏 / 分簇路径 256 盏 | 超量分别记录 `Rendering.LightLimit` / `Rendering.ClusterLightLimit` |

## FrameGraph

[`flare::FrameGraph`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/FrameGraph.h#L106) 用资源的读写版本号（`FrameGraphResourceRef`）显式描述 Pass 依赖。每帧调用 `Reset()` 复用内部 Arena 内存，声明完纹理与 Pass 后调用 [`Compile()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/FrameGraph.cpp#L481)：

1. **依赖解析与校验**：检测重复 Pass 名、同 Pass 重复写同资源、未定义读、以及依赖环（返回 `FrameGraphStatus::Cycle` 等）。
2. **死 Pass 剔除**：从标有 `sideEffect`（如 `Output`）或写入外部资源的节点反向追踪，自动裁剪无消费节点的冗余 Pass。
3. **拓扑排序与瞬态显存别名**：确定最终执行顺序，并按 `[firstPass, lastPass]` 生存期计算 64 KiB 对齐的 `TransientAliasPlan`（例如在后处理中让互不重叠的 `BloomB` 与 `LdrColor` 复用同一物理显存槽，详见 [Mesh Shader 与渲染深水区优化](mesh-shader.md#transientaliasplan)）。

[`RecordShadowedFrameGraph()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/ForwardRenderer.cpp#L262) 是标准阴影前向帧的编排入口，根据编译后的 `graph.Passes()` 依次调用 `rhi::CommandContext::BeginPass()` / `Draw*()` / `EndPass()`，真正的 D3D12 资源状态转换与屏障仍由 RHI 后端的 `D3D12StateTracker` 唯一生成：

```cpp
// Source/FunctionLayer/Rendering/ForwardRenderer.cpp#L284（精简示意）
graph.Write(clear, sceneColor);    graph.Write(clear, sceneDepth);
graph.Write(shadowPass, shadowDepth);
graph.Read(skyPass, sceneColor);   graph.Write(skyPass, sceneColor);
// 可选 DepthNormals → AmbientOcclusion 子图
const auto geometricInput = AddGeometryGraph(graph, geometryWidth, geometryHeight, geometry, occlusion, geometryDepth);
// Opaque 显式依赖场景颜色/深度、阴影图以及可选的几何与 AO 输出
graph.Read(opaque, sceneColor);    graph.Read(opaque, sceneDepth);  graph.Read(opaque, shadowDepth);
if (geometricInput != FrameGraphInvalid) graph.Read(opaque, geometricInput);
graph.Write(opaque, sceneColor);   graph.Write(opaque, sceneDepth);
// 透明 Pass → 后处理子图（BloomExtract/H/V → ToneMap → FXAA → UI）→ Output
const auto status = graph.Compile();
```

## 材质缓存、绘制排序与合批

在提交给 `CommandContext` 之前，场景中的可见网格先经过三步整理以减少管线切换和 Draw Call：

| 组件 | 源码位置 | 职责 |
| --- | --- | --- |
| `StaticModelRenderCache` | [`StaticModelRender.h#L46`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/StaticModelRender.h#L46) | 上传网格顶点/索引（支持普通 48B、压缩、80B 蒙皮及多级 LOD / Meshlet）、纹理压缩（BC1/BC5/BC7）与显存驻留管理 |
| `SortOpaqueDraws()` | [`RenderQueue.cpp#L19`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/FunctionLayer/Rendering/RenderQueue.cpp#L19) | 按 `Pipeline → 纹理三件套 → 材质标量/开关 → VB/IB → subPassIndex → AlphaMode` 构建稳定排序键，既聚合相同状态绘制，又严格保证多 Pass 不透明材质按 `Pass 0 → Pass 1 → ...` 顺序提交 |
| `SortTransparentDraws()` | [`RenderQueue.cpp#L26`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/2318549/Source/FunctionLayer/Rendering/RenderQueue.cpp#L26) | 按相机视空间深度从远到近排序（等深按稳定实体 ID 与 Pass 顺序），保证半透明与发光叠加混合顺序确定 |
| `PbrInstanceBatcher` | [`Instancing.h#L14`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Instancing.h#L14) | 把共享同一网格和材质的多次绘制合并为 `PbrInstanceBatch`（每实例 240 字节结构写入 `t7` SRV） |

::: tip 调试帧图与排查 Pass 依赖
- 调用 [`FrameGraph::WriteDot()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/FrameGraph.cpp#L896) 或 [`FrameGraph::WriteJson()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/FrameGraph.cpp#L952) 可导出当前帧完整的 Pass 拓扑、资源生存期区间和物理别名分组。
- 排查某一 Pass 的画面问题时，可用 `FrameGraph::SetPassLimit(N)` 仅保留排序后的前 `N` 个 Pass 进行二分定位。
- 像素着色阶段、材质绑定与多 Pass / ASE 着色器热编译见 [光照着色、材质接口与多 Pass / ASE Shader](materials.md)；阴影与分簇细节见 [阴影、分簇灯光与环境遮蔽](shadows.md)。
:::
