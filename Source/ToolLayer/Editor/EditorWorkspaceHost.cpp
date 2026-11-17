#include "EditorWorkspaceHost.h"

#include "BuildRestartRequest.h"
#include "EditorProcessRunner.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Scenes/SceneSerializer.h"

#include <Windows.h>
#include <shellapi.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <set>

namespace se
{
namespace
{
using Json = nlohmann::json;

std::filesystem::path EnvironmentPath(const wchar_t* name)
{
    const auto length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length)
    {
        return {};
    }
    std::wstring value(length, L'\0');
    const auto copied = GetEnvironmentVariableW(name, value.data(), length);
    if (!copied || copied >= length)
    {
        return {};
    }
    value.resize(copied);
    return std::filesystem::path(value);
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool SourceFile(const std::filesystem::path& path, const Json& status)
{
    const auto name = Lower(asset_io::Utf8(path.filename()));
    for (const auto& extension : status.value("sourceExtensions", std::vector<std::string>{}))
    {
        if (name.ends_with(Lower(extension)))
        {
            return true;
        }
    }
    for (const auto& filename : status.value("sourceFileNames", std::vector<std::string>{}))
    {
        if (name == Lower(filename))
        {
            return true;
        }
    }
    return false;
}

std::string Digest(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    return !path.empty() && asset_io::Read(path, bytes) ? asset_io::Hash(bytes) : std::string{};
}

std::string RelativeDocument(const std::filesystem::path& root, const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {};
    }
    std::error_code error;
    const auto absolute = std::filesystem::weakly_canonical(path, error);
    if (error || !asset_io::Within(root, absolute))
    {
        return {};
    }
    return asset_io::Utf8(absolute.lexically_relative(root));
}

bool SameDocument(const std::filesystem::path& left, const std::filesystem::path& right)
{
    return !left.empty() && !right.empty() && asset_io::Within(left, right) && asset_io::Within(right, left);
}

} // namespace

EditorWorkspaceHost::EditorWorkspaceHost()
{
    defaultScenePath_ = "Resources/Scenes/Untitled.scene.json";
    ui_.SetDefaultPath("Resources/UI/Untitled.ui.json");
    userOperationDirectory_ = EditorSessionStore::UserDataDirectory() / "operations";
}

EditorWorkspaceHost::~EditorWorkspaceHost()
{
    if (exportJob_)
    {
        exportJob_->Cancel();
        exportJob_->Join();
    }
}

void EditorWorkspaceHost::SetProjectRoot(std::filesystem::path root)
{
    projectRoot_ = root;
    files_.SetRoot(root);
    ui_.SetProjectRoot(std::move(root));
}

void EditorWorkspaceHost::SetDocumentPaths(const std::filesystem::path& scene, const std::filesystem::path& ui)
{
    if (!scene.empty())
    {
        defaultScenePath_ = scene;
    }
    if (!ui.empty())
    {
        ui_.SetDefaultPath(ui);
    }
}

std::filesystem::path EditorWorkspaceHost::EditorRoot() const
{
    const auto executableRoot = GetExecutableDirectory();
    if (std::filesystem::is_regular_file(executableRoot / "Scripts" / "Export-Game.ps1"))
    {
        return executableRoot;
    }
    const auto current = std::filesystem::current_path();
    if (std::filesystem::is_regular_file(current / "Scripts" / "Export-Game.ps1"))
    {
        return current;
    }
    if (!projectRoot_.empty() && std::filesystem::is_regular_file(projectRoot_ / "Scripts" / "Export-Game.ps1"))
    {
        return projectRoot_;
    }
    return executableRoot;
}

std::filesystem::path EditorWorkspaceHost::ProjectManifestPath() const
{
    if (!projectRoot_.empty())
    {
        return projectRoot_ / "project.json";
    }
    return EditorRoot() / "project.json";
}

void EditorWorkspaceHost::SyncAssetDeclarations(AssetStore& assets)
{
    if (projectRoot_.empty() || !std::filesystem::is_regular_file(projectRoot_ / "project.json"))
    {
        return;
    }
    std::vector<EditorAssetIssue> issues;
    if (!EditorAssetActions::SyncProjectAssets(projectRoot_, assets, issues))
    {
        for (const auto& issue : issues)
        {
            errorKey_ = issue.key;
            LogConsole(2, issue.key, issue.detail);
        }
    }
}

bool EditorWorkspaceHost::OpenMaterial(AssetStore& assets, const std::filesystem::path& requested, bool discard)
{
    bool newlyRegistered = false;
    if (!material_.Open(assets, requested, discard, &newlyRegistered))
    {
        errorKey_ = material_.LastError();
        return false;
    }
    documentContext_ = DocumentContext::Material;
    if (newlyRegistered)
    {
        SyncAssetDeclarations(assets);
    }
    errorKey_.clear();
    return true;
}

bool EditorWorkspaceHost::SaveMaterial(AssetStore& assets, const std::filesystem::path& targetPath, bool overwrite)
{
    bool newlyRegistered = false;
    if (!material_.Save(assets, targetPath, overwrite, &newlyRegistered))
    {
        errorKey_ = material_.LastError();
        if (material_.ExternalConflict())
        {
            externalConflict_ = ExternalConflictKind::Material;
            externalConflictPath_ = material_.ActivePath();
        }
        return false;
    }
    SyncAssetDeclarations(assets);
    errorKey_.clear();
    return true;
}

bool EditorWorkspaceHost::OpenUi(UiTree& ui, const std::filesystem::path& path, bool discard)
{
    if (!ui_.Open(ui, path, discard))
    {
        errorKey_ = ui_.LastError();
        return false;
    }
    errorKey_.clear();
    return true;
}

bool EditorWorkspaceHost::SaveUi(UiTree& ui, const std::filesystem::path& path)
{
    const auto target = path.empty() ? ui_.EffectivePath() : path;
    if (!ui_.Save(ui, target))
    {
        errorKey_ = ui_.LastError();
        if (ui_.ExternalConflict())
        {
            externalConflict_ = ExternalConflictKind::Ui;
            externalConflictPath_ = target;
        }
        return false;
    }
    errorKey_.clear();
    return true;
}

