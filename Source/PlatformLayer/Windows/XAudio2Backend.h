#pragma once

#include "PlatformLayer/Audio/AudioBackend.h"
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace flare
{
/// Windows XAudio2 硬件音频后端：仅依赖系统内置 xaudio2_9/8 DLL，零第三方分发 DLL。
class XAudio2Backend final : public AudioBackend
{
public:
    XAudio2Backend() = default;
    ~XAudio2Backend() override;

    XAudio2Backend(const XAudio2Backend&) = delete;
    XAudio2Backend& operator=(const XAudio2Backend&) = delete;

    [[nodiscard]] bool Initialize() override;
    void Shutdown() noexcept override;
    [[nodiscard]] bool IsInitialized() const noexcept override;

    [[nodiscard]] AudioVoiceHandle CreateVoice(const AudioPcmFormat& format) override;
    void DestroyVoice(AudioVoiceHandle voice) noexcept override;

    [[nodiscard]] bool SubmitSamples(
        AudioVoiceHandle voice,
        std::span<const std::int16_t> pcmInterleaved,
        bool loop) override;

    [[nodiscard]] bool SetVoiceParameters(
        AudioVoiceHandle voice,
        const AudioVoiceParameters& parameters) override;

    [[nodiscard]] bool SetVoicePaused(AudioVoiceHandle voice, bool paused) override;
    [[nodiscard]] bool IsVoicePlaying(AudioVoiceHandle voice) const noexcept override;
    void StopAll() noexcept override;

private:
    struct VoiceEntry
    {
        IXAudio2SourceVoice* sourceVoice = nullptr;
        AudioPcmFormat format{};
        std::vector<std::int16_t> samples;
        bool paused = false;
        bool submitted = false;
    };

    void DestroyVoiceEntry(VoiceEntry& entry) noexcept;

    mutable std::mutex mutex_;
    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* masteringVoice_ = nullptr;
    std::uint32_t masteringChannels_ = 2;
    bool initialized_ = false;
    bool comInitialized_ = false;
    AudioVoiceHandle nextHandle_ = 1;
    std::unordered_map<AudioVoiceHandle, VoiceEntry> voices_;
};
} // namespace flare
