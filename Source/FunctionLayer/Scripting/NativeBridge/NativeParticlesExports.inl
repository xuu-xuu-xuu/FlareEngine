int32_t FLARE_CALL flare_particles_emit_burst(
    FLARE_Session handle, FLARE_Entity entity, uint32_t count, uint32_t* spawned) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!spawned) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *spawned = 0;
        const auto handleInWorld = flare::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        const auto* component = s->WorldRef().GetComponent(
            s->WorldRef().FindComponent(handleInWorld, flare::ParticleEmitterComponent::TypeId));
        if (!component || !component->Active() || !s->WorldRef().IsEnabled(handleInWorld))
        {
            return Error(FLARE_INVALID_ARGUMENT, "Particle.EmitterInactive");
        }
        *spawned = static_cast<uint32_t>(s->sceneParticles.EmitBurst(s->WorldRef(), record->stableId, count));
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_particles_set_playing(FLARE_Session handle, FLARE_Entity entity, int32_t playing) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (playing != 0 && playing != 1) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto handleInWorld = flare::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        if (!s->sceneParticles.SetPlaying(s->WorldRef(), record->stableId, playing != 0))
        {
            return Error(FLARE_INVALID_ARGUMENT, "Particle.EmitterInactive");
        }
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_particles_clear(FLARE_Session handle, FLARE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (entity == 0)
        {
            s->sceneParticles.ClearParticles();
            return FLARE_OK;
        }
        const auto* record = s->WorldRef().Get(flare::EntityHandle::FromPacked(entity));
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        s->sceneParticles.ClearParticles(record->stableId);
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_particles_active_count(FLARE_Session handle, FLARE_Entity entity, uint32_t* count) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!count) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        if (entity == 0)
        {
            *count = static_cast<uint32_t>(s->sceneParticles.ActiveParticleCount());
            return FLARE_OK;
        }
        const auto* record = s->WorldRef().Get(flare::EntityHandle::FromPacked(entity));
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *count = static_cast<uint32_t>(s->sceneParticles.ActiveParticleCount(record->stableId));
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_particles_emitter_count(FLARE_Session handle, uint32_t* count) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!count) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *count = static_cast<uint32_t>(s->sceneParticles.TrackedEmitterCount());
        return FLARE_OK;
    });
}
