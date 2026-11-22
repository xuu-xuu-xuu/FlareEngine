#pragma once

#include "ExternalChangeMonitor.h"
#include "UICanvasEditor.h"
#include "FunctionLayer/GameUI/UiTree.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace flare
{

// 拥有 .ui.json 文档打开/保存、有界撤销历史与画布拖拽事务，不依赖任何具体 UI 壳。
class UiDocumentWorkflow
{
public:
    static constexpr std::size_t MaxUndoEntries = 100;

    void SetDefaultPath(std::filesystem::path path);
    void SetProjectRoot(std::filesystem::path root);

    void Bind(UiTree& ui);
    bool Open(UiTree& ui, const std::filesystem::path& path, bool discardUnsaved = false);
    bool Save(UiTree& ui, const std::filesystem::path& path);
    void MarkSaved(UiTree& ui, const std::filesystem::path& path);
    void RestoreRecovered(UiTree& ui, const std::filesystem::path& path, const std::filesystem::path& fallbackPath);

    bool Edit(UiTree& ui, const std::function<bool(UiTree&)>& operation);
    void BeginEdit(UiTree& ui);
    void EndEdit(UiTree& ui, bool commit);
    bool Undo(UiTree& ui);
    bool Redo(UiTree& ui);

    void Select(UiId id) noexcept { selected_ = id; }
    [[nodiscard]] UiId Selected() const noexcept { return selected_; }

    bool BeginDrag(UiTree& ui, UiId id, UiPoint screen);
    bool UpdateDrag(UiTree& ui, UiPoint screen);
    bool EndDrag(UiTree& ui, bool commit);

    [[nodiscard]] bool Dirty(const UiTree* ui) const;
    [[nodiscard]] bool Editing() const noexcept { return !transaction_.empty(); }
    [[nodiscard]] bool SameSavedPath(const std::filesystem::path& candidate) const;

    [[nodiscard]] UICanvasEditor& Canvas() noexcept { return canvas_; }
    [[nodiscard]] const UICanvasEditor& Canvas() const noexcept { return canvas_; }
    [[nodiscard]] const std::filesystem::path& DefaultPath() const noexcept { return defaultPath_; }
    [[nodiscard]] const std::filesystem::path& SavedPath() const noexcept { return savedPath_; }
    [[nodiscard]] const std::filesystem::path& EffectivePath() const noexcept
    {
        return !savedPath_.empty() ? savedPath_ : defaultPath_;
    }
    [[nodiscard]] const EditorFileStamp& Stamp() const noexcept { return stamp_; }
    [[nodiscard]] const std::string& LastError() const noexcept { return errorKey_; }
    [[nodiscard]] bool ExternalConflict() const noexcept { return externalConflict_; }
    void ClearExternalConflict() noexcept { externalConflict_ = false; }

private:
    bool CommitSnapshot(UiTree& ui, std::string before);

    struct Command
    {
        std::string before;
        std::string after;
    };

    UiTree* owner_ = nullptr;
    UICanvasEditor canvas_;
    std::filesystem::path projectRoot_;
    std::filesystem::path defaultPath_;
    std::filesystem::path savedPath_;
    std::string savedJson_;
    std::string transaction_;
    EditorFileStamp stamp_;
    UiId selected_ = 0;
    std::vector<Command> undo_;
    std::vector<Command> redo_;
    std::string errorKey_;
    bool externalConflict_ = false;
};

} // namespace flare
