# Mesh Shader 双轨管线与渲染深水区优化

这篇文档汇总 Flare Engine 在渲染深水区的五项核心性能优化：SM 6.5 硬件 Amplification/Mesh Shader 与 Compute 间接双轨提交、`FrameGraph` 物理显存别名、逐级静态阴影深度缓存、空间网格静态合批，以及昼夜光照联动与 1080p SSGI 时域降噪。全部实现严格遵守 `FunctionLayer/Rendering` 纯逻辑编排与 `PlatformLayer/Graphics/D3D12` 独占硬件执行的单向分层边界。

## 双轨提交与优化架构总览

```text
静态网格 MeshData ──▶ BakeMeshletSubmission() ──▶ MeshletSubmission (局部包围盒紧凑的簇表)
                                                      │
                  SelectMeshletSubmissionMode(caps, settings)
                   ┌──────────────────────────────────┴──────────────────────────────────┐
                   ▼ (meshShaderTier1 && preferMeshShader)                               ▼ (回退轨)
         D3D12Meshlet (SM 6.5 硬件轨)                                    D3D12Heterogeneous (Compute 间接轨)
         · 离线 .sebin DXIL（零运行时 dxcompiler.dll）                    · HeterogeneousCull.hlsl 视锥+HZB剔除
         · MeshletCull.as.hlsl → DispatchMesh → MeshletDraw.ms.hlsl      · 64 字节命令 ExecuteIndirect
         · groupshared 256 槽哈希表顶点去重（每簇 ≤ 64 三角形）

并行深水区优化：
  · TransientAliasPlan  ── FrameGraph 按生存期规划 64 KiB 槽 → D3D12MemoryAllocator 放置别名资源 + ALIASING 屏障
  · StaticShadowCache   ── ComputeCascadeStaticShadowHashes() 命中时 CopyTextureRegion 复制底图，仅补绘动态物体
  · StaticBatchCache    ── 按 32 m 空间单元合并静态 VB/IB，并支持 ResolveEntityByTriangle() 反向拾取
```

## MeshletSubmission

