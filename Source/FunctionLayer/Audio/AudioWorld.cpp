#include "FunctionLayer/Audio/AudioWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace se
{
namespace
{
constexpr float kMaxGain = 4.0f;
constexpr float kMinPitch = 0.1f;
constexpr float kMaxPitch = 4.0f;
constexpr float kMinDistanceFloor = 0.01f;

float ClampFinite(float value, float low, float high, float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
} // namespace

ListenerPose ListenerPoseFromRotation(Vec3 position, Quat rotation) noexcept
{
    const Vec3 safePos = IsFinite(position) ? position : Vec3{};
    const float lenSq = LengthSquared(rotation);
    const Quat safeRot = (IsFinite(rotation) && IsFinite(lenSq) && lenSq > 1e-10f)
        ? Normalize(rotation)
        : Quat{};
    return {
        safePos,
        Normalize(Rotate(safeRot, {0.0f, 0.0f, -1.0f})),
        Normalize(Rotate(safeRot, {0.0f, 1.0f, 0.0f})),
        Normalize(Rotate(safeRot, {1.0f, 0.0f, 0.0f}))};
}

SpatialGainPair ComputeSpatialGains(
    const ListenerPose& listener,
    Vec3 sourcePosition,
    float volume,
    float minDistanceMeters,
    float maxDistanceMeters,
    float spatialBlend) noexcept
{
    const float baseVolume = ClampFinite(volume, 0.0f, kMaxGain, 1.0f);
    const float blend = ClampFinite(spatialBlend, 0.0f, 1.0f, 0.0f);
    const float minD = ClampFinite(minDistanceMeters, kMinDistanceFloor, 10000.0f, 1.0f);
    const float maxD = ClampFinite(maxDistanceMeters, minD + kMinDistanceFloor, 100000.0f, std::max(minD + 0.01f, 30.0f));

    if (!IsFinite(sourcePosition) || !IsFinite(listener.position))
    {
        return {baseVolume * (1.0f - blend), baseVolume * (1.0f - blend), 1.0f - blend};
    }

    const Vec3 delta = sourcePosition - listener.position;
    const float distance = Length(delta);
    float attenuation = 1.0f;
    if (distance >= maxD)
    {
        attenuation = 0.0f;
    }
    else if (distance > minD)
    {
        // Inverse-distance rolloff normalized smoothly to 0.0 at maxD.
        const float inv = minD / distance;
        const float endInv = minD / maxD;
        attenuation = std::clamp((inv - endInv) / (1.0f - endInv), 0.0f, 1.0f);
    }

    float pan = 0.0f;
    if (distance > 1e-5f)
    {
        const Vec3 dir = delta / distance;
        const float rightLenSq = LengthSquared(listener.right);
        const Vec3 right = (IsFinite(listener.right) && rightLenSq > 1e-8f)
            ? (listener.right / std::sqrt(rightLenSq))
            : Vec3{1.0f, 0.0f, 0.0f};
        pan = std::clamp(Dot(dir, right), -1.0f, 1.0f);
    }

    // Constant-power stereo law: theta = (pan + 1) * pi / 4.
    const float theta = (pan + 1.0f) * 0.25f * Pi;
    const float gain2D = baseVolume;
    const float gain3DLeft = baseVolume * attenuation * std::cos(theta);
    const float gain3DRight = baseVolume * attenuation * std::sin(theta);

    const float left = gain2D + (gain3DLeft - gain2D) * blend;
    const float right = gain2D + (gain3DRight - gain2D) * blend;
    const float blendedAttenuation = 1.0f + (attenuation - 1.0f) * blend;
    return {std::max(0.0f, left), std::max(0.0f, right), blendedAttenuation};
}

AudioWorld::~AudioWorld()
{
    Shutdown();
}

bool AudioWorld::Fail(const char* key) const
{
    error_ = key;
    return false;
}

bool AudioWorld::EnsureBackendInitialized()
{
    if (backend_->IsInitialized())
    {
        return true;
    }
    if (!backend_->Initialize())
    {
        return Fail("Audio.BackendFailed");
    }
    return true;
}

void AudioWorld::SetBackend(AudioBackend* backend)
{
    StopAll();
    if (backend_ && backend_->IsInitialized())
    {
        backend_->Shutdown();
    }
    backend_ = backend ? backend : &fallbackBackend_;
}

void AudioWorld::Shutdown() noexcept
{
    StopAll();
    if (backend_ && backend_->IsInitialized())
    {
        backend_->Shutdown();
    }
}

void AudioWorld::ReleaseSlot(std::uint32_t index) noexcept
{
    auto& slot = slots_[index];
    if (!slot.alive)
    {
        return;
    }
    if (slot.backendHandle != 0 && backend_->IsInitialized())
    {
        backend_->DestroyVoice(slot.backendHandle);
    }
    slot = {};
    free_.push_back(index);
}

void AudioWorld::ReclaimFinished() noexcept
{
    if (paused_ || !backend_->IsInitialized())
    {
        return;
    }
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(slots_.size()); ++i)
    {
        auto& slot = slots_[i];
        if (!slot.alive || slot.description.loop)
        {
            continue;
        }
        if (!backend_->IsVoicePlaying(slot.backendHandle))
        {
            ReleaseSlot(i);
        }
    }
}

bool AudioWorld::ApplyVoiceMix(VoiceSlot& slot, const ListenerPose& listener)
{
    const float effectiveVolume =
        ClampFinite(slot.description.volume, 0.0f, kMaxGain, 1.0f) * masterVolume_;
    const auto gains = ComputeSpatialGains(
        listener,
        slot.description.position,
        effectiveVolume,
        slot.description.minDistanceMeters,
        slot.description.maxDistanceMeters,
        slot.description.spatialBlend);
    slot.lastAudibleGain = std::max(gains.leftVolume, gains.rightVolume);

    const float pitch = ClampFinite(slot.description.pitch, kMinPitch, kMaxPitch, 1.0f);
    if (!backend_->SetVoiceParameters(slot.backendHandle, {gains.leftVolume, gains.rightVolume, pitch}))
    {
        return Fail("Audio.BackendFailed");
    }
    return true;
}

std::uint32_t AudioWorld::AcquireVoiceSlot(std::int32_t priority)
{
    ReclaimFinished();

    std::size_t liveCount = 0;
    for (const auto& slot : slots_)
    {
        if (slot.alive)
        {
            ++liveCount;
        }
    }

    if (liveCount >= MaximumActiveVoices)
    {
        std::uint32_t victimIndex = UINT32_MAX;
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(slots_.size()); ++i)
        {
            const auto& slot = slots_[i];
            if (!slot.alive)
            {
                continue;
            }
            if (victimIndex == UINT32_MAX)
            {
                victimIndex = i;
                continue;
            }
            const auto& best = slots_[victimIndex];
            if (slot.description.priority < best.description.priority ||
                (slot.description.priority == best.description.priority &&
                 (slot.lastAudibleGain < best.lastAudibleGain ||
                  (slot.lastAudibleGain == best.lastAudibleGain && slot.sequence < best.sequence))))
            {
                victimIndex = i;
            }
        }

        if (victimIndex == UINT32_MAX || priority < slots_[victimIndex].description.priority)
        {
            Fail("Audio.VoiceLimitReached");
            return UINT32_MAX;
        }
        ReleaseSlot(victimIndex);
    }

    if (!free_.empty())
    {
        const std::uint32_t index = free_.back();
        free_.pop_back();
        return index;
    }
    const std::uint32_t index = static_cast<std::uint32_t>(slots_.size());
    slots_.emplace_back();
    return index;
}