bool EditorWorkspaceHost::LoadProjectSettings(bool discardUnsaved)
{
    projectSettingsError_.clear();
    const auto manifest = ProjectManifestPath();
    std::string key;
    if (!projectSettingsDocument_.Load(manifest, key, discardUnsaved))
    {
        projectSettingsError_ = key;
        errorKey_ = key;
        return false;
    }
    projectSettings_ = projectSettingsDocument_.Project();
    std::string ignored;
    documents_.Register("project-settings", "project-settings", manifest, ignored);
    documents_.MarkReloaded("project-settings", ignored);
    return true;
}

bool EditorWorkspaceHost::LoadBuildSettings(bool discardUnsaved)
{
    buildSettingsError_.clear();
    const auto buildJson = ProjectManifestPath().parent_path() / "build.json";
    std::string key;
    if (!buildSettingsDocument_.Load(buildJson, key, discardUnsaved))
    {
        buildSettingsError_ = key;
        errorKey_ = key;
        return false;
    }
    buildSettings_ = buildSettingsDocument_.Build();
    std::string ignored;
    documents_.Register("build-settings", "build-settings", buildJson, ignored);
    if (buildSettingsDocument_.SourceExists())
    {
        documents_.MarkReloaded("build-settings", ignored);
    }
    else
    {
        buildSettingsError_ = "Project.BuildSettingsMissing";
    }
    return true;
}

bool EditorWorkspaceHost::SetProjectSettings(const ProjectSettings& settings)
{
    std::string error;
    if (!projectSettingsDocument_.Loaded() && !LoadProjectSettings(true))
    {
        return false;
    }
    if (!projectSettingsDocument_.Set(settings, error))
    {
        projectSettingsError_ = error;
        errorKey_ = error;
        return false;
    }
    projectSettings_ = settings;
    return true;
}

bool EditorWorkspaceHost::SetBuildSettings(const BuildSettings& settings)
{
    std::string error;
    if (!buildSettingsDocument_.Loaded() && !LoadBuildSettings(true))
    {
        return false;
    }
    if (!buildSettingsDocument_.Set(settings, error))
    {
        buildSettingsError_ = error;
        errorKey_ = error;
        return false;
    }
    buildSettings_ = settings;
    return true;
}

bool EditorWorkspaceHost::SaveSettings(bool project)
{
    auto& document = project ? projectSettingsDocument_ : buildSettingsDocument_;
    auto& error = project ? projectSettingsError_ : buildSettingsError_;
    std::string detail;
    if (!document.Save(error, detail))
    {
        errorKey_ = error;
        LogConsole(2, error, detail);
        if (error == "Editor.ExternalChange")
        {
            externalConflict_ = project ? ExternalConflictKind::ProjectSettings : ExternalConflictKind::BuildSettings;
            externalConflictPath_ = document.Path();
        }
        return false;
    }
    if (project)
    {
        projectSettings_ = document.Project();
    }
    else
    {
        buildSettings_ = document.Build();
    }
    std::string ignored;
    documents_.MarkSaved(project ? "project-settings" : "build-settings", ignored);
    error.clear();
    errorKey_.clear();
    LogConsole(0, project ? "Editor.ProjectSettingsSaved" : "Editor.BuildSettingsSaved");
    return true;
}

void EditorWorkspaceHost::SetExternalDocuments(EditorExternalDocuments documents)
{
    externalDocuments_ = std::move(documents);
    externalDocumentsValid_ = !externalDocuments_.snapshot;
}

void EditorWorkspaceHost::ClearExternalConflict() noexcept
{
    externalConflict_ = ExternalConflictKind::None;
    externalConflictPath_.clear();
    errorKey_.clear();
}

bool EditorWorkspaceHost::ReloadExternalDocument(EditorSession&, AssetStore& assets, UiTree* ui)
{
    bool loaded = false;
    if (externalConflict_ == ExternalConflictKind::ProjectSettings)
    {
        loaded = LoadProjectSettings(true);
    }
    else if (externalConflict_ == ExternalConflictKind::BuildSettings)
    {
        loaded = LoadBuildSettings(true);
    }
    else if (externalConflict_ == ExternalConflictKind::Material)
    {
        loaded = OpenMaterial(assets, externalConflictPath_, true);
    }
    else if (externalConflict_ == ExternalConflictKind::Ui && ui)
    {
        loaded = OpenUi(*ui, externalConflictPath_, true);
    }
    if (loaded)
    {
        ClearExternalConflict();
    }
    return loaded;
}

bool EditorWorkspaceHost::SaveExternalCopy(AssetStore& assets, UiTree* ui, std::filesystem::path& copyPath)
{
    if (externalConflict_ == ExternalConflictKind::None || externalConflictPath_.empty())
    {
        return false;
    }
    copyPath = externalConflictPath_;
    const auto extension = copyPath.extension().string();
    copyPath.replace_filename(copyPath.stem().string() + "-external-" + asset_io::NewId().substr(0, 8) + extension);
    if (externalConflict_ == ExternalConflictKind::ProjectSettings ||
        externalConflict_ == ExternalConflictKind::BuildSettings)
    {
        auto& document = externalConflict_ == ExternalConflictKind::ProjectSettings
            ? projectSettingsDocument_
            : buildSettingsDocument_;
        RecoveryDocument snapshot;
        if (document.Snapshot(snapshot, errorKey_) &&
            asset_io::WriteAtomic(copyPath, {reinterpret_cast<const std::uint8_t*>(snapshot.content.data()),
                                             snapshot.content.size()}, false))
        {
            LogConsole(0, "Editor.RecoveryCopySaved", asset_io::Utf8(copyPath));
            ClearExternalConflict();
            return true;
        }
        return false;
    }
    if (externalConflict_ == ExternalConflictKind::Material)
    {
        if (SaveMaterial(assets, copyPath, true))
        {
            ClearExternalConflict();
            return true;
        }
        return false;
    }
    if (ui && SaveUi(*ui, copyPath))
    {
        ClearExternalConflict();
        return true;
    }
    return false;
}

