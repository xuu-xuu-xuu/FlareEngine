# 异步加载、分帧上传与纹理驻留

大体积模型与高分辨率贴图如果在主线程同步解码、一次性上传，必然造成明显掉帧或显存溢出。Flare 将这一过程拆成三个职责清晰、单向协作的组件：**`AsyncAssetLoader`（CPU 异步解码与状态机）**、**`AssetUploadScheduler`（按帧预算切片上传 GPU）** 和 **`TextureResidency`（LRU 纹理 mip 显存裁剪）**。

## 异步状态机与三组件协作流

```text
【AssetLoadState 状态流转】
  Unloaded ──(Request)──▶ LoadingCPU ──(Worker完成)──▶ CpuReady
                                                          │
                         ┌── target == CpuOnly ───────────┼──▶ Ready (仅 CPU)
                         │                                │
                         │   target == Renderable         ▼ (Pump 校验依赖并签发 UploadToken)
                         │                          UploadQueued
                         │                                │ (Scheduler::BeginUpload)
                         │                                ▼
                         │                          UploadingGPU (按帧预算分片上传 + Fence)
                         │                                │ (Fence 完成 → CompleteUpload)
                         │                                ▼
                         └── 失败 → Failed / 取消 → Cancelled ◀── 下一次 Pump 复核存活后发布 Ready

【每帧主循环协同】
  1. 业务层   : ticket = loader.Request(assetId, Model/Image, Renderable)
  2. 后台线程 : TaskQueue 执行 LoadCpuAssetSnapshot() ──▶ 扣减 CpuBudget 形成 CpuReservation
  3. 主线程   : loader.Pump()          ──▶ 合并解码完成项，推入 QueuedUploads / 发布已验收回执
  4. 渲染线程 : uploadScheduler.Tick() ──▶ 按单帧字节/任务预算切片拷贝 (Buffer/TextureRows)
                                          └─ Fence 完成后调 loader.CompleteUpload(receipt)
  5. 纹理流送 : residency.Touch() / TrimToBudget() ──▶ 显存吃紧时按 LRU 递增 desiredMip
                └─ 补传精细 mip 时调 rhi::Device::UploadTextureSubresourcesRange()
```

## AsyncAssetLoader

