# 交换链、绘制与读回

这篇文档讲清一个渲染窗口从上下文初始化、交换链重建、每帧后端 Pass 录制到离屏截图读回与性能剖析导出的完整生命周期。如果说 [device.md](device.md) 讲的是底层资源管理器，本页讲的就是“这些资源如何在一帧里真正画到屏幕上”。

## 窗口呈现与每帧录制流程

```text
InitializeD3D12Context()（DXGI 工厂 / 适配器 / ID3D12Device / 能力探测）
        │
        ▼
InitializeD3D12Presentation()（SwapChain / BackBuffers / D32 Depth / FrameContext[] / Fence）
        │  窗口尺寸变化时 → ResizeD3D12Presentation()（先等 GPU 完成，再安全重建尺寸相关目标）
        ▼
每帧录制（D3D12Presentation::Present / Submit）
  ├─ Shadow Pass         ── RecordD3D12Shadow()（CSM + 局部灯阴影）
  ├─ DepthPrepass / DN   ── D3D12DepthPrepass / D3D12DepthNormals
  ├─ Compute Pre-Opaque  ── D3D12LightClustering / D3D12AmbientOcclusion / D3D12RayTracingPreOpaque
  ├─ Opaque & Particles  ── RecordD3D12Meshes() / D3D12Heterogeneous / D3D12Meshlet
  ├─ Indoor & RT Post    ── RecordD3D12IndoorLighting() / D3D12RayTracingPostOpaque
  ├─ Transparent & Part. ── RecordD3D12Meshes(transparent) / RecordD3D12Particles()
  ├─ PostProcess / Out   ── RecordD3D12PostProcess() / RecordD3D12ForwardOutput()
  └─ 可选读回 / 诊断     ── Capture(LDR/HDR/Shadow/DepthNormals) → WritePpmImage() / CSV / FrameGraph DOT
```

## 每个窗口的呈现资源

- **全局图形上下文**：[`InitializeD3D12Context()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Context.cpp#L251) 创建 DXGI Factory、枚举适配器、初始化 `ID3D12Device` 并探测硬件特性（如 `meshShaderTier1`、DXR 1.1 `RayTracingTier1_1`）。
- **单窗口呈现状态**：[`InitializeD3D12Presentation()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Presentation.cpp#L221) 为窗口创建 DXGI 交换链、默认深度缓冲、多帧槽 `D3D12FrameContext`（独立命令分配器、上传 Arena、时间戳查询堆）以及共享 `ID3D12Fence`。编辑器的浮动视口也拥有独立呈现目标，但复用同一设备的直接命令队列以保持单队列状态追踪契约。
- **安全缩放重建**：[`ResizeD3D12Presentation()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Presentation.cpp#L1562) 显式等待所有引用旧交换链与旧分辨率离屏目标的 GPU 工作完成后，再释放并按新尺寸重建缓冲，同时使历史缓冲（TAA / Hi-Z / SSGI / RT SVGF）失效重来。

## 一次绘制在后端做什么

当上层通过 `CommandContext` 提交绘制后，D3D12 后端由四个核心录制模块落地硬件命令：

| 后端模块 | 源码位置 | 职责 |
| --- | --- | --- |
| `RecordD3D12Meshes()` | [`D3D12Mesh.cpp#L215`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Mesh.cpp#L215) | 绑定 PSO、根签名、视口/裁剪区、逐绘制 `b0`（320 B）、帧/阴影/扩展常量与纹理描述符表，支持绑定去重缓存统计并调用 `DrawIndexedInstanced` |
| `D3D12DepthPrepass` | [`D3D12DepthPrepass.h#L8`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12DepthPrepass.h#L8) | 在遮挡剔除模式下先写入近处遮挡物深度，为 Hi-Z 金字塔和主通道 `ReadOnlyLessEqual` 提前深度测试提供输入 |
| `RecordD3D12PostProcess()` | [`D3D12PostProcess.cpp#L147`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12PostProcess.cpp#L147) | 依次驱动 TAA、自动曝光、Bloom、ToneMap、FXAA 与叠加层，支持 FrameGraph 瞬态别名目标 |
| `RecordD3D12ForwardOutput()` | [`D3D12Forward.cpp#L138`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Forward.cpp#L138) | 在未启用完整后处理时，把线性 HDR 场景结果（可选采样 33³ LUT strip 或叠加阴影预览）直接输出到目标 RTV |

所有 CPU 常量结构体与 HLSL `cbuffer` 之间的二进制契约均由 [`MaterialLayout.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/MaterialLayout.h#L7) 中的 `static_assert(sizeof(...) && offsetof(...))` 和显式布局版本号（如 `PbrMaterialLayoutVersion=2`、`DepthNormalLayoutVersion=39`、`ShadingExtensionLayoutVersion=40`、`ParticleLayoutVersion=41`、`IndoorLightingLayoutVersion=42`）在编译期强校验。

## 截帧读回与性能诊断输出

引擎提供非侵入式的截帧与剖析接口，方便自动化像素回归测试和性能瓶颈定位：

```cpp
// Source/FunctionLayer/Rendering/SandboxRenderer.h#L44
renderer.SetCaptureOutput("captures/frame.ppm", /*frame=*/30); // 指定第 30 帧读回保存
renderer.SetProfileOutput("captures/profile");                 // 导出 CPU/GPU 耗时 CSV
```

| 调试能力 | 源码位置 | 说明 |
| --- | --- | --- |
| 多通道 GPU 读回 | [`Rhi.h#L718`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/Rhi.h#L718) | `Device::Submit()` 支持按需读回 LDR 颜色（`Image`）、线性 HDR（`HdrImage`）、阴影深度、分簇表、场景深度、`DepthNormals` 与屏幕空间遮蔽；仅在传入非空指针时执行同步等待 |
| 指定帧截帧 | [`SandboxRenderer.h#L46`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/SandboxRenderer.h#L46) / [`ImageWriter.cpp#L5`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/ImageWriter.cpp#L5) | `SetCaptureOutput()` 安排读回并通过 `WritePpmImage()` 写出无损 PPM 像素图；也可用 `SetMemoryCapture(true)` 直接留在内存中供测试断言 |
| CPU / GPU 耗时导出 | [`Profiler.cpp#L35`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Profiler.cpp#L35) / [`Rhi.h#L731`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/Rhi.h#L731) | `WriteProfileCsv()` 导出 CPU 采样；`Device::ExportGpuTimingsCsv()` 与 `ExportGpuPassTimingsCsv()` 导出逐帧及逐 Pass GPU 时间戳耗时 |
| 帧图拓扑导出 | [`FrameGraph.cpp#L896`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/FrameGraph.cpp#L896) / [`#L952`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/FrameGraph.cpp#L952) | `WriteDot()` 与 `WriteJson()` 导出完整 Pass 依赖图、纹理生存期区间与物理显存别名分组 |

::: tip 排查黑屏或读回花屏的快速路径
1. 先用 `Device::DebugMessages()` 检查是否存在 D3D12 调试层资源状态或根签名告警。
2. 导出 `FrameGraph::WriteDot()` 确认目标 Pass 未被死 Pass 剔除。
3. 依次传入 `shadowCapture`、`depthNormalCapture` 与 `hdrCapture` 做逐阶段读回，快速区分问题发生在阴影/几何、光照着色还是后处理映射阶段。
:::
