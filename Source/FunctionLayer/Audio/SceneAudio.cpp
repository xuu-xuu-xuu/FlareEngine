#include "FunctionLayer/Audio/SceneAudio.h"

#include "CoreLayer/Math/TransformDecomposition.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace flare
{
namespace
{
bool ResolveEntityPose(const World& world, EntityHandle entity, Vec3& position, Quat& rotation)
{
    Transform worldTrs{};
    if (!TryDecomposeTrs(world.GetWorldMatrix(entity), worldTrs))
    {
        return false;
    }
    position = worldTrs.position;
    rotation = worldTrs.rotation;
    return true;
}

ListenerPose ResolveSceneListener(const World& world, float& listenerVolume)
{
    listenerVolume = 1.0f;

    StableEntityId bestListenerId = UINT64_MAX;
    EntityHandle listenerEntity{};
    float bestListenerVolume = 1.0f;

    StableEntityId bestPrimaryCameraId = UINT64_MAX;
    EntityHandle cameraEntity{};

    for (const auto entity : world.Entities())
    {
        if (!world.IsEnabled(entity))
        {
            continue;
        }
        const auto* record = world.Get(entity);
        if (!record)
        {
            continue;
        }

        const auto* listenerComp = world.GetRegisteredComponent<AudioListenerComponent>(
            world.FindComponent(entity, AudioListenerComponent::TypeId));
        if (listenerComp && listenerComp->Enabled() && record->stableId < bestListenerId)
        {
            bestListenerId = record->stableId;
            listenerEntity = entity;
            bestListenerVolume = listenerComp->values.empty()
                ? 1.0f
                : std::clamp(std::get<float>(listenerComp->values[0]), 0.0f, 4.0f);
        }

        const auto* camera = world.GetCamera(entity);
        const auto* camComp = world.GetComponent(world.FindComponent(entity, ComponentTypes::Camera));
        if (camera && camera->primary && camComp && camComp->Active() && record->stableId < bestPrimaryCameraId)
        {
            bestPrimaryCameraId = record->stableId;
            cameraEntity = entity;
        }
    }

    const EntityHandle target = listenerEntity ? listenerEntity : cameraEntity;
    if (target)
    {
        Vec3 pos{};
        Quat rot{};
        if (ResolveEntityPose(world, target, pos, rot))
        {
            if (listenerEntity)
            {
                listenerVolume = bestListenerVolume;
            }
            return ListenerPoseFromRotation(pos, rot);
        }
    }
    return {};
}
} // namespace

std::shared_ptr<const AudioClipData> SceneAudio::ResolveClipData(AssetStore* assets, const AssetId& id)
{
    if (!assets || id.empty())
    {
        return {};
    }
    std::vector<AssetDiagnostic> diagnostics;
    const auto clip = assets->LoadAudioClip(id, diagnostics);
    if (!clip || !clip->Valid())
    {
        return {};
    }

    const auto existing = clipCache_.find(id);
    if (existing != clipCache_.end() && existing->second.rawPtr == clip.get() && existing->second.data)
    {
        return existing->second.data;
    }

    auto data = std::make_shared<AudioClipData>();
    data->sampleRate = clip->sampleRate;
    data->channels = clip->channels;
    data->samples = clip->samples;
    clipCache_.insert_or_assign(id, CachedClip{clip.get(), data});
    return data;
}

bool SceneAudio::Synchronize(const World& world, AssetStore* assets, AudioWorld& audio, bool paused)
{
    error_.clear();
    float listenerVolume = 1.0f;
    const ListenerPose listener = ResolveSceneListener(world, listenerVolume);
    audio.SetPaused(paused);

    std::unordered_set<StableEntityId> aliveSources;
    for (const auto entity : world.Entities())
    {
        if (!world.IsEnabled(entity))
        {
            continue;
        }
        const auto* record = world.Get(entity);
        if (!record)
        {
            continue;
        }
        const auto* sourceComp = world.GetRegisteredComponent<AudioSourceComponent>(
            world.FindComponent(entity, AudioSourceComponent::TypeId));
        if (!sourceComp || !sourceComp->Enabled() || sourceComp->values.size() < 8)
        {
            continue;
        }

        Vec3 position{};
        Quat rotation{};
        if (!ResolveEntityPose(world, entity, position, rotation))
        {
            continue;
        }

        const StableEntityId id = record->stableId;
        aliveSources.insert(id);

        const auto& clipId = std::get<std::string>(sourceComp->values[0]);
        const float volume = std::get<float>(sourceComp->values[1]) * listenerVolume;
        const float pitch = std::get<float>(sourceComp->values[2]);
        const bool loop = std::get<bool>(sourceComp->values[3]);
        const bool playOnStart = std::get<bool>(sourceComp->values[4]);
        const float spatialBlend = std::get<float>(sourceComp->values[5]);
        const float minDistance = std::get<float>(sourceComp->values[6]);
        const float maxDistance = std::get<float>(sourceComp->values[7]);

        auto& tracked = sources_[id];
        if (tracked.voice && (tracked.clipId != clipId || tracked.loop != loop))
        {
            audio.Stop(tracked.voice);
            tracked.voice = {};
            tracked.started = false;
        }
        tracked.clipId = clipId;
        tracked.loop = loop;

        if (!paused && !tracked.started && playOnStart && !clipId.empty())
        {
            if (const auto clipData = ResolveClipData(assets, clipId))
            {
                AudioPlayDesc desc;
                desc.clip = clipData;
                desc.volume = volume;
                desc.pitch = pitch;
                desc.minDistanceMeters = minDistance;
                desc.maxDistanceMeters = maxDistance;
                desc.position = position;
                desc.spatialBlend = spatialBlend;
                desc.loop = loop;
                tracked.voice = audio.Play(desc);
                tracked.started = true;
            }
        }
        else if (tracked.voice && audio.IsPlaying(tracked.voice))
        {
            audio.SetVoiceParameters(
                tracked.voice,
                volume,
                pitch,
                position,
                spatialBlend,
                minDistance,
                maxDistance);
        }
        else if (tracked.voice && !audio.IsPlaying(tracked.voice))
        {
            tracked.voice = {};
        }
    }

    std::erase_if(sources_, [&](const auto& item) {
        if (aliveSources.contains(item.first))
        {
            return false;
        }
        if (item.second.voice)
        {
            audio.Stop(item.second.voice);
        }
        return true;
    });

    audio.Update(listener);
    return true;
}

bool SceneAudio::PlaySource(const World& world, AssetStore* assets, AudioWorld& audio, StableEntityId entityId)
{
    const EntityHandle entity = world.FindByStableId(entityId);
    if (!entity || !world.IsEnabled(entity))
    {
        error_ = "Audio.InvalidVoice";
        return false;
    }
    const auto* sourceComp = world.GetRegisteredComponent<AudioSourceComponent>(
        world.FindComponent(entity, AudioSourceComponent::TypeId));
    if (!sourceComp || !sourceComp->Enabled() || sourceComp->values.size() < 8)
    {
        error_ = "Audio.InvalidClip";
        return false;
    }

    Vec3 position{};
    Quat rotation{};
    if (!ResolveEntityPose(world, entity, position, rotation))
    {
        error_ = "Audio.InvalidClip";
        return false;
    }

    const auto& clipId = std::get<std::string>(sourceComp->values[0]);
    const auto clipData = ResolveClipData(assets, clipId);
    if (!clipData)
    {
        error_ = "Audio.InvalidClip";
        return false;
    }

    float listenerVolume = 1.0f;
    const ListenerPose listener = ResolveSceneListener(world, listenerVolume);
    auto& tracked = sources_[entityId];
    if (tracked.voice)
    {
        audio.Stop(tracked.voice);
        tracked.voice = {};
    }

    AudioPlayDesc desc;
    desc.clip = clipData;
    desc.volume = std::get<float>(sourceComp->values[1]) * listenerVolume;
    desc.pitch = std::get<float>(sourceComp->values[2]);
    desc.loop = std::get<bool>(sourceComp->values[3]);
    desc.spatialBlend = std::get<float>(sourceComp->values[5]);
    desc.minDistanceMeters = std::get<float>(sourceComp->values[6]);
    desc.maxDistanceMeters = std::get<float>(sourceComp->values[7]);
    desc.position = position;

    tracked.clipId = clipId;
    tracked.loop = desc.loop;
    tracked.started = true;
    audio.Update(listener);
    tracked.voice = audio.Play(desc);
    if (!tracked.voice)
    {
        error_ = audio.LastError();
        return false;
    }
    error_.clear();
    return true;
}

bool SceneAudio::StopSource(AudioWorld& audio, StableEntityId entityId)
{
    const auto it = sources_.find(entityId);
    if (it == sources_.end())
    {
        return false;
    }
    if (it->second.voice)
    {
        audio.Stop(it->second.voice);
        it->second.voice = {};
    }
    it->second.started = true;
    return true;
}

bool SceneAudio::IsSourcePlaying(const AudioWorld& audio, StableEntityId entityId) const
{
    const auto it = sources_.find(entityId);
    return it != sources_.end() && it->second.voice && audio.IsPlaying(it->second.voice);
}

void SceneAudio::Clear(AudioWorld& audio) noexcept
{
    for (auto& [id, tracked] : sources_)
    {
        (void)id;
        if (tracked.voice)
        {
            audio.Stop(tracked.voice);
        }
    }
    sources_.clear();
    clipCache_.clear();
    audio.StopAll();
}

bool ValidateSceneAudio(const World& world, std::string& error)
{
    error.clear();
    const auto readNum = [&](EntityHandle entity, std::string_view key, float fallback) -> float {
        const auto value = FeatureValue(world, entity, key);
        if (const auto* ptr = std::get_if<float>(&value)) return *ptr;
        return fallback;
    };
    for (const auto entity : world.Entities())
    {
        if (!world.IsEnabled(entity))
        {
            continue;
        }
        const bool hasListener = static_cast<bool>(world.FindComponent(entity, AudioListenerComponent::TypeId));
        const bool hasSource = static_cast<bool>(world.FindComponent(entity, AudioSourceComponent::TypeId));
        if (!hasListener && !hasSource)
        {
            continue;
        }
        Transform trs{};
        if (!TryDecomposeTrs(world.GetWorldMatrix(entity), trs))
        {
            error = "Audio.InvalidClip";
            return false;
        }
        if (hasSource)
        {
            const float volume = readNum(entity, "audio.volume", 1.0f);
            const float pitch = readNum(entity, "audio.pitch", 1.0f);
            const float spatialBlend = readNum(entity, "audio.spatialBlend", 1.0f);
            const float minDist = readNum(entity, "audio.minDistanceMeters", 1.0f);
            const float maxDist = readNum(entity, "audio.maxDistanceMeters", 30.0f);
            if (!std::isfinite(volume) || volume < 0.0f ||
                !std::isfinite(pitch) || pitch <= 0.0f ||
                !std::isfinite(spatialBlend) || spatialBlend < 0.0f || spatialBlend > 1.0f ||
                !std::isfinite(minDist) || minDist <= 0.0f ||
                !std::isfinite(maxDist) || maxDist < minDist)
            {
                error = "Metadata.InvalidValue";
                return false;
            }
        }
    }
    return true;
}
} // namespace flare
