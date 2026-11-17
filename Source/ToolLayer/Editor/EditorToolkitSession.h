#pragma once

#include "ToolLayer/Editor/EditorToolkitForms.h"
#include "ToolLayer/Editor/EditorFloatingWindows.h"
#include "ToolLayer/Editor/EditorPlayInput.h"
#include "ToolLayer/Editor/EditorSessionStore.h"
#include "ToolLayer/Editor/EditorViewportCamera.h"
#include "ToolLayer/Editor/EditorUi/EditorInputAdapter.h"
#include "FunctionLayer/Physics/ScenePhysics.h"
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

namespace se
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
    EditorWorkspaceHost host;
    EditorScriptWorkflow scriptWorkflow;
    EditorViewportCamera editorCamera;
    EditorViewportController viewportController;
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
    std::array<float, 4> loggedViewport{-1.0f, -1.0f, -1.0f, -1.0f};
    std::string exportName = "GameExport";
    std::string pendingAsset;
    std::string importStatus;
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

} // namespace se