void EditorWorkspaceHost::SyncDocumentRegistry(EditorSession& editor, UiTree* ui)
{
    const auto sync = [this](const char* id, bool dirty, const std::filesystem::path& path) {
        if (path.empty())
        {
            return;
        }
        const auto* entry = documents_.Find(id);
        if (!entry || entry->path != path)
        {
            std::string error;
            documents_.Register(id, "document", path, error);
            entry = documents_.Find(id);
        }
        if (!entry)
        {
            return;
        }
        if (dirty && entry->editRevision == entry->savedRevision)
        {
            documents_.MarkEdited(id);
        }
        else if (!dirty && entry->editRevision != entry->savedRevision)
        {
            documents_.RevertTo(id, entry->savedRevision);
        }
    };
    const auto scenePath = !editor.Path().empty() ? editor.Path() : defaultScenePath_;
    sync("scene", editor.Dirty(), scenePath);
    sync("material", material_.Dirty(), material_.ActivePath());
    sync("ui", ui_.Dirty(ui), ui_.EffectivePath());
    if (projectSettingsDocument_.Loaded())
    {
        sync("project-settings", projectSettingsDocument_.Dirty(), projectSettingsDocument_.Path());
    }
    if (buildSettingsDocument_.Loaded())
    {
        sync("build-settings", buildSettingsDocument_.Dirty(), buildSettingsDocument_.Path());
    }
    if (externalDocuments_.snapshot)
    {
        std::string error;
        std::vector<EditorExternalDocument> snapshots;
        if (externalDocuments_.snapshot(snapshots, error))
        {
            externalDocumentsValid_ = true;
            externalDocumentSnapshots_ = std::move(snapshots);
            for (const auto& external : externalDocumentSnapshots_)
            {
                sync(external.document.documentId.c_str(), external.dirty,
                     projectRoot_ / asset_io::FromUtf8(external.document.relativePath));
            }
        }
        else
        {
            externalDocumentsValid_ = false;
            errorKey_ = error.empty() ? "Editor.DocumentSnapshotFailed" : error;
        }
    }
}

bool EditorWorkspaceHost::CaptureEditingDocuments(const EditorSession& editor, AssetStore& assets, UiTree* ui,
                                                  std::vector<EditorAssetDocument>& documents)
{
    documents.clear();
    std::string scene;
    std::string error;
    if (!SerializeScene(editor.EditWorld(), scene, error))
    {
        errorKey_ = error;
        return false;
    }
    documents.push_back({EditorAssetDocumentKind::Scene,
                         editor.Path().empty() ? defaultScenePath_ : editor.Path(),
                         std::move(scene)});
    if (material_.Ready())
    {
        std::string material;
        std::vector<AssetDiagnostic> diagnostics;
        const auto target = material_.ActivePath();
        if (!SerializeMaterialDocument(material_.Document(), material, diagnostics))
        {
            errorKey_ = diagnostics.empty() ? "Assets.InvalidMaterial" : diagnostics.back().key;
            return false;
        }
        documents.push_back({EditorAssetDocumentKind::Material,
                             target.is_absolute() ? target : assets.SourceRoot() / target,
                             std::move(material)});
    }
    if (ui)
    {
        documents.push_back({EditorAssetDocumentKind::Ui, ui_.EffectivePath(), ui->SaveJson()});
    }
    if (externalDocuments_.snapshot)
    {
        std::vector<EditorExternalDocument> snapshots;
        if (!externalDocuments_.snapshot(snapshots, error))
        {
            errorKey_ = error.empty() ? "Editor.DocumentSnapshotFailed" : error;
            return false;
        }
        for (const auto& snapshot : snapshots)
        {
            if (snapshot.document.kind == "bindings")
            {
                documents.push_back({EditorAssetDocumentKind::Bindings,
                                     projectRoot_ / asset_io::FromUtf8(snapshot.document.relativePath),
                                     snapshot.document.content});
            }
        }
    }
    return true;
}

bool EditorWorkspaceHost::SaveAllDocuments(EditorSession& editor, AssetStore& assets, UiTree* ui)
{
    SyncDocumentRegistry(editor, ui);
    if (!externalDocumentsValid_)
    {
        errorKey_ = "Editor.DocumentSnapshotFailed";
        LogConsole(2, errorKey_);
        return false;
    }
    std::vector<EditorAssetDocument> editingDocuments;
    std::vector<EditorAssetIssue> validation;
    if (!CaptureEditingDocuments(editor, assets, ui, editingDocuments))
    {
        return false;
    }
    if (!EditorAssetActions::ValidatePlayAssets(editor.EditWorld(), ui, assets, editingDocuments, validation))
    {
        for (const auto& issue : validation)
        {
            LogConsole(2, issue.key, issue.detail);
        }
        errorKey_ = validation.empty() ? "Assets.PlayValidationFailed" : validation.front().key;
        return false;
    }
    const auto results = documents_.SaveAll([&](const EditorDocumentEntry& entry, std::string& key, std::string& detail) {
        if (entry.id == "scene")
        {
            const auto action = files_.SaveCurrent(editor, defaultScenePath_);
            if (action == FileActionResult::Done)
            {
                return true;
            }
            key = action == FileActionResult::Waiting ? "Editor.SaveAllPending" : "Editor.SaveAllFailed";
            detail = files_.LastError();
            return false;
        }
        if (entry.id == "material")
        {
            if (SaveMaterial(assets))
            {
                return true;
            }
            key = material_.LastError() == "Editor.OverwriteConfirmationRequired"
                ? "Editor.SaveAllPending"
                : material_.ExternalConflict() ? "Editor.ExternalChange" : "Editor.SaveAllFailed";
            detail = errorKey_;
            return false;
        }
        if (entry.id == "ui")
        {
            if (ui && SaveUi(*ui, ui_.EffectivePath()))
            {
                return true;
            }
            key = ui_.ExternalConflict() ? "Editor.ExternalChange" : "Editor.SaveAllFailed";
            detail = errorKey_;
            return false;
        }
        if (entry.id == "project-settings" || entry.id == "build-settings")
        {
            if (SaveSettings(entry.id == "project-settings"))
            {
                return true;
            }
            key = errorKey_;
            return false;
        }
        for (const auto& external : externalDocumentSnapshots_)
        {
            if (external.document.documentId == entry.id && externalDocuments_.save)
            {
                return externalDocuments_.save(entry.id, key);
            }
        }
        key = "Editor.CommandUnknown";
        detail = entry.id;
        return false;
    });

    bool success = true;
    std::string failureKey;
    for (const auto& result : results)
    {
        if (result.saved)
        {
            continue;
        }
        success = false;
        if (failureKey.empty())
        {
            failureKey = result.diagnosticKey.empty() ? "Editor.SaveAllFailed" : result.diagnosticKey;
        }
        if (result.diagnosticKey == "Editor.ExternalChange")
        {
            if (result.id == "material")
            {
                externalConflict_ = ExternalConflictKind::Material;
                externalConflictPath_ = documents_.Find(result.id)->path;
            }
            else if (result.id == "ui")
            {
                externalConflict_ = ExternalConflictKind::Ui;
                externalConflictPath_ = documents_.Find(result.id)->path;
            }
            else if (result.id == "project-settings" || result.id == "build-settings")
            {
                externalConflict_ = result.id == "project-settings"
                    ? ExternalConflictKind::ProjectSettings
                    : ExternalConflictKind::BuildSettings;
                externalConflictPath_ = documents_.Find(result.id)->path;
            }
        }
        LogConsole(2, result.diagnosticKey.empty() ? "Editor.SaveAllFailed" : result.diagnosticKey, result.detail);
    }
    if (!success)
    {
        errorKey_ = failureKey;
    }
    return success;
}

