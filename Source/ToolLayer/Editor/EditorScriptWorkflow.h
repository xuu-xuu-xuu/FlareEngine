#pragma once
#include "EditorUi/EditorPanels.h"
#include "EditorSessionStore.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Localization;
namespace flare
{
struct EditorApplicationHooks;
struct CodeEditorSettings;

// Toolkit 的脚本面板与 Unity 式挂载适配器。脚本身份、字段校验和保存仍由托管绑定文档拥有。
class EditorScriptWorkflow
{
public:
    struct ScriptSourceMeta
    {
        std::filesystem::path path;
        std::string scriptId;
        std::string className;
        std::filesystem::file_time_type modified{};
    };

    struct HotReloadResult
    {
        bool finished = false;
        bool success = false;
        std::uint64_t attachedEntity = 0;
        std::string attachedDisplayName;
        std::string statusMessage;
        std::vector<std::string> consoleLines;
    };

    EditorScriptWorkflow(std::filesystem::path root, EditorApplicationHooks* hooks, const Localization& text);
    ~EditorScriptWorkflow();
    void AppendRows(std::uint64_t entity, bool playing, std::vector<ui::editor::EditorProperty>& rows,
                    const std::function<std::string(std::uint64_t)>& resolveEntityName = {});
    void AppendMultiRows(const std::vector<std::uint64_t>& entities, bool playing,
                         std::vector<ui::editor::EditorProperty>& rows,
                         const std::function<std::string(std::uint64_t)>& resolveEntityName = {});
    bool Apply(std::uint64_t entity, bool playing, const std::string& key, const std::string& value,
               const CodeEditorSettings& settings);
    bool ApplyMulti(const std::vector<std::uint64_t>& entities, bool playing, const std::string& key,
                    const std::string& value, const CodeEditorSettings& settings);
    const std::string& Status() const noexcept { return status_; }
    bool Dirty() const;
    bool Save();
    bool BeginEdit();
    bool CommitEdit();
    bool CancelEdit();
    bool HasUndo() const noexcept { return !undo_.empty(); }
    bool HasRedo() const noexcept { return !redo_.empty(); }
    bool Undo() { return Restore(false); }
    bool Redo() { return Restore(true); }

    const std::vector<std::filesystem::path>& Sources();
    void RefreshSources() { scanned_ = {}; }
    bool SourcesNeedCompile();
    void MarkSourcesCompiled();
    std::string NextSuggestedClassName();

    [[nodiscard]] bool CanHotReloadInProcess() const noexcept;
    [[nodiscard]] bool IsCompiling() const noexcept { return compileTask_.valid(); }
    [[nodiscard]] bool HasCompileError() const noexcept { return hasCompileError_; }
    bool StartInProcessCompile(std::uint64_t attachEntity = 0, std::string attachScriptId = {});
    HotReloadResult PollInProcessCompile();

    std::string DisplayName(std::string_view scriptId);
    std::string ScriptIdForSource(const std::filesystem::path& sourcePath);
    std::string ClassNameForSource(const std::filesystem::path& sourcePath);

    bool AttachScript(std::uint64_t entity, const std::string& scriptId, bool& outNeedsCompile);
    bool RenameScriptSource(const std::filesystem::path& oldSource, std::string_view newClassName,
                            std::filesystem::path& outNewSource);
    bool ConsumeCreatedPendingAttach(std::uint64_t& outEntity, std::string& outScriptId,
                                     std::filesystem::path& outSource);

    static bool EnsureProjectBindings(const std::filesystem::path& projectRoot);
    static void QueuePendingAttach(const std::filesystem::path& crashDir, std::uint64_t entity,
                                   std::string_view scriptId);
    bool ConsumePendingAttachOnStartup(const std::filesystem::path& crashDir, std::uint64_t& outEntity,
                                       std::string& outDisplayName);
    [[nodiscard]] std::string CaptureLiveEntityScripts(std::uint64_t entity) const;
    bool ApplyCapturedEntityScripts(std::uint64_t entity, const std::string& capturedDocJson);
    bool CloneEntityBindings(const std::unordered_map<std::uint64_t, std::uint64_t>& idRemap);

    bool OpenSelected(const CodeEditorSettings& settings) { return Open(selectedSource_, 1, 1, settings); }
    bool OpenSourceAtLine(const std::filesystem::path& sourceOrRelative, std::uint32_t line, std::uint32_t column,
                          const CodeEditorSettings& settings);
    void SelectSource(std::filesystem::path path)
    {
        selectedSource_ = path.extension() == ".cs" ? std::move(path) : std::filesystem::path{};
    }

private:
    struct AsyncBuildOutput
    {
        bool success = false;
        std::filesystem::path dllPath;
        std::filesystem::path outDir;
        std::string logText;
        std::string errorKey;
        std::uint64_t attachEntity = 0;
        std::string attachScriptId;
    };

    bool Open(const std::filesystem::path& source, std::uint32_t line, std::uint32_t column,
              const CodeEditorSettings& settings);
    bool Snapshot(RecoveryDocument& document) const;
    bool Restore(bool redo);
    std::string ComputeSourcesSignature();

    struct Change
    {
        RecoveryDocument before, after;
    };
    std::vector<Change> undo_, redo_;
    std::optional<RecoveryDocument> transactionBefore_;
    std::filesystem::path root_;
    EditorApplicationHooks* hooks_;
    const Localization& text_;
    std::string className_ = "NewBehaviour", status_;
    bool customClassEdited_ = false;
    std::filesystem::path selectedSource_;
    std::vector<std::filesystem::path> sources_;
    std::vector<ScriptSourceMeta> meta_;
    std::chrono::steady_clock::time_point scanned_{};
    std::string compiledSignature_;
    bool compiledSignatureInitialized_ = false;
    bool hasCompileError_ = false;

    std::uint64_t pendingAttachEntity_ = 0;
    std::string pendingAttachScriptId_;
    std::filesystem::path pendingCreatedSource_;
    std::uint64_t hotReloadSeq_ = 0;
    std::future<AsyncBuildOutput> compileTask_;
    std::string clipboardScriptType_;
    std::string clipboardScriptJson_;
};
}