[`flare::MeshletSubmission`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/MeshletSubmission.h#L32) 位于 `FunctionLayer/Rendering`，负责网格簇的离线重排校验与双轨提交模式选择，不依赖任何 D3D12 头文件。

### SelectMeshletSubmissionMode

[`SelectMeshletSubmissionMode()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/MeshletSubmission.cpp#L137) 根据设备能力 `meshShaderTier1` 与策略开关做出确定性决策；当显卡不支持 Tier 1、关闭 `preferMeshShader` 或设置环境变量 `FLARE_DISABLE_MESH_SHADER=1`（见 [`D3D12Context.cpp#L204`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Context.cpp#L204)）时，自动无感回退到 [D3D12Heterogeneous](visibility.md) 的 Compute + `ExecuteIndirect` 路径：

```cpp
// Source/FunctionLayer/Rendering/MeshletSubmission.cpp#L137
MeshletSubmissionMode SelectMeshletSubmissionMode(const rhi::DeviceCapabilities& capabilities,
    const MeshletSubmissionSettings& settings) noexcept
{
    if (!settings.enabled) return MeshletSubmissionMode::Disabled;
    if (capabilities.meshShaderTier1 && settings.preferMeshShader) return MeshletSubmissionMode::MeshShader;
    return MeshletSubmissionMode::ComputeIndirect;
}
```

### BakeMeshletSubmission

[`BakeMeshletSubmission()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/MeshletSubmission.cpp#L111) 先调用 `BakeMeshlets()` 切分紧凑网格簇，再由 `FlattenMeshletSubmission()` 强校验全部有向三角形多重集合与源网格严格一致，输出全局源索引流与 `MeshletSubmissionRange` 表供双轨共用。

## D3D12Meshlet

[`D3D12Meshlet`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Meshlet.h#L93) 封装 SM 6.5 硬件 Amplification / Mesh Shader 执行器（`ID3D12GraphicsCommandList6::DispatchMesh`）。构建期由 `ShaderBuilder` 调用 [`WriteD3D12MeshletShader()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Meshlet.h#L19) 将 `MeshletCull.as.hlsl` 与 [`MeshletDraw.ms.hlsl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Resources/Shaders/MeshletDraw.ms.hlsl#L86) 离线编译为签名 `.sebin` DXIL 随包分发，运行时零 `dxcompiler.dll` 依赖。

Mesh Shader 内部使用 `groupshared` 256 槽开放寻址哈希表对每个簇（最多 64 三角形）动态去重源顶点索引，压缩输出顶点着色量：

```hlsl
// Resources/Shaders/MeshletDraw.ms.hlsl#L46（groupshared 256 槽原子顶点去重）
void InsertUniqueVertex(uint key)
{
    uint slot = HashVertexKey(key);
    [loop] for (uint probe = 0u; probe < 256u; ++probe)
    {
        uint cur = sHashKey[slot];
        if (cur == key) return;
        if (cur == 0xFFFFFFFFu)
        {
            uint prev = 0u;
            InterlockedCompareExchange(sHashKey[slot], 0xFFFFFFFFu, key, prev);
            if (prev == 0xFFFFFFFFu)
            {
                uint localIdx = 0u;
                InterlockedAdd(sUniqueCount, 1u, localIdx);
                sUniqueSourceVertex[localIdx] = key;
                sHashVal[slot] = localIdx;
                return;
            }
            if (prev == key) return;
        }
        slot = (slot + 1u) & 255u;
    }
}
```

### Prepare

[`D3D12Meshlet::Prepare()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Meshlet.h#L99) 按 PSO 桶整理候选绘制并生成 GPU `GpuMeshletItem` 与 `MeshletCullConstants`（256 字节）；[`RecordBucket()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Meshlet.h#L103) 在硬件模式下录制 `DispatchMesh`，并通过状态跟踪器保证 VB/IB 状态安全衔接后续 Pass。

## TransientAliasPlan

[`FrameGraph`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/FrameGraph.h#L100) 在纯逻辑编译阶段依据非历史瞬态纹理的生存期区间 `[firstPass, lastPass]`，规划 64 KiB 对齐的物理槽位分配方案 `TransientAliasPlan`，通过 [`FrameGraph::AliasPlan()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/FrameGraph.h#L139) 暴露给渲染编排层。

后端 [`D3D12MemoryAllocator`](device.md) 通过 `CreateTransientSlot()` 与 `CreateAliasedResource()` 在同一 `ID3D12Heap` 上放置生存期互不重叠的目标（如 `BloomB` 与 `LdrColor`）；当物理槽位所有权发生切换时，[`D3D12StateRegistry`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12StateTracker.h#L51) 唯一插入 `D3D12_RESOURCE_BARRIER_TYPE_ALIASING` + 资源状态转换 + `DiscardResource`，确保不破坏内存一致性。

## ComputeCascadeStaticShadowHashes

[`ComputeCascadeStaticShadowHashes()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/CascadeShadow.cpp#L212) 在纯数学层为每级级联计算 64 位静态投射物指纹哈希：融合当前级 `clipFromWorld`、太阳方向、分辨率、按 `(entityId, geometryKey, transformHash, maskMaterialHash)` 排序且与该级正交视锥相交的全部静态投射物。

- **缓存命中**：[`D3D12Shadow`](shadows.md) 持有 3 级 `staticCacheDepth` 底图，哈希一致时直接 GPU `CopyTextureRegion` 复制静态深度，再仅补绘动态/蒙皮物体（见 [`D3D12Shadow.cpp#L174`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Shadow.cpp#L174)）。
- **原子提交**：新缓存哈希和 `shadowCacheHits / shadowCacheMisses` 统计仅在 `ExecuteCommandLists` 与 `states.Commit()` 成功后原子落盘，避免失败帧污染缓存。

## StaticBatchCache

[`flare::StaticBatchCache`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/StaticBatchCache.h#L106) 按默认 **32 m** 空间网格单元与 `(materialLayoutVersion, pipelineKey, vertexFormatVersion, materialBindingKey)` 合并静态实体 VB/IB：
- 对负行列式缩放自动翻转三角形绕序与切线手性，严格匹配 `Pbr.hlsl` 的 `logicalFront` 契约；
- 仅在实体增删或属性变更时局部标记对应空间单元为脏，由 `RebuildDirty()` 增量重建；
- 受门禁函数 [`ShouldEnableStaticBatching()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/StaticBatchCache.h#L104) 控制，仅当合并后批次数显著少于 GPU 实例化、显存预算满足且实测提交耗时有正收益时才开启。

### ResolveEntityByTriangle

合批并不会丢失编辑器拾取能力：[`ResolveEntityByTriangle()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/StaticBatchCache.h#L124) 与 [`RaycastPick()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/StaticBatchCache.h#L125) 借助保留的 `StaticBatchSubmeshRange` 范围表，将合并批次中的任意三角形下标反向映射回原始 `StableEntityId` 与子网格槽。

## 昼夜 GPU 联动与 1080p SSGI 时域降噪

- **昼夜光照与大气联动**：[`ApplyTimeOfDayFrameData()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Application/FeatureFrameData.cpp#L139) 从纯 CPU `TODSystem` 快照读取太阳高度角、大气光学质量（Air Mass）消光系数与色温辐射度写入 `FrameData`，零侵入驱动日出到夜晚的实时阴影与天空过渡。
- **1080p SSGI 时域累积与 À-Trous 降噪**：[`D3D12IndoorLighting`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12IndoorLighting.h#L25) 通过 `D3D12IndoorHistory` 持有双帧乒乓历史与矩缓冲，执行重投影方差夹取与 3 趟法线/深度加权 À-Trous 滤波（详见 [环境光照、大气与反射](environment.md)）。

::: tip 性能验证与排障开关
- 验证双轨一致性时，设置环境变量 `FLARE_DISABLE_MESH_SHADER=1` 可强制把硬件 Mesh Shader 轨切换为 `ComputeIndirect` 轨，方便同镜头逐像素 A/B 比对。
- 在 `rhi::Statistics` 中可直接观测 `transientAliasedSavedBytes`、`aliasBarriers`、`shadowCacheHits` 与 `shadowCacheMisses`，确认各项优化是否在目标场景真实生效。
:::