VoiceHandle AudioWorld::Play(const AudioPlayDesc& description)
{
    if (!description.clip || !description.clip->Valid() ||
        !IsFinite(description.position) || !IsFinite(description.volume) ||
        !IsFinite(description.pitch) || !IsFinite(description.minDistanceMeters) ||
        !IsFinite(description.maxDistanceMeters) || !IsFinite(description.spatialBlend))
    {
        Fail("Audio.InvalidClip");
        return {};
    }
    if (!EnsureBackendInitialized())
    {
        return {};
    }

    const std::uint32_t slotIndex = AcquireVoiceSlot(description.priority);
    if (slotIndex == UINT32_MAX)
    {
        return {};
    }

    const AudioPcmFormat format{description.clip->sampleRate, description.clip->channels};
    const AudioVoiceHandle backendHandle = backend_->CreateVoice(format);
    if (backendHandle == 0)
    {
        Fail("Audio.BackendFailed");
        free_.push_back(slotIndex);
        return {};
    }

    auto& slot = slots_[slotIndex];
    slot = {};
    slot.alive = true;
    slot.generation = nextGeneration_++;
    if (slot.generation == 0)
    {
        slot.generation = nextGeneration_++;
    }
    slot.backendHandle = backendHandle;
    slot.clip = description.clip;
    slot.description = description;
    slot.description.volume = ClampFinite(description.volume, 0.0f, kMaxGain, 1.0f);
    slot.description.pitch = ClampFinite(description.pitch, kMinPitch, kMaxPitch, 1.0f);
    slot.description.spatialBlend = ClampFinite(description.spatialBlend, 0.0f, 1.0f, 0.0f);
    slot.sequence = nextSequence_++;

    if (!ApplyVoiceMix(slot, lastListener_) ||
        !backend_->SetVoicePaused(backendHandle, paused_) ||
        !backend_->SubmitSamples(backendHandle, description.clip->samples, description.loop))
    {
        ReleaseSlot(slotIndex);
        Fail("Audio.BackendFailed");
        return {};
    }

    error_.clear();
    return {slotIndex, slot.generation};
}

