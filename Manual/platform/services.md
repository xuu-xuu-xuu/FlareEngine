# 基础服务、任务与诊断

`CoreLayer` 聚合了全引擎共用的底层基础设施：后台任务队列、双语本地化词表、空间包围盒索引、数学与编码哈希工具，以及贯穿 CPU 与 GPU 的性能统计、检查点日志和设备移除（DRED）故障转储。它们几乎不依赖上层引擎状态，可直接单独测试与复用。

## 基础服务全景

```text
                ┌─────────────────── CoreLayer（零渲染/业务依赖）───────────────────┐
                │                                                                   │
  后台计算 ────►│  TaskQueue + CancellationToken（工作线程产 CPU 结果，主线程 Pump） │
  双语界面 ────►│  Localization（zh-CN / en-US key=value + Check-Localization.ps1） │
  裁剪/射线 ───►│  spatial::BoundsIndex（静态 BVH / 动态 AABB 树）+ Math / Bounds   │
  编码/校验 ───►│  Utf（UTF-8/16/32）+ Sha256（Windows CNG BCryptHash）              │
  日志/诊断 ───►│  Log / LogCheckpoint（即时 flush）+ Profiler（CpuScope 6000 帧）   │
                └───────────────────────────────────────────────────────────────────┘
                                              │
                 ┌────────────────────────────┴────────────────────────────┐
                 ▼                                                         ▼
      PlatformLayer/Windows                                   Rendering & PlatformLayer/D3D12
   ProcessDiagnostics（UTF-8 控制台 + Logs/*.log）         RenderTelemetry（16 Pass / 240 帧环）
                                                           ReadD3D12GpuTimings() + DumpDred()
```

## TaskQueue 与 CancellationToken

