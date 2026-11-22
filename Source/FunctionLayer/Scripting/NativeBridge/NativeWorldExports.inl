int32_t FLARE_CALL flare_entity_destroy(FLARE_Session handle, FLARE_Entity entity) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!session->WorldRef().Destroy(flare::EntityHandle::FromPacked(entity))) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        SweepBodies(*session);
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_is_alive(FLARE_Session handle, FLARE_Entity entity, int32_t* alive) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!alive) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *alive = session->ReadWorld().IsAlive(flare::EntityHandle::FromPacked(entity)) ? 1 : 0;
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_get_enabled(FLARE_Session handle,FLARE_Entity entity,int32_t* enabled)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!enabled)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto* value=s->ReadWorld().GetKnown(flare::EntityHandle::FromPacked(entity));if(!value)return Error(FLARE_INVALID_HANDLE,"Bridge.InvalidEntity");*enabled=value->enabled?1:0;return FLARE_OK;});
}
int32_t FLARE_CALL flare_entity_set_enabled(FLARE_Session handle,FLARE_Entity entity,int32_t enabled)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(enabled!=0&&enabled!=1)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");if(s->editor&&!s->borrowedWorld)return Error(FLARE_BUSY,"Bridge.EditorReadOnly");return s->WorldRef().SetEnabled(flare::EntityHandle::FromPacked(entity),enabled!=0)?FLARE_OK:Error(FLARE_INVALID_HANDLE,"Bridge.InvalidEntity");});
}
int32_t FLARE_CALL flare_script_component_attach(FLARE_Session handle,FLARE_Entity owner,uint64_t controlled,int32_t enabled,uint64_t* component)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!component||!controlled||(enabled!=0&&enabled!=1))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");if(s->editor&&!s->borrowedWorld)return Error(FLARE_BUSY,"Bridge.EditorReadOnly");const auto value=s->WorldRef().AttachScriptComponent(flare::EntityHandle::FromPacked(owner),controlled,{},enabled!=0);if(!value)return Error(FLARE_INVALID_ARGUMENT,s->WorldRef().LastError().c_str());*component=value.Pack();return FLARE_OK;});
}
int32_t FLARE_CALL flare_script_component_remove(FLARE_Session handle,uint64_t component)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();const auto value=flare::ComponentHandle::FromPacked(component);if(!s->WorldRef().GetScriptComponent(value)&&!s->WorldRef().IsComponentPending(value,flare::ComponentTypes::Script))return Error(FLARE_INVALID_HANDLE,"Bridge.InvalidComponent");return s->WorldRef().RemoveComponent(value)?FLARE_OK:Error(FLARE_INVALID_HANDLE,"Bridge.InvalidComponent");});
}
int32_t FLARE_CALL flare_script_component_set_enabled(FLARE_Session handle,uint64_t component,int32_t enabled)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(enabled!=0&&enabled!=1)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto value=flare::ComponentHandle::FromPacked(component);if(!s->WorldRef().GetScriptComponent(value)&&!s->WorldRef().IsComponentPending(value,flare::ComponentTypes::Script))return Error(FLARE_INVALID_HANDLE,"Bridge.InvalidComponent");return s->WorldRef().SetComponentEnabled(value,enabled!=0)?FLARE_OK:Error(FLARE_INVALID_ARGUMENT,s->WorldRef().LastError().c_str());});
}
int32_t FLARE_CALL flare_script_component_state(FLARE_Session handle,uint64_t component,int32_t* alive,int32_t* active)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!alive||!active)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto* value=s->WorldRef().GetScriptComponent(flare::ComponentHandle::FromPacked(component));*alive=value?1:0;*active=value&&value->Active()&&value->Enabled()&&s->WorldRef().IsEnabled(value->Owner())?1:0;return FLARE_OK;});
}
int32_t FLARE_CALL flare_script_component_count(FLARE_Session handle,uint32_t* count)noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(!count)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*count=0;const auto& world=s->WorldRef();for(const auto owner:world.Entities())for(const auto value:world.Components(owner))if(world.GetComponent(value)->Type()==flare::ComponentTypes::Script)++*count;return FLARE_OK;});
}
int32_t FLARE_CALL flare_entity_get_transform(FLARE_Session handle, FLARE_Entity entity, FLARE_Transform* output) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!output) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto* t = session->ReadWorld().GetKnownTransform(flare::EntityHandle::FromPacked(entity));
        if (!t) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *output = {t->position.x,t->position.y,t->position.z,t->rotation.x,t->rotation.y,t->rotation.z,t->rotation.w,t->scale.x,t->scale.y,t->scale.z};
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_set_transform(FLARE_Session handle, FLARE_Entity entity, const FLARE_Transform* value) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!value || !ValidTransform(*value)) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidTransform");
        const auto targetHandle = flare::EntityHandle::FromPacked(entity);
        const auto* record=session->WorldRef().Get(targetHandle);
        if (!record&&!session->WorldRef().IsPending(targetHandle)) return Error(FLARE_INVALID_HANDLE,"Bridge.InvalidEntity");
        if (record&&HasBody(*session,record->stableId)) return Error(FLARE_BUSY,"Bridge.PhysicsOwnsTransform");
        if (session->characters.contains(entity))
        {
            const auto* old = session->WorldRef().GetKnownTransform(targetHandle);
            if (!old || old->position.x != value->px || old->position.y != value->py || old->position.z != value->pz)
                return Error(FLARE_BUSY,"Bridge.CharacterOwnsTransform");
        }
        const flare::Transform transform{{value->px,value->py,value->pz},{value->qx,value->qy,value->qz,value->qw},{value->sx,value->sy,value->sz}};
        return session->WorldRef().SetTransform(targetHandle, transform) ? FLARE_OK
            : Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
    });
}
namespace
{
flare::EntityHandle FindByStableId(const flare::World& world,std::uint64_t stable)
{
    for(auto candidate:world.Entities())
    {
        const auto* record=world.Get(candidate);
        if(record&&record->stableId==stable)return candidate;
    }
    return {};
}
flare::EntityHandle FindPrimaryOrCamera(const flare::World& world,std::uint64_t cameraStableId)
{
    if(cameraStableId!=0)return FindByStableId(world,cameraStableId);
    flare::EntityHandle fallback{};
    for(auto candidate:world.Entities())
    {
        if(!world.IsEnabled(candidate))continue;
        const auto camComp=world.FindComponent(candidate,flare::ComponentTypes::Camera);
        if(camComp&&(!world.GetComponent(camComp)||!world.GetComponent(camComp)->Active()))continue;
        if(const auto* cam=world.GetCamera(candidate))
        {
            if(cam->primary)return candidate;
            if(!fallback)fallback=candidate;
        }
    }
    return fallback;
}
template<class T> bool CloneRegisteredFeature(const flare::World& source,flare::EntityHandle from,flare::World& target,flare::EntityHandle to,
    const std::unordered_map<flare::StableEntityId,flare::StableEntityId>& idMap)
{
    const auto comp=source.FindComponent(from,T::TypeId);
    if(!comp)return true;
    const auto* typed=source.GetRegisteredComponent<T>(comp);
    if(!typed)return true;
    T copy=*typed;
    for(auto& val:copy.values)
    {
        if(auto* ref=std::get_if<flare::PropertyEntityReference>(&val))
        {
            if(const auto it=idMap.find(ref->stableId);it!=idMap.end())
                ref->stableId=it->second;
        }
    }
    return static_cast<bool>(target.AttachRegisteredComponent(to,copy,typed->Enabled()));
}
}
int32_t FLARE_CALL flare_entity_actor_play(FLARE_Session handle,uint64_t entity_stable_id,const char* clip,uint32_t length,float speed,int32_t loop) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!clip||length==0||length>4096||!ValidUtf8(clip,length)||!std::isfinite(speed))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        // 固定步内不能改组件属性（World 队列未清空），改为给姿态系统一个下一步生效的请求。
        s->Env().Skeletal().RequestPlay(entity_stable_id,std::string(clip,length),speed,loop!=0);
        return FLARE_OK;});
}
int32_t FLARE_CALL flare_entity_joint_position(FLARE_Session handle,uint64_t entity_stable_id,const char* joint,uint32_t length,FLARE_Vector3* position,int32_t* found) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!joint||length==0||length>4096||!ValidUtf8(joint,length)||!position||!found)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        flare::Vec3 value{};
        *found=s->Env().Skeletal().QueryJointPosition(s->WorldRef(),entity_stable_id,std::string(joint,length),value)?1:0;
        if(*found)*position={value.x,value.y,value.z};
        return FLARE_OK;});
}
int32_t FLARE_CALL flare_entity_set_mesh(FLARE_Session handle,FLARE_Entity entity,const char* asset_id,uint32_t length,int32_t casts_shadow,int32_t receives_shadow) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!asset_id||length!=32||!ValidUtf8(asset_id,length))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        flare::MeshComponent mesh;mesh.meshAsset.assign(asset_id,length);
        mesh.castsShadow=casts_shadow!=0;mesh.receivesShadow=receives_shadow!=0;
        const auto target=flare::EntityHandle::FromPacked(entity);
        const auto* record=s->WorldRef().Get(target);
        if(!record&&!s->WorldRef().IsPending(target))return Error(FLARE_INVALID_HANDLE,"Bridge.InvalidEntity");
        return s->WorldRef().SetMesh(target,mesh)?FLARE_OK:Error(FLARE_INVALID_ARGUMENT,"Bridge.WorldOperationFailed");});
}
int32_t FLARE_CALL flare_entity_set_camera_fov(FLARE_Session handle,uint64_t entity_stable_id,float fov) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!std::isfinite(fov)||fov<0.05f||fov>3.0f)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto entity=FindByStableId(s->WorldRef(),entity_stable_id);
        if(!entity)return Error(FLARE_INVALID_HANDLE,"Bridge.InvalidEntity");
        const auto* existing=s->WorldRef().GetCamera(entity);
        if(!existing)return Error(FLARE_INVALID_ARGUMENT,"World.MissingComponent");
        auto camera=*existing;camera.verticalFovRadians=fov;
        // SetCamera 通过 World 命令队列，允许脚本在同一固定步内与 Transform 一起写入。
        return s->WorldRef().SetCamera(entity,camera)?FLARE_OK:Error(FLARE_INVALID_ARGUMENT,"World.InvalidComponent");});
}
int32_t FLARE_CALL flare_session_set_look(FLARE_Session handle,float mode,float blend) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!std::isfinite(mode)||!std::isfinite(blend))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        if(mode<0){s->appearance.lookMode=-1;s->appearance.lookBlend=0;return FLARE_OK;}
        if(mode>2||blend<0||blend>1)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        s->appearance.lookMode=mode;s->appearance.lookBlend=blend;return FLARE_OK;});
}
int32_t FLARE_CALL flare_camera_project_direction(FLARE_Session handle,uint64_t camera_stable_id,FLARE_Vector3 direction,FLARE_Vector2* screen_uv,int32_t* in_front) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!screen_uv||!in_front||!std::isfinite(direction.x)||!std::isfinite(direction.y)||!std::isfinite(direction.z))
            return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *in_front=0;
        const auto entity=FindPrimaryOrCamera(s->WorldRef(),camera_stable_id);
        const auto* camera=entity?s->WorldRef().GetCamera(entity):nullptr;
        if(!camera)return Error(FLARE_INVALID_HANDLE,"Bridge.InvalidEntity");
        const auto* rec=s->WorldRef().Get(entity);
        const flare::CameraInstance instance{rec?rec->stableId:camera_stable_id,s->WorldRef().GetWorldMatrix(entity),*camera,true};
        // 游戏窗口和编辑器 Play 都按实际游戏视口布局 UI。无窗口时保留已请求的尺寸或单位视口。
        auto size=s->UiRef().ViewportSize();
        if(size.x<=0||size.y<=0)
            size=s->windowWidth?flare::UiPoint{float(s->windowWidth),float(s->windowHeight)}:flare::UiPoint{1,1};
        flare::CameraSnapshot snapshot;flare::CameraError error{};
        if(!flare::BuildCameraSnapshot(instance,{0,0,size.x,size.y},0,snapshot,error))return Error(FLARE_INVALID_ARGUMENT,"Camera.InvalidTransform");
        const auto length=std::sqrt(direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);
        if(!std::isfinite(length)||length<1e-6f)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const flare::Vec3 unit{direction.x/length,direction.y/length,direction.z/length};
        const auto projected=flare::ProjectWorldPoint(snapshot,snapshot.position+unit*1000.0f);
        if(!projected)return FLARE_OK;
        screen_uv->x=projected->x/size.x;screen_uv->y=projected->y/size.y;
        *in_front=1;
        return FLARE_OK;});
}
int32_t FLARE_CALL flare_camera_screen_point_to_ray(FLARE_Session handle,uint64_t camera_stable_id,FLARE_Vector2 screen_pixel,FLARE_Vector3* origin,FLARE_Vector3* direction) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!origin||!direction||!std::isfinite(screen_pixel.x)||!std::isfinite(screen_pixel.y))
            return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto entity=FindPrimaryOrCamera(s->ReadWorld(),camera_stable_id);
        const auto* camera=entity?s->ReadWorld().GetCamera(entity):nullptr;
        if(!camera)return Error(FLARE_INVALID_HANDLE,"Bridge.InvalidEntity");
        const auto* rec=s->ReadWorld().Get(entity);
        const flare::CameraInstance instance{rec?rec->stableId:camera_stable_id,s->ReadWorld().GetWorldMatrix(entity),*camera,true};
        auto size=s->UiRef().ViewportSize();
        if(size.x<=0||size.y<=0)
            size=s->windowWidth?flare::UiPoint{float(s->windowWidth),float(s->windowHeight)}:flare::UiPoint{1280,720};
        flare::CameraSnapshot snapshot;flare::CameraError error{};
        if(!flare::BuildCameraSnapshot(instance,{0,0,size.x,size.y},0,snapshot,error))return Error(FLARE_INVALID_ARGUMENT,"Camera.InvalidTransform");
        const auto ray=flare::ViewportPointToRay(snapshot,{screen_pixel.x,screen_pixel.y},true);
        if(!ray)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        *origin={ray->origin.x,ray->origin.y,ray->origin.z};
        *direction={ray->direction.x,ray->direction.y,ray->direction.z};
        return FLARE_OK;});
}
int32_t FLARE_CALL flare_session_set_sky(FLARE_Session handle,const char* json,uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(length==0){s->appearance.sky.reset();return FLARE_OK;}
        if(!json||length>4096||!ValidUtf8(json,length))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        try
        {
            const auto document=nlohmann::json::parse(std::string(json,length));
            const auto vector=[&](const char* key,flare::Vec3& output)
            {
                if(!document.contains(key)||!document[key].is_array()||document[key].size()!=3)return false;
                const flare::Vec3 value{document[key][0].get<float>(),document[key][1].get<float>(),document[key][2].get<float>()};
                if(!flare::IsFinite(value)||value.x<0||value.y<0||value.z<0||value.x>8||value.y>8||value.z>8)return false;
                output=value;return true;
            };
            flare::SkyView view;view.enabled=true;
            if(!vector("zenith",view.zenith)||!vector("horizon",view.horizon)||!vector("ground",view.ground)||!vector("sunColor",view.sunColor))
                return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
            view.sunIntensity=document.value("sunIntensity",12.0f);
            view.sunAngularRadius=document.value("sunAngularRadius",0.012f);
            if(!std::isfinite(view.sunIntensity)||view.sunIntensity<0||view.sunIntensity>1000||
                !std::isfinite(view.sunAngularRadius)||view.sunAngularRadius<0.001f||view.sunAngularRadius>0.10f)
                return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
            s->appearance.sky=view;return FLARE_OK;
        }
        catch(const std::exception&){return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");}
    });
}
int32_t FLARE_CALL flare_entity_get_stable_id(FLARE_Session handle, FLARE_Entity entity, uint64_t* stableId) noexcept{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!stableId) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto* record = session->ReadWorld().GetKnown(flare::EntityHandle::FromPacked(entity));
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *stableId = record->stableId; return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_find(FLARE_Session handle, uint64_t stableId, FLARE_Entity* entity) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!entity) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto found = session->ReadWorld().FindKnownByStableId(stableId);
        if (!found) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *entity = found.Pack(); return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_find_by_name(FLARE_Session handle, const char* name_utf8, uint32_t length, FLARE_Entity* entity, int32_t* found) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!entity || !found || !name_utf8 || length > 4096 || !ValidUtf8(name_utf8, length))
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *entity = 0; *found = 0;
        const std::string_view target(name_utf8, length);
        const auto& world = session->ReadWorld();
        for (const auto candidate : world.Entities())
        {
            const auto* rec = world.Get(candidate);
            if (rec && rec->name == target)
            {
                *entity = candidate.Pack();
                *found = 1;
                return FLARE_OK;
            }
        }
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_get_name(FLARE_Session handle, FLARE_Entity entity, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        const auto* record = session->ReadWorld().GetKnown(flare::EntityHandle::FromPacked(entity));
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        return CopyText(record->name, buffer, capacity, required);
    });
}
int32_t FLARE_CALL flare_entity_find_by_tag(FLARE_Session handle, const char* tag_utf8, uint32_t length, FLARE_Entity* entity, int32_t* found) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!entity || !found || !tag_utf8 || length > 256 || !ValidUtf8(tag_utf8, length))
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *entity = 0; *found = 0;
        const std::string_view target(tag_utf8, length);
        const auto& world = session->ReadWorld();
        for (const auto candidate : world.Entities())
        {
            const auto* rec = world.Get(candidate);
            if (rec && world.IsEnabled(candidate) && rec->tag == target)
            {
                *entity = candidate.Pack();
                *found = 1;
                return FLARE_OK;
            }
        }
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_get_tag(FLARE_Session handle, FLARE_Entity entity, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        const auto* record = session->ReadWorld().GetKnown(flare::EntityHandle::FromPacked(entity));
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        return CopyText(record->tag, buffer, capacity, required);
    });
}
int32_t FLARE_CALL flare_entity_set_tag(FLARE_Session handle, FLARE_Entity entity, const char* tag_utf8, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {
        Session* s = Find(handle); if (!s) return MissingSession();
        if ((!tag_utf8 && length > 0) || length > 256 || (tag_utf8 && !ValidUtf8(tag_utf8, length)))
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        if (s->editor && !s->borrowedWorld) return Error(FLARE_BUSY, "Bridge.EditorReadOnly");
        return s->WorldRef().SetTag(flare::EntityHandle::FromPacked(entity), length > 0 ? std::string(tag_utf8, length) : std::string("Untagged"))
            ? FLARE_OK : Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
    });
}
int32_t FLARE_CALL flare_entity_get_layer(FLARE_Session handle, FLARE_Entity entity, uint32_t* layer) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!layer) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto* record = session->ReadWorld().GetKnown(flare::EntityHandle::FromPacked(entity));
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *layer = record->layer;
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_set_layer(FLARE_Session handle, FLARE_Entity entity, uint32_t layer) noexcept
{
    return Protect([&]() -> int32_t {
        Session* s = Find(handle); if (!s) return MissingSession();
        if (layer > 31u) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        if (s->editor && !s->borrowedWorld) return Error(FLARE_BUSY, "Bridge.EditorReadOnly");
        return s->WorldRef().SetLayer(flare::EntityHandle::FromPacked(entity), layer)
            ? FLARE_OK : Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
    });
}
int32_t FLARE_CALL flare_entity_get_world_transform(FLARE_Session handle, FLARE_Entity entity, FLARE_Transform* output) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!output) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto target = flare::EntityHandle::FromPacked(entity);
        const auto& world = session->ReadWorld();
        if (!world.GetKnown(target)) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        const flare::Mat4 m = world.GetKnownWorldMatrix(target);
        flare::Transform decomposed{};
        if (!flare::TryDecomposeTrs(m, decomposed))
        {
            const auto* local = world.GetKnownTransform(target);
            if (!local) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
            decomposed = *local;
            decomposed.position = {m(0, 3), m(1, 3), m(2, 3)};
        }
        *output = {decomposed.position.x, decomposed.position.y, decomposed.position.z,
                   decomposed.rotation.x, decomposed.rotation.y, decomposed.rotation.z, decomposed.rotation.w,
                   decomposed.scale.x, decomposed.scale.y, decomposed.scale.z};
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_get_parent(FLARE_Session handle, FLARE_Entity entity, FLARE_Entity* parent) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!parent) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        const auto* record = session->ReadWorld().GetKnown(flare::EntityHandle::FromPacked(entity));
        if (!record) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        *parent = record->parent ? record->parent.Pack() : 0;
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_set_parent(FLARE_Session handle, FLARE_Entity entity, FLARE_Entity parent, int32_t keep_world) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (session->editor && !session->borrowedWorld) return Error(FLARE_BUSY, "Bridge.EditorReadOnly");
        auto& world = session->WorldRef();
        const auto child = flare::EntityHandle::FromPacked(entity);
        const auto nextParent = parent ? flare::EntityHandle::FromPacked(parent) : flare::EntityHandle{};
        if (!world.GetKnown(child) || (parent && !world.GetKnown(nextParent)))
            return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");
        const flare::Mat4 childWorld = keep_world ? world.GetKnownWorldMatrix(child) : flare::Mat4{};
        if (!world.SetParent(child, nextParent))
            return Error(FLARE_INVALID_ARGUMENT, world.LastError().c_str());
        if (keep_world)
        {
            const auto invParent = nextParent ? flare::Inverse(world.GetKnownWorldMatrix(nextParent)) : std::optional<flare::Mat4>(flare::Mat4{});
            flare::Transform local{};
            if (invParent && flare::TryDecomposeTrs(*invParent * childWorld, local))
                (void)world.SetTransform(child, local);
        }
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_duplicate(FLARE_Session handle, FLARE_Entity sourceHandle, const FLARE_Vector3* override_pos,
    const float* override_rot_xyzw, uint64_t* out_source_stable_ids, FLARE_Entity* out_cloned_entities,
    uint32_t capacity, uint32_t* out_count) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!out_count) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        if (session->editor && !session->borrowedWorld) return Error(FLARE_BUSY, "Bridge.EditorReadOnly");
        auto& world = session->WorldRef();
        const auto root = flare::EntityHandle::FromPacked(sourceHandle);
        if (!world.GetKnown(root)) return Error(FLARE_INVALID_HANDLE, "Bridge.InvalidEntity");

        const auto all = world.Entities();
        std::vector<flare::EntityHandle> subtree;
        subtree.push_back(root);
        for (auto candidate : all)
        {
            if (candidate == root) continue;
            for (auto cursor = world.GetKnown(candidate) ? world.GetKnown(candidate)->parent : flare::EntityHandle{};
                 cursor;
                 cursor = world.GetKnown(cursor) ? world.GetKnown(cursor)->parent : flare::EntityHandle{})
            {
                if (cursor == root) { subtree.push_back(candidate); break; }
            }
        }
        *out_count = static_cast<uint32_t>(subtree.size());
        if (capacity < subtree.size() || !out_source_stable_ids || !out_cloned_entities)
            return FLARE_BUFFER_TOO_SMALL;

        std::unordered_map<std::uint64_t, flare::EntityHandle> handleMap;
        std::unordered_map<flare::StableEntityId, flare::StableEntityId> stableMap;
        for (std::size_t i = 0; i < subtree.size(); ++i)
        {
            const auto src = subtree[i];
            const auto* rec = world.GetKnown(src);
            const std::string cloneName = (src == root) ? (rec->name + " (Clone)") : rec->name;
            const auto dst = world.Create(cloneName);
            if (!dst) return Error(FLARE_INTERNAL_ERROR, "Bridge.WorldOperationFailed");
            (void)world.SetEnabled(dst, rec->enabled);
            if (rec->tag != "Untagged") (void)world.SetTag(dst, rec->tag);
            if (rec->layer != 0) (void)world.SetLayer(dst, rec->layer);
            if (rec->staticFlags != 0) (void)world.SetStaticFlags(dst, rec->staticFlags);
            handleMap.emplace(src.Pack(), dst);
            const auto* dstRec = world.GetKnown(dst);
            stableMap.emplace(rec->stableId, dstRec->stableId);
            out_source_stable_ids[i] = rec->stableId;
            out_cloned_entities[i] = dst.Pack();
        }
        for (auto src : subtree)
        {
            const auto dst = handleMap.at(src.Pack());
            const auto* rec = world.GetKnown(src);
            if (src != root && rec->parent)
            {
                if (const auto it = handleMap.find(rec->parent.Pack()); it != handleMap.end())
                    (void)world.SetParent(dst, it->second);
            }
            else if (src == root && rec->parent && !override_pos && !override_rot_xyzw)
            {
                (void)world.SetParent(dst, rec->parent);
            }
            if (const auto* t = world.GetKnownTransform(src))
            {
                flare::Transform nextT = *t;
                if (src == root && (override_pos || override_rot_xyzw) && rec->parent)
                {
                    flare::Transform worldT{};
                    if (flare::TryDecomposeTrs(world.GetKnownWorldMatrix(src), worldT))
                        nextT = worldT;
                }
                if (src == root && override_pos)
                    nextT.position = {override_pos->x, override_pos->y, override_pos->z};
                if (src == root && override_rot_xyzw)
                    nextT.rotation = flare::Normalize(flare::Quat{override_rot_xyzw[0], override_rot_xyzw[1], override_rot_xyzw[2], override_rot_xyzw[3]});
                (void)world.SetTransform(dst, nextT);
            }
            if (const auto* mesh = world.GetMesh(src))
            {
                (void)world.SetMesh(dst, *mesh);
                if (const auto mc = world.FindComponent(src, flare::ComponentTypes::Mesh); mc && world.GetComponent(mc) && !world.GetComponent(mc)->Enabled())
                    (void)world.SetComponentEnabled(world.FindComponent(dst, flare::ComponentTypes::Mesh), false);
            }
            if (const auto* light = world.GetLight(src))
            {
                (void)world.SetLight(dst, *light);
                if (const auto lc = world.FindComponent(src, flare::ComponentTypes::Light); lc && world.GetComponent(lc) && !world.GetComponent(lc)->Enabled())
                    (void)world.SetComponentEnabled(world.FindComponent(dst, flare::ComponentTypes::Light), false);
            }
            if (const auto* cam = world.GetCamera(src))
            {
                auto copyCam = *cam;
                copyCam.primary = false;
                (void)world.SetCamera(dst, copyCam);
                if (const auto cc = world.FindComponent(src, flare::ComponentTypes::Camera); cc && world.GetComponent(cc) && !world.GetComponent(cc)->Enabled())
                    (void)world.SetComponentEnabled(world.FindComponent(dst, flare::ComponentTypes::Camera), false);
            }
            (void)CloneRegisteredFeature<flare::PhysicsBodyComponent>(world, src, world, dst, stableMap);
            (void)CloneRegisteredFeature<flare::CharacterSettingsComponent>(world, src, world, dst, stableMap);
            (void)CloneRegisteredFeature<flare::ActorSettingsComponent>(world, src, world, dst, stableMap);
            (void)CloneRegisteredFeature<flare::RectangleLightComponent>(world, src, world, dst, stableMap);
            (void)CloneRegisteredFeature<flare::ColliderSettingsComponent>(world, src, world, dst, stableMap);
            (void)CloneRegisteredFeature<flare::AudioListenerComponent>(world, src, world, dst, stableMap);
            (void)CloneRegisteredFeature<flare::AudioSourceComponent>(world, src, world, dst, stableMap);
            (void)CloneRegisteredFeature<flare::ParticleEmitterComponent>(world, src, world, dst, stableMap);
            (void)CloneRegisteredFeature<flare::NavigationAgentComponent>(world, src, world, dst, stableMap);
        }
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_scene_export(FLARE_Session handle, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        std::string text, error;
        if (!flare::SerializeScene(session->ReadWorld(), text, error)) return Error(FLARE_INTERNAL_ERROR, "Bridge.SceneInvalid");
        return CopyText(text, buffer, capacity, required);
    });
}
int32_t FLARE_CALL flare_scene_import(FLARE_Session handle, const char* text, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (session->stepping) return Error(FLARE_BUSY, "Bridge.SessionBusy");
        if (!text || length == 0 || length > 64u*1024u*1024u || !ValidUtf8(text, length))
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidUtf8");
        std::string error;
        if (!flare::DeserializeScene(std::string_view(text,length), session->WorldRef(), error)) return Error(FLARE_INVALID_ARGUMENT, "Bridge.SceneInvalid");
        CloseEventPublishers(handle);session->eventSubscriptions.clear();
        session->Env().Events().ResetWorld(session->WorldRef());
        session->Env().Scenes().ResetWorld(session->WorldRef());
        auto& events=EventsFor(*session);
        const std::uint8_t state[8]={1,0,0,0,2,0,0,0};
        events.GetPublisher().Post(4,1,state,flare::EventPhase::NextFrame);
        session->scenePhysics.Clear(session->physics);
        for (auto body:session->physics.Bodies()) session->physics.DestroyBody(body);
        session->physics.ResetTriggers();
        session->characterTriggerTracker.Clear();
        session->bodyOwners.clear();
        session->contactEvents.clear(); session->triggerEvents.clear(); session->physicsClock.Reset();
        session->characters.clear();
        session->sceneAudio.Clear(session->audio);
        session->audio.StopAll();
        session->audio.SetPaused(false);
        session->sceneParticles.Clear();
        session->sceneNavigation.Clear();
        return FLARE_OK;
    });
}

