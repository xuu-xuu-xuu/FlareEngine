#define FLARE_NATIVE_EXPORTS
#include "FunctionLayer/Scripting/NativeBridge/EngineNative.h"
#include "FunctionLayer/Runtime/World.h"
#include "FunctionLayer/Runtime/Environment.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"
#include "FunctionLayer/Physics/PhysicsWorld.h"
#include "FunctionLayer/Physics/ScenePhysics.h"
#include "FunctionLayer/Audio/AudioWorld.h"
#include "FunctionLayer/Audio/SceneAudio.h"
#include "FunctionLayer/Particles/SceneParticles.h"
#include "FunctionLayer/Navigation/SceneNavigation.h"
#include "PlatformLayer/Windows/XAudio2Backend.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include <nlohmann/json.hpp>
#include "FunctionLayer/Application/FeatureFrameData.h"
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
#include "CoreLayer/Math/TransformDecomposition.h"
#include "PlatformLayer/Windows/ProcessDiagnostics.h"
#include "ToolLayer/Editor/EditorApplication.h"
#include "ToolLayer/Editor/EditorRenderer.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Editor/EditorWorkspaceHost.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include <filesystem>
#include <fstream>
#include <array>
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
thread_local std::array<char, 256> lastError = [] {
    std::array<char, 256> initial{};
    constexpr char ok[] = "Bridge.Success";
    std::memcpy(initial.data(), ok, sizeof(ok));
    return initial;
}();
std::recursive_mutex sessionsMutex;
struct RuntimeAppearance
{
    float lookMode=-1,lookBlend=0;
    std::optional<flare::SkyView> sky;

