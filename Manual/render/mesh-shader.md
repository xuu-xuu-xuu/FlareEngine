# Mesh Shader 双轨管线与渲染深水区优化

本章汇总阶段四落地的高阶 GPU 渲染能力：硬件 Amplification/Mesh Shader（SM 6.5）与 Compute 间接双轨提交、FrameGraph 物理显存别名、逐级静态阴影缓存、空间网格静态合批，以及昼夜光照联动与 1080p SSGI 时域降噪。全部实现严格遵守 `FunctionLayer/Rendering` 纯逻辑编排与 `PlatformLayer/Graphics/D3D12` 独占硬件执行的单向边界。

## MeshletSubmission

`MeshletSubmission` 位于 `FunctionLayer/Rendering`，负责网格簇（Meshlet）离线切分与运行时提交模式判定，不引入任何 D3D12 头文件。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/MeshletSubmission.h#L32)

### SelectMeshletSubmissionMode

`MeshletSubmissionMode SelectMeshletSubmissionMode(const rhi::DeviceCapabilities&, const MeshletSubmissionSettings&)` 根据设备 `meshShaderTier1` 能力与策略开关在 `Disabled`、`ComputeIndirect`（`RG104-17.01`）与 `MeshShader`（`RG104-17.02`）之间做出确定性选择。硬件不支持 Tier 1、关闭 `preferMeshShader` 或设置 `FLARE_DISABLE_MESH_SHADER=1` 时自动无感回退至 `D3D12Heterogeneous` 的 Compute + `ExecuteIndirect` 路径。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/MeshletSubmission.h#L23)

### BakeMeshletSubmission

`bool BakeMeshletSubmission(const MeshData&, MeshletSubmission&, std::vector<AssetDiagnostic>&)` 将静态网格重排为局部包围盒紧凑的 Meshlet 簇表供双轨共用。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/MeshletSubmission.h#L45)

## D3D12Meshlet

`D3D12Meshlet` 位于 `PlatformLayer/Graphics/D3D12`，专门封装 SM 6.5 硬件 Amplification/Mesh Shader 管线（`RG104-17.02`，`ID3D12GraphicsCommandList6::DispatchMesh`）；当选择 `ComputeIndirect` 回退轨时由 [D3D12Heterogeneous](visibility.md) 执行 Compute 视锥/HZB 剔除与 `ExecuteIndirect`。SM 6.5 着色器（`MeshletCull.as.hlsl` 与 `MeshletDraw.ms.hlsl`）由构建期 `ShaderBuilder meshlet-ms` 调用 Windows SDK DXC 离线编译为签名 `.sebin` DXIL 随包分发，运行时零 `dxcompiler.dll` 依赖；Mesh Shader 内部以 `groupshared` 256 槽哈希表对每 64 三角形面片做顶点去重以压缩输出顶点量。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/Graphics/D3D12/D3D12Meshlet.h#L93)

### Prepare

`HRESULT Prepare(...)` 批量整理候选绘制并按 PSO 桶生成 GPU Meshlet 描述与常量；`RecordBucket()` 在硬件模式录制 `DispatchMesh` 调用，并在执行前后由状态跟踪器保证顶点/索引缓冲资源状态正确衔接后续 Pass。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/Graphics/D3D12/D3D12Meshlet.h#L99)

## TransientAliasPlan

`FrameGraph` 在纯逻辑层根据最终 Pass 拓扑区间 `[firstPass, lastPass]` 为非历史瞬态纹理规划 64 KiB 对齐物理槽位 `TransientAliasPlan`（`RG104-19`），经 [FrameGraph::AliasPlan()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/FrameGraph.h#L139) 暴露给编排层。[D3D12MemoryAllocator](device.md) 通过 `CreateTransientSlot` 与 `CreateAliasedResource` 在同一对齐 `ID3D12Heap` 上放置互不重叠的后处理瞬态目标（如 `Bloom.Pong` 与 `ToneMap.Ldr`），所有权切换时由 `D3D12StateTracker` 唯一发出 `D3D12_RESOURCE_BARRIER_TYPE_ALIASING` + 状态转换 + `DiscardResource`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/FrameGraph.h#L100)

## ComputeCascadeStaticShadowHashes

`ComputeCascadeStaticShadowHashes()` 位于 `CascadeShadow` 纯数学层（`RG104-18`），融合各级 `clipFromWorld`、光照方向、分辨率、排序静态投射物变换与 `Mask` 材质参数，并按正交视锥重叠计算逐级 64 位哈希。[D3D12Shadow](shadows.md) 持有 3 级静态深度缓存切片，命中时通过 GPU `CopyTextureRegion` 复制静态底图后仅补绘动态与蒙皮投射物；缓存哈希与统计仅在 `ExecuteCommandLists` 与 `states.Commit()` 之后原子提交。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/CascadeShadow.h#L52)

## StaticBatchCache

`StaticBatchCache` 位于 `FunctionLayer/Rendering`（`RG104-14`），按空间网格单元（默认 32 m）与 `(MaterialLayout, PipelineKey, VertexFormat, MaterialBinding)` 合并静态实体 VB/IB（负行列式缩放自动翻转三角形绕序与切线手性以严格匹配 `Pbr.hlsl` 的 `logicalFront` 契约），仅在实体增删或属性变化时局部重建对应空间单元；由 [ShouldEnableStaticBatching()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/StaticBatchCache.h#L104) 门禁控制在相对 GPU 实例化存在正收益时才启用。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/StaticBatchCache.h#L106)

### ResolveEntityByTriangle

`StableEntityId ResolveEntityByTriangle(std::size_t batchIndex, std::uint32_t triangleIndex, std::uint32_t* outSubmeshSlot)` 与 `RaycastPick()` 基于保留的子网格范围表 `StaticBatchSubmeshRange` 反向定位合并批次中任意三角形对应的编辑器 `StableEntityId`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Rendering/StaticBatchCache.h#L124)

## 昼夜 GPU 联动与 1080p SSGI 时域降噪

- **TOD 光照与大气联动（`E07`）**：[ApplyTimeOfDayFrameData()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Application/FeatureFrameData.cpp#L126) 从纯 CPU `TODSystem` 快照提取太阳高度角、色温辐射度与大气瑞利散射参数写入 `FrameData`，在不污染 `TODSystem` 职责的前提下实现日出到夜晚的实时阴影与天空过渡。
- **1080p SSGI 时域累积与 À-Trous 降噪**：[D3D12IndoorLighting](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/Graphics/D3D12/D3D12IndoorLighting.h#L25) 持有双帧乒乓历史缓冲（`RGBA16F` 辐射度 + 矩统计），经深度/法线重投影方差夹取与 3 趟法线-深度加权 À-Trous 空域滤波输出稳定间接光，切场景或窗口尺寸变化时自动重置历史有效位。
