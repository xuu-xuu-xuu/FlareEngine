namespace
{
bool HasConflictingPhysicsOwner(Session& s, SE_Entity entity, se::EntityHandle nativeEntity, se::StableEntityId stableId)
{
    return HasBody(s, stableId) || s.characters.contains(entity) || s.scenePhysics.Character(stableId) != nullptr ||
           bool(s.WorldRef().FindComponent(nativeEntity, se::PhysicsBodyComponent::TypeId)) ||
           bool(s.WorldRef().FindComponent(nativeEntity, se::ColliderSettingsComponent::TypeId)) ||
           bool(s.WorldRef().FindComponent(nativeEntity, se::CharacterSettingsComponent::TypeId));
}

int32_t AttachBodyForEntity(Session& s, SE_Entity entity, se::BodyDesc value, SE_Body* output)
{
    const auto nativeEntity = se::EntityHandle::FromPacked(entity);
    const auto* record = s.WorldRef().Get(nativeEntity);
    const auto* transform = s.WorldRef().GetTransform(nativeEntity);
    if (!record || !transform)
    {
        return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
    }
    if (record->parent || HasConflictingPhysicsOwner(s, entity, nativeEntity, record->stableId))
    {
        return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidBodyAttachment");
    }
    value.shape.center = transform->position;
    value.shape.rotation = transform->rotation;
    value.entityStableId = record->stableId;
    if (value.shapeKind == se::ColliderShapeKind::Box)
    {
        value.shape.halfExtents = {
            value.shape.halfExtents.x * std::abs(transform->scale.x),
            value.shape.halfExtents.y * std::abs(transform->scale.y),
            value.shape.halfExtents.z * std::abs(transform->scale.z)};
    }
    else
    {
        const float maxScale = std::max({std::abs(transform->scale.x), std::abs(transform->scale.y), std::abs(transform->scale.z)});
        value.radius *= maxScale;
    }
    auto body = s.physics.CreateBody(value);
    if (!s.physics.IsAlive(body))
    {
        return Error(SE_INVALID_ARGUMENT, "Bridge.PhysicsFailed");
    }
    try
    {
        s.bodyOwners.emplace(body.Pack(), nativeEntity);
    }
    catch (...)
    {
        s.physics.DestroyBody(body);
        throw;
    }
    *output = body.Pack();
    return SE_OK;
}
} // namespace

