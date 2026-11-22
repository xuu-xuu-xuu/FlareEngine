#include "EditorToolkitSession.h"
#include "EditorApplication.h"
#include "EditorUiAtlas.h"
#include "EditorWindowStyle.h"
#include "AssetImportService.h"
#include "BuildRestartRequest.h"
#include "EditorAssetActions.h"
#include "EditorCreation.h"
#include "EditorScriptTools.h"
#include "MaterialThumbnail.h"
#include "PrefabWorkflow.h"
#include "SceneInspector.h"
#include "TodInspector.h"
#include "UiInspector.h"
#include "ToolLayer/Editor/EditorUi/EditorViewportMath.h"
#include "ToolLayer/Tools/Project.h"
#include "FunctionLayer/Application/FeatureFrameData.h"
#include "FunctionLayer/Runtime/Camera.h"
#include "FunctionLayer/Runtime/FeatureComponents.h"
#include "CoreLayer/Log.h"
#include "PlatformLayer/Windows/StartupProgress.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Scenes/PrefabDocument.h"
#include "ResourceLayer/Assets/MaterialMetadata.h"
#include "ResourceLayer/Assets/ShaderGraph.h"
#include "ResourceLayer/Assets/SurfaceShader.h"

#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

namespace flare
{
using ui::editor::EditorCommand;
using ui::editor::EditorCommandId;
using ui::editor::EditorInputAdapter;
using ui::editor::EditorPanelFromName;
using ui::editor::EditorPanelId;
using ui::editor::EditorShell;

namespace
{
void NotifyToolkitStatus(ToolkitAppState& state, const std::string& message, bool logConsole = true)
{
    state.assetsListDirty = true;
    state.transientStatus = message;
    state.transientStatusUntilTick = GetTickCount64() + 3500;
    if (state.shell)
    {
        state.shell->SetStatus(message);
        if (logConsole && !message.empty()) state.shell->AppendConsoleLine(message);
    }
}

void ClearToolkitAssetSelectionState(ToolkitAppState& state)
{
    state.assetsListDirty = true;
    state.assetState.inspectUi = false;
    state.assetState.editTod = false;
    state.assetState.assetName.clear();
    state.assetState.assetPath.clear();
    state.assetState.targetPath.clear();
    state.assetState.rows.clear();
    state.assetState.modelId.clear();
    state.assetState.previewImage.reset();
    state.assetState.previewImageKey.clear();
    state.assetState.previewToken = 0;
    if (state.shell) state.shell->SetAssetPreviewImage({}, 0.0f, 0.0f);
}

bool IsUtilityPanel(EditorPanelId panel) noexcept
{
    switch (panel)
    {
    case EditorPanelId::ProjectSettings:
    case EditorPanelId::Preferences:
    case EditorPanelId::BuildExport:
    case EditorPanelId::AssetTools:
    case EditorPanelId::Material:
    case EditorPanelId::About:
    case EditorPanelId::Diagnostics:
        return true;
    default:
        return false;
    }
}

void OpenToolkitUtilityWindow(ToolkitAppState& state, EditorPanelId panel)
{
    if (!state.shell) return;
    if (!IsUtilityPanel(panel) || !state.floatingWindows || state.hidden || state.hooks != nullptr ||
        GetEnvironmentVariableW(L"FLARE_EDITOR_OPEN_PANEL", nullptr, 0) > 0)
    {
        state.shell->TogglePanel(panel);
        return;
    }
    const std::uint32_t existingSurface = state.shell->Dock().SurfaceFor(panel);
    if (existingSurface != 0)
    {
        state.shell->FocusPanel(panel);
        state.pendingFocusSurfaceId = existingSurface;
        if (HWND fw = state.floatingWindows->Window(existingSurface))
        {
            if (IsIconic(fw)) ShowWindow(fw, SW_RESTORE);
            else ShowWindow(fw, SW_SHOWNORMAL);
            SetForegroundWindow(fw);
        }
        return;
    }
    int dipW = 680;
    int dipH = 480;
    if (panel == EditorPanelId::ProjectSettings || panel == EditorPanelId::Preferences)
    {
        dipW = 780;
        dipH = 540;
    }
    else if (panel == EditorPanelId::Material)
    {
        dipW = 540;
        dipH = 600;
    }
    else if (panel == EditorPanelId::About || panel == EditorPanelId::Diagnostics)
    {
        dipW = 520;
        dipH = 360;
    }
    const UINT dpi = state.hostWindow ? GetDpiForWindow(state.hostWindow) : 96u;
    const float scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    const int physW = static_cast<int>(std::lround(static_cast<float>(dipW) * scale));
    const int physH = static_cast<int>(std::lround(static_cast<float>(dipH) * scale));
    RECT hostRect{100, 100, 1380, 860};
    if (state.hostWindow) GetWindowRect(state.hostWindow, &hostRect);
    const int cx = (hostRect.left + hostRect.right - physW) / 2;
    const int cy = (hostRect.top + hostRect.bottom - physH) / 2;
    const std::uint32_t surfaceId = state.shell->FloatPanel(panel, cx, cy, physW, physH);
    if (surfaceId != 0)
    {
        state.shell->FocusPanel(panel);
        state.pendingFocusSurfaceId = surfaceId;
    }
    else
    {
        state.shell->TogglePanel(panel);
    }
}

std::filesystem::path ResolveSelectedTargetFolder(const ToolkitAppState& state)
{
    if (state.shell)
    {
        const std::string& selected = state.shell->SelectedAssetPath();
        if (!selected.empty())
        {
            const std::filesystem::path p = asset_io::FromUtf8(selected);
            std::error_code ec;
            if (std::filesystem::is_directory(p, ec) && !ec) return p;
            if (p.has_parent_path() && std::filesystem::is_directory(p.parent_path(), ec) && !ec)
                return p.parent_path();
        }
        const std::string& curRel = state.shell->CurrentAssetFolder();
        if (!curRel.empty())
        {
            const std::filesystem::path candidateRoot = state.root / asset_io::FromUtf8(curRel);
            std::error_code ec;
            if (std::filesystem::is_directory(candidateRoot, ec) && !ec) return candidateRoot;
            const std::filesystem::path candidateContent = state.assetsRoot / asset_io::FromUtf8(curRel);
            if (std::filesystem::is_directory(candidateContent, ec) && !ec) return candidateContent;
        }
    }
    return state.assetsRoot;
}

std::string NormalizeRelSlash(const std::filesystem::path& rel)
{
    std::string s = asset_io::Utf8(rel);
    for (char& ch : s) if (ch == '\\') ch = '/';
    if (s == ".") s.clear();
    return s;
}

std::string EnsureAssetThumbnail(ToolkitAppState& state,
                                 const std::filesystem::path& resolved,
                                 const AssetId& id,
                                 std::string_view type)
{
    if (state.hidden) return {};
    if (type != "material" && type != "texture" && !(type == "shader" && IsShaderGraphPath(resolved))) return {};
    const std::string key = "thumb:" + asset_io::Utf8(resolved);
    std::error_code timeEc;
    const auto mtime = std::filesystem::last_write_time(resolved, timeEc);
    if (auto found = state.assetThumbnails.find(key); found != state.assetThumbnails.end())
    {
        if (timeEc || found->second.mtime == mtime)
            return found->second.image ? key : std::string{};
    }

    std::shared_ptr<ImageData> thumbImg;
    if (type == "shader" && IsShaderGraphPath(resolved))
    {
        ShaderGraphDocument sgDoc;
        std::string sgErr;
        if (LoadShaderGraph(resolved, sgDoc, sgErr))
        {
            const ShaderPreviewImage rendered = RenderShaderGraphPreviewSphere(sgDoc, 0.0f, -0.55f, 0.52f, {}, 40);
            if (rendered.width > 0 && rendered.height > 0 && !rendered.rgba8.empty())
            {
                thumbImg = std::make_shared<ImageData>();
                thumbImg->width = rendered.width;
                thumbImg->height = rendered.height;
                thumbImg->rgba8 = rendered.rgba8;
            }
        }
    }
    else if (type == "material")
    {
        std::vector<AssetDiagnostic> diags;
        if (const auto mat = state.assets.LoadMaterial(id, diags))
        {
            const Vec4 bc{mat->baseColor[0], mat->baseColor[1], mat->baseColor[2], mat->baseColor[3]};
            const MaterialThumbnail rendered = RenderMaterialThumbnail(bc, mat->metallic, mat->roughness, 40);
            if (rendered.width > 0 && rendered.height > 0 && !rendered.rgba8.empty())
            {
                thumbImg = std::make_shared<ImageData>();
                thumbImg->width = rendered.width;
                thumbImg->height = rendered.height;
                thumbImg->rgba8 = rendered.rgba8;
            }
        }
    }
    else if (type == "texture")
    {
        std::vector<AssetDiagnostic> diags;
        if (const auto src = state.assets.LoadTextureImage(id, diags);
            src && src->width > 0 && src->height > 0 && src->rgba8.size() >= static_cast<std::size_t>(src->width) * src->height * 4)
        {
            constexpr std::uint32_t kMaxThumb = 40;
            const std::uint32_t tw = std::min(src->width, kMaxThumb);
            const std::uint32_t th = std::min(src->height, kMaxThumb);
            thumbImg = std::make_shared<ImageData>();
            thumbImg->width = tw;
            thumbImg->height = th;
            thumbImg->rgba8.resize(static_cast<std::size_t>(tw) * th * 4);
            for (std::uint32_t y = 0; y < th; ++y)
            {
                const std::uint32_t sy = std::min(src->height - 1, (y * src->height) / th);
                for (std::uint32_t x = 0; x < tw; ++x)
                {
                    const std::uint32_t sx = std::min(src->width - 1, (x * src->width) / tw);
                    const std::size_t srcIdx = (static_cast<std::size_t>(sy) * src->width + sx) * 4;
                    const std::size_t dstIdx = (static_cast<std::size_t>(y) * tw + x) * 4;
                    for (std::size_t c = 0; c < 4; ++c) thumbImg->rgba8[dstIdx + c] = src->rgba8[srcIdx + c];
                }
            }
        }
    }
    std::uint64_t token = 0;
    if (auto existing = state.assetThumbnails.find(key); existing != state.assetThumbnails.end() && existing->second.token != 0)
        token = existing->second.token;
    else if (thumbImg)
        token = state.nextAssetThumbToken++;
    state.assetThumbnails[key] = {token, thumbImg, mtime};
    return thumbImg ? key : std::string{};
}

void RefreshShaderWatchMtimes(ToolkitAppState& state, bool triggerReloadOnDiff)
{
    std::string changedNames;
    std::size_t changedCount = 0;
    for (const AssetId& id : state.assets.ListIds())
    {
        const std::filesystem::path resolved = state.assets.Resolve(id);
        if (resolved.empty() || !IsUserSurfaceAssetPath(resolved)) continue;
        std::error_code ec;
        if (!std::filesystem::exists(resolved, ec) || ec) continue;
        const auto mtime = std::filesystem::last_write_time(resolved, ec);
        if (ec) continue;
        const std::string key = asset_io::Utf8(resolved);
        auto it = state.shaderFileMtimes.find(key);
        if (it == state.shaderFileMtimes.end())
        {
            state.shaderFileMtimes.emplace(key, mtime);
            if (triggerReloadOnDiff && state.shaderMtimesInitialized)
            {
                ++changedCount;
                if (changedNames.empty()) changedNames = asset_io::Utf8(resolved.filename());
            }
        }
        else if (it->second != mtime)
        {
            it->second = mtime;
            if (triggerReloadOnDiff)
            {
                ++changedCount;
                if (changedNames.empty()) changedNames = asset_io::Utf8(resolved.filename());
            }
        }
    }
    state.shaderMtimesInitialized = true;
    if (triggerReloadOnDiff && changedCount > 0)
    {
        ++state.shaderReloadGeneration;
        state.lastLoggedShaderDiagnostic.clear();
        if (state.shaderGraph) (void)state.shaderGraph->ReloadIfModifiedOnDisk();
        if (state.shell)
        {
            if (!state.assetState.assetPath.empty() && IsMaterialDocumentPath(state.assetState.assetPath))
                state.shell->SelectAsset(asset_io::Utf8(state.assetState.assetPath));
            const std::string msg = state.text.GetText("EditorUi.ShaderHotReloaded") + ": " + changedNames;
            NotifyToolkitStatus(state, msg, false);
            state.shell->AppendConsoleLine("[Info] " + msg);
        }
    }
}

void NotifyToolkitAssetPathChanged(ToolkitAppState& state,
                                   const std::filesystem::path& oldPath,
                                   const std::filesystem::path& newPath)
{
    for (auto it = state.shaderFileMtimes.begin(); it != state.shaderFileMtimes.end();)
    {
        const std::filesystem::path p = asset_io::FromUtf8(it->first);
        if (p == oldPath || asset_io::Within(oldPath, p)) it = state.shaderFileMtimes.erase(it);
        else ++it;
    }
    for (auto it = state.assetThumbnails.begin(); it != state.assetThumbnails.end();)
    {
        const std::string_view key = it->first;
        const std::filesystem::path p = asset_io::FromUtf8(key.starts_with("thumb:") ? key.substr(6) : key);
        if (p == oldPath || asset_io::Within(oldPath, p)) it = state.assetThumbnails.erase(it);
        else ++it;
    }
    if (newPath.empty())
    {
        if (state.shaderGraph) state.shaderGraph->NotifyAssetDeleted(oldPath);
    }
    else
    {
        if (state.shaderGraph) state.shaderGraph->NotifyAssetMoved(oldPath, newPath);
    }
    ++state.shaderReloadGeneration;
    state.lastLoggedShaderDiagnostic.clear();
    RefreshShaderWatchMtimes(state, false);
}

bool PromptSaveSceneAsDialog(ToolkitAppState& state)
{
    wchar_t file[32768]{};
    const std::filesystem::path scenesDir = std::filesystem::is_directory(state.root / "Scenes")
        ? (state.root / "Scenes") : state.root;
    std::filesystem::path initialFile = state.session.Path().empty()
        ? (scenesDir / "Main.scene.json") : state.session.Path();
    wcsncpy_s(file, initialFile.c_str(), _TRUNCATE);
    const std::wstring initialDirW = scenesDir.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.hostWindow ? state.hostWindow : GetActiveWindow();
    dialog.lpstrFilter = L"Flare Scene (*.scene.json)\0*.scene.json\0All Files (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.lpstrInitialDir = initialDirW.c_str();
    dialog.lpstrDefExt = L"scene.json";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return false;
    std::filesystem::path chosen(file);
    if (!asset_io::Utf8(chosen).ends_with(".scene.json"))
    {
        if (chosen.extension() == L".json") chosen.replace_extension();
        chosen += L".scene.json";
    }
    if (!state.session.Save(chosen))
    {
        NotifyToolkitStatus(state, LocalizedToolkitStatus(state.text, state.session.LastError(), "EditorUi.SaveFailed"));
        return false;
    }
    state.externalChange.Reset(state.session.Path());
    if (asset_io::Within(state.assetsRoot, chosen))
    {
        AssetId id;
        std::vector<AssetDiagnostic> diags;
        state.assets.RegisterSource(chosen, {}, id, diags);
    }
    std::vector<AssetDiagnostic> scanDiags;
    state.assets.Scan(scanDiags);
    std::vector<EditorAssetIssue> issues;
    EditorAssetActions::SyncProjectAssets(state.root, state.assets, issues);
    NotifyToolkitStatus(state, state.text.GetText("EditorUi.Saved") + ": " + asset_io::Utf8(chosen.filename()));
    Log(LogLevel::Info, "EditorUi.ToolkitScene: saved-as");
    return true;
}

bool PromptOpenSceneDialog(ToolkitAppState& state)
{
    wchar_t file[32768]{};
    const std::filesystem::path scenesDir = std::filesystem::is_directory(state.root / "Scenes")
        ? (state.root / "Scenes") : state.root;
    const std::wstring initialDirW = scenesDir.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.hostWindow ? state.hostWindow : GetActiveWindow();
    dialog.lpstrFilter = L"Flare Scene (*.scene.json)\0*.scene.json\0All Files (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.lpstrInitialDir = initialDirW.c_str();
    dialog.lpstrDefExt = L"scene.json";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return true;
    const std::filesystem::path chosen(file);
    if (!state.session.Open(chosen, true))
    {
        NotifyToolkitStatus(state, LocalizedToolkitStatus(state.text, state.session.LastError(), "EditorUi.SaveFailed"));
        return false;
    }
    ClearToolkitAssetSelectionState(state);
    state.externalChange.Reset(state.session.Path());
    state.shell->FocusPanel(EditorPanelId::Viewport);
    NotifyToolkitStatus(state, state.text.GetText("EditorUi.SceneOpened") + ": " + asset_io::Utf8(chosen.filename()));
    Log(LogLevel::Info, "EditorUi.ToolkitScene: opened");
    return true;
}

void ConfirmIfDirtyThen(ToolkitAppState& state, ToolkitWorkspaceContext& ctx, std::function<void()> proceed)
{
    const bool anyDirty = state.host.AnyDocumentDirty(state.session, &state.ui) || state.scriptWorkflow.Dirty();
    if (!anyDirty)
    {
        proceed();
        return;
    }
    state.shell->ShowConfirm(
        state.text.GetText("EditorUi.UnsavedSceneTitle"),
        state.text.GetText("EditorUi.UnsavedScenePrompt"),
        state.text.GetText("EditorUi.SaveAndContinue"),
        state.text.GetText("EditorUi.DontSave"),
        state.text.GetText("EditorUi.Cancel"),
        [&state, &ctx, proceed]() {
            if (state.session.Path().empty() && !state.hidden && state.hostWindow != nullptr)
            {
                if (!PromptSaveSceneAsDialog(state)) return;
            }
            if (!SaveAllToolkitDocuments(ctx)) return;
            proceed();
        },
        [proceed]() {
            proceed();
        });
}

void LaunchToolkitImportPlan(ToolkitAppState& state, AssetImportPlan plan)
{
    std::filesystem::path primary;
    for (const auto& file : plan.files)
    {
        if (file.primary) { primary = file.target; break; }
    }
    state.importTask = std::async(std::launch::async, [plan = std::move(plan), primary = std::move(primary),
                                                       cache = state.root / "out" / "Cache" / "Assets"]() {
        ToolkitImportResult result;
        AssetStore worker(plan.contentRoot, cache);
        std::vector<AssetId> imported;
        result.primary = primary;
        result.success = AssetImportService::ExecuteRegistered(plan, worker, imported, result.key, result.detail);
        if (result.success) result.detail = std::to_string(imported.size()) + " | " + asset_io::Utf8(result.primary);
        return result;
    });
}

void OpenToolkitImportConfirm(ToolkitAppState& state, std::vector<std::filesystem::path> sources)
{
    auto report = [&state](const char* key, const std::string& detail = {}, const char* tag = "") {
        state.importStatus = state.text.GetText(key) + (detail.empty() ? std::string{} : ": " + detail);
        state.shell->SetStatus(state.importStatus);
        state.shell->SetAssetsPreviewText(state.importStatus);
        state.shell->AppendConsoleLine(std::string(tag) + state.importStatus);
    };
    if (sources.empty()) return;
    if (state.importTask.valid() || state.host.ExportBusy()) { report("Editor.AssetOperationBusy", {}, "[Warning] "); return; }
    if (state.session.IsPlaying()) { report("Editor.EditWhilePlaying", {}, "[Warning] "); return; }

    const auto plan = AssetImportService::Plan({std::move(sources), state.assets.SourceRoot(), {}});
    std::string detail;
    if (!plan.valid)
    {
        const std::string key = !plan.diagnostics.empty() ? plan.diagnostics.front().key : plan.errorKey;
        detail = state.text.GetText(key.empty() ? "Editor.AssetImportFailed" : key);
        if (!plan.diagnostics.empty() && !plan.diagnostics.front().detail.empty()) detail += ": " + plan.diagnostics.front().detail;
        else if (!plan.detail.empty()) detail += ": " + plan.detail;
    }
    else
    {
        detail = state.text.GetText("Editor.AssetImportConfirm") + " (" + std::to_string(plan.files.size()) + ")";
    }
    state.shell->ShowConfirm(
        state.text.GetText("EditorUi.Import"), detail, state.text.GetText("EditorUi.Import"), {},
        state.text.GetText("EditorUi.Cancel"),
        [&state, plan, report]() {
            report("Editor.AssetImporting", plan.files.empty() ? std::string{} : asset_io::Utf8(plan.files.front().target.filename()));
            LaunchToolkitImportPlan(state, plan);
        },
        {}, plan.valid);
}

bool BrowseAndImportToolkitAsset(ToolkitAppState& state)
{
    auto report = [&state](const char* key, const std::string& detail = {}, const char* tag = "") {
        state.importStatus = state.text.GetText(key) + (detail.empty() ? std::string{} : ": " + detail);
        state.shell->SetStatus(state.importStatus);
        state.shell->SetAssetsPreviewText(state.importStatus);
        state.shell->AppendConsoleLine(std::string(tag) + state.importStatus);
    };
    if (state.importTask.valid() || state.host.ExportBusy()) { report("Editor.AssetOperationBusy", {}, "[Warning] "); return false; }
    if (state.session.IsPlaying()) { report("Editor.EditWhilePlaying", {}, "[Warning] "); return false; }

    std::filesystem::path source;
    if (state.hooks && state.hooks->chooseAssetImportSource)
    {
        source = state.hooks->chooseAssetImportSource();
    }
    else
    {
        wchar_t file[32768]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = state.hostWindow;
        dialog.lpstrFilter = L"Assets\0*.fbx;*.gltf;*.glb;*.png;*.jpg;*.jpeg;*.wav;*.ogg;*.material.json;*.sebin\0All files\0*.*\0";
        dialog.lpstrFile = file;
        dialog.nMaxFile = static_cast<DWORD>(std::size(file));
        const std::wstring initial = state.assets.SourceRoot().wstring();
        dialog.lpstrInitialDir = initial.c_str();
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog))
        {
            const DWORD error = CommDlgExtendedError();
            if (error) report("Editor.AssetImportFailed", std::to_string(error), "[Error] ");
            return false;
        }
        source = file;
    }
    if (source.empty()) return false;
    report("Editor.AssetImporting", asset_io::Utf8(source.filename()));
    const AssetImportRequest request{{source}, state.assets.SourceRoot(), {}};
    state.importTask = std::async(std::launch::async, [request, cache = state.root / "out" / "Cache" / "Assets"]() {
        ToolkitImportResult result;
        const auto plan = AssetImportService::Plan(request);
        if (!plan.valid)
        {
            result.key = plan.errorKey;
            result.detail = plan.detail;
            if (!plan.diagnostics.empty())
            {
                result.key = plan.diagnostics.front().key;
                result.detail = plan.diagnostics.front().detail;
            }
            return result;
        }
        for (const auto& file : plan.files) if (file.primary) { result.primary = file.target; break; }
        AssetStore worker(plan.contentRoot, cache);
        std::vector<AssetId> imported;
        result.success = AssetImportService::ExecuteRegistered(plan, worker, imported, result.key, result.detail);
        if (result.success) result.detail = std::to_string(imported.size()) + " | " + asset_io::Utf8(result.primary);
        return result;
    });
    return true;
}