bool EditorWorkspaceHost::DocumentsDirty(const UiTree* ui) const
{
    if (!externalDocumentsValid_)
    {
        return true;
    }
    if (material_.Dirty() || tod_.Dirty())
    {
        return true;
    }
    if (projectSettingsDocument_.Dirty() || buildSettingsDocument_.Dirty())
    {
        return true;
    }
    for (const auto& external : externalDocumentSnapshots_)
    {
        if (external.dirty)
        {
            return true;
        }
    }
    return ui_.Dirty(ui);
}

bool EditorWorkspaceHost::AnyDocumentDirty(const EditorSession& editor, const UiTree* ui) const
{
    return editor.Dirty() || DocumentsDirty(ui);
}

std::vector<std::string> EditorWorkspaceHost::SessionDocuments(const EditorSession& editor) const
{
    std::vector<std::string> result;
    for (const auto& path : {editor.Path(), material_.ActivePath(), ui_.SavedPath()})
    {
        const auto relative = RelativeDocument(projectRoot_, path);
        if (!relative.empty())
        {
            result.push_back(relative);
        }
    }
    for (const auto* document : {&projectSettingsDocument_, &buildSettingsDocument_})
    {
        if (document->Loaded())
        {
            const auto relative = RelativeDocument(projectRoot_, document->Path());
            if (!relative.empty() && std::find(result.begin(), result.end(), relative) == result.end())
            {
                result.push_back(relative);
            }
        }
    }
    return result;
}

std::string EditorWorkspaceHost::SessionActiveDocument(const EditorSession& editor) const
{
    std::filesystem::path path;
    switch (documentContext_)
    {
    case DocumentContext::Material:
        if (material_.Ready())
        {
            path = material_.ActivePath();
        }
        break;
    case DocumentContext::Ui:
        path = ui_.SavedPath();
        break;
    case DocumentContext::ProjectSettings:
        if (projectSettingsDocument_.Loaded())
        {
            path = projectSettingsDocument_.Path();
        }
        break;
    case DocumentContext::BuildSettings:
        if (buildSettingsDocument_.Loaded())
        {
            path = buildSettingsDocument_.Path();
        }
        break;
    case DocumentContext::Scene:
        path = editor.Path();
        break;
    }
    const auto relative = RelativeDocument(projectRoot_, path);
    return relative.empty() ? RelativeDocument(projectRoot_, editor.Path()) : relative;
}

std::optional<EditorWorkspaceHost::DocumentContext> EditorWorkspaceHost::RestoreSessionDocuments(
    const std::vector<std::string>& documents, AssetStore& assets, UiTree& ui, std::string_view activePath)
{
    const auto previousContext = documentContext_;
    auto active = asset_io::FromUtf8(activePath);
    if (!active.empty() && !active.is_absolute())
    {
        active = projectRoot_ / active;
    }
    if (!active.empty() && !asset_io::Within(projectRoot_, active))
    {
        active.clear();
    }
    std::optional<DocumentContext> matchedActive;
    for (const auto& document : documents)
    {
        const auto requested = asset_io::FromUtf8(document);
        const auto path = requested.is_absolute() ? requested : projectRoot_ / requested;
        if (!asset_io::Within(projectRoot_, path))
        {
            LogConsole(1, "Editor.SessionDocumentMissing", document);
            continue;
        }
        std::optional<DocumentContext> restored;
        if (document.ends_with(".material.json"))
        {
            if (OpenMaterial(assets, path))
            {
                restored = DocumentContext::Material;
            }
            else
            {
                LogConsole(1, errorKey_, document);
            }
        }
        else if (document.ends_with(".ui.json"))
        {
            if (OpenUi(ui, path, true))
            {
                restored = DocumentContext::Ui;
            }
            else
            {
                LogConsole(1, errorKey_, document);
            }
        }
        else if (SameDocument(path, projectRoot_ / "project.json"))
        {
            if (LoadProjectSettings())
            {
                restored = DocumentContext::ProjectSettings;
            }
            else
            {
                LogConsole(1, projectSettingsError_, document);
            }
        }
        else if (SameDocument(path, projectRoot_ / "build.json"))
        {
            LoadBuildSettings();
            if (buildSettingsDocument_.Loaded() &&
                (buildSettingsError_.empty() || buildSettingsError_ == "Project.BuildSettingsMissing"))
            {
                restored = DocumentContext::BuildSettings;
            }
            else
            {
                LogConsole(1, buildSettingsError_, document);
            }
        }
        else if (document.ends_with(".scene.json"))
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error) && !error)
            {
                restored = DocumentContext::Scene;
            }
        }
        if (restored && SameDocument(path, active))
        {
            matchedActive = restored;
        }
    }
    documentContext_ = matchedActive.value_or(previousContext);
    return matchedActive;
}

