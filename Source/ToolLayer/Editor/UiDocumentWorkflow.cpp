#include "UiDocumentWorkflow.h"

#include "ResourceLayer/Assets/AssetIO.h"

#include <cmath>

namespace se
{
namespace
{
bool EquivalentPath(const std::filesystem::path& left, const std::filesystem::path& right)
{
    return !left.empty() && !right.empty() && asset_io::Within(left, right) && asset_io::Within(right, left);
}
} // namespace

void UiDocumentWorkflow::SetDefaultPath(std::filesystem::path path)
{
    defaultPath_ = std::move(path);
}

void UiDocumentWorkflow::SetProjectRoot(std::filesystem::path root)
{
    projectRoot_ = std::move(root);
}

bool UiDocumentWorkflow::SameSavedPath(const std::filesystem::path& candidate) const
{
    return EquivalentPath(candidate, savedPath_);
}

bool UiDocumentWorkflow::CommitSnapshot(UiTree& ui, std::string before)
{
    const auto after = ui.SaveJson();
    if (before == after)
    {
        return true;
    }
    if (!transaction_.empty())
    {
        return true;
    }
    if (undo_.size() == MaxUndoEntries)
    {
        undo_.erase(undo_.begin());
    }
    undo_.push_back({std::move(before), after});
    redo_.clear();
    return true;
}

void UiDocumentWorkflow::Bind(UiTree& ui)
{
    if (owner_ == &ui)
    {
        return;
    }
    owner_ = &ui;
    selected_ = 0;
    undo_.clear();
    redo_.clear();
    transaction_.clear();
    savedJson_.clear();
    savedPath_.clear();
    stamp_ = {};
    externalConflict_ = false;
    if (!defaultPath_.empty() && (projectRoot_.empty() || asset_io::Within(projectRoot_, defaultPath_)))
    {
        std::vector<std::uint8_t> bytes;
        if (asset_io::Read(defaultPath_, bytes))
        {
            UiTree stored;
            std::string error;
            const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (stored.LoadJson(text, error) && stored.SaveJson() == ui.SaveJson())
            {
                savedJson_ = ui.SaveJson();
                savedPath_ = defaultPath_;
                RecordEditorFileStamp(stamp_, defaultPath_);
            }
        }
    }
    canvas_.Reset();
}

bool UiDocumentWorkflow::Open(UiTree& ui, const std::filesystem::path& path, bool discardUnsaved)
{
    Bind(ui);
    if (!transaction_.empty())
    {
        errorKey_ = "Editor.TransactionActive";
        return false;
    }
    if (!discardUnsaved && (savedJson_.empty() ? ui.Count() != 0 : ui.SaveJson() != savedJson_))
    {
        errorKey_ = "Editor.UnsavedChanges";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!asset_io::Read(path, bytes))
    {
        errorKey_ = "EditorUi.UiReadFailed";
        return false;
    }
    if (!ui.LoadJson(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), errorKey_))
    {
        return false;
    }
    savedJson_ = ui.SaveJson();
    selected_ = 0;
    undo_.clear();
    redo_.clear();
    savedPath_ = path;
    defaultPath_ = path;
    RecordEditorFileStamp(stamp_, path);
    externalConflict_ = false;
    errorKey_.clear();
    return true;
}

bool UiDocumentWorkflow::Save(UiTree& ui, const std::filesystem::path& path)
{
    Bind(ui);
    externalConflict_ = false;
    if (!transaction_.empty())
    {
        errorKey_ = "Editor.TransactionActive";
        return false;
    }
    if (!savedPath_.empty() && EquivalentPath(path, savedPath_) && EditorFileStampChanged(stamp_, path))
    {
        externalConflict_ = true;
        errorKey_ = "Editor.ExternalChange";
        return false;
    }
    if (!SameSavedPath(path) && std::filesystem::exists(path))
    {
        externalConflict_ = true;
        errorKey_ = "Editor.ExternalChange";
        return false;
    }
    const auto json = ui.SaveJson();
    if (!asset_io::WriteAtomic(path, {reinterpret_cast<const std::uint8_t*>(json.data()), json.size()}))
    {
        errorKey_ = "EditorUi.UiWriteFailed";
        return false;
    }
    savedJson_ = json;
    savedPath_ = path;
    defaultPath_ = path;
    RecordEditorFileStamp(stamp_, path);
    errorKey_.clear();
    return true;
}