void ApplyStartupEnvironmentFlags(ToolkitAppState& state)
{
    wchar_t openPanel[64]{};
    if (GetEnvironmentVariableW(L"FLARE_EDITOR_OPEN_PANEL", openPanel, 64) > 0)
    {
        EditorPanelId panel = EditorPanelId::None;
        if (EditorPanelFromName(asset_io::Utf8(openPanel), panel)) state.shell->FocusPanel(panel);
    }
    wchar_t openAsset[512]{};
    if (GetEnvironmentVariableW(L"FLARE_EDITOR_OPEN_ASSET", openAsset, 512) > 0)
        state.pendingAsset = asset_io::Utf8(openAsset);
    wchar_t flag[8]{};
    state.autoExport = GetEnvironmentVariableW(L"FLARE_EDITOR_AUTO_EXPORT", flag, 8) > 0;
    state.autoExportFlow = GetEnvironmentVariableW(L"FLARE_EDITOR_AUTO_EXPORT_FLOW", flag, 8) > 0;
    wchar_t cancelMs[16]{};
    if (GetEnvironmentVariableW(L"FLARE_EDITOR_AUTO_EXPORT_CANCEL_MS", cancelMs, 16) > 0)
        state.autoCancelMs = std::atoi(asset_io::Utf8(cancelMs).c_str());
}

void PollBackgroundImportAndExport(ToolkitAppState& state)
{
    if (state.importTask.valid() && state.importTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        ToolkitImportResult result;
        try { result = state.importTask.get(); }
        catch (const std::exception& failure) { result.key = "Editor.AssetImportFailed"; result.detail = failure.what(); }
        if (result.success)
        {
            std::vector<AssetDiagnostic> failures;
            if (!state.assets.Scan(failures))
            {
                result.success = false;
                result.key = failures.empty() ? "Assets.ScanFailed" : failures.front().key;
                if (!failures.empty()) result.detail = failures.front().detail;
            }
            else
            {
                state.pendingAsset = asset_io::Utf8(result.primary);
                std::vector<EditorAssetIssue> issues;
                if (!EditorAssetActions::SyncProjectAssets(state.root, state.assets, issues))
                {
                    for (const auto& issue : issues)
                    {
                        const auto message = state.text.GetText(issue.key) + ": " + issue.detail;
                        state.shell->AppendConsoleLine("[Warning] " + message);
                        Log(LogLevel::Warning, message);
                    }
                }
            }
        }
        if (result.key.empty()) result.key = result.success ? "Editor.AssetImportSucceeded" : "Editor.AssetImportFailed";
        state.importStatus = state.text.GetText(result.key) + (result.detail.empty() ? std::string{} : ": " + result.detail);
        state.shell->SetAssetsPreviewText(state.importStatus);
        state.shell->AppendConsoleLine((result.success ? std::string{} : std::string("[Error] ")) + state.importStatus);
        Log(result.success ? LogLevel::Info : LogLevel::Warning, state.importStatus);
        state.shell->FocusPanel(EditorPanelId::Assets);
    }

    state.host.PollExport(state.text);
    const bool exportBusy = state.host.ExportBusy();
    if (state.autoCancelMs > 0 && exportBusy && !state.autoCancelRequested &&
        GetTickCount64() - state.autoExportStart >= static_cast<std::uint64_t>(state.autoCancelMs))
    {
        state.host.RequestCancelExport();
        state.autoCancelRequested = true;
        Log(LogLevel::Info, "EditorUi.ToolkitExport: auto-cancel");
    }
    if (exportBusy != state.exportBusy)
    {
        state.exportBusy = exportBusy;
        state.shell->SetFormActionLabel(EditorPanelId::BuildExport,
            exportBusy ? state.text.GetText("EditorUi.Cancel") : state.text.GetText("EditorUi.Export"));
    }
    if (exportBusy)
    {
        const auto progress = state.host.ExportSnapshot();
        if (!progress.report.stage.empty())
            state.shell->SetFormStatus(EditorPanelId::BuildExport, state.text.GetText(progress.report.stage));
    }
    else
    {
        const auto snapshot = state.host.ExportSnapshot();
        if (snapshot.finished && !state.exportFinished)
        {
            std::string exportStatus = snapshot.cancelled
                ? state.text.GetText("EditorUi.ExportCancelled")
                : snapshot.report.state == ExportOperationState::Succeeded
                    ? state.text.GetText("EditorUi.ExportSucceeded")
                    : state.text.GetText("EditorUi.ExportFailed");
            if (!snapshot.cancelled && snapshot.report.state != ExportOperationState::Succeeded &&
                !snapshot.report.diagnosticKey.empty())
            {
                exportStatus += ": " + LocalizedToolkitStatus(state.text, snapshot.report.diagnosticKey, "EditorUi.ExportFailed");
            }
            if (!snapshot.outputPackage.empty()) exportStatus += "  " + asset_io::Utf8(snapshot.outputPackage);
            state.shell->SetFormStatus(EditorPanelId::BuildExport, exportStatus);
            if (state.exportStarted && !state.exportFinished)
            {
                state.exportFinished = true;
                Log(LogLevel::Info, std::string("EditorUi.ToolkitExport: finished ") +
                    (snapshot.cancelled ? "cancelled" : snapshot.report.state == ExportOperationState::Succeeded ? "succeeded" : "failed") +
                    (snapshot.report.diagnosticKey.empty() ? "" : (" " + snapshot.report.diagnosticKey)));
            }
            if (state.autoExportFlow && state.autoExportFlowPhase == 0 && snapshot.cancelled && snapshot.finished)
            {
                state.autoExportFlowPhase = 1;
                state.autoCancelMs = 0;
                state.autoCancelRequested = false;
                state.exportStarted = false;
                state.exportFinished = false;
                if (state.autoExportFn) state.autoExportFn();
            }
            else
            {
                state.exportFinished = true;
            }
        }
    }
}

void SyncToolkitHierarchyAssetsAndInspector(ToolkitAppState& state)
{
    std::vector<ui::editor::EditorHierarchyNode> hierarchy;
    const World& editWorld = state.session.EditWorld();
    const World& hierarchyWorld = state.session.PlayWorld() ? *state.session.PlayWorld() : editWorld;
    for (const EditorHierarchyItem& item : BuildEditorHierarchy(hierarchyWorld))
    {
        ui::editor::EditorHierarchyNode node;
        node.id = item.id;
        node.parent = item.parent;
        node.depth = item.depth;
        node.hasChildren = item.hasChildren;
        node.expanded = true;
        const EntityHandle handle = hierarchyWorld.FindByStableId(item.id);
        if (const EntityRecord* record = hierarchyWorld.Get(handle))
        {
            node.label = record->name;
            node.enabled = hierarchyWorld.IsEnabled(handle);
            node.prefab = hierarchyWorld.GetPrefabLink(handle) != nullptr;
            if (hierarchyWorld.GetCamera(handle)) node.iconKind = "camera";
            else if (hierarchyWorld.GetLight(handle) || HasComponent(hierarchyWorld, handle, "rectangle")) node.iconKind = "light";
            else if (hierarchyWorld.GetMesh(handle)) node.iconKind = "mesh";
            else if (HasComponent(hierarchyWorld, handle, "audio") || HasComponent(hierarchyWorld, handle, "listener")) node.iconKind = "audio";
            else if (HasComponent(hierarchyWorld, handle, "emitter")) node.iconKind = "particle";
            else node.iconKind = "empty";
        }
        else node.label = "Entity " + std::to_string(item.id);
        hierarchy.push_back(std::move(node));
    }
    state.shell->SetHierarchy(std::move(hierarchy));
    state.shell->SetHierarchySelection(state.session.Selection(), state.session.Selected());

    const std::uint64_t assetScanNow = GetTickCount64();
    const bool activelyResizingUi = state.interactiveResizing || state.shell->SplitterActive();
    const std::size_t currentAssetIds = state.assets.ListIds().size();
    const std::size_t currentScriptCount = state.scriptWorkflow.Sources().size();
    const bool needAssetScan = state.assetsListDirty ||
                               state.cachedAssetEntries.empty() ||
                               !state.pendingAsset.empty() ||
                               !state.pendingRenameAssetPath.empty() ||
                               currentAssetIds != state.lastAssetIdCount ||
                               currentScriptCount != state.lastScriptSourceCount ||
                               (!activelyResizingUi && (assetScanNow - state.lastAssetScanTick >= 500));
    if (needAssetScan)
    {
        state.assetsListDirty = false;
        state.lastAssetScanTick = assetScanNow;
        state.lastAssetIdCount = currentAssetIds;
        state.lastScriptSourceCount = currentScriptCount;

        std::vector<ui::editor::EditorAssetEntry> entries;
        const auto computeRelDir = [&](const std::filesystem::path& fileOrDir, const std::filesystem::path& rootBase,
                                       std::string_view topPrefix) -> std::string {
            std::error_code ec;
            const auto parent = fileOrDir.parent_path();
            if (std::filesystem::equivalent(parent, rootBase, ec) && !ec)
                return std::string(topPrefix);
            const std::string sub = NormalizeRelSlash(parent.lexically_relative(rootBase));
            if (sub.empty() || sub.starts_with("..")) return std::string(topPrefix);
            return topPrefix.empty() ? sub : (std::string(topPrefix) + "/" + sub);
        };
        const auto scanFolderTree = [&](const std::filesystem::path& dirRoot, std::string_view topPrefix) {
            std::error_code ec;
            if (!std::filesystem::is_directory(dirRoot, ec) || ec) return;
            if (!topPrefix.empty())
            {
                const bool dup = std::any_of(entries.begin(), entries.end(), [&](const auto& e) {
                    return e.type == "folder" && e.relativeDir.empty() && e.displayName == topPrefix;
                });
                if (!dup)
                {
                    ui::editor::EditorAssetEntry topEntry;
                    topEntry.path = asset_io::Utf8(dirRoot);
                    topEntry.type = "folder";
                    topEntry.relativeDir = {};
                    topEntry.displayName = std::string(topPrefix);
                    entries.push_back(std::move(topEntry));
                }
            }
            for (std::filesystem::recursive_directory_iterator it(dirRoot, ec), end; !ec && it != end; it.increment(ec))
            {
                if (!it->is_directory(ec)) continue;
                const std::string dirName = asset_io::Utf8(it->path().filename());
                if (dirName.empty() || dirName.front() == '.' || dirName == "bin" || dirName == "obj" || dirName == "out")
                {
                    it.disable_recursion_pending();
                    continue;
                }
                ui::editor::EditorAssetEntry folderEntry;
                folderEntry.path = asset_io::Utf8(it->path());
                folderEntry.type = "folder";
                folderEntry.relativeDir = computeRelDir(it->path(), dirRoot, topPrefix);
                folderEntry.displayName = dirName;
                entries.push_back(std::move(folderEntry));
            }
        };
        scanFolderTree(state.assetsRoot, {});
        std::error_code sameRootEc;
        if (!(std::filesystem::equivalent(state.assetsRoot, state.root, sameRootEc) && !sameRootEc))
        {
            if (std::filesystem::is_directory(state.root / "Scenes", sameRootEc))
                scanFolderTree(state.root / "Scenes", "Scenes");
            if (std::filesystem::is_directory(state.root / "Scripts", sameRootEc))
                scanFolderTree(state.root / "Scripts", "Scripts");
        }

        std::unordered_set<std::string> seenScenePaths;
        for (const AssetId& id : state.assets.ListIds())
        {
            const std::filesystem::path resolved = state.assets.Resolve(id);
            std::string extension = resolved.extension().string();
            for (char& ch : extension)
            {
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            }
            const std::string utf8Path = asset_io::Utf8(resolved);
            std::string type = "asset";
            if (extension == ".fbx" || extension == ".gltf" || extension == ".glb") type = "model";
            else if (IsPrefabDocumentPath(resolved)) type = "prefab";
            else if (IsMaterialDocumentPath(resolved)) type = "material";
            else if (utf8Path.ends_with(".scene.json")) { type = "scene"; seenScenePaths.insert(utf8Path); }
            else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga") type = "texture";
            else if (extension == ".wav" || extension == ".ogg") type = "audio";
            else if (IsShaderGraphPath(resolved) || extension == ".hlsl" || extension == ".hlsli" || extension == ".sebin") type = "shader";
            else if (extension == ".seenv") type = "env";
            else if (extension == ".tod") type = "tod";
            else if (extension == ".senav") type = "navmesh";
            else if (extension == ".json" || extension == ".selod"
                || extension == ".sehlod" || extension == ".semeshlet" || extension == ".seimp"
                || extension == ".sebc" || extension == ".ppm" || extension == ".sesh"
                || extension == ".sedp" || extension == ".sebrdf" || extension == ".seltc"
                || extension == ".sesheen" || extension == ".seskin")
            {
                type = "data";
            }
            ui::editor::EditorAssetEntry assetEntry;
            assetEntry.path = utf8Path;
            assetEntry.type = type;
            if (asset_io::Within(state.assetsRoot, resolved))
                assetEntry.relativeDir = computeRelDir(resolved, state.assetsRoot, {});
            else if (asset_io::Within(state.root / "Scenes", resolved))
                assetEntry.relativeDir = computeRelDir(resolved, state.root / "Scenes", "Scenes");
            assetEntry.thumbnail = EnsureAssetThumbnail(state, resolved, id, type);
            entries.push_back(std::move(assetEntry));
        }
        if (std::filesystem::is_directory(state.root / "Scenes", sameRootEc))
        {
            for (std::filesystem::recursive_directory_iterator it(state.root / "Scenes", sameRootEc), end;
                 !sameRootEc && it != end; it.increment(sameRootEc))
            {
                if (!it->is_regular_file(sameRootEc)) continue;
                const std::string utf8Scene = asset_io::Utf8(it->path());
                if (!utf8Scene.ends_with(".scene.json") || seenScenePaths.contains(utf8Scene)) continue;
                ui::editor::EditorAssetEntry sceneEntry;
                sceneEntry.path = utf8Scene;
                sceneEntry.type = "scene";
                sceneEntry.relativeDir = computeRelDir(it->path(), state.root / "Scenes", "Scenes");
                entries.push_back(std::move(sceneEntry));
            }
        }
        for (const auto& source : state.scriptWorkflow.Sources())
        {
            ui::editor::EditorAssetEntry scriptEntry;
            scriptEntry.path = asset_io::Utf8(source);
            scriptEntry.type = "C#";
            if (asset_io::Within(state.root / "Scripts", source))
                scriptEntry.relativeDir = computeRelDir(source, state.root / "Scripts", "Scripts");
            else if (asset_io::Within(state.assetsRoot, source))
                scriptEntry.relativeDir = computeRelDir(source, state.assetsRoot, {});
            entries.push_back(std::move(scriptEntry));
        }
        state.cachedAssetEntries = std::move(entries);
        state.shell->SetAssets(state.cachedAssetEntries);
    }

    std::string pendingAssetPath;
    if (!state.pendingAsset.empty())
    {
        for (const ui::editor::EditorAssetEntry& entry : state.cachedAssetEntries)
        {
            if (entry.path == state.pendingAsset || entry.path.find(state.pendingAsset) != std::string::npos)
            {
                pendingAssetPath = entry.path;
                state.pendingAsset.clear();
                break;
            }
        }
    }
    if (!pendingAssetPath.empty()) state.shell->SelectAsset(pendingAssetPath);
    if (!state.pendingRenameAssetPath.empty())
    {
        const std::string toRename = std::move(state.pendingRenameAssetPath);
        state.pendingRenameAssetPath.clear();
        state.shell->Frame();
        (void)state.shell->BeginAssetRename(toRename);
    }

    std::vector<ui::editor::EditorProperty> properties;
    std::string inspectorLabel = state.text.GetText("EditorUi.NoSelection");
    const World& activeWorld = (state.session.IsPlaying() && state.session.PlayWorld())
        ? *state.session.PlayWorld()
        : editWorld;
    if (state.shell->InspectorLocked())
    {
        if (state.assetState.lockedToEntity && state.assetState.lockedEntityId != 0 &&
            !activeWorld.FindByStableId(state.assetState.lockedEntityId))
        {
            state.shell->SetInspectorLocked(false);
        }
        else if (state.assetState.lockedToAsset)
        {
            std::error_code lockEc;
            if (state.assetState.lockedAssetPath.empty() ||
                !std::filesystem::exists(asset_io::FromUtf8(state.assetState.lockedAssetPath), lockEc) || lockEc)
            {
                state.shell->SetInspectorLocked(false);
            }
        }
    }
    const bool lockedEntityMode = state.shell->InspectorLocked() && state.assetState.lockedToEntity;
    const bool lockedAssetMode = state.shell->InspectorLocked() && state.assetState.lockedToAsset &&
                                 !state.assetState.assetName.empty();
    const StableEntityId inspectedId = lockedEntityMode ? state.assetState.lockedEntityId : state.session.Selected();
    if (const auto* record = activeWorld.Get(activeWorld.FindByStableId(inspectedId))) inspectorLabel = record->name;
    if (!lockedAssetMode &&
        (state.shell->ActivePanel() == EditorPanelId::Hierarchy || state.shell->ActivePanel() == EditorPanelId::Viewport))
    {
        state.assetState.inspectUi = false;
    }
    state.shell->SetPlayKeepArmed(state.session.IsPlaying() && inspectedId != 0 &&
                                  state.assetState.playKeepEntities.contains(inspectedId));
    if (!lockedEntityMode && !lockedAssetMode && state.assetState.inspectUi)
    {
        state.shell->SetInspector(state.host.Ui().Selected(), state.text.GetText("EditorUi.UiDesigner"),
                                  UiInspectorRows(state.ui, state.host.Ui().Selected(), state.text, state.session.IsPlaying()));
    }
    else if (!lockedEntityMode && (lockedAssetMode || !state.assetState.assetName.empty()))
    {
        state.shell->SetInspector(0, state.assetState.assetName, state.assetState.rows);
    }
    else
    {
        std::vector<StableEntityId> lockedAliveSel;
        if (lockedEntityMode)
        {
            for (const StableEntityId sid : state.assetState.lockedSelection)
                if (activeWorld.FindByStableId(sid)) lockedAliveSel.push_back(sid);
            if (lockedAliveSel.empty() && inspectedId != 0) lockedAliveSel.push_back(inspectedId);
        }
        const auto& sel = lockedEntityMode ? lockedAliveSel : state.session.Selection();
        const auto resolveRefName = [&activeWorld](std::uint64_t refId) -> std::string {
            if (const auto* rec = activeWorld.Get(activeWorld.FindByStableId(refId)))
                return rec->name;
            return {};
        };
        if (sel.size() > 1)
        {
            inspectorLabel = std::to_string(sel.size()) + " " + state.text.GetText("EditorUi.MultiSelected");
            AppendMultiSceneInspector(state.session, state.assets, state.text, sel, properties);
            state.scriptWorkflow.AppendMultiRows(sel, state.session.IsPlaying(), properties, resolveRefName);
        }
        else
        {
            AppendSceneInspector(state.session, state.assets, state.text, inspectedId, properties);
            const auto clip = std::find_if(properties.begin(), properties.end(), [](const auto& p) { return p.key == "actor.clip"; });
            if (clip != properties.end() && !state.session.IsPlaying())
            {
                const bool active = state.clipPreview.Target() == inspectedId;
                const bool running = active && state.clipPreview.Playing();
                const auto group = clip->section;
                ui::editor::EditorProperty play{"preview.play", state.text.GetText(running ? "EditorUi.PauseClip" : "EditorUi.PlayClip"),
                                                ui::editor::EditorPropertyType::Action, {}, false, {}, group};
                ui::editor::EditorProperty time{"preview.time", state.text.GetText("EditorUi.ClipTime"),
                                                ui::editor::EditorPropertyType::Number,
                                                std::to_string(active ? std::round(state.clipPreview.Time() * 10.) / 10. : 0.), false, {}, group};
                properties.insert(std::next(clip), {std::move(play), std::move(time)});
            }
            if (inspectedId == 0)
            {
                AppendTodInspector(state.host.TimeOfDayPreview(), state.text, state.session.IsPlaying(), properties);
            }
            state.scriptWorkflow.AppendRows(inspectedId, state.session.IsPlaying(), properties, resolveRefName);
        }
        state.shell->SetSuggestedScriptClassName(state.scriptWorkflow.NextSuggestedClassName());
        state.shell->SetInspector(inspectedId, inspectorLabel, std::move(properties));
    }
}
} // namespace