void EditorWorkspaceHost::ConfigureRecovery(std::filesystem::path directory, bool offerRecovery)
{
    recoveryDirectory_ = std::move(directory);
    recoveryGroups_.clear();
    recoveryProtectedGroups_.clear();
    recoveryPrunedSignature_.clear();
    if (offerRecovery)
    {
        std::string error;
        recoveryGroups_ = EditorSessionStore::ListRecoveryGroups(recoveryDirectory_, projectRoot_, error);
        if (!error.empty())
        {
            LogConsole(1, error);
        }
    }
    recoveryOffered_ = !recoveryGroups_.empty();
}

std::vector<RecoveryDocument> EditorWorkspaceHost::CaptureRecoveryDocuments(const EditorSession& editor,
                                                                            const UiTree* ui)
{
    recoveryCaptureValid_ = false;
    std::vector<RecoveryDocument> documents;
    const auto add = [&](std::string id, std::string kind, const std::filesystem::path& path, std::string base,
                         std::string content) {
        documents.push_back(
            {std::move(id), std::move(kind), RelativeDocument(projectRoot_, path), std::move(base), std::move(content),
             0});
    };
    if (editor.Dirty())
    {
        std::string scene;
        std::string error;
        if (SerializeScene(editor.EditWorld(), scene, error))
        {
            add("scene", "scene", editor.Path(), editor.SourceDigest(), std::move(scene));
        }
        else
        {
            LogConsole(1, error);
            return {};
        }
    }
    if (material_.Dirty())
    {
        std::string material;
        std::vector<AssetDiagnostic> diagnostics;
        if (!SerializeMaterialDocument(material_.Document(), material, diagnostics))
        {
            LogConsole(1, "Assets.InvalidMaterial");
            return {};
        }
        add("material", "material", material_.ActivePath(), material_.Stamp().digest, std::move(material));
    }
    if (ui_.Dirty(ui))
    {
        add("ui", "ui", ui_.SavedPath(), ui_.Stamp().digest, ui->SaveJson());
    }
    for (const auto* settings : {&projectSettingsDocument_, &buildSettingsDocument_})
    {
        if (settings->Dirty())
        {
            RecoveryDocument document;
            std::string error;
            if (!settings->Snapshot(document, error))
            {
                LogConsole(1, error);
                return {};
            }
            documents.push_back(std::move(document));
        }
    }
    if (externalDocuments_.snapshot)
    {
        std::string error;
        std::vector<EditorExternalDocument> snapshots;
        if (externalDocuments_.snapshot(snapshots, error))
        {
            externalDocumentSnapshots_ = std::move(snapshots);
            for (const auto& external : externalDocumentSnapshots_)
            {
                if (external.dirty)
                {
                    documents.push_back(external.document);
                }
            }
        }
        else
        {
            LogConsole(1, error);
            return {};
        }
    }
    std::string signature;
    for (const auto& document : documents)
    {
        signature += document.documentId + document.relativePath + document.sourceDigest + document.content;
    }
    const auto digest = asset_io::Hash({reinterpret_cast<const std::uint8_t*>(signature.data()), signature.size()});
    if (digest != recoverySignature_)
    {
        recoverySignature_ = digest;
        ++recoveryRevision_;
    }
    for (auto& document : documents)
    {
        document.revision = recoveryRevision_;
    }
    recoveryCaptureValid_ = true;
    return documents;
}

bool EditorWorkspaceHost::RecoverySourceChanged(const RecoveryGroup& group) const
{
    for (const auto& document : group.documents)
    {
        if (document.relativePath.empty())
        {
            continue;
        }
        const auto path = projectRoot_ / asset_io::FromUtf8(document.relativePath);
        if (!asset_io::Within(projectRoot_, path) || Digest(path) != document.sourceDigest)
        {
            return true;
        }
    }
    return false;
}

bool EditorWorkspaceHost::RestoreRecovery(const RecoveryGroup& group, EditorSession& editor, AssetStore& assets,
                                          UiTree& ui)
{
    if (editor.IsPlaying() || editor.Dirty() || DocumentsDirty(&ui))
    {
        errorKey_ = "Editor.UnsavedChanges";
        return false;
    }
    if (RecoverySourceChanged(group))
    {
        errorKey_ = "Editor.RecoverySourceChanged";
        return false;
    }
    const RecoveryDocument* scene = nullptr;
    const RecoveryDocument* material = nullptr;
    const RecoveryDocument* uiDocument = nullptr;
    std::vector<const RecoveryDocument*> settingsDocuments;
    std::vector<const RecoveryDocument*> external;
    MaterialDocument parsedMaterial;
    UiTree parsedUi;
    World parsedScene;
    for (const auto& document : group.documents)
    {
        if (document.kind == "scene" && !scene)
        {
            if (!DeserializeScene(document.content, parsedScene, errorKey_))
            {
                return false;
            }
            scene = &document;
        }
        else if (document.kind == "material" && !material)
        {
            std::vector<AssetDiagnostic> diagnostics;
            if (!DeserializeMaterialDocument(document.content, parsedMaterial, diagnostics))
            {
                errorKey_ = "Assets.InvalidMaterial";
                return false;
            }
            if (!document.relativePath.empty() &&
                !asset_io::Within(assets.SourceRoot(), projectRoot_ / asset_io::FromUtf8(document.relativePath)))
            {
                errorKey_ = "Assets.ReferenceOutsideRoot";
                return false;
            }
            material = &document;
        }
        else if (document.kind == "ui" && !uiDocument)
        {
            if (!parsedUi.LoadJson(document.content, errorKey_))
            {
                return false;
            }
            uiDocument = &document;
        }
        else if (document.kind == "project-settings" || document.kind == "build-settings")
        {
            const bool project = document.kind == "project-settings";
            if (project && !projectSettingsDocument_.Loaded())
            {
                LoadProjectSettings();
            }
            if (!project && !buildSettingsDocument_.Loaded())
            {
                LoadBuildSettings();
            }
            auto& settings = project ? projectSettingsDocument_ : buildSettingsDocument_;
            if (!settings.Restore(document, false, errorKey_))
            {
                return false;
            }
            settingsDocuments.push_back(&document);
        }
        else if (externalDocuments_.restore && externalDocuments_.restore(document, false, errorKey_))
        {
            external.push_back(&document);
        }
        else
        {
            if (errorKey_.empty())
            {
                errorKey_ = "Editor.RecoveryInvalidDocument";
            }
            return false;
        }
    }
    const auto resolvePath = [&](const RecoveryDocument& document) {
        return document.relativePath.empty() ? std::filesystem::path{}
                                             : projectRoot_ / asset_io::FromUtf8(document.relativePath);
    };
    if (scene && !editor.RestoreUnsavedDocument(scene->content, resolvePath(*scene), scene->sourceDigest))
    {
        errorKey_ = editor.LastError();
        return false;
    }
    if (scene)
    {
        recoveryScenePath_ = resolvePath(*scene);
        recoverySceneDigest_ = scene->sourceDigest;
        if (!editor.Path().empty())
        {
            defaultScenePath_ = editor.Path();
        }
    }
    if (material)
    {
        material_.RestoreRecovered(assets, parsedMaterial, resolvePath(*material));
    }
    if (uiDocument)
    {
        ui = std::move(parsedUi);
        ui_.RestoreRecovered(ui, resolvePath(*uiDocument),
                             assets.SourceRoot() / "UI" / ("Recovered-" + asset_io::NewId() + ".ui.json"));
    }
    for (const auto* document : external)
    {
        if (!externalDocuments_.restore(*document, true, errorKey_))
        {
            return false;
        }
    }
    for (const auto* document : settingsDocuments)
    {
        const bool project = document->kind == "project-settings";
        auto& settings = project ? projectSettingsDocument_ : buildSettingsDocument_;
        if (!settings.Restore(*document, true, errorKey_))
        {
            return false;
        }
        if (project)
        {
            projectSettings_ = settings.Project();
        }
        else
        {
            buildSettings_ = settings.Build();
        }
    }
    errorKey_.clear();
    return true;
}