[`TaskQueue`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/TaskQueue.h#L40) 把耗时 CPU 工作（如资产解码、烘焙、文件读取）交给后台线程池，但**严禁工作线程直接修改世界或渲染状态**——所有状态发布必须在所有者线程显式调用 `Pump()`：

```cpp
// Source/CoreLayer/TaskQueue.h
// Work 在后台线程执行，返回 Completion 闭包；主线程调用 Pump() 时再执行 Completion 发布结果。
using Completion = std::function<void()>;
using Work       = std::function<Completion(const CancellationToken&)>;
using Failure    = std::function<void(const TaskErrors&)>;
```

关键护栏设计（[`TaskQueue.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/TaskQueue.cpp#L45)）：

- **有界容量、永不阻塞生产者**：默认 2 个工作线程、上限 1024（允许配置 1..16 线程、1..65536 容量）；队列满或正在关闭时 `Submit()` 立即返回 `TaskStatus::Rejected` 而不卡主线程。
- **协作式取消**：[`CancellationToken`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/TaskQueue.h#L24) 供长任务周期性检查 `IsCancelled()` 主动退出；即使任务已进入就绪队列，取消后 `Pump()` 也会跳过其完成回调。
- **线程与重入校验**：[`Pump(limit)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/TaskQueue.cpp#L117) 与 [`Shutdown()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/TaskQueue.cpp#L180) 均校验调用线程是否为构造时的 `owner_` 线程并禁止重入；后台工作、完成发布与失败处理器三段异常分别记录在 `TaskErrors{work, completion, handler}`。
- **锁外安全释放**：`StopWorkers()` 先把未执行和未发布的任务队列 `swap` 到局部变量再解锁析构，避免被捕获对象的析构函数回调队列导致死锁。

## Localization 中英双语词表

[`Localization`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Localization.h#L9) 是轻量的 UTF-8 `key=value` 文本表，对应仓库中的 `Resources/Localization/zh-CN.txt` 和 `en-US.txt`：

- [`Load()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Localization.cpp#L48) 严格校验 UTF-8 合法性（拒绝截断、过长编码与代理项），允许 `#` 注释行并自动剥离 UTF-8 BOM；重复键或空表均返回失败并保留旧表不变。
- `GetText(key)` 返回 `std::string` 副本，缺失键时回退返回键名本身，绝不崩溃或返回空指针。
- **自动双语一致性守护**：[`Scripts/Check-Localization.ps1`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Scripts/Check-Localization.ps1#L1) 会比对 `zh-CN.txt` 与 `en-US.txt` 是否逐键一一对应，并扫描 `Source/`、`Samples/`、`Tests/Managed/` 中所有带模块前缀（如 `EditorUi.*`、`Project.*`、`D3D12.*`、`Script.*`）的字符串键，缺任何一个都会报错。

## BoundsIndex 空间索引与数学/编码/哈希

- **空间加速结构 [`flare::spatial::BoundsIndex`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Spatial/BoundsIndex.h#L20)**：不拥有业务对象，仅以非零 `uint64_t key` 索引 `Aabb`。静态场景调用 `Build(leaves)` 一次建好平衡 BVH；动态实体调用 `Upsert(leaf)` / `Remove(key)` 增量更新并在树高退化时自动 `RebuildIfDegenerate()`。支持包围盒重叠查询 `Query(Aabb)` 与射线查询 `Query(PreparedRay)`。
- **数学与投影**：[`Math.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Math/Math.h#L10) 提供列主序 `Mat4`、四元数 `Quat` 与 `Transform`；[`Projection.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Math/Projection.h#L27) 提供右手系、相机朝 `-Z`、D3D 深度 `[0, 1]` 的 `PerspectiveRH()` / `OrthographicRH()` 与严格校验正交性的 `InverseRigid()`；[`Bounds.h`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Math/Bounds.h#L15) 的 `TransformBounds()` 与 `BuildFrustum()` / `Intersects()` 在数值异常或非仿射矩阵时走保守保留路径，宁可多画也不把合法物体错误剔除。
- **UTF 与 SHA-256**：[`DecodeUtf8()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Utf.cpp#L6) / [`EncodeUtf16()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Utf.cpp#L79) 统一处理跨平台字符串边界；[`Sha256()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Hash.cpp#L7) 调用 Windows CNG `BCryptHash` 计算资产与构建输入摘要。

## Profiler、RenderTelemetry 与 GPU 计时

Flare 把性能数据清晰分为三条互补轨道：

1. **CPU 墙钟区段 [`Profiler`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Profiler.h#L27)**：配合 RAII [`CpuScope`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Profiler.h#L57) 记录各阶段耗时，默认保留最近 6,000 帧历史，可直接计算 `average / P50 / P95 / P99` 并通过 `ExportFramesCsv()` / `ExportSummaryCsv()` 落盘。
2. **渲染帧遥测环 [`RenderTelemetry`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/RenderTelemetry.h#L85)**：约定 16 个固定 [`RenderPassId`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/RenderTelemetry.h#L15) 槽位（`Shadow / Prepass / Hzb / ClusterCull / Sky / Opaque / AmbientOcclusion / ... / Gui / Composite / Other`），保留 240 帧固定结构 `RenderStats`，避免在渲染热路径上拼字符串或堆分配；迟到的 GPU 时间戳按 `submission` 序号回填到对应历史帧。
3. **D3D12 时间戳回读 [`ReadD3D12GpuTimings()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12GpuTimer.cpp#L11)**：每帧上下文配对 `ID3D12QueryHeap` 与回读缓冲，等待 `fence->GetCompletedValue()` 跨越后无阻塞 `Map` 读取，严格校验各 Pass 时间戳单调不倒退，再换算毫秒数入队。

## 日志、进程诊断与 DRED 设备移除

- **实时刷盘日志与检查点**：[`StartFileLog()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Log.cpp#L36) 打开日志文件后，每条 `Log()` 与 [`LogCheckpoint()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Log.cpp#L59) 都附带 UTC 时间戳和进程已运行毫秒数 `elapsed_ms` 并立即 `flush()`，确保崩溃或强制结束前总能读到最后一个检查点。[`SampleDiagnosticFrame()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Log.cpp#L51) 默认只记录前 3 帧及每 600 帧的心跳（可通过环境变量 `FLARE_DIAGNOSTIC_FRAMES=3..120` 临时放大开场采样窗口）。
- **进程启动诊断**：[`InitializeProcessDiagnostics()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/ProcessDiagnostics.cpp#L14) 通过 `std::call_once` 将控制台切为 UTF-8，优先按 `FLARE_LOG_DIRECTORY`（否则 `<exe>/Logs`）生成 `<role>-<UTC时间戳>-<PID>.log` 并登记 `Process.Start` / `Process.Exit` 检查点。
- **GPU 故障转储（DRED）**：设备创建前由 [`EnableDred()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Dred.cpp#L19) 强制开启自动面包屑与页错误追踪；一旦触发 `DXGI_ERROR_DEVICE_REMOVED`，[`DumpDred()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Dred.cpp#L39) 将每个命令列表最后完成的 GPU 操作序号、历史 op 序列以及 Page Fault 虚拟地址和相关存活/刚释放的资源名导出到文本报告，快速定位驱动崩溃或越界访问。

## 基础服务速查表

| 模块 / 函数 | 源码位置 | 关键限制或约定 |
| --- | --- | --- |
| `TaskQueue` / `CancellationToken` | [`TaskQueue.h#L40`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/TaskQueue.h#L40) | 仅所有者线程可 `Pump()` / `Shutdown()`；容量满立刻 `Rejected` |
| `Localization` | [`Localization.h#L9`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Localization.h#L9) | `key=value` 严格 UTF-8；缺失回退键名；双语同步由 `Check-Localization.ps1` 守卫 |
| `spatial::BoundsIndex` | [`BoundsIndex.h#L20`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Spatial/BoundsIndex.h#L20) | 非零 `uint64_t` key；支持静态 `Build` 与动态 `Upsert/Remove` |
| `Profiler` / `CpuScope` | [`Profiler.h#L27`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Profiler.h#L27) | 嵌套区段包含子区段耗时，切勿简单相加当总帧时间 |
| `RenderTelemetry` | [`RenderTelemetry.h#L85`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/RenderTelemetry.h#L85) | 16 固定 Pass 槽位 + 240 帧环形缓冲 + CSV 导出 |
| `StartFileLog()` / `LogCheckpoint()` | [`Log.cpp#L36`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/CoreLayer/Log.cpp#L36) | 逐条 flush；可用 `FLARE_LOG_DIRECTORY` 指定日志输出目录 |
| `EnableDred()` / `DumpDred()` | [`D3D12Dred.cpp#L19`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Dred.cpp#L19) | 须在创建 `ID3D12Device` 前开启；设备移除后落盘面包屑与 VA |

::: tip 排查启动闪退或渲染卡死的 30 秒步骤
1. 打开 `<exe>/Logs/`（或 `FLARE_LOG_DIRECTORY` 指向的目录）下最新的 `*.log`，看最后一条 `stage=...` 检查点停在哪个阶段。
2. 如需抓开场更多帧日志，启动前设置 `$env:FLARE_DIAGNOSTIC_FRAMES = 60`。
3. 若遇到 `DEVICE_REMOVED`，直接查看同目录生成的 DRED 报告里的 `last completed breadcrumb` 与 `page fault VA`。
:::