EditorSessionSnapshot CaptureEditorSessionSnapshot(const std::filesystem::path& root,
                                                 const EditorSession& session,
                                                 const EditorViewportCamera& camera,
                                                 const EditorWorkspaceHost& host)
{
    EditorSessionSnapshot snapshot;
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(root, error);
    if (error) return snapshot;
    snapshot.project = asset_io::Utf8(canonical);
    snapshot.documents = host.SessionDocuments(session);
    snapshot.activeDocument = host.SessionActiveDocument(session);
    if (!session.Selection().empty()) snapshot.selection = session.Selection();
    else if (session.Selected()) snapshot.selection.push_back(session.Selected());
    snapshot.cameraX = camera.state.position.x;
    snapshot.cameraY = camera.state.position.y;
    snapshot.cameraZ = camera.state.position.z;
    snapshot.cameraYaw = camera.state.yaw;
    snapshot.cameraPitch = camera.state.pitch;
    snapshot.layoutVersion = 1;
    return snapshot;
}

void PersistToolkitSession(ToolkitAppState& state, bool force)
{
    const auto current = CaptureEditorSessionSnapshot(state.root, state.session, state.editorCamera, state.host);
    const auto now = std::chrono::steady_clock::now();
    if (current != state.observedSession)
    {
        state.observedSession = current;
        state.sessionChangedAt = now;
    }
    if (current != state.persistedSession && (force || now - state.sessionChangedAt >= std::chrono::seconds(1)))
    {
        std::string failure;
        if (EditorSessionStore::SaveSession(state.crashDirectory, current, failure))
        {
            state.persistedSession = current;
        }
        else
        {
            Log(LogLevel::Warning, failure);
            state.sessionChangedAt = now;
        }
    }
}

