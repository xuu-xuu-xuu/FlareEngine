# 阴影、分簇灯光与环境遮蔽

这篇文档完整拆解 Flare Engine 的直接光阴影、局部光阴影、Forward+ 256 盏点光源分簇，以及基于深度/法线预通道的屏幕空间遮蔽（GTAO 与接触阴影）。同时重点讲透最新一代阴影滤波升级：PSSM `λ = 0.80` 切分、硬件双线性比较采样器 `s1`、4-tap 4×4 三次 B 样条 PCF、16-tap IGN 旋转黄金角 Vogel PCSS、局部光 12-tap Vogel 软阴影以及 IGN 接触阴影抖动。

## 一帧阴影与遮蔽的执行管线

```text
                       ┌───────────────────────────────────────────────────────┐
                       │  1. Shadow Pass（D3D12Shadow）                         │
                       │  · 方向光：单图 或 3 级 PSSM CSM (D32 Texture2DArray)  │
                       │  · 局部光：最多 8 盏（点光×6面 / 聚光×1层，512×512）    │
                       └──────────────────────────┬────────────────────────────┘
                                                  │ t3 (方向阴影) / t25 (局部阴影)
┌─────────────────────────────────┐               │
│  2. DepthNormals Prepass        │               │  ┌──────────────────────────────────┐
│  D32 深度 + RG16F 世界几何法线   │               │  │  3. LightClustering Compute      │
│  (+ 可选 RGBA16F 材质供 SSGI/RT) │               │  │  16×9×24 = 3456 簇，最多 256 点光 │
└───────────────┬─────────────────┘               │  │  每簇保留最近 32 盏，溢出回退前 8  │
                │                                 │  └────────────────┬─────────────────┘
                ▼                                 │                   │ t9..t12
┌─────────────────────────────────┐               │                   │
│  4. AmbientOcclusion Compute    │               │                   │
│  · GtaoHalf (半分辨率 4×16 tap)  │               │                   │
│  · ResolveOcclusion (全分辨率)： │               │                   │
│    5×5 深度法线双边升采样        │               │                   │
│    + 12 步 IGN 抖动接触阴影      │               │                   │
└───────────────┬─────────────────┘               │                   │
                │ t13 (RG16F: x=AO, y=Contact)    │                   │
                └───────────────────────┬─────────┴───────────────────┘
                                        ▼
               5. Opaque Forward Shading（s1 硬件双线性比较采样器）
                  · 方向光：4-tap 三次 B 样条 PCF / 16-tap IGN Vogel PCSS
                  · 局部光：12-tap 世界坐标哈希旋转 Vogel + SampleCmpLevelZero
                  · 点光源：读 clusterRanges / clusterIndices 累加直接光
```

## 方向光阴影与 PSSM 级联切分