bool EditorWorkspaceHost::SaveRecoveryCopy(const RecoveryGroup& group, std::filesystem::path& destination)
{
    destination = projectRoot_ / "Recovered" / (group.id + "-" + asset_io::NewId());
    std::error_code error;
    if (!std::filesystem::create_directories(destination, error) || error)
    {
        errorKey_ = "Editor.RecoveryWriteFailed";
        return false;
    }
    for (const auto& document : group.documents)
    {
        const auto relative = document.relativePath.empty()
            ? asset_io::FromUtf8(document.documentId +
                                 (document.kind == "scene" ? ".scene.json"
                                  : document.kind == "material" ? ".material.json"
                                  : document.kind == "bindings" ? ".bindings.json"
                                                                : ".ui.json"))
            : asset_io::FromUtf8(document.relativePath);
        const auto target = destination / relative;
        if (!asset_io::Within(destination, target) ||
            !asset_io::WriteAtomic(target,
                                   {reinterpret_cast<const std::uint8_t*>(document.content.data()),
                                    document.content.size()},
                                   false))
        {
            errorKey_ = "Editor.RecoveryWriteFailed";
            return false;
        }
    }
    errorKey_.clear();
    LogConsole(0, "Editor.RecoveryCopySaved", asset_io::Utf8(destination));
    return true;
}

void EditorWorkspaceHost::ProtectRecoveryGroup(std::string id)
{
    if (std::find(recoveryProtectedGroups_.begin(), recoveryProtectedGroups_.end(), id) ==
        recoveryProtectedGroups_.end())
    {
        recoveryProtectedGroups_.push_back(std::move(id));
    }
}

void EditorWorkspaceHost::FinishRecovery(bool discardCurrent)
{
    if (recoveryDirectory_.empty() || !discardCurrent)
    {
        return;
    }
    std::string error;
    const auto groups = EditorSessionStore::ListRecoveryGroups(recoveryDirectory_, projectRoot_, error);
    for (const auto& group : groups)
    {
        if (std::find(recoveryProtectedGroups_.begin(), recoveryProtectedGroups_.end(), group.id) ==
            recoveryProtectedGroups_.end())
        {
            if (!EditorSessionStore::DiscardRecoveryGroup(recoveryDirectory_, group.id, error))
            {
                LogConsole(1, error);
            }
        }
    }
}

void EditorWorkspaceHost::PruneRecoveryDocuments(const std::vector<RecoveryDocument>& current)
{
    if (!recoveryCaptureValid_ || recoverySignature_ == recoveryPrunedSignature_)
    {
        return;
    }
    std::string error;
    const auto groups = EditorSessionStore::ListRecoveryGroups(recoveryDirectory_, projectRoot_, error);
    if (!error.empty())
    {
        LogConsole(1, error);
        return;
    }
    std::vector<std::string> obsolete;
    for (const auto& group : groups)
    {
        if (std::find(recoveryProtectedGroups_.begin(), recoveryProtectedGroups_.end(), group.id) !=
            recoveryProtectedGroups_.end())
        {
            continue;
        }
        bool changed = false;
        for (const auto& document : group.documents)
        {
            const bool stillPresent = std::any_of(current.begin(), current.end(), [&](const auto& active) {
                return active.documentId == document.documentId &&
                       active.relativePath == document.relativePath &&
                       active.sourceDigest == document.sourceDigest;
            });
            if (!stillPresent)
            {
                changed = true;
            }
        }
        if (changed)
        {
            obsolete.push_back(group.id);
        }
    }
    if (!obsolete.empty() && !current.empty())
    {
        if (!EditorSessionStore::WriteRecoveryGroup(recoveryDirectory_, projectRoot_, current, error,
                                                    200ull * 1024 * 1024, 5, recoveryProtectedGroups_))
        {
            LogConsole(1, error);
            return;
        }
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
        recoveryDebounce_.Mark(now, recoveryRevision_);
    }
    for (const auto& id : obsolete)
    {
        if (!EditorSessionStore::DiscardRecoveryGroup(recoveryDirectory_, id, error) &&
            error != "Editor.RecoveryNotFound")
        {
            LogConsole(1, error);
            return;
        }
    }
    recoveryPrunedSignature_ = recoverySignature_;
}

