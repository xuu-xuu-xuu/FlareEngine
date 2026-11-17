#define SELFENGINE_NATIVE_EXPORTS
#include "FunctionLayer/Scripting/NativeBridge/EngineNative.h"
#include "FunctionLayer/Runtime/World.h"
#include "FunctionLayer/Runtime/Environment.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"
#include "FunctionLayer/Physics/ScenePhysics.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include <nlohmann/json.hpp>
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "WindowProfile.h"
#include "PlayerQualityMenu.h"
#include "NativeLogicObjects.h"
#include "PlatformLayer/InputState.h"
#include "FunctionLayer/Input/InputActionMap.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "PlatformLayer/Windows/ProcessDiagnostics.h"
#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorRenderer.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Editor/EditorWorkspaceHost.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
thread_local const char* lastError = "Bridge.Success";
std::recursive_mutex sessionsMutex;
struct RuntimeAppearance
{
    float lookMode=-1,lookBlend=0;
    std::optional<se::SkyView> sky;

    void Apply(se::FrameData& frame) const
    {
        if(lookMode>=0){frame.lookMode=lookMode;frame.lookBlend=lookBlend;}
        if(sky)frame.sky=*sky;
    }
};
struct Session
{
    Session(){environment.ConfigureClock(1.0/60,8,false);environment.Events().RegisterType({4,"engine.scene_state",1,8});}
    ~Session(){environment.BeginShutdown();world.Clear();while(world.HasPendingCommands())world.FlushCommands();}
    se::Environment environment;
    NativeLogicObjects logicObjects;
    se::World& world=environment.GetWorld();
    se::Environment* borrowedEnvironment=nullptr;
    se::Environment& Env(){return borrowedEnvironment?*borrowedEnvironment:environment;}
    const se::Environment& ReadEnvironment()const{return borrowedEnvironment?*borrowedEnvironment:editor?editor->EditEnvironment():environment;}
    se::World* borrowedWorld=nullptr;
    se::World& WorldRef() { return borrowedWorld?*borrowedWorld:world; }
    const se::World& ReadWorld() const {return borrowedWorld?*borrowedWorld:editor?editor->EditWorld():world;}
    se::PhysicsWorld physics;
    se::ScenePhysics scenePhysics;
    std::unordered_map<std::uint64_t,se::EntityHandle> bodyOwners;
    bool physicsDebugDraw=true;
    se::FixedStepClock physicsClock;
    std::vector<SE_ContactEvent> contactEvents;
    se::UiTree ui;
    se::UiTree* borrowedUi=nullptr;
    se::UiTree& UiRef() { return borrowedUi?*borrowedUi:ui; }
    std::vector<se::UiEvent> uiEvents;
    SE_InputState input{};
    SE_PointerState pointer{};
    se::InputActionMap inputActions{se::DefaultInputActionDocument()};
    std::unordered_map<SE_Entity,se::CharacterController> characters;
    Localization localization;
    std::unique_ptr<se::AssetStore> assets;
    bool windowRunning=false,stopWindow=false;
    std::unique_ptr<se::PlayerQualityPreference> playerQuality;
    bool showPlayerQualityMenu=true;
    se::QualityProfiles qualityProfiles=se::DefaultQualityProfiles;
    std::filesystem::path windowCapture;
    std::uint64_t windowCaptureFrame=0;
    // 普通 Player 的固定回放尺寸与固定步长；序列抓帧用同一组值保证 updateIndex 与 tick 一一对应。
    std::uint32_t windowWidth=0,windowHeight=0;
    double windowFixedStep=0;
    std::filesystem::path captureSequence;
    std::uint64_t captureSequenceCount=0;
    RuntimeAppearance appearance;
    std::filesystem::path windowProfile;
    std::uint32_t profileWidth=0,profileHeight=0;
    se::EditorSession* editor=nullptr;
    int32_t pendingPlay=-1;
    SE_EditorCallback editorCallback=nullptr;
    void* editorContext=nullptr;
    SE_EditorDataCallback editorDataCallback=nullptr;
    void* editorDataContext=nullptr;
    std::string inspectorError;
    std::thread::id thread = std::this_thread::get_id();
    SE_UpdateCallback callback = nullptr;
    void* context = nullptr;
    bool stepping = false;
    SE_TickCallback tickCallback=nullptr;
    void* tickContext=nullptr;
    SE_EventCallback eventCallback=nullptr;
    void* eventContext=nullptr;
    SE_SceneCallback sceneCallback=nullptr;
    void* sceneContext=nullptr;
    std::unordered_map<std::uint64_t,se::EventBus::SubscriptionToken> eventSubscriptions;
    bool environmentDriven=false,environmentPhysics=false,pendingEnvironmentRelease=false,callbackFailed=false;
    double value = 0.0;
    uint64_t stepCount = 0;
};
struct Slot
{
    std::unique_ptr<Session> value;
    uint32_t generation = 1;
};
std::vector<Slot> slots;
struct EventPublisher {SE_Session session;se::EventBus::Publisher value;};
std::unordered_map<std::uint64_t,EventPublisher> eventPublishers;
se::EventBus& EventsFor(Session& session)
{
    auto& events=session.Env().Events();
    if(!session.Env().Closing()&&!events.BindWorld(session.WorldRef()))throw std::runtime_error("Event.ContextMismatch");
    if(!session.Env().Closing())events.SynchronizeIdleClock(session.Env().FrameIndex(),session.Env().FixedStepIndex());
    for(auto it=session.eventSubscriptions.begin();it!=session.eventSubscriptions.end();)if(!it->second.Active())it=session.eventSubscriptions.erase(it);else ++it;
    return events;
}
void CloseEventPublishers(SE_Session handle)
{
    for(auto it=eventPublishers.begin();it!=eventPublishers.end();)
        if(it->second.session==handle){it->second.value.Close();it=eventPublishers.erase(it);}else ++it;
}

