# 可见性、Hi-Z 与间接绘制

这篇文档讲清引擎如何在 CPU 与 GPU 两端剔除视锥外和被遮挡的几何体，并把幸存绘制通过 `ExecuteIndirect` 批量提交，减少无效顶点与像素着色开销。

## GPU 驱动剔除与间接提交数据流

```text
World 实体变换 + StaticRenderModel 局部包围盒
   │  WorldBoundsCache::Resolve() + BuildFrustum()
   ▼
可见性候选集（VisibilityItem：世界 AABB + 实体/簇元数据）
   │
   ├─ 传统路径：DepthPrepass / DepthNormals ──▶ D3D12HiZ 逐级 max-归约生成层次深度金字塔 (HZB)
   │                                                 │
   ▼                                                 ▼
D3D12Visibility / D3D12Heterogeneous::RecordCull()（HeterogeneousCull.hlsl）
   · 6 平面视锥剔除 Visible(item)
   · [可选 render.gpuDrivenHiZ] 保守 max-HZB 遮挡测试 HiddenByCurrentFrame(item)
   · 仅把 64 字节间接命令的 InstanceCount 置 1 或 0（不压紧、保稳定次序）
   │
   ▼
D3D12Indirect / D3D12Heterogeneous::RecordBucket() ──▶ ExecuteIndirect()
   │
   ▼
OcclusionHud + RenderTelemetry（剔除计数、可见比例、GPU Pass 耗时统计）
```

## 包围体缓存与 CPU 参考剔除

- **世界包围盒缓存**：[`WorldBoundsCache`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/Visibility.h#L9) 按 `StableEntityId` 缓存世界空间 AABB，仅在实体变换矩阵或模型指针变化时重算，帧末自动回收失效条目。
- **视锥提取与同构校验**：[`BuildFrustum()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Math/Bounds.h#L33) 从 `clipFromWorld` 提取归一化 6 平面；CPU 对照函数 [`CullVisibilityReference()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/VisibilityData.cpp#L57) 与 GPU 共享同一 `VisibilityItem` 布局和剔除谓词，便于离线逐项核对 GPU 剔除结果。

## 层次深度（Hi-Z / HZB）与保守遮挡

[`D3D12HiZ`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12HiZ.h#L27) 将深度缓冲逐级按 **`max`** 归约成层次深度金字塔（在标准 `0..1` 深度下，`max` 代表该块内最远的遮挡物深度，保证绝不误剔）。

- **CPU 保守对照**：[`TestOcclusion()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/HiZ.cpp#L86) 把世界包围盒 8 个角点投影到屏幕矩形，挑选覆盖范围 `≤ 2×2` 纹素的 Mip，只有当包围盒最近深度严格大于该区域最远深度 `farthest + bias` 时才判定遮挡；[`HiZHistory`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/HiZ.h#L76) 追踪分辨率变化或相机突变，及时使历史 HZB 失效。
- **当前帧 HZB 剔除（`render.gpuDrivenHiZ`）**：[`HeterogeneousCull.hlsl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/HeterogeneousCull.hlsl#L18) 直接消费当前帧完整 `DepthNormals` 预绘制生成的 HZB，彻底消除“上一帧历史重投影误剔闪现”的问题：

```hlsl
// Resources/Shaders/HeterogeneousCull.hlsl#L42（保守 max-HZB 遮挡测试与间接命令写出）
uint level = 0; float scale = 1; uint levels = (uint)screen.w;
[loop] while (level + 1 < levels && any((hi - lo) / scale > 1.99)) { ++level; scale *= 2; }
uint2 dimensions = max(uint2(1, 1), (uint2)screen.xy >> level);
if (all(hi <= float2(dimensions) * scale + .0001))
{
    uint2 first = min((uint2)floor(lo / scale), dimensions - 1);
    uint2 last  = min((uint2)max(0, ceil(hi / scale) - 1), dimensions - 1);
    float farthest = 0;
    [unroll] for (uint sy = 0; sy < 4; ++sy)
    [unroll] for (uint sx = 0; sx < 4; ++sx)
    {
        uint2 texel = first + uint2(sx, sy);
        if (all(texel <= last)) farthest = max(farthest, currentFrameHiZ.Load(int3(texel, level)));
    }
    // 只有整个包围盒最近点确定落在最远遮挡面之后才剔除
    hidden = isfinite(farthest) && nearest > farthest + screen.z;
}

// 64 字节间接命令布局：VBV(16) + IBV(16) + CBV(8) + DrawIndexedArgs(20, InstanceCount 位于偏移 44) + pad(4)
[numthreads(64, 1, 1)]
void Cull(uint3 thread : SV_DispatchThreadID)
{
    if (thread.x < commandCount)
        outputCommands.Store(thread.x * 64 + 44,
            Visible(inputBounds[thread.x]) && !HiddenByCurrentFrame(inputBounds[thread.x]) ? 1u : 0u);
}
```

任何角点跨越近平面（`clip.w <= 1e-6`）、超出屏幕边界或无法被低级 Mip 完整覆盖时，均保守保留可见。

## 普通静态网格与 Meshlet 的间接提交

| 类 / 函数 | 源码位置 | 职责 |
| --- | --- | --- |
| `D3D12Visibility` | [`D3D12Visibility.h#L25`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Visibility.h#L25) | 执行常规单批次 GPU 视锥与 Hi-Z 可见性计算 |
| `D3D12Indirect` | [`D3D12Indirect.h#L34`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Indirect.h#L34) | 管理常规间接参数缓冲与 `ExecuteIndirect` 录制 |
| `D3D12Heterogeneous` | [`D3D12Heterogeneous.cpp#L217`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Heterogeneous.cpp#L217) | 异构间接提交：按 PSO 桶组织不同网格/材质的 64 字节间接命令（支持 `render.gpuDriven` 整对象与 `render.meshletCulling` 索引簇双模式） |
| `BuildOcclusionHud()` | [`OcclusionHud.cpp#L30`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/OcclusionHud.cpp#L30) | 将 `rhi::OcclusionCounters` 转为视口调试 HUD |
| `RenderTelemetry` | [`RenderTelemetry.h#L85`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/RenderTelemetry.h#L85) | 汇总逐帧 CPU/GPU Pass 耗时、绘制数量与剔除率 |

### 异构间接命令的分轨与回退规则

- **`render.gpuDriven`**：兼容的不透明静态绘制共享每条 **64 字节**间接命令（内嵌 `VBV + IBV + CBV + DrawIndexedArguments`），通过 `D3D12Heterogeneous::Prepare()` / `RecordCull()` / `RecordBucket()` 执行。
- **`render.meshletCulling`**：对已完成 Meshlet 离线切分的网格，按连续索引区间展开为细粒度间接命令做 GPU 视锥 + HZB 剔除（硬件支持 SM 6.5 时还可自动升级到硬件 Amplification/Mesh Shader 轨道，见 [Mesh Shader 双轨管线](mesh-shader.md)）。
- **自动回退**：蒙皮变形、自定义不兼容着色器或不满足分组条件的绘制自动走常规直接绘制路径，保证画面正确。

::: tip 性能验证与排障建议
- `render.gpuDriven` 与 `render.gpuDrivenHiZ` 在模板“渲染工作室”中默认关闭；启用调优时建议同时开启 `render.occlusionCounters` 与 `render.occlusionTiming`，避免低面数小场景因 Compute + HZB 固定开销反而抵消剔除收益。
- 若发现某些物体意外消失，优先检查该模型的 `localBounds` 是否准确，以及自定义顶点位移是否超出了静态包围盒。
:::
