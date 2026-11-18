#pragma once

#include "CoreLayer/Math/Math.h"
#include "PlatformLayer/Audio/AudioBackend.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace se
{
/// 纯领域 16-bit PCM 音频缓冲描述（与 AssetStore 资源寿命解耦）。
struct AudioClipData
{
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::vector<std::int16_t> samples;

    [[nodiscard]] bool Valid() const noexcept
    {
        return sampleRate >= 8000 && sampleRate <= 192000 && (channels == 1 || channels == 2) &&
               !samples.empty() && (samples.size() % channels) == 0;
    }
};

struct ListenerPose
{
    Vec3 position{};
    Vec3 forward{0.0f, 0.0f, -1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};
};

[[nodiscard]] ListenerPose ListenerPoseFromRotation(Vec3 position, Quat rotation) noexcept;

struct VoiceHandle
{
    std::uint32_t index = UINT32_MAX;
    std::uint32_t generation = 0;

    explicit operator bool() const noexcept { return index != UINT32_MAX && generation != 0; }
    [[nodiscard]] std::uint64_t Pack() const noexcept { return (std::uint64_t{generation} << 32) | index; }
    bool operator==(const VoiceHandle&) const = default;
};

struct AudioPlayDesc
{
    std::shared_ptr<const AudioClipData> clip;
    float volume = 1.0f;
    float pitch = 1.0f;
    float minDistanceMeters = 1.0f;
    float maxDistanceMeters = 30.0f;
    Vec3 position{};
    float spatialBlend = 0.0f; // 0 = 纯 2D，1 = 完全 3D 距离衰减与声像定位
    bool loop = false;
    std::int32_t priority = 0;
};

struct SpatialGainPair
{
    float leftVolume = 1.0f;
    float rightVolume = 1.0f;
    float attenuation = 1.0f;
};

[[nodiscard]] SpatialGainPair ComputeSpatialGains(
    const ListenerPose& listener,
    Vec3 sourcePosition,
    float volume,
    float minDistanceMeters,
    float maxDistanceMeters,
    float spatialBlend) noexcept;

/// 纯领域 3D 混音与并发管理（仅依赖 EngineCore 与 AudioBackend 抽象接口）。
class AudioWorld
{
public:
    static constexpr std::size_t MaximumActiveVoices = 64;

    AudioWorld() = default;
    ~AudioWorld();

    AudioWorld(const AudioWorld&) = delete;
    AudioWorld& operator=(const AudioWorld&) = delete;

    /// 绑定平台音频后端（非拥有指针；传 nullptr 回退至内置 NullAudioBackend）。
    void SetBackend(AudioBackend* backend);
    [[nodiscard]] AudioBackend& Backend() noexcept { return *backend_; }
    [[nodiscard]] const AudioBackend& Backend() const noexcept { return *backend_; }

    void Shutdown() noexcept;

    [[nodiscard]] VoiceHandle Play(const AudioPlayDesc& description);
    bool Stop(VoiceHandle voice);
    [[nodiscard]] bool IsPlaying(VoiceHandle voice) const;

    bool SetVoiceParameters(
        VoiceHandle voice,
        float volume,
        float pitch,
        Vec3 position,
        float spatialBlend,
        float minDistanceMeters,
        float maxDistanceMeters);

    void SetPaused(bool paused);
    [[nodiscard]] bool Paused() const noexcept { return paused_; }

    void SetMasterVolume(float volume);
    [[nodiscard]] float MasterVolume() const noexcept { return masterVolume_; }

    void Update(const ListenerPose& listener);
    void StopAll();

    [[nodiscard]] std::size_t ActiveVoiceCount() const;
    [[nodiscard]] std::vector<VoiceHandle> ActiveVoices() const;
    [[nodiscard]] const std::string& LastError() const noexcept { return error_; }

private:
    struct VoiceSlot
    {
        AudioVoiceHandle backendHandle = 0;
        std::shared_ptr<const AudioClipData> clip;
        AudioPlayDesc description;
        float lastAudibleGain = 0.0f;
        std::uint64_t sequence = 0;
        std::uint32_t generation = 0;
        bool alive = false;
    };

    bool Fail(const char* key) const;
    bool EnsureBackendInitialized();
    void ReleaseSlot(std::uint32_t index) noexcept;
    void ReclaimFinished() noexcept;
    [[nodiscard]] bool ApplyVoiceMix(VoiceSlot& slot, const ListenerPose& listener);
    [[nodiscard]] std::uint32_t AcquireVoiceSlot(std::int32_t priority);

    NullAudioBackend fallbackBackend_;
    AudioBackend* backend_ = &fallbackBackend_;
    ListenerPose lastListener_{};
    std::vector<VoiceSlot> slots_;
    std::vector<std::uint32_t> free_;
    std::uint64_t nextSequence_ = 1;
    std::uint32_t nextGeneration_ = 1;
    float masterVolume_ = 1.0f;
    bool paused_ = false;
    mutable std::string error_;
};
} // namespace se
