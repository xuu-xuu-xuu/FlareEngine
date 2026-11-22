#pragma once

#include "FunctionLayer/Audio/AudioWorld.h"
#include "FunctionLayer/Runtime/World.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace flare
{
/// 将 World 中的 AudioListenerComponent 与 AudioSourceComponent 同步到 AudioWorld；
/// 只在运行时与播放期持有活跃发声句柄，暂停时挂起，停止/卸载时 Clear 释放全部音源。
class SceneAudio
{
public:
    bool Synchronize(const World& world, AssetStore* assets, AudioWorld& audio, bool paused = false);
    bool PlaySource(const World& world, AssetStore* assets, AudioWorld& audio, StableEntityId entityId);
    bool StopSource(AudioWorld& audio, StableEntityId entityId);
    [[nodiscard]] bool IsSourcePlaying(const AudioWorld& audio, StableEntityId entityId) const;
    void Clear(AudioWorld& audio) noexcept;

    [[nodiscard]] std::size_t TrackedSourceCount() const noexcept { return sources_.size(); }
    [[nodiscard]] const std::string& LastError() const noexcept { return error_; }

private:
    struct TrackedSource
    {
        VoiceHandle voice;
        AssetId clipId;
        bool started = false;
        bool loop = false;
    };

    struct CachedClip
    {
        const AudioClip* rawPtr = nullptr;
        std::shared_ptr<const AudioClipData> data;
    };

    std::shared_ptr<const AudioClipData> ResolveClipData(AssetStore* assets, const AssetId& id);

    std::unordered_map<StableEntityId, TrackedSource> sources_;
    std::unordered_map<AssetId, CachedClip> clipCache_;
    std::string error_;
};

[[nodiscard]] bool ValidateSceneAudio(const World& world, std::string& error);
} // namespace flare
