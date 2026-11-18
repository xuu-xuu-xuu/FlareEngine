int32_t SE_CALL se_entity_destroy(SE_Session handle, SE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!session->WorldRef().Destroy(se::EntityHandle::FromPacked(entity))) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        SweepBodies(*session);
        return SE_OK;
    });
}
int32_t SE_CALL se_entity_is_alive(SE_Session handle, SE_Entity entity, int32_t* alive) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!alive) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *alive = session->ReadWorld().IsAlive(se::EntityHandle::FromPacked(entity)) ? 1 : 0;
        return SE_OK;
    });
}
int32_t SE_CALL se_entity_get_enabled(SE_Session handle,SE_Entity entity,int32_t* enabled)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!enabled)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto* value=s->ReadWorld().Get(se::EntityHandle::FromPacked(entity));if(!value)return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");*enabled=value->enabled?1:0;return SE_OK;});
}
int32_t SE_CALL se_entity_set_enabled(SE_Session handle,SE_Entity entity,int32_t enabled)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(enabled!=0&&enabled!=1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");if(s->editor&&!s->borrowedWorld)return Error(SE_BUSY,"Bridge.EditorReadOnly");return s->WorldRef().SetEnabled(se::EntityHandle::FromPacked(entity),enabled!=0)?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");});
}
int32_t SE_CALL se_script_component_attach(SE_Session handle,SE_Entity owner,uint64_t controlled,int32_t enabled,uint64_t* component)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!component||!controlled||(enabled!=0&&enabled!=1))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");if(s->editor&&!s->borrowedWorld)return Error(SE_BUSY,"Bridge.EditorReadOnly");const auto value=s->WorldRef().AttachScriptComponent(se::EntityHandle::FromPacked(owner),controlled,{},enabled!=0);if(!value)return Error(SE_INVALID_ARGUMENT,s->WorldRef().LastError().c_str());*component=value.Pack();return SE_OK;});
}
int32_t SE_CALL se_script_component_remove(SE_Session handle,uint64_t component)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();const auto value=se::ComponentHandle::FromPacked(component);if(!s->WorldRef().GetScriptComponent(value)&&!s->WorldRef().IsComponentPending(value,se::ComponentTypes::Script))return Error(SE_INVALID_HANDLE,"Bridge.InvalidComponent");return s->WorldRef().RemoveComponent(value)?SE_OK:Error(SE_INVALID_HANDLE,"Bridge.InvalidComponent");});
}
int32_t SE_CALL se_script_component_set_enabled(SE_Session handle,uint64_t component,int32_t enabled)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(enabled!=0&&enabled!=1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto value=se::ComponentHandle::FromPacked(component);if(!s->WorldRef().GetScriptComponent(value)&&!s->WorldRef().IsComponentPending(value,se::ComponentTypes::Script))return Error(SE_INVALID_HANDLE,"Bridge.InvalidComponent");return s->WorldRef().SetComponentEnabled(value,enabled!=0)?SE_OK:Error(SE_INVALID_ARGUMENT,s->WorldRef().LastError().c_str());});
}
int32_t SE_CALL se_script_component_state(SE_Session handle,uint64_t component,int32_t* alive,int32_t* active)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!alive||!active)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto* value=s->WorldRef().GetScriptComponent(se::ComponentHandle::FromPacked(component));*alive=value?1:0;*active=value&&value->Active()&&value->Enabled()&&s->WorldRef().IsEnabled(value->Owner())?1:0;return SE_OK;});
}
int32_t SE_CALL se_script_component_count(SE_Session handle,uint32_t* count)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!count)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*count=0;const auto& world=s->WorldRef();for(const auto owner:world.Entities())for(const auto value:world.Components(owner))if(world.GetComponent(value)->Type()==se::ComponentTypes::Script)++*count;return SE_OK;});
}
int32_t SE_CALL se_entity_get_transform(SE_Session handle, SE_Entity entity, SE_Transform* output) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!output) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto* t = session->ReadWorld().GetTransform(se::EntityHandle::FromPacked(entity));
        if (!t) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *output = {t->position.x,t->position.y,t->position.z,t->rotation.x,t->rotation.y,t->rotation.z,t->rotation.w,t->scale.x,t->scale.y,t->scale.z};
        return SE_OK;
    });
}
int32_t SE_CALL se_entity_set_transform(SE_Session handle, SE_Entity entity, const SE_Transform* value) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!value || !ValidTransform(*value)) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidTransform");
        const auto* record=session->WorldRef().Get(se::EntityHandle::FromPacked(entity));
        if (!record&&!session->WorldRef().IsPending(se::EntityHandle::FromPacked(entity))) return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        if (record&&HasBody(*session,record->stableId)) return Error(SE_BUSY,"Bridge.PhysicsOwnsTransform");
        if(session->characters.contains(entity))return Error(SE_BUSY,"Bridge.CharacterOwnsTransform");
        const se::Transform transform{{value->px,value->py,value->pz},{value->qx,value->qy,value->qz,value->qw},{value->sx,value->sy,value->sz}};
        return session->WorldRef().SetTransform(se::EntityHandle::FromPacked(entity), transform) ? SE_OK
            : Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
    });
}
namespace
{
se::EntityHandle FindByStableId(const se::World& world,std::uint64_t stable)
{
    for(auto candidate:world.Entities())
    {
        const auto* record=world.Get(candidate);
        if(record&&record->stableId==stable)return candidate;
    }
    return {};
}
}
int32_t SE_CALL se_entity_actor_play(SE_Session handle,uint64_t entity_stable_id,const char* clip,uint32_t length,float speed,int32_t loop) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!clip||length==0||length>4096||!ValidUtf8(clip,length)||!std::isfinite(speed))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        // 固定步内不能改组件属性（World 队列未清空），改为给姿态系统一个下一步生效的请求。
        s->Env().Skeletal().RequestPlay(entity_stable_id,std::string(clip,length),speed,loop!=0);
        return SE_OK;});
}
int32_t SE_CALL se_entity_joint_position(SE_Session handle,uint64_t entity_stable_id,const char* joint,uint32_t length,SE_Vector3* position,int32_t* found) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!joint||length==0||length>4096||!ValidUtf8(joint,length)||!position||!found)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        se::Vec3 value{};
        *found=s->Env().Skeletal().QueryJointPosition(s->WorldRef(),entity_stable_id,std::string(joint,length),value)?1:0;
        if(*found)*position={value.x,value.y,value.z};
        return SE_OK;});
}
int32_t SE_CALL se_entity_set_mesh(SE_Session handle,SE_Entity entity,const char* asset_id,uint32_t length,int32_t casts_shadow,int32_t receives_shadow) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!asset_id||length!=32||!ValidUtf8(asset_id,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        se::MeshComponent mesh;mesh.meshAsset.assign(asset_id,length);
        mesh.castsShadow=casts_shadow!=0;mesh.receivesShadow=receives_shadow!=0;
        const auto target=se::EntityHandle::FromPacked(entity);
        const auto* record=s->WorldRef().Get(target);
        if(!record&&!s->WorldRef().IsPending(target))return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        return s->WorldRef().SetMesh(target,mesh)?SE_OK:Error(SE_INVALID_ARGUMENT,"Bridge.WorldOperationFailed");});
}
int32_t SE_CALL se_entity_set_camera_fov(SE_Session handle,uint64_t entity_stable_id,float fov) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!std::isfinite(fov)||fov<0.05f||fov>3.0f)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto entity=FindByStableId(s->WorldRef(),entity_stable_id);
        if(!entity)return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        const auto* existing=s->WorldRef().GetCamera(entity);
        if(!existing)return Error(SE_INVALID_ARGUMENT,"World.MissingComponent");
        auto camera=*existing;camera.verticalFovRadians=fov;
        // SetCamera 通过 World 命令队列，允许脚本在同一固定步内与 Transform 一起写入。
        return s->WorldRef().SetCamera(entity,camera)?SE_OK:Error(SE_INVALID_ARGUMENT,"World.InvalidComponent");});
}
int32_t SE_CALL se_session_set_look(SE_Session handle,float mode,float blend) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!std::isfinite(mode)||!std::isfinite(blend))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        if(mode<0){s->appearance.lookMode=-1;s->appearance.lookBlend=0;return SE_OK;}
        if(mode>2||blend<0||blend>1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        s->appearance.lookMode=mode;s->appearance.lookBlend=blend;return SE_OK;});
}
int32_t SE_CALL se_camera_project_direction(SE_Session handle,uint64_t camera_stable_id,SE_Vector3 direction,SE_Vector2* screen_uv,int32_t* in_front) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!screen_uv||!in_front||!std::isfinite(direction.x)||!std::isfinite(direction.y)||!std::isfinite(direction.z))
            return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *in_front=0;
        const auto entity=FindByStableId(s->WorldRef(),camera_stable_id);
        const auto* camera=entity?s->WorldRef().GetCamera(entity):nullptr;
        if(!camera)return Error(SE_INVALID_HANDLE,"Bridge.InvalidEntity");
        const se::CameraInstance instance{camera_stable_id,s->WorldRef().GetWorldMatrix(entity),*camera,true};
        // 游戏窗口和编辑器 Play 都按实际游戏视口布局 UI。无窗口时保留已请求的尺寸或单位视口。
        auto size=s->UiRef().ViewportSize();
        if(size.x<=0||size.y<=0)
            size=s->windowWidth?se::UiPoint{float(s->windowWidth),float(s->windowHeight)}:se::UiPoint{1,1};
        se::CameraSnapshot snapshot;se::CameraError error{};
        if(!se::BuildCameraSnapshot(instance,{0,0,size.x,size.y},0,snapshot,error))return Error(SE_INVALID_ARGUMENT,"Camera.InvalidTransform");
        const auto length=std::sqrt(direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);
        if(!std::isfinite(length)||length<1e-6f)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const se::Vec3 unit{direction.x/length,direction.y/length,direction.z/length};
        const auto projected=se::ProjectWorldPoint(snapshot,snapshot.position+unit*1000.0f);
        if(!projected)return SE_OK;
        screen_uv->x=projected->x/size.x;screen_uv->y=projected->y/size.y;
        *in_front=1;
        return SE_OK;});
}
int32_t SE_CALL se_session_set_sky(SE_Session handle,const char* json,uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(length==0){s->appearance.sky.reset();return SE_OK;}
        if(!json||length>4096||!ValidUtf8(json,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        try
        {
            const auto document=nlohmann::json::parse(std::string(json,length));
            const auto vector=[&](const char* key,se::Vec3& output)
            {
                if(!document.contains(key)||!document[key].is_array()||document[key].size()!=3)return false;
                const se::Vec3 value{document[key][0].get<float>(),document[key][1].get<float>(),document[key][2].get<float>()};
                if(!se::IsFinite(value)||value.x<0||value.y<0||value.z<0||value.x>8||value.y>8||value.z>8)return false;
                output=value;return true;
            };
            se::SkyView view;view.enabled=true;
            if(!vector("zenith",view.zenith)||!vector("horizon",view.horizon)||!vector("ground",view.ground)||!vector("sunColor",view.sunColor))
                return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
            view.sunIntensity=document.value("sunIntensity",12.0f);
            view.sunAngularRadius=document.value("sunAngularRadius",0.012f);
            if(!std::isfinite(view.sunIntensity)||view.sunIntensity<0||view.sunIntensity>1000||
                !std::isfinite(view.sunAngularRadius)||view.sunAngularRadius<0.001f||view.sunAngularRadius>0.10f)
                return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
            s->appearance.sky=view;return SE_OK;
        }
        catch(const std::exception&){return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");}
    });
}
int32_t SE_CALL se_entity_get_stable_id(SE_Session handle, SE_Entity entity, uint64_t* stableId) noexcept{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!stableId) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto* record = session->ReadWorld().Get(se::EntityHandle::FromPacked(entity));
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *stableId = record->stableId; return SE_OK;
    });
}
int32_t SE_CALL se_entity_find(SE_Session handle, uint64_t stableId, SE_Entity* entity) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!entity) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto found = session->ReadWorld().FindByStableId(stableId);
        if (!session->ReadWorld().IsAlive(found)) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *entity = found.Pack(); return SE_OK;
    });
}
int32_t SE_CALL se_entity_get_name(SE_Session handle, SE_Entity entity, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        const auto* record = session->ReadWorld().Get(se::EntityHandle::FromPacked(entity));
        if (!record) return Error(SE_INVALID_HANDLE, "Bridge.InvalidEntity");
        return CopyText(record->name, buffer, capacity, required);
    });
}
int32_t SE_CALL se_scene_export(SE_Session handle, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        std::string text, error;
        if (!se::SerializeScene(session->ReadWorld(), text, error)) return Error(SE_INTERNAL_ERROR, "Bridge.SceneInvalid");
        return CopyText(text, buffer, capacity, required);
    });
}
int32_t SE_CALL se_scene_import(SE_Session handle, const char* text, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (session->stepping) return Error(SE_BUSY, "Bridge.SessionBusy");
        if (!text || length == 0 || length > 64u*1024u*1024u || !ValidUtf8(text, length))
            return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidUtf8");
        std::string error;
        if (!se::DeserializeScene(std::string_view(text,length), session->WorldRef(), error)) return Error(SE_INVALID_ARGUMENT, "Bridge.SceneInvalid");
        CloseEventPublishers(handle);session->eventSubscriptions.clear();
        session->Env().Events().ResetWorld(session->WorldRef());
        session->Env().Scenes().ResetWorld(session->WorldRef());
        auto& events=EventsFor(*session);
        const std::uint8_t state[8]={1,0,0,0,2,0,0,0};
        events.GetPublisher().Post(4,1,state,se::EventPhase::NextFrame);
        session->scenePhysics.Clear(session->physics);
        for (auto body:session->physics.Bodies()) session->physics.DestroyBody(body);
        session->physics.ResetTriggers();
        session->characterTriggerTracker.Clear();
        session->bodyOwners.clear();
        session->contactEvents.clear(); session->triggerEvents.clear(); session->physicsClock.Reset();
        session->characters.clear();
        return SE_OK;
    });
}