void BindToolkitImportAndCommands(ToolkitAppState& state, ToolkitWorkspaceContext& ctx)
{
    state.openImportConfirm = [&state](std::vector<std::filesystem::path> sources) {
        OpenToolkitImportConfirm(state, std::move(sources));
    };
    state.shell->SetAssetImportHandler([&state]() {
        if (!state.hooks || !state.hooks->chooseAssetImportSource)
            state.pendingDialogCommand = EditorCommandId::Import;
        else
            BrowseAndImportToolkitAsset(state);
    });
    state.shell->SetAssetActivateHandler([&state, &ctx](const std::string& path) {
        if (state.session.IsPlaying()) return;
        const std::filesystem::path target = asset_io::FromUtf8(path);
        std::string ext = asset_io::Utf8(target.extension());
        for (char& ch : ext) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        if (path.ends_with(".scene.json"))
        {
            ConfirmIfDirtyThen(state, ctx, [&state, target]() {
                if (state.session.Open(target, true))
                {
                    ClearToolkitAssetSelectionState(state);
                    state.externalChange.Reset(state.session.Path());
                    state.shell->FocusPanel(EditorPanelId::Viewport);
                    NotifyToolkitStatus(state, state.text.GetText("EditorUi.SceneOpened") + ": " +
                                               asset_io::Utf8(target.filename()));
                }
                else
                {
                    NotifyToolkitStatus(state, LocalizedToolkitStatus(state.text, state.session.LastError(), "EditorUi.SaveFailed"));
                }
            });
            return;
        }
        if (ext == ".cs")
        {
            CodeEditorSettings settings;
            settings.executable = asset_io::FromUtf8(state.host.Preferences().externalEditor);
            if (!state.scriptWorkflow.OpenSelected(settings))
                state.shell->SetStatus(state.scriptWorkflow.Status());
            return;
        }
        if (IsShaderGraphPath(target))
        {
            if (state.shaderGraph) (void)state.shaderGraph->Open(target, true);
            return;
        }
        if (IsSurfaceShaderHlslPath(target) || ext == ".hlsl" || ext == ".hlsli")
        {
            CodeEditorSettings settings;
            settings.executable = asset_io::FromUtf8(state.host.Preferences().externalEditor);
            std::string err, detail;
            if (!EditorScriptTools::OpenSource(state.root, target, 1, 1, settings, err, detail))
                state.shell->SetStatus(LocalizedToolkitStatus(state.text, err, "EditorUi.SaveFailed"));
            return;
        }
        if (IsMaterialDocumentPath(target))
        {
            OpenToolkitUtilityWindow(state, EditorPanelId::Material);
            return;
        }
        if (IsPrefabDocumentPath(target) && !state.assetState.modelId.empty())
        {
            std::string error;
            StableEntityId rootId = 0;
            PrefabDocument doc;
            Transform rootTransform{};
            if (ReadPrefabDocument(state.assets.Resolve(state.assetState.modelId), doc, error))
                rootTransform.scale = PrefabRootAuthoredScale(doc);
            if (InstantiatePrefabIntoScene(state.session, state.assets, state.assetState.modelId,
                                           rootTransform, rootId, error, state.hooks))
            {
                state.assetState.assetName.clear();
                state.viewportController.Action("Frame", state.session, state.assets);
                state.shell->FocusPanel(EditorPanelId::Viewport);
            }
            return;
        }
        if ((ext == ".fbx" || ext == ".glb" || ext == ".gltf") && !state.assetState.modelId.empty())
        {
            std::string error;
            if (AddModelToScene(state.session, state.assets, state.assetState.modelId, error))
            {
                state.assetState.assetName.clear();
                state.viewportController.Action("Frame", state.session, state.assets);
                state.shell->FocusPanel(EditorPanelId::Viewport);
            }
        }
    });
    const auto lowerExt = [](const std::filesystem::path& p) {
        std::string ext = asset_io::Utf8(p.extension());
        for (char& ch : ext)
        {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        return ext;
    };
    const auto isCsScriptPath = [lowerExt](const std::filesystem::path& p) {
        return lowerExt(p) == ".cs";
    };
    const auto applyAssetToEntity = [&state, lowerExt](const std::string& path, StableEntityId targetId,
                                                       EditorPanelId dropPanel) -> bool {
        if (state.session.IsPlaying()) return false;
        const std::filesystem::path target = asset_io::FromUtf8(path);
        const std::string ext = lowerExt(target);
        if (ext == ".cs")
        {
            if (targetId == 0 && dropPanel == EditorPanelId::Inspector) targetId = state.session.Selected();
            if (targetId == 0)
            {
                state.shell->SetStatus(state.text.GetText("EditorUi.SelectObjectFirst"));
                return false;
            }
            state.session.Select(targetId);
            state.assetState.assetName.clear();
            const std::string scriptId = state.scriptWorkflow.ScriptIdForSource(target);
            const std::string display = state.scriptWorkflow.ClassNameForSource(target);
            bool needsCompile = false;
            if (state.scriptWorkflow.AttachScript(targetId, scriptId, needsCompile))
            {
                const std::string msg = state.text.GetText("EditorUi.ScriptAttached") + ": " + display;
                state.shell->SetStatus(msg);
                state.shell->AppendConsoleLine(msg);
                state.shell->FocusPanel(EditorPanelId::Inspector);
                return true;
            }
            if (needsCompile && !scriptId.empty())
            {
                state.shell->Commands().Execute(
                    {EditorCommandId::Build, std::to_string(targetId) + "|" + scriptId});
                return true;
            }
            if (!state.scriptWorkflow.Status().empty()) state.shell->SetStatus(state.scriptWorkflow.Status());
            return false;
        }
        if (IsMaterialDocumentPath(target))
        {
            if (targetId == 0 && dropPanel == EditorPanelId::Inspector) targetId = state.session.Selected();
            if (targetId == 0) return false;
            AssetId matId;
            for (const auto& id : state.assets.ListIds())
            {
                std::error_code ec;
                if (state.assets.Resolve(id) == target ||
                    (std::filesystem::equivalent(state.assets.Resolve(id), target, ec) && !ec))
                {
                    matId = id;
                    break;
                }
            }
            if (matId.empty()) return false;
            if (!ApplySceneInspector(state.session, targetId, "mesh.material", matId, &state.assets)) return false;
            state.session.Select(targetId);
            state.assetState.assetName.clear();
            const std::string msg = state.text.GetText("EditorUi.MaterialAssigned") + ": " +
                                    asset_io::Utf8(target.stem().stem());
            state.shell->SetStatus(msg);
            state.shell->AppendConsoleLine(msg);
            return true;
        }
        if (dropPanel == EditorPanelId::Hierarchy &&
            (ext == ".fbx" || ext == ".glb" || ext == ".gltf" || IsPrefabDocumentPath(target)))
        {
            AssetId assetId;
            for (const auto& id : state.assets.ListIds())
            {
                std::error_code ec;
                if (state.assets.Resolve(id) == target ||
                    (std::filesystem::equivalent(state.assets.Resolve(id), target, ec) && !ec))
                {
                    assetId = id;
                    break;
                }
            }
            if (assetId.empty()) return false;
            std::string error;
            StableEntityId createdId = 0;
            bool ok = false;
            if (IsPrefabDocumentPath(target))
            {
                PrefabDocument doc;
                Transform rootTransform{};
                if (ReadPrefabDocument(target, doc, error))
                    rootTransform.scale = PrefabRootAuthoredScale(doc);
                ok = InstantiatePrefabIntoScene(state.session, state.assets, assetId, rootTransform,
                                                createdId, error, state.hooks);
            }
            else
            {
                ok = AddModelToScene(state.session, state.assets, assetId, error);
                createdId = state.session.Selected();
            }
            if (!ok)
            {
                state.shell->SetStatus(LocalizedToolkitStatus(state.text, error, "EditorUi.DropFailed"));
                return false;
            }
            if (targetId != 0 && createdId != 0 && createdId != targetId)
            {
                if (state.session.BeginEdit("Reparent"))
                {
                    if (state.session.SetParent(createdId, targetId)) state.session.CommitEdit();
                    else state.session.CancelEdit();
                }
            }
            state.assetState.assetName.clear();
            state.viewportController.Action("Frame", state.session, state.assets);
            return true;
        }
        return false;
    };

    state.shell->SetAssetEntityDropHandler(applyAssetToEntity);
    state.shell->SetHierarchyAssetDropHandler([&state](std::uint64_t entityId) -> bool {
        if (state.session.IsPlaying() || state.importTask.valid() || state.host.ExportBusy() || entityId == 0)
            return false;
        std::filesystem::path created;
        AssetId createdId;
        std::string error;
        const std::filesystem::path targetFolder = ResolveSelectedTargetFolder(state);
        const bool saved = CreatePrefabFromSelection(
            state.session, state.assets, state.root, entityId, targetFolder, created, createdId, error, state.hooks);
        if (saved)
        {
            state.assetsListDirty = true;
            state.pendingAsset = asset_io::Utf8(created);
            const std::string msg = state.text.GetText("EditorUi.PrefabCreated") + ": " + asset_io::Utf8(created.filename());
            NotifyToolkitStatus(state, msg, false);
            state.shell->AppendConsoleLine("[Info] " + msg);
        }
        else
        {
            const std::string msg = LocalizedToolkitStatus(state.text, error, "EditorUi.SaveFailed");
            NotifyToolkitStatus(state, msg, false);
            state.shell->AppendConsoleLine("[Error] " + msg);
        }
        return saved;
    });
    state.shell->SetAssetInspectorSlotDropHandler(
        [&state](const std::string& pathUtf8, std::uint64_t targetId, const std::string& propKey) -> bool {
            if (targetId == 0 && !propKey.starts_with("tod.") && !propKey.starts_with("asset.mat.")) return false;
            const std::filesystem::path target = asset_io::FromUtf8(pathUtf8);
            AssetId resolvedId;
            for (const auto& id : state.assets.ListIds())
            {
                std::error_code ec;
                if (state.assets.Resolve(id) == target ||
                    (std::filesystem::equivalent(state.assets.Resolve(id), target, ec) && !ec))
                {
                    resolvedId = id;
                    break;
                }
            }
            if (resolvedId.empty()) return false;
            if (propKey.starts_with("tod."))
            {
                return ApplyTodInspector(state.host.TimeOfDayPreview(), state.assets, state.session, propKey, resolvedId);
            }
            if (propKey.starts_with("asset.mat."))
            {
                if (state.session.IsPlaying() || !state.host.Material().Ready()) return false;
                const std::string matKey = propKey.substr(10);
                const PropertyDescriptor* desc = FindMaterialProperty(matKey);
                PropertyValue parsed;
                if (!desc || !ParseMaterialFormValue(*desc, resolvedId, parsed)) return false;
                if (!state.host.Material().SetProperty(matKey, parsed) || !state.host.SaveMaterial(state.assets)) return false;
                const std::string curMatPath = asset_io::Utf8(state.assetState.assetPath);
                state.shell->SelectAsset(curMatPath);
                NotifyToolkitStatus(state, state.text.GetText("EditorUi.Saved") + ": " + asset_io::Utf8(target.filename()));
                return true;
            }
            const bool applied = ApplySceneInspector(state.session, targetId, propKey, resolvedId, &state.assets);
            if (applied)
            {
                if (!state.session.IsPlaying()) state.assetState.preferScriptUndo = false;
                NotifyToolkitStatus(state, state.text.GetText("EditorUi.Saved") + ": " + asset_io::Utf8(target.filename()));
            }
            return applied;
        });
    state.shell->SetConsoleActivateHandler([&state](const std::string& text) {
        std::size_t extPos = std::string::npos;
        std::size_t extLen = 0;
        for (const std::string_view token : {std::string_view(".cs"), std::string_view(".hlsl"), std::string_view(".hlsli")})
        {
            for (std::size_t pos = 0; (pos = text.find(token, pos)) != std::string::npos; pos += token.size())
            {
                const std::size_t after = pos + token.size();
                if (after < text.size() && (text[after] == '(' || text[after] == ':'))
                {
                    if (extPos == std::string::npos || pos < extPos)
                    {
                        extPos = pos;
                        extLen = token.size();
                    }
                    break;
                }
            }
        }
        if (extPos == std::string::npos) return;
        std::size_t start = extPos;
        while (start > 0)
        {
            const char c = text[start - 1];
            if (c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '<' || c == '>' || c == '|') break;
            if (c == '(' && (start == 1 || text[start - 2] == ' ')) break;
            if (c == ' ' && start >= 2 && text[start - 2] == ']') break;
            if (c == ' ' && start >= 4 && text.substr(start - 4, 4) == " in ") break;
            --start;
        }
        while (start < extPos && text[start] == ' ') ++start;
        const std::string filePart = text.substr(start, extPos + extLen - start);
        std::uint32_t line = 1, col = 1;
        const std::size_t sepPos = extPos + extLen;
        if (sepPos + 1 < text.size())
        {
            const char* p = text.c_str() + sepPos + 1;
            char* endLine = nullptr;
            const unsigned long parsedLine = std::strtoul(p, &endLine, 10);
            if (endLine != p && parsedLine > 0)
            {
                line = static_cast<std::uint32_t>(parsedLine);
                if (*endLine == ',' || *endLine == ':')
                {
                    char* endCol = nullptr;
                    const unsigned long parsedCol = std::strtoul(endLine + 1, &endCol, 10);
                    if (endCol != endLine + 1 && parsedCol > 0) col = static_cast<std::uint32_t>(parsedCol);
                }
            }
        }
        CodeEditorSettings settings;
        settings.executable = asset_io::FromUtf8(state.host.Preferences().externalEditor);
        if (!state.scriptWorkflow.OpenSourceAtLine(asset_io::FromUtf8(filePart), line, col, settings))
        {
            NotifyToolkitStatus(state, state.scriptWorkflow.Status());
        }
    });
    state.shell->SetAssetRenameHandler([&state](const std::string& oldPathUtf8, const std::string& newBaseName) -> bool {
        if (state.session.IsPlaying() || state.host.ExportBusy() || newBaseName.empty()) return false;
        if (newBaseName.find_first_of("\\/:*?\"<>|") != std::string::npos)
        {
            NotifyToolkitStatus(state, state.text.GetText("EditorUi.InvalidValue"));
            return false;
        }
        const std::filesystem::path src = asset_io::FromUtf8(oldPathUtf8);
        std::error_code ec;
        if (!std::filesystem::exists(src, ec) || ec) return false;
        if (std::filesystem::is_directory(src, ec))
        {
            std::error_code eqEc;
            if ((std::filesystem::equivalent(src, state.assetsRoot, eqEc) && !eqEc) ||
                (std::filesystem::equivalent(src, state.root / "Scenes", eqEc) && !eqEc) ||
                (std::filesystem::equivalent(src, state.root / "Scripts", eqEc) && !eqEc))
            {
                return false;
            }
            const std::filesystem::path dst = src.parent_path() / asset_io::FromUtf8(newBaseName);
            if (dst == src) return true;
            if (std::filesystem::exists(dst, ec))
            {
                NotifyToolkitStatus(state, state.text.GetText("Assets.DestinationExists"));
                return false;
            }
            bool hasInnerFiles = false;
            for (std::filesystem::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec))
            {
                if (it->is_regular_file(ec)) { hasInnerFiles = true; break; }
            }
            if (!hasInnerFiles)
            {
                std::filesystem::rename(src, dst, ec);
                if (ec) { NotifyToolkitStatus(state, state.text.GetText("Assets.WriteFailed")); return false; }
                state.pendingAsset = asset_io::Utf8(dst);
                NotifyToolkitStatus(state, state.text.GetText("EditorUi.AssetRenamed") + ": " + newBaseName);
                return true;
            }
            if (asset_io::Within(state.assetsRoot, src))
            {
                std::filesystem::create_directories(dst, ec);
                std::vector<std::filesystem::path> primaryAssets;
                for (const auto& id : state.assets.ListIds())
                {
                    const auto resolved = state.assets.Resolve(id);
                    if (asset_io::Within(src, resolved)) primaryAssets.push_back(resolved);
                }
                for (const auto& assetFile : primaryAssets)
                {
                    const auto rel = assetFile.lexically_relative(src);
                    std::vector<EditorAssetDocument> editing;
                    state.host.CaptureEditingDocuments(state.session, state.assets, &state.ui, editing);
                    const auto plan = EditorAssetActions::Plan(
                        {EditorAssetAction::Move, state.root, state.assets.SourceRoot(), assetFile, dst / rel, editing},
                        state.assets);
                    std::vector<EditorAssetIssue> issues;
                    if (!plan.valid || !EditorAssetActions::Execute(plan, state.assets, editing, issues))
                    {
                        NotifyToolkitStatus(state, state.text.GetText("Assets.WriteFailed"));
                        return false;
                    }
                }
                std::filesystem::remove_all(src, ec);
                std::vector<AssetDiagnostic> scanDiag;
                state.assets.Scan(scanDiag);
                std::vector<EditorAssetIssue> syncIssues;
                EditorAssetActions::SyncProjectAssets(state.root, state.assets, syncIssues);
                NotifyToolkitAssetPathChanged(state, src, dst);
                state.pendingAsset = asset_io::Utf8(dst);
                NotifyToolkitStatus(state, state.text.GetText("EditorUi.AssetRenamed") + ": " + newBaseName);
                return true;
            }
            std::filesystem::rename(src, dst, ec);
            if (ec) return false;
            state.scriptWorkflow.RefreshSources();
            NotifyToolkitAssetPathChanged(state, src, dst);
            state.pendingAsset = asset_io::Utf8(dst);
            NotifyToolkitStatus(state, state.text.GetText("EditorUi.AssetRenamed") + ": " + newBaseName);
            return true;
        }
        std::string srcName = asset_io::Utf8(src.filename());
        std::string extSuffix;
        if (srcName.ends_with(".material.json")) extSuffix = ".material.json";
        else if (srcName.ends_with(".prefab.json")) extSuffix = ".prefab.json";
        else if (srcName.ends_with(".scene.json")) extSuffix = ".scene.json";
        else if (IsSurfaceShaderHlslPath(src)) extSuffix = ".fshader.hlsl";
        else if (IsShaderGraphPath(src)) extSuffix = ".fshader.json";
        else extSuffix = asset_io::Utf8(src.extension());

        if (extSuffix == ".cs")
        {
            std::string stem = newBaseName;
            if (stem.ends_with(".cs")) stem.erase(stem.size() - 3);
            std::filesystem::path renamed;
            if (!state.scriptWorkflow.RenameScriptSource(src, stem, renamed))
            {
                NotifyToolkitStatus(state, state.scriptWorkflow.Status());
                return false;
            }
            state.pendingAsset = asset_io::Utf8(renamed);
            NotifyToolkitStatus(state, state.text.GetText("EditorUi.ScriptRenamed") + ": " + stem);
            return true;
        }
        std::string finalFileName = newBaseName;
        if (!extSuffix.empty() && !finalFileName.ends_with(extSuffix))
            finalFileName += extSuffix;
        const std::filesystem::path dst = src.parent_path() / asset_io::FromUtf8(finalFileName);
        if (dst == src) return true;
        if (asset_io::Within(state.assetsRoot, src))
        {
            std::vector<EditorAssetDocument> editing;
            if (!state.host.CaptureEditingDocuments(state.session, state.assets, &state.ui, editing)) return false;
            const auto plan = EditorAssetActions::Plan(
                {EditorAssetAction::Move, state.root, state.assets.SourceRoot(), src, dst, editing},
                state.assets);
            std::vector<EditorAssetIssue> issues;
            if (!plan.valid || !EditorAssetActions::Execute(plan, state.assets, editing, issues))
            {
                const std::string errKey = !plan.diagnostics.empty() ? plan.diagnostics.front().key
                    : (!issues.empty() ? issues.front().key : "Assets.WriteFailed");
                NotifyToolkitStatus(state, state.text.GetText(errKey));
                return false;
            }
            std::vector<AssetDiagnostic> scanDiag;
            state.assets.Scan(scanDiag);
            std::vector<EditorAssetIssue> syncIssues;
            EditorAssetActions::SyncProjectAssets(state.root, state.assets, syncIssues);
            NotifyToolkitAssetPathChanged(state, src, dst);
            state.pendingAsset = asset_io::Utf8(dst);
            NotifyToolkitStatus(state, state.text.GetText("EditorUi.AssetRenamed") + ": " + finalFileName);
            return true;
        }
        std::filesystem::rename(src, dst, ec);
        if (ec) return false;
        NotifyToolkitAssetPathChanged(state, src, dst);
        state.pendingAsset = asset_io::Utf8(dst);
        NotifyToolkitStatus(state, state.text.GetText("EditorUi.AssetRenamed") + ": " + finalFileName);
        return true;
    });

    state.shell->SetAssetFolderDropHandler([&state](const std::string& assetPathUtf8, const std::string& folderTarget) -> bool {
        if (state.session.IsPlaying() || state.host.ExportBusy()) return false;
        const std::filesystem::path src = asset_io::FromUtf8(assetPathUtf8);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(src, ec) || ec) return false;
        std::filesystem::path dstFolder;
        if (folderTarget.starts_with("@crumb:"))
        {
            const std::string rel = folderTarget.substr(7);
            if (rel.empty())
            {
                dstFolder = asset_io::Within(state.root / "Scripts", src) ? (state.root / "Scripts")
                    : asset_io::Within(state.root / "Scenes", src) ? (state.root / "Scenes")
                    : state.assetsRoot;
            }
            else if (rel == "Scripts" || rel.starts_with("Scripts/"))
            {
                dstFolder = state.root / asset_io::FromUtf8(rel);
            }
            else if (rel == "Scenes" || rel.starts_with("Scenes/"))
            {
                dstFolder = state.root / asset_io::FromUtf8(rel);
            }
            else
            {
                dstFolder = state.assetsRoot / asset_io::FromUtf8(rel);
            }
        }
        else
        {
            dstFolder = asset_io::FromUtf8(folderTarget);
        }
        if (!std::filesystem::is_directory(dstFolder, ec) || ec) return false;
        const std::filesystem::path dst = dstFolder / src.filename();
        if (dst == src || (std::filesystem::equivalent(src.parent_path(), dstFolder, ec) && !ec)) return false;

        if (asset_io::Within(state.assetsRoot, src) && asset_io::Within(state.assetsRoot, dstFolder))
        {
            std::vector<EditorAssetDocument> editing;
            if (!state.host.CaptureEditingDocuments(state.session, state.assets, &state.ui, editing)) return false;
            const auto plan = EditorAssetActions::Plan(
                {EditorAssetAction::Move, state.root, state.assets.SourceRoot(), src, dst, editing},
                state.assets);
            std::vector<EditorAssetIssue> issues;
            if (!plan.valid || !EditorAssetActions::Execute(plan, state.assets, editing, issues))
            {
                const std::string errKey = !plan.diagnostics.empty() ? plan.diagnostics.front().key
                    : (!issues.empty() ? issues.front().key : "Assets.WriteFailed");
                NotifyToolkitStatus(state, state.text.GetText(errKey));
                return false;
            }
            std::vector<AssetDiagnostic> scanDiag;
            state.assets.Scan(scanDiag);
            std::vector<EditorAssetIssue> syncIssues;
            EditorAssetActions::SyncProjectAssets(state.root, state.assets, syncIssues);
            NotifyToolkitAssetPathChanged(state, src, dst);
            state.pendingAsset = asset_io::Utf8(dst);
            NotifyToolkitStatus(state, state.text.GetText("EditorUi.AssetMoved") + ": " + asset_io::Utf8(dst.filename()));
            return true;
        }
        if (src.extension() == L".cs" && asset_io::Within(state.root / "Scripts", dstFolder))
        {
            if (std::filesystem::exists(dst, ec))
            {
                NotifyToolkitStatus(state, state.text.GetText("Assets.DestinationExists"));
                return false;
            }
            std::filesystem::rename(src, dst, ec);
            if (ec) return false;
            state.scriptWorkflow.RefreshSources();
            state.pendingAsset = asset_io::Utf8(dst);
            NotifyToolkitStatus(state, state.text.GetText("EditorUi.AssetMoved") + ": " + asset_io::Utf8(dst.filename()));
            return true;
        }
        return false;
    });

    state.shell->SetAssetContextActionHandler([&state, &ctx, isCsScriptPath](const std::string& pathUtf8, std::string_view action) {
        const std::filesystem::path target = asset_io::FromUtf8(pathUtf8);
        std::error_code ec;
        if (action == "explorer")
        {
            if (std::filesystem::exists(target, ec))
            {
                const bool isDir = std::filesystem::is_directory(target, ec);
                state.host.OpenInExplorer(target, !isDir);
            }
            return;
        }
        if (action == "delete")
        {
            if (state.session.IsPlaying() || state.host.ExportBusy()) return;
            if (std::filesystem::is_directory(target, ec) && !ec)
            {
                std::error_code eqEc;
                if ((std::filesystem::equivalent(target, state.assetsRoot, eqEc) && !eqEc) ||
                    (std::filesystem::equivalent(target, state.root / "Scenes", eqEc) && !eqEc) ||
                    (std::filesystem::equivalent(target, state.root / "Scripts", eqEc) && !eqEc))
                {
                    return;
                }
                if (!std::filesystem::is_empty(target, ec))
                {
                    NotifyToolkitStatus(state, state.text.GetText("EditorUi.FolderNotEmpty"));
                    return;
                }
                if (std::filesystem::remove(target, ec) && !ec)
                {
                    NotifyToolkitAssetPathChanged(state, target, {});
                    ClearToolkitAssetSelectionState(state);
                    NotifyToolkitStatus(state, state.text.GetText("EditorUi.FolderDeleted") + ": " +
                                               asset_io::Utf8(target.filename()));
                }
                return;
            }
            if (asset_io::Within(state.root / "Scripts", target) && isCsScriptPath(target))
            {
                state.shell->SelectAsset(pathUtf8);
                (void)state.shell->Commands().Execute({EditorCommandId::Delete, {}});
                return;
            }
            if (asset_io::Within(state.assetsRoot, target))
            {
                state.shell->SelectAsset(pathUtf8);
                state.assetState.assetPath = target;
                if (auto propHandler = [&state, &ctx]() {
                        std::vector<EditorAssetDocument> editing;
                        if (!ctx.host.CaptureEditingDocuments(ctx.session, ctx.assets, &ctx.ui, editing)) return;
                        const auto plan = EditorAssetActions::Plan(
                            {EditorAssetAction::Delete, ctx.root, ctx.assets.SourceRoot(), ctx.assetState.assetPath, {},
                             std::move(editing)},
                            ctx.assets);
                        std::string detail = plan.valid
                            ? ctx.text.GetText("Editor.AssetDeleteConfirm")
                            : ctx.text.GetText(plan.diagnostics.empty() ? "Editor.AssetImportFailed" : plan.diagnostics.front().key);
                        ctx.shell.ShowConfirm(
                            ctx.text.GetText("Editor.AssetDelete"), detail, ctx.text.GetText("Editor.AssetConfirm"), {},
                            ctx.text.GetText("EditorUi.Cancel"),
                            [plan, &state, &ctx]() {
                                std::vector<EditorAssetDocument> currentEditing;
                                ctx.host.CaptureEditingDocuments(ctx.session, ctx.assets, &ctx.ui, currentEditing);
                                std::vector<EditorAssetIssue> issues;
                                if (!EditorAssetActions::Execute(plan, ctx.assets, currentEditing, issues))
                                {
                                    NotifyToolkitStatus(state, ctx.text.GetText(issues.empty() ? "Editor.AssetImportFailed" : issues.front().key));
                                    return;
                                }
                                std::vector<AssetDiagnostic> scanDiag;
                                ctx.assets.Scan(scanDiag);
                                std::vector<EditorAssetIssue> syncIssues;
                                EditorAssetActions::SyncProjectAssets(ctx.root, ctx.assets, syncIssues);
                                NotifyToolkitAssetPathChanged(state, plan.request.source, {});
                                ClearToolkitAssetSelectionState(state);
                                NotifyToolkitStatus(state, ctx.text.GetText("Editor.AssetActionSucceeded"));
                            },
                            {}, plan.valid);
                    }; true)
                {
                    propHandler();
                }
            }
        }
    });
    state.shell->SetAssetDragHandler([&state, isCsScriptPath](const std::string& path, float x, float y, bool active) {
        if (!active || !state.camera || state.session.IsPlaying())
        {
            state.dropWorkflow.ClearHover();
            state.hoverMaterialEntity = 0;
            state.hoverMaterialAssetId.clear();
            return;
        }
        float u = 0.0f, v = 0.0f;
        ui::editor::ViewportNormalize({state.viewportRect.x, state.viewportRect.y,
                                       state.viewportRect.width, state.viewportRect.height}, x, y, u, v);
        const std::filesystem::path dragPath = asset_io::FromUtf8(path);
        if (IsMaterialDocumentPath(dragPath))
        {
            state.dropWorkflow.ClearHover();
            const StableEntityId picked = PickEditorBounds(state.session.EditWorld(), state.assets, *state.camera, u, v);
            const EntityHandle pickedH = state.session.EditWorld().FindByStableId(picked);
            if (picked != 0 && state.session.EditWorld().GetMesh(pickedH) != nullptr)
            {
                AssetId matId;
                for (const auto& id : state.assets.ListIds())
                {
                    std::error_code ec;
                    if (state.assets.Resolve(id) == dragPath ||
                        (std::filesystem::equivalent(state.assets.Resolve(id), dragPath, ec) && !ec))
                    { matId = id; break; }
                }
                state.hoverMaterialEntity = matId.empty() ? 0 : picked;
                state.hoverMaterialAssetId = std::move(matId);
            }
            else
            {
                state.hoverMaterialEntity = 0;
                state.hoverMaterialAssetId.clear();
            }
            return;
        }
        state.hoverMaterialEntity = 0;
        state.hoverMaterialAssetId.clear();
        if (isCsScriptPath(dragPath))
        {
            state.dropWorkflow.ClearHover();
            return;
        }
        state.dropWorkflow.BeginOrUpdateHover(path, *state.camera, {u, v},
            state.session.EditWorld(), state.assets, &state.session.EditEnvironment().Spatial());
    });
    state.shell->SetAssetDropHandler([&state, applyAssetToEntity, isCsScriptPath](const std::string& path, float x, float y) {
        state.hoverMaterialEntity = 0;
        state.hoverMaterialAssetId.clear();
        if (!state.camera || state.session.IsPlaying())
        {
            state.dropWorkflow.ClearHover();
            return false;
        }
        float u = 0.0f, v = 0.0f;
        ui::editor::ViewportNormalize({state.viewportRect.x, state.viewportRect.y,
                                       state.viewportRect.width, state.viewportRect.height}, x, y, u, v);
        const std::filesystem::path targetPath = asset_io::FromUtf8(path);
        if (isCsScriptPath(targetPath) || IsMaterialDocumentPath(targetPath))
        {
            state.dropWorkflow.ClearHover();
            StableEntityId picked = PickEditorBounds(state.session.EditWorld(), state.assets, *state.camera, u, v);
            if (picked == 0 && isCsScriptPath(targetPath)) picked = state.session.Selected();
            return applyAssetToEntity(path, picked, EditorPanelId::Viewport);
        }
        std::string error;
        StableEntityId created = 0;
        if (!state.dropWorkflow.CommitDrop(path, *state.camera, {u, v},
                                           state.session, state.assets, error, state.hooks, &created))
        {
            const std::string msg = LocalizedToolkitStatus(state.text, error, "EditorUi.DropFailed");
            state.shell->SetStatus(msg);
            state.shell->AppendConsoleLine("[Error] " + msg);
            return false;
        }
        state.assetState.assetName.clear();
        state.shell->FocusPanel(EditorPanelId::Viewport);
        Log(LogLevel::Info, "EditorUi.ToolkitDrop: " + std::to_string(created));
        return true;
    });
    state.shell->SetViewportActionHandler([&state](std::string_view action) {
        state.viewportController.Action(action, state.session, state.assets);
    });
    state.shell->SetReparentHandler([&state](std::uint64_t id, std::uint64_t parent) {
        if (!state.session.BeginEdit("Reparent")) return false;
        if (!state.session.SetParent(id, parent, ReparentMode::KeepWorld) &&
            state.session.LastError() == "Editor.TransformNotRepresentable")
        {
            (void)state.session.SetParent(id, parent, ReparentMode::KeepLocal);
        }
        if (!state.session.EditWorld().FindByStableId(id)) { state.session.CancelEdit(); return false; }
        const auto targetParent = parent == 0 ? EntityHandle{} : state.session.EditWorld().FindByStableId(parent);
        if (state.session.EditWorld().Get(state.session.EditWorld().FindByStableId(id))->parent != targetParent)
        {
            state.session.CancelEdit();
            return false;
        }
        const bool committed = state.session.CommitEdit();
        if (committed) Log(LogLevel::Info, "EditorUi.ToolkitReparent: " + std::to_string(id) + " -> " + std::to_string(parent));
        return committed;
    });

    auto& commands = state.shell->Commands();
    const auto saveAll = [&state, &ctx]() {
        const bool isAutomatedSmoke = state.autoExportFlow ||
            GetEnvironmentVariableW(L"FLARE_EDITOR_SMOKE_WORKFLOW", nullptr, 0) > 0 ||
            GetEnvironmentVariableW(L"FLARE_EDITOR_SMOKE_PLAYBACK", nullptr, 0) > 0 ||
            GetEnvironmentVariableW(L"FLARE_EDITOR_SMOKE_EXPORT", nullptr, 0) > 0;
        if (state.session.Path().empty() && state.session.Dirty() && !state.hidden && state.hostWindow != nullptr && !isAutomatedSmoke)
        {
            if (!PromptSaveSceneAsDialog(state)) return false;
        }
        if (state.shaderGraph && state.shaderGraph->IsOpen()) (void)state.shaderGraph->Save();
        const bool ok = SaveAllToolkitDocuments(ctx);
        if (ok)
        {
            const std::string sceneName = state.session.Path().empty()
                ? std::string("Scene")
                : asset_io::Utf8(state.session.Path().filename());
            NotifyToolkitStatus(state, state.text.GetText("EditorUi.Saved") + ": " + sceneName);
        }
        return ok;
    };
    commands.Register(EditorCommandId::Save, [saveAll](const EditorCommand&) { return saveAll(); });
    commands.Register(EditorCommandId::SaveAll, [&state, saveAll](const EditorCommand&) {
        const bool ok = saveAll();
        if (ok) NotifyToolkitStatus(state, state.text.GetText("EditorUi.AllSaved"));
        return ok;
    });
    commands.Register(EditorCommandId::Open, [&state, &ctx](const EditorCommand&) {
        if (state.session.IsPlaying())
        {
            NotifyToolkitStatus(state, state.text.GetText("Editor.EditWhilePlaying"));
            return false;
        }
        ConfirmIfDirtyThen(state, ctx, [&state]() {
            state.pendingDialogCommand = EditorCommandId::Open;
        });
        return true;
    });
    commands.Register(EditorCommandId::NewScene, [&state, &ctx](const EditorCommand&) {
        if (state.session.IsPlaying())
        {
            NotifyToolkitStatus(state, state.text.GetText("Editor.EditWhilePlaying"));
            return false;
        }
        ConfirmIfDirtyThen(state, ctx, [&state]() {
            std::string error;
            if (!state.session.NewDefaultScene(error))
            {
                NotifyToolkitStatus(state, LocalizedToolkitStatus(state.text, error, "EditorUi.SaveFailed"));
                return;
            }
            ClearToolkitAssetSelectionState(state);
            state.externalChange.Reset(state.session.Path());
            state.shell->FocusPanel(EditorPanelId::Viewport);
            NotifyToolkitStatus(state, state.text.GetText("EditorUi.NewSceneCreated"));
            Log(LogLevel::Info, "EditorUi.ToolkitScene: new");
        });
        return true;
    });
    commands.Register(EditorCommandId::SaveAs, [&state](const EditorCommand&) {
        if (state.session.IsPlaying())
        {
            NotifyToolkitStatus(state, state.text.GetText("Editor.EditWhilePlaying"));
            return false;
        }
        state.pendingDialogCommand = EditorCommandId::SaveAs;
        return true;
    });
    state.exportAction = [&state, saveAll]() {
        if (state.host.ExportBusy())
        {
            state.host.RequestCancelExport();
            Log(LogLevel::Info, "EditorUi.ToolkitExport: cancel");
            state.shell->SetFormStatus(EditorPanelId::BuildExport, state.text.GetText("EditorUi.CancellingExport"));
            return true;
        }
        if (!saveAll())
        {
            state.shell->SetFormStatus(EditorPanelId::BuildExport, state.text.GetText("EditorUi.SaveFailed"));
            return false;
        }
        std::string error;
        state.exportStarted = false;
        state.exportFinished = false;
        const std::string base = state.exportName.empty() ? "GameExport" : state.exportName;
        const auto packageExists = [](const std::string& candidate) {
            const auto packages = std::filesystem::current_path() / "packages";
            return std::filesystem::exists(packages / candidate) ||
                std::filesystem::exists(std::filesystem::current_path() / ".cache" / "packages" / candidate) ||
                std::filesystem::exists(std::filesystem::current_path() / "out" / "packages" / candidate);
        };
        std::string exportName = base;
        for (int suffix = 2; packageExists(exportName) && suffix < 1000; ++suffix) exportName = base + "-" + std::to_string(suffix);
        if (!state.host.StartExport(exportName, error))
        {
            state.shell->SetFormStatus(EditorPanelId::BuildExport,
                LocalizedToolkitStatus(state.text, error, "EditorUi.ExportFailed"));
            return false;
        }
        state.shell->SetFormStatus(EditorPanelId::BuildExport, state.text.GetText("EditorUi.ExportStarted"));
        state.exportStarted = true;
        Log(LogLevel::Info, "EditorUi.ToolkitExport: start");
        return true;
    };
    commands.Register(EditorCommandId::Export, [&state](const EditorCommand&) { return state.exportAction(); });
    commands.Register(EditorCommandId::Build, [&state, saveAll](const EditorCommand& command) {
        if (state.session.IsPlaying())
        {
            NotifyToolkitStatus(state, state.text.GetText("Editor.EditWhilePlaying"));
            return false;
        }
        std::uint64_t pendingEntity = 0;
        std::string pendingScriptId;
        if (const auto sep = command.argument.find('|'); sep != std::string::npos)
        {
            pendingEntity = static_cast<std::uint64_t>(std::strtoull(command.argument.c_str(), nullptr, 10));
            pendingScriptId = command.argument.substr(sep + 1);
        }
        if (state.scriptWorkflow.CanHotReloadInProcess())
        {
            if (state.scriptWorkflow.IsCompiling())
            {
                NotifyToolkitStatus(state, state.text.GetText("EditorUi.ScriptBuildBusy"));
                return false;
            }
            if (EditorScriptWorkflow::EnsureProjectBindings(state.root))
            {
                state.host.LoadBuildSettings(true);
                state.shell->SetFormProperties(EditorPanelId::BuildExport,
                    BuildBuildSettingsRows(state.host.Build(), state.exportName, state.text));
            }
            if (!saveAll())
            {
                state.shell->SetStatus(state.text.GetText("EditorUi.SaveFailed"));
                return false;
            }
            if (!state.scriptWorkflow.StartInProcessCompile(pendingEntity, pendingScriptId))
            {
                const std::string err = state.scriptWorkflow.Status().empty()
                    ? state.text.GetText("EditorUi.ScriptCompileFailed")
                    : state.scriptWorkflow.Status();
                NotifyToolkitStatus(state, err, false);
                state.shell->AppendConsoleLine("[Error] " + err);
                return false;
            }
            const std::string compilingMsg = state.text.GetText("EditorUi.ScriptHotReloading");
            NotifyToolkitStatus(state, compilingMsg, false);
            state.shell->AppendConsoleLine("[Info] " + compilingMsg);
            Log(LogLevel::Info, "EditorUi.ToolkitBuild: hot-reload started");
            return true;
        }
        wchar_t requestPath[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"FLARE_EDITOR_RESTART_REQUEST", requestPath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            state.shell->SetStatus(state.text.GetText(
                pendingEntity != 0 ? "EditorUi.ScriptNeedsCompile" : "EditorUi.BuildControlUnavailable"));
            return false;
        }
        EditorScriptWorkflow::EnsureProjectBindings(state.root);
        if (!saveAll()) { state.shell->SetStatus(state.text.GetText("EditorUi.SaveFailed")); return false; }
        PersistToolkitSession(state, true);
        std::string error;
        if (!WriteBuildRestartRequest(requestPath, error))
        {
            state.shell->SetStatus(LocalizedToolkitStatus(state.text, error, "EditorUi.SaveFailed"));
            return false;
        }
        if (pendingEntity != 0 && !pendingScriptId.empty())
        {
            EditorScriptWorkflow::QueuePendingAttach(state.crashDirectory, pendingEntity, pendingScriptId);
        }
        state.shell->SetStatus(state.text.GetText(
            pendingEntity != 0 ? "EditorUi.ScriptQueuedCompile" : "EditorUi.BuildRestart"));
        Log(LogLevel::Info, "EditorUi.ToolkitBuild: restart requested");
        if (state.hostWindow) PostMessageW(state.hostWindow, WM_CLOSE, 0, 0);
        return true;
    });
    commands.Register(EditorCommandId::Undo, [&state](const EditorCommand&) {
        if (!state.session.IsPlaying() && state.shell->ActivePanel() == EditorPanelId::ShaderGraph &&
            state.shaderGraph && state.shaderGraph->IsOpen())
        {
            return state.shaderGraph->Undo();
        }
        if (state.assetState.inspectUi && !state.session.IsPlaying()) return state.host.Ui().Undo(state.ui);
        if (state.assetState.editTod && !state.session.IsPlaying()) return state.host.TimeOfDayPreview().Undo();
        if (!state.session.IsPlaying() &&
            (state.assetState.preferMaterialUndo || state.shell->ActivePanel() == EditorPanelId::Material) &&
            state.host.Material().Undo())
        {
            (void)state.host.SaveMaterial(state.assets);
            state.shell->SetFormProperties(EditorPanelId::Material, BuildMaterialFormRows(state.host.Material(), state.text, &state.assets));
            if (!state.assetState.assetPath.empty() && IsMaterialDocumentPath(state.assetState.assetPath))
                state.shell->SelectAsset(asset_io::Utf8(state.assetState.assetPath));
            return true;
        }
        if (!state.session.IsPlaying() && state.assetState.preferScriptUndo && state.scriptWorkflow.HasUndo())
            return state.scriptWorkflow.Undo();
        if (state.session.Undo()) return true;
        if (!state.session.IsPlaying() && state.scriptWorkflow.HasUndo())
            return state.scriptWorkflow.Undo();
        return false;
    });
    commands.Register(EditorCommandId::Redo, [&state](const EditorCommand&) {
        if (!state.session.IsPlaying() && state.shell->ActivePanel() == EditorPanelId::ShaderGraph &&
            state.shaderGraph && state.shaderGraph->IsOpen())
        {
            return state.shaderGraph->Redo();
        }
        if (state.assetState.inspectUi && !state.session.IsPlaying()) return state.host.Ui().Redo(state.ui);
        if (state.assetState.editTod && !state.session.IsPlaying()) return state.host.TimeOfDayPreview().Redo();
        if (!state.session.IsPlaying() &&
            (state.assetState.preferMaterialUndo || state.shell->ActivePanel() == EditorPanelId::Material) &&
            state.host.Material().Redo())
        {
            (void)state.host.SaveMaterial(state.assets);
            state.shell->SetFormProperties(EditorPanelId::Material, BuildMaterialFormRows(state.host.Material(), state.text, &state.assets));
            if (!state.assetState.assetPath.empty() && IsMaterialDocumentPath(state.assetState.assetPath))
                state.shell->SelectAsset(asset_io::Utf8(state.assetState.assetPath));
            return true;
        }
        if (!state.session.IsPlaying() && state.assetState.preferScriptUndo && state.scriptWorkflow.HasRedo())
            return state.scriptWorkflow.Redo();
        if (state.session.Redo()) return true;
        if (!state.session.IsPlaying() && state.scriptWorkflow.HasRedo())
            return state.scriptWorkflow.Redo();
        return false;
    });
    const auto stopPlayWithKeepChanges = [&state]() {
        std::vector<CapturedEntityPlayState> capturedEntities;
        std::vector<std::pair<StableEntityId, std::string>> capturedScripts;
        if (!state.assetState.playKeepEntities.empty() && state.session.PlayWorld())
        {
            for (const StableEntityId keepId : state.assetState.playKeepEntities)
            {
                CapturedEntityPlayState snap;
                if (state.session.CaptureEntityPlayState(keepId, snap))
                    capturedEntities.push_back(std::move(snap));
                std::string scriptDoc = state.scriptWorkflow.CaptureLiveEntityScripts(keepId);
                if (!scriptDoc.empty())
                    capturedScripts.emplace_back(keepId, std::move(scriptDoc));
            }
        }
        state.assetState.playKeepEntities.clear();
        if (state.shell) state.shell->SetPlayKeepArmed(false);
        state.session.StopPlay();
        if (!capturedEntities.empty() || !capturedScripts.empty())
        {
            bool anyApplied = false;
            if (!capturedEntities.empty() && state.session.ApplyCapturedPlayStates(capturedEntities, &state.assets))
                anyApplied = true;
            if (!capturedScripts.empty())
            {
                const bool tx = state.scriptWorkflow.BeginEdit();
                bool anyScript = false;
                for (const auto& [sid, doc] : capturedScripts)
                {
                    if (state.scriptWorkflow.ApplyCapturedEntityScripts(sid, doc)) anyScript = true;
                }
                if (tx)
                {
                    if (anyScript) (void)state.scriptWorkflow.CommitEdit();
                    else (void)state.scriptWorkflow.CancelEdit();
                }
                if (anyScript) anyApplied = true;
            }
            if (anyApplied)
            {
                state.assetState.preferScriptUndo = false;
                state.assetState.preferMaterialUndo = false;
                NotifyToolkitStatus(state, state.text.GetText("EditorUi.PlayChangesApplied"));
            }
        }
    };
    commands.Register(EditorCommandId::Play, [&state, stopPlayWithKeepChanges](const EditorCommand&) {
        if (state.assetState.previewAudioVoice)
        {
            state.audio.Stop(state.assetState.previewAudioVoice);
            state.assetState.previewAudioVoice = {};
        }
        if (state.session.IsPlaying())
        {
            state.pendingPlayAfterCompile = false;
            stopPlayWithKeepChanges();
            Log(LogLevel::Info, "EditorUi.ToolkitPlay: stop");
            return true;
        }
        if (state.importTask.valid()) return false;
        if (state.scriptWorkflow.IsCompiling())
        {
            state.pendingPlayAfterCompile = true;
            const std::string waitMsg = state.text.GetText("EditorUi.PlayWaitingForCompile");
            NotifyToolkitStatus(state, waitMsg, false);
            state.shell->AppendConsoleLine("[Info] " + waitMsg);
            return true;
        }
        if (state.scriptWorkflow.CanHotReloadInProcess())
        {
            state.scriptWorkflow.RefreshSources();
            if (!state.scriptWorkflow.Sources().empty() && state.scriptWorkflow.SourcesNeedCompile())
            {
                state.pendingPlayAfterCompile = true;
                const std::string waitMsg = state.text.GetText("EditorUi.PlayWaitingForCompile");
                NotifyToolkitStatus(state, waitMsg, false);
                (void)state.shell->Commands().Execute({EditorCommandId::Build, {}});
                return true;
            }
            if (state.scriptWorkflow.HasCompileError())
            {
                const std::string errMsg = state.text.GetText("EditorUi.PlayBlockedByCompileError");
                NotifyToolkitStatus(state, errMsg, false);
                state.shell->AppendConsoleLine("[Error] " + errMsg);
                state.shell->FocusPanel(EditorPanelId::Console);
                return false;
            }
        }
        std::string reason;
        if (!state.session.ValidatePlay(reason)) { Log(LogLevel::Warning, "EditorUi.ToolkitPlay: " + reason); return false; }
        if (state.shell->ConsoleClearOnPlay()) state.shell->ClearConsole();
        state.assetState.playKeepEntities.clear();
        state.shell->SetPlayKeepArmed(false);
        if (!state.session.StartPlay()) { Log(LogLevel::Warning, "EditorUi.ToolkitPlay: " + state.session.LastError()); return false; }
        state.session.SetPlayPaused(false);
        state.gameplayLogged = false;
        Log(LogLevel::Info, "EditorUi.ToolkitPlay: start");
        return true;
    });
    commands.Register(EditorCommandId::Pause, [&state](const EditorCommand&) {
        if (!state.session.IsPlaying()) return false;
        const bool paused = state.session.PlayPaused();
        if (!state.session.SetPlayPaused(!paused)) return false;
        Log(LogLevel::Info, paused ? "EditorUi.ToolkitPlay: resume" : "EditorUi.ToolkitPlay: pause");
        return true;
    });
    commands.Register(EditorCommandId::Step, [&state](const EditorCommand&) {
        if (!state.session.IsPlaying() || !state.session.PlayPaused()) return false;
        state.session.RequestPlayStep();
        return true;
    });
    commands.Register(EditorCommandId::Stop, [&state, stopPlayWithKeepChanges](const EditorCommand&) {
        state.pendingPlayAfterCompile = false;
        if (!state.session.IsPlaying()) return false;
        stopPlayWithKeepChanges();
        Log(LogLevel::Info, "EditorUi.ToolkitPlay: stop");
        return true;
    });
    commands.Register(EditorCommandId::Delete, [&state, &ctx, isCsScriptPath](const EditorCommand&) {
        if (state.session.IsPlaying()) return false;
        if (state.shell->ActivePanel() == EditorPanelId::ShaderGraph &&
            state.shaderGraph && state.shaderGraph->IsOpen())
        {
            return state.shaderGraph->DeleteSelected();
        }
        if (state.assetState.inspectUi)
        {
            UiId selected = state.host.Ui().Selected();
            const bool result = state.host.Ui().Edit(state.ui, [&](UiTree& tree) {
                return ApplyUiInspector(tree, selected, "ui.delete", {}, selected);
            });
            if (result) state.host.Ui().Select(selected);
            return result;
        }
        if (!state.assetState.assetName.empty() && !state.assetState.assetPath.empty())
        {
            const std::filesystem::path target = state.assetState.assetPath;
            if (asset_io::Within(state.root / "Scripts", target) && isCsScriptPath(target))
            {
                const std::string fileName = asset_io::Utf8(target.filename());
                const std::string scriptId = state.scriptWorkflow.ScriptIdForSource(target);
                state.shell->ShowConfirm(
                    state.text.GetText("Editor.AssetDelete"),
                    state.text.GetText("EditorUi.ScriptDeleteConfirm") + ": " + fileName,
                    state.text.GetText("Editor.AssetConfirm"), {},
                    state.text.GetText("EditorUi.Cancel"),
                    [&state, target, scriptId, fileName]() {
                        if (!scriptId.empty() && state.hooks && state.hooks->scriptApply)
                        {
                            std::string ignoredErr;
                            const std::string cmd = "{\"PurgeType\":\"" + scriptId + "\"}";
                            (void)state.hooks->scriptApply(0, cmd, ignoredErr);
                            (void)state.scriptWorkflow.Save();
                        }
                        std::error_code ec;
                        if (std::filesystem::remove(target, ec) && !ec)
                        {
                            state.scriptWorkflow.RefreshSources();
                            state.assetsListDirty = true;
                            ClearToolkitAssetSelectionState(state);
                            const std::string msg = state.text.GetText("EditorUi.ScriptDeleted") + ": " + fileName;
                            NotifyToolkitStatus(state, msg, false);
                            state.shell->AppendConsoleLine("[Info] " + msg);
                            if (state.scriptWorkflow.CanHotReloadInProcess() && !state.scriptWorkflow.Sources().empty())
                                (void)state.shell->Commands().Execute({EditorCommandId::Build, {}});
                        }
                    },
                    {}, true);
                return true;
            }
            if (asset_io::Within(state.assetsRoot, target))
            {
                std::vector<EditorAssetDocument> editing;
                if (!ctx.host.CaptureEditingDocuments(ctx.session, ctx.assets, &ctx.ui, editing)) return false;
                const auto plan = EditorAssetActions::Plan(
                    {EditorAssetAction::Delete, ctx.root, ctx.assets.SourceRoot(), target, {}, std::move(editing)},
                    ctx.assets);
                const std::string detail = plan.valid
                    ? ctx.text.GetText("Editor.AssetDeleteConfirm")
                    : ctx.text.GetText(plan.diagnostics.empty() ? "Editor.AssetImportFailed" : plan.diagnostics.front().key);
                ctx.shell.ShowConfirm(
                    ctx.text.GetText("Editor.AssetDelete"), detail, ctx.text.GetText("Editor.AssetConfirm"), {},
                    ctx.text.GetText("EditorUi.Cancel"),
                    [plan, &state, &ctx]() {
                        std::vector<EditorAssetDocument> currentEditing;
                        ctx.host.CaptureEditingDocuments(ctx.session, ctx.assets, &ctx.ui, currentEditing);
                        std::vector<EditorAssetIssue> issues;
                        if (!EditorAssetActions::Execute(plan, ctx.assets, currentEditing, issues))
                        {
                            NotifyToolkitStatus(state, ctx.text.GetText(issues.empty() ? "Editor.AssetImportFailed" : issues.front().key));
                            return;
                        }
                        std::vector<AssetDiagnostic> scanDiag;
                        ctx.assets.Scan(scanDiag);
                        std::vector<EditorAssetIssue> syncIssues;
                        EditorAssetActions::SyncProjectAssets(ctx.root, ctx.assets, syncIssues);
                        NotifyToolkitAssetPathChanged(state, plan.request.source, {});
                        ClearToolkitAssetSelectionState(state);
                        NotifyToolkitStatus(state, ctx.text.GetText("Editor.AssetActionSucceeded"));
                    },
                    {}, plan.valid);
                return true;
            }
            return false;
        }
        const auto sel = state.session.Selection();
        if (sel.size() > 1)
        {
            if (!state.session.BeginEdit("Editor.Delete")) return false;
            bool deletedAny = false;
            for (const StableEntityId sid : sel)
            {
                if (state.session.EditWorld().FindByStableId(sid) && state.session.Delete(sid))
                    deletedAny = true;
            }
            if (deletedAny) return state.session.CommitEdit();
            state.session.CancelEdit();
            return false;
        }
        return state.session.Selected() && state.session.Delete(state.session.Selected());
    });
    commands.Register(EditorCommandId::Duplicate, [&state](const EditorCommand&) {
        if (!state.session.IsPlaying() && state.shell->ActivePanel() == EditorPanelId::ShaderGraph &&
            state.shaderGraph && state.shaderGraph->IsOpen())
        {
            return state.shaderGraph->DuplicateSelected();
        }
        if (state.assetState.inspectUi || !state.assetState.assetName.empty()) return false;
        const auto sel = state.session.Selection();
        std::unordered_map<StableEntityId, StableEntityId> idRemap;
        if (sel.size() > 1)
        {
            if (!state.session.BeginEdit("Editor.Duplicate")) return false;
            std::vector<StableEntityId> createdIds;
            const World& ew = state.session.EditWorld();
            std::vector<StableEntityId> topRoots;
            for (const StableEntityId sid : sel)
            {
                const EntityHandle eh = ew.FindByStableId(sid);
                if (!eh) continue;
                bool ancSelected = false;
                for (EntityHandle cur = ew.Get(eh) ? ew.Get(eh)->parent : EntityHandle{}; cur;
                     cur = ew.Get(cur) ? ew.Get(cur)->parent : EntityHandle{})
                {
                    if (ew.Get(cur) && state.session.IsSelected(ew.Get(cur)->stableId)) { ancSelected = true; break; }
                }
                if (!ancSelected) topRoots.push_back(sid);
            }
            for (const StableEntityId rootId : topRoots)
            {
                if (const StableEntityId dup = state.session.Duplicate(rootId, &idRemap); dup != 0)
                    createdIds.push_back(dup);
            }
            if (!createdIds.empty())
            {
                state.session.SetSelection(createdIds, createdIds.back());
                const bool committed = state.session.CommitEdit();
                if (committed && !idRemap.empty()) (void)state.scriptWorkflow.CloneEntityBindings(idRemap);
                return committed;
            }
            state.session.CancelEdit();
            return false;
        }
        if (state.session.Selected() == 0) return false;
        const StableEntityId dup = state.session.Duplicate(state.session.Selected(), &idRemap);
        if (dup == 0) return false;
        if (!idRemap.empty()) (void)state.scriptWorkflow.CloneEntityBindings(idRemap);
        return true;
    });
    commands.Register(EditorCommandId::Import, [&state](const EditorCommand&) {
        if (!state.hooks || !state.hooks->chooseAssetImportSource)
        {
            state.pendingDialogCommand = EditorCommandId::Import;
            return true;
        }
        return BrowseAndImportToolkitAsset(state);
    });
    commands.Register(EditorCommandId::CreateEntity, [&state](const EditorCommand& command) {
        if (!CreateEditorObject(state.session, command.argument, command.target, &state.text)) return false;
        state.assetState.assetName.clear();
        Log(LogLevel::Info, "EditorUi.ToolkitCreated: " + command.argument + " " + std::to_string(state.session.Selected()));
        return true;
    });
    commands.Register(EditorCommandId::CreateAsset, [&state](const EditorCommand& command) {
        if (state.session.IsPlaying() || state.importTask.valid() || state.host.ExportBusy())
        {
            state.shell->SetStatus(state.text.GetText("Editor.AssetOperationBusy"));
            return false;
        }
        if (command.argument == "prefab")
        {
            const StableEntityId targetId = command.target != 0 ? command.target : state.session.Selected();
            std::filesystem::path created;
            AssetId createdId;
            std::string error;
            const bool saved = CreatePrefabFromSelection(
                state.session, state.assets, state.root, targetId, {}, created, createdId, error, state.hooks);
            if (saved)
            {
                state.pendingAsset = asset_io::Utf8(created);
                const std::string msg = state.text.GetText("EditorUi.PrefabCreated") + ": " + asset_io::Utf8(created.filename());
                state.shell->SetStatus(msg);
                state.shell->AppendConsoleLine(msg);
            }
            else
            {
                const std::string msg = LocalizedToolkitStatus(state.text, error, "EditorUi.SaveFailed");
                state.shell->SetStatus(msg);
                state.shell->AppendConsoleLine("[Error] " + msg);
            }
            return saved;
        }
        if (command.argument == "navmesh")
        {
            const auto outPath = state.assetsRoot / L"Navigation" / L"scene.senav";
            std::string bakeError;
            const bool started = state.navigationBake.StartBake(
                state.session.EditWorld(),
                state.assets,
                outPath,
                NavBakeSettings{},
                [&state](const NavigationBakeResult& result) {
                    if (!result.success)
                    {
                        const std::string msg = LocalizedToolkitStatus(
                            state.text, result.errorKey, "Navigation.BakeFailed");
                        if (state.shell)
                        {
                            state.shell->SetStatus(msg);
                            state.shell->AppendConsoleLine("[Error] " + msg);
                        }
                        return;
                    }
                    std::vector<EditorAssetIssue> issues;
                    EditorAssetActions::SyncProjectAssets(state.root, state.assets, issues);
                    if (!state.session.IsPlaying() && !result.assetId.empty()
                        && NavigationBakeService::HasUnassignedNavAgent(state.session.EditWorld()))
                    {
                        state.session.Apply("Navigation.BakeScene", [&result](World& world) {
                            return NavigationBakeService::MountBakedNavMeshToWorld(world, result.assetId) > 0u;
                        });
                    }
                    state.pendingAsset = asset_io::Utf8(result.outputPath);
                    const std::string msg = state.text.GetText("Navigation.BakeSucceeded")
                        + ": " + asset_io::Utf8(result.outputPath.filename());
                    if (state.shell)
                    {
                        state.shell->SetStatus(msg);
                        state.shell->AppendConsoleLine(msg);
                    }
                },
                bakeError);
            if (!started)
            {
                const std::string msg = LocalizedToolkitStatus(state.text, bakeError, "Navigation.BakeFailed");
                state.shell->SetStatus(msg);
                state.shell->AppendConsoleLine("[Error] " + msg);
            }
            return started;
        }
        std::filesystem::path created;
        std::string error;
        const std::filesystem::path targetFolder = ResolveSelectedTargetFolder(state);
        const bool saved = CreateEditorAsset(command.argument, state.root, state.assets, created, error, targetFolder);
        if (!created.empty())
        {
            if (command.argument == "folder")
            {
                const std::string msg = state.text.GetText("EditorUi.FolderCreated") + ": " +
                                        asset_io::Utf8(created.filename());
                NotifyToolkitStatus(state, msg);
                state.pendingAsset = asset_io::Utf8(created);
                state.pendingRenameAssetPath = asset_io::Utf8(created);
            }
            else if (command.argument == "script")
            {
                EditorScriptWorkflow::EnsureProjectBindings(state.root);
                const std::string msg = state.text.GetText("Editor.ScriptCreated") + ": " +
                                        asset_io::Utf8(created.filename());
                NotifyToolkitStatus(state, msg);
            }
            else
            {
                NotifyToolkitStatus(state, state.text.GetText("EditorUi.Saved") + ": " +
                                           asset_io::Utf8(created.filename()));
            }
            if (command.argument == "shader-graph" && state.shaderGraph)
            {
                (void)state.shaderGraph->Open(created, true);
            }
            RefreshShaderWatchMtimes(state, false);
            state.scriptWorkflow.RefreshSources();
            state.pendingAsset = asset_io::Utf8(created);
        }
        if (!saved)
        {
            state.importStatus = LocalizedToolkitStatus(state.text, error, "Editor.AssetImportFailed");
            state.shell->SetStatus(state.importStatus);
            state.shell->AppendConsoleLine("[Error] " + state.importStatus);
        }
        return saved;
    });
    commands.Register(EditorCommandId::SetProperty, [&state](const EditorCommand& command) {
        if (command.argument == "@align-with-view")
        {
            if (!state.viewportController.Action("AlignWithView", state.session, state.assets))
                return false;
            state.assetState.preferScriptUndo = false;
            NotifyToolkitStatus(state, state.text.GetText("EditorUi.AlignedWithView"));
            return true;
        }
        const StableEntityId targetId = command.target != 0
            ? command.target
            : ((state.shell && state.shell->InspectorLocked() && state.assetState.lockedToEntity)
                   ? state.assetState.lockedEntityId
                   : state.session.Selected());
        if (command.argument.starts_with("prefab.applyProperty:"))
        {
            std::string error;
            if (!ApplyPropertyOverrideToPrefab(state.session, state.assets, state.root, targetId,
                                               std::string_view(command.argument).substr(21), error, state.hooks))
            {
                state.shell->SetStatus(LocalizedToolkitStatus(state.text, error, "EditorUi.SaveFailed"));
                return false;
            }
            const auto msg = state.text.GetText("EditorUi.PrefabApplied");
            state.shell->SetStatus(msg);
            state.shell->AppendConsoleLine(msg);
            return true;
        }
        if (!command.argument.starts_with("prefab.revertProperty:")) return false;
        std::string error;
        if (!RevertPropertyOverride(state.session, state.assets, targetId,
                                    std::string_view(command.argument).substr(22), error))
        {
            state.shell->SetStatus(LocalizedToolkitStatus(state.text, error, "EditorUi.SaveFailed"));
            return false;
        }
        state.shell->SetStatus(state.text.GetText("EditorUi.PrefabReverted"));
        return true;
    });
    commands.Register(EditorCommandId::Rename, [&state](const EditorCommand&) {
        return !state.assetState.inspectUi && state.shell && state.shell->BeginRename();
    });
    commands.Register(EditorCommandId::TogglePanel, [&state](const EditorCommand& command) {
        if (command.argument == "@maximize-on-play" ||
            command.argument == "@preview-windowed" ||
            command.argument == "@preview-fullscreen")
        {
            const bool next = command.argument == "@preview-fullscreen" ? true
                            : command.argument == "@preview-windowed"   ? false
                                                                        : !state.shell->MaximizeOnPlay();
            state.shell->SetMaximizeOnPlay(next);
            if (state.session.IsPlaying())
            {
                state.shell->SetGameMaximized(next);
                state.enteredMaximizedOnPlay = next;
            }
            return true;
        }
        if (command.argument.starts_with("@layout:"))
            return state.shell->ApplyLayoutPreset(std::string_view(command.argument).substr(8));
        if (command.argument == "ui-designer") state.assetState.inspectUi = true;
        EditorPanelId panel = EditorPanelId::None;
        if (!EditorPanelFromName(command.argument, panel)) return false;
        Log(LogLevel::Info, "EditorUi.ToolkitPanel: " + command.argument);
        if (IsUtilityPanel(panel))
            OpenToolkitUtilityWindow(state, panel);
        else
            state.shell->TogglePanel(panel);
        return true;
    });
    commands.Register(EditorCommandId::FocusViewport, [&state](const EditorCommand&) {
        const bool next = !state.shell->MaximizeOnPlay();
        state.shell->SetMaximizeOnPlay(next);
        if (state.session.IsPlaying())
        {
            state.shell->SetGameMaximized(next);
            state.enteredMaximizedOnPlay = next;
        }
        return true;
    });
    commands.Register(EditorCommandId::OpenSettings, [&state](const EditorCommand&) {
        Log(LogLevel::Info, "EditorUi.ToolkitPanel: project-settings");
        OpenToolkitUtilityWindow(state, EditorPanelId::ProjectSettings);
        return true;
    });
}

