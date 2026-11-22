# DXR 1.1 混合硬件光线追踪与 SVGF 降噪

这篇文档拆解 Flare Engine 的混合硬件光线追踪管线。开启场景渲染设置中的 `render.rayTracing` 后，在支持 DXR 1.1（`RayTracingTier1_1` Inline `RayQuery`）的显卡上，引擎以光栅化 [DepthNormals](shadows.md#深度、法线与屏幕空间遮蔽) 作为一级表面输入，用硬件光追接管或增强太阳软阴影、RTAO、镜面/粗糙反射和单次反弹 RTGI；硬件不支持或关闭开关时，零开销自动回退到光栅与屏幕空间路径。

## 混合光追一帧执行流程图

```text
1. PrepareD3D12RayTracing()（帧录制前）
   ├─ 收集不透明网格 + 阴影投射物（含镜头外物体）
   ├─ 静态网格按几何缓冲身份查常驻 GPU BLAS 缓存（blasCacheHits / blasBuilds）
   ├─ 构建每帧槽 TLAS（≤ 4,096 实例）+ 唯一网格几何/反照率表（≤ 64 种网格）
   └─ 上传 D3D12RayTracingConstants (1280 B) 与 D3D12RayTracingInstanceData (144 B/实例)
          │
          ▼
2. DepthNormals Prepass（全分辨率 D32 深度 + RG16F 法线 + RGBA16F 材质）
          │
          ▼
3. RecordD3D12RayTracingPreOpaque()（不透明主着色之前）
   CSTraceVisibility ──▶ CSTemporalVisibility ──▶ 多轮 CSATrousVisibility
   └─ 输出全分辨率 RG16_FLOAT（x=RTAO, y=太阳软阴影）挂载到 t13
   └─ 仅开其一时，SyncD3D12RayTracingScreenFallback() 保留另一通道的屏幕空间 GTAO/接触阴影
          │
          ▼
4. Opaque Forward Shading（读取 t13 直接应用 RTAO 与 RT 太阳阴影）
          │
          ▼
5. RecordD3D12RayTracingPostOpaque()（不透明绘制后、Transparent 之前）
   CSTraceIndirect ──▶ CSTemporalIndirect ──▶ CSATrousIndirect ──▶ 合成叠加回线性 HDR
   └─ 镜面/粗糙反射（GGX VNDF，roughness ≤ 0.78）+ 单次反弹漫反射 RTGI（太阳+8点光+8局部灯+天空）
```

## RayTracingSettings

[`flare::rhi::RayTracingSettings`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/Rhi.h#L422) 由 [`ApplyFeatureFrameData()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FeatureFrameData.cpp#L76) 从 `RenderSettingsComponent` 提取并写入 `ForwardLighting::rayTracing`，内部按 `VisibilityEnabled()` 与 `IndirectEnabled()` 拆分为前/后两个可独立开关的阶段：

```cpp
// Source/PlatformLayer/Graphics/Rhi.h#L422
struct RayTracingSettings
{
    bool  enabled = false;
    bool  shadows = true, reflections = true, ambientOcclusion = true, globalIllumination = true;
    float shadowStrength = 1.f, reflectionStrength = 1.f, aoStrength = .85f, giStrength = 1.f;
    float aoRadius = 1.5f, maxDistance = 120.f, sunAngularRadius = .0093f, normalBias = .015f;
    float maxRoughness = .78f, historyBlendWeight = .90f;
    Mat4  view, clipFromView, previousClipFromWorld = Mat4::Identity();
    std::uint32_t frameIndex = 0;
    bool  historyValid = false;
    std::uint64_t sceneToken = 0;
};
```

| 子通道与场景键 | 默认值 | 说明 |
| --- | --- | --- |
| `render.rayTracing` | `false` | 混合硬件光线追踪总开关 |
| `render.rtShadows` / `rtShadowStrength` / `rtSunAngularRadius` | `true` / `1.0` / `0.0093 rad` | 在太阳圆盘角半径内随机抖动采样软阴影 |
| `render.rtAmbientOcclusion` / `rtAoStrength` / `rtAoRadius` | `true` / `0.85` / `1.5 m` | 余弦加权半球硬件光追环境遮蔽（RTAO） |
| `render.rtReflections` / `rtReflectionStrength` | `true` / `1.0` | GGX VNDF 重要性采样镜面与粗糙表面反射（粗糙度上限 `0.78`） |
| `render.rtGi` / `rtGiStrength` | `true` / `1.0` | 命中点估算直接光（太阳 + 最多 8 点光 + 8 局部灯 + 天空）的单次反弹漫反射 |
| `render.rtMaxDistance` | `120 m` | 单条光线最大追踪距离 |

## D3D12RayTracingSystem

`D3D12RayTracingSystem` 定义在 [`D3D12RayTracing.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12RayTracing.h#L81) 与 [`D3D12RayTracing.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12RayTracing.cpp#L33)，负责常驻加速结构与预编译 `SERT1` 着色器包管理。构建阶段由 [`WriteD3D12RayTracingShaders()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12RayTracing.cpp#L594) 调用 DXC 将 [`Resources/Shaders/RayTracing.hlsl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/RayTracing.hlsl#L421) 的 6 个 `cs_6_5` Compute 入口与合成着色器编译打包，免除运行时编译卡顿。

由于采用 DXR 1.1 Inline `RayQuery`（而非传统的 RayGen/HitGroup Shader Binding Table），全部追踪逻辑直接写在常规 Compute Shader 中，大幅简化管线状态切换：

```hlsl
// Resources/Shaders/RayTracing.hlsl#L450（CSTraceVisibility 中 RTAO 内联追踪片段）
if (rtFlags.z != 0u)
{
    float2 uAo   = SampleSequence2D(uint2(pixel), rtFlags.x, 0u);
    float3 aoDir = SampleCosineHemisphere(uAo, worldNormal);
    RayDesc aoRay;
    aoRay.Origin    = rayOrigin;
    aoRay.Direction = aoDir;
    aoRay.TMin      = 0.002;
    aoRay.TMax      = rtTracingParams.x; // rtAoRadius

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
           | RAY_FLAG_FORCE_OPAQUE
           | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qAo;
    qAo.TraceRayInline(sceneTlas, RAY_FLAG_NONE, 0xFF, aoRay);
    qAo.Proceed();
    if (qAo.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        float hitRatio = saturate(qAo.CommittedRayT() / max(1e-3, rtTracingParams.x));
        ao = lerp(1.0, hitRatio, saturate(rtStrengths.y));
    }
}
```

### PrepareD3D12RayTracing

[`PrepareD3D12RayTracing()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12RayTracing.cpp#L844) 在每帧录制前收集不透明绘制与全部阴影投射物：
- **跨帧持久 BLAS 缓存**：静态网格按底层几何缓冲身份缓存 BLAS，命中计入 `blasCacheHits`，首次构建计入 `blasBuilds`。
- **帧槽 TLAS 与无绑定几何表**：构建最多 `4,096` 个实例的 TLAS（`MaxRtInstances`）和最多 `64` 种唯一网格（`MaxUniqueRtMeshes`）的顶点、索引与反照率描述符表，使镜头外的物体同样能投射正确软阴影、出现在镜面反射里并贡献反弹漫反射颜色。

### RecordD3D12RayTracingPreOpaque

[`RecordD3D12RayTracingPreOpaque()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12RayTracing.cpp#L1483) 在不透明前向着色之前运行可见性光追与 SVGF 降噪：
1. `CSTraceVisibility`（[`#L421`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/RayTracing.hlsl#L421)）：按像素发射 1 条 RTAO 半球光线与 1 条太阳圆盘软阴影光线。
2. `CSTemporalVisibility`（[`#L520`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/RayTracing.hlsl#L520)）：结合 `previousClipFromWorld` 运动重投影与深度/法线一致性检验累积历史，并估计一/二阶矩方差。
3. 多轮 `CSATrousVisibility`（[`#L617`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/RayTracing.hlsl#L617)）：方差与深度/法线引导的小波边缘保持滤波，产出全分辨率 `RG16_FLOAT` 绑定至主着色器 `t13`。
4. **混合回退联动**：如果用户只开了 RTAO 没开 RT 阴影（或反之），[`SyncD3D12RayTracingScreenFallback()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12RayTracing.cpp#L666) 自动保留未启用通道的屏幕空间 GTAO 或接触阴影结果，二者无缝拼合进 `t13`。

### RecordD3D12RayTracingPostOpaque

[`RecordD3D12RayTracingPostOpaque()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12RayTracing.cpp#L1834) 在不透明前向绘制之后、透明物体合成之前运行间接光追：
1. `CSTraceIndirect`（[`#L691`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/RayTracing.hlsl#L691)）：发射 GGX VNDF 镜面反射光线与半球漫反射 RTGI 光线，命中后按网格表读取三角形插值 UV、反照率与法线并计算直接光着色。
2. `CSTemporalIndirect`（[`#L775`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/RayTracing.hlsl#L775)）与 `CSATrousIndirect`（[`#L881`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/RayTracing.hlsl#L881)）：分别对镜面与漫反射缓冲做时域累积与方差引导降噪。
3. 最终合成着色器按表面粗糙度、金属度与菲涅耳项将降噪后的反射与 RTGI 叠加回线性 HDR 颜色缓冲。

## RayTracingStatistics

[`flare::rhi::RayTracingStatistics`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/Rhi.h#L443) 零回读地向性能诊断与 HUD 暴露光追状态：

| 字段 | 含义 |
| --- | --- |
| `requested` / `supported` / `valid` | 是否请求启用、当前显卡是否具备 DXR 1.1、本帧光追是否有效运行 |
| `shadowsValid` / `aoValid` / `reflectionsValid` / `giValid` | 四个子通道各自的本帧有效状态 |
| `fallback` (`RayTracingFallback`) | 回退原因：`None`、`UnsupportedHardware`、`MissingDepthNormals`、`IncompleteGeometry`、`EmptyAccelerationStructure`、`ShaderUnavailable`、`ResourceFailure` |
| `blasCount` / `blasBuilds` / `blasCacheHits` | 常驻 BLAS 总数、本帧新建数、缓存命中数 |
| `tlasInstances` / `uniqueMeshes` / `unsupportedDraws` | 本帧 TLAS 实例数、唯一网格表数量与跳过的不支持绘制数 |
| `accelerationBytes` / `scratchBytes` / `denoiserBytes` | 加速结构、暂存缓冲与 SVGF 降噪目标各自占用的显存字节数 |

::: tip 排障与显存观察入口
- 若开启 `render.rayTracing` 后画面没有变化，优先检查 `RayTracingStatistics::fallback`：最常见原因是设备不支持 `RayTracingTier1_1`（`UnsupportedHardware`），或 `Resources/Shaders/` 缺少预编译 `SERT1` 着色器包且本机未找到 Windows SDK DXC（`ShaderUnavailable`）。
- 切换场景或视口缩放时，引擎自动通过 `sceneToken` 与分辨率校验重置历史有效位，避免旧帧重投影残留鬼影。
:::
