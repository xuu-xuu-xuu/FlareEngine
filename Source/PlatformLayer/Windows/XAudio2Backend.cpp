#include "PlatformLayer/Windows/XAudio2Backend.h"

#include "CoreLayer/Log.h"
#include <Windows.h>
#include <xaudio2.h>
#include <algorithm>
#include <cmath>

namespace se
{
XAudio2Backend::~XAudio2Backend()
{
    Shutdown();
}

bool XAudio2Backend::Initialize()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_)
    {
        return true;
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comResult))
    {
        comInitialized_ = true;
    }
    else if (comResult != RPC_E_CHANGED_MODE)
    {
        Log(LogLevel::Warning, "Audio.BackendFailed: COM init failed");
        return false;
    }

    IXAudio2* engine = nullptr;
    if (FAILED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !engine)
    {
        Log(LogLevel::Warning, "Audio.BackendFailed: XAudio2Create failed");
        if (comInitialized_)
        {
            CoUninitialize();
            comInitialized_ = false;
        }
        return false;
    }

    IXAudio2MasteringVoice* mastering = nullptr;
    if (FAILED(engine->CreateMasteringVoice(&mastering)) || !mastering)
    {
        Log(LogLevel::Warning, "Audio.BackendFailed: no active mastering audio endpoint");
        engine->Release();
        if (comInitialized_)
        {
            CoUninitialize();
            comInitialized_ = false;
        }
        return false;
    }

    XAUDIO2_VOICE_DETAILS details{};
    mastering->GetVoiceDetails(&details);
    masteringChannels_ = std::max<std::uint32_t>(1u, details.InputChannels);
    engine_ = engine;
    masteringVoice_ = mastering;
    initialized_ = true;
    return true;
}

void XAudio2Backend::DestroyVoiceEntry(VoiceEntry& entry) noexcept
{
    if (entry.sourceVoice)
    {
        entry.sourceVoice->Stop(0);
        entry.sourceVoice->FlushSourceBuffers();
        entry.sourceVoice->DestroyVoice();
        entry.sourceVoice = nullptr;
    }
    entry.samples.clear();
    entry.submitted = false;
}

void XAudio2Backend::Shutdown() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [handle, entry] : voices_)
    {
        (void)handle;
        DestroyVoiceEntry(entry);
    }
    voices_.clear();

    if (masteringVoice_)
    {
        masteringVoice_->DestroyVoice();
        masteringVoice_ = nullptr;
    }
    if (engine_)
    {
        engine_->StopEngine();
        engine_->Release();
        engine_ = nullptr;
    }
    initialized_ = false;
    if (comInitialized_)
    {
        CoUninitialize();
        comInitialized_ = false;
    }
}

bool XAudio2Backend::IsInitialized() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

AudioVoiceHandle XAudio2Backend::CreateVoice(const AudioPcmFormat& format)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !engine_ || format.sampleRate == 0 || (format.channels != 1 && format.channels != 2))
    {
        return 0;
    }

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = format.channels;
    wfx.nSamplesPerSec = format.sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * (wfx.wBitsPerSample / 8));
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    IXAudio2SourceVoice* sourceVoice = nullptr;
    if (FAILED(engine_->CreateSourceVoice(&sourceVoice, &wfx, 0, 4.0f)) || !sourceVoice)
    {
        return 0;
    }

    const AudioVoiceHandle handle = nextHandle_++;
    VoiceEntry entry;
    entry.sourceVoice = sourceVoice;
    entry.format = format;
    voices_.emplace(handle, std::move(entry));
    return handle;
}

void XAudio2Backend::DestroyVoice(AudioVoiceHandle voice) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = voices_.find(voice);
    if (it == voices_.end())
    {
        return;
    }
    DestroyVoiceEntry(it->second);
    voices_.erase(it);
}