int32_t SE_CALL se_body_create(SE_Session handle, SE_Entity entity, const SE_BoxBodyDesc* desc, SE_Body* output) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle); if(!s) return MissingSession();
        if(!desc||!output) return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        se::BodyDesc value;
        value.shapeKind=se::ColliderShapeKind::Box;
        value.shape.halfExtents={desc->half_extents.x,desc->half_extents.y,desc->half_extents.z};
        value.mass=desc->mass; value.friction=desc->friction; value.restitution=desc->restitution;
        value.layer=desc->layer; value.mask=desc->mask;
        return AttachBodyForEntity(*s,entity,value,output);
    });
}
int32_t SE_CALL se_body_create_sphere(SE_Session handle, SE_Entity entity, const SE_SphereBodyDesc* desc, SE_Body* output) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle); if(!s) return MissingSession();
        if(!desc||!output||(desc->is_trigger!=0&&desc->is_trigger!=1)) return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        se::BodyDesc value;
        value.shapeKind=se::ColliderShapeKind::Sphere;
        value.radius=desc->radius;
        value.isTrigger=desc->is_trigger!=0;
        value.mass=desc->mass; value.friction=desc->friction; value.restitution=desc->restitution;
        value.layer=desc->layer; value.mask=desc->mask;
        return AttachBodyForEntity(*s,entity,value,output);
    });
}
int32_t SE_CALL se_body_set_trigger(SE_Session handle, SE_Body body, int32_t is_trigger) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle); if(!s) return MissingSession();
        if(is_trigger!=0&&is_trigger!=1) return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        if(!BodyAlive(*s,body)) return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
        return s->physics.SetTrigger(BodyFromPacked(body),is_trigger!=0)?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
    });
}
int32_t SE_CALL se_body_is_trigger(SE_Session handle, SE_Body body, int32_t* is_trigger) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle); if(!s) return MissingSession();
        if(!is_trigger) return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto* state=s->physics.GetBody(BodyFromPacked(body));
        if(!state||!BodyAlive(*s,body)) return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
        *is_trigger=state->description.isTrigger?1:0;
        return SE_OK;
    });
}
int32_t SE_CALL se_body_find(SE_Session handle,SE_Entity entity,SE_Body* output)noexcept
{
    return Protect([&]()->int32_t{auto* session=Find(handle);if(!session)return MissingSession();if(!output)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto owner=se::EntityHandle::FromPacked(entity);if(!session->WorldRef().IsAlive(owner))return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        if(!session->scenePhysics.Synchronize(session->WorldRef(),session->physics))return Error(SE_INTERNAL_ERROR,"Bridge.PhysicsFailed");
        *output=0;const auto id=session->WorldRef().Get(owner)->stableId;for(auto body:session->physics.Bodies())if(session->physics.GetBody(body)->description.entityStableId==id){*output=body.Pack();break;}return SE_OK;});
}
int32_t SE_CALL se_character_component_state(SE_Session handle,SE_Entity entity,SE_CharacterState* state)noexcept
{
    return Protect([&]()->int32_t{auto* session=Find(handle);if(!session)return MissingSession();if(!state)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto owner=se::EntityHandle::FromPacked(entity);if(!session->WorldRef().IsAlive(owner))return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        if(!session->scenePhysics.Synchronize(session->WorldRef(),session->physics))return Error(SE_INTERNAL_ERROR,"Bridge.PhysicsFailed");
        const auto* character=session->scenePhysics.Character(session->WorldRef().Get(owner)->stableId);if(!character)return Error(SE_INVALID_HANDLE,"Bridge.InvalidCharacter");
        *state={{character->position.x,character->position.y,character->position.z},character->verticalVelocity,character->grounded?1:0};return SE_OK;});
}
int32_t SE_CALL se_body_destroy(SE_Session handle, SE_Body body) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(s->scenePhysics.Owns(BodyFromPacked(body)))
        {
            const auto* state=s->physics.GetBody(BodyFromPacked(body));
            if(state)
            {
                const auto owner=s->WorldRef().FindByStableId(state->description.entityStableId);
                for(const auto typeId:{se::PhysicsBodyComponent::TypeId,se::ColliderSettingsComponent::TypeId})
                {
                    const auto component=s->WorldRef().FindComponent(owner,typeId);
                    if(component&&!s->WorldRef().RemoveComponent(component))return Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");
                }
            }
        }
        if(!s->physics.DestroyBody(BodyFromPacked(body)))return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");s->bodyOwners.erase(body);return SE_OK;});
}
int32_t SE_CALL se_body_get_state(SE_Session handle,SE_Body body,SE_BodyState* output) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle);if(!s)return MissingSession();if(!output)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto* state=s->physics.GetBody(BodyFromPacked(body));if(!state||!BodyAlive(*s,body))return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
        const auto& world=s->ReadWorld();const auto* transform=world.GetTransform(world.FindByStableId(state->description.entityStableId));
        if(!transform)return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        const auto& p=state->description.shape.center;const auto& q=state->description.shape.rotation;const auto& scale=transform->scale;
        const auto& linear=state->linearVelocity;const auto& angular=state->angularVelocity;
        *output={{p.x,p.y,p.z,q.x,q.y,q.z,q.w,scale.x,scale.y,scale.z},{linear.x,linear.y,linear.z},{angular.x,angular.y,angular.z},state->description.mass,state->sleeping?1:0};return SE_OK;
    });
}
int32_t SE_CALL se_body_set_velocity(SE_Session handle, SE_Body body, SE_Vector3 linear, SE_Vector3 angular) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!BodyAlive(*s,body))return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
        return s->physics.SetVelocity(BodyFromPacked(body),Vector(linear),Vector(angular))?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");});
}
int32_t SE_CALL se_body_apply_force(SE_Session handle, SE_Body body, SE_Vector3 force, SE_Vector3 point, int32_t impulse) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!BodyAlive(*s,body))return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
        const bool ok=impulse?s->physics.ApplyImpulse(BodyFromPacked(body),Vector(force),Vector(point)):s->physics.ApplyForce(BodyFromPacked(body),Vector(force),Vector(point));
        return ok?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");});
}
int32_t SE_CALL se_body_teleport(SE_Session handle, SE_Body body, const SE_Transform* value, int32_t clear) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!value||!ValidTransform(*value))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidTransform");
        const auto* state=s->physics.GetBody(BodyFromPacked(body));
        if(!state)return Error(SE_INVALID_HANDLE,"Bridge.InvalidBody");
        auto entity=s->WorldRef().FindByStableId(state->description.entityStableId);
        const auto* old=s->WorldRef().GetTransform(entity);
        if(!old)return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        if(old->scale.x!=value->sx||old->scale.y!=value->sy||old->scale.z!=value->sz)return Error(SE_INVALID_ARGUMENT,"Bridge.BodyScaleChanged");
        auto transform=*old;
        transform.position={value->px,value->py,value->pz}; transform.rotation={value->qx,value->qy,value->qz,value->qw};
        if(!s->physics.Teleport(BodyFromPacked(body),transform.position,transform.rotation,clear!=0))return Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");
        return s->WorldRef().SetTransform(entity,transform)?SE_OK:Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");});
}
int32_t SE_CALL se_physics_set_debug_draw(SE_Session handle,int32_t enabled,int32_t* previous) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!previous||(enabled!=0&&enabled!=1))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *previous=s->physicsDebugDraw?1:0;s->physicsDebugDraw=enabled!=0;return SE_OK;});
}
int32_t SE_CALL se_physics_step(SE_Session handle, double dt) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(s->environmentDriven)return Error(SE_BUSY,"Script.PhysicsClockOwned");return PhysicsStep(*s,dt);});
}
int32_t SE_CALL se_physics_advance(SE_Session handle, double dt, uint32_t* steps, double* dropped) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!steps||!dropped||!std::isfinite(dt)||dt<0||dt>1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidDelta");
        if(s->environmentDriven)return Error(SE_BUSY,"Script.PhysicsClockOwned");
        int32_t result=SE_OK;
        *steps=static_cast<uint32_t>(s->physicsClock.Advance(dt,[&](double fixed){if(result==SE_OK)result=PhysicsStep(*s,fixed);}));
        *dropped=s->physicsClock.DroppedSeconds(); return result;});
}
int32_t SE_CALL se_physics_raycast(SE_Session handle, SE_Vector3 origin, SE_Vector3 direction, float distance, uint32_t mask, SE_RayHit* output, int32_t* found) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!output||!found||!std::isfinite(distance)||distance<=0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *output={}; *found=0;
        const auto hit=s->physics.Raycast(Vector(origin),Vector(direction),distance,mask);
        if(hit){*found=1; *output={hit->body.Pack(),hit->entityStableId,{hit->point.x,hit->point.y,hit->point.z},{hit->normal.x,hit->normal.y,hit->normal.z},hit->distance,0};}
        return SE_OK;});
}
int32_t SE_CALL se_physics_drain_contacts(SE_Session handle, SE_ContactEvent* output, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!required||(!output&&capacity!=0))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *required=static_cast<uint32_t>(s->contactEvents.size());
        if(capacity<*required)return SE_BUFFER_TOO_SMALL;
        if(!s->contactEvents.empty())std::copy(s->contactEvents.begin(),s->contactEvents.end(),output);
        s->contactEvents.clear(); return SE_OK;});
}
int32_t SE_CALL se_physics_drain_triggers(SE_Session handle, SE_TriggerEvent* output, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle); if(!s)return MissingSession();
        if(!required||(!output&&capacity!=0))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *required=static_cast<uint32_t>(s->triggerEvents.size());
        if(capacity<*required)return SE_BUFFER_TOO_SMALL;
        if(!s->triggerEvents.empty())std::copy(s->triggerEvents.begin(),s->triggerEvents.end(),output);
        s->triggerEvents.clear(); return SE_OK;});
}
int32_t SE_CALL se_character_create(SE_Session handle, SE_Entity entity, const SE_CharacterDesc* desc) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!desc)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto nativeEntity=se::EntityHandle::FromPacked(entity);
        const auto* record=s->WorldRef().Get(nativeEntity);const auto* transform=s->WorldRef().GetTransform(nativeEntity);
        if(!record||!transform)return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        if(record->parent||HasConflictingPhysicsOwner(*s,entity,nativeEntity,record->stableId))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidBodyAttachment");
        if(!std::isfinite(desc->radius)||desc->radius<=0||!std::isfinite(desc->segment_half_height)||desc->segment_half_height<0
            ||!std::isfinite(desc->walk_speed)||desc->walk_speed<0||!std::isfinite(desc->jump_speed)||desc->jump_speed<0
            ||!std::isfinite(desc->maximum_push_impulse)||desc->maximum_push_impulse<0)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidCharacter");
        se::CharacterController value;value.position=transform->position;value.radius=desc->radius;value.segmentHalfHeight=desc->segment_half_height;
        value.walkSpeed=desc->walk_speed;value.jumpSpeed=desc->jump_speed;value.maximumPushImpulse=desc->maximum_push_impulse;
        s->characters.emplace(entity,value);return SE_OK;});
}
int32_t SE_CALL se_character_move(SE_Session handle, SE_Entity entity, SE_Vector3 direction, int32_t jump, float seconds, SE_CharacterState* state) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!state)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto found=s->characters.find(entity);const auto* transform=s->WorldRef().GetTransform(se::EntityHandle::FromPacked(entity));
        if(!transform)return Error(SE_INVALID_HANDLE,"Bridge.InvalidCharacter");
        if(found==s->characters.end())
        {
            const auto owner=se::EntityHandle::FromPacked(entity);const auto id=s->WorldRef().Get(owner)->stableId;
            if(!s->scenePhysics.MoveCharacter(s->WorldRef(),s->physics,id,Vector(direction),jump!=0,seconds))return Error(SE_INVALID_HANDLE,"Bridge.InvalidCharacter");
            const auto* value=s->scenePhysics.Character(id);*state={{value->position.x,value->position.y,value->position.z},value->verticalVelocity,value->grounded?1:0};return SE_OK;
        }
        auto& value=found->second;
        if(!value.Move(s->physics,Vector(direction),jump!=0,seconds))return Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");
        auto updated=*transform;updated.position=value.position;
        if(!s->WorldRef().SetTransform(se::EntityHandle::FromPacked(entity),updated))return Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");
        *state={{value.position.x,value.position.y,value.position.z},value.verticalVelocity,value.grounded?1:0};return SE_OK;});
}
int32_t SE_CALL se_character_teleport(SE_Session handle, SE_Entity entity, SE_Vector3 position) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!std::isfinite(position.x)||!std::isfinite(position.y)||!std::isfinite(position.z))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidTransform");
        const auto found=s->characters.find(entity);const auto* transform=s->WorldRef().GetTransform(se::EntityHandle::FromPacked(entity));
        if(!transform)return Error(SE_INVALID_HANDLE,"Bridge.InvalidCharacter");
        if(found==s->characters.end())return s->scenePhysics.TeleportCharacter(s->WorldRef(),s->WorldRef().Get(se::EntityHandle::FromPacked(entity))->stableId,Vector(position))?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidCharacter");
        auto& value=found->second;value.position=Vector(position);value.verticalVelocity=0;value.grounded=false;
        auto updated=*transform;updated.position=value.position;
        return s->WorldRef().SetTransform(se::EntityHandle::FromPacked(entity),updated)?SE_OK:Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");});
}
int32_t SE_CALL se_character_destroy(SE_Session handle, SE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        const auto component=s->WorldRef().FindComponent(se::EntityHandle::FromPacked(entity),se::CharacterSettingsComponent::TypeId);
        if(component)return s->WorldRef().RemoveComponent(component)?SE_OK:Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");
        return s->characters.erase(entity)?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidCharacter");});
}