void ConfigureToolkitRecoveryPrompt(ToolkitAppState& state)
{
    if (state.host.RecoveryGroups().empty()) return;
    auto groups = std::make_shared<std::vector<RecoveryGroup>>(state.host.RecoveryGroups());
    const auto discardAll = [&state]() {
        state.host.FinishRecovery(true);
        state.host.DismissRecoveryOffer();
    };
    const auto recover = [&state, groups]() {
        const bool opened = state.host.RestoreRecovery(groups->front(), state.session, state.assets, state.ui);
        if (opened) state.externalChange.Reset(state.session.Path());
        state.host.FinishRecovery(true);
        state.host.DismissRecoveryOffer();
        state.shell->SetStatus(state.text.GetText(opened ? "EditorUi.RecoveryOpened" : "EditorUi.RecoveryFailed"));
        Log(LogLevel::Info, opened ? "EditorUi.ToolkitRecovery: opened" : "EditorUi.ToolkitRecovery: failed");
    };
    state.shell->ShowConfirm(
        state.text.GetText("EditorUi.RecoveryTitle"),
        state.text.GetText("EditorUi.RecoveryMessage") + " (" + std::to_string(groups->size()) + ")",
        state.text.GetText("EditorUi.Recover"), state.text.GetText("EditorUi.DiscardChanges"),
        state.text.GetText("EditorUi.Cancel"), recover, discardAll);
}