[`flare::AsyncAssetLoader`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AsyncAssetLoader.h#L101) 接收 `(AssetId, AsyncAssetKind, AssetTarget)` 请求，返回轻量化句柄 [`AssetTicket`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AsyncAssetLoader.h#L62)（`Ticket.Cancel()` 支持跨线程调用，其余方法限定在构造线程调用）。

```cpp
// Source/ResourceLayer/Assets/AsyncAssetLoader.h#L8-L10 & #L91-L95
enum class AsyncAssetKind { Image, Model };
enum class AssetTarget    { CpuOnly, Renderable };
enum class AssetLoadState { Unloaded, LoadingCPU, CpuReady, UploadQueued, UploadingGPU, Ready, Failed, Cancelled };

struct AsyncAssetLimits
{
    std::size_t   operations    = 128;               // 最大并发资产记录数（上限 4096）
    std::size_t   subscriptions = 512;               // 最大活跃 Ticket 订阅数（上限 65536）
    std::uint64_t cpuBytes      = 256ull * 1024 * 1024; // CPU 解码暂存总预算（默认 256 MiB）
};
```

### 关键工程设计

1. **多请求自动去重合并**：多个系统同时 `Request()` 同一 `(kind, id)` 时复用同一条 `AssetRecord`（统计记入 `sharedRequests`），后台只解码一次；只要仍有任意一个未取消的 `AssetTicket` 存活，数据保持有效；全部取消后 `Prune()` 自动取消后台任务并释放 CPU/GPU 租约。
2. **后台零锁独立解码**：[`LoadCpuAssetSnapshot()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AsyncAssetLoader.cpp#L135) 只持有主线程复制出的 `AssetSourceSnapshot`，在工作线程完成模型解析、布局展平（`BuildStaticModelLayout`）和全部关联贴图解码（`PrepareModelImages`），不触碰主线程的 `AssetStore`。
3. **RAII CPU 预算记账**：解码成功后通过 `CpuBudget::Acquire(bytes)` 记账并把所有 `shared_ptr` 别名绑定到 `CpuReservation`（[`AsyncAssetLoader.cpp#L202-L210`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AsyncAssetLoader.cpp#L202)），即使外部持有 `CpuAssetData` 子指针，直到最后一个引用释放才会归还 `cpuBytes` 预算。
4. **防陈旧上传回执**：每次排队上传都会签发四元组 [`AssetUploadToken { session, operation, revision, attempt }`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AsyncAssetLoader.h#L44)。[`CompleteUpload()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AsyncAssetLoader.cpp#L344) 收到过期或已取消任务的回执会直接拒绝（记入 `staleReceipts`）；合法回执也先暂存，等到下一次 [`Pump()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AsyncAssetLoader.cpp#L294) 再次确认需求依然存在才正式发布 `Ready`。
5. **内置占位数据**：在资源尚未 `Ready` 前，业务层可直接调用 [`AsyncAssetLoader::Placeholder(kind)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AsyncAssetLoader.cpp#L374) 获取默认品红棋盘格贴图或单位立方体模型避免画面空白。

## AssetUploadScheduler：分帧预算上传

[`flare::AssetUploadScheduler`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/AssetUploadScheduler.h#L25) 衔接 `AsyncAssetLoader` 与 `rhi::Device`，将待上传队列切成小块平摊到多帧提交，避免单帧上传峰值：

```cpp
// Source/FunctionLayer/Rendering/AssetUploadScheduler.h#L8-L24
struct RenderImageAsset final : RenderAssetLease { rhi::Texture texture; /* ... */ };
struct RenderModelAsset final : RenderAssetLease { std::shared_ptr<const StaticRenderModel> model; /* ... */ };

struct AssetUploadBudget
{
    std::uint64_t bytes = 256 * 1024; // 单帧 staging 预算（默认 256 KiB，允许 512 B .. 64 MiB）
    std::size_t   tasks = 8;          // 单帧最多执行的上传子片数
};
```

每帧调用 [`Tick(AssetUploadStep& output)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/AssetUploadScheduler.cpp#L90) 的执行顺序：
1. **验收已完成的 Fence**：遍历在途候选，若所有子任务已发完且 `device.IsComplete(c.last)` 为 `true`，封装 `RenderImageAsset` / `RenderModelAsset` 租约回传给 `resources.CompleteUpload(...)`。
2. **接纳新队列项**：从 `resources.QueuedUploads()` 拉取最多 `budget.tasks` 个待上传项，对模型自动按 `ImageContentKey` 去重相同贴图，生成顶点缓冲、索引缓冲与贴图 `Work` 清单。
3. **轮询切片上传**：按剩余 staging 字节预算对大缓冲调用 `UploadBufferRange`、对大纹理按行调用 `UploadTextureRows`；所有切片合并在单次 `BeginUploadBatch()` / `EndUploadBatch(token)` 批提交内，仅记录 `rhi::CompletionToken`，**绝不为取消而调用 `WaitIdle()`**（见 [`Shutdown()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/AssetUploadScheduler.cpp#L169)）。

GPU 资源的实际析构时机另由后端提交 Fence 保障，详见[设备与 Fence](../render/device.md)。

## TextureResidency：纹理显存预算与 LRU 裁剪

[`flare::TextureResidency`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/TextureResidency.h#L20) 是纯策略层的纹理显存预算器（默认预算 `512 MiB`），不直接做 I/O 或 RHI 调用，只负责决定每张纹理当前允许驻留的最细 mip 级别 `desiredMip`：

```cpp
// Source/FunctionLayer/Rendering/TextureResidency.cpp#L75-L86
void TextureResidency::Trim()
{
    while (loadedBytes_ > budget_)
    {
        TextureResidencyEntry* victim = nullptr;
        for (auto& [_, entry] : entries_)
            if (entry.desiredMip + 1 < entry.mipCount &&
                (!victim || entry.lastUsedFrame < victim->lastUsedFrame))
                victim = &entry;
        if (!victim) break; // 所有纹理都已退到最粗的一级 mip，保留最小可用表示
        ++victim->desiredMip;
        Recompute(*victim);
    }
}
```

### 常用接口速查

| 方法 | 作用 |
| :--- | :--- |
| [`Request(id, mipCount, desiredMip, baseBytes, frame, minimumBytes)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/TextureResidency.cpp#L22) | 登记或更新纹理驻留区间 `[desiredMip, mipCount)` 并立即触发 `Trim()`；返回最终是否在预算内满足请求的精度。`minimumBytes` 通常填压缩块大小（BC1/BC4 为 8 B，BC5/BC7 为 16 B）。 |
| [`Touch(id, frame, desiredMip)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/TextureResidency.cpp#L62) | 每帧渲染用到该纹理时刷新 `lastUsedFrame`，可选同时更新目标 mip。 |
| [`SetDesired(id, desiredMip)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/TextureResidency.cpp#L49) | 当预算回落有余量时，将已降级纹理恢复到更精细的 mip。 |
| [`TrimToBudget()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/TextureResidency.cpp#L36) | 显式执行 LRU 裁剪并返回本次发生降级的纹理数量。 |

当流送系统根据 `TextureResidency` 的决策需要补传局部精细 mip 区间时，由 RHI 的 [`rhi::Device::UploadTextureSubresourcesRange()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/Rhi.h#L697) 将 `[firstMip, mipCount)` 范围子资源记录到上传命令列表。

::: warning 调试与调优提示
- `AsyncAssetLoader::Stats()` 与 `StatusRows()` 可直接查看各阶段数量、`cpuBytes / peakCpuBytes`、`sharedRequests` 与 `staleReceipts`，适合排查资源泄漏或预算过紧。
- 如果 `AssetUploadScheduler::Tick()` 报 `Assets.UploadBudgetTooSmall`，说明单行纹理或单步长缓冲的对齐 staging 占用超过了 `AssetUploadBudget::bytes`（例如超高分辨率贴图单行字节超过预算），需要调大 `SetBudget()`。
:::