void EditorWorkspaceHost::UpdateRecovery(const EditorSession& editor, const UiTree* ui, bool pauseCapture)
{
    if (recoveryDirectory_.empty() || recoveryOffered_ || editor.IsPlaying() || material_.Editing() ||
        ui_.Editing() || pauseCapture)
    {
        return;
    }
    const auto documents = CaptureRecoveryDocuments(editor, ui);
    PruneRecoveryDocuments(documents);
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (!documents.empty() && now >= recoveryRetryAfter_ && recoveryDebounce_.Due(now, recoveryRevision_))
    {
        std::string error;
        if (EditorSessionStore::WriteRecoveryGroup(recoveryDirectory_, projectRoot_, documents, error,
                                                   200ull * 1024 * 1024, 5, recoveryProtectedGroups_))
        {
            recoveryDebounce_.Mark(now, recoveryRevision_);
            recoveryRetryAfter_ = 0;
        }
        else
        {
            LogConsole(1, error);
            recoveryRetryAfter_ = now + 60;
        }
    }
}

void EditorWorkspaceHost::ConfigurePreferences(const EditorPreferences& preferences,
                                               std::filesystem::path directory,
                                               std::function<bool(const std::string&)> language)
{
    preferences_ = preferences;
    preferencesDirectory_ = std::move(directory);
    changeLanguage_ = std::move(language);
}

bool EditorWorkspaceHost::SavePreferences()
{
    if (preferencesDirectory_.empty())
    {
        preferencesDirectory_ = EditorSessionStore::UserDataDirectory();
    }
    std::string error;
    if (!EditorSessionStore::SavePreferences(preferencesDirectory_, preferences_, error))
    {
        errorKey_ = error;
        LogConsole(1, error);
        return false;
    }
    if (changeLanguage_ && !changeLanguage_(preferences_.language))
    {
        errorKey_ = "Editor.PreferencesLanguageFailed";
        return false;
    }
    errorKey_.clear();
    LogConsole(0, "Editor.PreferencesSaved");
    return true;
}

std::filesystem::path EditorWorkspaceHost::ColorLutPath() const
{
    if (preferences_.colorLut.empty())
    {
        return {};
    }
    auto path = asset_io::FromUtf8(preferences_.colorLut);
    if (path.is_relative() && !projectRoot_.empty())
    {
        path = projectRoot_ / path;
    }
    return path;
}

bool EditorWorkspaceHost::ExportBusy() const
{
    return exportJob_ && exportJob_->Running();
}

bool EditorWorkspaceHost::ExportFinished() const
{
    return exportSnapshot_.finished && !(exportJob_ && exportJob_->Running());
}

void EditorWorkspaceHost::RequestCancelExport()
{
    if (exportJob_)
    {
        exportJob_->Cancel();
    }
}

bool EditorWorkspaceHost::StartExport(const std::string& name, std::string& errorKey)
{
    errorKey.clear();
    if (ExportBusy())
    {
        errorKey = "Editor.ExportBusy";
        return false;
    }
    if (name.empty() || name.size() > 128 || name.find_first_of("<>:\"/\\|?*") != std::string::npos)
    {
        errorKey = "Project.InvalidName";
        return false;
    }
    const auto root = EditorRoot();
    const auto manifest = ProjectManifestPath();
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(manifest, fileError))
    {
        errorKey = "Project.ManifestNotFound";
        return false;
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char stamp[64]{};
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u-%llu", time.wYear, time.wMonth, time.wDay,
                  time.wHour, time.wMinute, time.wSecond, static_cast<unsigned long long>(++exportSequence_));
    const auto evidence = userOperationDirectory_ / (std::string("export-") + stamp);
    ExportJobRequest request{manifest, name, evidence, workspaceGeneration_};
    if (!exportJob_)
    {
        exportJob_ = std::make_unique<ExportJob>();
    }
    ExportService::Runner runner = [this, root, evidence](const std::vector<std::string>& arguments,
                                                          std::string& output,
                                                          const ExportService::Progress& progress) {
        std::vector<std::wstring> wide = {L"-NoProfile", L"-ExecutionPolicy", L"Bypass", L"-File",
                                          (root / L"Scripts" / L"Export-Game.ps1").wstring(), L"-SkipLease",
                                          L"-SkipSmoke"};
        for (const auto& argument : arguments)
        {
            wide.push_back(asset_io::FromUtf8(argument).wstring());
        }
        std::string lastPhase;
        const auto tick = [&] {
            const auto report = ExportService::ReadReport(evidence);
            if (report.valid && !report.phase.empty() && report.phase != lastPhase)
            {
                lastPhase = report.phase;
                if (progress)
                {
                    progress(report.phase);
                }
            }
        };
        ProcessRunResult result;
        const auto* cancel = exportJob_ ? &exportJob_->CancelFlag() : nullptr;
        const bool started =
            RunEditorProcess(WindowsPowerShellPath(), wide, root, evidence / "editor-export.log", cancel, result, tick);
        output = result.output;
        if (!started)
        {
            return -1;
        }
        return result.cancelled ? 1 : result.exitCode;
    };
    if (!exportJob_->Start(request, runner, errorKey))
    {
        return false;
    }
    exportSnapshot_ = exportJob_->Snapshot();
    exportJoined_ = false;
    lastLoggedOperation_ = 0;
    LogConsole(0, "Editor.ExportStarted", name + " -> " + evidence.string());
    return true;
}

void EditorWorkspaceHost::PollExport(const Localization&)
{
    if (!exportReportLoaded_)
    {
        exportReportLoaded_ = true;
        ExportJobSnapshot loaded;
        std::string error;
        if (LoadExportSnapshot(userOperationDirectory_, loaded, error) && loaded.finished)
        {
            exportSnapshot_ = loaded;
        }
    }
    if (exportJob_)
    {
        const bool running = exportJob_->Running();
        exportSnapshot_ = exportJob_->Snapshot();
        if (!running && !exportJoined_)
        {
            exportJob_->Join();
            exportJoined_ = true;
            std::string error;
            SaveExportSnapshot(userOperationDirectory_, exportSnapshot_, error);
        }
    }
    const auto& report = exportSnapshot_.report;
    if (report.operationId != 0 && report.operationId != lastLoggedOperation_ && exportSnapshot_.finished)
    {
        lastLoggedOperation_ = report.operationId;
        if (exportSnapshot_.cancelled || report.state == ExportOperationState::Cancelled)
        {
            LogConsole(1, "Editor.ExportCancelled");
        }
        else if (report.state == ExportOperationState::Succeeded)
        {
            LogConsole(0, "Editor.ExportSucceeded", exportSnapshot_.outputPackage.string());
        }
        else
        {
            LogConsole(2, report.diagnosticKey.empty() ? "Editor.ExportFailed" : report.diagnosticKey,
                       exportSnapshot_.detail.empty() ? report.logPath : exportSnapshot_.detail);
        }
    }
}