int32_t FLARE_CALL flare_scene_set_callback(FLARE_Session handle,FLARE_SceneCallback callback,void* context)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(FLARE_BUSY,"Bridge.SessionBusy");s->sceneCallback=callback;s->sceneContext=callback?context:nullptr;return FLARE_OK;});
}
int32_t FLARE_CALL flare_scene_create(FLARE_Session handle,const char* name,uint32_t length,uint64_t* scene)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!scene||!name||!length||length>256||!ValidUtf8(name,length))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        auto& scenes=ScenesFor(*s);*scene=scenes.Create(std::string(name,length)).value;return *scene?FLARE_OK:Error(FLARE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t FLARE_CALL flare_scene_request_file(FLARE_Session handle,uint64_t scene,const char* path,uint32_t length,uint32_t mode,uint64_t replacing,uint64_t* request)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!request||!path||!length||length>32768||mode>1||!ValidUtf8(path,length))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        auto& scenes=ScenesFor(*s);*request=scenes.RequestFile({scene},std::filesystem::path(std::u8string(path,path+length)),static_cast<flare::SceneLoadMode>(mode),{replacing});return *request?FLARE_OK:Error(FLARE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t FLARE_CALL flare_scene_request_json(FLARE_Session handle,uint64_t scene,const char* text,uint32_t length,uint32_t mode,uint64_t replacing,uint64_t* request)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!request||!text||!length||length>16*1024*1024||mode>1||!ValidUtf8(text,length))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        flare::SceneDocument document;std::string error;if(!flare::ParseSceneDocument({text,length},document,error))return Error(FLARE_INVALID_ARGUMENT,"Bridge.SceneInvalid");
        auto& scenes=ScenesFor(*s);*request=scenes.RequestDocument({scene},std::move(document),static_cast<flare::SceneLoadMode>(mode),{replacing});return *request?FLARE_OK:Error(FLARE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t FLARE_CALL flare_scene_cancel(FLARE_Session handle,uint64_t scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();auto& scenes=ScenesFor(*s);return scenes.Cancel({scene})?FLARE_OK:Error(FLARE_INVALID_ARGUMENT,scenes.LastError().c_str());});}
