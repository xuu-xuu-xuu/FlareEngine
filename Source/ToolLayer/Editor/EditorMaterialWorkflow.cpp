#include "EditorMaterialWorkflow.h"

#include "ResourceLayer/Assets/AssetIO.h"

namespace flare
{
namespace
{
bool EquivalentPath(const std::filesystem::path& left, const std::filesystem::path& right)
{
    return !left.empty() && !right.empty() && asset_io::Within(left, right) && asset_io::Within(right, left);
}
} // namespace

bool EditorMaterialWorkflow::Open(AssetStore& assets, const std::filesystem::path& requested, bool discardUnsaved,
                                  bool* newlyRegistered)
{
    if (newlyRegistered)
    {
        *newlyRegistered = false;
    }
    externalConflict_ = false;
    if (!discardUnsaved && Dirty())
    {
        errorKey_ = "Editor.UnsavedChanges";
        return false;
    }
    std::error_code pathError;
    const auto path = std::filesystem::weakly_canonical(
        requested.is_absolute() ? requested : assets.SourceRoot() / requested, pathError);
    if (pathError || path.empty())
    {
        errorKey_ = "Assets.MaterialPathInvalid";
        return false;
    }
    if (!asset_io::Within(assets.SourceRoot(), path))
    {
        errorKey_ = "Assets.ReferenceOutsideRoot";
        return false;
    }
    MaterialDocument loaded;
    std::vector<AssetDiagnostic> diagnostics;
    if (!LoadMaterialDocument(path, loaded, diagnostics))
    {
        errorKey_ = diagnostics.empty() ? "Assets.InvalidMaterial" : diagnostics.back().key;
        return false;
    }
    AssetId id;
    for (const auto& candidate : assets.ListIds())
    {
        if (EquivalentPath(path, assets.Resolve(candidate)))
        {
            id = candidate;
            break;
        }
    }
    const bool registered = id.empty();
    if (registered && !assets.RegisterSource(path, {}, id, diagnostics))
    {
        errorKey_ = diagnostics.empty() ? "Assets.InvalidMetadata" : diagnostics.back().key;
        return false;
    }
    if (!session_.Reset(loaded, true))
    {
        errorKey_ = session_.LastError();
        return false;
    }
    activePath_ = path;
    activeId_ = id;
    ready_ = true;
    RecordEditorFileStamp(stamp_, path);
    errorKey_.clear();
    if (newlyRegistered)
    {
        *newlyRegistered = registered;
    }
    return true;
}

bool EditorMaterialWorkflow::Save(AssetStore& assets, const std::filesystem::path& targetPath, bool overwrite,
                                  bool* newlyRegistered)
{
    if (newlyRegistered)
    {
        *newlyRegistered = false;
    }
    externalConflict_ = false;
    if (!ready_)
    {
        return false;
    }
    std::error_code pathError;
    const auto requested = targetPath.empty() ? activePath_ : targetPath;
    const auto target = std::filesystem::weakly_canonical(
        requested.is_absolute() ? requested : assets.SourceRoot() / requested, pathError);
    if (pathError || target.empty() || !IsMaterialDocumentPath(target))
    {
        errorKey_ = "Assets.MaterialPathInvalid";
        return false;
    }
    if (!asset_io::Within(assets.SourceRoot(), target))
    {
        errorKey_ = "Assets.ReferenceOutsideRoot";
        return false;
    }
    const bool sameActive = EquivalentPath(target, activePath_);
    if (!overwrite && sameActive && EditorFileStampChanged(stamp_, target))
    {
        externalConflict_ = true;
        errorKey_ = "Editor.ExternalChange";
        return false;
    }
    if (!overwrite && std::filesystem::exists(target) && !sameActive)
    {
        errorKey_ = "Editor.OverwriteConfirmationRequired";
        return false;
    }
    std::vector<AssetDiagnostic> diagnostics;
    if (overwrite && !std::filesystem::exists(std::filesystem::path(target.wstring() + L".asset.json")))
    {
        AssetId imported;
        if (!assets.RegisterSource(target, {}, imported, diagnostics))
        {
            errorKey_ = diagnostics.empty() ? "Assets.InvalidMetadata" : diagnostics.back().key;
            return false;
        }
        if (newlyRegistered)
        {
            *newlyRegistered = true;
        }
    }
    if (session_.Editing())
    {
        session_.CommitEdit();
    }
    AssetId id;
    if (!assets.SaveMaterialSource(target, session_.Document(), id, diagnostics))
    {
        errorKey_ = diagnostics.empty() ? "Assets.InvalidMaterial" : diagnostics.back().key;
        return false;
    }
    if (!session_.MarkSaved())
    {
        errorKey_ = session_.LastError();
        return false;
    }
    activePath_ = target;
    activeId_ = id;
    RecordEditorFileStamp(stamp_, target);
    errorKey_.clear();
    return true;
}

void EditorMaterialWorkflow::RestoreRecovered(AssetStore& assets, const MaterialDocument& document,
                                            const std::filesystem::path& path)
{
    session_.Reset(document, false);
    ready_ = true;
    activePath_ = path;
    activeId_.clear();
    for (const auto& id : assets.ListIds())
    {
        if (EquivalentPath(assets.Resolve(id), activePath_))
        {
            activeId_ = id;
            break;
        }
    }
    RecordEditorFileStamp(stamp_, activePath_);
    externalConflict_ = false;
    errorKey_.clear();
}

bool EditorMaterialWorkflow::SetProperty(std::string_view key, const PropertyValue& value)
{
    if (!ready_)
    {
        return false;
    }
    if (!session_.Editing() && !session_.BeginEdit())
    {
        return false;
    }
    if (!session_.SetProperty(key, value))
    {
        session_.CancelEdit();
        return false;
    }
    return session_.CommitEdit();
}

std::optional<MaterialPreview> EditorMaterialWorkflow::Preview() const
{
    if (!ready_ || activeId_.empty() || !session_.Dirty())
    {
        return {};
    }
    return MaterialPreview{activeId_, session_.Document()};
}

} // namespace flare