void EditorWorkspaceHost::OpenInExplorer(const std::filesystem::path& path, bool selectFile) const
{
    if (path.empty())
    {
        return;
    }
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        return;
    }
    if (selectFile)
    {
        const auto arguments = L"/select,\"" + path.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void EditorWorkspaceHost::ConfigureBuildControl()
{
    restartRequestPath_ = EnvironmentPath(L"SELFENGINE_EDITOR_RESTART_REQUEST");
    const auto statusPath = EnvironmentPath(L"SELFENGINE_EDITOR_BUILD_STATUS");
    buildDiagnosticsPath_ = EnvironmentPath(L"SELFENGINE_EDITOR_BUILD_DIAGNOSTICS");
    if (restartRequestPath_.empty())
    {
        return;
    }
    if (!asset_io::Within(projectRoot_ / "out" / "Editor", restartRequestPath_) ||
        restartRequestPath_.filename() != L"restart.json" ||
        statusPath.parent_path() != restartRequestPath_.parent_path())
    {
        restartRequestPath_.clear();
        buildStale_ = true;
        LogConsole(2, "Editor.BuildControlInvalid");
        return;
    }
    std::vector<std::uint8_t> bytes;
    if (!asset_io::Read(statusPath, bytes))
    {
        buildStale_ = true;
        LogConsole(2, "Editor.BuildControlInvalid");
        return;
    }
    try
    {
        buildStatusJson_.assign(bytes.begin(), bytes.end());
        const auto status = Json::parse(buildStatusJson_);
        if (status.value("schemaVersion", 0) != 1)
        {
            throw std::runtime_error("schema");
        }
        buildHasCandidate_ = status.value("hasCandidate", false);
        buildStale_ = status.value("state", std::string{}) != "ready";
        if (buildStale_)
        {
            LogConsole(2, status.value("diagnosticKey", std::string("Editor.BuildRequired")),
                       asset_io::Utf8(buildDiagnosticsPath_));
        }
    }
    catch (...)
    {
        buildStale_ = true;
        buildStatusJson_.clear();
        LogConsole(2, "Editor.BuildControlInvalid");
    }
}

bool EditorWorkspaceHost::ValidateBuildSources()
{
    if (allowStaleBuild_)
    {
        return true;
    }
    if (buildStale_)
    {
        return false;
    }
    if (buildStatusJson_.empty())
    {
        return true;
    }
    try
    {
        const auto status = Json::parse(buildStatusJson_);
        BuildSettings currentBuild;
        std::string buildError;
        if (!buildHasCandidate_ && std::filesystem::exists(projectRoot_ / "build.json"))
        {
            if (!ProjectSettingsService::ReadBuild(projectRoot_ / "build.json", currentBuild, buildError) ||
                !currentBuild.scriptsProject.empty())
            {
                buildStale_ = true;
                return false;
            }
        }
        std::set<std::string> expected;
        std::set<std::string> observed;
        for (const auto& source : status.value("sourceFiles", Json::array()))
        {
            const auto relative = source.at("path").get<std::string>();
            const auto path = projectRoot_ / asset_io::FromUtf8(relative);
            std::vector<std::uint8_t> bytes;
            if (!asset_io::Within(projectRoot_, path) || !asset_io::Read(path, bytes) ||
                Lower(asset_io::Hash(bytes)) != Lower(source.at("sha256").get<std::string>()))
            {
                buildStale_ = true;
                return false;
            }
            expected.insert(Lower(relative));
        }
        for (const auto& directory : status.value("sourceDirectories", std::vector<std::string>{}))
        {
            const auto path = (projectRoot_ / asset_io::FromUtf8(directory)).lexically_normal();
            if (!asset_io::Within(projectRoot_, path))
            {
                buildStale_ = true;
                return false;
            }
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator it(path, error), end; !error && it != end;
                 it.increment(error))
            {
                const auto name = Lower(asset_io::Utf8(it->path().filename()));
                if (it->is_directory(error) &&
                    (name == "out" || name == "bin" || name == "obj" || name == ".git" || name == ".vs" ||
                     name == ".cache"))
                {
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->is_symlink(error))
                {
                    it.disable_recursion_pending();
                    buildStale_ = true;
                    return false;
                }
                if (it->is_regular_file(error) && SourceFile(it->path(), status))
                {
                    observed.insert(Lower(asset_io::Utf8(
                        it->path().lexically_normal().lexically_relative(projectRoot_.lexically_normal())
                            .generic_wstring())));
                }
            }
            if (error)
            {
                buildStale_ = true;
                return false;
            }
        }
        if (expected != observed)
        {
            buildStale_ = true;
            return false;
        }
        return true;
    }
    catch (...)
    {
        buildStale_ = true;
        return false;
    }
}

bool EditorWorkspaceHost::PublishRestartRequest(bool closeApproved)
{
    if (!recompileRequested_)
    {
        return true;
    }
    if (!closeApproved || restartRequestPath_.empty())
    {
        return false;
    }
    std::string error;
    if (!WriteBuildRestartRequest(restartRequestPath_, error))
    {
        LogConsole(2, "Editor.BuildControlInvalid");
        return false;
    }
    return true;
}

void EditorWorkspaceHost::LogConsole(int level, const std::string& key, const std::string& detail)
{
    consoleEntries_.push_back({level, key, detail});
    if (consoleEntries_.size() > 500)
    {
        consoleEntries_.erase(consoleEntries_.begin(), consoleEntries_.begin() + 100);
    }
}

} // namespace se
