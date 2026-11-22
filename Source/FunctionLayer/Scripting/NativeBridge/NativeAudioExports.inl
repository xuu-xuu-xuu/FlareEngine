namespace
{
flare::VoiceHandle UnpackAudioVoice(FLARE_AudioVoice packed) noexcept
{
    if (packed == 0) return {};
    return {static_cast<std::uint32_t>(packed & 0xFFFFFFFFu), static_cast<std::uint32_t>(packed >> 32)};
}

bool ValidAudioPlayDesc(const FLARE_AudioPlayDesc& desc) noexcept
{
    return std::isfinite(desc.position.x) && std::isfinite(desc.position.y) && std::isfinite(desc.position.z) &&
           std::isfinite(desc.volume) && desc.volume >= 0.0f &&
           std::isfinite(desc.pitch) && desc.pitch > 0.0f &&
           std::isfinite(desc.min_distance_meters) && desc.min_distance_meters >= 0.0f &&
           std::isfinite(desc.max_distance_meters) && desc.max_distance_meters >= 0.0f &&
           std::isfinite(desc.spatial_blend) &&
           (desc.loop == 0 || desc.loop == 1);
}
} // namespace

int32_t FLARE_CALL flare_audio_play(
    FLARE_Session handle, const char* asset_id, uint32_t length, const FLARE_AudioPlayDesc* description, FLARE_AudioVoice* voice) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!s->assets) return Error(FLARE_INVALID_ARGUMENT, "Bridge.AssetsNotConfigured");
        if (!asset_id || length == 0 || length > 4096u || !ValidUtf8(asset_id, length) || !description || !voice ||
            !ValidAudioPlayDesc(*description))
        {
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }
        *voice = 0;
        std::vector<flare::AssetDiagnostic> diagnostics;
        const auto clip = s->assets->LoadAudioClip(std::string(asset_id, length), diagnostics);
        if (!clip || !clip->Valid())
        {
            const char* key = diagnostics.empty() ? "Bridge.AssetLoadFailed" : diagnostics.front().key.c_str();
            return Error(FLARE_INVALID_ARGUMENT, key);
        }
        auto data = std::make_shared<flare::AudioClipData>();
        data->sampleRate = clip->sampleRate;
        data->channels = clip->channels;
        data->samples = clip->samples;
        flare::AudioPlayDesc nativeDesc;
        nativeDesc.clip = std::move(data);
        nativeDesc.volume = description->volume;
        nativeDesc.pitch = description->pitch;
        nativeDesc.minDistanceMeters = description->min_distance_meters;
        nativeDesc.maxDistanceMeters = description->max_distance_meters;
        nativeDesc.position = {description->position.x, description->position.y, description->position.z};
        nativeDesc.spatialBlend = description->spatial_blend;
        nativeDesc.loop = description->loop != 0;
        nativeDesc.priority = description->priority;
        const auto handleOut = s->audio.Play(nativeDesc);
        if (!handleOut)
        {
            return Error(FLARE_INTERNAL_ERROR, s->audio.LastError().empty() ? "Audio.PlaybackFailed" : s->audio.LastError().c_str());
        }
        *voice = handleOut.Pack();
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_stop_voice(FLARE_Session handle, FLARE_AudioVoice voice) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        const auto decoded = UnpackAudioVoice(voice);
        if (!decoded) return Error(FLARE_INVALID_HANDLE, "Audio.InvalidVoice");
        s->audio.Stop(decoded);
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_is_voice_playing(FLARE_Session handle, FLARE_AudioVoice voice, int32_t* playing) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!playing) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto decoded = UnpackAudioVoice(voice);
        *playing = (decoded && s->audio.IsPlaying(decoded)) ? 1 : 0;
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_set_voice_parameters(
    FLARE_Session handle, FLARE_AudioVoice voice, const FLARE_AudioPlayDesc* description) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!description || !ValidAudioPlayDesc(*description))
        {
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }
        const auto decoded = UnpackAudioVoice(voice);
        if (!decoded) return Error(FLARE_INVALID_HANDLE, "Audio.InvalidVoice");
        if (!s->audio.SetVoiceParameters(
                decoded,
                description->volume,
                description->pitch,
                {description->position.x, description->position.y, description->position.z},
                description->spatial_blend,
                description->min_distance_meters,
                description->max_distance_meters))
        {
            return Error(FLARE_INVALID_HANDLE, "Audio.InvalidVoice");
        }
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_play_source(FLARE_Session handle, FLARE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!s->assets) return Error(FLARE_INVALID_ARGUMENT, "Bridge.AssetsNotConfigured");
        const auto* record = s->WorldRef().Get(flare::EntityHandle::FromPacked(entity));
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        if (!s->sceneAudio.PlaySource(s->WorldRef(), s->assets.get(), s->audio, record->stableId))
        {
            return Error(FLARE_INVALID_ARGUMENT, s->sceneAudio.LastError().empty() ? "Audio.PlaybackFailed" : s->sceneAudio.LastError().c_str());
        }
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_stop_source(FLARE_Session handle, FLARE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        const auto* record = s->WorldRef().Get(flare::EntityHandle::FromPacked(entity));
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        s->sceneAudio.StopSource(s->audio, record->stableId);
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_is_source_playing(FLARE_Session handle, FLARE_Entity entity, int32_t* playing) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!playing) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto* record = s->WorldRef().Get(flare::EntityHandle::FromPacked(entity));
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *playing = s->sceneAudio.IsSourcePlaying(s->audio, record->stableId) ? 1 : 0;
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_set_master_volume(FLARE_Session handle, float volume) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!std::isfinite(volume) || volume < 0.0f) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        s->audio.SetMasterVolume(volume);
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_get_master_volume(FLARE_Session handle, float* volume) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!volume) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *volume = s->audio.MasterVolume();
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_set_paused(FLARE_Session handle, int32_t paused) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (paused != 0 && paused != 1) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        s->audio.SetPaused(paused != 0);
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_stop_all(FLARE_Session handle) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        s->sceneAudio.Clear(s->audio);
        s->audio.StopAll();
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_active_voice_count(FLARE_Session handle, uint32_t* count) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!count) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *count = static_cast<uint32_t>(s->audio.ActiveVoiceCount());
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_audio_use_null_backend(FLARE_Session handle, int32_t force_null) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (force_null != 0 && force_null != 1) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        s->sceneAudio.Clear(s->audio);
        s->audio.SetBackend(nullptr);
        if (s->audioBackend)
        {
            s->audioBackend->Shutdown();
            s->audioBackend.reset();
        }
        if (force_null == 0)
        {
            s->audioBackend = std::make_unique<flare::XAudio2Backend>();
            if (s->audioBackend->Initialize()) s->audio.SetBackend(s->audioBackend.get());
            else s->audioBackend.reset();
        }
        return FLARE_OK;
    });
}
