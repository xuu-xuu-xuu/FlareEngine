#pragma once

#include "ToolLayer/Editor/EditorToolkitForms.h"
#include "ToolLayer/Editor/EditorFloatingWindows.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "ToolLayer/Editor/EditorPlayInput.h"
#include "ToolLayer/Editor/EditorSessionStore.h"
#include "ToolLayer/Editor/ViewportDropWorkflow.h"
#include "ToolLayer/Editor/EditorViewportCamera.h"
#include "ToolLayer/Editor/EditorUi/EditorInputAdapter.h"
#include "FunctionLayer/Audio/SceneAudio.h"
#include "FunctionLayer/Navigation/SceneNavigation.h"
#include "FunctionLayer/Particles/SceneParticles.h"
#include "FunctionLayer/Physics/ScenePhysics.h"
#include "ToolLayer/Editor/NavigationBakeService.h"
#include "PlatformLayer/Windows/XAudio2Backend.h"
#include "PlatformLayer/Windows/UiDirectWriteTextService.h"
#include "PlatformLayer/Windows/Window.h"

#include <Windows.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace flare
{
struct ToolkitSceneImageProvider final : ui::UiImageProvider
{
    std::unordered_map<std::string, ui::UiImageInfo> images;
    bool Resolve(std::string_view image, ui::UiImageInfo& info) const override
    {
        if (image == "scene")
        {
            info.textureToken = 2;
            return true;
        }
        const auto found = images.find(std::string(image));
        if (found == images.end()) return false;
        info = found->second;
        return true;
    }
};

struct ToolkitAppState
{
    std::filesystem::path root;
    std::filesystem::path resourceDirectory;
    std::filesystem::path assetsRoot;
    std::filesystem::path crashDirectory;
    Localization text;
    AssetStore assets;
    EditorSession session;
    UiTree ui;
    std::unique_ptr<UiTree> playUi;
    std::unique_ptr<EditorPlayInput> playInput;
    PhysicsWorld authoredPhysics;
    ScenePhysics authoredScenePhysics;
    std::unique_ptr<XAudio2Backend> audioBackend;
    AudioWorld audio;
    SceneAudio sceneAudio;
    SceneParticles sceneParticles;
    SceneNavigation sceneNavigation;
    NavigationBakeService navigationBake;
    EditorWorkspaceHost host;
    EditorScriptWorkflow scriptWorkflow;
    EditorViewportCamera editorCamera;
    EditorViewportController viewportController;
    ViewportDropWorkflow dropWorkflow;
    EditorClipPreview clipPreview;
    ExternalChangeMonitor externalChange;
    std::unique_ptr<ui::editor::EditorShell> shell;
    std::unique_ptr<EditorFeatureTools> featureTools;
    std::unique_ptr<ui::editor::EditorInputAdapter> input;
    std::unique_ptr<EditorFloatingWindows> floatingWindows;
    std::unique_ptr<UiDirectWriteTextService> fontService;
    std::vector<UiDirectWriteTextService::FontPageRgba> fontPages;
    ToolkitAssetInspectorState assetState;
    std::optional<EditorWorkspaceHost::DocumentContext> restoredContext;
    std::optional<CameraSnapshot> camera;
    ui::UiRect viewportRect{};
    ui::UiRect playViewportRect{};
    ui::editor::EditorPanelId prePlayGameGroupTab = ui::editor::EditorPanelId::None;
    bool enteredMaximizedOnPlay = false;
    bool pendingActivatePlayInput = false;
    bool pendingPlayAfterCompile = false;
    bool initialScriptCheckDone = false;
    HWND hostWindow = nullptr;
    HWND nativeMoveWindow = nullptr;
    EditorApplicationHooks* hooks = nullptr;
    bool safeMode = false;
    bool hidden = false;
    bool dpiOverride = false;
    bool gameplayLogged = false;
    bool closeApproved = false;
    bool extractLogged = false;
    bool exportBusy = false;
    bool exportStarted = false;
    bool exportFinished = false;
    bool autoExport = false;
    bool autoExportStarted = false;
    bool autoExportFlow = false;
    int autoExportFlowPhase = 0;
    bool autoCancelRequested = false;
    int autoCancelMs = 0;
    std::uint64_t autoExportStart = 0;
    std::uint64_t editorFrame = 0;
    StableEntityId hoverMaterialEntity = 0;
    AssetId hoverMaterialAssetId;
    std::array<float, 4> loggedViewport{-1.0f, -1.0f, -1.0f, -1.0f};
    std::string exportName = "GameExport";
    std::string pendingAsset;
    std::string pendingRenameAssetPath;
    std::string importStatus;
    std::string transientStatus;
    std::uint64_t transientStatusUntilTick = 0;
    std::string lastWindowTitle;
    std::optional<ui::editor::EditorCommandId> pendingDialogCommand;
    std::uint32_t pendingFocusSurfaceId = 0;
    HWND gameSurfaceWindow = nullptr;
    std::unique_ptr<SandboxRenderer> gameRenderer;
    std::uint64_t gameUpdateIndex = 0;
    std::uint64_t gameImageRevision = 0;
    std::uint64_t lastGameRenderKey = 0;
    std::uint64_t lastGameRenderTick = 0;
    bool interactiveResizing = false;
    bool prevSplitterActive = false;
    std::uint64_t lastInteractiveResizeTick = 0;
    bool assetsListDirty = true;
    std::uint64_t lastAssetScanTick = 0;
    std::size_t lastAssetIdCount = 0;
    std::size_t lastScriptSourceCount = 0;
    std::vector<ui::editor::EditorAssetEntry> cachedAssetEntries;
    struct CachedAssetThumb
    {
        std::uint64_t token = 0;
        std::shared_ptr<ImageData> image;
        std::filesystem::file_time_type mtime{};
    };
    std::unordered_map<std::string, CachedAssetThumb> assetThumbnails;
    std::uint64_t nextAssetThumbToken = 0x180000000ull;
    std::future<ToolkitImportResult> importTask;
    std::function<bool()> exportAction;
    std::function<bool()> autoExportFn;
    std::function<void(std::vector<std::filesystem::path>)> openImportConfirm;
    EditorSessionSnapshot observedSession;
    EditorSessionSnapshot persistedSession;
    std::chrono::steady_clock::time_point sessionChangedAt;

    ToolkitAppState(const std::filesystem::path& projectRoot,
                    const std::filesystem::path& contentRoot,
                    const std::filesystem::path& cacheRoot,
                    EditorApplicationHooks* appHooks,
                    Localization loc)
        : root(projectRoot),
          assetsRoot(contentRoot),
          text(std::move(loc)),
          assets(contentRoot, cacheRoot),
          scriptWorkflow(projectRoot, appHooks, text),
          viewportController(editorCamera),
          hooks(appHooks)
    {
        audioBackend = std::make_unique<XAudio2Backend>();
        if (audioBackend->Initialize()) audio.SetBackend(audioBackend.get());
        else audioBackend.reset();
    }
    ~ToolkitAppState()
    {
        if (gameRenderer)
        {
            gameRenderer->Shutdown();
            gameRenderer.reset();
        }
        if (gameSurfaceWindow)
        {
            DestroyWindow(gameSurfaceWindow);
            gameSurfaceWindow = nullptr;
        }
        sceneNavigation.Clear();
        sceneParticles.Clear();
        sceneAudio.Clear(audio);
        audio.Shutdown();
        if (audioBackend) audioBackend->Shutdown();
    }
};

[[nodiscard]] EditorSessionSnapshot CaptureEditorSessionSnapshot(const std::filesystem::path& root,
                                                                 const EditorSession& session,
                                                                 const EditorViewportCamera& camera,
                                                                 const EditorWorkspaceHost& host);

void PersistToolkitSession(ToolkitAppState& state, bool force);

bool InitializeToolkitWindowUi(ToolkitAppState& state, HWND window);

void FinalizeToolkitWindowUi(ToolkitAppState& state);

void BindToolkitImportAndCommands(ToolkitAppState& state, ToolkitWorkspaceContext& ctx);

void ConfigureToolkitRecoveryPrompt(ToolkitAppState& state);

std::optional<LRESULT> HandleToolkitWindowMessage(ToolkitAppState& state, HWND window, UINT message,
                                                  WPARAM wParam, LPARAM lParam);

void ExtractToolkitFrameData(ToolkitAppState& state, FrameData& frame);

} // namespace flare
