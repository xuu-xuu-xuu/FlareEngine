# 3D 空间音频、资产解码与 XAudio2 后端

这篇文档说明 Flare 音频栈的三层设计：如何把 WAV/OGG 解码并缓存为 `.seaud` 二进制容器、如何在纯 CPU 层计算 3D 反距离衰减与等功率立体声声像、64 路并发声部超限时如何淘汰，以及 XAudio2 硬件后端与 C# 脚本控制接口。

## 一句话定位

Flare 音频系统坚持“资产解码、领域混音、硬件输出”彻底分层：`ResourceLayer` 处理文件编解码；`FunctionLayer/Audio` 纯 CPU 计算 3D 混音与声部调度；`PlatformLayer/Windows` 封装 Windows 内置 XAudio2，在无声卡环境自动降级为 `NullAudioBackend`。

## 三层架构与数据流

```text
  .wav / .ogg 源文件
        │  dr_wav / stb_vorbis (DecodeAudioSource)
        ▼
  AudioClip (int16 PCM, 8k..192kHz, 1/2ch) ◀──▶ .seaud 容器 (156B 头 + SHA-256 校验)
        │  AssetStore::LoadAudioClip()
        ▼
  SceneAudio::Synchronize(world, assets, audio, paused)
    ├─ 选听者: 最小 stableId 的 AudioListenerComponent (回退主 Camera)
    └─ 同步各 AudioSourceComponent (playOnStart / loop / volume * listenerVolume)
        │
        ▼
  AudioWorld (纯 CPU, 上限 64 声部, 超限按 priority → audibleGain → sequence 淘汰)
    ├─ ComputeSpatialGains() (反距离衰减 + 等功率 cos/sin 声像 + spatialBlend 插值)
    └─ AudioBackend 抽象接口
         ├─ XAudio2Backend (Windows xaudio2_9/8，零第三方 DLL)
         └─ NullAudioBackend (无音频设备 / 自动化测试自动回退)
```

## 核心源码拆解

### 1. 音频资产解码与 `.seaud` 容器（`AudioClip`）