void UiDocumentWorkflow::MarkSaved(UiTree& ui, const std::filesystem::path& path)
{
    Bind(ui);
    savedJson_ = ui.SaveJson();
    savedPath_ = path;
    if (!path.empty())
    {
        defaultPath_ = path;
        RecordEditorFileStamp(stamp_, path);
    }
    externalConflict_ = false;
}

void UiDocumentWorkflow::RestoreRecovered(UiTree& ui, const std::filesystem::path& path,
                                          const std::filesystem::path& fallbackPath)
{
    Bind(ui);
    savedPath_ = path;
    defaultPath_ = path.empty() ? fallbackPath : path;
    savedJson_.clear();
    RecordEditorFileStamp(stamp_, savedPath_);
    undo_.clear();
    redo_.clear();
    selected_ = 0;
    externalConflict_ = false;
}

bool UiDocumentWorkflow::Edit(UiTree& ui, const std::function<bool(UiTree&)>& operation)
{
    Bind(ui);
    const auto before = ui.SaveJson();
    if (!operation || !operation(ui))
    {
        std::string error;
        ui.LoadJson(before, error);
        return false;
    }
    return CommitSnapshot(ui, before);
}

void UiDocumentWorkflow::BeginEdit(UiTree& ui)
{
    Bind(ui);
    if (transaction_.empty())
    {
        transaction_ = ui.SaveJson();
    }
}

void UiDocumentWorkflow::EndEdit(UiTree& ui, bool commit)
{
    if (transaction_.empty())
    {
        return;
    }
    auto before = std::move(transaction_);
    transaction_.clear();
    if (commit)
    {
        CommitSnapshot(ui, std::move(before));
    }
    else
    {
        ui.LoadJson(before, errorKey_);
    }
}

bool UiDocumentWorkflow::Undo(UiTree& ui)
{
    Bind(ui);
    if (undo_.empty() || !transaction_.empty())
    {
        return false;
    }
    if (!ui.LoadJson(undo_.back().before, errorKey_))
    {
        return false;
    }
    if (!ui.Get(selected_))
    {
        selected_ = 0;
    }
    redo_.push_back(std::move(undo_.back()));
    undo_.pop_back();
    return true;
}

bool UiDocumentWorkflow::Redo(UiTree& ui)
{
    Bind(ui);
    if (redo_.empty() || !transaction_.empty())
    {
        return false;
    }
    if (!ui.LoadJson(redo_.back().after, errorKey_))
    {
        return false;
    }
    if (!ui.Get(selected_))
    {
        selected_ = 0;
    }
    undo_.push_back(std::move(redo_.back()));
    redo_.pop_back();
    return true;
}

bool UiDocumentWorkflow::BeginDrag(UiTree& ui, UiId id, UiPoint screen)
{
    BeginEdit(ui);
    UiCanvasDrag mode = UiCanvasDrag::Move;
    if (const auto* element = ui.Get(id))
    {
        const UiPoint local = canvas_.ToPreview(screen);
        const UiRect r = element->rectangle;
        if (std::abs(local.x - (r.x + r.width)) <= 10.0f && std::abs(local.y - (r.y + r.height)) <= 10.0f)
        {
            mode = UiCanvasDrag::Resize;
        }
    }
    if (!canvas_.Begin(ui, id, mode, screen))
    {
        EndEdit(ui, false);
        return false;
    }
    selected_ = id;
    return true;
}

bool UiDocumentWorkflow::UpdateDrag(UiTree& ui, UiPoint screen)
{
    return canvas_.Update(ui, screen);
}

bool UiDocumentWorkflow::EndDrag(UiTree& ui, bool commit)
{
    const bool finished = canvas_.End(ui, commit);
    EndEdit(ui, commit && finished);
    return finished;
}

bool UiDocumentWorkflow::Dirty(const UiTree* ui) const
{
    if (!ui || owner_ != ui)
    {
        return false;
    }
    if (savedJson_.empty())
    {
        return ui->Count() > 0;
    }
    return ui->SaveJson() != savedJson_;
}

} // namespace se
