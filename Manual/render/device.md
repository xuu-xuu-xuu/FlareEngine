# D3D12 设备、命令与 GPU 资源寿命

这篇文档讲清引擎如何在 `flare::rhi::Device` 背后封装 D3D12 硬件资源：命令录制与 Fence 寿命管理、Placed Heap 显存分配与别名、描述符池与唯一状态追踪器、帧上传 Arena，以及着色器与 PSO 持久管线库缓存。

## RHI 与 D3D12 后端模块关系

```text
FunctionLayer/Rendering（纯逻辑编排，仅见 flare::rhi 句柄）
        │
        ▼
flare::rhi::Device (CreateD3D12 / Create* / BeginCommands / Submit / Collect)
        │
        ├──▶ D3D12MemoryAllocator    ── DEFAULT Heap 显存池、预算控制、Placed & Aliased 资源
        ├──▶ D3D12DescriptorPool     ── 持久 SRV/RTV/DSV 堆 + 每帧可见描述符租约
        ├──▶ D3D12StateRegistry      ── 全设备唯一子资源状态表、屏障/UAV/Aliasing 屏障生成
        ├──▶ D3D12UploadArena        ── 每帧槽 64 KiB 映射上传页（Fence 完成后整页 Reset）
        └──▶ D3D12PipelineCache      ── 源码/字节码缓存 + 根签名哈希 PSO 键 + Release 持久管线库
```

## 记录、提交与资源寿命（RHI 契约）

上层渲染代码只接触 [`flare::rhi::Device`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/Rhi.h#L679) 和 [`flare::rhi::CommandContext`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/Rhi.h#L634)：

1. **创建设备**：调用 [`Device::CreateD3D12()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/Rhi.h#L685) 初始化适配器、直接命令队列与主窗口呈现上下文。
2. **录制命令**：通过 [`BeginCommands()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/Rhi.h#L715) 拿到命令上下文，依次 `Configure*()`、`BeginPass()`、`Draw*()`、`EndPass()`、`Close()`。
3. **异步提交与延后回收**：调用 [`Submit()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/Rhi.h#L718) 返回 `CompletionToken`。设备把该批次借用的所有缓冲、纹理、管线和描述符租约挂在在途队列上；即使上层调用 `Destroy()` 销毁句柄，底层 COM 资源也会等到对应 **Fence 完成**且 `Collect()` 轮询后才真正释放。

```cpp
// Source/PlatformLayer/Graphics/Rhi.h#L715（标准单帧提交骨架）
auto commands = device->BeginCommands(frameIndex);
commands->ConfigureView(viewProjection);
commands->BeginPass({rhi::PassKind::Clear, rhi::ResourceUse::ColorWrite, rhi::ResourceUse::DepthWrite, "Clear"});
commands->EndPass();
// ... 录制 Shadow / Opaque / PostProcess / Output Pass ...
commands->Close();

rhi::CompletionToken token;
device->Submit(*commands, token); // 非阻塞提交；仅当显式传入 capture 指针时才等待读回
device->Collect();                // 回收已完成 Fence 对应的延后释放资源
```

## 分配、状态追踪与上传

| 后端组件 | 源码位置 | 核心机制 |
| --- | --- | --- |
| `D3D12MemoryAllocator` | [`D3D12Memory.h#L24`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Memory.h#L24) | 统一管理 DEFAULT 堆（Placed Heap + 可选 Committed 回退）、显存预算上限，并提供 `CreateTransientSlot()` / `CreateAliasedResource()` 支持 FrameGraph 显存别名；通过资源私有元数据绑定 Allocation Lease，不额外持有资源 COM 强引用 |
| `D3D12DescriptorPool` | [`D3D12Descriptors.h#L33`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Descriptors.h#L33) | 分配持久 SRV/RTV/DSV 描述符范围，释放同样走延后回收队列直到在途 Fence 完成 |
| `D3D12StateRegistry` / `D3D12StateRecording` | [`D3D12StateTracker.h#L51`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12StateTracker.h#L51) | 全设备唯一的资源状态真相源（覆盖子资源状态、UAV 屏障与别名槽所有权切换）。录制阶段写入局部暂存表，**仅在 `ExecuteCommandLists` 之后调用 `Commit()` 原子写回**，录制失败直接丢弃不污染全局状态 |
| `D3D12UploadArena` | [`D3D12UploadArena.h#L20`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12UploadArena.h#L20) | 归属各 `FrameContext`，以 64 KiB 映射 UPLOAD 页线性切分每帧常量与动态实例切片（默认预算 256 MiB），该帧槽 Fence 完成后整页 `Reset()` 复用 |
| `RecordD3D12TextureRows()` | [`D3D12StagedUpload.cpp#L44`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12StagedUpload.cpp#L44) | 配合 `CreateStagedTexture` / `UploadTextureRows` 按行分块复制大纹理数据，支持 `BeginUploadBatch()` / `EndUploadBatch()` 合并提交 |

## 着色器、PSO 缓存与预热

- **着色器加载与校验**：[`LoadD3D12ShaderProgram()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L693) 读取预编译产物或调用编译，按源码 + 宏 + 依赖头 + 布局版本做 SHA-256 缓存键。
- **PSO 缓存与持久管线库**：[`D3D12PipelineCache`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12PipelineCache.h#L31) 先序列化根签名并将其 SHA-256 编入 PSO 键（防止不同根参数/采样器组合撞键，见 [`D3D12PipelineCache.cpp#L247`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12PipelineCache.cpp#L247)）。在非调试层 Release 环境下启用磁盘 `.cache/pso/library-v5-*.bin` 持久库加速二次启动。
- **库底层字节保活**：[`RetainD3D12PipelineLibraryStorage()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12PipelineLibraryStorage.cpp#L26) 通过 `ID3D12Object::SetPrivateDataInterface` 把持久库依赖的二进制缓冲绑定到 `ID3D12PipelineLibrary` 对象寿命上，确保库释放前底层内存有效。
- **管线预热**：[`WarmupPipelines()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/PipelineWarmup.cpp#L6) 在首帧渲染前批量预创建所需变体，并统计新建数、缓存命中数与耗时，消除运行时首次遇新材质卡顿。

::: warning D3D12 调试层与持久管线库说明
- Debug 构建或显式启用 D3D12 调试层时，`D3D12PipelineCache` 会主动跳过磁盘持久库（避免驱动库未命中触发冗余校验报错），仅保留进程内弱引用缓存。
- 调整任何 `MaterialLayout.h` 结构体或根签名槽位时，请同时检查 `D3D12PipelineCache.cpp` 中的 `libraryVersion`，避免加载到旧版布局的缓存库条目。
:::