bool XAudio2Backend::SubmitSamples(
    AudioVoiceHandle voice,
    std::span<const std::int16_t> pcmInterleaved,
    bool loop)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = voices_.find(voice);
    if (!initialized_ || it == voices_.end() || !it->second.sourceVoice || pcmInterleaved.empty() ||
        (pcmInterleaved.size() % it->second.format.channels) != 0)
    {
        return false;
    }
    const std::size_t byteCount = pcmInterleaved.size() * sizeof(std::int16_t);
    if (byteCount > UINT32_MAX)
    {
        return false;
    }

    auto& entry = it->second;
    entry.sourceVoice->Stop(0);
    entry.sourceVoice->FlushSourceBuffers();
    entry.samples.assign(pcmInterleaved.begin(), pcmInterleaved.end());

    XAUDIO2_BUFFER buffer{};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = static_cast<UINT32>(byteCount);
    buffer.pAudioData = reinterpret_cast<const BYTE*>(entry.samples.data());
    buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;

    if (FAILED(entry.sourceVoice->SubmitSourceBuffer(&buffer)))
    {
        entry.samples.clear();
        entry.submitted = false;
        return false;
    }
    entry.submitted = true;
    if (!entry.paused && FAILED(entry.sourceVoice->Start(0)))
    {
        entry.sourceVoice->FlushSourceBuffers();
        entry.samples.clear();
        entry.submitted = false;
        return false;
    }
    return true;
}

bool XAudio2Backend::SetVoiceParameters(
    AudioVoiceHandle voice,
    const AudioVoiceParameters& parameters)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = voices_.find(voice);
    if (!initialized_ || !masteringVoice_ || it == voices_.end() || !it->second.sourceVoice)
    {
        return false;
    }

    auto& entry = it->second;
    const float left = std::isfinite(parameters.leftVolume) ? std::max(0.0f, parameters.leftVolume) : 0.0f;
    const float right = std::isfinite(parameters.rightVolume) ? std::max(0.0f, parameters.rightVolume) : 0.0f;
    const float pitch = std::isfinite(parameters.pitch)
        ? std::clamp(parameters.pitch, XAUDIO2_MIN_FREQ_RATIO, 4.0f)
        : 1.0f;

    const std::uint32_t srcCh = entry.format.channels;
    const std::uint32_t dstCh = masteringChannels_;
    // XAudio2 OutputMatrix row-major: matrix[dst * srcCh + src]
    std::vector<float> matrix(static_cast<std::size_t>(srcCh) * dstCh, 0.0f);
    if (srcCh == 1)
    {
        if (dstCh == 1)
        {
            matrix[0] = (left + right) * 0.5f;
        }
        else
        {
            matrix[0] = left;
            matrix[1] = right;
        }
    }
    else
    {
        if (dstCh == 1)
        {
            matrix[0] = left * 0.5f;
            matrix[1] = right * 0.5f;
        }
        else
        {
            matrix[0 * srcCh + 0] = left;
            matrix[1 * srcCh + 1] = right;
        }
    }

    if (FAILED(entry.sourceVoice->SetOutputMatrix(masteringVoice_, srcCh, dstCh, matrix.data())))
    {
        return false;
    }
    if (FAILED(entry.sourceVoice->SetFrequencyRatio(pitch)))
    {
        return false;
    }
    return true;
}

bool XAudio2Backend::SetVoicePaused(AudioVoiceHandle voice, bool paused)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = voices_.find(voice);
    if (!initialized_ || it == voices_.end() || !it->second.sourceVoice)
    {
        return false;
    }
    auto& entry = it->second;
    if (entry.paused == paused)
    {
        return true;
    }
    entry.paused = paused;
    const HRESULT hr = paused ? entry.sourceVoice->Stop(0) : entry.sourceVoice->Start(0);
    return SUCCEEDED(hr);
}

bool XAudio2Backend::IsVoicePlaying(AudioVoiceHandle voice) const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = voices_.find(voice);
    if (!initialized_ || it == voices_.end() || !it->second.sourceVoice || !it->second.submitted)
    {
        return false;
    }
    XAUDIO2_VOICE_STATE state{};
    it->second.sourceVoice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return state.BuffersQueued > 0;
}

void XAudio2Backend::StopAll() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [handle, entry] : voices_)
    {
        (void)handle;
        if (entry.sourceVoice)
        {
            entry.sourceVoice->Stop(0);
            entry.sourceVoice->FlushSourceBuffers();
        }
        entry.samples.clear();
        entry.submitted = false;
    }
}
} // namespace se
