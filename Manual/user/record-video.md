# 连续抓帧并导出 MP4

本页带你**一键导出可独立双击运行的 Windows 游戏包 (`Play.exe`)**，并学会如何以固定帧率录制无卡顿的恒定帧率 H.264 视频 `clip.mp4`（支持编辑器纯图形界面一键完成，也提供 PowerShell 命令行批量录制方案）。

## 导出游戏 {#export-game}

### 方式 A：编辑器图形界面一键导出（推荐，零命令行）

1. 先按 **`Ctrl + Alt + S`** 确保场景、材质和脚本修改都已保存（未保存的改动在导出前也会自动触发保存）。
2. 点击顶部菜单 **项目 → 构建与导出**（或工具栏右上角 **构建导出** / 快捷键 `Ctrl + Shift + B`）。
3. 填写基础配置：
   - **导出名称**：输入本次打包的文件夹名称（仅限字母、数字、下划线和连字符，例如 `DemoBuild_01`）。
   - 如果需要**同步录制入口场景视频**，勾选 **同时导出 MP4**，并选择：
     - **视频分辨率**：`640x360` / `1280x720` / `1920x1080`
     - **帧率（帧/秒）**：`24` / `30` / `60`
     - **录制时长（秒）**：例如 `2`（选择 `1280x720`、30 帧/秒、2 秒会精确录制 60 帧）
4. 点击面板底部的 **导出** 按钮，等待状态条依次走完校验、脚本构建、验证、打包（以及视频录制编码）。
5. 完成后点击 **打开输出目录**，里面包含：
   - **`Play.exe`**：双击即可在任意 Windows x64 电脑上独立运行的游戏入口；
   - **`clip.mp4`**：（勾选录制时生成）恒定帧率无声 H.264 视频，直接双击就能用系统播放器观看。

### 方式 B：使用 PowerShell 脚本导出（适合自动化与保留原始帧）

如果你需要保留每一张原始 `.ppm` 帧图，或需要批量跑多个镜头录制，请先完成 [从源码构建 Release 版](build-from-source.md)，然后在**引擎仓库根目录**打开 PowerShell，在同一个窗口依次执行下面命令：

```powershell
# 提示输入时粘贴你的项目 project.json 完整路径（不加引号）
# 想直接试录内置模板，可输入: Resources\Templates\Basic3D\project.json
$project = (Resolve-Path -LiteralPath (Read-Host '项目的 project.json 路径')).Path
$take = 'Take-' + [guid]::NewGuid().ToString('N').Substring(0,8)
$packages = Join-Path $PWD 'out\packages'

& .\Scripts\Export-Game.ps1 -Project $project -Name $take -OutputDirectory $packages
if ($LASTEXITCODE -ne 0) { throw '游戏导出失败，请查看导出日志' }
$launcher = (Resolve-Path "$packages\$take\Start-Game.ps1").Path
```

::: tip 已有导出包时可跳过这一步
如果你已经导出过游戏包，直接把 `$launcher` 设为包内 `Start-Game.ps1` 的完整路径，并为本次录制起一个新的 `$take` 名称即可。
:::

## 连续抓取 60 帧 {#capture}

下面示例从游戏入口场景抓取 **640 × 360、30 帧/秒、2 秒（共 60 帧）** 的无损帧序列：

```powershell
$recording = Join-Path $PWD "out\captures\$take"
$frames = Join-Path $recording 'frames'

& $launcher -Hidden -Resolution 640x360 -CaptureSequence $frames -CaptureCount 60
if ($LASTEXITCODE -ne 0) { throw '抓帧失败，请检查玩家输出' }

$sequence = Get-Content -Raw -LiteralPath (Join-Path $frames 'frames.json') | ConvertFrom-Json
$sequence | Select-Object count, width, height, fps
```

看到输出 `60 / 640 / 360 / 30` 即表示抓取成功，目录下会生成 `frame_0000.ppm` 到 `frame_0059.ppm` 以及清单 `frames.json`。

::: warning 抓帧注意事项
- **每次录制必须用空目录**，避免混入上一次遗留的旧帧图片。
- `-Hidden` 只隐藏播放器窗口，依然需要正常显卡驱动渲染画面；**不要**同时传入 `-Headless`、`-Editor` 或单帧截图参数 `-Capture`。
:::

## 编码并检查成片 {#encode}

用引擎自带的 `VideoEncoder.exe` 把帧序列合成为恒定帧率 H.264 MP4，并自动解码校验每一帧：

```powershell
$mp4 = Join-Path $recording 'clip.mp4'
$report = Join-Path $recording 'encode.json'

& .\out\build\windows-x64-release\VideoEncoder.exe --frames $frames --count 60 --fps 30 --output $mp4 --manifest $report
if ($LASTEXITCODE -ne 0) { throw '编码失败，请查看编码器诊断' }

$result = Get-Content -Raw -LiteralPath $report | ConvertFrom-Json
$result.verification | Select-Object status, decodedSamples, durationSeconds, constantFrameRate
```

看到 `ok / 60 / 2 / True` 表示校验通过，全部 60 帧完好无损。直接打开 `$mp4` 即可观看；确认成片后如果不需要保留体积较大的原始 `.ppm` 图片，删除 `frames` 目录只保留 `clip.mp4` 和 `encode.json` 即可。

## 自定义分辨率、帧率与常见排查

| 想怎么改 | 怎么设参数 |
| :--- | :--- |
| **录 2 秒、60 FPS 丝滑成片** | 总帧数 = 时长 × 帧率 = `120`。抓帧时加上 `-FixedStep (1.0 / 60.0) -CaptureCount 120`，编码时同步改成 `--count 120 --fps 60`（**抓帧步长和编码帧率必须一致**，只改编码帧率会导致快放或慢放）。 |
| **提高成片清晰度** | 把 `-Resolution` 改为 `1280x720` 或 `1920x1080`（宽和高都必须是偶数；原始 `.ppm` 临时占用空间约为 `宽 × 高 × 3 × 帧数` 字节）。 |
| **提示找不到 `VideoEncoder.exe`** | 在配置好 MSVC 环境的仓库根目录 PowerShell 执行：`cmake --build --preset windows-x64-release --target VideoEncoder`。 |
| **导出时提示「请先停止运行预览」** | 先按 `F5` / `F8` 停止编辑器里的游戏预览，再点击 **导出**。 |
