#include "EditorApplication.h"
#include "EditorToolkitSession.h"
#include "EditorSessionStore.h"
#include "ToolLayer/Editor/EditorUi/EditorViewportMath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ToolLayer/Tools/Project.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "PlatformLayer/Windows/StartupProgress.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "CoreLayer/Log.h"
#if defined(FLARE_LOCAL_VALIDATION)
#include "EditorSmokeDriver.h"
#endif

#include <Windows.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace se
{
namespace
{
bool SameProject(const std::string& stored, const std::filesystem::path& root)
{
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    return stored == asset_io::Utf8(canonical);
}

std::filesystem::path ResolveResourceDirectory(const std::filesystem::path& root,
                                               const EditorApplicationHooks* hooks)
{
    auto directory = root / "Resources";
    if (hooks && !hooks->resourceDirectory.empty()) directory = hooks->resourceDirectory;
    if (!std::filesystem::is_directory(directory / "Localization"))
        directory = GetExecutableDirectory() / "Resources";
    return directory;
}

bool LoadEditorLocalization(const std::filesystem::path& resourceDirectory,
                            const std::string& locale,
                            Localization& text)
{
    std::string error;
    if (!text.Load(resourceDirectory / "Localization" / (locale + ".txt"), error))
    {
        Log(LogLevel::Warning, "Language " + locale + ": " + error);
        if (locale != "en-US") text.Load(resourceDirectory / "Localization" / "en-US.txt", error);
    }
    return true;
}

bool ResolveAssetsRoot(const std::filesystem::path& root,
                       const Localization& text,
                       std::filesystem::path& assetsRoot)
{
    assetsRoot = std::filesystem::is_directory(root / "Content") ? root / "Content"
        : std::filesystem::is_directory(root / "Assets") ? root / "Assets"
        : std::filesystem::is_directory(root / "Resources") ? root / "Resources" : root;
    if (!std::filesystem::exists(root / "project.json")) return true;
    ProjectDescription project;
    std::vector<ProjectDiagnostic> failures;
    if (!ReadProject(root / "project.json", project, failures))
    {
        for (const auto& failure : failures) Log(LogLevel::Error, text.GetText(failure.key) + ": " + failure.detail);
        return false;
    }
    assetsRoot = project.contentRoot;
    return true;
}

void ConfigureSessionAndWorkspace(ToolkitAppState& state,
                                  const std::string& locale,
                                  bool hidden,
                                  const EditorPreferences& loadedPreferences,
                                  bool hasRestoredSession,
                                  const EditorSessionSnapshot& restoredSession)
{
    state.session.SetTODResolver([&state](const std::string& id, std::string& error) {
        std::vector<AssetDiagnostic> failures;
        auto profile = state.assets.LoadTODProfile(id, failures);
        if (!profile) error = failures.empty() ? "TOD.ResolveFailed" : failures.back().key;
        return profile;
    });
    const bool managedDefaultScene = GetEnvironmentVariableW(L"SELFENGINE_EDITOR_SCENE_FILE", nullptr, 0) > 0 &&
        GetEnvironmentVariableW(L"SELFENGINE_EDITOR_SCENE_EXPLICIT", nullptr, 0) == 0;
    if (hasRestoredSession && (state.session.Path().empty() || managedDefaultScene) && !restoredSession.documents.empty())
    {
        for (const auto& document : restoredSession.documents)
        {
            if (!document.ends_with(".scene.json")) continue;
            const auto requested = asset_io::FromUtf8(document);
            const auto path = requested.is_absolute() ? requested : state.root / requested;
            if (!asset_io::Within(state.root, path) || !state.session.Open(path, true))
                Log(LogLevel::Warning, "Editor.SessionDocumentMissing");
            break;
        }
    }
    state.session.SetPlayCallbacks(
        [&state](World& world) {
            state.sceneNavigation.Clear();
            state.authoredScenePhysics.Clear(state.authoredPhysics);
            state.session.PlayEnvironment()->SetSkeletalResolver([&state](const std::string& asset) {
                std::vector<AssetDiagnostic> diagnostics;
                return state.assets.LoadModel(asset, diagnostics);
            });
            if (!state.hooks || !state.hooks->update)
            {
                state.session.PlayEnvironment()->SetPhysicsService([&state](double seconds) {
                    if (!state.session.PlayWorld()) return false;
                    auto& playWorld = *state.session.PlayWorld();
                    const float dt = static_cast<float>(seconds);
                    if (!state.authoredScenePhysics.Synchronize(playWorld, state.authoredPhysics)) return false;
                    if (!state.authoredPhysics.Step(dt)) return false;
                    if (!state.authoredScenePhysics.ApplyTransforms(playWorld, state.authoredPhysics)) return false;
                    state.sceneNavigation.StepAgents(
                        playWorld, state.authoredPhysics, state.authoredScenePhysics, &state.assets, dt);
                    return state.authoredScenePhysics.AdvanceCharacters(playWorld, state.authoredPhysics, dt);
                });
            }
            state.playUi = std::make_unique<UiTree>(state.ui);
            state.playUi->CancelInput();
            if (state.playInput) state.playInput->Attach(state.playUi.get());
            return !state.hooks || !state.hooks->playStarted || state.hooks->playStarted(world, *state.playUi);
        },
        [&state](World& world) {
            state.sceneNavigation.Clear();
            state.authoredScenePhysics.Clear(state.authoredPhysics);
            if (state.playInput) state.playInput->Attach(nullptr);
            struct ClearUi { std::unique_ptr<UiTree>& value; ~ClearUi() { value.reset(); } } clearUi{state.playUi};
            if (state.hooks && state.hooks->playStopping && state.playUi) state.hooks->playStopping(world, *state.playUi);
        });

    if (hasRestoredSession)
    {
        if (std::isfinite(restoredSession.cameraX) && std::isfinite(restoredSession.cameraY) &&
            std::isfinite(restoredSession.cameraZ) && std::isfinite(restoredSession.cameraYaw) &&
            std::isfinite(restoredSession.cameraPitch))
        {
            state.editorCamera.state.position = {restoredSession.cameraX, restoredSession.cameraY, restoredSession.cameraZ};
            state.editorCamera.state.yaw = restoredSession.cameraYaw;
            state.editorCamera.state.pitch = restoredSession.cameraPitch;
        }
        if (!restoredSession.selection.empty()) state.session.Select(restoredSession.selection.front());
    }

    state.playInput = std::make_unique<EditorPlayInput>(
        [&state](UiPoint point) {
            if (!state.shell) return false;
            const float dpi = state.input ? state.input->DpiScale() : 1.0f;
            const auto hit = ui::UiHitTest(state.shell->Document(), state.shell->Layout())
                .Hit(state.shell->CurrentInputRoot(), point.x / dpi, point.y / dpi).target;
            return hit == state.shell->ViewportPanel();
        },
        [&state]() { if (state.shell) state.shell->FocusPanel(ui::editor::EditorPanelId::Viewport); },
        [&state]() {
            return state.shell && (state.shell->CloseModalOpen() || state.shell->ConfirmOpen() ||
                                   state.shell->MenuOpen() || state.shell->ColorPickerOpen());
        });

    if (state.hooks) state.host.SetExternalDocuments(state.hooks->externalDocuments);
    state.host.SetProjectRoot(state.root);
    EditorPreferences preferences = loadedPreferences;
    preferences.language = locale;
    state.host.ConfigurePreferences(preferences, EditorSessionStore::UserDataDirectory(),
        [&state](const std::string& language) {
            std::string failure;
            return state.text.Load(state.resourceDirectory / "Localization" / (language + ".txt"), failure);
        });
    state.host.SetDocumentPaths(state.assetsRoot / "Scenes" / "Untitled.scene.json",
                                state.assetsRoot / "UI" / "Untitled.ui.json");
    if (state.hooks && state.hooks->configure) state.hooks->configure(state.host);
    if (hasRestoredSession)
    {
        state.restoredContext = state.host.RestoreSessionDocuments(restoredSession.documents, state.assets,
                                                                   state.ui, restoredSession.activeDocument);
    }
    state.host.ConfigureRecovery(state.crashDirectory, !state.safeMode && !hidden);
    state.host.ConfigureBuildControl();
    state.observedSession = CaptureEditorSessionSnapshot(state.root, state.session, state.editorCamera, state.host);
    state.persistedSession = hasRestoredSession ? restoredSession : EditorSessionSnapshot{};
    state.sessionChangedAt = std::chrono::steady_clock::now();
}
} // namespace

int RunEditorApp(Renderer& renderer, const std::string& locale, const std::filesystem::path& requestedRoot,
                 bool hidden, std::uint64_t maximumFrames, EditorApplicationHooks* hooks)
{
    struct CrashMarkerGuard
    {
        std::filesystem::path directory;
        bool active = false;
        ~CrashMarkerGuard()
        {
            if (active) { std::string error; EditorSessionStore::ClearCrashMarker(directory, error); }
        }
    } crashGuard;

    try
    {
        const auto root = std::filesystem::weakly_canonical(requestedRoot);
        if (!std::filesystem::is_directory(root)) { Log(LogLevel::Error, "EditorUi.InvalidProject"); return 1; }
#if defined(FLARE_LOCAL_VALIDATION)
        auto smoke = EditorSmokeDriver::FromEnvironment(root);
#endif
        crashGuard.directory = EditorSessionStore::ProjectDirectory(root);
        EditorSessionStore::CrashMarker previous;
        std::string startupError;
        const auto startup = EditorSessionStore::InspectStartup(crashGuard.directory, previous, startupError);
        if (startup == EditorSessionStore::StartupState::ActiveInstance) { Log(LogLevel::Warning, "Editor.AnotherInstance"); return 1; }
        if (startup == EditorSessionStore::StartupState::UncleanExit) Log(LogLevel::Warning, "Editor.UncleanExit");
        std::string cleanupError;
        if (!EditorSessionStore::CleanupAbandonedCandidates(crashGuard.directory, cleanupError)) Log(LogLevel::Warning, cleanupError);
        EditorSessionStore::CrashMarker marker;
        marker.processId = GetCurrentProcessId();
        marker.processCreation = EditorSessionStore::CurrentProcessCreation();
        std::string markerError;
        crashGuard.active = EditorSessionStore::WriteCrashMarker(crashGuard.directory, marker, markerError);
        if (!crashGuard.active) Log(LogLevel::Warning, markerError);

        const bool safeMode = GetEnvironmentVariableW(L"SELFENGINE_EDITOR_SAFE_MODE", nullptr, 0) > 0;
        EditorPreferences preferences;
        std::string preferencesError;
        EditorSessionStore::LoadPreferences(EditorSessionStore::UserDataDirectory(), preferences, preferencesError);

        const auto resourceDirectory = ResolveResourceDirectory(root, hooks);
        Localization text;
        LoadEditorLocalization(resourceDirectory, locale, text);
        std::filesystem::path assetsRoot;
        if (!ResolveAssetsRoot(root, text, assetsRoot)) return 1;
        if (!ReportStartup(StartupStage::Assets)) return 1;

        ToolkitAppState state(root, assetsRoot, root / "out" / "Cache" / "Assets", hooks, std::move(text));
        state.resourceDirectory = resourceDirectory;
        state.crashDirectory = crashGuard.directory;
        state.safeMode = safeMode;
        state.hidden = hidden;

        std::vector<AssetDiagnostic> diagnostics;
        if (!state.assets.Scan(diagnostics))
        {
            for (const auto& diagnostic : diagnostics)
                Log(LogLevel::Error, state.text.GetText(diagnostic.key) + ": " + diagnostic.detail);
            return 1;
        }
        Log(LogLevel::Info, "EditorUi.Assets: " + std::to_string(state.assets.ListIds().size()));

        EditorSessionSnapshot restoredSession;
        std::string sessionError;
        const bool hasRestoredSession = !safeMode &&
            EditorSessionStore::LoadSession(crashGuard.directory, restoredSession, sessionError) &&
            SameProject(restoredSession.project, root);
        if (!ReportStartup(StartupStage::Scene)) return 1;
        if (hooks && hooks->initialize && !hooks->initialize(state.session, state.ui)) return 1;
        ConfigureSessionAndWorkspace(state, locale, hidden, preferences, hasRestoredSession, restoredSession);

        struct PlayLifetime
        {
            EditorSession& session;
            ~PlayLifetime() { session.StopPlay(); session.SetPlayCallbacks({}, {}); }
        } playLifetime{state.session};

        std::unique_ptr<ToolkitWorkspaceContext> workspaceContext;
        ApplicationServices services;
        services.resourceDirectory = resourceDirectory;
        services.maximumFrames = maximumFrames;
        services.initialWidth = 1280;
        services.initialHeight = 760;
        services.liveWindowResize = true;
        services.minimumClientWidthDip = 320;
        services.minimumClientHeightDip = 240;

        services.messageHandler = [&state](HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
            return HandleToolkitWindowMessage(state, window, message, wParam, lParam);
        };
        services.windowOpened = [&state, &workspaceContext, &services, hidden](HWND window) {
            if (!InitializeToolkitWindowUi(state, window)) return false;
            state.floatingWindows = std::make_unique<EditorFloatingWindows>(window, *state.shell, state.text,
                state.resourceDirectory,
                [&services](HWND target, UINT message, WPARAM w, LPARAM l) { return services.messageHandler(target, message, w, l); },
                !hidden);
            workspaceContext = std::make_unique<ToolkitWorkspaceContext>(ToolkitWorkspaceContext{
                state.root, state.assetsRoot, state.assets, state.session, state.host, state.ui,
                state.scriptWorkflow, state.clipPreview, state.viewportController, state.externalChange,
                state.text, *state.shell, state.featureTools.get(), state.assetState,
                state.exportName, state.importStatus, state.importTask, state.hooks, &state.audio});
            BindToolkitImportAndCommands(state, *workspaceContext);
            BindToolkitWorkspaceForms(*workspaceContext, state.exportAction);
            FinalizeToolkitWindowUi(state);
            return true;
        };
        services.windowClosing = [&state]() {
            state.playInput->Deactivate(state.hostWindow);
            state.navigationBake.CancelAndWait();
            state.sceneNavigation.Clear();
            state.sceneParticles.Clear();
            state.sceneAudio.Clear(state.audio);
            state.audio.StopAll();
            state.assetState.previewAudioVoice = {};
            state.closeApproved = state.shell && state.shell->CloseApproved();
            Log(LogLevel::Info, "EditorUi.ToolkitClosing");
            state.viewportController.Blur(state.session);
            state.featureTools.reset();
            state.floatingWindows.reset();
            state.input.reset();
            state.shell.reset();
            state.fontService.reset();
            state.fontPages.clear();
            state.hostWindow = nullptr;
        };
        services.captureKeyboard = []() { return false; };
        services.capturePointer = []() { return false; };
        services.input = [&state](const InputState& input, bool keyboard, bool pointer) {
            if (state.session.IsPlaying() && !state.gameplayLogged)
            {
                bool hasGameplayInput = !input.pointerEvents.empty();
                for (bool down : input.keysDown) hasGameplayInput = hasGameplayInput || down;
                if (hasGameplayInput) { state.gameplayLogged = true; Log(LogLevel::Info, "EditorUi.ToolkitGameplayInput"); }
            }
            if (state.hooks && state.hooks->input && state.session.IsPlaying())
            {
                float dpi = state.input ? state.input->DpiScale() : 1.0f;
                const auto* floatingInput = state.floatingWindows ? state.floatingWindows->GameplayInput(dpi) : nullptr;
                InputState gameplay = floatingInput ? *floatingInput : input;
                if (floatingInput)
                {
                    gameplay.gamepads = input.gamepads;
                }
                const ui::editor::ViewportRectF viewportMath{state.viewportRect.x, state.viewportRect.y,
                                                             state.viewportRect.width, state.viewportRect.height};
                float mouseX = static_cast<float>(gameplay.mouseX), mouseY = static_cast<float>(gameplay.mouseY);
                ui::editor::ViewportOffsetPlayInput(viewportMath, dpi, mouseX, mouseY);
                gameplay.mouseX = static_cast<int>(std::lround(mouseX));
                gameplay.mouseY = static_cast<int>(std::lround(mouseY));
                for (auto& event : gameplay.pointerEvents)
                {
                    float ex = static_cast<float>(event.x), ey = static_cast<float>(event.y);
                    ui::editor::ViewportOffsetPlayInput(viewportMath, dpi, ex, ey);
                    event.x = static_cast<int>(std::lround(ex));
                    event.y = static_cast<int>(std::lround(ey));
                }
                state.hooks->input(gameplay, keyboard, pointer);
            }
            state.playInput->EndFrame();
        };
        services.runEnvironmentFrame = [&state](double seconds) {
            if (state.featureTools) state.featureTools->Poll();
            state.navigationBake.Poll();
            if (state.floatingWindows && !state.nativeMoveWindow)
            {
                state.viewportController.Update(seconds, state.session, state.assets);
                const bool uiCaptured = state.shell && state.shell->Dispatcher().PointerCapture().IsValid();
                if (!state.nativeMoveWindow && !state.viewportController.Captured() && !uiCaptured &&
                    (GetCapture() == state.hostWindow || state.floatingWindows->Owns(GetCapture())))
                {
                    ReleaseCapture();
                }
            }
            if (state.host.TimeOfDayPreview().Ready() && !state.host.TimeOfDayPreview().Advance(seconds)) return false;
            if (state.hooks && state.hooks->frame && !state.hooks->frame(state.session, seconds)) return false;
            state.host.UpdateRecovery(state.session, &state.ui,
                !state.shell || state.shell->Dispatcher().PointerCapture().IsValid() || state.shell->TextFieldFocused());
            state.ui.Update(seconds);
            if (state.session.IsPlaying() && state.playUi)
            {
                if (state.session.ConsumePlayStep())
                {
                    if (state.hooks && state.hooks->singleStep)
                    {
                        if (!state.hooks->singleStep(*state.session.PlayWorld(), *state.playUi)) return false;
                    }
                    else
                    {
                        state.playUi->Update(state.session.PlayFixedStep());
                        if (!state.session.StepPlayFrame()) return false;
                        state.sceneParticles.Synchronize(*state.session.PlayWorld(), state.session.PlayFixedStep(), false);
                    }
                }
                else if (state.hooks && state.hooks->update)
                {
                    if (!state.hooks->update(*state.session.PlayWorld(), *state.playUi, seconds)) return false;
                }
                else
                {
                    if (!state.session.PlayPaused()) state.playUi->Update(seconds);
                    if (!state.session.PlayEnvironment()->RunFrame(seconds)) return false;
                    state.sceneParticles.Synchronize(*state.session.PlayWorld(),
                        state.session.PlayPaused() ? 0.0 : seconds, state.session.PlayPaused());
                }
                state.session.CompletePlayFrame();
            }
            return true;
        };
        services.extract = [&state](FrameData& frame) {
#if defined(FLARE_LOCAL_VALIDATION)
            // 由 EditorApplication 主提取前先推进自动化烟测命令，保持单线程时序。
#endif
            ExtractToolkitFrameData(state, frame);
        };
#if defined(FLARE_LOCAL_VALIDATION)
        if (smoke.Enabled())
        {
            services.extract = [&state, &smoke](FrameData& frame) {
                if (state.shell)
                {
                    smoke.OperationFrame(state.host, state.session, [&state](std::string_view id) {
                        std::optional<ui::editor::EditorCommandId> command;
                        if (id == "editor.play")
                        {
                            std::string reason;
                            if (!state.session.ValidatePlay(reason)) return EditorCommandOutcome::Rejected(reason);
                            command = ui::editor::EditorCommandId::Play;
                        }
                        else if (id == "editor.pause" || id == "editor.resume")
                        {
                            if (!state.session.IsPlaying()) return EditorCommandOutcome::Rejected("Editor.PlaybackInvalidState");
                            if (state.session.PlayPaused() == (id == "editor.pause")) return EditorCommandOutcome::Succeeded();
                            command = ui::editor::EditorCommandId::Pause;
                        }
                        else if (id == "editor.step") command = ui::editor::EditorCommandId::Step;
                        else if (id == "editor.stop") command = ui::editor::EditorCommandId::Stop;
                        else if (id == "editor.save-all") command = ui::editor::EditorCommandId::SaveAll;
                        else if (id == "editor.compile") command = ui::editor::EditorCommandId::Build;
                        if (!command) return EditorCommandOutcome::Rejected("Editor.CommandUnknown", std::string(id));
                        return state.shell->Commands().Execute({*command, {}}) ? EditorCommandOutcome::Succeeded()
                            : EditorCommandOutcome::Failed("Editor.PlaybackInvalidState");
                    }, [&state]() { PostMessageW(state.hostWindow, WM_CLOSE, 0, 0); });
                }
                ExtractToolkitFrameData(state, frame);
            };
        }
#endif

        EnableEngineDpiAwareness();
        if (hooks && hooks->startPlaying && !state.session.StartPlay()) return 1;
        const int result = RunSandboxApp(locale, renderer, !hidden, &services);
        Log(LogLevel::Info, std::string("EditorUi.ToolkitResult: ") + std::to_string(result));
        state.session.StopPlay();
        if (hooks && hooks->closing) hooks->closing(state.session, state.ui);
        PersistToolkitSession(state, true);
        Log(LogLevel::Info, "EditorUi.ToolkitPersisted");
        state.host.FinishRecovery(result == 0 && state.closeApproved);
        if (result == 0 && !state.host.PublishRestartRequest(state.closeApproved)) return 1;
#if defined(FLARE_LOCAL_VALIDATION)
        return smoke.FinalResult(result);
#else
        return result;
#endif
    }
    catch (const std::exception& exception)
    {
        Log(LogLevel::Error, std::string("EditorUi.StartFailed: ") + exception.what());
        return 1;
    }
}
} // namespace se
