# 固定步连续抓帧与 MP4 编码

玩家宿主把每次固定步输出画面逐帧写成无损 PPM 序列并签发 `frames.json`，独立工具 `VideoEncoder.exe` 再用 Windows Media Foundation 将序列编码为恒定帧率、字节级可复现的 H.264 MP4，整个过程不依赖 FFmpeg，且磁盘写入速度绝不影响成片时长。可直接执行的 PowerShell 命令见 [抓帧与编码操作](../user/record-video.md#capture)。

## 固定步抓帧与编码两阶段流程

```text
阶段 1：玩家宿主连续抓帧（逻辑时间与 I/O 完全解耦）
  EngineSession.SetWindowResolution(W, H)
  EngineSession.SetWindowFixedStep(1.0 / fps)     # 缺省 1/30 秒 = 每帧推进 2 个 60Hz 物理步
  EngineSession.SetCaptureSequence(dir, count)    # 上限 100,000 帧，与单帧抓帧互斥
  EngineSession.RunWindow(visible: false)
     │
     ├─ 第 0 帧：步进 dt = 0.0（先拍初始化后的 tick 0 初态）──► frame_0000.ppm
     ├─ 第 1..N-1 帧：每帧步进 fixedStep（世界模拟恒定 60 Hz）──► frame_0001..N-1.ppm
     └─ 逐帧核对文件存在且字节数 > 0 ──► 写 frames.json.tmp ──► 原子重命名为 frames.json

阶段 2：VideoEncoder.exe 离线封装与回读验证（Windows Media Foundation，无需 FFmpeg）
  VideoEncoder.exe --frames <dir> --count <N> --fps <fps> --output clip.mp4 --manifest report.json
     │
     ├─ ScanFrameSequence()       核对编号连续、无重复、P6 8-bit、全帧同尺寸且宽高为偶数
     ├─ RGB → NV12 + H.264 MFT    单线程、High profile、GOP 30、B-frame 0、CBR（1080p=12Mbps 按像素缩放）
     ├─ NormalizeMp4Timestamps()  把 mvhd/tkhd/mdhd 时间戳归一，实现同批帧字节级可复现
     ├─ MoveFileExW 原子替换      从 .partial-*.mp4 切到目标 output.mp4
     └─ VerifyMp4File()           重新用 MF Source Reader 解码全片，校验样本数、恒定帧率与时长
```

## EngineSession

在已经加载场景与资产的托管会话上按如下顺序配置即可开始离线录制。`directory` 应使用本次录制专用的空目录；`visible: false` 隐藏窗口时仍正常执行完整 D3D12 渲染与后处理。

```csharp
session.SetWindowResolution(640, 360);
session.SetWindowFixedStep(1.0 / 30.0);
session.SetCaptureSequence(directory, 60);
session.RunWindow(visible: false);
```

### SetCaptureSequence

`SetCaptureSequence(string? directory, ulong count)` 会自动创建输出目录并登记序列抓帧配置：

- 最多允许录制 **100,000 帧**；传空目录（`null` 或 `""`）或 `count == 0` 可清除配置。
- 必须在调用 `RunWindow()` 之前设置；窗口运行或步进期间调用会返回 `Bridge.SessionBusy`。
- 不能与单帧抓帧 `SetWindowCapture()` 同时启用，否则 `RunWindow()` 返回 `Bridge.InvalidArgument`。
- **源码位置**：[托管入口 `EngineSession.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L151) · [原生参数检查 `NativeSessionExports.inl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativeSessionExports.inl#L334)

### RunWindow

序列模式下，[`flare_session_run_window()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativeRenderExports.inl#L1) 强制把宿主更新步长与渲染帧一一绑定：

```cpp
// Source/FunctionLayer/Scripting/NativeBridge/NativeRenderExports.inl
// 序列抓帧缺省 1/30 秒，即每个宿主输出帧推进 2 个 60 Hz 固定模拟步。
const double fixedStep = s->windowFixedStep > 0 ? s->windowFixedStep : (sequence ? 1.0 / 30.0 : 0);
if (fixedStep > 0) services.fixedDeltaSeconds = fixedStep;

// 第 0 个输出帧取初始化后的 tick 0：先渲染初态，不推进模拟时钟。
if (sequence && captureCursor == 0)
    return !s->stopWindow && flare_session_step(handle, 0.0) == FLARE_OK && !s->stopWindow;
```

- **命名与计数**：文件名从 `frame_0000.ppm` 起连续编号（`index` 与内部 60 Hz 模拟 `tick` 不是同一计数）。
- **完整度保护与原子清单**：默认在录满 `count` 帧后自动退出；若 `maximumFrames` 小于请求数或提前关闭窗口导致缺帧，直接返回 `Bridge.CaptureSequenceIncomplete` 而绝不写出不完整的清单。全部文件核验非空后，先写 `frames.json.tmp` 再重命名为 `frames.json`，顶层包含 `schema / count / width / height / fps / fixedStep / simulationHz`，每帧条目包含 `index / updateIndex / tick / file / bytes`。
- **源码位置**：[托管入口 `EngineSession.cs`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L172) · [默认步长与帧上限 `NativeRenderExports.inl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativeRenderExports.inl#L26) · [原子清单生成 `NativeRenderExports.inl`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativeRenderExports.inl#L93)

## VideoEncoder

`VideoEncoder.exe` 是独立 C++ 工具（不依赖引擎渲染层、D3D12 或 FFmpeg），在仓库根目录执行以下命令即可从已配置的 Release 预设单独编译：

```powershell
cmake --build --preset windows-x64-release --target VideoEncoder
```

产物位于 `out/build/windows-x64-release/VideoEncoder.exe`（便携编辑器包中位于 `Tools/VideoEncoder.exe`）。[命令行与配置源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Tools/VideoEncoder/VideoEncoder.cpp#L116)

| 参数 | 与抓帧的约定 |
| --- | --- |
| `--frames <dir>`、`--count <n>` | 必填。输入目录和帧数（`1..1,000,000`）。从 `--first` 起逐号核对连续性，缺号或重复编号立即失败；区间外的多余文件被忽略并计入 `ignoredFileCount`。 |
| `--pattern <fmt>`、`--first <i>` | 可选，默认 `frame_%04d.ppm` 与 `0`，与宿主一致。输入须为二进制 `P6`、每通道 8 位，全序列同尺寸且宽高均为偶数（NV12 4:2:0 要求）。 |
| `--fps <n>` | 可选，恒定播放帧率整数（`1..1000`，默认 `30`），应与抓帧的 `1 / fixedStep` 一致。 |
| `--output <file.mp4>` | 必填。目标 MP4 路径；编码前先试写探测文件确认可写，编码期间写入 `.partial-*.mp4`，成功后原子替换。 |
| `--manifest <file.json>` | 可选。将与 stdout 相同的 JSON 编码/核验报告原子落盘（与宿主生成的 `frames.json` 是两份独立清单）。 |
| `--selftest <tempdir>` | 独立环境自检模式：自动生成 8 帧 `64×64` 测试图（8 fps），走完整编码、时间戳归一与解码验证，退出码 `0` 或 `1`。 |

**固定编码规格与可复现性**（[`VideoEncoder.cpp#L567`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Tools/VideoEncoder/VideoEncoder.cpp#L567)）：直接通过 `ICodecAPI` 驱动 Microsoft H.264 MFT（High profile、`yuv420p` NV12 输入、单线程、GOP = 30、B 帧 = 0、CBR 码率以 1080p 12 Mbps 按像素数线性缩放并钳位在 `200 kbps..60 Mbps`），封装完成后调用 [`NormalizeMp4Timestamps()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Tools/VideoEncoder/VideoEncoder.cpp#L619) 把 `mvhd / tkhd / mdhd` 中的创建与修改时间戳归为固定值，使同一批输入帧在两次独立编码中得到逐字节一致的 SHA-256。

### VerifyMp4File

编码和原子替换完成后，`VideoEncoder` 不会直接宣告成功，而是立即调用 [`VerifyMp4File()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Tools/VideoEncoder/VideoEncoderMp4.cpp#L1713) 重新用 Media Foundation Source Reader 打开成片并解码全片，逐一核对样本数、时间戳间距、帧率与总时长：

- 自动化脚本应先判断进程退出码为 `0`（失败退出码 `2..16` 分别对应稳定诊断键，如 `3 = Encoder.FrameMissing`、`8 = Encoder.FrameDimensionMismatch`、`14 = Encoder.VerificationFailed`）。
- 再核对 JSON 报告中的 `status == "ok"`、`verification.decodedSamples`、`verification.constantFrameRate` 与 `verification.durationSeconds`（例如 60 帧以 30 fps 编码，应解出 `60` 个样本、`2.0` 秒时长）。
- **源码位置**：[输入序列扫描 `VideoEncoderPpm.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Tools/VideoEncoder/VideoEncoderPpm.cpp#L357) · [成片回读验证 `VideoEncoderMp4.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Tools/VideoEncoder/VideoEncoderMp4.cpp#L1713)

::: tip 单独排查编码器环境是否正常
在无画面输出或 CI 机器上怀疑缺少 Windows Media Foundation H.264 编码器时，先直接运行：

```powershell
out/build/windows-x64-release/VideoEncoder.exe --selftest out/tmp/encoder-selftest
```

返回码为 `0` 且输出 JSON 中 `verification.status == "ok"` 即证明本机编码与解码环境完好。
:::

::: warning 分辨率必须为偶数
H.264 `yuv420p` / NV12 要求宽高都是偶数。调用 `SetWindowResolution()` 或在 `Start-Game.ps1 -Resolution` 传参时请使用偶数分辨率（如 `640x360`、`1280x720`、`1920x1080`），奇数宽高会被 `VideoEncoder` 以退出码 `6`（`Encoder.DimensionsUnsupported`）拒绝。
:::
