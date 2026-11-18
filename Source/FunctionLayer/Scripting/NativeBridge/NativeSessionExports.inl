int32_t SE_CALL se_get_abi_info(SE_AbiInfo* info) noexcept
{
    if (!info) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
    *info = {1, sizeof(SE_Transform), sizeof(void*), 0};
    return SE_OK;
}
int32_t SE_CALL se_get_last_error(char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return CopyText(lastError.data(), buffer, capacity, required);
}
int32_t SE_CALL se_session_create(SE_Session* output) noexcept
{
    InitializeProcessDiagnostics("managed-native");
    LogCheckpoint("Native.Session.Create.Begin");
    return Protect([&]() -> int32_t {
        if (!output) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *output = 0;
        auto session = std::make_unique<Session>();
        size_t index = 0;
        for (; index < slots.size(); ++index)
            if (!slots[index].value && slots[index].generation != UINT32_MAX) break;
        if (index == slots.size()) slots.emplace_back();
        const auto created=(static_cast<uint64_t>(slots[index].generation)<<32)|(index+1);
        SceneServices(*session,created);slots[index].value=std::move(session);*output=created;
        LogCheckpoint("Native.Session.Create.End");
        return SE_OK;
    });
}
int32_t SE_CALL se_session_close(SE_Session handle) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle, false);
        if (!session) return SE_INVALID_HANDLE;
        if (session->stepping || session->windowRunning) return Error(SE_BUSY, "Bridge.SessionBusy");
        Slot& slot = slots[static_cast<uint32_t>(handle) - 1];
        LogCheckpoint("Native.Session.Close.Begin");
        session->Env().BeginShutdown();
        CloseEventPublishers(handle);
        slot.value.reset();
        if (slot.generation != UINT32_MAX) ++slot.generation;
        LogCheckpoint("Native.Session.Close.End");
        return SE_OK;
    });
}
int32_t SE_CALL se_session_set_update(SE_Session handle, SE_UpdateCallback callback, void* context) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle);
        if (!session) return MissingSession();
        if (session->stepping) return Error(SE_BUSY, "Bridge.SessionBusy");
        session->callback = callback;
        session->context = callback ? context : nullptr;
        return SE_OK;
    });
}
int32_t SE_CALL se_session_step(SE_Session handle, double dt) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle);
        if (!session) return MissingSession();
        if (!std::isfinite(dt) || dt < 0.0 || dt > 1.0) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidDelta");
        return AdvanceSession(*session,handle,dt,false);
    });
}
int32_t SE_CALL se_environment_set_callback(SE_Session handle,SE_TickCallback callback,void* context)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");s->tickCallback=callback;s->tickContext=callback?context:nullptr;return SE_OK;});
}
int32_t SE_CALL se_environment_configure(SE_Session handle,double seconds,uint32_t maximum,int32_t automatic,int32_t physics)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!std::isfinite(seconds)||seconds<=0||seconds>1||!maximum||(automatic!=0&&automatic!=1)||(physics!=0&&physics!=1))return Error(SE_INVALID_ARGUMENT,"Clock.InvalidSettings");
        if(!s->Env().ConfigureClock(seconds,maximum,automatic!=0))return Error(SE_INVALID_ARGUMENT,s->Env().LastError().c_str());
        s->environmentDriven=true;s->environmentPhysics=physics!=0;s->pendingEnvironmentRelease=false;s->Env().SetPaused(false);s->physics.SetEnvironmentDriven(true);s->physicsClock.ClearRemainder();return SE_OK;
    });
}
int32_t SE_CALL se_environment_set_paused(SE_Session handle,int32_t paused)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(paused!=0&&paused!=1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");s->Env().SetPaused(paused!=0);s->audio.SetPaused(paused!=0);return SE_OK;});
}
int32_t SE_CALL se_environment_release(SE_Session handle)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();s->pendingEnvironmentRelease=true;if(!s->stepping)FinishEnvironmentRelease(*s);return SE_OK;});
}
int32_t SE_CALL se_environment_stats(SE_Session handle,uint64_t* steps,double* simulation,double* dropped)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!steps||!simulation||!dropped)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*steps=s->Env().FixedStepIndex();*simulation=s->Env().SimulationSeconds();*dropped=s->Env().Clock().DroppedSeconds();return SE_OK;});
}
int32_t SE_CALL se_tod_snapshot(SE_Session handle,char* buffer,uint32_t capacity,uint32_t* required)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();const auto& environment=s->ReadEnvironment();
        const auto v=environment.Snapshot();const auto vector=[](se::Vec3 a){return nlohmann::json{{"X",a.x},{"Y",a.y},{"Z",a.z}};};
        const nlohmann::json data={{"Active",bool(environment.TimeOfDay().Profile())},{"FrameIndex",v.frameIndex},{"Day",v.day},{"Time",v.time},
            {"SimulationSeconds",v.simulationSeconds},{"Rate",environment.TimeOfDay().State().rate},{"Daylight",v.daylight},
            {"SunDirection",vector(v.sunDirection)},{"SunColor",vector(v.parameters.sunColor)},{"SunIntensity",v.parameters.sunIntensity},
            {"SkyColor",vector(v.parameters.skyColor)},{"FogColor",vector(v.parameters.fogColor)},{"FogDensity",v.parameters.fogDensity}};
        return CopyText(data.dump(),buffer,capacity,required);
    });
}
int32_t SE_CALL se_tod_set_time(SE_Session handle,int64_t day,double time,int32_t replay)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();
        if(s->editor&&!s->borrowedEnvironment)return Error(SE_BUSY,"Bridge.EditorReadOnly");
        if(replay!=0&&replay!=1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        return s->Env().TimeOfDay().SetTime(day,time,replay!=0)?SE_OK:Error(SE_INVALID_ARGUMENT,s->Env().TimeOfDay().LastError().c_str());});
}
int32_t SE_CALL se_tod_set_rate(SE_Session handle,double rate)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();
        if(s->editor&&!s->borrowedEnvironment)return Error(SE_BUSY,"Bridge.EditorReadOnly");
        return s->Env().TimeOfDay().SetRate(rate)?SE_OK:Error(SE_INVALID_ARGUMENT,s->Env().TimeOfDay().LastError().c_str());});
}
int32_t SE_CALL se_environment_fixed_step(SE_Session handle,double seconds)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!s->environmentDriven||!std::isfinite(seconds)||seconds<=0||seconds>1)return Error(SE_INVALID_ARGUMENT,"Tick.ManualFixedRejected");
        struct Guard{Session& s;~Guard(){s.stepping=false;}} guard{*s};s->stepping=true;s->callbackFailed=false;
        const bool paused=s->Env().Paused()||s->Env().FramePaused();
        const bool ok=s->Env().RunFixed(seconds,EnvironmentHooks(*s,handle));SweepBodies(*s);if(ok){s->sceneAudio.Synchronize(s->WorldRef(),s->assets.get(),s->audio,paused);s->sceneParticles.Synchronize(s->WorldRef(),paused?0.0:seconds,paused);}FinishEnvironmentRelease(*s);
        if(!ok)return s->callbackFailed?Error(SE_CALLBACK_FAILED,"Bridge.CallbackFailed"):Error(SE_INVALID_ARGUMENT,s->Env().LastError().c_str());return SE_OK;
    });
}
int32_t SE_CALL se_event_set_callback(SE_Session handle,SE_EventCallback callback,void* context)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");s->eventCallback=callback;s->eventContext=callback?context:nullptr;return SE_OK;});
}
int32_t SE_CALL se_event_register_type(SE_Session handle,uint64_t type,uint32_t version,const char* key,uint32_t length,uint32_t maximum)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!key||!length||length>256||!ValidUtf8(key,length))return Error(SE_INVALID_ARGUMENT,"Event.InvalidType");
        auto& events=EventsFor(*s);return events.RegisterType({type,std::string(key,length),version,maximum})?SE_OK:Error(SE_INVALID_ARGUMENT,events.LastError().c_str());});
}
int32_t SE_CALL se_event_publisher_create(SE_Session handle,uint64_t* publisher)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!publisher)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*publisher=0;
        auto& events=EventsFor(*s);auto value=events.CreatePublisher();const auto id=value.ChannelId();if(!id)return Error(SE_INVALID_ARGUMENT,events.LastError().c_str());
        eventPublishers.emplace(id,EventPublisher{handle,std::move(value)});*publisher=id;return SE_OK;});
}
int32_t SE_CALL se_event_publisher_close(uint64_t publisher)noexcept
{
    return Protect([&]()->int32_t{auto it=eventPublishers.find(publisher);if(it!=eventPublishers.end()){it->second.value.Close();eventPublishers.erase(it);}return SE_OK;});
}
int32_t SE_CALL se_event_publish(uint64_t publisher,uint64_t type,uint32_t version,uint32_t phase,const uint8_t* bytes,uint32_t length,SE_Entity source,uint64_t stable,SE_Entity target,uint64_t scene,uint32_t* result)noexcept
{
    return Protect([&]()->int32_t{
        if(!result||(!bytes&&length)||length>16*1024*1024||phase>2||(source&&!se::EntityHandle::FromPacked(source))||(target&&!se::EntityHandle::FromPacked(target)))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto it=eventPublishers.find(publisher);*result=static_cast<uint32_t>(it==eventPublishers.end()?se::EventPostResult::Closed:
            it->second.value.Post(type,version,{bytes,length},static_cast<se::EventPhase>(phase),se::EntityHandle::FromPacked(source),stable,se::EntityHandle::FromPacked(target),scene));return SE_OK;
    });
}
int32_t SE_CALL se_event_subscribe(SE_Session handle,uint64_t publisher,uint64_t component,uint64_t type,SE_Entity target,uint64_t scene,uint64_t* subscription)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();if(!subscription||!s->eventCallback||(target&&!se::EntityHandle::FromPacked(target)))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*subscription=0;
        auto found=eventPublishers.find(publisher);if(found==eventPublishers.end()||found->second.session!=handle)return Error(SE_INVALID_ARGUMENT,"Event.ContextMismatch");
        auto& events=EventsFor(*s);auto token=events.Subscribe(se::ComponentHandle::FromPacked(component),type,[s,handle](const se::EventMessage& message,uint64_t token){
            if(!s->eventCallback)return false;const auto& e=message.envelope;
            const SE_EventHeader header{e.world,e.channel,e.type,e.source?e.source.Pack():0,e.sourceStableId,e.target?e.target.Pack():0,e.scene,e.frame,e.fixedStep,e.sequence,e.version,static_cast<uint32_t>(e.phase)};
            int32_t result=2;
            {struct Unlock{Unlock(){sessionsMutex.unlock();}~Unlock(){sessionsMutex.lock();}} unlock;
                result=s->eventCallback(handle,token,&header,message.payload->data(),static_cast<uint32_t>(message.payload->size()),s->eventContext);}
            if(result==3)throw se::EventPayloadRejected{};if(result!=0&&result!=2)throw std::runtime_error("Event.CallbackFailed");return result==0;
        },&found->second.value,se::EntityHandle::FromPacked(target),scene);
        const auto id=token.Id();if(!id)return Error(SE_INVALID_ARGUMENT,events.LastError().c_str());
        s->eventSubscriptions.emplace(id,std::move(token));*subscription=id;return SE_OK;
    });
}
int32_t SE_CALL se_event_unsubscribe(SE_Session handle,uint64_t subscription)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();s->eventSubscriptions.erase(subscription);s->Env().Events().Unsubscribe(subscription);return SE_OK;});
}
int32_t SE_CALL se_event_stats(SE_Session handle,SE_EventStats* stats)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!stats)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");const auto v=EventsFor(*s).Statistics();
        *stats={v.accepted,v.delivered,v.rejected,v.droppedTargets,v.noReceiver,v.callbackFailures,v.invalidPayload,v.mailbox.pending,v.mailbox.queued,v.mailbox.bytes,v.mailbox.peakPending,v.mailbox.peakBytes,v.mailbox.closed?1u:0u,0};return SE_OK;});
}
int32_t SE_CALL se_event_trace(SE_Session handle,char* buffer,uint32_t capacity,uint32_t* required)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();auto rows=nlohmann::json::array();for(const auto& row:EventsFor(*s).Trace()){
        const auto& e=row.envelope;rows.push_back({{"kind",static_cast<uint32_t>(row.kind)},{"world",e.world},{"channel",e.channel},{"type",e.type},{"version",e.version},{"source",e.source.Pack()},{"sourceStableId",e.sourceStableId},{"target",e.target.Pack()},{"scene",e.scene},{"frame",e.frame},{"fixedStep",e.fixedStep},{"phase",static_cast<uint32_t>(e.phase)},{"sequence",e.sequence},{"subscriber",row.subscriber},{"queued",row.queued},{"deliveryFrame",row.deliveryFrame},{"deliveryStep",row.deliveryStep}});}
        return CopyText(rows.dump(),buffer,capacity,required);});
}
int32_t SE_CALL se_component_tick_register(SE_Session handle,uint64_t component,uint32_t stage,int32_t simulation,int32_t paused,uint64_t* task)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();if(!task||stage>5||(simulation!=0&&simulation!=1)||(paused!=0&&paused!=1)||!s->tickCallback)return Error(SE_INVALID_ARGUMENT,"Tick.InvalidDescriptor");
        se::TickDescriptor descriptor;descriptor.key="bridge."+std::to_string(component)+'.'+std::to_string(stage);descriptor.component=se::ComponentHandle::FromPacked(component);
        descriptor.stage=static_cast<se::TickStage>(stage);descriptor.time=simulation?se::TickTimeDomain::Simulation:se::TickTimeDomain::Frame;descriptor.runWhenPaused=paused!=0;
        descriptor.callback=[s,handle,component,stage](const se::TickContext& context){if(EnvironmentCallback(*s,handle,component,stage,context))throw std::runtime_error("Tick.CallbackFailed");};
        *task=s->Env().Ticks().Register(s->WorldRef(),std::move(descriptor));return *task?SE_OK:Error(SE_INVALID_ARGUMENT,s->Env().Ticks().LastError().c_str());
    });
}
int32_t SE_CALL se_component_tick_remove(SE_Session handle,uint64_t task)noexcept
{
    return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();return s->Env().Ticks().Unregister(task)?SE_OK:Error(SE_INVALID_HANDLE,"Tick.InvalidTask");});
}
int32_t SE_CALL se_component_tick_prerequisites(SE_Session handle,uint64_t task,const SE_TickPrerequisite* values,uint32_t count)noexcept
{
    return Protect([&]()->int32_t{
        auto* s=Find(handle);if(!s)return MissingSession();if(count>256||(count&&!values))return Error(SE_INVALID_ARGUMENT,"Tick.InvalidPrerequisite");
        std::vector<se::TickPrerequisite> prerequisites;
        for(uint32_t i=0;i<count;++i){if(values[i].reserved||values[i].mode>1)return Error(SE_INVALID_ARGUMENT,"Tick.InvalidPrerequisite");prerequisites.push_back({values[i].task,static_cast<se::TickDependencyMode>(values[i].mode)});}
        return s->Env().Ticks().SetPrerequisites(task,std::move(prerequisites))?SE_OK:Error(SE_INVALID_ARGUMENT,s->Env().Ticks().LastError().c_str());
    });
}
int32_t SE_CALL se_entity_is_pending(SE_Session handle,SE_Entity entity,int32_t* pending)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!pending)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*pending=s->WorldRef().IsPending(se::EntityHandle::FromPacked(entity))?1:0;return SE_OK;});}
int32_t SE_CALL se_script_component_pending(SE_Session handle,uint64_t component,int32_t* pending)noexcept
{return Protect([&]()->int32_t{auto* s=Find(handle);if(!s)return MissingSession();if(!pending)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*pending=s->WorldRef().IsComponentPending(se::ComponentHandle::FromPacked(component),se::ComponentTypes::Script)?1:0;return SE_OK;});}
int32_t SE_CALL se_session_get_value(SE_Session handle, double* value) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!value) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *value = session->value; return SE_OK;
    });
}
int32_t SE_CALL se_session_set_value(SE_Session handle, double value) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!std::isfinite(value)) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        session->value = value; return SE_OK;
    });
}
int32_t SE_CALL se_session_get_step_count(SE_Session handle, uint64_t* count) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!count) return Error(SE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        *count = session->stepCount; return SE_OK;
    });
}
int32_t SE_CALL se_session_get_input(SE_Session handle, SE_InputState* input) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!input)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*input=s->input;return SE_OK;});
}
int32_t SE_CALL se_session_get_pointer(SE_Session handle, SE_PointerState* input) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!input)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");*input=s->pointer;return SE_OK;});
}
int32_t SE_CALL se_session_request_stop(SE_Session handle) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();s->stopWindow=true;return SE_OK;});
}
int32_t SE_CALL se_session_set_locale(SE_Session handle, const char* locale, uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!locale||length!=5||!ValidUtf8(locale,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const std::string language(locale,length);
        if(language!="zh-CN"&&language!="en-US")return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto directory=ResourceDirectory();std::string error;
        if(directory.empty()||!s->localization.Load(directory/"Localization"/(language+".txt"),error))return Error(SE_INTERNAL_ERROR,"Bridge.LocalizationFailed");
        return SE_OK;});
}
int32_t SE_CALL se_session_text(SE_Session handle, const char* key, uint32_t length, char* buffer, uint32_t capacity, uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!key||length>4096||!ValidUtf8(key,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        return CopyText(s->localization.GetText(std::string_view(key,length)),buffer,capacity,required);});
}
int32_t SE_CALL se_session_configure_quality(SE_Session handle,const char* path,uint32_t length,int32_t showMenu) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!path||!length||length>32768||!ValidUtf8(path,length)||std::memchr(path,0,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto file=std::filesystem::path(std::u8string(path,path+length));if(!file.is_absolute())return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        s->playerQuality=std::make_unique<se::PlayerQualityPreference>(file);s->showPlayerQualityMenu=showMenu!=0;return SE_OK;});
}
int32_t SE_CALL se_session_player_quality(SE_Session handle,int32_t requested,int32_t* current) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!current||requested< -1||requested>3)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        if(!s->playerQuality)return Error(SE_INVALID_ARGUMENT,"PlayerQuality.NotConfigured");
        if(requested>=0&&!s->playerQuality->Select(static_cast<se::PlayerQuality>(requested)))return Error(SE_INTERNAL_ERROR,"PlayerQuality.SaveFailed");
        *current=static_cast<int32_t>(s->playerQuality->Level());return SE_OK;});
}
int32_t SE_CALL se_session_set_quality_profiles(SE_Session handle,const char* json,uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!json||!length||length>16384||!ValidUtf8(json,length))return Error(SE_INVALID_ARGUMENT,"PlayerQuality.InvalidProfiles");
        const auto value=nlohmann::json::parse(json,json+length,nullptr,false);
        if(!se::ReadQualityProfiles(value,s->qualityProfiles))return Error(SE_INVALID_ARGUMENT,"PlayerQuality.InvalidProfiles");
        return SE_OK;});
}
int32_t SE_CALL se_session_set_window_capture(SE_Session handle,const char* path,uint32_t length,uint64_t frame) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(length==0&&frame==0){s->windowCapture.clear();s->windowCaptureFrame=0;return SE_OK;}
        if(!path||!length||length>32768||!frame||!ValidUtf8(path,length)||std::memchr(path,0,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        std::u8string utf8(length,u8'\0');std::memcpy(utf8.data(),path,length);s->windowCapture=std::filesystem::path(utf8);s->windowCaptureFrame=frame;return SE_OK;});
}
int32_t SE_CALL se_session_set_window_fixed_step(SE_Session handle,double seconds) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!std::isfinite(seconds)||seconds<0||seconds>0.1)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        s->windowFixedStep=seconds;return SE_OK;});
}
int32_t SE_CALL se_session_set_window_resolution(SE_Session handle,uint32_t width,uint32_t height) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(width==0&&height==0){s->windowWidth=s->windowHeight=0;return SE_OK;}
        if(width<64||height<64||width>16384||height>16384)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        s->windowWidth=width;s->windowHeight=height;return SE_OK;});
}
int32_t SE_CALL se_session_set_capture_sequence(SE_Session handle,const char* path,uint32_t length,uint64_t count) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(length==0||count==0){s->captureSequence.clear();s->captureSequenceCount=0;return SE_OK;}
        if(!path||length>32768||count>100000||!ValidUtf8(path,length)||std::memchr(path,0,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        std::u8string utf8(length,u8'\0');std::memcpy(utf8.data(),path,length);
        s->captureSequence=std::filesystem::path(utf8);s->captureSequenceCount=count;return SE_OK;});
}
int32_t SE_CALL se_session_set_window_profile(SE_Session handle,const char* path,uint32_t length,uint32_t width,uint32_t height) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(length==0&&width==0&&height==0){s->windowProfile.clear();s->profileWidth=s->profileHeight=0;return SE_OK;}
        if(!path||!length||length>32768||width<64||height<64||width>8192||height>8192||!ValidUtf8(path,length)||std::memchr(path,0,length))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        std::u8string utf8(length,u8'\0');std::memcpy(utf8.data(),path,length);auto candidate=std::filesystem::absolute(std::filesystem::path(utf8));
        if(std::filesystem::exists(candidate))return Error(SE_INVALID_ARGUMENT,"Bridge.ProfileOutputExists");
        s->windowProfile=std::move(candidate);s->profileWidth=width;s->profileHeight=height;return SE_OK;});
}
int32_t SE_CALL se_session_set_asset_root(SE_Session handle,const char* root,uint32_t rootLength,const char* cache,uint32_t cacheLength) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->windowRunning||s->stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
        if(!root||!cache||rootLength==0||cacheLength==0||rootLength>32768||cacheLength>32768||!ValidUtf8(root,rootLength)||!ValidUtf8(cache,cacheLength))return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        std::u8string rootUtf8(rootLength,u8'\0'),cacheUtf8(cacheLength,u8'\0');
        std::memcpy(rootUtf8.data(),root,rootLength);std::memcpy(cacheUtf8.data(),cache,cacheLength);
        const std::filesystem::path rootPath(rootUtf8);
        auto candidate=std::make_unique<se::AssetStore>(rootPath,std::filesystem::path(cacheUtf8));
        std::vector<se::AssetDiagnostic> diagnostics;
        if(!candidate->Scan(diagnostics))return Error(SE_INVALID_ARGUMENT,"Bridge.AssetScanFailed");
        se::InputActionDocument inputDoc=se::DefaultInputActionDocument();
        const auto inputSettingsPath=rootPath/std::filesystem::path(se::InputActionsRelativePath);
        std::error_code inputEc;
        if(std::filesystem::exists(inputSettingsPath,inputEc))
        {
            std::string inputError;
            if(!se::ReadInputActionDocument(inputSettingsPath,inputDoc,inputError))
                return Error(SE_INVALID_ARGUMENT,inputError.empty()?"Input.InvalidDocument":inputError.c_str());
        }
        s->inputActions.SetDocument(std::move(inputDoc));
        s->assets=std::move(candidate);return SE_OK;});
}
int32_t SE_CALL se_assets_list(SE_Session handle,char* output,uint32_t capacity,uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->assets)return Error(SE_INVALID_ARGUMENT,"Bridge.AssetsNotConfigured");
        return CopyText(nlohmann::json(s->assets->ListIds()).dump(),output,capacity,required);});
}
int32_t SE_CALL se_assets_load_model_info(SE_Session handle,const char* id,uint32_t length,SE_ModelInfo* info) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->assets)return Error(SE_INVALID_ARGUMENT,"Bridge.AssetsNotConfigured");
        if(!id||length!=32||!ValidUtf8(id,length)||!info)return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        std::vector<se::AssetDiagnostic> diagnostics;const auto model=s->assets->LoadModel(std::string(id,length),diagnostics);
        if(!model)return Error(SE_INVALID_ARGUMENT,"Bridge.AssetLoadFailed");
        *info={static_cast<uint32_t>(model->meshes.size()),0,0,0};
        for(const auto& mesh:model->meshes){info->vertices+=static_cast<uint32_t>(mesh.vertices.size());info->indices+=static_cast<uint32_t>(mesh.indices.size());}
        return SE_OK;});
}
