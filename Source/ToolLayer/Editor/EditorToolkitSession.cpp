#include "EditorToolkitSession.h"
#include "EditorApplication.h"
#include "EditorUiAtlas.h"
#include "EditorWindowStyle.h"
#include "AssetImportService.h"
#include "BuildRestartRequest.h"
#include "EditorAssetActions.h"
#include "EditorCreation.h"
#include "EditorScriptTools.h"
#include "PrefabWorkflow.h"
#include "SceneInspector.h"
#include "TodInspector.h"
#include "UiInspector.h"
#include "ToolLayer/Editor/EditorUi/EditorViewportMath.h"
#include "ToolLayer/Tools/Project.h"
#include "FunctionLayer/Application/FeatureFrameData.h"
#include "CoreLayer/Log.h"
#include "PlatformLayer/Windows/StartupProgress.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Scenes/PrefabDocument.h"
#include "ResourceLayer/Assets/MaterialMetadata.h"

#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace se
{
using ui::editor::EditorCommand;
using ui::editor::EditorCommandId;
using ui::editor::EditorInputAdapter;
using ui::editor::EditorPanelFromName;
using ui::editor::EditorPanelId;
using ui::editor::EditorShell;

namespace
{
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
    auto report = [&state](const char* key, const std::string& detail = {}) {
        state.importStatus = state.text.GetText(key) + (detail.empty() ? std::string{} : ": " + detail);
        state.shell->SetStatus(state.importStatus);
        state.shell->SetAssetsPreviewText(state.importStatus);
        state.shell->AppendConsoleLine(state.importStatus);
    };
    if (sources.empty()) return;
    if (state.importTask.valid() || state.host.ExportBusy()) { report("Editor.AssetOperationBusy"); return; }
    if (state.session.IsPlaying()) { report("Editor.EditWhilePlaying"); return; }

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
    auto report = [&state](const char* key, const std::string& detail = {}) {
        state.importStatus = state.text.GetText(key) + (detail.empty() ? std::string{} : ": " + detail);
        state.shell->SetStatus(state.importStatus);
        state.shell->SetAssetsPreviewText(state.importStatus);
        state.shell->AppendConsoleLine(state.importStatus);
    };
    if (state.importTask.valid() || state.host.ExportBusy()) { report("Editor.AssetOperationBusy"); return false; }
    if (state.session.IsPlaying()) { report("Editor.EditWhilePlaying"); return false; }

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
            if (error) report("Editor.AssetImportFailed", std::to_string(error));
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
    if (GetEnvironmentVariableW(L"SELFENGINE_EDITOR_OPEN_PANEL", openPanel, 64) > 0)
    {
        EditorPanelId panel = EditorPanelId::None;
        if (EditorPanelFromName(asset_io::Utf8(openPanel), panel)) state.shell->FocusPanel(panel);
    }
    wchar_t openAsset[512]{};
    if (GetEnvironmentVariableW(L"SELFENGINE_EDITOR_OPEN_ASSET", openAsset, 512) > 0)
        state.pendingAsset = asset_io::Utf8(openAsset);
    wchar_t flag[8]{};
    state.autoExport = GetEnvironmentVariableW(L"SELFENGINE_EDITOR_AUTO_EXPORT", flag, 8) > 0;
    state.autoExportFlow = GetEnvironmentVariableW(L"SELFENGINE_EDITOR_AUTO_EXPORT_FLOW", flag, 8) > 0;
    wchar_t cancelMs[16]{};
    if (GetEnvironmentVariableW(L"SELFENGINE_EDITOR_AUTO_EXPORT_CANCEL_MS", cancelMs, 16) > 0)
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
                        state.shell->AppendConsoleLine(message);
                        Log(LogLevel::Warning, message);
                    }
                }
            }
        }
        if (result.key.empty()) result.key = result.success ? "Editor.AssetImportSucceeded" : "Editor.AssetImportFailed";
        state.importStatus = state.text.GetText(result.key) + (result.detail.empty() ? std::string{} : ": " + result.detail);
        state.shell->SetAssetsPreviewText(state.importStatus);
        state.shell->AppendConsoleLine(state.importStatus);
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
    for (const EditorHierarchyItem& item : BuildEditorHierarchy(editWorld))
    {
        ui::editor::EditorHierarchyNode node;
        node.id = item.id;
        node.parent = item.parent;
        node.depth = item.depth;
        node.hasChildren = item.hasChildren;
        node.expanded = true;
        if (const EntityRecord* record = editWorld.Get(editWorld.FindByStableId(item.id))) node.label = record->name;
        else node.label = "Entity " + std::to_string(item.id);
        hierarchy.push_back(std::move(node));
    }
    state.shell->SetHierarchy(std::move(hierarchy));

    std::vector<ui::editor::EditorAssetEntry> entries;
    for (const AssetId& id : state.assets.ListIds())
    {
        const std::filesystem::path resolved = state.assets.Resolve(id);
        std::string extension = resolved.extension().string();
        for (char& ch : extension)
        {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        std::string type = "asset";
        if (extension == ".fbx" || extension == ".gltf" || extension == ".glb") type = "model";
        else if (IsPrefabDocumentPath(resolved)) type = "prefab";
        else if (IsMaterialDocumentPath(resolved)) type = "material";
        else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".tga") type = "texture";
        else if (extension == ".wav" || extension == ".ogg") type = "audio";
        else if (extension == ".hlsl" || extension == ".hlsli") type = "shader";
        else if (extension == ".json" || extension == ".senav" || extension == ".selod"
            || extension == ".sehlod" || extension == ".semeshlet" || extension == ".seimp"
            || extension == ".sebc" || extension == ".ppm" || extension == ".sesh"
            || extension == ".sedp" || extension == ".sebrdf" || extension == ".seltc"
            || extension == ".sesheen" || extension == ".seskin")
        {
            type = "data";
        }
        entries.push_back({asset_io::Utf8(resolved), type});
    }
    for (const auto& source : state.scriptWorkflow.Sources()) entries.push_back({asset_io::Utf8(source), "C#"});
    std::string pendingAssetPath;
    if (!state.pendingAsset.empty())
    {
        for (const ui::editor::EditorAssetEntry& entry : entries)
        {
            if (entry.path.find(state.pendingAsset) != std::string::npos)
            {
                pendingAssetPath = entry.path;
                state.pendingAsset.clear();
                break;
            }
        }
    }
    state.shell->SetAssets(std::move(entries));
    if (!pendingAssetPath.empty()) state.shell->SelectAsset(pendingAssetPath);

    std::vector<ui::editor::EditorProperty> properties;
    std::string inspectorLabel = state.text.GetText("EditorUi.NoSelection");
    const StableEntityId selected = state.session.Selected();
    if (const auto* record = editWorld.Get(editWorld.FindByStableId(selected))) inspectorLabel = record->name;
    if (state.shell->ActivePanel() == EditorPanelId::Hierarchy || state.shell->ActivePanel() == EditorPanelId::Viewport)
        state.assetState.inspectUi = false;
    if (state.assetState.inspectUi)
    {
        state.shell->SetInspector(state.host.Ui().Selected(), state.text.GetText("EditorUi.UiDesigner"),
                                  UiInspectorRows(state.ui, state.host.Ui().Selected(), state.text, state.session.IsPlaying()));
    }
    else if (!state.assetState.assetName.empty())
    {
        state.shell->SetInspector(0, state.assetState.assetName, state.assetState.rows);
    }
    else
    {
        AppendSceneInspector(state.session, state.assets, state.text, selected, properties);
        const auto clip = std::find_if(properties.begin(), properties.end(), [](const auto& p) { return p.key == "actor.clip"; });
        if (clip != properties.end() && !state.session.IsPlaying())
        {
            const bool active = state.clipPreview.Target() == selected;
            const bool running = active && state.clipPreview.Playing();
            const auto group = clip->section;
            ui::editor::EditorProperty play{"preview.play", state.text.GetText(running ? "EditorUi.PauseClip" : "EditorUi.PlayClip"),
                                            ui::editor::EditorPropertyType::Action, {}, false, {}, group};
            ui::editor::EditorProperty time{"preview.time", state.text.GetText("EditorUi.ClipTime"),
                                            ui::editor::EditorPropertyType::Number,
                                            std::to_string(active ? std::round(state.clipPreview.Time() * 10.) / 10. : 0.), false, {}, group};
            properties.insert(std::next(clip), {std::move(play), std::move(time)});
        }
        AppendTodInspector(state.host.TimeOfDayPreview(), state.text, state.session.IsPlaying(), properties);
        state.scriptWorkflow.AppendRows(selected, state.session.IsPlaying(), properties);
        state.shell->SetInspector(selected, inspectorLabel, std::move(properties));
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
    if (session.Selected()) snapshot.selection.push_back(session.Selected());
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
    state.shell->SetAssetImportHandler([&state]() { BrowseAndImportToolkitAsset(state); });
    state.shell->SetAssetDragHandler([&state](const std::string& path, float x, float y, bool active) {
        if (!active || !state.camera || state.session.IsPlaying())
        {
            state.dropWorkflow.ClearHover();
            return;
        }
        float u = 0.0f, v = 0.0f;
        ui::editor::ViewportNormalize({state.viewportRect.x, state.viewportRect.y,
                                       state.viewportRect.width, state.viewportRect.height}, x, y, u, v);
        state.dropWorkflow.BeginOrUpdateHover(path, *state.camera, {u, v},
            state.session.EditWorld(), state.assets, &state.session.EditEnvironment().Spatial());
    });
    state.shell->SetAssetDropHandler([&state](const std::string& path, float x, float y) {
        if (!state.camera || state.session.IsPlaying())
        {
            state.dropWorkflow.ClearHover();
            return false;
        }
        float u = 0.0f, v = 0.0f;
        ui::editor::ViewportNormalize({state.viewportRect.x, state.viewportRect.y,
                                       state.viewportRect.width, state.viewportRect.height}, x, y, u, v);
        std::string error;
        StableEntityId created = 0;
        if (!state.dropWorkflow.CommitDrop(path, *state.camera, {u, v},
                                           state.session, state.assets, error, state.hooks, &created))
        {
            const std::string msg = LocalizedToolkitStatus(state.text, error, "EditorUi.DropFailed");
            state.shell->SetStatus(msg);
            state.shell->AppendConsoleLine(msg);
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
        if (!state.session.SetParent(id, parent)) { state.session.CancelEdit(); return false; }
        const bool committed = state.session.CommitEdit();
        if (committed) Log(LogLevel::Info, "EditorUi.ToolkitReparent: " + std::to_string(id) + " -> " + std::to_string(parent));
        return committed;
    });

    auto& commands = state.shell->Commands();
    const auto saveAll = [&ctx]() { return SaveAllToolkitDocuments(ctx); };
    commands.Register(EditorCommandId::Save, [saveAll](const EditorCommand&) { return saveAll(); });
    commands.Register(EditorCommandId::SaveAll, [saveAll](const EditorCommand&) { return saveAll(); });
    commands.Register(EditorCommandId::Open, [&state](const EditorCommand&) {
        if (state.session.Dirty()) { state.shell->SetStatus(state.text.GetText("EditorUi.SaveFirst")); return false; }
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = GetActiveWindow();
        dialog.lpstrFilter = L"Flare Scene\0*.scene.json\0All Files\0*.*\0";
        dialog.lpstrFile = file;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrDefExt = L"scene.json";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&dialog)) return true;
        if (!state.session.Open(file, true))
        {
            state.shell->SetStatus(LocalizedToolkitStatus(state.text, state.session.LastError(), "EditorUi.SaveFailed"));
            return false;
        }
        state.externalChange.Reset(state.session.Path());
        Log(LogLevel::Info, "EditorUi.ToolkitScene: opened");
        return true;
    });
    commands.Register(EditorCommandId::NewScene, [&state](const EditorCommand&) {
        if (state.session.Dirty()) { state.shell->SetStatus(state.text.GetText("EditorUi.SaveFirst")); return false; }
        std::string error;
        if (!state.session.NewDefaultScene(error))
        {
            state.shell->SetStatus(LocalizedToolkitStatus(state.text, error, "EditorUi.SaveFailed"));
            return false;
        }
        state.externalChange.Reset(state.session.Path());
        Log(LogLevel::Info, "EditorUi.ToolkitScene: new");
        return true;
    });
    commands.Register(EditorCommandId::SaveAs, [&state](const EditorCommand&) {
        wchar_t file[MAX_PATH]{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = GetActiveWindow();
        dialog.lpstrFilter = L"Flare Scene\0*.scene.json\0All Files\0*.*\0";
        dialog.lpstrFile = file;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrDefExt = L"scene.json";
        if (!state.session.Path().empty()) wcsncpy_s(file, state.session.Path().c_str(), _TRUNCATE);
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&dialog)) return true;
        if (!state.session.Save(file))
        {
            state.shell->SetStatus(LocalizedToolkitStatus(state.text, state.session.LastError(), "EditorUi.SaveFailed"));
            return false;
        }
        state.externalChange.Reset(state.session.Path());
        Log(LogLevel::Info, "EditorUi.ToolkitScene: saved-as");
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
    commands.Register(EditorCommandId::Build, [&state, saveAll](const EditorCommand&) {
        wchar_t requestPath[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(L"SELFENGINE_EDITOR_RESTART_REQUEST", requestPath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            state.shell->SetStatus(state.text.GetText("EditorUi.BuildControlUnavailable"));
            return false;
        }
        if (!saveAll()) { state.shell->SetStatus(state.text.GetText("EditorUi.SaveFailed")); return false; }
        std::string error;
        if (!WriteBuildRestartRequest(requestPath, error))
        {
            state.shell->SetStatus(LocalizedToolkitStatus(state.text, error, "EditorUi.SaveFailed"));
            return false;
        }
        state.shell->SetStatus(state.text.GetText("EditorUi.BuildRestart"));
        Log(LogLevel::Info, "EditorUi.ToolkitBuild: restart requested");
        if (state.hostWindow) PostMessageW(state.hostWindow, WM_CLOSE, 0, 0);
        return true;
    });
    commands.Register(EditorCommandId::Undo, [&state](const EditorCommand&) {
        if (state.assetState.inspectUi && !state.session.IsPlaying()) return state.host.Ui().Undo(state.ui);
        if (state.assetState.editTod && !state.session.IsPlaying()) return state.host.TimeOfDayPreview().Undo();
        return state.session.Undo();
    });
    commands.Register(EditorCommandId::Redo, [&state](const EditorCommand&) {
        if (state.assetState.inspectUi && !state.session.IsPlaying()) return state.host.Ui().Redo(state.ui);
        if (state.assetState.editTod && !state.session.IsPlaying()) return state.host.TimeOfDayPreview().Redo();
        return state.session.Redo();
    });
    commands.Register(EditorCommandId::Play, [&state](const EditorCommand&) {
        if (state.assetState.previewAudioVoice)
        {
            state.audio.Stop(state.assetState.previewAudioVoice);
            state.assetState.previewAudioVoice = {};
        }
        if (state.session.IsPlaying()) { state.session.StopPlay(); Log(LogLevel::Info, "EditorUi.ToolkitPlay: stop"); return true; }
        if (state.importTask.valid()) return false;
        std::string reason;
        if (!state.session.ValidatePlay(reason)) { Log(LogLevel::Warning, "EditorUi.ToolkitPlay: " + reason); return false; }
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
    commands.Register(EditorCommandId::Stop, [&state](const EditorCommand&) {
        if (!state.session.IsPlaying()) return false;
        state.session.StopPlay();
        Log(LogLevel::Info, "EditorUi.ToolkitPlay: stop");
        return true;
    });
    commands.Register(EditorCommandId::Delete, [&state](const EditorCommand&) {
        if (state.assetState.inspectUi && !state.session.IsPlaying())
        {
            UiId selected = state.host.Ui().Selected();
            const bool result = state.host.Ui().Edit(state.ui, [&](UiTree& tree) {
                return ApplyUiInspector(tree, selected, "ui.delete", {}, selected);
            });
            if (result) state.host.Ui().Select(selected);
            return result;
        }
        return state.assetState.assetName.empty() && state.session.Selected() && state.session.Delete(state.session.Selected());
    });
    commands.Register(EditorCommandId::Duplicate, [&state](const EditorCommand&) {
        if (state.assetState.inspectUi) return false;
        return state.assetState.assetName.empty() && state.session.Selected() && state.session.Duplicate(state.session.Selected()) != 0;
    });
    commands.Register(EditorCommandId::Import, [&state](const EditorCommand&) { return BrowseAndImportToolkitAsset(state); });
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
                state.shell->AppendConsoleLine(msg);
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
                            state.shell->AppendConsoleLine(msg);
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
                state.shell->AppendConsoleLine(msg);
            }
            return started;
        }
        std::filesystem::path created;
        std::string error;
        const bool saved = CreateEditorAsset(command.argument, state.root, state.assets, created, error);
        if (!created.empty())
        {
            state.scriptWorkflow.RefreshSources();
            state.pendingAsset = asset_io::Utf8(created);
        }
        if (!saved)
        {
            state.importStatus = LocalizedToolkitStatus(state.text, error, "Editor.AssetImportFailed");
            state.shell->SetStatus(state.importStatus);
            state.shell->AppendConsoleLine(state.importStatus);
        }
        return saved;
    });
    commands.Register(EditorCommandId::SetProperty, [&state](const EditorCommand& command) {
        if (!command.argument.starts_with("prefab.revertProperty:")) return false;
        std::string error;
        const StableEntityId targetId = command.target != 0 ? command.target : state.session.Selected();
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
        if (command.argument == "ui-designer") state.assetState.inspectUi = true;
        EditorPanelId panel = EditorPanelId::None;
        if (!EditorPanelFromName(command.argument, panel)) return false;
        Log(LogLevel::Info, "EditorUi.ToolkitPanel: " + command.argument);
        state.shell->TogglePanel(panel);
        return true;
    });
    commands.Register(EditorCommandId::FocusViewport, [&state](const EditorCommand&) {
        state.shell->FocusPanel(EditorPanelId::Viewport);
        return true;
    });
    commands.Register(EditorCommandId::OpenSettings, [&state](const EditorCommand&) {
        Log(LogLevel::Info, "EditorUi.ToolkitPanel: project-settings");
        state.shell->TogglePanel(EditorPanelId::ProjectSettings);
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
    flare::StyleTitleBar(window);
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
    if (GetEnvironmentVariableW(L"SELFENGINE_TEST_DPI_SCALE", dpiOverride, 16) > 0)
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
        std::to_string(state.host.Preferences().uiScale) + "\nUI: se::ui toolkit");

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
        return std::optional<LRESULT>{};
    }
    if (message == WM_EXITSIZEMOVE)
    {
        if (state.nativeMoveWindow == window) state.nativeMoveWindow = nullptr;
        return std::optional<LRESULT>{};
    }
    if (message == WM_SYSKEYDOWN && wParam == VK_F4) return std::optional<LRESULT>{};
    if (message == WM_CLOSE)
    {
        if (!state.shell || state.shell->CloseApproved()) return std::optional<LRESULT>{};
        state.shell->SetDirty(state.host.AnyDocumentDirty(state.session, &state.ui));
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
                                              state.shell->MenuOpen() || state.shell->ColorPickerOpen());
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
                           state.shell->MenuOpen() || state.shell->ColorPickerOpen();
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
        if (!blocked && !handled && (captured || key))
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
    if (state.hooks && state.hooks->operationFrame) state.hooks->operationFrame(state.host, state.session);
    PollBackgroundImportAndExport(state);
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

    state.shell->SetStatus(state.session.IsPlaying()
        ? state.text.GetText(state.session.PlayPaused() ? "EditorUi.Paused" : "EditorUi.Running")
        : !state.importStatus.empty() ? state.importStatus
        : state.text.GetText(state.viewportController.LastError().empty() ? "EditorUi.Ready" : state.viewportController.LastError()));
    if (state.externalChange.Path() != state.session.Path()) state.externalChange.Reset(state.session.Path());
    if (!state.shell->ConfirmOpen() && state.externalChange.Poll())
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

    state.shell->SetDirty(state.host.AnyDocumentDirty(state.session, &state.ui));
    if (state.shell->ConsumeCloseRequest()) PostMessageW(state.hostWindow, WM_CLOSE, 0, 0);

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
        frame.timeOfDay = state.session.PlayEnvironment()->Snapshot();
    if (!state.session.IsPlaying() && state.host.TimeOfDayPreview().Ready())
        frame.timeOfDay = state.host.TimeOfDayPreview().Snapshot();
    frame.colorLutPath = state.host.ColorLutPath();
    if (!state.session.IsPlaying()) frame.materialPreview = state.host.Material().Preview();

    ui::UiRect viewportRect = state.shell->Layout().Rect(state.shell->ViewportPanel());
    if (framebufferScaleX > 0.0f && framebufferScaleY > 0.0f && viewportRect.width > 0.0f && viewportRect.height > 0.0f)
    {
        viewportRect.x = static_cast<float>(std::lround(viewportRect.x * framebufferScaleX)) / framebufferScaleX;
        viewportRect.y = static_cast<float>(std::lround(viewportRect.y * framebufferScaleY)) / framebufferScaleY;
        viewportRect.width = static_cast<float>(std::max<long>(1L, std::lround(viewportRect.width * framebufferScaleX))) / framebufferScaleX;
        viewportRect.height = static_cast<float>(std::max<long>(1L, std::lround(viewportRect.height * framebufferScaleY))) / framebufferScaleY;
    }
    state.viewportController.Prepare(state.session, state.assets, viewportRect, ++state.editorFrame);
    state.camera = state.viewportController.Camera();
    state.viewportRect = viewportRect;
    state.shell->SetViewportState(state.viewportController.Tool(), state.viewportController.Local(),
                                  state.viewportController.Center(), state.viewportController.Sky(),
                                  state.viewportController.Grid(), state.viewportController.Gizmos(),
                                  state.session.IsPlaying(), state.session.PlayPaused());
    state.shell->Frame();

    if (state.hooks && state.hooks->toolkitDrawn) state.hooks->toolkitDrawn(*state.shell, state.session);
    ui::UiDisplayList display;
    ui::UiPaintStats paint;
    ui::UiPainter painter(state.shell->Document(), state.shell->Layout());
    if (!painter.Build(state.shell->Root(), display, paint)) throw std::runtime_error("EditorUi.ToolkitPaintFailed");
    for (auto it = display.commands.begin(); it != display.commands.end(); ++it)
    {
        if (it->kind == ui::UiPaintCommandKind::Image && it->image == "scene")
        {
            display.commands.insert(it + 1, state.viewportController.Overlay().commands.begin(),
                                    state.viewportController.Overlay().commands.end());
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
    if (state.featureTools && state.featureTools->Preview())
    {
        imageProvider.images["feature-preview"] = {state.featureTools->PreviewToken(),
                                                   static_cast<float>(state.featureTools->Preview()->width),
                                                   static_cast<float>(state.featureTools->Preview()->height)};
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
    if (state.assetState.previewImage && !state.assetState.previewImageKey.empty() && state.assetState.previewToken != 0)
    {
        frame.editorUiImages.push_back({state.assetState.previewToken, state.assetState.previewImage->width,
                                        state.assetState.previewImage->height, state.assetState.previewImage->rgba8.data(),
                                        state.assetState.previewImage->rgba8.size()});
    }
    frame.assets = &state.assets;
    frame.sceneViewport = {viewportRect.x, viewportRect.y, viewportRect.width, viewportRect.height};
    frame.editorCamera = state.viewportController.Camera();
    frame.editorSkyEnabled = state.viewportController.Sky();
    frame.editorGridEnabled = state.viewportController.Grid() && !state.session.IsPlaying();
    frame.editorSceneLines = state.viewportController.SceneLines();
    if (!state.session.IsPlaying()) state.dropWorkflow.AppendPreviewLines(frame.editorSceneLines);
    frame.editorSelected = state.session.IsPlaying() ? 0 : state.session.Selected();
    if (state.loggedViewport[0] != viewportRect.x || state.loggedViewport[1] != viewportRect.y ||
        state.loggedViewport[2] != viewportRect.width || state.loggedViewport[3] != viewportRect.height)
    {
        state.loggedViewport = {viewportRect.x, viewportRect.y, viewportRect.width, viewportRect.height};
        Log(LogLevel::Info, "EditorUi.ToolkitViewport: " + std::to_string(viewportRect.x) + "," +
            std::to_string(viewportRect.y) + "," + std::to_string(viewportRect.width) + "," + std::to_string(viewportRect.height));
    }
    state.playInput->SetViewport(state.hostWindow, {viewportRect.x * dpi, viewportRect.y * dpi, viewportRect.width * dpi, viewportRect.height * dpi});
    if (state.floatingWindows && state.hooks && state.hooks->floatingDrawn) state.hooks->floatingDrawn(*state.floatingWindows, frame);
    if (state.floatingWindows && !state.floatingWindows->Render(frame, imageProvider, state.viewportController.Overlay()))
        throw std::runtime_error(state.text.GetText("EditorUi.FloatingRenderFailed"));
}

} // namespace se
