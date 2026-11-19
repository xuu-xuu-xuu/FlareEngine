#pragma once
#include "EditorUi/EditorPanels.h"
#include "EditorSessionStore.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class Localization;
namespace se
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

    EditorScriptWorkflow(std::filesystem::path root, EditorApplicationHooks* hooks, const Localization& text);
    void AppendRows(std::uint64_t entity, bool playing, std::vector<ui::editor::EditorProperty>& rows);
    bool Apply(std::uint64_t entity, bool playing, const std::string& key, const std::string& value,
               const CodeEditorSettings& settings);
    const std::string& Status() const noexcept { return status_; }
    bool Dirty() const;
    bool Save();
    const std::vector<std::filesystem::path>& Sources();
    void RefreshSources() { scanned_ = {}; }
    bool SourcesNeedCompile();
    void MarkSourcesCompiled();

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

    bool OpenSelected(const CodeEditorSettings& settings) { return Open(selectedSource_, settings); }
    void SelectSource(std::filesystem::path path)
    {
        selectedSource_ = path.extension() == ".cs" ? std::move(path) : std::filesystem::path{};
    }

private:
    bool Open(const std::filesystem::path& source, const CodeEditorSettings& settings);
    bool Snapshot(RecoveryDocument& document) const;
    bool Restore(bool redo);
    std::string ComputeSourcesSignature();

    struct Change
    {
        RecoveryDocument before, after;
    };
    std::vector<Change> undo_, redo_;
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

    std::uint64_t pendingAttachEntity_ = 0;
    std::string pendingAttachScriptId_;
    std::filesystem::path pendingCreatedSource_;
};
}
