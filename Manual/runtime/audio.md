# 3D 空间音频、资产解码与 XAudio2 后端

音频栈严格分层：`ResourceLayer` 解码 WAV/OGG 到统一的 `.seaud` 音频容器；`FunctionLayer/Audio` 负责纯 CPU 3D 距离衰减、立体声声像与声道并发上限；`PlatformLayer/Windows` 封装 XAudio2 硬件输出，无音频设备或离线测试时自动回退至 `NullAudioBackend`。

## AudioClip

`AudioClip` 位于 `ResourceLayer/Assets`，以统一 16 位有符号整型（`int16_t`）交错 PCM 帧保存解码后的采样率、声道数与样本数组，支持经 `dr_wav` 与 `stb_vorbis` 导入 `.wav`/`.ogg` 并持久化为 `.seaud` 容器。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ResourceLayer/Assets/AudioClip.h#L13)

### DecodeAudioSource

`bool DecodeAudioSource(...)` 从磁盘文件或内存字节流解码 WAV/OGG 源码到 `AudioClip`；`EncodeAudioContainer()` 与 `DecodeAudioContainer()` 负责 `.seaud` 二进制容器读写与校验。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ResourceLayer/Assets/AudioClip.h#L38)

## XAudio2Backend

`XAudio2Backend` 继承平台抽象 `AudioBackend`，管理 XAudio2 Mastering Voice 与源声道池生命周期，析构时先停止全部活跃声道再释放 COM 接口。无扬声器或 CI 环境下可注入内置 `NullAudioBackend` 保持接口行为一致。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/Windows/XAudio2Backend.h#L16)

### Initialize

`bool Initialize()` 按需初始化 COM 与 `IXAudio2` 实例；失败时保持安全未就绪状态而不崩溃。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/Windows/XAudio2Backend.h#L25)

## AudioWorld

`AudioWorld` 位于独立库 `EngineAudio`（仅依赖 `EngineCore` 与 `AudioBackend` 抽象），管理多路 `VoiceHandle`、主音量、暂停状态、并发上限（超限时按优先级与可听增益淘汰最弱声道）以及 3D 距离衰减与左右声像混音。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Audio/AudioWorld.h#L76)

### Play

`VoiceHandle Play(const AudioPlayDesc& description)` 启动一路 2D 或 3D 音频（`spatialBlend` 在 `0..1` 间插值），返回代际句柄；`Stop()`、`SetVoiceParameters()` 与 `Update(const ListenerPose&)` 分别负责停止、实时更新声源位置/音高与逐帧重算空间声像。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Audio/AudioWorld.h#L94)

## SceneAudio

`SceneAudio` 位于 `EngineSceneAudio`，将世界中的 `AudioListenerComponent` 与 `AudioSourceComponent` 同步到 `AudioWorld`：根据活动听者（或主相机回退）位置更新 `ListenerPose`，自动管理 `playOnStart` 与组件启用/禁用切换。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Audio/SceneAudio.h#L14)

### Synchronize

`bool Synchronize(const World&, AssetStore*, AudioWorld&, bool paused = false)` 在固定步后将场景声源状态与资产解码结果同步给 `AudioWorld`，暂停或退出 Play 模式时安全静音/回收声道。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Audio/SceneAudio.h#L17)

## Audio

C# 脚本通过 `Session.Audio`（`FlareEngine.Audio`）触发一次性音效、3D 定位声源或控制实体上的 `AudioSource` 组件。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Scripting/Managed/Audio.cs#L60)

### PlayOneShotAt

`AudioVoiceHandle PlayOneShotAt(string assetId, Vector3 position, float volume = 1f, float minDistance = 1f, float maxDistance = 30f)` 在指定世界坐标播放一次 3D 音效；播放实体挂载的音源组件使用 `PlaySource(Entity)` 与 `StopSource(Entity)`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Scripting/Managed/Audio.cs#L108)