bool InitializeToolkitWindowUi(ToolkitAppState& state, HWND window)
{
    if (!ReportStartup(StartupStage::Interface)) return false;
    SetWindowTextW(window, asset_io::FromUtf8(state.text.GetText("Editor.WindowTitle")).c_str());
    flare::palette::StyleTitleBar(window);
    DragAcceptFiles(window, TRUE);
    state.hostWindow = window;
    state.fontService = std::make_unique<UiDirectWriteTextService>(L"Segoe UI");
    state.shell = std::make_unique<EditorShell>(state.fontService.get());
    state.shell->SetLocalization(&state.text);
    if (!state.shell->Initialize(1280.0f, 760.0f))
    {
        Log(LogLevel::Error, "EditorUi.ToolkitInitFailed");
        state.shell.reset();
        return false;
    }
    state.shell->SetViewportImage("scene");
    state.shell->SetLayoutPath(state.safeMode ? std::filesystem::path{} : state.crashDirectory / ".editor-ui.json");

    state.shaderGraph = std::make_unique<EditorShaderGraphController>(
        state.root, state.assets.CacheRoot(), state.assets, state.session, state.text);
    state.shaderGraph->Attach(*state.shell);
    state.shaderGraph->SetReloadCallback([&state]() {
        ++state.shaderReloadGeneration;
        state.lastLoggedShaderDiagnostic.clear();
        RefreshShaderWatchMtimes(state, false);
        if (state.shell && !state.assetState.assetPath.empty() && IsMaterialDocumentPath(state.assetState.assetPath))
            state.shell->SelectAsset(asset_io::Utf8(state.assetState.assetPath));
    });
    state.shaderGraph->SetOpenHlslCallback([&state](const std::filesystem::path& hlslPath) {
        CodeEditorSettings settings;
        settings.executable = asset_io::FromUtf8(state.host.Preferences().externalEditor);
        std::string err, detail;
        if (!EditorScriptTools::OpenSource(state.root, hlslPath, 1, 1, settings, err, detail))
            NotifyToolkitStatus(state, LocalizedToolkitStatus(state.text, err, "EditorUi.SaveFailed"));
    });
    state.shaderGraph->SetMaterialCreatedCallback([&state](const std::filesystem::path& matPath) {
        state.assetsListDirty = true;
        const std::string utf8 = asset_io::Utf8(matPath);
        state.pendingAsset = utf8;
        if (state.shell)
        {
            state.shell->SelectAsset(utf8);
            state.shell->PingAsset(utf8);
        }
        NotifyToolkitStatus(state, state.text.GetText("EditorUi.Saved") + ": " + asset_io::Utf8(matPath.filename()));
    });
    RefreshShaderWatchMtimes(state, false);

    {
        ProjectDescription description;
        std::vector<ProjectDiagnostic> projectDiagnostics;
        if (ReadProject(state.root / "project.json", description, projectDiagnostics) && !description.uiDocuments.empty())
        {
            const auto& uiPath = description.uiDocuments.front();
            std::ifstream input(uiPath, std::ios::binary);
            if (input)
            {
                const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
                std::string uiError;
                if (state.ui.LoadJson(json, uiError))
                {
                    state.assetState.explicitUiPath = uiPath;
                    state.host.SetDocumentPaths(state.session.Path(), uiPath);
                    state.host.Ui().MarkSaved(state.ui, uiPath);
                    Log(LogLevel::Info, "EditorUi.ToolkitUi: loaded " + asset_io::Utf8(uiPath));
                }
                else
                {
                    Log(LogLevel::Warning, "EditorUi.ToolkitUi: " + uiError);
                }
            }
        }
        else if (!state.host.Ui().EffectivePath().empty())
        {
            state.assetState.explicitUiPath = state.host.Ui().EffectivePath();
        }
    }

    ApplyStartupEnvironmentFlags(state);
    const UINT dpi = GetDpiForWindow(window);
    float uiScale = dpi ? static_cast<float>(dpi) / 96.0f : 1.0f;
    wchar_t dpiOverride[16]{};
    if (GetEnvironmentVariableW(L"FLARE_TEST_DPI_SCALE", dpiOverride, 16) > 0)
    {
        const float parsed = static_cast<float>(std::atof(asset_io::Utf8(dpiOverride).c_str()));
        if (parsed > 0.5f && parsed < 4.0f) { uiScale = parsed; state.dpiOverride = true; }
    }
    state.input = std::make_unique<EditorInputAdapter>(uiScale);
    state.input->SetWindow(window);
    return true;
}

void FinalizeToolkitWindowUi(ToolkitAppState& state)
{
    state.shell->SetPanelText(EditorPanelId::About, state.text.GetText("EditorUi.AboutText"));
    state.shell->SetPanelText(EditorPanelId::Diagnostics,
        state.text.GetText("EditorUi.DiagnosticsTitle") + "\nProject: " + asset_io::Utf8(state.root) +
        "\nScene: " + (state.session.Path().empty() ? std::string("-") : asset_io::Utf8(state.session.Path())) +
        "\nDPI: " + std::to_string(GetDpiForWindow(state.hostWindow)) + "\nUI scale: " +
        std::to_string(state.host.Preferences().uiScale) + "\nUI: flare::ui toolkit");

    ConfigureToolkitRecoveryPrompt(state);
    if (state.autoExport && state.exportAction) state.autoExportFn = state.exportAction;
    if (state.restoredContext)
    {
        switch (*state.restoredContext)
        {
        case EditorWorkspaceHost::DocumentContext::Material: state.shell->FocusPanel(EditorPanelId::Material); break;
        case EditorWorkspaceHost::DocumentContext::Ui: state.shell->FocusPanel(EditorPanelId::UiDesigner); break;
        case EditorWorkspaceHost::DocumentContext::ProjectSettings: state.shell->FocusPanel(EditorPanelId::ProjectSettings); break;
        case EditorWorkspaceHost::DocumentContext::BuildSettings: state.shell->FocusPanel(EditorPanelId::BuildExport); break;
        case EditorWorkspaceHost::DocumentContext::Scene: state.shell->FocusPanel(EditorPanelId::Viewport); break;
        }
    }
    state.featureTools = std::make_unique<EditorFeatureTools>(state.root, state.assets, state.session, state.text);
    state.featureTools->Attach(*state.shell);
    {
        std::uint64_t attachedEntity = 0;
        std::string attachedName;
        if (state.scriptWorkflow.ConsumePendingAttachOnStartup(state.crashDirectory, attachedEntity, attachedName))
        {
            state.session.Select(attachedEntity);
            state.assetState.assetName.clear();
            const std::string msg = state.text.GetText("EditorUi.ScriptAttached") + ": " + attachedName;
            state.importStatus = msg;
            state.shell->SetStatus(msg);
            state.shell->AppendConsoleLine(msg);
        }
    }
    Log(LogLevel::Info, "EditorUi.ToolkitHostReady");
}