bool AudioWorld::Stop(VoiceHandle voice)
{
    if (!voice || voice.index >= slots_.size() || !slots_[voice.index].alive ||
        slots_[voice.index].generation != voice.generation)
    {
        return Fail("Audio.InvalidVoice");
    }
    ReleaseSlot(voice.index);
    error_.clear();
    return true;
}

bool AudioWorld::IsPlaying(VoiceHandle voice) const
{
    if (!voice || voice.index >= slots_.size() || !slots_[voice.index].alive ||
        slots_[voice.index].generation != voice.generation)
    {
        return false;
    }
    return backend_->IsInitialized() && backend_->IsVoicePlaying(slots_[voice.index].backendHandle);
}

bool AudioWorld::SetVoiceParameters(
    VoiceHandle voice,
    float volume,
    float pitch,
    Vec3 position,
    float spatialBlend,
    float minDistanceMeters,
    float maxDistanceMeters)
{
    if (!voice || voice.index >= slots_.size() || !slots_[voice.index].alive ||
        slots_[voice.index].generation != voice.generation)
    {
        return Fail("Audio.InvalidVoice");
    }
    if (!IsFinite(position) || !IsFinite(volume) || !IsFinite(pitch) ||
        !IsFinite(minDistanceMeters) || !IsFinite(maxDistanceMeters) || !IsFinite(spatialBlend))
    {
        return Fail("Audio.InvalidClip");
    }

    auto& slot = slots_[voice.index];
    slot.description.volume = ClampFinite(volume, 0.0f, kMaxGain, 1.0f);
    slot.description.pitch = ClampFinite(pitch, kMinPitch, kMaxPitch, 1.0f);
    slot.description.position = position;
    slot.description.spatialBlend = ClampFinite(spatialBlend, 0.0f, 1.0f, 0.0f);
    slot.description.minDistanceMeters = minDistanceMeters;
    slot.description.maxDistanceMeters = maxDistanceMeters;
    return ApplyVoiceMix(slot, lastListener_);
}

void AudioWorld::SetPaused(bool paused)
{
    if (paused_ == paused)
    {
        return;
    }
    paused_ = paused;
    if (!backend_->IsInitialized())
    {
        return;
    }
    for (const auto& slot : slots_)
    {
        if (slot.alive && !backend_->SetVoicePaused(slot.backendHandle, paused_))
        {
            Fail("Audio.BackendFailed");
        }
    }
}

void AudioWorld::SetMasterVolume(float volume)
{
    masterVolume_ = ClampFinite(volume, 0.0f, kMaxGain, 1.0f);
    if (!backend_->IsInitialized())
    {
        return;
    }
    for (auto& slot : slots_)
    {
        if (slot.alive && !ApplyVoiceMix(slot, lastListener_))
        {
            Fail("Audio.BackendFailed");
        }
    }
}

void AudioWorld::Update(const ListenerPose& listener)
{
    lastListener_ = listener;
    if (!backend_->IsInitialized())
    {
        return;
    }
    ReclaimFinished();
    for (auto& slot : slots_)
    {
        if (slot.alive && !ApplyVoiceMix(slot, lastListener_))
        {
            Fail("Audio.BackendFailed");
        }
    }
}

void AudioWorld::StopAll()
{
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(slots_.size()); ++i)
    {
        if (slots_[i].alive)
        {
            ReleaseSlot(i);
        }
    }
    if (backend_ && backend_->IsInitialized())
    {
        backend_->StopAll();
    }
}

std::size_t AudioWorld::ActiveVoiceCount() const
{
    std::size_t count = 0;
    for (const auto& slot : slots_)
    {
        if (slot.alive)
        {
            ++count;
        }
    }
    return count;
}

std::vector<VoiceHandle> AudioWorld::ActiveVoices() const
{
    std::vector<VoiceHandle> result;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(slots_.size()); ++i)
    {
        if (slots_[i].alive)
        {
            result.push_back({i, slots_[i].generation});
        }
    }
    return result;
}
} // namespace se
