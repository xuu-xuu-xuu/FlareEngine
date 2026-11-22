int32_t FLARE_CALL flare_nav_find_path(
    FLARE_Session handle,
    const char* nav_asset_id,
    uint32_t nav_asset_length,
    FLARE_Vector3 start,
    FLARE_Vector3 goal,
    FLARE_Vector3* out_waypoints,
    uint32_t max_waypoints,
    FLARE_NavPathInfo* out_info) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!out_info) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        if (max_waypoints > 0u && !out_waypoints) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        if (nav_asset_length > 0u && (!nav_asset_id || !ValidUtf8(nav_asset_id, nav_asset_length)))
        {
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }

        const flare::Vec3 startPos{start.x, start.y, start.z};
        const flare::Vec3 goalPos{goal.x, goal.y, goal.z};
        if (!flare::IsFinite(startPos) || !flare::IsFinite(goalPos))
        {
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }

        const std::string assetId = nav_asset_id && nav_asset_length > 0u
            ? std::string(nav_asset_id, nav_asset_length)
            : std::string{};
        flare::NavPathResult result;
        std::string errorKey;
        if (!s->sceneNavigation.FindPath(s->assets.get(), assetId, startPos, goalPos, result, errorKey))
        {
            return Error(FLARE_INVALID_ARGUMENT, errorKey.empty() ? "Navigation.NavMeshNotAssigned" : errorKey.c_str());
        }

        out_info->status = static_cast<int32_t>(result.status);
        out_info->waypoint_count = static_cast<uint32_t>(result.waypoints.size());
        out_info->total_length_meters = result.totalLengthMeters;
        if (out_waypoints && max_waypoints > 0u)
        {
            const uint32_t count = std::min(max_waypoints, out_info->waypoint_count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const auto& p = result.waypoints[i];
                out_waypoints[i] = {p.x, p.y, p.z};
            }
        }
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_nav_set_destination(FLARE_Session handle, FLARE_Entity entity, FLARE_Vector3 target) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        const auto handleInWorld = flare::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");

        std::string errorKey;
        if (!s->sceneNavigation.SetDestination(
                s->WorldRef(), s->assets.get(), record->stableId, {target.x, target.y, target.z}, errorKey))
        {
            return Error(FLARE_INVALID_ARGUMENT, errorKey.empty() ? "Navigation.AgentInactive" : errorKey.c_str());
        }
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_nav_stop_agent(FLARE_Session handle, FLARE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        const auto handleInWorld = flare::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");

        std::string errorKey;
        if (!s->sceneNavigation.StopAgent(s->WorldRef(), record->stableId, errorKey))
        {
            return Error(FLARE_INVALID_ARGUMENT, errorKey.empty() ? "Navigation.AgentInactive" : errorKey.c_str());
        }
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_nav_remaining_distance(FLARE_Session handle, FLARE_Entity entity, float* out_meters) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!out_meters) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto handleInWorld = flare::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        const auto* comp = s->WorldRef().GetComponent(
            s->WorldRef().FindComponent(handleInWorld, flare::NavigationAgentComponent::TypeId));
        if (!comp || !comp->Active() || !s->WorldRef().IsEnabled(handleInWorld))
        {
            return Error(FLARE_INVALID_ARGUMENT, "Navigation.AgentInactive");
        }
        *out_meters = s->sceneNavigation.RemainingDistanceMeters(s->WorldRef(), record->stableId);
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_nav_agent_status(FLARE_Session handle, FLARE_Entity entity, int32_t* out_status) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!out_status) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto handleInWorld = flare::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        const auto* comp = s->WorldRef().GetComponent(
            s->WorldRef().FindComponent(handleInWorld, flare::NavigationAgentComponent::TypeId));
        if (!comp || !comp->Active() || !s->WorldRef().IsEnabled(handleInWorld))
        {
            return Error(FLARE_INVALID_ARGUMENT, "Navigation.AgentInactive");
        }
        *out_status = static_cast<int32_t>(s->sceneNavigation.AgentStatus(record->stableId));
        return FLARE_OK;
    });
}
