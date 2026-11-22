# 后处理、抗锯齿与调色

这篇文档说明线性 HDR 场景渲染完成后，画面如何经过时间抗锯齿（TAA）、自动曝光直方图、泛光（Bloom）、色调映射与 3D LUT 调色、FXAA 与 UI 叠加，最终写入可显示的 LDR 交换链缓冲。

## 后处理一帧数据流

```text
线性 HDR SceneColor (RGBA16F) + SceneDepth (D32)
   │
   ├─ [可选] D3D12Taa：Halton 8 相位抖动 → RecordVelocity() → Resolve()（历史邻域夹取）
   │
   ├─ [可选] D3D12Exposure：256 桶 log2 亮度直方图 → 60%~80% 分位目标 → 亮/暗速适应 (1×1 曝光)
   │
   ├─ [可选] Bloom（半分辨率）：BloomExtract → BloomHorizontal → BloomVertical
   │
   ├─ ToneMap（Reinhard 或 SE_TONEMAP_ACES + 自动曝光 + 33³ 3D LUT / Lift-Gamma-Gain）──▶ LDR
   │
   ├─ [可选] FXAA ──▶ UI Overlay ──▶ Output (呈现到交换链 / 读回)
```

## HDR 输出、泛光与自动曝光

### Pass 录制与参数钳制

- [`PostProcessRenderer::Initialize()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/PostProcessRenderer.cpp#L7) 准备 `BloomExtract`、`BloomBlur`、`ToneMap`（含可选 `SE_TONEMAP_ACES` 变体）、`Fxaa` 与 `UiOverlay` 管线。
- [`PreparePostProcessSettings()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/PostProcessRenderer.cpp#L33) 对全部浮点参数做有限数与安全区间钳制（矩阵非法时直接关停 TAA 防止 NaN 速度），再由 [`RecordPostProcessPasses()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/PostProcessRenderer.cpp#L79) 按固定顺序发出 Pass。
- 当通过 `FrameGraph` 录制且启用 `transientAliasing` 时，互不重叠的半分辨率 `BloomB` 与全分辨率 `LdrColor` 会自动复用同一 64 KiB 对齐物理显存槽。

### 自动曝光（256 桶 log2 直方图）

GPU 路径由 [`D3D12Exposure`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Exposure.h#L43) 录制直方图统计与时间适应计算，并将结果绑定到 `ToneMap` 的 `t2`（布局版本 `ToneMapLayoutVersion = 27`）；CPU 参考实现 [`BuildExposureHistogram()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/AutoExposure.cpp#L30) 与 [`AutoExposure`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/AutoExposure.h#L34) 与其严格同构：

| 默认参数 (`ExposureSettings`) | 默认值 | 说明 |
| --- | --- | --- |
| `histogramMinLog2` / `histogramMaxLog2` | `-12.0` / `4.0` | 256 桶对数亮度区间，屏蔽非有限值与 `≤ 0` 极暗像素 |
| `lowPercentile` / `highPercentile` | `0.60` / `0.80` | 取 60%~80% 分位几何中点作为目标亮度，抑制极端高光与纯黑边角干扰 |
| `brightAdaptation` / `darkAdaptation` | `3.0/s` / `1.0/s` | 进亮处快适应、进暗处慢适应的双速指数趋近 |
| `minimumExposure` / `maximumExposure` / `keyValue` | `1/1024` / `64` / `0.18` | 曝光上下限与 18% 中灰基准键 |

## 时间抗锯齿（TAA）

TAA 通过子像素抖动采样在多帧间重建亚像素几何与高光细节：

```cpp
// Source/FunctionLayer/Rendering/TemporalAA.cpp#L24
// 8 帧循环的 Halton(2, 3) 低差异子像素抖动，范围 [-0.5, +0.5] 像素
TaaJitter HaltonJitter(std::uint32_t frameIndex)
{
    const auto phase = frameIndex % 8u;
    return {Halton(phase + 1u, 2u) - 0.5f, Halton(phase + 1u, 3u) - 0.5f};
}
```

- **投影抖动与速度场**：[`HaltonJitter()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/TemporalAA.cpp#L24) 生成抖动并由 [`ApplyProjectionJitter()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/TemporalAA.cpp#L29) 偏移投影矩阵；D3D12 后端先通过 [`D3D12Taa::RecordVelocity()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Taa.cpp#L231) 写出屏幕空间速度缓冲。
- **重投影与邻域夹取**：[`D3D12Taa::Resolve()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Taa.cpp#L265)（CPU 对照见 [`ResolveTaa()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/TemporalAA.cpp#L85)）按速度重投影历史颜色，并以当前帧 3×3 邻域色彩范围夹取历史值，有效抑制运动物体拖影（Ghosting），并按 `taaStaticBlend`（默认 `0.1`）与 `taaDynamicBlend`（默认 `0.2`）自适应混合。

## 色调映射与颜色分级（3D LUT）

色调映射和三级色彩校正既在 GPU `ToneMap.hlsl` 执行，也在 CPU [`ColorGrading.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/ColorGrading.cpp#L160) 保留精确对照：

```cpp
// Source/FunctionLayer/Rendering/ColorGrading.cpp#L160
Vec3 Tonemap(Vec3 linear, ToneMapper mapper)
{
    return mapper == ToneMapper::AcesFitted ? TonemapAcesFitted(linear) : TonemapReinhard(linear);
}

Vec3 ApplyLiftGammaGain(Vec3 linear, const ColorGradingSettings& settings)
{
    const auto apply = [&](float value, float lift, float gamma, float gain) {
        const auto safeGamma = std::isfinite(gamma) && gamma > 1e-4f ? gamma : 1.0f;
        const auto graded    = std::max(0.0f, value * gain + lift);
        return std::pow(graded, 1.0f / safeGamma);
    };
    return {apply(linear.x, settings.lift[0], settings.gamma[0], settings.gain[0]),
            apply(linear.y, settings.lift[1], settings.gamma[1], settings.gain[1]),
            apply(linear.z, settings.lift[2], settings.gamma[2], settings.gain[2])};
}
```

- **33³ LUT 布局**：GPU 将 3D LUT 打包为 `1089 × 33` 的 2D Strip（支持 `Rgba8` 或 `Rgba16Float`），供 `ToneMap`（`t3`，布局 `27`）或直接输出通道（`t2`，布局 `OutputLutLayoutVersion = 29`）采样。
- **`.selut` 热重载**：[`ColorLutFile`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/ColorLutFile.h#L25) 按轮询间隔（默认 `0.20 s`）仅检查文件时间戳与字节数，变化时才重载并递增 `revision` 触发 GPU 上传；若磁盘文件被误删或写坏，保留上次有效 LUT 避免画面闪回未调色状态。

::: tip 排障与调色建议
- **开启 TAA 后有闪烁或重影**：确认 `DepthNormals` 预通道完整、相机与动态物体正确传入了上一帧矩阵，且 `taaDepthThreshold` / `taaVelocityThreshold` 未被调得过大。
- **编辑 `.selut` 没生效**：查看 `ColorLutFile::Status()` 的 `error`、`reloads` 与 `failures`，确认 `render.lutEnabled` 与 `render.post` 均已开启。
:::
