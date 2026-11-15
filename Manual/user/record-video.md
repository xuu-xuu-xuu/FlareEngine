# 连续抓帧并导出 MP4

下面从游戏的入口场景录制 **640 × 360、30 帧/秒、2 秒**的视频。游戏先把每一帧保存为图片，再由编码器生成无声的 H.264 MP4。想让画面里有动作，可以先[给模型设置骨骼动画](animate-model.md)。

## 导出游戏 {#export-game}

在 Windows 上完成[从源码构建 Release 版](build-from-source.md)，保存场景。在**引擎仓库根目录**打开 PowerShell，依次执行各段命令，保持同一个窗口。第一行提示输入时，粘贴项目的 `project.json` 完整路径（不加引号）；只想先试录，也可以输入仓库内的 `Resources\Templates\EmptyGame\project.json`。

```powershell
$project = (Resolve-Path -LiteralPath (Read-Host '项目的 project.json 路径')).Path
$take = 'Take-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$packages = Join-Path $PWD 'out\packages'
& .\Scripts\Export-Game.ps1 -Project $project -Name $take -OutputDirectory $packages
if ($LASTEXITCODE -ne 0) { throw '游戏导出失败，请查看导出日志' }
$launcher = (Resolve-Path "$packages\$take\Start-Game.ps1").Path
```

每次导出都会使用一个新名字。已有导出包时可以跳过导出：把 `$launcher` 设为包内 `Start-Game.ps1` 的完整路径，并为本次录制设置一个新的 `$take`。

## 连续抓取 60 帧 {#capture}

```powershell
$recording = Join-Path $PWD "out\captures\$take"
$frames = Join-Path $recording 'frames'
& $launcher -Hidden -Resolution 640x360 -CaptureSequence $frames -CaptureCount 60
if ($LASTEXITCODE -ne 0) { throw '抓帧失败，请检查玩家输出' }
$sequence = Get-Content -Raw -LiteralPath (Join-Path $frames 'frames.json') | ConvertFrom-Json
$sequence | Select-Object count, width, height, fps
```

成功时会看到 `60 / 640 / 360 / 30`，目录里有 `frame_0000.ppm` 到 `frame_0059.ppm` 和清单 `frames.json`。**每次录制用空目录**，以免上一次留下的图片混进来。`-Hidden` 只隐藏窗口，仍需要能运行 Flare 的显卡和驱动；不要同时使用 `-Headless`、`-Editor` 或单张截图的 `-Capture`。

## 编码并检查成片 {#encode}

```powershell
$mp4 = Join-Path $recording 'clip.mp4'
$report = Join-Path $recording 'encode.json'
& .\out\build\windows-x64-release\VideoEncoder.exe --frames $frames --count 60 --fps 30 --output $mp4 --manifest $report
if ($LASTEXITCODE -ne 0) { throw '编码失败，请查看编码器诊断' }
$result = Get-Content -Raw -LiteralPath $report | ConvertFrom-Json
$result.verification | Select-Object status, decodedSamples, durationSeconds, constantFrameRate
```

预期结果为 `ok / 60 / 2 / True`：编码器重新打开成片，解出了全部 60 帧。用系统播放器打开 `$mp4` 指向的文件即可观看。确认成片后，如果不需要重新编码，只保留 MP4 和报告即可。

**改变时长或帧率：** 总帧数 = 时长（秒）× 帧率。抓帧默认每帧推进 1/30 秒；要录 2 秒、60 帧/秒，在抓帧命令中加 `-FixedStep (1.0 / 60.0)` 并改用 `-CaptureCount 120`，编码时也要改成 `--count 120 --fps 60`。只改编码帧率会改变播放速度。要提高分辨率，可将 `-Resolution` 改成 `1920x1080`；宽高都用偶数，原始帧大约占「宽 × 高 × 3 × 帧数」字节。

**没有得到视频：** 先看抓帧清单的数量是否够，再检查所选范围内有无缺号或重复编号、所有图片尺寸是否一致。如果找不到 `VideoEncoder.exe`，在已配置好的开发者 PowerShell 中从仓库根目录运行 `cmake --build --preset windows-x64-release --target VideoEncoder`。
