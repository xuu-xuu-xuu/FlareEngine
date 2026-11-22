#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace flare
{
using AudioVoiceHandle = std::uint64_t;

struct AudioPcmFormat
{
    std::uint32_t sampleRate = 48000;
    std::uint16_t channels = 2;
};

struct AudioVoiceParameters
{
    float leftVolume = 1.0f;
    float rightVolume = 1.0f;
    float pitch = 1.0f;
};

/// 平台音频后端纯抽象接口：允许 EngineAudio 在零硬件/Win32 依赖下执行混音与单元测试。
class AudioBackend
{
public:
    virtual ~AudioBackend() = default;

    [[nodiscard]] virtual bool Initialize() = 0;
    virtual void Shutdown() noexcept = 0;
    [[nodiscard]] virtual bool IsInitialized() const noexcept = 0;

    [[nodiscard]] virtual AudioVoiceHandle CreateVoice(const AudioPcmFormat& format) = 0;
    virtual void DestroyVoice(AudioVoiceHandle voice) noexcept = 0;

    [[nodiscard]] virtual bool SubmitSamples(
        AudioVoiceHandle voice,
        std::span<const std::int16_t> pcmInterleaved,
        bool loop) = 0;

    [[nodiscard]] virtual bool SetVoiceParameters(
        AudioVoiceHandle voice,
        const AudioVoiceParameters& parameters) = 0;

    [[nodiscard]] virtual bool SetVoicePaused(AudioVoiceHandle voice, bool paused) = 0;
    [[nodiscard]] virtual bool IsVoicePlaying(AudioVoiceHandle voice) const noexcept = 0;
    virtual void StopAll() noexcept = 0;
};

/// 无声卡机器与确定性单元测试使用的纯内存音频后端。
class NullAudioBackend final : public AudioBackend
{
public:
    struct RecordedVoice
    {
        AudioPcmFormat format{};
        AudioVoiceParameters parameters{};
        std::size_t sampleCount = 0;
        bool loop = false;
        bool paused = false;
        bool playing = false;
    };

    bool Initialize() override
    {
        initialized_ = true;
        return true;
    }

    void Shutdown() noexcept override
    {
        voices_.clear();
        initialized_ = false;
    }

    [[nodiscard]] bool IsInitialized() const noexcept override { return initialized_; }

    AudioVoiceHandle CreateVoice(const AudioPcmFormat& format) override
    {
        if (!initialized_ || format.sampleRate == 0 || (format.channels != 1 && format.channels != 2))
        {
            return 0;
        }
        const AudioVoiceHandle handle = nextHandle_++;
        voices_.emplace(handle, RecordedVoice{format, {}, 0, false, false, false});
        return handle;
    }

    void DestroyVoice(AudioVoiceHandle voice) noexcept override
    {
        voices_.erase(voice);
    }

    bool SubmitSamples(
        AudioVoiceHandle voice,
        std::span<const std::int16_t> pcmInterleaved,
        bool loop) override
    {
        const auto it = voices_.find(voice);
        if (!initialized_ || it == voices_.end() || pcmInterleaved.empty() ||
            (pcmInterleaved.size() % it->second.format.channels) != 0)
        {
            return false;
        }
        it->second.sampleCount = pcmInterleaved.size();
        it->second.loop = loop;
        it->second.playing = true;
        ++submitCount_;
        return true;
    }

    bool SetVoiceParameters(AudioVoiceHandle voice, const AudioVoiceParameters& parameters) override
    {
        const auto it = voices_.find(voice);
        if (!initialized_ || it == voices_.end())
        {
            return false;
        }
        it->second.parameters = parameters;
        return true;
    }

    bool SetVoicePaused(AudioVoiceHandle voice, bool paused) override
    {
        const auto it = voices_.find(voice);
        if (!initialized_ || it == voices_.end())
        {
            return false;
        }
        it->second.paused = paused;
        return true;
    }

    [[nodiscard]] bool IsVoicePlaying(AudioVoiceHandle voice) const noexcept override
    {
        const auto it = voices_.find(voice);
        return it != voices_.end() && it->second.playing;
    }

    void StopAll() noexcept override
    {
        for (auto& [handle, entry] : voices_)
        {
            (void)handle;
            entry.playing = false;
            entry.sampleCount = 0;
        }
    }

    // 测试辅助查询方法：不污染 AudioBackend 接口本身。
    void SimulateVoiceFinished(AudioVoiceHandle voice) noexcept
    {
        const auto it = voices_.find(voice);
        if (it != voices_.end())
        {
            it->second.playing = false;
        }
    }

    [[nodiscard]] std::size_t ActiveVoiceCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& [handle, entry] : voices_)
        {
            (void)handle;
            if (entry.playing)
            {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] std::size_t AllocatedVoiceCount() const noexcept { return voices_.size(); }
    [[nodiscard]] std::size_t SubmitCount() const noexcept { return submitCount_; }

    [[nodiscard]] const RecordedVoice* InspectVoice(AudioVoiceHandle voice) const noexcept
    {
        const auto it = voices_.find(voice);
        return it != voices_.end() ? &it->second : nullptr;
    }

private:
    std::unordered_map<AudioVoiceHandle, RecordedVoice> voices_;
    AudioVoiceHandle nextHandle_ = 1;
    std::size_t submitCount_ = 0;
    bool initialized_ = false;
};
} // namespace flare
