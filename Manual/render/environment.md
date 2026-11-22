# 环境光照、大气与反射

这篇文档说明 HDR 环境贴图如何烘焙成 IBL 三件套进入渲染器、全局与局部 9 系数球谐（SH）漫反射探针如何采样，以及不透明前向绘制完成后，室内实时间接漫反射（SSGI）、屏幕空间反射（SSR）与局部光体积雾如何在同一通道里完成合成。

## 环境与室内实时光照数据流

```text
Radiance .hdr ──▶ DecodeRadiance() ──▶ BakeEnvironment() ──▶ .seenv 资产
                                             │  EnvironmentRenderCache::Load/Upload
                                             ▼  t4(辐照度) / t5(预滤波镜面) / t6(BRDF LUT)
DiffuseProbeCooker ──▶ .sedp 探针资产 ──▶ ShadingResourceCache (b5 全局 SH + t15 局部网格)
                                             │
                                             ▼
                      Opaque Forward Shading（直接光 + IBL/SH 间接光）
                                             │  复制只读线性 HDR + 读取 DepthNormals(D32/RG16F/RGBA16F)
                                             ▼
             RecordD3D12IndoorLighting()（Resources/Shaders/IndoorLighting.hlsl）
             ├─ 1/4 屏 DiffuseBounce ──▶ CSTemporalAccumulate ──▶ 3× CSATrousFilter
             ├─ 全屏合成：ReconstructDiffuse (SSGI) + ReflectedChange (SSR) + ApplyFog (体积雾)
             └─ 之后再进入 Transparent 半透明物体绘制
```

## 环境贴图进入渲染器（IBL）