std::optional<LRESULT> HandleToolkitWindowMessage(ToolkitAppState& state, HWND window, UINT message,
                                                  WPARAM wParam, LPARAM lParam)
{
    auto* windowInput = window == state.hostWindow ? state.input.get()
        : (state.floatingWindows ? state.floatingWindows->Input(window) : nullptr);
    if (message == WM_ENTERSIZEMOVE)
    {
        state.nativeMoveWindow = window;
        state.viewportController.Blur(state.session);
        if (state.shell) state.shell->Dispatcher().ReleasePointerCapture();
        if (window == state.hostWindow)
        {
            state.interactiveResizing = true;
            state.lastInteractiveResizeTick = GetTickCount64();
        }
        return std::optional<LRESULT>{};
    }
    if (message == WM_EXITSIZEMOVE)
    {
        if (state.nativeMoveWindow == window) state.nativeMoveWindow = nullptr;
        if (window == state.hostWindow)
        {
            state.interactiveResizing = false;
            state.lastInteractiveResizeTick = GetTickCount64();
        }
        return std::optional<LRESULT>{};
    }
    if (message == WM_ACTIVATEAPP && wParam == TRUE && window == state.hostWindow && !state.hidden && state.shell)
    {
        if (!state.host.ExportBusy())
        {
            RefreshShaderWatchMtimes(state, true);
        }
        if (!state.session.IsPlaying() && !state.host.ExportBusy() &&
            state.scriptWorkflow.CanHotReloadInProcess() && !state.scriptWorkflow.IsCompiling())
        {
            state.scriptWorkflow.RefreshSources();
            if (!state.scriptWorkflow.Sources().empty() && state.scriptWorkflow.SourcesNeedCompile())
            {
                (void)state.shell->Commands().Execute({EditorCommandId::Build, {}});
            }
        }
    }
    if (message == WM_SYSKEYDOWN && wParam == VK_F4) return std::optional<LRESULT>{};
    if (message == WM_CLOSE)
    {
        if (!state.shell || state.shell->CloseApproved()) return std::optional<LRESULT>{};
        state.shell->SetDirty(state.host.AnyDocumentDirty(state.session, &state.ui) || state.scriptWorkflow.Dirty());
        if (state.shell->RequestClose()) return std::optional<LRESULT>{0};
        return std::optional<LRESULT>{};
    }
    if (message == WM_DROPFILES)
    {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
        std::vector<std::filesystem::path> dropped;
        dropped.reserve(count);
        for (UINT index = 0; index < count; ++index)
        {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring path(length, L'\0');
            if (length > 0 && DragQueryFileW(drop, index, path.data(), length + 1) > 0) dropped.emplace_back(std::move(path));
        }
        DragFinish(drop);
        if (state.openImportConfirm) state.openImportConfirm(std::move(dropped));
        return std::optional<LRESULT>{0};
    }
    if (message == WM_DPICHANGED)
    {
        const UINT dpi = HIWORD(wParam);
        Log(LogLevel::Info, "EditorUi.ToolkitDpi: " + std::to_string(dpi));
        if (windowInput && dpi) windowInput->SetDpiScale(static_cast<float>(dpi) / 96.0f);
        if (lParam)
        {
            const auto& bounds = *reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                         bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return std::optional<LRESULT>{0};
    }
    if (message == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT && state.shell && windowInput)
    {
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor))
        {
            if (state.floatingWindows) state.floatingWindows->SelectSurface(window);
            const float scale = windowInput->DpiScale();
            const auto dragCursor = state.shell->AssetDragCursor();
            if (dragCursor == ui::editor::EditorDragCursor::Forbid)
            {
                SetCursor(LoadCursor(nullptr, IDC_NO));
                return std::optional<LRESULT>{TRUE};
            }
            if (dragCursor == ui::editor::EditorDragCursor::Allow)
            {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return std::optional<LRESULT>{TRUE};
            }
            if (auto axis = state.shell->SplitterCursorAt(cursor.x / scale, cursor.y / scale))
            {
                SetCursor(LoadCursor(nullptr, *axis == ui::editor::DockAxis::Horizontal ? IDC_SIZEWE : IDC_SIZENS));
                return std::optional<LRESULT>{TRUE};
            }
            if (state.shell->InspectorNumericCursorAt(cursor.x / scale, cursor.y / scale))
            {
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                return std::optional<LRESULT>{TRUE};
            }
        }
    }
    if (message == WM_MOUSEWHEEL)
    {
        POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
        ScreenToClient(window, &point);
        lParam = MAKELPARAM(point.x, point.y);
    }
    HWND targetWindow = window;
    if (state.floatingWindows)
    {
        targetWindow = state.floatingWindows->RoutePointer(window, message, lParam);
        state.floatingWindows->SelectSurface(targetWindow);
        if (targetWindow && targetWindow != window)
            windowInput = targetWindow == state.hostWindow ? state.input.get() : state.floatingWindows->Input(targetWindow);
    }
    const bool modalOpenNow = state.shell && (state.shell->CloseModalOpen() || state.shell->ConfirmOpen() ||
                                              state.shell->MenuOpen() || state.shell->ColorPickerOpen() ||
                                              state.shell->AddComponentPopupOpen());
    if (state.session.IsPlaying() && window == state.hostWindow && !modalOpenNow)
    {
        const auto playRoute = state.playInput->Process(window, message, wParam, lParam);
        if (playRoute.handled) return playRoute.handled;
        if (!playRoute.forwardEditor) return std::optional<LRESULT>{0};
    }
    ui::UiInputEvent event;
    if (windowInput && state.shell) windowInput->SetTextInputActive(state.shell->TextFieldFocused());
    if (windowInput && windowInput->Translate(message, wParam, lParam, event))
    {
        if (!state.shell) return std::optional<LRESULT>{};
        const bool lostFocus = event.type == ui::UiEventType::FocusLost;
        const bool lostCapture = event.type == ui::UiEventType::PointerCaptureLost;
        if (lostFocus || lostCapture)
        {
            if (lostFocus) state.shell->CancelPropertyInteraction();
            // 主动松开捕获不等于失焦；保留视口焦点，随后 QWER 仍应切换工具。
            if (lostFocus || state.viewportController.Captured()) state.viewportController.Blur(state.session);
            auto& dispatcher = state.shell->Dispatcher();
            event.target = dispatcher.PointerCapture();
            if (!event.target.IsValid()) event.target = dispatcher.Focus();
            dispatcher.Dispatch(event);
            dispatcher.ReleasePointerCapture();
            if (lostFocus) dispatcher.ClearFocus();
            if (!state.nativeMoveWindow && GetCapture() == window) ReleaseCapture();
            return std::optional<LRESULT>{};
        }
        const bool modal = state.shell->CloseModalOpen() || state.shell->ConfirmOpen() ||
                           state.shell->MenuOpen() || state.shell->ColorPickerOpen() ||
                           state.shell->AddComponentPopupOpen();
        const auto hit = ui::UiHitTest(state.shell->Document(), state.shell->Layout())
            .Hit(state.shell->CurrentInputRoot(), event.x, event.y).target;
        const bool onViewport = hit == state.shell->ViewportPanel();
        const bool key = event.type == ui::UiEventType::KeyDown || event.type == ui::UiEventType::KeyUp;
        const bool blocked = modal || (key && state.shell->TextFieldFocused());
        if (!blocked && onViewport && event.type == ui::UiEventType::PointerDown && event.button == 0 && !event.alt)
            state.assetState.assetName.clear();
        if (blocked) state.viewportController.Blur(state.session);
        if (event.type == ui::UiEventType::PointerDown && !state.viewportController.Captured())
        {
            if (!onViewport) state.viewportController.Blur(state.session);
            else if (!modal) state.shell->Dispatcher().ClearFocus();
        }
        bool handled = false;
        if (!blocked && event.type == ui::UiEventType::KeyDown && event.key == VK_ESCAPE &&
            (state.shell->ActivePanel() == EditorPanelId::UiDesigner || state.host.Ui().Canvas().Active()))
        {
            if (state.host.Ui().Canvas().Active()) handled = state.host.Ui().EndDrag(state.ui, false);
            else if (state.host.Ui().Selected() != 0) { state.host.Ui().Select(0); handled = true; }
        }
        const bool captured = state.viewportController.Captured();
        const bool shaderGraphActive = state.shell->ActivePanel() == EditorPanelId::ShaderGraph;
        if (!blocked && !handled && (captured || (key && !shaderGraphActive)))
            handled = state.viewportController.Handle(event, state.session, state.assets, onViewport);
        if (!handled && !captured && event.type == ui::UiEventType::KeyDown)
            handled = state.shell->RouteShortcut(event, blocked);
        if (!handled && !captured)
        {
            state.shell->Dispatcher().Dispatch(event);
            if (!blocked && !event.handled)
            {
                handled = state.viewportController.Handle(event, state.session, state.assets, onViewport);
                if (state.viewportController.Captured()) state.shell->Dispatcher().ReleasePointerCapture();
            }
        }
        if (const auto point = state.viewportController.TakeContextClick())
            state.shell->OpenCreateMenu(EditorPanelId::Viewport, point->x, point->y);
        const bool pointerCaptured = state.viewportController.Captured() || state.shell->Dispatcher().PointerCapture().IsValid();
        if (!state.nativeMoveWindow)
        {
            if (pointerCaptured && GetCapture() != window) SetCapture(window);
            else if (!pointerCaptured && GetCapture() == window) ReleaseCapture();
        }
        windowInput->SetTextInputActive(state.shell->TextFieldFocused());
        return std::optional<LRESULT>{0};
    }
    return std::optional<LRESULT>{};
}