int32_t FLARE_CALL flare_scene_unload(FLARE_Session handle,uint64_t scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();auto& scenes=ScenesFor(*s);return scenes.Unload({scene})?FLARE_OK:Error(FLARE_INVALID_ARGUMENT,scenes.LastError().c_str());});}
int32_t FLARE_CALL flare_scene_active(FLARE_Session handle,uint64_t* scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!scene)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*scene=ScenesFor(*s).ActiveScene().value;return FLARE_OK;});}
int32_t FLARE_CALL flare_scene_of_entity(FLARE_Session handle,FLARE_Entity entity,uint64_t* scene)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!scene)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*scene=ScenesFor(*s).SceneOf(flare::EntityHandle::FromPacked(entity)).value;return FLARE_OK;});}
int32_t FLARE_CALL flare_scene_create_entity(FLARE_Session handle,uint64_t scene,const char* name,uint32_t length,FLARE_Entity* entity)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!entity||!name||length>4096||!ValidUtf8(name,length))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        auto& scenes=ScenesFor(*s);const auto created=scenes.CreateEntity({scene},std::string(name,length));*entity=created?created.Pack():0;return created?FLARE_OK:Error(FLARE_INVALID_ARGUMENT,scenes.LastError().c_str());});
}
int32_t FLARE_CALL flare_scene_get(FLARE_Session handle,uint64_t scene,char* buffer,uint32_t capacity,uint32_t* required)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();const auto value=ScenesFor(*s).Inspect({scene});if(!value.handle)return Error(FLARE_INVALID_HANDLE,"Scene.InvalidRequest");
        auto members=nlohmann::json::array();for(const auto entity:value.members)members.push_back(entity.Pack());
        const nlohmann::json json{{"Id",value.handle.value},{"State",static_cast<uint32_t>(value.state)},{"Active",value.active},{"Request",value.request},{"Revision",value.revision},{"Scope",value.scope},{"Name",value.name},{"Source",value.source},{"RequestedSource",value.requestedSource},{"Error",value.error},{"Members",std::move(members)}};
        return CopyText(json.dump(),buffer,capacity,required);});
}
int32_t FLARE_CALL flare_scene_stats(FLARE_Session handle,char* buffer,uint32_t capacity,uint32_t* required)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();const auto v=ScenesFor(*s).Statistics();
        return CopyText(nlohmann::json{{"Requested",v.requested},{"Activated",v.activated},{"Unloaded",v.unloaded},{"Failed",v.failed},{"Cancelled",v.cancelled},{"StaleResults",v.staleResults},{"CallbackFaults",v.callbackFaults},{"NotificationFailures",v.notificationFailures},{"PendingJobs",v.pendingJobs},{"Records",v.records},{"Members",v.members}}.dump(),buffer,capacity,required);});
}
int32_t FLARE_CALL flare_scene_callback_scope(FLARE_Session handle,uint64_t scene,uint64_t* previous)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!previous)return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");auto& scenes=ScenesFor(*s);flare::SceneHandle prior;
        if(!scenes.ExchangeCreationScope({scene},prior))return Error(FLARE_INVALID_ARGUMENT,scenes.LastError().c_str());*previous=prior.value;return FLARE_OK;});
}
