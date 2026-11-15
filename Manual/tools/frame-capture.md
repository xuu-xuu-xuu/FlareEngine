# 固定步连续抓帧与 MP4 编码

玩家宿主把「一次输出更新」对应的画面写成 PPM 序列，独立工具 `VideoEncoder.exe` 将序列合成为恒定帧率的 H.264 MP4。两阶段共享帧数、尺寸和帧率；磁盘写入快慢不决定视频时长。可直接执行的 PowerShell 命令见[抓帧与编码操作](../user/record-video.md#capture)。

## EngineSession

在已经加载场景和资产的托管会话上按下列顺序调用。示例中 `directory` 是本次录制专用的空目录，单位为秒；窗口隐藏时仍会运行 D3D12 渲染。

```csharp
session.SetWindowResolution(640, 360);
session.SetWindowFixedStep(1.0 / 30.0);
session.SetCaptureSequence(directory, 60);
session.RunWindow(visible: false);
```

### SetCaptureSequence

`SetCaptureSequence(string? directory, ulong count)` 创建输出目录并配置序列；最多 100,000 帧，传空目录参数（`null` 或空字符串）或 0 帧可清除配置。先设置再调用 `RunWindow`；运行中的窗口不能修改抓帧设置。不能与单帧 `SetWindowCapture` 同时启用。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L102) · [原生参数检查](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L765)

### RunWindow

序列模式下，未设置固定步长时每个输出更新推进 1/30 秒；世界模拟仍以 60 Hz 运行。文件名从 `frame_0000.ppm` 开始，编号与模拟 tick 不是同一计数。默认达到请求帧数后结束；若 `maximumFrames` 小于请求数，或提前关闭窗口导致缺帧，则不会报告一次完整录制。写完并检查各帧后才发布 `frames.json`：总体包含 `count / width / height / fps / fixedStep / simulationHz`，每帧包含 `index / updateIndex / tick / file / bytes`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Scripting/Managed/EngineSession.cs#L123) · [默认步长及帧上限](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L837) · [清单生成](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Source/FunctionLayer/Scripting/NativeBridge/EngineNative.cpp#L894)

## VideoEncoder

从已配置的 Release 构建目录生成目标：在仓库根目录运行 `cmake --build --preset windows-x64-release --target VideoEncoder`。工具位于 `out/build/windows-x64-release/VideoEncoder.exe`，使用 Windows Media Foundation 编码，无需 FFmpeg。[命令行定义](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Tools/VideoEncoder/VideoEncoder.cpp#L119)

| 参数 | 与抓帧的约定 |
| --- | --- |
| `--frames`、`--count` | 输入目录和要编码的帧数。编码器从 `--first` 起核对范围内的每一帧，遇到缺号或重复编号会失败；范围外的文件忽略。例如 `--first 10 --count 3` 取第 10、11、12 号帧。 |
| `--pattern`、`--first` | 默认 `frame_%04d.ppm`、`0`，与宿主一致。输入是二进制 P6、每色通道 8 位，所有帧同尺寸且宽高均为偶数。 |
| `--fps` | 恒定播放帧率，整数，默认 30。应与抓帧的 `1 / fixedStep` 对应。 |
| `--output`、`--manifest` | MP4 路径和可选的编码报告路径。后者与宿主生成的 `frames.json` 是两份不同的清单。 |

### VerifyMp4File

编码完成后，工具重新打开 MP4，解码并核对样本数、时间戳、帧率与时长。自动化应先检查进程退出码为 0，再核对报告的 `status`、`verification.decodedSamples`、`verification.constantFrameRate` 和 `verification.durationSeconds`。例如 60 帧以 30 fps 编码，应解出 60 个样本、时长 2 秒。要单独排查编码环境，可运行 `VideoEncoder.exe --selftest <临时目录>`，工具会生成 8 帧并走完整的编码和解码流程。[输入序列检查](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Tools/VideoEncoder/VideoEncoderPpm.cpp#L357) · [查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/542afe82dcdf65eb987477b3862dd3b5d82b43e5/Tools/VideoEncoder/VideoEncoderMp4.cpp#L1713)