int32_t Error(int32_t code, const char* key) noexcept { lastError = key; return code; }
template<class Function> int32_t Protect(Function&& function) noexcept
{
    try { std::lock_guard lock(sessionsMutex); lastError = "Bridge.Success"; return function(); }
    catch (const std::bad_alloc&) { return Error(SE_OUT_OF_MEMORY, "Bridge.OutOfMemory"); }
    catch (...) { return Error(SE_INTERNAL_ERROR, "Bridge.NativeException"); }
}
Session* Find(SE_Session handle, bool checkThread = true)
{
    const uint32_t encodedIndex = static_cast<uint32_t>(handle);
    const uint32_t generation = static_cast<uint32_t>(handle >> 32);
    if (encodedIndex == 0 || encodedIndex - 1 >= slots.size()
        || slots[encodedIndex - 1].generation != generation || !slots[encodedIndex - 1].value)
    {
        Error(SE_INVALID_HANDLE, "Bridge.InvalidSession");
        return nullptr;
    }
    Session* session = slots[encodedIndex - 1].value.get();
    if (checkThread && session->thread != std::this_thread::get_id())
    {
        Error(SE_WRONG_THREAD, "Bridge.WrongThread");
        return nullptr;
    }
    return session;
}
int32_t MissingSession() { return std::strcmp(lastError, "Bridge.WrongThread") == 0 ? SE_WRONG_THREAD : SE_INVALID_HANDLE; }
bool ValidUtf8(const char* text, uint32_t size)
{
    if (!text && size != 0) return false;
    for (uint32_t i = 0; i < size;)
    {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first == 0) return false;
        if (first < 0x80) continue;
        uint32_t code = 0, tail = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { code = first & 0x1f; tail = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { code = first & 0x0f; tail = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { code = first & 0x07; tail = 3; minimum = 0x10000; }
        else return false;
        if (tail > size - i) return false;
        while (tail--)
        {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
int32_t CopyText(const std::string_view text, char* buffer, uint32_t capacity, uint32_t* required)
{
    if (!required || (capacity != 0 && !buffer)) return SE_INVALID_ARGUMENT;
    *required = static_cast<uint32_t>(text.size() + 1);
    if (capacity < *required) return SE_BUFFER_TOO_SMALL;
    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    return SE_OK;
}
nlohmann::json BindingJson(const se::UiEventBinding& binding)
{return {{"Event",static_cast<int>(binding.event)},{"Entity",binding.entity},{"Script",binding.script},{"Action",binding.action}};}
bool ValidTransform(const SE_Transform& t)
{
    const se::Transform transform{{t.px,t.py,t.pz},{t.qx,t.qy,t.qz,t.qw},{t.sx,t.sy,t.sz}};
    return se::IsValid(transform)&&std::abs(t.sx)>1e-6f&&std::abs(t.sy)>1e-6f&&std::abs(t.sz)>1e-6f;
}
se::BodyHandle BodyFromPacked(SE_Body handle) { return {static_cast<uint32_t>(handle),static_cast<uint32_t>(handle>>32)}; }
se::Vec3 Vector(SE_Vector3 value) { return {value.x,value.y,value.z}; }
std::filesystem::path ResourceDirectory()
{
    // 候选/便携运行时携带构建后的 shader 资源；源码 ENGINE_ROOT 仅定位工具与配置。
    HMODULE module=nullptr;wchar_t modulePath[32768]{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&se_session_run_window),&module))return {};
    const DWORD pathLength=GetModuleFileNameW(module,modulePath,32768);
    if(!pathLength||pathLength>=32768)return {};
    return std::filesystem::path(modulePath).parent_path()/L"Resources";
}
std::filesystem::path EditorEnvironmentPath(const wchar_t* name)
{
    const DWORD required=GetEnvironmentVariableW(name,nullptr,0);
    if(!required||required>32768)return {};
    std::wstring value(required,L'\0');
    const auto length=GetEnvironmentVariableW(name,value.data(),required);
    if(!length||length>=required)return {};
    value.resize(length);return std::filesystem::path(value);
}
bool HasBody(Session& session, uint64_t stableId)
{
    for (auto body:session.physics.Bodies()) if (session.physics.GetBody(body)->description.entityStableId==stableId) return true;
    return false;
}
bool BodyAlive(Session& session,SE_Body body)
{
    if(!session.physics.IsAlive(BodyFromPacked(body)))return false;
    const auto owner=session.bodyOwners.find(body);
    return owner==session.bodyOwners.end()||(session.WorldRef().IsAlive(owner->second)&&static_cast<bool>(session.Env().Scenes().SceneOf(owner->second)));
}
void SweepBodies(Session& session)
{
    for (auto body:session.physics.Bodies())
        if(!BodyAlive(session,body.Pack())||!session.WorldRef().IsAlive(session.WorldRef().FindByStableId(session.physics.GetBody(body)->description.entityStableId)))session.physics.DestroyBody(body);
    std::erase_if(session.bodyOwners,[&](const auto& item){return !session.physics.IsAlive(BodyFromPacked(item.first));});
    std::erase_if(session.characters,[&](const auto& item){return !session.WorldRef().IsAlive(se::EntityHandle::FromPacked(item.first));});
}
se::SceneManager& ScenesFor(Session& session)
{
    EventsFor(session);auto& scenes=session.Env().Scenes();if(!session.Env().Closing()&&!scenes.BindWorld(session.WorldRef()))throw std::logic_error("Scene.ContextMismatch");return scenes;
}
void SceneServices(Session& session,SE_Session handle)
{
    session.Env().Events().RegisterType({4,"engine.scene_state",1,8});
    if(session.assets||!session.borrowedEnvironment)session.Env().SetTODResolver([&session](const std::string& id,std::string& error){
        std::vector<se::AssetDiagnostic> diagnostics;auto profile=session.assets?session.assets->LoadTODProfile(id,diagnostics):nullptr;
        if(!profile)error=diagnostics.empty()?"TOD.ResolveFailed":diagnostics.back().key;return profile;
    });
    session.Env().Scenes().SetValidator([&session](const se::World& candidate,std::string& error){
        for(const auto entity:candidate.Entities())if(const auto* mesh=candidate.GetMesh(entity))
        {
            std::vector<se::AssetDiagnostic> diagnostics;
            if(!session.assets||!session.assets->LoadModel(mesh->meshAsset,diagnostics)){error="Scene.DependencyFailed";return false;}
            for(const auto& material:mesh->materials)if(!material.empty()&&!session.assets->LoadMaterial(material,diagnostics)){error="Scene.DependencyFailed";return false;}
        }
        return true;
    });
    session.Env().Scenes().SetUnloadHook([&session,handle](se::SceneHandle scene,std::span<const se::EntityHandle> members,std::span<const se::StableEntityId> invalidated){
        bool failed=false;const bool previous=session.stepping;session.stepping=true;
        struct Restore{Session& session;bool previous;~Restore(){session.stepping=previous;}} restore{session,previous};
        try
        {
            if(session.sceneCallback)
            {
                std::vector<SE_Entity> packed;packed.reserve(members.size());for(const auto entity:members)packed.push_back(entity.Pack());
                struct Unlock{Unlock(){sessionsMutex.unlock();}~Unlock(){sessionsMutex.lock();}} unlock;
                failed=session.sceneCallback(handle,scene.value,packed.data(),static_cast<uint32_t>(packed.size()),invalidated.data(),static_cast<uint32_t>(invalidated.size()),session.sceneContext)!=0;
            }
        }
        catch(...){failed=true;}
        for(const auto body:session.physics.Bodies())
        {
            const auto owner=session.bodyOwners.find(body.Pack());
            if(owner!=session.bodyOwners.end()&&std::find(members.begin(),members.end(),owner->second)!=members.end())
            {session.physics.DestroyBody(body);session.bodyOwners.erase(owner);}
        }
        std::erase_if(session.characters,[&](const auto& item){return std::find_if(members.begin(),members.end(),[&](auto entity){return entity.Pack()==item.first;})!=members.end();});
        std::erase_if(session.contactEvents,[&](const auto& event){return std::any_of(members.begin(),members.end(),[&](auto entity){const auto* record=session.WorldRef().Get(entity);return record&&(record->stableId==event.entity_a||record->stableId==event.entity_b);});});
        if(failed)throw std::runtime_error("Scene.CallbackFailed");
    });
}
void CaptureInput(Session& session,const InputState& input,bool consumed,bool pointerConsumed)
{
            session.inputActions.Update(input, consumed, pointerConsumed);
            session.pointer={};session.pointer.x=input.mouseX;session.pointer.y=input.mouseY;session.pointer.consumed=pointerConsumed?1:0;
            if(!pointerConsumed)
            {
                for(size_t i=0;i<input.mouseButtons.size();++i)if(input.mouseButtons[i])session.pointer.down|=1u<<i;
                for(const auto& event:input.pointerEvents)
                {
                    if(event.type==PointerEventType::Cancel){session.pointer.down=session.pointer.pressed=session.pointer.released=0;session.pointer.wheel=0;session.pointer.consumed=1;continue;}
                    if(event.type==PointerEventType::Wheel)session.pointer.wheel+=event.wheel;
                    if(event.button>=3)continue;
                    if(event.type==PointerEventType::Down)session.pointer.pressed|=1u<<event.button;
                    if(event.type==PointerEventType::Up)session.pointer.released|=1u<<event.button;
                }
            }
            session.input={};session.input.keyboard_consumed=consumed?1:0;
            if(consumed)return;
            for(size_t i=0;i<input.keysDown.size();++i)session.input.down[i]=input.keysDown[i]?1:0;
            for(const auto& event:input.keyEvents)
            {
                // 取消也标记本帧不可消费，清除托管层尚未到固定步的输入边沿。
                if(event.type==KeyEventType::Reset){session.input={};session.input.keyboard_consumed=1;continue;}
                if(event.keyCode>=256)continue;
                if(event.type==KeyEventType::Pressed)session.input.pressed[event.keyCode]=1;
                else if(event.type==KeyEventType::Released)session.input.released[event.keyCode]=1;
            }
}
int32_t PhysicsStep(Session& session, double dt)
{
    if (!std::isfinite(dt)||dt<=0||dt>1) return Error(SE_INVALID_ARGUMENT,"Bridge.InvalidDelta");
    SweepBodies(session);
    if(!session.scenePhysics.Synchronize(session.WorldRef(),session.physics))return Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");
    const bool stepped=session.environmentDriven?session.physics.StepFromEnvironment(static_cast<float>(dt)):session.physics.Step(static_cast<float>(dt));
    if(!stepped)return Error(SE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");
    for (auto body:session.physics.Bodies())
    {
        if(session.scenePhysics.Owns(body))continue;
        const auto& state=*session.physics.GetBody(body);
        const auto entity=session.WorldRef().FindByStableId(state.description.entityStableId);
        if (const auto* original=session.WorldRef().GetTransform(entity))
        {
            auto transform=*original;
            transform.position=state.description.shape.center;
            transform.rotation=state.description.shape.rotation;
            if (!session.WorldRef().SetTransform(entity,transform)) return Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");
        }
    }
    if(!session.scenePhysics.ApplyTransforms(session.WorldRef(),session.physics)||
        !session.scenePhysics.AdvanceCharacters(session.WorldRef(),session.physics,static_cast<float>(dt)))return Error(SE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");
    for (const auto& event:session.physics.Events())
        session.contactEvents.push_back({event.entityA,event.entityB,event.began?1:0,event.ended?1:0});
    return SE_OK;
}
int32_t EnvironmentCallback(Session& session,SE_Session handle,std::uint64_t component,std::uint32_t phase,const se::TickContext& context)
{
    if(!session.tickCallback)return 0;
    struct Unlock{Unlock(){sessionsMutex.unlock();}~Unlock(){sessionsMutex.lock();}} unlock;
    const auto result=session.tickCallback(handle,component,phase,context.deltaSeconds,context.frame,context.fixedStep,session.tickContext);
    if(result)session.callbackFailed=true;return result;
}
se::EnvironmentHooks EnvironmentHooks(Session& session,SE_Session handle)
{
    se::EnvironmentHooks hooks;
    hooks.phase=[&session,handle](se::EnvironmentPhase phase,const se::TickContext& context){
        if(phase==se::EnvironmentPhase::FrameBegin&&session.callback)
        {
            struct Unlock{Unlock(){sessionsMutex.unlock();}~Unlock(){sessionsMutex.lock();}} unlock;
            if(session.callback(handle,context.deltaSeconds,session.context)){session.callbackFailed=true;return false;}
        }
        if(phase==se::EnvironmentPhase::BeforePhysics)SweepBodies(session);
        if(EnvironmentCallback(session,handle,0,0x100u|static_cast<std::uint32_t>(phase),context))return false;
        if(phase==se::EnvironmentPhase::FrameEnd)session.UiRef().Update(context.frameSeconds);
        return true;
    };
    hooks.physics=[&session](double seconds){return !session.environmentDriven||!session.environmentPhysics||PhysicsStep(session,seconds)==SE_OK;};
    return hooks;
}
void FinishEnvironmentRelease(Session& session)
{
    if(!session.pendingEnvironmentRelease)return;
    session.environmentDriven=false;session.environmentPhysics=false;session.pendingEnvironmentRelease=false;
    session.physics.SetEnvironmentDriven(false);
    if(!session.Env().Closing()){session.Env().ConfigureClock(1.0/60,8,false);session.Env().SetPaused(false);}session.physicsClock.ClearRemainder();
}
// 调用者持有 sessionsMutex；普通帧与编辑器单步共用回调、物理、UI 和退出清理边界。
int32_t AdvanceSession(Session& session,SE_Session handle,double seconds,bool singleStep)
{
    if(session.stepping)return Error(SE_BUSY,"Bridge.SessionBusy");
    if(!singleStep&&session.Env().FramePaused()&&!session.Env().Closing())return SE_OK;
    struct SteppingGuard{Session& value;~SteppingGuard(){value.stepping=false;}} guard{session};
    session.stepping=true;session.callbackFailed=false;
    const auto hooks=EnvironmentHooks(session,handle);
    const bool result=singleStep?session.Env().StepFrame(hooks):session.Env().RunFrame(seconds,hooks);
    SweepBodies(session);FinishEnvironmentRelease(session);
    if(!result)return session.callbackFailed?Error(SE_CALLBACK_FAILED,"Bridge.CallbackFailed"):Error(SE_INTERNAL_ERROR,session.Env().LastError().c_str());
    ++session.stepCount;return SE_OK;
}
bool EditorDocumentRequest(SE_Session handle,Session& session,const nlohmann::json& request,bool snapshot,
    nlohmann::json& response,std::string& error)
{
    error.clear();std::string reply;
    const auto command=request.dump();
    const auto result=Protect([&]()->int32_t{
        if(Find(handle)!=&session)return MissingSession();
        if(!session.editor||!session.editorDataCallback)return Error(SE_INVALID_ARGUMENT,"Bridge.EditorNotRunning");
        const auto operation=request.value("Document",std::string{});
        if(session.editor->IsPlaying()&&(operation=="Save"||operation=="Restore"))return Error(SE_BUSY,"Bridge.EditorReadOnly");
        const bool prior=session.stepping;session.stepping=true;
        struct Restore{Session& session;bool prior;~Restore(){sessionsMutex.lock();session.stepping=prior;}} restore{session,prior};
        sessionsMutex.unlock();
        std::uint32_t required=0;std::vector<char> buffer;
        if(snapshot)
        {
            const auto measured=session.editorDataCallback(handle,0,command.data(),static_cast<std::uint32_t>(command.size()),nullptr,0,&required,session.editorDataContext);
            if((measured!=SE_OK&&measured!=SE_BUFFER_TOO_SMALL)||required==0||required>32u*1024*1024)
                return Error(SE_CALLBACK_FAILED,"Script.MetadataFailed");
            buffer.resize(required);
        }
        else buffer.resize(4096); // 命令结果仅 {} 或 Error；长度不足也不能重试有副作用的命令。
        const auto called=session.editorDataCallback(handle,0,command.data(),static_cast<std::uint32_t>(command.size()),buffer.data(),
            static_cast<std::uint32_t>(buffer.size()),&required,session.editorDataContext);
        if(called!=SE_OK||required==0||required>buffer.size()||buffer[required-1]!='\0')return Error(SE_CALLBACK_FAILED,"Script.MetadataFailed");
        reply.assign(buffer.data(),required-1);return SE_OK;
    });
    if(result!=SE_OK){error=lastError;return false;}
    try
    {
        response=nlohmann::json::parse(reply);
        if(!response.is_object()){error="Script.DocumentInvalid";return false;}
        if(response.contains("Error")){error=response.at("Error").get<std::string>();if(error.empty())error="Script.DocumentInvalid";return false;}
        return true;
    }
    catch(const std::exception&){error="Script.DocumentInvalid";return false;}
}
}
static_assert(sizeof(SE_Transform) == 40);
static_assert(sizeof(SE_EventHeader)==88&&sizeof(SE_EventStats)==104);
static_assert(sizeof(SE_BodyState)==72);
static_assert(sizeof(SE_AbiInfo) == 16);
static_assert(sizeof(SE_ModelInfo)==16);
static_assert(sizeof(SE_Vector3)==12 && sizeof(SE_BoxBodyDesc)==32 && sizeof(SE_RayHit)==48);
static_assert(sizeof(SE_ContactEvent)==24 && sizeof(SE_UiStyle)==56 && sizeof(SE_InputState)==772);
static_assert(sizeof(SE_PointerState)==28 && sizeof(SE_CharacterDesc)==20 && sizeof(SE_CharacterState)==20);

#include "NativeSessionExports.inl"
#include "NativeRenderExports.inl"
#include "NativeWorldExports.inl"
#include "NativePhysicsExports.inl"
#include "NativeUiExports.inl"
#include "NativeInputExports.inl"
#include "NativeLogicExports.inl"