int32_t SE_CALL se_scene_set_callback(SE_Session handle,SE_SceneCallback callback,void* context)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");s->sceneCallback=callback;s->sceneContext=callback?context:nullptr;return SE_OK;});
}
int32_t SE_CALL se_scene_create(SE_Session handle,const char* name,uint32_t length,uint64_t* scene)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!scene||!name||!length||length>256||!ValidUtf8(name,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        auto& scenes=ScenesFor(*s);*scene=scenes.Create(std::string(name,length)).value;return *scene?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t SE_CALL se_scene_request_file(SE_Session handle,uint64_t scene,const char* path,uint32_t length,uint32_t mode,uint64_t replacing,uint64_t* request)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!request||!path||!length||length>32768||mode>1||!ValidUtf8(path,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        auto& scenes=ScenesFor(*s);*request=scenes.RequestFile({scene},std::filesystem::path(std::u8string(path,path+length)),static_cast<se::SceneLoadMode>(mode),{replacing});return *request?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t SE_CALL se_scene_request_json(SE_Session handle,uint64_t scene,const char* text,uint32_t length,uint32_t mode,uint64_t replacing,uint64_t* request)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!request||!text||!length||length>16*1024*1024||mode>1||!ValidUtf8(text,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        se::SceneDocument document;std::string error;if(!se::ParseSceneDocument({text,length},document,error))return Error(SE_INVALID_ARGUMENT,"Bridge.SceneInvalid");
        auto& scenes=ScenesFor(*s);*request=scenes.RequestDocument({scene},std::move(document),static_cast<se::SceneLoadMode>(mode),{replacing});return *request?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t SE_CALL se_scene_cancel(SE_Session handle,uint64_t scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();auto& scenes=ScenesFor(*s);return scenes.Cancel({scene})?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});}
int32_t SE_CALL se_scene_unload(SE_Session handle,uint64_t scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();auto& scenes=ScenesFor(*s);return scenes.Unload({scene})?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});}
int32_t SE_CALL se_scene_active(SE_Session handle,uint64_t* scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!scene)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*scene=ScenesFor(*s).ActiveScene().value;return SE_OK;});}
int32_t SE_CALL se_scene_of_entity(SE_Session handle,SE_Entity entity,uint64_t* scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!scene)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*scene=ScenesFor(*s).SceneOf(se::EntityHandle::FromPacked(entity)).value;return SE_OK;});}
int32_t SE_CALL se_scene_create_entity(SE_Session handle,uint64_t scene,const char* name,uint32_t length,SE_Entity* entity)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!entity||!name||length>4096||!ValidUtf8(name,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        auto& scenes=ScenesFor(*s);const auto created=scenes.CreateEntity({scene},std::string(name,length));*entity=created?created.Pack():0;return created?SE_OK:Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t SE_CALL se_scene_get(SE_Session handle,uint64_t scene,char* buffer,uint32_t capacity,uint32_t* required)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();const auto value=ScenesFor(*s).Inspect({scene});if(!value.handle)return Error(SE_INVALID_HANDLE,"Scene.InvalidRequest");
        auto members=nlohmann::json::array();for(const auto entity:value.members)members.push_back(entity.Pack());
        const nlohmann::json json{{"Id",value.handle.value},{"State",static_cast<uint32_t>(value.state)},{"Active",value.active},{"Request",value.request},{"Revision",value.revision},{"Scope",value.scope},{"Name",value.name},{"Source",value.source},{"RequestedSource",value.requestedSource},{"Error",value.error},{"Members",std::move(members)}};
        return CopyText(json.dump(),buffer,capacity,required);});
}
int32_t SE_CALL se_scene_stats(SE_Session handle,char* buffer,uint32_t capacity,uint32_t* required)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();const auto v=ScenesFor(*s).Statistics();
        return CopyText(nlohmann::json{{"Requested",v.requested},{"Activated",v.activated},{"Unloaded",v.unloaded},{"Failed",v.failed},{"Cancelled",v.cancelled},{"StaleResults",v.staleResults},{"CallbackFaults",v.callbackFaults},{"NotificationFailures",v.notificationFailures},{"PendingJobs",v.pendingJobs},{"Records",v.records},{"Members",v.members}}.dump(),buffer,capacity,required);});
}
int32_t SE_CALL se_scene_callback_scope(SE_Session handle,uint64_t scene,uint64_t* previous)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!previous)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");auto& scenes=ScenesFor(*s);se::SceneHandle prior;
        if(!scenes.ExchangeCreationScope({scene},prior))return Error(SE_INVALID_ARGUMENT,scenes.LastError().c_str());*previous=prior.value;return SE_OK;});
}
