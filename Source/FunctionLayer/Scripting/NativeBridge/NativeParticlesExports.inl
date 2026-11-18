int32_t SE_CALL se_particles_emit_burst(
    SE_Session handle, SE_Entity entity, uint32_t count, uint32_t* spawned) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!spawned) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *spawned = 0;
        const auto handleInWorld = se::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        const auto* component = s->WorldRef().GetComponent(
            s->WorldRef().FindComponent(handleInWorld, se::ParticleEmitterComponent::TypeId));
        if (!component || !component->Active() || !s->WorldRef().IsEnabled(handleInWorld))
        {
            return Error(SE_INVALID_ARGUMENT, "Particle.EmitterInactive");
        }
        *spawned = static_cast<uint32_t>(s->sceneParticles.EmitBurst(s->WorldRef(), record->stableId, count));
        return SE_OK;
    });
}

int32_t SE_CALL se_particles_set_playing(SE_Session handle, SE_Entity entity, int32_t playing) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (playing != 0 && playing != 1) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto handleInWorld = se::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        if (!s->sceneParticles.SetPlaying(s->WorldRef(), record->stableId, playing != 0))
        {
            return Error(SE_INVALID_ARGUMENT, "Particle.EmitterInactive");
        }
        return SE_OK;
    });
}

int32_t SE_CALL se_particles_clear(SE_Session handle, SE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (entity == 0)
        {
            s->sceneParticles.ClearParticles();
            return SE_OK;
        }
        const auto* record = s->WorldRef().Get(se::EntityHandle::FromPacked(entity));
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        s->sceneParticles.ClearParticles(record->stableId);
        return SE_OK;
    });
}

int32_t SE_CALL se_particles_active_count(SE_Session handle, SE_Entity entity, uint32_t* count) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!count) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        if (entity == 0)
        {
            *count = static_cast<uint32_t>(s->sceneParticles.ActiveParticleCount());
            return SE_OK;
        }
        const auto* record = s->WorldRef().Get(se::EntityHandle::FromPacked(entity));
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *count = static_cast<uint32_t>(s->sceneParticles.ActiveParticleCount(record->stableId));
        return SE_OK;
    });
}

int32_t SE_CALL se_particles_emitter_count(SE_Session handle, uint32_t* count) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!count) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *count = static_cast<uint32_t>(s->sceneParticles.TrackedEmitterCount());
        return SE_OK;
    });
}
