int32_t FLARE_CALL flare_session_run_window(FLARE_Session handle, const char* locale, uint32_t length, int32_t visible, uint64_t maximumFrames) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle);if(!s)return MissingSession();
        if(s->stepping||s->windowRunning)return Error(FLARE_BUSY,"Bridge.SessionBusy");
        if(!locale||length!=5||!ValidUtf8(locale,length))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const std::string language(locale,length);
        if(language!="zh-CN"&&language!="en-US")return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        if(!s->windowProfile.empty()){
            std::filesystem::create_directories(s->windowProfile.parent_path());
            if(!std::filesystem::create_directory(s->windowProfile))return Error(FLARE_INVALID_ARGUMENT,"Bridge.ProfileOutputExists");
        }
        struct WindowGuard{Session& s;~WindowGuard(){ReleaseSessionCursor(s);s.activeGameWindow=nullptr;s.activeGameScreenRect={};s.windowRunning=false;s.input={};s.pointer={};s.mouseDelta={};s.inputActions.Reset();}} guard{*s};
        s->windowRunning=true;s->stopWindow=false;
        const bool sequence=!s->captureSequence.empty()&&s->captureSequenceCount>0;
        if(sequence&&!s->windowCapture.empty())return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        flare::ApplicationServices services;services.ui=&s->UiRef();services.maximumFrames=maximumFrames;services.advanceUi=false;
        flare::Profiler profiler(flare::WindowProfileRenderer::FrameLimit);
        if(!s->windowProfile.empty()){
            profiler.SetEnabled(true);services.profiler=&profiler;
            services.initialWidth=s->profileWidth;services.initialHeight=s->profileHeight;
        }
        // 固定回放尺寸优先于窗口默认值；交换链在渲染器初始化前按客户区建立。
        if(s->windowWidth&&s->windowHeight){services.initialWidth=s->windowWidth;services.initialHeight=s->windowHeight;}
        // 序列抓帧必须与固定步一一对应：缺省 1/30 秒，即每个宿主更新推进 2 个 60 Hz 固定步。
        const double fixedStep=s->windowFixedStep>0?s->windowFixedStep:(sequence?1.0/30.0:0);
        if(fixedStep>0)services.fixedDeltaSeconds=fixedStep;
        if(sequence)
        {
            std::error_code error;std::filesystem::create_directories(s->captureSequence,error);
            if(error)return Error(FLARE_INVALID_ARGUMENT,"Bridge.CaptureDirectoryFailed");
            if(services.maximumFrames==0||services.maximumFrames>s->captureSequenceCount)services.maximumFrames=s->captureSequenceCount;
        }
        services.resourceDirectory=ResourceDirectory();
        if(services.resourceDirectory.empty())return Error(FLARE_INTERNAL_ERROR,"Bridge.WindowFailed");
        // 序列抓帧状态：cursor 是已抽取的输出帧序号；log 记录输出帧 → 固定步，供清单核对。
        struct CapturedFrame{std::uint64_t index=0,updateIndex=0,fixedStep=0;std::filesystem::path path;};
        std::uint64_t captureCursor=0;
        std::vector<CapturedFrame> captureLog;
        SandboxRenderer* gameRenderer=nullptr;
        // 动画资产按 AssetId 解析；缺失时该角色保持绑定姿态，不阻断整帧。
        s->Env().SetSkeletalResolver([s](const std::string& asset)->std::shared_ptr<const flare::StaticModel>{
            if(!s->assets)return nullptr;std::vector<flare::AssetDiagnostic> diagnostics;auto model=s->assets->LoadModel(asset,diagnostics);
            return model;});
        const bool interactive=visible&&!sequence&&s->windowCapture.empty()&&s->windowProfile.empty();
        std::unique_ptr<flare::PlayerQualityMenu> qualityMenu;
        HWND playerWindow=nullptr;
        services.windowOpened=[&](HWND hwnd){playerWindow=hwnd;s->activeGameWindow=hwnd;s->activeGameScreenRect={};return true;};
        if(interactive&&s->playerQuality&&s->showPlayerQualityMenu){
            qualityMenu=std::make_unique<flare::PlayerQualityMenu>(*s->playerQuality,s->localization,s->qualityProfiles);
            services.messageHandler=[&](HWND hwnd,UINT msg,WPARAM w,LPARAM l){return qualityMenu->Message(hwnd,msg,w,l);};
        }
        services.input=[s,&qualityMenu](const InputState& input,bool consumed,bool pointerConsumed){
            CaptureInput(*s,input,consumed||(qualityMenu&&qualityMenu->ConsumeKeys()),pointerConsumed||(qualityMenu&&qualityMenu->ConsumePointer()));
            if(qualityMenu)qualityMenu->EndInput();};
        services.runEnvironmentFrame=[s,handle,p=services.profiler,sequence,&captureCursor,&qualityMenu](double dt){
            if(qualityMenu&&qualityMenu->Open())return !s->stopWindow;
            flare::CpuScope scope(p,"CPU.Environment");
            // 第 0 个输出帧取初始化后的 tick 0：先渲染初态，不推进模拟时钟。
            if(sequence&&captureCursor==0)return !s->stopWindow&&flare_session_step(handle,0.0)==FLARE_OK&&!s->stopWindow;
            return !s->stopWindow&&flare_session_step(handle,dt)==FLARE_OK&&!s->stopWindow;};
        services.extract=[s,sequence,interactive,&qualityMenu,&playerWindow,&captureCursor,&captureLog,&gameRenderer](flare::FrameData& data){
            flare::BuildFrameData(s->Env(),&s->UiRef(),s->physicsDebugDraw?&s->physics:nullptr,data);data.assets=s->assets.get();
            flare::PopulateParticleFrame(s->sceneParticles,data);
            if(interactive&&s->playerQuality)flare::ApplyPlayerQuality(s->playerQuality->Level(),data,s->qualityProfiles);
            if(qualityMenu)qualityMenu->Append(playerWindow,data);
            s->appearance.Apply(data);
            if(s->physicsDebugDraw){for(const auto& item:s->characters){const auto lines=item.second.DebugLines();data.debugLines.insert(data.debugLines.end(),lines.begin(),lines.end());}}
            if(sequence&&gameRenderer&&captureCursor<s->captureSequenceCount)
            {
                char name[32]{};std::snprintf(name,sizeof(name),"frame_%04llu.ppm",static_cast<unsigned long long>(captureCursor));
                const auto path=s->captureSequence/name;
                // 宿主的抓帧按更新序号（从 1 起）触发；这里第 i 个输出帧就是第 i+1 次更新。
                gameRenderer->SetCaptureOutput(path,captureCursor+1);
                captureLog.push_back({captureCursor,captureCursor+1,s->Env().FixedStepIndex(),path});
                ++captureCursor;
            }};
        // 普通游戏窗口与编辑器 Play 预览共用同一条前向 + 阴影 + 后处理路径；
        // 场景的 RenderSettings/EnvironmentSettings 组件决定具体开关，不再使用只画 PBR 的演示档。
        SandboxRenderer renderer(SandboxScene::BasicLit,services.resourceDirectory);
        gameRenderer=&renderer;
        if(!s->windowCapture.empty())renderer.SetCaptureOutput(s->windowCapture,s->windowCaptureFrame);
        std::unique_ptr<flare::WindowProfileRenderer> measured;
        if(!s->windowProfile.empty())measured=std::make_unique<flare::WindowProfileRenderer>(renderer,profiler,s->windowProfile);
        Renderer& activeRenderer=measured?static_cast<Renderer&>(*measured):static_cast<Renderer&>(renderer);
        int exitCode=1;
        {
            struct HostUnlock{HostUnlock(){sessionsMutex.unlock();}~HostUnlock(){sessionsMutex.lock();}} unlock;
            exitCode=RunSandboxApp(language,activeRenderer,visible!=0,&services);
        }
        if(measured&&!measured->Finish(exitCode==0||s->stopWindow))return Error(FLARE_INTERNAL_ERROR,"Bridge.ProfileFailed");
        if(exitCode!=0&&!s->stopWindow)return Error(FLARE_INTERNAL_ERROR,"Bridge.WindowFailed");
        if(sequence)
        {
            // 清单只能描述真实落盘的帧：缺帧、少帧都按失败返回，不允许把不完整序列标成成功。
            if(captureLog.size()!=s->captureSequenceCount)return Error(FLARE_INTERNAL_ERROR,"Bridge.CaptureSequenceIncomplete");
            nlohmann::json manifest;manifest["schema"]=1;manifest["count"]=captureLog.size();
            manifest["simulationHz"]=60;manifest["fps"]=fixedStep>0?static_cast<std::uint32_t>(std::lround(1.0/fixedStep)):0;
            manifest["fixedStep"]=fixedStep;
            std::uint32_t width=0,height=0;
            for(const auto& frame:captureLog)
            {
                std::error_code error;const auto bytes=std::filesystem::file_size(frame.path,error);
                if(error||bytes==0)return Error(FLARE_INTERNAL_ERROR,"Bridge.CaptureSequenceIncomplete");
                if(width==0){std::ifstream stream(frame.path,std::ios::binary);std::string magic;std::uint32_t w=0,h=0,max=0;stream>>magic>>w>>h>>max;width=w;height=h;}
                manifest["frames"].push_back({{"index",frame.index},{"updateIndex",frame.updateIndex},{"tick",frame.fixedStep},
                    {"file",frame.path.filename().string()},{"bytes",bytes}});
            }
            manifest["width"]=width;manifest["height"]=height;
            const std::string text=manifest.dump(1);
            const auto temporary=s->captureSequence/"frames.json.tmp";
            {
                std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);
                stream.write(text.data(),static_cast<std::streamsize>(text.size()));
                if(!stream)return Error(FLARE_INTERNAL_ERROR,"Bridge.CaptureManifestFailed");
            }
            std::error_code renameError;
            std::filesystem::rename(temporary,s->captureSequence/"frames.json",renameError);
            if(renameError)return Error(FLARE_INTERNAL_ERROR,"Bridge.CaptureManifestFailed");
        }
        if(!renderer.CaptureSucceeded())return Error(FLARE_INTERNAL_ERROR,"Sample.CaptureMissing");
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_entity_create(FLARE_Session handle, const char* name, uint32_t length, FLARE_Entity* entity) noexcept
{
    return Protect([&]() -> int32_t {
        Session* session = Find(handle); if (!session) return MissingSession();
        if (!entity || length > 4096 || !ValidUtf8(name, length)) return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidUtf8");
        const auto created = session->WorldRef().Create(length == 0 ? std::string{} : std::string(name, length));
        if (!created) return Error(FLARE_INVALID_ARGUMENT, "Bridge.WorldOperationFailed");
        *entity = created.Pack();
        return FLARE_OK;
    });
}
int32_t FLARE_CALL flare_session_set_editor_callback(FLARE_Session handle, FLARE_EditorCallback callback, void* context) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(s->stepping||s->windowRunning)return Error(FLARE_BUSY,"Bridge.SessionBusy");
        s->editorCallback=callback;s->editorContext=callback?context:nullptr;return FLARE_OK;});
}
int32_t FLARE_CALL flare_editor_set_playing(FLARE_Session handle, int32_t playing) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->editor||playing<0||playing>1)return Error(FLARE_INVALID_ARGUMENT,"Bridge.EditorNotRunning");
        s->pendingPlay=playing;return FLARE_OK;});
}
int32_t FLARE_CALL flare_session_set_editor_data(FLARE_Session handle,FLARE_EditorDataCallback callback,void* context) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();if(s->windowRunning||s->stepping)return Error(FLARE_BUSY,"Bridge.SessionBusy");
        s->editorDataCallback=callback;s->editorDataContext=callback?context:nullptr;return FLARE_OK;});
}
int32_t FLARE_CALL flare_editor_inspector_query(FLARE_Session handle,uint64_t entity,char* output,uint32_t capacity,uint32_t* required) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->editor||!s->editorDataCallback)return Error(FLARE_INVALID_ARGUMENT,"Bridge.EditorNotRunning");
        if(!required||(!output&&capacity))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const bool prior=s->stepping;s->stepping=true;
        struct Restore{Session& s;bool prior;~Restore(){sessionsMutex.lock();s.stepping=prior;}} restore{*s,prior};
        sessionsMutex.unlock();const auto result=s->editorDataCallback(handle,entity,nullptr,0,output,capacity,required,s->editorDataContext);
        return result==FLARE_OK||result==FLARE_BUFFER_TOO_SMALL?result:Error(FLARE_CALLBACK_FAILED,"Script.MetadataFailed");});
}
int32_t FLARE_CALL flare_editor_inspector_apply(FLARE_Session handle,uint64_t entity,const char* request,uint32_t length) noexcept
{
    return Protect([&]() -> int32_t {auto* s=Find(handle);if(!s)return MissingSession();
        if(!s->editor||!s->editorDataCallback)return Error(FLARE_INVALID_ARGUMENT,"Bridge.EditorNotRunning");
        if(!request||length>1024*1024||!ValidUtf8(request,length))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidUtf8");
        if(s->editor->IsPlaying())
        {
            bool liveEdit=false;
            if(length>=2&&length<=65536)
            {
                const auto parsed=nlohmann::json::parse(request,request+length,nullptr,false);
                liveEdit=parsed.is_object()&&parsed.value("Live",false);
            }
            if(!liveEdit)return Error(FLARE_BUSY,"Bridge.EditorReadOnly");
        }
        const bool prior=s->stepping;s->stepping=true;
        struct Restore{Session& s;bool prior;~Restore(){sessionsMutex.lock();s.stepping=prior;}} restore{*s,prior};
        sessionsMutex.unlock();uint32_t required=0;const auto result=s->editorDataCallback(handle,entity,request,length,nullptr,0,&required,s->editorDataContext);
        return result==FLARE_OK||result==FLARE_BUFFER_TOO_SMALL?FLARE_OK:Error(FLARE_CALLBACK_FAILED,"Script.FieldValueInvalid");});
}
int32_t FLARE_CALL flare_session_run_editor(FLARE_Session handle,const char* root,uint32_t rootLength,const char* locale,uint32_t localeLength,int32_t visible,uint64_t maximumFrames,int32_t startPlaying) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s=Find(handle);if(!s)return MissingSession();
        if(s->stepping||s->windowRunning)return Error(FLARE_BUSY,"Bridge.SessionBusy");
        if(!root||rootLength==0||rootLength>32768||!ValidUtf8(root,rootLength)||!locale||localeLength!=5||!ValidUtf8(locale,localeLength))return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const std::string language(locale,localeLength);
        if(language!="zh-CN"&&language!="en-US")return Error(FLARE_INVALID_ARGUMENT,"Bridge.InvalidArgument");
        const auto resources=ResourceDirectory();
        const auto sceneFile=EditorEnvironmentPath(L"FLARE_EDITOR_SCENE_FILE"),uiFile=EditorEnvironmentPath(L"FLARE_EDITOR_UI_FILE");
        std::string localizationError;s->localization.Load(resources/"Localization"/(language+".txt"),localizationError);
        struct EditorGuard{Session& s;~EditorGuard(){s.editor=nullptr;s.borrowedWorld=nullptr;s.borrowedEnvironment=nullptr;s.borrowedUi=nullptr;s.windowRunning=false;s.input={};s.pointer={};s.inputActions.Reset();}} guard{*s};
        s->windowRunning=true;s->stopWindow=false;s->pendingPlay=-1;
        bool callbackFailed=false;
        auto clearRuntime=[s]{s->scenePhysics.Clear(s->physics);for(auto body:s->physics.Bodies())s->physics.DestroyBody(body);s->physics.ResetTriggers();s->characterTriggerTracker.Clear();s->bodyOwners.clear();s->characters.clear();s->physicsClock.Reset();s->contactEvents.clear();s->triggerEvents.clear();s->uiEvents.clear();s->sceneAudio.Clear(s->audio);s->audio.StopAll();s->audio.SetPaused(false);s->sceneParticles.Clear();s->sceneNavigation.Clear();s->environmentDriven=s->environmentPhysics=s->pendingEnvironmentRelease=false;s->physics.SetEnvironmentDriven(false);};
        flare::EditorApplicationHooks hooks;hooks.resourceDirectory=resources;hooks.startPlaying=startPlaying!=0;
        hooks.initialize=[s,sceneFile](flare::EditorSession& editor,flare::UiTree& ui){
            s->editor=&editor;ui=s->ui;
            if(!sceneFile.empty())
            {if(!sceneFile.is_absolute()||!editor.Open(sceneFile,true))return false;}
            else if(!editor.Apply("Bridge.EditorInitialize",[s](flare::World& world){world=s->world;return true;}))return false;
            const auto entities=editor.EditWorld().Entities();if(!entities.empty())editor.Select(editor.EditWorld().Get(entities.front())->stableId);return true;
        };
        hooks.configure=[s,sceneFile,uiFile](flare::EditorWorkspaceHost& host){host.SetDocumentPaths(s->editor?s->editor->Path():sceneFile,uiFile);};
        if(s->editorDataCallback)
        {
            hooks.externalDocuments.snapshot=[s,handle](std::vector<flare::EditorExternalDocument>& output,std::string& error){
                nlohmann::json value;
                if(!EditorDocumentRequest(handle,*s,{{"Document","Snapshot"}},true,value,error))return false;
                output.clear();
                if(!value.contains("Schema"))return true; // 兼容只实现旧 Inspector 元数据的宿主。
                try
                {
                    if(value.at("Schema")!=1||value.at("Id")!="script-bindings"||value.at("Kind")!="bindings"
                        ||!value.at("Revision").is_number_unsigned()){error="Script.DocumentInvalid";return false;}
                    flare::EditorExternalDocument item;
                    item.document.documentId=value.at("Id").get<std::string>();item.document.kind=value.at("Kind").get<std::string>();
                    item.document.relativePath=value.at("RelativePath").get<std::string>();item.document.content=value.at("Content").get<std::string>();
                    item.document.sourceDigest=value.at("SourceDigest").get<std::string>();item.document.revision=value.at("Revision").get<std::uint64_t>();
                    item.dirty=value.at("Dirty").get<bool>();output.push_back(std::move(item));return true;
                }
                catch(const std::exception&){error="Script.DocumentInvalid";return false;}
            };
            hooks.externalDocuments.save=[s,handle](const std::string& id,std::string& error){
                if(id!="script-bindings"){error="Script.DocumentInvalid";return false;}
                nlohmann::json response;return EditorDocumentRequest(handle,*s,{{"Document","Save"},{"AllowOverwrite",false}},false,response,error);
            };
            hooks.externalDocuments.restore=[s,handle](const flare::RecoveryDocument& document,bool apply,std::string& error){
                if(document.documentId!="script-bindings"||document.kind!="bindings"){error="Script.DocumentInvalid";return false;}
                nlohmann::json response;
                return EditorDocumentRequest(handle,*s,{{"Document",apply?"Restore":"ValidateRestore"},{"Content",document.content},
                    {"RelativePath",document.relativePath},{"SourceDigest",document.sourceDigest}},false,response,error);
            };
        }
        RuntimeAppearance appearanceBeforePlay;
        hooks.playStarted=[&](flare::World& world,flare::UiTree& ui){
            appearanceBeforePlay=s->appearance;
            clearRuntime();s->inputActions.Reset();
            if(s->assets)
            {
                const auto inputPath=s->assets->SourceRoot()/std::filesystem::path(flare::InputActionsRelativePath);
                std::error_code inputEc;
                if(std::filesystem::exists(inputPath,inputEc))
                {
                    flare::InputActionDocument inputDoc; std::string inputErr;
                    if(!flare::ReadInputActionDocument(inputPath,inputDoc,inputErr)) return false;
                    s->inputActions.SetDocument(std::move(inputDoc));
                }
                else
                {
                    s->inputActions.SetDocument(flare::DefaultInputActionDocument());
                }
            }
            s->borrowedWorld=&world;s->borrowedEnvironment=s->editor->PlayEnvironment();s->borrowedUi=&ui;
            // EditorSession 的 Play 用的是自己的 Environment 实例；不在这里装蒙皮解析器，
            // 姿态系统会因为没有解析器直接返回，角色停在绑定姿态（E18）。
            s->Env().SetSkeletalResolver([s](const std::string& asset)->std::shared_ptr<const flare::StaticModel>{
                if(!s->assets||asset.empty())return nullptr;std::vector<flare::AssetDiagnostic> diagnostics;
                return s->assets->LoadModel(asset,diagnostics);});
            CloseEventPublishers(handle);s->eventSubscriptions.clear();SceneServices(*s,handle);
            const bool ok=!s->editorCallback||s->editorCallback(handle,0,s->editorContext)==0;
            callbackFailed|=!ok;return ok;
        };
        hooks.playStopping=[&](flare::World&,flare::UiTree&){
            ReleaseSessionCursor(*s);s->cursorLocked=false;s->cursorVisible=true;s->cursorLockPausedByEsc=false;s->activeGameWindow=nullptr;s->activeGameScreenRect={};s->mouseDelta={};
            if(s->editorCallback&&s->editorCallback(handle,1,s->editorContext)!=0)callbackFailed=true;
            s->appearance=appearanceBeforePlay;
            clearRuntime();s->inputActions.Reset();s->borrowedWorld=nullptr;s->borrowedEnvironment=nullptr;s->borrowedUi=nullptr;
            CloseEventPublishers(handle);s->eventSubscriptions.clear();
        };
        hooks.syncPlayCursor=[s](void* wnd,const int (&rect)[4]){
            s->activeGameWindow=static_cast<HWND>(wnd);
            s->activeGameScreenRect={rect[0],rect[1],rect[2],rect[3]};
            return s->cursorLocked&&!s->cursorLockPausedByEsc;
        };
        hooks.input=[s](const InputState& input,bool keyboard,bool pointer){CaptureInput(*s,input,keyboard,pointer);};
        hooks.frame=[s](flare::EditorSession& editor,double){
            if(s->stopWindow)return false;
            if(s->pendingPlay!=-1){const auto requested=s->pendingPlay;s->pendingPlay=-1;
                if(requested==0)editor.StopPlay();else if(!editor.IsPlaying()&&!editor.StartPlay())return false;}
            return true;
        };
        hooks.update=[&](flare::World&,flare::UiTree&,double seconds){
            const bool ok=!s->stopWindow&&flare_session_step(handle,seconds)==FLARE_OK&&!s->stopWindow;
            callbackFailed|=s->callbackFailed;
            if(s->pendingPlay==0&&s->editor){s->pendingPlay=-1;s->editor->StopPlay();}
            return ok;
        };
        hooks.singleStep=[&](flare::World&,flare::UiTree&){
            const bool ok=!s->stopWindow&&Protect([&]()->int32_t{
                auto* current=Find(handle);if(!current)return MissingSession();
                return AdvanceSession(*current,handle,current->Env().Clock().Step(),true);
            })==FLARE_OK&&!s->stopWindow;
            callbackFailed|=s->callbackFailed;
            if(s->pendingPlay==0&&s->editor){s->pendingPlay=-1;s->editor->StopPlay();}
            return ok;
        };
        hooks.extract=[s](flare::World& world,flare::UiTree& ui,flare::FrameData& frame){
            // Play 时必须用播放环境重建帧：蒙皮调色板在 Environment 的姿态系统里，
            // 只传 World 会覆盖成空调色板，角色停在绑定姿态（E18）。
            if(s->editor&&s->editor->IsPlaying())
            {
                flare::BuildFrameData(s->Env(),&ui,s->physicsDebugDraw?&s->physics:nullptr,frame);
                flare::PopulateParticleFrame(s->sceneParticles,frame);
                s->appearance.Apply(frame);
            }
            else flare::BuildFrameData(world,&ui,s->physicsDebugDraw?&s->physics:nullptr,frame);
            if(s->physicsDebugDraw){const auto authored=s->scenePhysics.CharacterLines();frame.debugLines.insert(frame.debugLines.end(),authored.begin(),authored.end());for(const auto& item:s->characters){const auto lines=item.second.DebugLines();frame.debugLines.insert(frame.debugLines.end(),lines.begin(),lines.end());}}
        };
        hooks.scriptQuery=[handle](uint64_t entity,std::string& document,std::string& error){
            uint32_t required=0;
            if(flare_editor_inspector_query(handle,entity,nullptr,0,&required)!=FLARE_BUFFER_TOO_SMALL || required>4*1024*1024)
            {error="Script.MetadataFailed";return false;}
            std::vector<char> buffer(required);
            if(flare_editor_inspector_query(handle,entity,buffer.data(),required,&required)!=FLARE_OK)
            {error="Script.MetadataFailed";return false;}
            document.assign(buffer.data());return true;
        };
        hooks.scriptApply=[handle](uint64_t entity,const std::string& command,std::string& error){
            if(flare_editor_inspector_apply(handle,entity,command.data(),static_cast<uint32_t>(command.size()))!=FLARE_OK)
            {error=lastError[0]?std::string(lastError.data()):std::string("Script.FieldValueInvalid");return false;}
            return true;
        };
        if(s->editorDataCallback)
        {
            hooks.drainScriptLogs=[s,handle](std::vector<std::string>& outLogs){
                nlohmann::json value;std::string ignored;
                if(!EditorDocumentRequest(handle,*s,{{"Document","DrainLogs"}},true,value,ignored))return;
                if(value.contains("Logs")&&value["Logs"].is_array())
                    for(const auto& item:value["Logs"])if(item.is_string())outLogs.push_back(item.get<std::string>());
            };
        }
        hooks.closing=[s](flare::EditorSession& editor,flare::UiTree& ui){s->environment.Clear();s->world=editor.EditWorld();s->environment.Scenes().SynchronizeIdle();s->ui=ui;s->editor=nullptr;};
        // 编辑器 Play 也必须解析蒙皮资产：没有解析器时姿态系统直接返回，角色会停在绑定姿态，
        // 于是“编辑器 Play”与“导出运行”在同一 tick 上对不上。
        s->Env().SetSkeletalResolver([s](const std::string& asset)->std::shared_ptr<const flare::StaticModel>{
            if(!s->assets||asset.empty())return nullptr;std::vector<flare::AssetDiagnostic> diagnostics;
            return s->assets->LoadModel(asset,diagnostics);});
        flare::EditorRenderer renderer(resources);int exitCode=1;
        const auto captureFile=EditorEnvironmentPath(L"FLARE_EDITOR_CAPTURE_FILE");
        if(!captureFile.empty())
        {
            const auto captureFrame=EditorEnvironmentPath(L"FLARE_EDITOR_CAPTURE_FRAME").wstring();
            renderer.SetCaptureOutput(captureFile,captureFrame.empty()?30ull:std::stoull(captureFrame));
        }
        {
            struct HostUnlock{HostUnlock(){sessionsMutex.unlock();}~HostUnlock(){sessionsMutex.lock();}} unlock;
            std::u8string rootUtf8(rootLength,u8'\0');std::memcpy(rootUtf8.data(),root,rootLength);
            exitCode=flare::RunEditorApp(renderer,language,std::filesystem::path(rootUtf8),visible==0,maximumFrames,&hooks);
        }
        if(callbackFailed)return Error(FLARE_CALLBACK_FAILED,"Bridge.CallbackFailed");
        return (exitCode==0||s->stopWindow)&&renderer.CaptureSucceeded()?FLARE_OK:Error(FLARE_INTERNAL_ERROR,"Bridge.WindowFailed");
    });
}
