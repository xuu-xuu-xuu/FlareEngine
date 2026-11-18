int32_t SE_CALL se_nav_find_path(
    SE_Session handle,
    const char* nav_asset_id,
    uint32_t nav_asset_length,
    SE_Vector3 start,
    SE_Vector3 goal,
    SE_Vector3* out_waypoints,
    uint32_t max_waypoints,
    SE_NavPathInfo* out_info) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!out_info) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        if (max_waypoints > 0u && !out_waypoints) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        if (nav_asset_length > 0u && (!nav_asset_id || !ValidUtf8(nav_asset_id, nav_asset_length)))
        {
            return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }

        const se::Vec3 startPos{start.x, start.y, start.z};
        const se::Vec3 goalPos{goal.x, goal.y, goal.z};
        if (!se::IsFinite(startPos) || !se::IsFinite(goalPos))
        {
            return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        }

        const std::string assetId = nav_asset_id && nav_asset_length > 0u
            ? std::string(nav_asset_id, nav_asset_length)
            : std::string{};
        se::NavPathResult result;
        std::string errorKey;
        if (!s->sceneNavigation.FindPath(s->assets.get(), assetId, startPos, goalPos, result, errorKey))
        {
            return Error(SE_INVALID_ARGUMENT, errorKey.empty() ? "Navigation.NavMeshNotAssigned" : errorKey.c_str());
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
        return SE_OK;
    });
}

int32_t SE_CALL se_nav_set_destination(SE_Session handle, SE_Entity entity, SE_Vector3 target) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        const auto handleInWorld = se::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");

        std::string errorKey;
        if (!s->sceneNavigation.SetDestination(
                s->WorldRef(), s->assets.get(), record->stableId, {target.x, target.y, target.z}, errorKey))
        {
            return Error(SE_INVALID_ARGUMENT, errorKey.empty() ? "Navigation.AgentInactive" : errorKey.c_str());
        }
        return SE_OK;
    });
}

int32_t SE_CALL se_nav_stop_agent(SE_Session handle, SE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        const auto handleInWorld = se::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");

        std::string errorKey;
        if (!s->sceneNavigation.StopAgent(s->WorldRef(), record->stableId, errorKey))
        {
            return Error(SE_INVALID_ARGUMENT, errorKey.empty() ? "Navigation.AgentInactive" : errorKey.c_str());
        }
        return SE_OK;
    });
}

int32_t SE_CALL se_nav_remaining_distance(SE_Session handle, SE_Entity entity, float* out_meters) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!out_meters) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto handleInWorld = se::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        const auto* comp = s->WorldRef().GetComponent(
            s->WorldRef().FindComponent(handleInWorld, se::NavigationAgentComponent::TypeId));
        if (!comp || !comp->Active() || !s->WorldRef().IsEnabled(handleInWorld))
        {
            return Error(SE_INVALID_ARGUMENT, "Navigation.AgentInactive");
        }
        *out_meters = s->sceneNavigation.RemainingDistanceMeters(s->WorldRef(), record->stableId);
        return SE_OK;
    });
}

int32_t SE_CALL se_nav_agent_status(SE_Session handle, SE_Entity entity, int32_t* out_status) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle);
        if (!s) return MissingSession();
        if (!out_status) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto handleInWorld = se::EntityHandle::FromPacked(entity);
        const auto* record = s->WorldRef().Get(handleInWorld);
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        const auto* comp = s->WorldRef().GetComponent(
            s->WorldRef().FindComponent(handleInWorld, se::NavigationAgentComponent::TypeId));
        if (!comp || !comp->Active() || !s->WorldRef().IsEnabled(handleInWorld))
        {
            return Error(SE_INVALID_ARGUMENT, "Navigation.AgentInactive");
        }
        *out_status = static_cast<int32_t>(s->sceneNavigation.AgentStatus(record->stableId));
        return SE_OK;
    });
}