单图方向光由 [`PrepareDirectionalShadow()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/ShadowSystem.cpp#L9) 构造正交投影并做 texel 对齐；开启 `render.cascades` 后，[`PrepareDirectionalCascadeShadow()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/ShadowSystem.cpp#L39) 调用纯数学层 [`PrepareCascadeShadows()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/CascadeShadow.cpp#L101) 为相机视锥生成最多 **3 级**光空间投影，最后由 [`RecordD3D12Shadow()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Shadow.cpp#L252) 录制到 `D32_FLOAT` 深度数组。

### PSSM 对数-线性混合切分（λ = 0.80）与防抖动

早期的固定比例切分 `{0.10, 0.35, 1.0}` 在大范围场景下近景级联依然偏稀。现行主线采用 Zhang et al. 的 **PSSM 实用切分公式**（`λ = 0.80`），大幅加密第 0、1 级联的近景纹素密度，同时保留足够远的过渡区间：

```cpp
// Source/FunctionLayer/Rendering/CascadeShadow.cpp#L14
// PSSM 实用对数-线性混合切分（Zhang et al.）：加密第 0/1 级联近景纹素密度，同时保证远端平滑衔接。
float ComputePracticalSplitDistance(float nearPlane, float shadowDistance, std::uint32_t cascadeIndex)
{
    if (cascadeIndex + 1 >= CascadeShadowMaximumCascades) return shadowDistance;
    const double n = std::max(double(nearPlane), 1e-4);
    const double f = std::max(double(shadowDistance), n + 1e-4);
    const double t = double(cascadeIndex + 1) / double(CascadeShadowMaximumCascades);
    const double logarithmic = n * std::pow(f / n, t);
    const double uniform     = n + (f - n) * t;
    constexpr double lambda  = 0.80;
    return float(lambda * logarithmic + (1.0 - lambda) * uniform);
}
```

每一级在拟合正交包围体时还做了三项稳定性设计（[`CascadeShadow.cpp#L71`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/CascadeShadow.cpp#L71)）：
1. **旋转不变包围球 + 半径量化**：按子视锥 8 角点计算外接球并按 `1/16` 米向上量化，避免相机旋转时正交视口宽度逐帧伸缩导致阴影边缘呼吸闪烁。
2. **滤波护边与光空间纹素对齐（Texel Snapping）**：预留 `filterGuardTexels`（PCSS 时自动扩为 `ceil(maxRadius) + 1`），并将包围球中心对齐到整数光空间纹素格点，抑制相机平移时的阴影爬行。
3. **投射体深度外扩（`casterDepthPadding = 100 m`）**：仅向光源方向拉长近平面，确保镜头背后或视锥外的遮挡物依然能在镜头内留下完整影子。

级联边界通过 `blendFraction = 0.10` 进行平滑交叉混合，CPU 侧参考选择函数见 [`SelectCascade()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/CascadeShadow.cpp#L139)，与 HLSL 中的 `CascadeSelectionForPosition()` 严格同构。静态场景还可开启逐级静态阴影深度缓存，详见 [ComputeCascadeStaticShadowHashes](mesh-shader.md#computecascadestaticshadowhashes)。

## 硬件双线性比较采样器 `s1`

整个阴影滤波升级的底层硬件基石在于静态比较采样器 `s1`（`shadowSampler`）。在 [`D3D12PipelineCache.cpp#L184`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12PipelineCache.cpp#L184) 中，`s1` 被配置为硬件 2×2 双线性比较滤波 + 白色边框（超出阴影图视为未遮挡）：

```cpp
// Source/PlatformLayer/Graphics/D3D12/D3D12PipelineCache.cpp#L184
samplers[1].ShaderRegister = 1;
samplers[1].Filter         = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT; // 硬件 2×2 双线性深度比较
samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
samplers[1].BorderColor    = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
```

硬件比较采样器一次 `SampleCmpLevelZero` 就会对邻近 2×2 四个深度纹素分别执行 `reference <= depth` 比较，再按亚纹素坐标对 4 个 `0/1` 结果做双线性插值。这让 PCF 和 PCSS 的**每次采样都等效覆盖 4 个纹素**，显著降低采样次数并彻底消除亚纹素阶梯。

::: info 关闭 PCF 时的严格点采样语义
当 `pcf == false`（`framePointCount.z <= 0.5`）或 PCSS 太阳角半径为 `0` 时，着色器显式把采样 UV 吸附到真实深度纹素中心 `(floor(uv / texelSize) + 0.5) * texelSize`，即使底层 `s1` 是双线性比较采样器，也严格保持单纹素硬阴影语义。
:::

## 4-tap 4×4 三次 B 样条 PCF 与接收平面深度梯度

开启 PCF 但未开启 PCSS 时，[`ShadowBindings.hlsli#L82`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/ShadowBindings.hlsli#L82) 采用 **Sigg & Hadwiger（GPU Gems 2）三次 B 样条分解技巧**：将原本需要 16 次点采样的 4×4 平滑 B 样条核折叠为仅 **4 次硬件双线性 `SampleCmpLevelZero`**，在极低带宽开销下得到 `C²` 连续的柔和锯齿抑制。

同时，对于斜向受光面，如果直接用中心点的深度 `reference` 去和邻近偏移纹素比较，容易产生条纹状自遮挡（Shadow Acne）。引擎通过 [`CascadeReceiverGradient()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/ShadowBindings.hlsli#L11) 直接用正交光矩阵行向量与几何法线解析求解接收平面的深度梯度 `dz/d(u, v)`（不依赖 `ddx/ddy` 屏幕导数，避免了物体边缘 quad 导数爆炸），并对梯度夹紧后加上半纹素双线性保护偏置 `bilinearGuard`：

```hlsl
// Resources/Shaders/ShadowBindings.hlsli#L82
// 1. 解析接收面梯度做上限夹紧，避免掠射角梯度过大导致透光（Peter-Panning）
float2 maxGrad         = 0.0015 / max(shadowParameters.xx, 1e-6);
float2 clampedGradient = clamp(gradient, -maxGrad, maxGrad);
// 2. 补偿硬件 2×2 双线性内部 ±0.5 texel 跨度的平面深度变化
float  bilinearGuard   = 0.5 * dot(abs(clampedGradient), shadowParameters.xx);

// 3. 计算 4×4 三次 B 样条权重 g0..g3，并合并成 2×2 双线性偏移 uv0 / uv1
float2 invTexel  = 1.0 / max(shadowParameters.xx, 1e-6);
float2 texPos    = uv * invTexel - 0.5;
float2 baseTex   = floor(texPos);
float2 f         = texPos - baseTex;
float2 oneMinusF = 1.0 - f;
float2 g0 = (oneMinusF * oneMinusF * oneMinusF) * (1.0 / 6.0);
float2 g1 = (4.0 - 6.0 * f * f + 3.0 * f * f * f) * (1.0 / 6.0);
float2 g2 = (1.0 + 3.0 * f + 3.0 * f * f - 3.0 * f * f * f) * (1.0 / 6.0);
float2 g3 = (f * f * f) * (1.0 / 6.0);
float2 w0 = g0 + g1;
float2 w1 = 1.0 - w0;
float2 uv0 = (baseTex - 0.5 + g1 / max(w0, 1e-6)) * shadowParameters.x;
float2 uv1 = (baseTex + 1.5 + g3 / max(w1, 1e-6)) * shadowParameters.x;

// 4. 仅 4 次硬件 2×2 比较采样完成等效 4×4 滤波
float baseRef = reference - bilinearGuard;
float c00 = directionalShadow.SampleCmpLevelZero(shadowSampler, float3(uv0.x, uv0.y, slice), baseRef + dot(clampedGradient, float2(uv0.x, uv0.y) - uv));
float c10 = directionalShadow.SampleCmpLevelZero(shadowSampler, float3(uv1.x, uv0.y, slice), baseRef + dot(clampedGradient, float2(uv1.x, uv0.y) - uv));
float c01 = directionalShadow.SampleCmpLevelZero(shadowSampler, float3(uv0.x, uv1.y, slice), baseRef + dot(clampedGradient, float2(uv0.x, uv1.y) - uv));
float c11 = directionalShadow.SampleCmpLevelZero(shadowSampler, float3(uv1.x, uv1.y, slice), baseRef + dot(clampedGradient, float2(uv1.x, uv1.y) - uv));
result = lerp(lerp(c00, c10, w1.x), lerp(c01, c11, w1.x), w1.y);
```

## 16-tap IGN 旋转黄金角 Vogel PCSS

在级联与 PCF 均开启时启用 `render.shadowPcss`，CPU 侧由 [`PrepareD3D12ShadowFilterConstants()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Shadow.cpp#L265) 填充 `b3`（80 字节，含 `tan(angularRadius)`、搜索距离、最大纹素半径以及各级联的真实世界深度跨度和单纹素世界 XY 尺寸），GPU 侧在 [`ShadowFilterBindings.hlsli#L32`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/ShadowFilterBindings.hlsli#L32) 执行物理半影滤波：

1. **IGN 逐像素圆盘旋转**：使用 Jorge Jimenez 的 Interleaved Gradient Noise 旋转预生成的 16 点黄金角 Vogel 低差异圆盘（`ShadowFilterVogel16`），把结构化条纹打散成视觉更柔和的高频蓝噪样式。
2. **16-tap 遮挡搜索（Blocker Search）**：在 `searchWorld = tan(angularRadius) * blockerSearchDistance` 换算出的纹素半径内（下限 `1.5 texels`）读取深度并应用 `clampedGradient`，统计真实遮挡物的平均世界光轴间距 `blockerGapWorld`。若无遮挡物则直接返回可见度 `1`，跳过滤波循环。
3. **半影估计 + Nyquist 最小核下限**：按 `requestedRadius = tan(angularRadius) * blockerGapWorld` 计算物理半影，但将滤波半径下限钳制在 **`1.35 texels`**（`minFilter`），防止物体贴地接触处半影缩得过小重新露出阴影图锯齿。
4. **16-tap 硬件双线性 PCF**：配合 `s1` 的 2×2 硬件比较（等效覆盖 64 纹素）与 `bilinearGuard` 输出最终软阴影可见度。

```hlsl
// Resources/Shaders/ShadowFilterBindings.hlsli#L55（核心逻辑）
float3 metrics = filterCascadeMetrics[level].xyz; // x=世界深度跨度, yz=单 texel 世界 XY 尺寸
float2 maxGrad = 0.0015 / max(inverseDimensions, 1e-6);
float2 clampedGradient = clamp(gradient, -maxGrad, maxGrad);
float  bilinearGuard   = 0.5 * dot(abs(clampedGradient), inverseDimensions);

// Jorge Jimenez IGN 逐像素随机旋转角
float ign = frac(52.9829189 * frac(dot(floor(screenPixel), float2(0.06711056, 0.00583715))));
float rotSin, rotCos;
sincos(ign * 6.2831853, rotSin, rotCos);

// Pass 1：16-tap 遮挡物搜索（带最小 1.5 texel 搜索下限）
float  searchWorld  = filterParameters.x * filterParameters.y;
float2 minSearch    = min(float2(1.5, 1.5), filterParameters.zz);
float2 searchTexels = clamp(searchWorld / metrics.yz, minSearch, filterParameters.zz);
float gapSum = 0; uint blockers = 0;
[loop] for (uint s = 0; s < 16; ++s)
{
    float2 d   = ShadowFilterVogel16[s];
    float2 rot = float2(d.x * rotCos - d.y * rotSin, d.x * rotSin + d.y * rotCos);
    int2 texel = (int2)floor(uv * dimensions + rot * searchTexels);
    if (all(texel >= 0) && all(texel < int2(width, height)))
    {
        float2 tapUv = (float2(texel) + .5) * inverseDimensions;
        float depth  = directionalShadow.Load(int4(texel, level, 0));
        if (depth < 1 && depth < reference + dot(clampedGradient, tapUv - uv))
        {
            float receiverPlane = unbiased.z + dot(clampedGradient, tapUv - unbiasedUv);
            gapSum += max(0, receiverPlane - depth) * metrics.x;
            ++blockers;
        }
    }
}

// Pass 2：半影估计 + Nyquist 1.35 texel 最小抗锯齿下限 + 16-tap 硬件双线性 PCF
if (blockers > 0)
{
    result.blockerGapWorld = gapSum / blockers;
    float  requestedRadius = filterParameters.x * result.blockerGapWorld;
    float2 minFilter       = min(float2(1.35, 1.35), filterParameters.zz);
    float2 filterTexels    = clamp(requestedRadius / metrics.yz, minFilter, filterParameters.zz);
    float2 filterRadiusUv  = filterTexels * inverseDimensions;
    float  baseRef         = reference - bilinearGuard;
    float  visibility      = 0;
    [loop] for (uint f = 0; f < 16; ++f)
    {
        float2 d        = ShadowFilterVogel16[f];
        float2 rot      = float2(d.x * rotCos - d.y * rotSin, d.x * rotSin + d.y * rotCos);
        float2 offsetUv = rot * filterRadiusUv;
        float2 tapUv    = uv + offsetUv;
        float  tapVis   = all(tapUv >= 0) && all(tapUv <= 1)
            ? directionalShadow.SampleCmpLevelZero(shadowSampler, float3(tapUv, level), baseRef + dot(clampedGradient, offsetUv))
            : 1.0;
        visibility += tapVis;
    }
    result.visibility = visibility * (1.0 / 16.0);
}
```

## 点光投影与聚光锥（局部光双路径阴影）

[`PrepareForwardLighting()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/ForwardRenderer.cpp#L39) 为聚光灯和开启 `castsShadow` 的点光分配最多 8 个 `localLights` 槽位。[`PrepareD3D12LocalLightConstants()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12LocalLighting.cpp#L31) 将它们打包进 `b7`（3600 字节）：

- 每盏投影点光占用同一 `D32_FLOAT` 深度数组 `t25` 的 **6 层**（±X/±Y/±Z 立方体面），投影聚光占用 **1 层**，总计最多 **48 层**，每层固定 `512 × 512`。
- 即使场景中没有方向光，只要存在任意投射阴影的局部灯，Shadow Pass 仍会用完整投射者列表渲染局部阴影图。
- 聚光在 `[outerAngle, innerAngle]` 间按 Hermite 平滑曲线衰减；设 `innerAngle == outerAngle` 时退化为硬切光锥。

在着色器 [`LocalLightBindings.hlsli#L20`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/LocalLightBindings.hlsli#L20) 中，`LocalLightVisibility()` 利用编译宏 `#ifdef SE_SHADOW_BINDINGS` 明确区分了**直接受光像素着色器**与**无比较采样器的计算着色器（如体积雾/室内光照）**两条路径：

```hlsl
// Resources/Shaders/LocalLightBindings.hlsli#L56
#ifdef SE_SHADOW_BINDINGS
    // 路径 A（直接表面受光 PS）：12-tap 世界坐标哈希旋转黄金角 Vogel + 硬件 2×2 SampleCmpLevelZero
    // 相比旧 3×3 点采样显著柔和化 512×512 局部阴影图边缘，并沿接收面解析透视深度梯度防自遮挡
    float  diskRadius = 1.65 / max((float)size, 1.0);
    float  hashPhase  = frac(52.9829189 * frac(dot(world.xz + world.yy * 0.37, float2(12.9898, 78.233)))) * 6.2831853;
    float  sRot, cRot; sincos(hashPhase, sRot, cRot);
    float2 clampGrad  = clamp(gradient, float2(-0.08, -0.08), float2(0.08, 0.08));
    [unroll] for (uint k = 0; k < 12u; ++k)
    {
        float r = sqrt(((float)k + 0.5) / 12.0) * diskRadius;
        float theta = (float)k * 2.3999632; // 黄金角 ≈ 2.39996 rad
        float st, ct; sincos(theta, st, ct);
        float2 dUv      = float2(ct * cRot - st * sRot, ct * sRot + st * cRot) * r;
        float2 sampleUv = clamp(uv + dUv, uvMin, uvMax);
        float  ref      = ndc.z + dot(clampGrad, sampleUv - uv) - 4e-6;
        sum += localShadowMap.SampleCmpLevelZero(shadowSampler, float3(sampleUv, (float)slice), ref);
    }
    visibility = sum * (1.0 / 12.0);
#else
    // 路径 B（计算着色器 / 体积雾等无 s1 路径）：3×3 共 9 次整数 Load + 二次 B 样条亚纹素权重（C1 连续）
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2   texel       = clamp(center + int2(x, y), int2(0, 0), int2(size - 1, size - 1));
        float2 texelCenter = (float2(texel) + .5) / size;
        float  ref         = ndc.z + dot(gradient, texelCenter - uv) - 2e-6;
        float  tap         = (ref <= localShadowMap.Load(int4(texel, (int)slice, 0))) ? 1.0 : 0.0;
        sum += tap * wx[x + 1] * wy[y + 1];
    }
    visibility = sum;
#endif
```

关闭某个网格实体的 `receivesShadow` 属性可单独跳过该网格的方向光与局部光阴影查询。

## Forward+ 256 灯分簇

开启 `render.clusteredLighting` 后，前向点光源上限从 8 盏提升到 **256 盏**，采用固定的 **`16 × 9 × 24 = 3,456` 空间分簇**（Z 方向按对数深度切分，与相机视锥远近自然匹配）。

- **GPU 4 阶段 Compute 建表**：[`D3D12LightClustering::Record()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12LightClustering.cpp#L108) 依次调度 [`LightClustering.hlsl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/LightClustering.hlsl#L37) 的四个阶段：
  1. `CountLights`（3456 组）：每簇构造视空间 AABB，用 8 个 32-bit 掩码记录相交候选灯与原始计数。
  2. `PrefixLights`（1 组）：对每簇截断后计数 `min(raw, 32)` 做前缀和，分配全局索引偏移并汇总统计计数器。
  3. `FillSparseLights` / `FillDenseLights`（54 组）：每簇写入最多 **32 盏**最近点光索引（等距时按稳定实体 ID 确定排序）。
- **溢出回退与热力图**：全局索引缓冲容量为 `65,536`；发生溢出时 `clusterCounters[6]` 置位，该帧自动回退到传统前 8 盏点光列表，避免闪烁或越界读取。开启 `render.clusterDebug` 可在画面叠加分簇灯数热力图。
- **CPU 逐项对照参考**：[`BuildLightClusters()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/LightClustering.cpp#L34)、[`FillLightClustersGlobal()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/LightClustering.cpp#L113) 与有界对照 [`BuildBoundedLightClusters()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/LightClustering.cpp#L118) 与 GPU 共享同一套分簇公式，方便离线回归测试。

## 深度、法线与屏幕空间遮蔽

### DepthNormals 几何预通道

[`D3D12DepthNormals`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12DepthNormals.cpp#L80) 在不透明主着色之前运行，复用与主绘制**完全一致的顶点着色器**（包含蒙皮和顶点压缩）与 Alpha 裁切覆盖规则，输出：
- `D32_FLOAT` 完整深度（`DepthNormals.Depth`）；
- `RG16_FLOAT` 八面体编码世界空间几何法线（`DepthNormals.WorldGeometricOct`）；
- 当室内光照（SSGI/SSR）或硬件间接光追启用时，同步输出 `RGBA16_FLOAT` 表面材质缓冲（`DepthNormals.Material`）。

只有当不透明绘制全部受支持且完整覆写预通道（`statistics.complete == true`）时，下游屏幕空间算法才消费这份几何缓冲，否则安全回退避免错误遮挡。

### 半分辨率 GTAO、双边升采样与 IGN 接触阴影

[`D3D12AmbientOcclusion::Record()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12AmbientOcclusion.cpp#L226)（入口 [`RecordD3D12AmbientOcclusion()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12AmbientOcclusion.cpp#L253)）驱动 [`AmbientOcclusion.hlsl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/AmbientOcclusion.hlsl#L22) 的两个计算阶段，输出全分辨率 `RG16_FLOAT` 可见度贴图绑定到 `t13`（`x` 调制间接环境光，`y` 乘在接收阴影表面的方向光可见度上）：

1. **`GtaoHalf`（半分辨率）**：每像素按 IGN 旋转 4 个方位切片、每侧 8 步（共 64 tap）执行地平线积分，估算间接光环境遮蔽。
2. **`ResolveOcclusion`（全分辨率）**：
   - `AoUpsample()` 以 5×5 深度 + 法线联合双边权重把半分辨率 GTAO 升采样到全分辨率（任一深度/法线阈值失配即拒绝采样，保护薄片和前景轮廓）；
   - `AoContact()` 沿方向光推进 12 步屏幕空间射线，检测阴影图分辨率无法捕捉的微小缝隙与贴地接触遮挡，并用 **IGN 逐像素半步抖动**（`contactStep - 1.0 + ign`）彻底打散固定步进带来的阶梯状条带：

```hlsl
// Resources/Shaders/AmbientOcclusion.hlsl#L106
float AoContact(int2 screenPixel, float3 center, float3 normal)
{
    // IGN 逐像素相位抖动，将 12 步固定射线步进的离散分层转化为细腻过渡
    float ign = frac(52.9829189 * frac(dot(float2(screenPixel), float2(0.06711056, 0.00583715))));
    float result = 1;
    float3 origin = center + normal * contactParameters.y;
    [loop] for (uint contactStep = 1; contactStep <= 12; ++contactStep)
    {
        float fraction = max(contactStep - 1.0 + ign, 0.05) / 12.0;
        float3 ray = origin + aoLightDirection.xyz * (contactParameters.x * fraction);
        int2 pixel = 0;
        if (!AoProject(ray, pixel)) break;
        float3 surface = 0;
        if (AoReadPosition(pixel, surface))
        {
            float difference = surface.z - ray.z;
            if (difference > contactParameters.y && difference < contactParameters.z
                && dot(surface - center, normal) > contactParameters.y)
            {
                float shadow = contactParameters.w > 0 ? saturate((1 - fraction) / contactParameters.w) : 1;
                result = 1 - shadow;
                break;
            }
        }
    }
    return result;
}
```

纯 CPU 参考实现 [`ComputeGtao()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/AmbientOcclusion.cpp#L65)、[`ComputeContactShadows()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/AmbientOcclusion.cpp#L108) 与 [`UpsampleBilateral()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/AmbientOcclusion.cpp#L144) 可用于离线单元测试与像素对齐验证。将 AO 强度设为 `0` 时，遮蔽输出恒为 `1`。

## 常用开关与调试视图速查

| 场景设置键 / 字段 | 作用 | 调试与排查手段 |
| --- | --- | --- |
| `render.cascades` | 启用最多 3 级 PSSM 方向光级联 | 打开 `cascadeDebug` 查看红/绿/蓝三级分带与混合过渡区 |
| `render.shadowPcss` | 在级联 + PCF 上启用 16-tap Vogel PCSS | `ShadowFilterDebug::Visibility` (1) 查看纯可见度；`(2)` 分别以 RGB 输出半影世界半径、遮挡间距、命中比 |
| `render.clusteredLighting` | 启用 256 盏点光 Forward+ 分簇 | 打开 `render.clusterDebug` 显示每簇灯数热力图；读回 `clusterCounters[6]` 查是否溢出 |
| `render.gtao` / `render.contactShadows` | 启用半分辨率 GTAO 与接触阴影 | 查看 `D3D12Presentation::screenSpaceStatistics.fallback` 诊断回退原因（如 `IncompleteGeometry`） |

::: tip 扩展与排障建议
- **看到斜坡条纹自遮挡（Shadow Acne）**：先检查 `cascadeSlopeBias` / `cascadeNormalBias` 和接收面法线是否规范化；确认没有禁用 `SE_CSM_RECEIVER_PLANE`。
- **局部灯开启阴影但没有影子**：核对场景中聚光 + 投影点光总数是否超出 `MaximumLocalLights`（8 槽），以及网格 `receivesShadow` / `castsShadow` 标志。
- 需要更高精度的屏幕外阴影与全局遮蔽时，可结合 [DXR 1.1 混合硬件光线追踪](ray-tracing.md) 直接替换 `t13` 的 AO 与太阳可见度通道。
:::