void ExtractToolkitFrameData(ToolkitAppState& state, FrameData& frame)
{
    if (!state.shell || !state.hostWindow) throw std::runtime_error("EditorUi.ToolkitNotReady");
    if (state.pendingDialogCommand)
    {
        const EditorCommandId dialogCmd = *state.pendingDialogCommand;
        state.pendingDialogCommand.reset();
        state.shell->Dispatcher().ReleasePointerCapture();
        if (GetCapture() == state.hostWindow) ReleaseCapture();
        if (dialogCmd == EditorCommandId::Open) (void)PromptOpenSceneDialog(state);
        else if (dialogCmd == EditorCommandId::SaveAs) (void)PromptSaveSceneAsDialog(state);
        else if (dialogCmd == EditorCommandId::Import) (void)BrowseAndImportToolkitAsset(state);
    }
    if (state.hooks && state.hooks->operationFrame) state.hooks->operationFrame(state.host, state.session);
    PollBackgroundImportAndExport(state);
    if (!state.initialScriptCheckDone)
    {
        state.initialScriptCheckDone = true;
        if (!state.hidden && !state.session.IsPlaying() && state.scriptWorkflow.CanHotReloadInProcess() &&
            !state.scriptWorkflow.IsCompiling())
        {
            if (!state.scriptWorkflow.Sources().empty() && state.scriptWorkflow.SourcesNeedCompile())
            {
                (void)state.scriptWorkflow.StartInProcessCompile();
            }
        }
    }
    {
        const auto hot = state.scriptWorkflow.PollInProcessCompile();
        if (hot.finished)
        {
            for (const auto& line : hot.consoleLines)
            {
                state.shell->AppendConsoleLine(line);
            }
            state.assetsListDirty = true;
            state.host.LoadBuildSettings(true);
            state.shell->SetFormProperties(EditorPanelId::BuildExport,
                BuildBuildSettingsRows(state.host.Build(), state.exportName, state.text));
            if (hot.success && hot.attachedEntity != 0)
            {
                state.session.Select(hot.attachedEntity);
                state.assetState.assetName.clear();
                state.assetState.preferScriptUndo = true;
                state.shell->FocusPanel(EditorPanelId::Inspector);
            }
            if (!hot.statusMessage.empty())
            {
                NotifyToolkitStatus(state, hot.statusMessage, false);
            }
            const bool wantPlay = std::exchange(state.pendingPlayAfterCompile, false);
            if (!hot.success)
            {
                state.shell->FocusPanel(EditorPanelId::Console);
            }
            else if (wantPlay && !state.session.IsPlaying())
            {
                (void)state.shell->Commands().Execute({EditorCommandId::Play, {}});
            }
        }
    }
    if (state.hooks && state.hooks->drainScriptLogs)
    {
        std::vector<std::string> scriptLogs;
        state.hooks->drainScriptLogs(scriptLogs);
        for (auto& msg : scriptLogs)
        {
            state.shell->AppendConsoleLine(std::move(msg));
        }
    }
    if (!state.extractLogged) { state.extractLogged = true; Log(LogLevel::Info, "EditorUi.ToolkitExtract"); }
    if (state.autoExportFn && !state.autoExportStarted)
    {
        state.autoExportStarted = true;
        state.autoExportStart = GetTickCount64();
        state.autoExportFn();
    }

    RECT client{};
    GetClientRect(state.hostWindow, &client);
    const float width = static_cast<float>(client.right - client.left);
    const float height = static_cast<float>(client.bottom - client.top);
    const float dpi = state.input ? state.input->DpiScale() : 1.0f;
    state.shell->SetDpiScale(dpi);
    state.shell->Resize(width, height);
    if (state.floatingWindows && !state.floatingWindows->Synchronize())
        throw std::runtime_error(state.text.GetText("EditorUi.FloatingWindowFailed"));
    if (state.pendingFocusSurfaceId != 0 && state.floatingWindows)
    {
        if (HWND fw = state.floatingWindows->Window(state.pendingFocusSurfaceId))
        {
            ShowWindow(fw, SW_SHOWNORMAL);
            SetForegroundWindow(fw);
        }
        state.pendingFocusSurfaceId = 0;
    }
    float framebufferScaleX = dpi, framebufferScaleY = dpi;
    if (state.dpiOverride && state.shell->Width() > 0.0f && state.shell->Height() > 0.0f)
    {
        framebufferScaleX = width / state.shell->Width();
        framebufferScaleY = height / state.shell->Height();
    }
    state.fontService->SetRasterScale(framebufferScaleY);
    state.shell->Frame();

    SyncToolkitHierarchyAssetsAndInspector(state);
    const ui::UiRect designerContent = state.shell->Layout().Rect(state.shell->UiDesignerContent());
    if (designerContent.width > 2.0f && designerContent.height > 2.0f)
    {
        state.host.Ui().Canvas().Prepare(state.ui, {designerContent.x, designerContent.y, designerContent.width, designerContent.height});
        std::vector<ui::editor::EditorDesignerNode> designerNodes;
        for (const UiVisual& visual : state.host.Ui().Canvas().Visuals(state.ui))
        {
            ui::editor::EditorDesignerNode node;
            node.id = visual.id;
            node.rect = {visual.rectangle.x, visual.rectangle.y, visual.rectangle.width, visual.rectangle.height};
            node.text = visual.text;
            node.color = ui::UiColor{visual.color.r, visual.color.g, visual.color.b, visual.color.a};
            node.selected = (visual.id == state.host.Ui().Selected());
            node.visible = true;
            designerNodes.push_back(std::move(node));
        }
        state.shell->SetDesignerNodes(std::move(designerNodes));
    }

    if (state.input)
    {
        const ui::UiElementHandle focus = state.shell->Dispatcher().Focus();
        if (focus.IsValid())
        {
            const ui::UiRect rect = state.shell->Layout().Rect(focus);
            const auto surface = state.shell->SurfaceForElement(focus);
            auto* input = surface && state.floatingWindows
                ? state.floatingWindows->Input(state.floatingWindows->Window(surface)) : state.input.get();
            if (input) input->SetImeCandidateAnchor({rect.x, rect.y, rect.width, rect.height});
        }
    }

    const ULONGLONG nowTick = GetTickCount64();
    if (!state.transientStatus.empty() && nowTick >= state.transientStatusUntilTick)
    {
        state.transientStatus.clear();
        state.importStatus.clear();
    }
    state.shell->SetStatus(state.session.IsPlaying()
        ? state.text.GetText(state.session.PlayPaused() ? "EditorUi.Paused" : "EditorUi.Running")
        : !state.transientStatus.empty() ? state.transientStatus
        : !state.importStatus.empty() ? state.importStatus
        : state.text.GetText(state.viewportController.LastError().empty() ? "EditorUi.Ready" : state.viewportController.LastError()));

    const bool anyDirtyNow = state.host.AnyDocumentDirty(state.session, &state.ui) || state.scriptWorkflow.Dirty();
    if (!state.hidden && GetEnvironmentVariableW(L"FLARE_EDITOR_OPEN_PANEL", nullptr, 0) == 0 &&
        GetEnvironmentVariableW(L"FLARE_EDITOR_OPEN_ASSET", nullptr, 0) == 0)
    {
        std::string sceneStem = state.session.Path().empty()
            ? std::string("Untitled")
            : asset_io::Utf8(state.session.Path().filename());
        if (sceneStem.ends_with(".scene.json")) sceneStem.erase(sceneStem.size() - 11);
        std::string desiredTitle = state.text.GetText("Editor.WindowTitle") + " — " + sceneStem + (anyDirtyNow ? " *" : "");
        if (desiredTitle != state.lastWindowTitle)
        {
            state.lastWindowTitle = std::move(desiredTitle);
            SetWindowTextW(state.hostWindow, asset_io::FromUtf8(state.lastWindowTitle).c_str());
        }
    }
    if (state.externalChange.Path() != state.session.Path()) state.externalChange.Reset(state.session.Path());
    if (!state.interactiveResizing && !state.shell->ConfirmOpen() && state.externalChange.Poll())
    {
        const auto changedPath = state.externalChange.Path();
        state.externalChange.Consume();
        state.shell->ShowConfirm(
            state.text.GetText("EditorUi.ExternalChangedTitle"),
            state.text.GetText("EditorUi.ExternalChangedMessage") + ": " + asset_io::Utf8(changedPath),
            state.text.GetText("EditorUi.Reload"), state.text.GetText("EditorUi.Keep"), state.text.GetText("EditorUi.Cancel"),
            [&state, changedPath]() {
                if (state.session.Dirty()) { state.shell->SetStatus(state.text.GetText("EditorUi.SaveFirst")); return; }
                if (state.session.Open(changedPath, true))
                {
                    state.externalChange.Reset(state.session.Path());
                    state.shell->SetStatus(state.text.GetText("EditorUi.Reloaded"));
                }
                else
                {
                    state.shell->SetStatus(LocalizedToolkitStatus(state.text, state.session.LastError(), "EditorUi.SaveFailed"));
                }
            },
            [&state]() { state.shell->SetStatus(state.text.GetText("EditorUi.Keep")); });
    }

    state.shell->SetScriptsDirty(state.scriptWorkflow.SourcesNeedCompile());
    state.shell->SetDirty(state.host.AnyDocumentDirty(state.session, &state.ui) || state.scriptWorkflow.Dirty());
    if (state.shell->ConsumeCloseRequest()) PostMessageW(state.hostWindow, WM_CLOSE, 0, 0);

    if (!state.pendingShaderDiagnostics.empty())
    {
        for (const std::string& diag : state.pendingShaderDiagnostics)
        {
            if (diag.empty() || diag == state.lastLoggedShaderDiagnostic) continue;
            state.lastLoggedShaderDiagnostic = diag;
            const std::string header = state.text.GetText("EditorUi.ShaderHotReloadFailed");
            state.shell->AppendConsoleLine("[Error] " + header + ": " + diag);
            NotifyToolkitStatus(state, header, false);
            state.shell->FocusPanel(EditorPanelId::Console);
        }
        state.pendingShaderDiagnostics.clear();
    }
    if (state.shaderGraph && state.shaderGraph->IsOpen())
    {
        state.shaderGraph->TickPreview(static_cast<double>(GetTickCount64()) / 1000.0);
    }

    if (state.session.IsPlaying())
    {
        if (state.assetState.previewAudioVoice)
        {
            state.audio.Stop(state.assetState.previewAudioVoice);
            state.assetState.previewAudioVoice = {};
        }
        state.clipPreview.Clear();
        BuildFrameData(*state.session.PlayEnvironment(), state.playUi.get(), nullptr, frame);
        if (!state.hooks || !state.hooks->playStarted)
        {
            state.sceneAudio.Synchronize(*state.session.PlayWorld(), &state.assets, state.audio,
                                         state.session.PlayPaused());
            PopulateParticleFrame(state.sceneParticles, frame);
        }
    }
    else
    {
        if (state.sceneNavigation.TrackedAgentCount() > 0) state.sceneNavigation.Clear();
        if (state.sceneParticles.TrackedEmitterCount() > 0) state.sceneParticles.Clear();
        if (state.sceneAudio.TrackedSourceCount() > 0)
        {
            state.sceneAudio.Clear(state.audio);
            state.audio.StopAll();
            state.audio.SetPaused(false);
        }
        state.audio.Update(ListenerPose{});
        BuildFrameData(state.session.EditWorld(), nullptr, nullptr, frame);
        state.clipPreview.Update(state.session, frame);
    }
    if (state.session.IsPlaying() && state.playUi && state.hooks && state.hooks->extract)
    {
        state.hooks->extract(*state.session.PlayWorld(), *state.playUi, frame);
    }
    else if (state.viewportController.Gizmos())
    {
        const auto& world = state.session.IsPlaying() ? *state.session.PlayWorld() : state.session.EditWorld();
        if (!state.session.IsPlaying()) state.authoredScenePhysics.Synchronize(world, state.authoredPhysics);
        frame.debugLines = state.authoredPhysics.DebugLines();
        const auto lines = state.authoredScenePhysics.CharacterLines();
        frame.debugLines.insert(frame.debugLines.end(), lines.begin(), lines.end());
    }
    if (state.session.IsPlaying() && state.session.PlayEnvironment()->TimeOfDay().Profile())
    {
        frame.timeOfDay = state.session.PlayEnvironment()->Snapshot();
        ApplyTimeOfDayFrameData(frame);
    }
    else if (!state.session.IsPlaying())
    {
        if (state.host.TimeOfDayPreview().Ready())
        {
            frame.timeOfDay = state.host.TimeOfDayPreview().Snapshot();
            ApplyTimeOfDayFrameData(frame);
        }
        else if (const auto& todId = state.session.EditWorld().EnvironmentSettings().todProfile; !todId.empty())
        {
            std::vector<AssetDiagnostic> ignored;
            if (const auto profile = state.assets.LoadTODProfile(todId, ignored))
            {
                World emptyWorld;
                EventBus events(emptyWorld, 16, 1024);
                TODSystem tod(events);
                if (tod.SelectProfile(*profile))
                {
                    frame.timeOfDay = tod.Snapshot(0);
                    ApplyTimeOfDayFrameData(frame);
                }
            }
        }
    }
    frame.colorLutPath = state.host.ColorLutPath();
    if (!state.session.IsPlaying()) frame.materialPreview = state.host.Material().Preview();
    frame.shaderReloadGeneration = state.shaderReloadGeneration;
    frame.editorShaderDiagnosticSink = [&state](const std::string& diag) {
        state.pendingShaderDiagnostics.push_back(diag);
    };

    const bool renderGameAllowed = !state.hidden || GetEnvironmentVariableW(L"FLARE_RENDER_GAME_VIEWPORT", nullptr, 0) > 0;
    const bool viewportPanelVisible = state.shell->Document().Visible(state.shell->ViewportPanel());
    const bool gamePanelVisible = state.shell->Document().Visible(state.shell->GamePanel());
    const bool directGameScene = renderGameAllowed && gamePanelVisible && !viewportPanelVisible &&
                                 state.shell->Dock().SurfaceFor(EditorPanelId::Game) == 0;

    ui::UiRect viewportRect = state.shell->Layout().Rect(state.shell->ViewportPanel());
    if (framebufferScaleX > 0.0f && framebufferScaleY > 0.0f && viewportRect.width > 0.0f && viewportRect.height > 0.0f)
    {
        viewportRect.x = static_cast<float>(std::lround(viewportRect.x * framebufferScaleX)) / framebufferScaleX;
        viewportRect.y = static_cast<float>(std::lround(viewportRect.y * framebufferScaleY)) / framebufferScaleY;
        viewportRect.width = static_cast<float>(std::max<long>(1L, std::lround(viewportRect.width * framebufferScaleX))) / framebufferScaleX;
        viewportRect.height = static_cast<float>(std::max<long>(1L, std::lround(viewportRect.height * framebufferScaleY))) / framebufferScaleY;
    }
    ui::UiRect gameRect = state.shell->Layout().Rect(state.shell->PanelContent(EditorPanelId::Game));
    if (framebufferScaleX > 0.0f && framebufferScaleY > 0.0f && gameRect.width > 0.0f && gameRect.height > 0.0f)
    {
        gameRect.x = static_cast<float>(std::lround(gameRect.x * framebufferScaleX)) / framebufferScaleX;
        gameRect.y = static_cast<float>(std::lround(gameRect.y * framebufferScaleY)) / framebufferScaleY;
        gameRect.width = static_cast<float>(std::max<long>(1L, std::lround(gameRect.width * framebufferScaleX))) / framebufferScaleX;
        gameRect.height = static_cast<float>(std::max<long>(1L, std::lround(gameRect.height * framebufferScaleY))) / framebufferScaleY;
    }

    state.viewportController.SetSceneViewDuringPlay(renderGameAllowed && viewportPanelVisible);
    state.viewportController.Prepare(state.session, state.assets, viewportRect, ++state.editorFrame);
    state.camera = state.viewportController.Camera();
    state.viewportRect = viewportRect;
    state.shell->SetViewportState(state.viewportController.Tool(), state.viewportController.Local(),
                                  state.viewportController.Center(), state.viewportController.Sky(),
                                  state.viewportController.Grid(), state.viewportController.Gizmos(),
                                  state.session.IsPlaying(), state.session.PlayPaused(),
                                  state.viewportController.SceneViewDuringPlay());

    const std::vector<UiVisual> playUiVisuals = frame.ui;

    if (directGameScene)
    {
        // 全屏预览 / 仅摄像机视口可见：直接绑定主窗口 GPU 零拷贝 "scene" 目标，满帧率零 CPU 读回。
        if (!state.session.IsPlaying())
        {
            state.ui.Layout(std::max(1.0f, gameRect.width * framebufferScaleX),
                            std::max(1.0f, gameRect.height * framebufferScaleY));
            frame.ui = state.ui.Visuals();
        }
        const CameraViewport directVp{0.0f, 0.0f, std::max(1.0f, gameRect.width * framebufferScaleX),
                                      std::max(1.0f, gameRect.height * framebufferScaleY)};
        const auto directCam = SelectCamera(frame.cameras, directVp, state.editorFrame, frame.cameraBinding);
        const bool hasDirectOutput = directCam.snapshot.has_value() || !frame.ui.empty();
        state.shell->SetViewportImage({});
        state.shell->SetGameViewportImage(hasDirectOutput ? "scene" : "");
        state.playViewportRect = gameRect;
    }
    else
    {
        state.shell->SetViewportImage("scene");
        if (renderGameAllowed && state.session.IsPlaying() && state.viewportController.SceneViewDuringPlay())
        {
            // 双视口运行模式：游戏运行时 UI 仅绘制在摄像机视口，不遮挡场景视口（Scene View）。
            frame.ui.clear();
        }
        if (gamePanelVisible && renderGameAllowed && gameRect.width >= 16.0f && gameRect.height >= 16.0f)
        {
            const unsigned gw = static_cast<unsigned>(std::clamp<long>(
                std::lround(gameRect.width * framebufferScaleX), 64L, 2560L));
            const unsigned gh = static_cast<unsigned>(std::clamp<long>(
                std::lround(gameRect.height * framebufferScaleY), 48L, 1440L));
            CameraViewport gameVp{0.0f, 0.0f, static_cast<float>(gw), static_cast<float>(gh)};
            const auto camSelection = SelectCamera(frame.cameras, gameVp, state.gameUpdateIndex + 1, frame.cameraBinding);
            const bool hasGameOutput = camSelection.snapshot.has_value() ||
                                       (state.session.IsPlaying() && !playUiVisuals.empty());
            if (hasGameOutput)
            {
                if (!state.gameSurfaceWindow)
                {
                    state.gameSurfaceWindow = CreateWindowExW(
                        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
                        -32000, -32000, static_cast<int>(gw), static_cast<int>(gh),
                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                }
                if (state.gameSurfaceWindow && !state.gameRenderer)
                {
                    auto candidate = std::make_unique<SandboxRenderer>(SandboxScene::BasicLit, state.resourceDirectory);
                    candidate->SetMemoryCapture(true);
                    if (candidate->Initialize({state.gameSurfaceWindow, gw, gh}, state.text))
                        state.gameRenderer = std::move(candidate);
                }
                if (state.gameRenderer && state.gameSurfaceWindow)
                {
                    std::uint64_t renderKey = (static_cast<std::uint64_t>(gw) << 32) ^ static_cast<std::uint64_t>(gh);
                    const auto hashFloat = [&](float v) {
                        std::uint32_t bits = 0;
                        std::memcpy(&bits, &v, sizeof(bits));
                        renderKey ^= static_cast<std::uint64_t>(bits) + 0x9e3779b97f4a7c15ull + (renderKey << 6) + (renderKey >> 2);
                    };
                    if (camSelection.snapshot.has_value())
                    {
                        for (float v : camSelection.snapshot->viewProjection.values) hashFloat(v);
                    }
                    renderKey ^= playUiVisuals.size() * 0x9e3779b97f4a7c15ull;
                    renderKey ^= frame.meshes.size() * 0x517cc1b727220a95ull;
                    for (const auto& m : frame.meshes)
                    {
                        renderKey ^= m.entity * 1315423911ull;
                        hashFloat(m.world.values[12]);
                        hashFloat(m.world.values[13]);
                        hashFloat(m.world.values[14]);
                        hashFloat(m.world.values[0]);
                        hashFloat(m.world.values[5]);
                        for (const auto& matId : m.mesh.materials)
                            renderKey ^= std::hash<std::string>{}(matId);
                    }
                    for (const auto& l : frame.lights)
                    {
                        hashFloat(l.light.intensity);
                        hashFloat(l.world.values[8]);
                        hashFloat(l.world.values[9]);
                        hashFloat(l.world.values[10]);
                        hashFloat(l.light.color.x);
                    }
                    hashFloat(frame.exposure);
                    renderKey ^= std::hash<std::string>{}(asset_io::Utf8(frame.colorLutPath));
                    renderKey ^= state.shaderReloadGeneration * 0x9e3779b97f4a7c15ull;

                    const bool dynamicMode = state.session.IsPlaying() || state.clipPreview.Playing() ||
                                             state.host.TimeOfDayPreview().Ready();
                    const ULONGLONG tick = GetTickCount64();
                    const bool splitterNow = state.shell->SplitterActive();
                    if (state.prevSplitterActive && !splitterNow)
                        state.lastInteractiveResizeTick = 0;
                    state.prevSplitterActive = splitterNow;
                    const bool activelyResizing = state.interactiveResizing || splitterNow;
                    if (activelyResizing) state.lastInteractiveResizeTick = tick;
                    const bool deferGameResize = (state.gameImageRevision > 0) &&
                                                 (activelyResizing ||
                                                  (state.lastInteractiveResizeTick != 0 &&
                                                   (tick - state.lastInteractiveResizeTick < 80)));
                    const bool needsRender = (state.gameImageRevision == 0) ||
                                             (!deferGameResize && ((renderKey != state.lastGameRenderKey) ||
                                                                   (dynamicMode && (tick - state.lastGameRenderTick >= 16))));
                    if (needsRender)
                    {
                        SetWindowPos(state.gameSurfaceWindow, nullptr, -32000, -32000,
                                     static_cast<int>(gw), static_cast<int>(gh),
                                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
                        FrameData gameFrame = frame;
                        if (state.session.IsPlaying())
                        {
                            gameFrame.ui = playUiVisuals;
                        }
                        else
                        {
                            state.ui.Layout(static_cast<float>(gw), static_cast<float>(gh));
                            BuildFrameData(state.session.EditWorld(), &state.ui, nullptr, gameFrame);
                            state.clipPreview.Update(state.session, gameFrame);
                            gameFrame.timeOfDay = frame.timeOfDay;
                            if (gameFrame.timeOfDay) ApplyTimeOfDayFrameData(gameFrame);
                            gameFrame.colorLutPath = frame.colorLutPath;
                        }
                        gameFrame.assets = &state.assets;
                        gameFrame.shaderReloadGeneration = state.shaderReloadGeneration;
                        gameFrame.editorShaderDiagnosticSink = {};
                        gameFrame.editorGuiEnabled = false;
                        gameFrame.editorGui = {};
                        gameFrame.editorFontPages.clear();
                        gameFrame.editorUiImages.clear();
                        gameFrame.editorCamera = std::nullopt;
                        gameFrame.editorGridEnabled = false;
                        gameFrame.editorSceneLines.clear();
                        gameFrame.debugLines.clear();
                        gameFrame.editorSelected = 0;
                        gameFrame.editorSelectedEntities.clear();
                        gameFrame.sceneViewport = {0.0f, 0.0f, static_cast<float>(gw), static_cast<float>(gh)};
                        const RenderFrame rf{{state.gameSurfaceWindow, gw, gh}, 1.0 / 60.0, 1.0 / 60.0,
                                             tick / 1000.0, ++state.gameUpdateIndex, &gameFrame};
                        if (state.gameRenderer->Render(rf, state.text) != RenderResult::Failed)
                        {
                            state.lastGameRenderKey = renderKey;
                            state.lastGameRenderTick = tick;
                            ++state.gameImageRevision;
                        }
                    }
                    const auto& captured = state.gameRenderer->LatestMemoryCapture();
                    if (captured.width > 0 && captured.height > 0 && !captured.rgba8.empty())
                        state.shell->SetGameViewportImage("game");
                    else
                        state.shell->SetGameViewportImage({});
                }
                else
                {
                    state.shell->SetGameViewportImage({});
                }
            }
            else
            {
                state.shell->SetGameViewportImage({});
            }
            state.playViewportRect = gameRect;
        }
        else
        {
            state.shell->SetGameViewportImage({});
            state.playViewportRect = renderGameAllowed ? ui::UiRect{0.0f, 0.0f, 0.0f, 0.0f} : viewportRect;
        }
    }
    state.shell->Frame();

    if (state.hooks && state.hooks->toolkitDrawn) state.hooks->toolkitDrawn(*state.shell, state.session);
    ui::UiDisplayList display;
    ui::UiPaintStats paint;
    ui::UiPainter painter(state.shell->Document(), state.shell->Layout());
    if (!painter.Build(state.shell->Root(), display, paint)) throw std::runtime_error("EditorUi.ToolkitPaintFailed");
    if (!directGameScene)
    {
        for (auto it = display.commands.begin(); it != display.commands.end(); ++it)
        {
            if (it->kind == ui::UiPaintCommandKind::Image && it->image == "scene")
            {
                display.commands.insert(it + 1, state.viewportController.Overlay().commands.begin(),
                                        state.viewportController.Overlay().commands.end());
                break;
            }
        }
    }
    for (auto it = display.commands.begin(); it != display.commands.end(); ++it)
    {
        if (it->kind == ui::UiPaintCommandKind::Image && it->image == "shader-graph-canvas")
        {
            ui::UiDisplayList sgOverlay;
            state.shell->PaintShaderGraphCanvas(sgOverlay, state.shell->Root());
            it = display.commands.erase(it);
            display.commands.insert(it, sgOverlay.commands.begin(), sgOverlay.commands.end());
            break;
        }
    }
    GuiGeometry geometry;
    ui::UiGeometryStats geometryStats;
    std::vector<std::string> errors;
    ui::UiGeometryOptions options = MakeEditorUiGeometryOptions({framebufferScaleX, framebufferScaleY});
    options.whiteU = 0.0f;
    options.whiteV = 0.0f;
    options.whiteWidth = 1.0f;
    options.whiteHeight = 1.0f;
    ToolkitSceneImageProvider imageProvider;
    imageProvider.images = state.assetState.imageInfos;
    for (const auto& [thumbKey, thumbEntry] : state.assetThumbnails)
    {
        if (thumbEntry.token != 0 && thumbEntry.image && thumbEntry.image->width > 0 && thumbEntry.image->height > 0)
        {
            imageProvider.images[thumbKey] = {thumbEntry.token,
                                              static_cast<float>(thumbEntry.image->width),
                                              static_cast<float>(thumbEntry.image->height)};
        }
    }
    if (state.gameRenderer)
    {
        const auto& captured = state.gameRenderer->LatestMemoryCapture();
        if (captured.width > 0 && captured.height > 0 && !captured.rgba8.empty())
        {
            imageProvider.images["game"] = {4ull, static_cast<float>(captured.width), static_cast<float>(captured.height)};
        }
    }
    if (state.featureTools && state.featureTools->Preview())
    {
        imageProvider.images["feature-preview"] = {state.featureTools->PreviewToken(),
                                                   static_cast<float>(state.featureTools->Preview()->width),
                                                   static_cast<float>(state.featureTools->Preview()->height)};
    }
    if (state.shaderGraph && state.shaderGraph->Preview())
    {
        imageProvider.images["shader-graph-preview"] = {state.shaderGraph->PreviewToken(),
                                                        static_cast<float>(state.shaderGraph->Preview()->width),
                                                        static_cast<float>(state.shaderGraph->Preview()->height)};
    }
    if (!ui::UiGeometryBuilder{}.Build(display, &state.shell->TextService(), &imageProvider, options, geometry, geometryStats, errors))
    {
        std::string detail = "EditorUi.ToolkitGeometryFailed";
        for (const auto& error : errors) detail += " " + error;
        if (state.fontService && !state.fontService->LastError().empty()) detail += " " + state.fontService->LastError();
        throw std::runtime_error(detail);
    }
    if (!state.fontService || !AttachEditorFontPages(*state.fontService, state.fontPages, frame))
        throw std::runtime_error("EditorUi.ToolkitFontAtlasFailed");
    geometry.framebufferScale = {framebufferScaleX, framebufferScaleY};
    frame.editorGui = std::move(geometry);
    frame.editorGuiEnabled = true;
    if (state.featureTools && state.featureTools->Preview())
    {
        const auto& image = *state.featureTools->Preview();
        frame.editorUiImages.push_back({state.featureTools->PreviewToken(), image.width, image.height,
                                        image.rgba8.data(), image.rgba8.size()});
    }
    if (state.shaderGraph && state.shaderGraph->Preview())
    {
        const auto& image = *state.shaderGraph->Preview();
        frame.editorUiImages.push_back({state.shaderGraph->PreviewToken(), image.width, image.height,
                                        image.rgba8.data(), image.rgba8.size(),
                                        state.shaderGraph->PreviewRevision()});
    }
    if (state.assetState.previewImage && !state.assetState.previewImageKey.empty() && state.assetState.previewToken != 0)
    {
        frame.editorUiImages.push_back({state.assetState.previewToken, state.assetState.previewImage->width,
                                        state.assetState.previewImage->height, state.assetState.previewImage->rgba8.data(),
                                        state.assetState.previewImage->rgba8.size()});
    }
    for (const auto& [thumbKey, thumbEntry] : state.assetThumbnails)
    {
        if (thumbEntry.token != 0 && thumbEntry.image && !thumbEntry.image->rgba8.empty())
        {
            frame.editorUiImages.push_back({thumbEntry.token, thumbEntry.image->width, thumbEntry.image->height,
                                            thumbEntry.image->rgba8.data(), thumbEntry.image->rgba8.size(), 0});
        }
    }
    if (state.gameRenderer)
    {
        const auto& captured = state.gameRenderer->LatestMemoryCapture();
        if (captured.width > 0 && captured.height > 0 && !captured.rgba8.empty())
        {
            frame.editorUiImages.push_back({4ull, captured.width, captured.height,
                                            captured.rgba8.data(), captured.rgba8.size(), state.gameImageRevision});
        }
    }
    frame.assets = &state.assets;
    if (directGameScene)
    {
        frame.sceneViewport = {gameRect.x, gameRect.y, gameRect.width, gameRect.height};
        frame.editorCamera = std::nullopt;
        frame.editorSkyEnabled = true;
        frame.editorGridEnabled = false;
        frame.editorSceneLines.clear();
        frame.debugLines.clear();
        frame.editorSelected = 0;
        frame.editorSelectedEntities.clear();
    }
    else
    {
        frame.sceneViewport = {viewportRect.x, viewportRect.y, viewportRect.width, viewportRect.height};
        frame.editorCamera = state.viewportController.Camera();
        frame.editorSkyEnabled = state.viewportController.Sky();
        frame.editorGridEnabled = state.viewportController.Grid() &&
                                  (!state.session.IsPlaying() || state.viewportController.SceneViewDuringPlay());
        frame.editorSceneLines = state.viewportController.SceneLines();
        if (!state.session.IsPlaying()) state.dropWorkflow.AppendPreviewLines(frame.editorSceneLines);
        frame.editorSelected = state.session.IsPlaying() ? 0 : state.session.Selected();
        frame.editorSelectedEntities = state.session.IsPlaying()
            ? std::vector<StableEntityId>{} : state.session.Selection();
        if (!state.session.IsPlaying() && state.hoverMaterialEntity != 0 && !state.hoverMaterialAssetId.empty())
        {
            for (auto& meshInst : frame.meshes)
            {
                if (meshInst.entity == state.hoverMaterialEntity)
                {
                    if (meshInst.mesh.materials.empty())
                        meshInst.mesh.materials.push_back(state.hoverMaterialAssetId);
                    else
                        meshInst.mesh.materials[0] = state.hoverMaterialAssetId;
                }
            }
            if (std::find(frame.editorSelectedEntities.begin(), frame.editorSelectedEntities.end(),
                          state.hoverMaterialEntity) == frame.editorSelectedEntities.end())
            {
                frame.editorSelectedEntities.push_back(state.hoverMaterialEntity);
            }
        }
    }
    const ui::UiRect& loggedRect = directGameScene ? gameRect : viewportRect;
    if (state.loggedViewport[0] != loggedRect.x || state.loggedViewport[1] != loggedRect.y ||
        state.loggedViewport[2] != loggedRect.width || state.loggedViewport[3] != loggedRect.height)
    {
        state.loggedViewport = {loggedRect.x, loggedRect.y, loggedRect.width, loggedRect.height};
        Log(LogLevel::Info, "EditorUi.ToolkitViewport: " + std::to_string(loggedRect.x) + "," +
            std::to_string(loggedRect.y) + "," + std::to_string(loggedRect.width) + "," + std::to_string(loggedRect.height));
    }
    state.playInput->SetViewport(state.hostWindow, {state.playViewportRect.x * dpi, state.playViewportRect.y * dpi,
                                                    state.playViewportRect.width * dpi, state.playViewportRect.height * dpi});
    if (state.pendingActivatePlayInput && state.session.IsPlaying())
    {
        if (state.playViewportRect.width > 0.0f && state.playViewportRect.height > 0.0f)
        {
            state.playInput->Activate();
        }
        state.pendingActivatePlayInput = false;
    }
    if (state.floatingWindows && state.hooks && state.hooks->floatingDrawn) state.hooks->floatingDrawn(*state.floatingWindows, frame);
    if (state.floatingWindows && !state.floatingWindows->Render(frame, imageProvider, state.viewportController.Overlay()))
        throw std::runtime_error(state.text.GetText("EditorUi.FloatingRenderFailed"));
}

} // namespace flare
