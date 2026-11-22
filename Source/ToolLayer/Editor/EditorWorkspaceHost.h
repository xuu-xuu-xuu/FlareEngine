#pragma once

#include "EditorAssetActions.h"
#include "EditorCommand.h"
#include "EditorExternalDocuments.h"
#include "EditorMaterialWorkflow.h"
#include "EditorSession.h"
#include "EditorSessionStore.h"
#include "EditorSettingsDocuments.h"
#include "ExportJob.h"
#include "SceneFileWorkflow.h"
#include "TODPreview.h"
#include "UiDocumentWorkflow.h"
#include "CoreLayer/Localization.h"
#include "FunctionLayer/GameUI/UiTree.h"
#include "ResourceLayer/Assets/AssetStore.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flare
{

// 拥有编辑器非 UI 宿主的全部文档、设置、恢复、导出与构建控制工作流。
class EditorWorkspaceHost
{
public:
    enum class DocumentContext
    {
        Scene,
        Material,
        Ui,
        ProjectSettings,
        BuildSettings
    };

    enum class ExternalConflictKind
    {
        None,
        Material,
        Ui,
        ProjectSettings,
        BuildSettings
    };

    struct ConsoleEntry
    {
        int level = 0;
        std::string key;
        std::string detail;
    };

    EditorWorkspaceHost();
    ~EditorWorkspaceHost();
    EditorWorkspaceHost(const EditorWorkspaceHost&) = delete;
    EditorWorkspaceHost& operator=(const EditorWorkspaceHost&) = delete;

    void SetProjectRoot(std::filesystem::path root);
    void SetDocumentPaths(const std::filesystem::path& scene, const std::filesystem::path& ui);
    void SetUserOperationDirectory(std::filesystem::path directory) { userOperationDirectory_ = std::move(directory); }
    [[nodiscard]] const std::filesystem::path& ProjectRoot() const noexcept { return projectRoot_; }
    [[nodiscard]] std::filesystem::path ProjectManifestPath() const;
    [[nodiscard]] std::filesystem::path EditorRoot() const;
    [[nodiscard]] const std::filesystem::path& DefaultScenePath() const noexcept { return defaultScenePath_; }

    [[nodiscard]] UiDocumentWorkflow& Ui() noexcept { return ui_; }
    [[nodiscard]] const UiDocumentWorkflow& Ui() const noexcept { return ui_; }
    [[nodiscard]] EditorMaterialWorkflow& Material() noexcept { return material_; }
    [[nodiscard]] const EditorMaterialWorkflow& Material() const noexcept { return material_; }
    [[nodiscard]] TODPreview& TimeOfDayPreview() noexcept { return tod_; }
    [[nodiscard]] const TODPreview& TimeOfDayPreview() const noexcept { return tod_; }
    [[nodiscard]] SceneFileWorkflow& Files() noexcept { return files_; }
    [[nodiscard]] const SceneFileWorkflow& Files() const noexcept { return files_; }
    [[nodiscard]] EditorDocumentRegistry& Documents() noexcept { return documents_; }

    bool OpenMaterial(AssetStore& assets, const std::filesystem::path& requested, bool discard = false);
    bool SaveMaterial(AssetStore& assets, const std::filesystem::path& targetPath = {}, bool overwrite = false);
    bool OpenUi(UiTree& ui, const std::filesystem::path& path, bool discard = false);
    bool SaveUi(UiTree& ui, const std::filesystem::path& path);

    bool LoadProjectSettings(bool discardUnsaved = false);
    bool LoadBuildSettings(bool discardUnsaved = false);
    bool SetProjectSettings(const ProjectSettings& settings);
    bool SetBuildSettings(const BuildSettings& settings);
    bool SaveSettings(bool project);
    [[nodiscard]] EditorSettingsDocument& ProjectSettingsDoc() noexcept { return projectSettingsDocument_; }
    [[nodiscard]] const EditorSettingsDocument& ProjectSettingsDoc() const noexcept { return projectSettingsDocument_; }
    [[nodiscard]] EditorSettingsDocument& BuildSettingsDoc() noexcept { return buildSettingsDocument_; }
    [[nodiscard]] const EditorSettingsDocument& BuildSettingsDoc() const noexcept { return buildSettingsDocument_; }
    [[nodiscard]] const ProjectSettings& Project() const noexcept { return projectSettings_; }
    [[nodiscard]] const BuildSettings& Build() const noexcept { return buildSettings_; }

    void SetExternalDocuments(EditorExternalDocuments documents);
    void SyncDocumentRegistry(EditorSession& editor, UiTree* ui);
    bool CaptureEditingDocuments(const EditorSession& editor, AssetStore& assets, UiTree* ui,
                                 std::vector<EditorAssetDocument>& documents);
    bool SaveAllDocuments(EditorSession& editor, AssetStore& assets, UiTree* ui);
    [[nodiscard]] bool DocumentsDirty(const UiTree* ui) const;
    [[nodiscard]] bool AnyDocumentDirty(const EditorSession& editor, const UiTree* ui) const;

    bool ReloadExternalDocument(EditorSession& editor, AssetStore& assets, UiTree* ui);
    bool SaveExternalCopy(AssetStore& assets, UiTree* ui, std::filesystem::path& copyPath);
    [[nodiscard]] ExternalConflictKind ExternalConflict() const noexcept { return externalConflict_; }
    [[nodiscard]] const std::filesystem::path& ExternalConflictPath() const noexcept { return externalConflictPath_; }
    void ClearExternalConflict() noexcept;
    void SyncAssetDeclarations(AssetStore& assets);

    void SetDocumentContext(DocumentContext context) noexcept { documentContext_ = context; }
    [[nodiscard]] DocumentContext ActiveDocumentContext() const noexcept { return documentContext_; }
    [[nodiscard]] std::vector<std::string> SessionDocuments(const EditorSession& editor) const;
    [[nodiscard]] std::string SessionActiveDocument(const EditorSession& editor) const;
    std::optional<DocumentContext> RestoreSessionDocuments(const std::vector<std::string>& documents,
                                                           AssetStore& assets, UiTree& ui,
                                                           std::string_view activePath = {});

    void ConfigureRecovery(std::filesystem::path directory, bool offerRecovery);
    [[nodiscard]] std::vector<RecoveryDocument> CaptureRecoveryDocuments(const EditorSession& editor, const UiTree* ui);
    [[nodiscard]] bool RecoverySourceChanged(const RecoveryGroup& group) const;
    bool RestoreRecovery(const RecoveryGroup& group, EditorSession& editor, AssetStore& assets, UiTree& ui);
    bool SaveRecoveryCopy(const RecoveryGroup& group, std::filesystem::path& destination);
    void PruneRecoveryDocuments(const std::vector<RecoveryDocument>& current);
    void UpdateRecovery(const EditorSession& editor, const UiTree* ui, bool pauseCapture = false);
    void FinishRecovery(bool discardCurrent);
    void ProtectRecoveryGroup(std::string id);
    [[nodiscard]] const std::vector<RecoveryGroup>& RecoveryGroups() const noexcept { return recoveryGroups_; }
    [[nodiscard]] bool RecoveryOffered() const noexcept { return recoveryOffered_; }
    void DismissRecoveryOffer() noexcept { recoveryOffered_ = false; }

    void ConfigurePreferences(const EditorPreferences& preferences, std::filesystem::path directory,
                              std::function<bool(const std::string&)> language);
    [[nodiscard]] EditorPreferences& Preferences() noexcept { return preferences_; }
    [[nodiscard]] const EditorPreferences& Preferences() const noexcept { return preferences_; }
    bool SavePreferences();
    [[nodiscard]] std::filesystem::path ColorLutPath() const;

    bool StartExport(const std::string& name, std::string& errorKey);
    void PollExport(const Localization& text);
    [[nodiscard]] bool ExportBusy() const;
    [[nodiscard]] bool ExportFinished() const;
    [[nodiscard]] ExportJobSnapshot ExportSnapshot() const { return exportSnapshot_; }
    void RequestCancelExport();
    void OpenInExplorer(const std::filesystem::path& path, bool selectFile = false) const;

    void ConfigureBuildControl();
    bool ValidateBuildSources();
    bool PublishRestartRequest(bool closeApproved);
    void MarkRecompileRequested() noexcept { recompileRequested_ = true; }
    [[nodiscard]] bool HasBuildControl() const noexcept { return !restartRequestPath_.empty(); }
    [[nodiscard]] bool BuildStale() const noexcept { return buildStale_; }
    [[nodiscard]] bool BuildHasCandidate() const noexcept { return buildHasCandidate_; }
    [[nodiscard]] bool AllowStaleBuild() const noexcept { return allowStaleBuild_; }
    void SetAllowStaleBuild(bool allow) noexcept { allowStaleBuild_ = allow; }

    void LogConsole(int level, const std::string& key, const std::string& detail = {});
    [[nodiscard]] const std::vector<ConsoleEntry>& ConsoleLines() const noexcept { return consoleEntries_; }
    [[nodiscard]] const std::string& LastError() const noexcept { return errorKey_; }

private:
    std::filesystem::path projectRoot_;
    std::filesystem::path userOperationDirectory_;
    std::filesystem::path defaultScenePath_;
    SceneFileWorkflow files_;
    EditorDocumentRegistry documents_;
    UiDocumentWorkflow ui_;
    EditorMaterialWorkflow material_;
    TODPreview tod_;
    DocumentContext documentContext_ = DocumentContext::Scene;

    EditorSettingsDocument projectSettingsDocument_{EditorSettingsKind::Project};
    EditorSettingsDocument buildSettingsDocument_{EditorSettingsKind::Build};
    ProjectSettings projectSettings_;
    BuildSettings buildSettings_;
    std::string projectSettingsError_;
    std::string buildSettingsError_;

    EditorExternalDocuments externalDocuments_;
    std::vector<EditorExternalDocument> externalDocumentSnapshots_;
    bool externalDocumentsValid_ = true;
    ExternalConflictKind externalConflict_ = ExternalConflictKind::None;
    std::filesystem::path externalConflictPath_;

    EditorPreferences preferences_;
    std::filesystem::path preferencesDirectory_;
    std::function<bool(const std::string&)> changeLanguage_;

    std::unique_ptr<ExportJob> exportJob_;
    ExportJobSnapshot exportSnapshot_;
    bool exportReportLoaded_ = false;
    bool exportJoined_ = true;
    std::uint64_t exportSequence_ = 0;
    std::uint64_t lastLoggedOperation_ = 0;
    std::uint64_t workspaceGeneration_ = 1;

    std::filesystem::path recoveryDirectory_;
    std::filesystem::path recoveryScenePath_;
    std::string recoverySceneDigest_;
    std::string recoverySignature_;
    std::string recoveryPrunedSignature_;
    std::uint64_t recoveryRevision_ = 0;
    std::uint64_t recoveryRetryAfter_ = 0;
    EditorSessionStore::RecoveryDebounce recoveryDebounce_;
    std::vector<RecoveryGroup> recoveryGroups_;
    bool recoveryOffered_ = false;
    bool recoveryCaptureValid_ = true;
    std::vector<std::string> recoveryProtectedGroups_;

    std::filesystem::path restartRequestPath_;
    std::filesystem::path buildDiagnosticsPath_;
    std::string buildStatusJson_;
    bool buildStale_ = false;
    bool allowStaleBuild_ = false;
    bool recompileRequested_ = false;
    bool buildHasCandidate_ = false;

    std::vector<ConsoleEntry> consoleEntries_;
    std::string errorKey_;
};

} // namespace flare