    void Apply(flare::FrameData& frame) const
    {
        if(lookMode>=0){frame.lookMode=lookMode;frame.lookBlend=lookBlend;}
        if(sky)frame.sky=*sky;
    }
};
struct Session
{
    Session()
    {
        environment.ConfigureClock(1.0/60,8,false);
        environment.Events().RegisterType({4,"engine.scene_state",1,8});
        audioBackend=std::make_unique<flare::XAudio2Backend>();
        if(audioBackend->Initialize())audio.SetBackend(audioBackend.get());
        else audioBackend.reset();
    }
    ~Session()
    {
        if(cursorClipApplied){ClipCursor(nullptr);cursorClipApplied=false;}
        if(cursorHiddenApplied){ShowCursor(TRUE);cursorHiddenApplied=false;}
        sceneNavigation.Clear();
        sceneParticles.Clear();
        sceneAudio.Clear(audio);
        audio.Shutdown();
        if(audioBackend)audioBackend->Shutdown();
        environment.BeginShutdown();
        world.Clear();
        while(world.HasPendingCommands())world.FlushCommands();
    }
    flare::Environment environment;
    NativeLogicObjects logicObjects;
    flare::World& world=environment.GetWorld();
    flare::Environment* borrowedEnvironment=nullptr;
    flare::Environment& Env(){return borrowedEnvironment?*borrowedEnvironment:environment;}
    const flare::Environment& ReadEnvironment()const{return borrowedEnvironment?*borrowedEnvironment:editor?editor->EditEnvironment():environment;}
    flare::World* borrowedWorld=nullptr;
    flare::World& WorldRef() { return borrowedWorld?*borrowedWorld:world; }
    const flare::World& ReadWorld() const {return borrowedWorld?*borrowedWorld:editor?editor->EditWorld():world;}
    flare::PhysicsWorld physics;
    flare::ScenePhysics scenePhysics;
    std::unique_ptr<flare::XAudio2Backend> audioBackend;
    flare::AudioWorld audio;
    flare::SceneAudio sceneAudio;
    flare::SceneParticles sceneParticles;
    flare::SceneNavigation sceneNavigation;
    std::unordered_map<std::uint64_t,flare::EntityHandle> bodyOwners;
    bool physicsDebugDraw=true;
    flare::FixedStepClock physicsClock;
    std::vector<FLARE_ContactEvent> contactEvents;
    std::vector<FLARE_TriggerEvent> triggerEvents;
    flare::TriggerTracker characterTriggerTracker;
    flare::UiTree ui;
    flare::UiTree* borrowedUi=nullptr;
    flare::UiTree& UiRef() { return borrowedUi?*borrowedUi:ui; }
    std::vector<flare::UiEvent> uiEvents;
    FLARE_InputState input{};
    FLARE_PointerState pointer{};
    FLARE_Vector2 mouseDelta{};
    bool cursorLocked=false,cursorVisible=true,cursorLockPausedByEsc=false;
    bool cursorClipApplied=false,cursorHiddenApplied=false;
    HWND activeGameWindow=nullptr;
    RECT activeGameScreenRect{};
    POINT lockedCenterScreen{};
    flare::InputActionMap inputActions{flare::DefaultInputActionDocument()};
    std::unordered_map<FLARE_Entity,flare::CharacterController> characters;
    Localization localization;
    std::unique_ptr<flare::AssetStore> assets;
    bool windowRunning=false,stopWindow=false;
    std::unique_ptr<flare::PlayerQualityPreference> playerQuality;
    bool showPlayerQualityMenu=true;
    flare::QualityProfiles qualityProfiles=flare::DefaultQualityProfiles;
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
    flare::EditorSession* editor=nullptr;
    int32_t pendingPlay=-1;
    FLARE_EditorCallback editorCallback=nullptr;
    void* editorContext=nullptr;
    FLARE_EditorDataCallback editorDataCallback=nullptr;
    void* editorDataContext=nullptr;
    std::string inspectorError;
    std::thread::id thread = std::this_thread::get_id();
    FLARE_UpdateCallback callback = nullptr;
    void* context = nullptr;
    FLARE_UpdateCallback postStepCallback = nullptr;
    void* postStepContext = nullptr;
    bool stepping = false;
    FLARE_TickCallback tickCallback=nullptr;
    void* tickContext=nullptr;
    FLARE_EventCallback eventCallback=nullptr;
    void* eventContext=nullptr;
    FLARE_SceneCallback sceneCallback=nullptr;
    void* sceneContext=nullptr;
    std::unordered_map<std::uint64_t,flare::EventBus::SubscriptionToken> eventSubscriptions;
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
struct EventPublisher {FLARE_Session session;flare::EventBus::Publisher value;};
std::unordered_map<std::uint64_t,EventPublisher> eventPublishers;
flare::EventBus& EventsFor(Session& session)
{
    auto& events=session.Env().Events();
    if(!session.Env().Closing()&&!events.BindWorld(session.WorldRef()))throw std::runtime_error("Event.ContextMismatch");
    if(!session.Env().Closing())events.SynchronizeIdleClock(session.Env().FrameIndex(),session.Env().FixedStepIndex());
    for(auto it=session.eventSubscriptions.begin();it!=session.eventSubscriptions.end();)if(!it->second.Active())it=session.eventSubscriptions.erase(it);else ++it;
    return events;
}
void CloseEventPublishers(FLARE_Session handle)
{
    for(auto it=eventPublishers.begin();it!=eventPublishers.end();)
        if(it->second.session==handle){it->second.value.Close();it=eventPublishers.erase(it);}else ++it;
}

int32_t Error(int32_t code, const char* key) noexcept
{
    const std::string_view text = (key && *key) ? std::string_view(key) : std::string_view("Bridge.NativeException");
    const std::size_t length = std::min<std::size_t>(text.size(), lastError.size() - 1);
    if (length > 0) std::memcpy(lastError.data(), text.data(), length);
    lastError[length] = '\0';
    return code;
}
template<class Function> int32_t Protect(Function&& function) noexcept
{
    try { std::lock_guard lock(sessionsMutex); Error(FLARE_OK, "Bridge.Success"); return function(); }
    catch (const std::bad_alloc&) { return Error(FLARE_OUT_OF_MEMORY, "Bridge.OutOfMemory"); }
    catch (...) { return Error(FLARE_INTERNAL_ERROR, "Bridge.NativeException"); }
}
Session* Find(FLARE_Session handle, bool checkThread = true)
{
    const uint32_t encodedIndex = static_cast<uint32_t>(handle);
    const uint32_t generation = static_cast<uint32_t>(handle >> 32);
    if (encodedIndex == 0 || encodedIndex - 1 >= slots.size()
        || slots[encodedIndex - 1].generation != generation || !slots[encodedIndex - 1].value)
    {
        Error(FLARE_INVALID_HANDLE, "Bridge.InvalidSession");
        return nullptr;
    }
    Session* session = slots[encodedIndex - 1].value.get();
    if (checkThread && session->thread != std::this_thread::get_id())
    {
        Error(FLARE_WRONG_THREAD, "Bridge.WrongThread");
        return nullptr;
    }
    return session;
}
int32_t MissingSession() { return std::strcmp(lastError.data(), "Bridge.WrongThread") == 0 ? FLARE_WRONG_THREAD : FLARE_INVALID_HANDLE; }
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
    if (!required || (capacity != 0 && !buffer)) return FLARE_INVALID_ARGUMENT;
    *required = static_cast<uint32_t>(text.size() + 1);
    if (capacity < *required) return FLARE_BUFFER_TOO_SMALL;
    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    return FLARE_OK;
}
nlohmann::json BindingJson(const flare::UiEventBinding& binding)
{return {{"Event",static_cast<int>(binding.event)},{"Entity",binding.entity},{"Script",binding.script},{"Action",binding.action}};}
bool ValidTransform(const FLARE_Transform& t)
{
    const flare::Transform transform{{t.px,t.py,t.pz},{t.qx,t.qy,t.qz,t.qw},{t.sx,t.sy,t.sz}};
    return flare::IsValid(transform)&&std::abs(t.sx)>1e-6f&&std::abs(t.sy)>1e-6f&&std::abs(t.sz)>1e-6f;
}
flare::BodyHandle BodyFromPacked(FLARE_Body handle) { return {static_cast<uint32_t>(handle),static_cast<uint32_t>(handle>>32)}; }
flare::Vec3 Vector(FLARE_Vector3 value) { return {value.x,value.y,value.z}; }
std::filesystem::path ResourceDirectory()
{
    // 候选/便携运行时携带构建后的 shader 资源；源码 ENGINE_ROOT 仅定位工具与配置。
    HMODULE module=nullptr;wchar_t modulePath[32768]{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&flare_session_run_window),&module))return {};
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
bool BodyAlive(Session& session,FLARE_Body body)
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
    std::erase_if(session.characters,[&](const auto& item){return !session.WorldRef().IsAlive(flare::EntityHandle::FromPacked(item.first));});
}
flare::SceneManager& ScenesFor(Session& session)
{
    EventsFor(session);auto& scenes=session.Env().Scenes();if(!session.Env().Closing()&&!scenes.BindWorld(session.WorldRef()))throw std::logic_error("Scene.ContextMismatch");return scenes;
}
void SceneServices(Session& session,FLARE_Session handle)
{
    session.Env().Events().RegisterType({4,"engine.scene_state",1,8});
    if(session.assets||!session.borrowedEnvironment)session.Env().SetTODResolver([&session](const std::string& id,std::string& error){
        std::vector<flare::AssetDiagnostic> diagnostics;auto profile=session.assets?session.assets->LoadTODProfile(id,diagnostics):nullptr;
        if(!profile)error=diagnostics.empty()?"TOD.ResolveFailed":diagnostics.back().key;return profile;
    });
    session.Env().Scenes().SetValidator([&session](const flare::World& candidate,std::string& error){
        for(const auto entity:candidate.Entities())if(const auto* mesh=candidate.GetMesh(entity))
        {
            std::vector<flare::AssetDiagnostic> diagnostics;
            if(!session.assets||!session.assets->LoadModel(mesh->meshAsset,diagnostics)){error="Scene.DependencyFailed";return false;}
            for(const auto& material:mesh->materials)if(!material.empty()&&!session.assets->LoadMaterial(material,diagnostics)){error="Scene.DependencyFailed";return false;}
        }
        return true;
    });
    session.Env().Scenes().SetUnloadHook([&session,handle](flare::SceneHandle scene,std::span<const flare::EntityHandle> members,std::span<const flare::StableEntityId> invalidated){
        bool failed=false;const bool previous=session.stepping;session.stepping=true;
        struct Restore{Session& session;bool previous;~Restore(){session.stepping=previous;}} restore{session,previous};
        try
        {
            if(session.sceneCallback)
            {
                std::vector<FLARE_Entity> packed;packed.reserve(members.size());for(const auto entity:members)packed.push_back(entity.Pack());
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
        std::erase_if(session.triggerEvents,[&](const auto& event){return std::any_of(members.begin(),members.end(),[&](auto entity){const auto* record=session.WorldRef().Get(entity);return record&&(record->stableId==event.trigger_entity||record->stableId==event.other_entity);});});
        for(const auto id:invalidated)
        {
            session.sceneParticles.RemoveEmitter(id);
            session.sceneNavigation.RemoveAgent(id);
        }
        for(const auto entity:members)if(const auto* record=session.WorldRef().Get(entity))
        {
            session.sceneParticles.RemoveEmitter(record->stableId);
            session.sceneNavigation.RemoveAgent(record->stableId);
        }
        if(failed)throw std::runtime_error("Scene.CallbackFailed");
    });
}
void ReleaseSessionCursor(Session& session) noexcept
{
    if(session.cursorClipApplied){ClipCursor(nullptr);session.cursorClipApplied=false;}
    if(session.cursorHiddenApplied){ShowCursor(TRUE);session.cursorHiddenApplied=false;}
}
void CaptureInput(Session& session,const InputState& input,bool consumed,bool pointerConsumed)
{
            session.inputActions.Update(input, consumed, pointerConsumed);
            session.pointer={};session.pointer.x=input.mouseX;session.pointer.y=input.mouseY;session.pointer.consumed=pointerConsumed?1:0;
            session.mouseDelta=pointerConsumed?FLARE_Vector2{0,0}:FLARE_Vector2{static_cast<float>(input.mouseDeltaX),static_cast<float>(input.mouseDeltaY)};
            if(!consumed)
            {
                for(const auto& ev:input.keyEvents)
                {
                    if(ev.type==KeyEventType::Pressed&&ev.keyCode==VK_ESCAPE&&session.cursorLocked)
                    {
                        session.cursorLockPausedByEsc=true;
                        ReleaseSessionCursor(session);
                    }
                }
            }
            if(!pointerConsumed)
            {
                for(size_t i=0;i<input.mouseButtons.size();++i)if(input.mouseButtons[i])session.pointer.down|=1u<<i;
                for(const auto& event:input.pointerEvents)
                {
                    if(event.type==PointerEventType::Cancel){session.pointer.down=session.pointer.pressed=session.pointer.released=0;session.pointer.wheel=0;session.pointer.consumed=1;session.mouseDelta={0,0};ReleaseSessionCursor(session);continue;}
                    if(event.type==PointerEventType::Wheel)session.pointer.wheel+=event.wheel;
                    if(event.button>=3)continue;
                    if(event.type==PointerEventType::Down){session.pointer.pressed|=1u<<event.button;session.cursorLockPausedByEsc=false;}
                    if(event.type==PointerEventType::Up)session.pointer.released|=1u<<event.button;
                }
            }
            const bool windowForeground=session.activeGameWindow!=nullptr&&IsWindow(session.activeGameWindow)&&GetForegroundWindow()==session.activeGameWindow;
            const bool wantLock=session.cursorLocked&&!session.cursorLockPausedByEsc&&!pointerConsumed&&windowForeground;
            if(wantLock)
            {
                RECT clip=session.activeGameScreenRect;
                if(clip.right<=clip.left||clip.bottom<=clip.top)
                {
                    RECT client{};
                    if(GetClientRect(session.activeGameWindow,&client))
                    {
                        POINT tl{client.left,client.top},br{client.right,client.bottom};
                        ClientToScreen(session.activeGameWindow,&tl);
                        ClientToScreen(session.activeGameWindow,&br);
                        clip={tl.x,tl.y,br.x,br.y};
                    }
                }
                if((clip.right-clip.left)>=16&&(clip.bottom-clip.top)>=16)
                {
                    const POINT center{(clip.left+clip.right)/2,(clip.top+clip.bottom)/2};
                    POINT cur{};
                    if(GetCursorPos(&cur))
                    {
                        if(session.cursorClipApplied)
                            session.mouseDelta={static_cast<float>(cur.x-session.lockedCenterScreen.x),static_cast<float>(cur.y-session.lockedCenterScreen.y)};
                        if(cur.x!=center.x||cur.y!=center.y)
                            SetCursorPos(center.x,center.y);
                    }
                    session.lockedCenterScreen=center;
                    ClipCursor(&clip);
                    session.cursorClipApplied=true;
                }
            }
            else if(session.cursorClipApplied)
            {
                ClipCursor(nullptr);
                session.cursorClipApplied=false;
            }
            const bool wantHidden=(!session.cursorVisible||(session.cursorLocked&&!session.cursorLockPausedByEsc))&&!pointerConsumed&&windowForeground;
            if(wantHidden&&!session.cursorHiddenApplied)
            {
                ShowCursor(FALSE);
                session.cursorHiddenApplied=true;
            }
            else if(!wantHidden&&session.cursorHiddenApplied)
            {
                ShowCursor(TRUE);
                session.cursorHiddenApplied=false;
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
    if (!std::isfinite(dt)||dt<=0||dt>1) return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidDelta");
    SweepBodies(session);
    if(!session.scenePhysics.Synchronize(session.WorldRef(),session.physics))return Error(FLARE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");
    const bool stepped=session.environmentDriven?session.physics.StepFromEnvironment(static_cast<float>(dt)):session.physics.Step(static_cast<float>(dt));
    if(!stepped)return Error(FLARE_INVALID_ARGUMENT,"Bridge.PhysicsFailed");
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
            if (!session.WorldRef().SetTransform(entity,transform)) return Error(FLARE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");
        }
    }
    if(!session.scenePhysics.ApplyTransforms(session.WorldRef(),session.physics))return Error(FLARE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");
    session.sceneNavigation.StepAgents(session.WorldRef(),session.physics,session.scenePhysics,session.assets.get(),static_cast<float>(dt));
    if(!session.scenePhysics.AdvanceCharacters(session.WorldRef(),session.physics,static_cast<float>(dt)))return Error(FLARE_INTERNAL_ERROR,"Bridge.WorldOperationFailed");
    for (const auto& event:session.physics.Events())
        session.contactEvents.push_back({event.entityA,event.entityB,event.began?1:0,event.ended?1:0,{event.point.x,event.point.y,event.point.z},{event.normal.x,event.normal.y,event.normal.z},event.normalImpulse,0});
    for (const auto& event:session.physics.TriggerEvents())
        session.triggerEvents.push_back({event.triggerEntity,event.otherEntity,event.entered?1:0,0});

    std::vector<flare::TriggerOverlap> characterOverlaps;
    const auto collectCharacterTriggers=[&](flare::StableEntityId charStableId,const flare::CharacterController& character)
    {
        if(!charStableId)return;
        const flare::BodyHandle syntheticChar{static_cast<std::uint32_t>(charStableId),static_cast<std::uint32_t>(charStableId>>32)};
        for(const auto body:session.physics.Bodies())
        {
            const auto* state=session.physics.GetBody(body);
            if(!state||!state->description.isTrigger||!state->description.entityStableId)continue;
            const auto& desc=state->description;
            const auto contact=desc.shapeKind==flare::ColliderShapeKind::Sphere
                ?flare::CollideCapsuleSphere(character.position,character.segmentHalfHeight,character.radius,{desc.shape.center,desc.radius})
                :flare::CollideCapsuleBox(character.position,character.segmentHalfHeight,character.radius,desc.shape);
            if(contact&&contact->depth>1e-5f)
                characterOverlaps.push_back({body,syntheticChar,desc.entityStableId,charStableId});
        }
    };
    for(const auto& [packedEntity,character]:session.characters)
    {
        const auto* record=session.WorldRef().Get(flare::EntityHandle::FromPacked(packedEntity));
        if(record)collectCharacterTriggers(record->stableId,character);
    }
    for(const auto& [charStableId,character]:session.scenePhysics.Characters())
        collectCharacterTriggers(charStableId,character);
    session.characterTriggerTracker.Update(characterOverlaps);
    for(const auto& event:session.characterTriggerTracker.Events())
        session.triggerEvents.push_back({event.triggerEntity,event.otherEntity,event.entered?1:0,0});
    return FLARE_OK;
}
int32_t EnvironmentCallback(Session& session,FLARE_Session handle,std::uint64_t component,std::uint32_t phase,const flare::TickContext& context)
{
    if(!session.tickCallback)return 0;
    struct Unlock{Unlock(){sessionsMutex.unlock();}~Unlock(){sessionsMutex.lock();}} unlock;
    const auto result=session.tickCallback(handle,component,phase,context.deltaSeconds,context.frame,context.fixedStep,session.tickContext);
    if(result)session.callbackFailed=true;return result;
}
flare::EnvironmentHooks EnvironmentHooks(Session& session,FLARE_Session handle)
{
    flare::EnvironmentHooks hooks;
    hooks.phase=[&session,handle](flare::EnvironmentPhase phase,const flare::TickContext& context){
        if(phase==flare::EnvironmentPhase::FrameBegin&&session.callback)
        {
            struct Unlock{Unlock(){sessionsMutex.unlock();}~Unlock(){sessionsMutex.lock();}} unlock;
            if(session.callback(handle,context.deltaSeconds,session.context)){session.callbackFailed=true;return false;}
        }
        if(phase==flare::EnvironmentPhase::BeforePhysics)SweepBodies(session);
        if(EnvironmentCallback(session,handle,0,0x100u|static_cast<std::uint32_t>(phase),context))return false;
        if(phase==flare::EnvironmentPhase::FrameEnd)session.UiRef().Update(context.frameSeconds);
        return true;
    };
    hooks.physics=[&session](double seconds){return !session.environmentDriven||!session.environmentPhysics||PhysicsStep(session,seconds)==FLARE_OK;};
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
int32_t AdvanceSession(Session& session,FLARE_Session handle,double seconds,bool singleStep)
{
    if(session.stepping)return Error(FLARE_BUSY,"Bridge.SessionBusy");
    if(!singleStep&&session.Env().FramePaused()&&!session.Env().Closing())
    {
        session.sceneAudio.Synchronize(session.WorldRef(),session.assets.get(),session.audio,true);
        session.sceneParticles.Synchronize(session.WorldRef(),0.0,true);
        return FLARE_OK;
    }
    struct SteppingGuard{Session& value;~SteppingGuard(){value.stepping=false;}} guard{session};
    session.stepping=true;session.callbackFailed=false;
    const auto hooks=EnvironmentHooks(session,handle);
    const bool result=singleStep?session.Env().StepFrame(hooks):session.Env().RunFrame(seconds,hooks);
    SweepBodies(session);
    if(result)
    {
        const bool audioPaused=!singleStep&&(session.Env().Paused()||session.Env().FramePaused());
        session.sceneAudio.Synchronize(session.WorldRef(),session.assets.get(),session.audio,audioPaused);
        const double particleDt=singleStep?session.Env().Clock().Step():(audioPaused?0.0:seconds);
        session.sceneParticles.Synchronize(session.WorldRef(),particleDt,audioPaused);
    }
    FinishEnvironmentRelease(session);
    if(!result)return session.callbackFailed?Error(FLARE_CALLBACK_FAILED,"Bridge.CallbackFailed"):Error(FLARE_INTERNAL_ERROR,session.Env().LastError().c_str());
    ++session.stepCount;return FLARE_OK;
}
bool EditorDocumentRequest(FLARE_Session handle,Session& session,const nlohmann::json& request,bool snapshot,
    nlohmann::json& response,std::string& error)
{
    error.clear();std::string reply;
    const auto command=request.dump();
    const auto result=Protect([&]()->int32_t{
        if(Find(handle)!=&session)return MissingSession();
        if(!session.editor||!session.editorDataCallback)return Error(FLARE_INVALID_ARGUMENT,"Bridge.EditorNotRunning");
        const auto operation=request.value("Document",std::string{});
        if(session.editor->IsPlaying()&&(operation=="Save"||operation=="Restore"))return Error(FLARE_BUSY,"Bridge.EditorReadOnly");
        const bool prior=session.stepping;session.stepping=true;
        struct Restore{Session& session;bool prior;~Restore(){sessionsMutex.lock();session.stepping=prior;}} restore{session,prior};
        sessionsMutex.unlock();
        std::uint32_t required=0;std::vector<char> buffer;
        if(snapshot)
        {
            const auto measured=session.editorDataCallback(handle,0,command.data(),static_cast<std::uint32_t>(command.size()),nullptr,0,&required,session.editorDataContext);
            if((measured!=FLARE_OK&&measured!=FLARE_BUFFER_TOO_SMALL)||required==0||required>32u*1024*1024)
                return Error(FLARE_CALLBACK_FAILED,"Script.MetadataFailed");
            buffer.resize(required);
        }
        else buffer.resize(4096); // 命令结果仅 {} 或 Error；长度不足也不能重试有副作用的命令。
        const auto called=session.editorDataCallback(handle,0,command.data(),static_cast<std::uint32_t>(command.size()),buffer.data(),
            static_cast<std::uint32_t>(buffer.size()),&required,session.editorDataContext);
        if(called!=FLARE_OK||required==0||required>buffer.size()||buffer[required-1]!='\0')return Error(FLARE_CALLBACK_FAILED,"Script.MetadataFailed");
        reply.assign(buffer.data(),required-1);return FLARE_OK;
    });
    if(result!=FLARE_OK){error=lastError.data();return false;}
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
static_assert(sizeof(FLARE_Transform) == 40);
static_assert(sizeof(FLARE_EventHeader)==88&&sizeof(FLARE_EventStats)==104);
static_assert(sizeof(FLARE_BodyState)==72);
static_assert(sizeof(FLARE_AbiInfo) == 16);
static_assert(sizeof(FLARE_ModelInfo)==16);
static_assert(sizeof(FLARE_Vector3)==12 && sizeof(FLARE_BoxBodyDesc)==32 && sizeof(FLARE_SphereBodyDesc)==32 && sizeof(FLARE_RayHit)==48);
static_assert(sizeof(FLARE_ContactEvent)==56 && sizeof(FLARE_TriggerEvent)==24 && sizeof(FLARE_UiStyle)==56 && sizeof(FLARE_InputState)==772);
static_assert(sizeof(FLARE_PointerState)==28 && sizeof(FLARE_CharacterDesc)==20 && sizeof(FLARE_CharacterState)==20);
static_assert(sizeof(FLARE_AudioPlayDesc)==40);
static_assert(sizeof(FLARE_NavPathInfo)==12);

#include "NativeSessionExports.inl"
#include "NativeRenderExports.inl"
#include "NativeWorldExports.inl"
#include "NativePhysicsExports.inl"
#include "NativeUiExports.inl"
#include "NativeInputExports.inl"
#include "NativeAudioExports.inl"
#include "NativeParticlesExports.inl"
#include "NativeNavigationExports.inl"
#include "NativeLogicExports.inl"
