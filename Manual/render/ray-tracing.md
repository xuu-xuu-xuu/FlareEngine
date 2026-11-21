# DXR 1.1 混合硬件光线追踪与 SVGF 降噪

开启场景渲染设置中的 `render.rayTracing` 后，渲染器在具备 DXR 1.1（`RayTracingTier1_1` Inline `RayQuery`）能力的显卡上启用混合硬件光追管线：以光栅化 [DepthNormals](shadows.md#深度、法线与屏幕空间遮蔽) 作为一级可见表面输入，用硬件光线追踪替代或增强方向软阴影、环境遮蔽（RTAO）、镜面/粗糙反射和单次反弹全局漫反射（RTGI），在不支持硬件光追或关闭开关时零开销回退光栅与屏幕空间路径。

## RayTracingSettings

`se::rhi::RayTracingSettings` 由 `ApplyFeatureFrameData()` 从场景 `RenderSettingsComponent` 提取并写入 `ForwardLighting::rayTracing`，按作用阶段拆为两组独立子通道：

| 子通道与场景键 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `render.rayTracing` | `false` | 混合硬件光线追踪总开关 |
| `render.rtShadows` / `rtShadowStrength` | `true` / `1.0` | 按 `rtSunAngularRadius`（默认 `0.0093` rad）采样太阳圆盘锥形软阴影 |
| `render.rtAmbientOcclusion` / `rtAoStrength` / `rtAoRadius` | `true` / `0.85` / `1.5m` | 余弦加权半球环境光遮蔽（RTAO） |
| `render.rtReflections` / `rtReflectionStrength` | `true` / `1.0` | 基于 GGX VNDF 采样的镜面与粗糙表面反射（粗糙度上限 `0.78`） |
| `render.rtGi` / `rtGiStrength` | `true` / `1.0` | 命中点直接光（太阳 + 最多 8 盏点光 + 8 盏聚光/局部灯 + 天空）的单次反弹漫反射 |
| `render.rtMaxDistance` | `120m` | 单条射线最远追踪距离 |

## D3D12RayTracingSystem

`D3D12RayTracingSystem` 管理持久加速结构与预编译 `SERT1` 光追着色器包（由 `WriteD3D12RayTracingShaders()` 编译 `Resources/Shaders/RayTracing.hlsl` 中的 6 个 Compute 入口与合成着色器生成）。

### PrepareD3D12RayTracing

`PrepareD3D12RayTracing()` 在每帧录制前收集不透明网格与阴影投射物：静态网格按几何缓冲身份复用跨帧常驻 GPU BLAS 缓存（命中记入 `blasCacheHits`，首次出现写入 `blasBuilds`），再按帧槽构建最多 `4,096` 个实例的顶层加速结构（TLAS）以及最多 `64` 种唯一网格的顶点、索引与反照率纹理描述符表，使未出现在屏幕内的物体同样能投射阴影、出现在镜面反射中并贡献色彩反弹。

### RecordD3D12RayTracingPreOpaque

`RecordD3D12RayTracingPreOpaque()` 在不透明主着色之前运行可见性光追与 SVGF 滤波（`CSTraceVisibility` → `CSTemporalVisibility` → 多轮 `CSATrousVisibility`）：联合历史运动重投影、二阶矩方差估计与深度/法线引导的小波边缘保持滤波，输出全分辨率 `RG16_FLOAT`（AO 与太阳可见度）供主着色直接采样。若仅开启其中一项，`SyncD3D12RayTracingScreenFallback()` 自动保留另一项的屏幕空间 GTAO 或接触阴影结果。

### RecordD3D12RayTracingPostOpaque

`RecordD3D12RayTracingPostOpaque()` 在不透明前向绘制之后、透明物体合成之前执行间接光追（`CSTraceIndirect` → `CSTemporalIndirect` → `CSATrousIndirect`），对镜面反射与漫反射反弹分别做时域累积和方差引导降噪，最后按表面粗糙度、金属度与菲涅耳项叠加回线性 HDR 颜色缓冲。

## RayTracingStatistics

`se::rhi::RayTracingStatistics` 向帧统计与性能诊断暴露硬件支持性、`RayTracingFallback` 回退原因、`blasCount / blasBuilds / blasCacheHits / tlasInstances` 以及加速结构、暂存缓冲与降噪目标显存字节数，不依赖额外 CPU 回读等待。