[`AudioClip`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AudioClip.h#L13) 位于 `ResourceLayer/Assets`，将所有导入音频统一解码为 16 位有符号交错 PCM（`std::vector<std::int16_t>`，采样率限制 `8000..192000 Hz`，声道数 `1` 或 `2`，单剪辑解码上限 `64M` 样本 / `128 MiB`）。

- [`DecodeAudioSource()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AudioClip.h#L38)：分别使用 `dr_wav` 与 `stb_vorbis` 从文件路径或内存字节流解码 `.wav` / `.ogg`。
- [`EncodeAudioContainer()` / `DecodeAudioContainer()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AudioClip.h#L49)：读写 `.seaud` 二进制缓存容器（魔数 `SEAUD01\0`、156 字节定长头，含源文件 SHA-256 与载荷校验哈希，确保源音频修改后自动失效重编）。

### 2. 纯 CPU 3D 混音与 64 声部淘汰策略（`AudioWorld`）

[`AudioWorld`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Audio/AudioWorld.h#L76) 仅依赖 `EngineCore` 与 `AudioBackend`，持有代际校验句柄 `VoiceHandle{ index, generation }`、全局主音量 `masterVolume_` 和暂停状态。

#### 3D 距离衰减与等功率声像法则

[`ComputeSpatialGains()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Audio/AudioWorld.cpp#L36) 先用归一化的反距离滚降在 `[minD, maxD]` 间平滑衰减到 `0`，再把声源方向投影到听者右向量得到 `pan ∈ [-1, 1]`，按等功率法则分配左右声道，并由 `spatialBlend ∈ [0, 1]` 在纯 2D 与完全 3D 之间线性插值：

```cpp
// Source/FunctionLayer/Audio/AudioWorld.cpp
float attenuation = 1.0f;
if (distance >= maxD) attenuation = 0.0f;
else if (distance > minD)
{
    // 反距离滚降，并在 maxD 处平滑归一化到 0.0
    const float inv = minD / distance;
    const float endInv = minD / maxD;
    attenuation = std::clamp((inv - endInv) / (1.0f - endInv), 0.0f, 1.0f);
}

const float pan = distance > 1e-5f ? std::clamp(Dot(delta / distance, right), -1.0f, 1.0f) : 0.0f;
// 等功率立体声定律: theta = (pan + 1) * pi / 4
const float theta = (pan + 1.0f) * 0.25f * Pi;
const float gain3DLeft  = baseVolume * attenuation * std::cos(theta);
const float gain3DRight = baseVolume * attenuation * std::sin(theta);

const float left  = baseVolume + (gain3DLeft  - baseVolume) * blend;
const float right = baseVolume + (gain3DRight - baseVolume) * blend;
```

#### 并发上限与淘汰策略

同时发声上限为 `MaximumActiveVoices = 64`。当新调用 [`Play()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Audio/AudioWorld.cpp#L248) 遇到声部槽满时，`AcquireVoiceSlot()` 会先调用 `ReclaimFinished()` 回收已播完的非循环声部；若仍满，则挑选淘汰目标，优先级从高到低依次比较：
1. `description.priority` 最低的声部（若新请求优先级比所有现存声部都低，直接返回空句柄并记录 `"Audio.VoiceLimitReached"`）；
2. 同优先级中当前可听增益 `lastAudibleGain` 最弱的声部；
3. 同增益中最先启动（`sequence` 最小）的声部。

### 3. 硬件输出后端（`XAudio2Backend` & `NullAudioBackend`）

[`XAudio2Backend`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/XAudio2Backend.h#L16) 继承 `AudioBackend`，动态绑定 Windows 系统自带的 XAudio2 接口（零外部 DLL 分发）。`Initialize()` 按需初始化 COM 与 `IXAudio2` / Mastering Voice；析构或 `Shutdown()` 时先停止并销毁全部活跃 `IXAudio2SourceVoice` 再释放 COM 对象。在无声卡主机、远程 CI 或测试场景中，通过 `AudioWorld::SetBackend(nullptr)` 自动使用内置 `NullAudioBackend`，全部播放/暂停/查询接口保持一致语义而不报错。

### 4. 场景音频同步（`SceneAudio`）

[`SceneAudio`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Audio/SceneAudio.h#L14) 负责把场景实体状态同步到 `AudioWorld`：

- **听者解析**：[`ResolveSceneListener()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Audio/SceneAudio.cpp#L25) 优先选取稳定 ID 最小的启用 `AudioListenerComponent`（并将其 `listener.volume` 乘入音源增益）；若场景未挂听者组件，自动回退到主相机（`CameraComponent::primary == true`）。
- **音源生命周期**：[`Synchronize()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Audio/SceneAudio.cpp#L113) 逐帧更新所有活跃 `AudioSourceComponent` 的位置/音量/音高/衰减距离；首次启动时按 `playOnStart` 自动发声；当剪辑资产 ID 或 `loop` 标志变更时自动停止旧声部并重置状态；实体禁用或移除时立即停声回收；退出 Play 模式由 `Clear()` 清空缓存并调用 `StopAll()`。

### 5. C# 托管脚本接口（`F_Audio`）

C# 脚本在 `F_Behaviour` 中直接通过 `Audio` 属性（底层即 `Scripts.Session.Audio`，类型为 [`FlareEngine.F_Audio`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Audio.cs#L106)，兼容无前缀子类 `Audio`）触发一次性音效或操控实体上的音源组件：

```csharp
// 在指定 3D 世界坐标播放一次空间衰减音效
F_AudioVoiceHandle hit = Audio.PlayOneShotAt(
    clipAssetId, hitPoint, volume: 0.9f, minDistance: 1.5f, maxDistance: 25f);

// 控制实体上挂载的 AudioSource 组件
Audio.PlaySource(Entity);
if (Audio.IsSourcePlaying(Entity))
    Audio.StopSource(Entity);
```

## 常用接口速查

| 模块 | 接口 | 说明 | 源码位置 |
| --- | --- | --- | --- |
| `AudioClip` | `DecodeAudioSource` / `EncodeAudioContainer` / `DecodeAudioContainer` | WAV/OGG 解码与 `.seaud` 二进制缓存读写 | [AudioClip.h#L38](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ResourceLayer/Assets/AudioClip.h#L38) |
| `AudioWorld` | `Play` / `Stop` / `SetVoiceParameters` / `Update` / `SetMasterVolume` / `SetPaused` | 3D 混音、并发声部管理与参数动态更新 | [AudioWorld.h#L94](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Audio/AudioWorld.h#L94) |
| `SceneAudio` | `Synchronize` / `PlaySource` / `StopSource` / `IsSourcePlaying` / `Clear` | 同步 `AudioListener` / `AudioSource` 组件到 `AudioWorld` | [SceneAudio.h#L17](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Audio/SceneAudio.h#L17) |
| `XAudio2Backend` | `Initialize` / `Shutdown` / `CreateVoice` / `SubmitSamples` | Windows XAudio2 硬件后端生命周期与采样提交 | [XAudio2Backend.h#L25](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Windows/XAudio2Backend.h#L25) |
| `F_Audio` (C#) | `Play` / `PlayOneShot` / `PlayOneShotAt` / `PlaySource` / `StopSource` / `UseNullBackend` | C# 托管脚本音频控制服务 | [Audio.cs#L135](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Audio.cs#L135) |

::: tip 立体声音源与 3D 空间化
`AudioWorld` 允许单声道或双声道 `AudioClip`，音高合法范围 `0.1..4.0`，音量增益上限 `4.0`。想在编辑器里先验证纯 2D 背景音乐不受距离影响，只需把 `AudioSourceComponent` 的 `audio.spatialBlend` 设为 `0.0`；设为 `1.0` 则是完整 3D 空间衰减与左右声像定位。
:::

::: warning 为什么 `AudioSource` 在暂停状态不会触发 `playOnStart`？
查看 [`SceneAudio.cpp#L168`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Audio/SceneAudio.cpp#L168)：`if (!paused && !tracked.started && playOnStart && !clipId.empty())`。在编辑器编辑态或模拟暂停时，`Synchronize(..., paused = true)` 会挂起已有声部且推迟 `playOnStart`，避免刚进入暂停帧就漏出声音。
:::