1. **解码 HDR**：[`DecodeRadiance()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/HdrImage.cpp#L19) 将 32-bit RLE RGBE 全景图解码为非负有限的浮点像素（显式拒绝含未支持 `EXPOSURE=` / `PRIMARIES=` 元数据的文件，避免色彩误解）。
2. **离线烘焙三件套**：[`BakeEnvironment()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/Environment.h#L31) 生成 3 张 `RGBA16F` 纹理：漫反射辐照度立方体（`irradiance`）、按粗糙度分级 Mip 的预滤波镜面立方体（`prefiltered radiance`）与 GGX `BRDF LUT`。
3. **版本上传与绑定**：[`EnvironmentRenderCache`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/EnvironmentRender.h#L14) 负责缓存当前版本与引擎默认环境（`Resources/Environments/soft.seenv`）；[`PrepareEnvironmentBindings()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/EnvironmentRender.cpp#L61) 钳制曝光强度（`[0, 64]`）并提取去除平移后的相机天空逆矩阵交给天空与材质着色器。

## 全局与局部漫反射探针（9 系数 SH）

开启 `render.diffuseSh` 后，漫反射间接光改由 2 阶 9 系数球谐（L0–L2）重建，预滤波立方体继续负责镜面反射。

- **全局与局部无缝回退**：在 [`ShadingResourceCache::Prepare()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/ShadingResources.cpp#L173) 中，全局 9 系数可来自探针资产或直接由当前环境贴图投影写入 `b5`；当同时开启 `render.localDiffuseSh` 并指定有效 `render.diffuseProbe` 资产时，局部 3D 探针网格上传至 `t15`（宽 10 像素 × 最多 `16,384` 行，第 0–8 列存 SH 系数，第 9 列 `x/y` 分别存探针和网格单元有效位）。
- **防跨墙漏光**：着色器 [`DiffuseSh()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/ShadingBindings.hlsli#L48)（与 CPU 参考 [`SampleDiffuseProbeGrid()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/DiffuseProbeBake.cpp#L218) 同构）在单元 8 个角点间做三线性插值；若位置越界、网格单元被墙体阻断或任一角探针无效，自动回退到全局 SH 系数。
- **离线烘焙工具**：[`DiffuseProbeCooker`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/DiffuseProbeCooker.cpp#L67) 的 `local` 模式调用 [`BakeDiffuseProbeGrid()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/DiffuseProbeBake.cpp#L196)，追踪静态几何与光源的首跳直接光反弹输出规则网格（移动静态几何或灯光后需重新烘焙）。

## 室内实时光照（SSGI / SSR / 体积雾）

[`ApplyFeatureFrameData()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FeatureFrameData.cpp#L45) 将 `render.indirect`、`render.reflections`、`render.volumetricFog` 及强度/雾距参数写入 `FrameData::indoor`。不透明前向绘制完成后、`Transparent` 透明合成之前，后端调用 [`RecordD3D12IndoorLighting()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12IndoorLighting.cpp#L131)，将线性 HDR 颜色复制为只读快照，连同同分辨率 `DepthNormals` 的深度、法线与材质输出交给 [`IndoorLighting.hlsl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/IndoorLighting.hlsl#L119)。

当启用 `render.indirect` 时，[`D3D12IndoorHistory`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12IndoorLighting.h#L8) 额外维护双帧乒乓历史缓冲（`historyIrradiance`、`historyMoments`）与 2 张 À-Trous 缓冲，运行 **1/4 分辨率采样 → 时域重投影方差夹取 (`CSTemporalAccumulate`) → 3 趟法线/深度加权 `CSATrousFilter` → 全屏重建合成**，在 1080p 下抑制屏幕空间 GI 噪点（切场景或改分辨率时自动清零历史有效位）。

```hlsl
// Resources/Shaders/IndoorLighting.hlsl#L521（全屏合成主入口核心片段）
if (strengths.x > 0)
    color.rgb += strengths.x * ReconstructDiffuse(pixel, p, n) * ((1 - metal) * material.rgb * (1 - f0));
if (strengths.y > 0)
    color.rgb += strengths.y * ReflectedChange(p, n, eye, f0, rough);
// 体积雾最后沿视线积分（在所有不透明表面光照之后、透明物体之前）
if (strengths.z > 0)
    color.rgb = ApplyFog(max(color.rgb, 0), nearView, p);
```

| 场景开关 | 核心着色器函数与机制 | 适用范围与画面边界 |
| --- | --- | --- |
| `render.indirect` | [`DiffuseBounce()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/IndoorLighting.hlsl#L119) 在 1/4 分辨率采样周围可见表面计算单次漫反射反弹，经 `CSTemporalAccumulate` + 3 趟 `CSATrousFilter` 降噪后，由 [`ReconstructDiffuse()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/IndoorLighting.hlsl#L322) 结合平面距离与法线重建到全屏 | 保留基础环境/探针光；屏幕外的发光或高反光表面无法提供反弹（需屏幕外反弹请用 [硬件光追 RTGI](ray-tracing.md)） |
| `render.reflections` | [`ReflectionHit()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/IndoorLighting.hlsl#L346) 在视空间追踪并二分细分命中，[`ReflectedChange()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/IndoorLighting.hlsl#L388) 按菲涅耳与置信度替换环境镜面项 | 粗糙度 `≥ 0.68` 直接跳过追踪；未命中或临近屏幕边缘时平滑退回环境立方体 |
| `render.volumetricFog` | [`ApplyFog()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/IndoorLighting.hlsl#L478) 按 Beer–Lambert 计算透射率：方向光沿射线采样 16 点阴影可见度；每盏局部灯在射线穿过其影响球的区间取 16 个等角度样本以密集捕捉近灯体积光束 | 积分截止于首个不透明表面深度或 `render.fogDistance`；透明物体深度不参与本次体积积分 |

## 大气、雾与反射的 CPU 参考模型

以下纯 CPU 数学模型用于昼夜参数预演、离线烘焙与像素级单元测试：

| 模块与入口 | 源码位置 | 职责 |
| --- | --- | --- |
| `Atmosphere` | [`Atmosphere.h#L22`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Atmosphere.h#L22) | 按大气参数缓存 64×32 透射率 LUT；[`SkyRadiance()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Atmosphere.cpp#L114) 16 段单次散射积分 |
| `FogTransmittance()` | [`Atmosphere.cpp#L152`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Atmosphere.cpp#L152) | 16 段梯形解析积分高度雾透射率（与 [昼夜系统](../runtime/daylight.md) 联动） |
| `BoxProjectReflection()` / `BlendReflectionProbes()` | [`Reflection.cpp#L66`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Reflection.cpp#L66) / [`#L95`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Reflection.cpp#L95) | 视差包围盒校正反射方向与多探针权重混合 |
| `PrefilterGgx()` / `TraceScreenSpaceReflection()` | [`Reflection.cpp#L131`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Reflection.cpp#L131) / [`#L157`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Reflection.cpp#L157) | GGX 粗糙度预滤波与 CPU 屏幕空间反射追踪对照 |

::: tip 实践对比与注意事项
- `DepthNormals` 预绘制覆盖不完整（例如有自定义不支持的绘制）时，`RecordD3D12IndoorLighting()` 会直接保留原 HDR 帧，防止错误法线/深度造成黑斑或伪影。
- 可在启动器打开“室内展厅”模板直观对比这三项开关，操作见 [室内展厅的编辑器操作](../user/indoor-lighting.md) 与 [正式包同镜头画面对比](../user/indoor-lighting.md#gallery-comparison)。
:::
