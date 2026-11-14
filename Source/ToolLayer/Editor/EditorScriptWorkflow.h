#pragma once
#include "EditorUi/EditorPanels.h"
#include "EditorSessionStore.h"
#include <filesystem>
#include <chrono>
#include <string>
#include <vector>

class Localization;
namespace se
{
struct EditorApplicationHooks;
struct CodeEditorSettings;

// Toolkit 的脚本面板适配器。脚本身份、字段校验和保存仍由托管绑定文档拥有。
class EditorScriptWorkflow
{
public:
    EditorScriptWorkflow(std::filesystem::path root,EditorApplicationHooks* hooks,const Localization& text);
    void AppendRows(std::uint64_t entity,bool playing,std::vector<ui::editor::EditorProperty>& rows);
    bool Apply(std::uint64_t entity,bool playing,const std::string& key,const std::string& value,
               const CodeEditorSettings& settings);
    const std::string& Status()const noexcept{return status_;}
    bool Save();
    const std::vector<std::filesystem::path>& Sources();
    void RefreshSources(){scanned_={};}
    bool OpenSelected(const CodeEditorSettings& settings){return Open(selectedSource_,settings);}
    void SelectSource(std::filesystem::path path){selectedSource_=path.extension()==".cs"?std::move(path):std::filesystem::path{};}
private:
    bool Open(const std::filesystem::path& source,const CodeEditorSettings& settings);
    bool Snapshot(RecoveryDocument& document);
    bool Restore(bool redo);
    struct Change {RecoveryDocument before,after;};
    std::vector<Change> undo_,redo_;
    std::filesystem::path root_;
    EditorApplicationHooks* hooks_;
    const Localization& text_;
    std::string className_="NewBehaviour",status_;
    std::filesystem::path selectedSource_;
    std::vector<std::filesystem::path> sources_;
    std::chrono::steady_clock::time_point scanned_{};
};
}
