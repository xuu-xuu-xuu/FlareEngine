#pragma once

#include "ExternalChangeMonitor.h"
#include "MaterialEditSession.h"
#include "FunctionLayer/Application/FrameData.h"
#include "ResourceLayer/Assets/AssetStore.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace flare
{

// 拥有材质文档的打开/保存、外部变更时间戳校验与热预览状态。
class EditorMaterialWorkflow
{
public:
    bool Open(AssetStore& assets, const std::filesystem::path& requested, bool discardUnsaved = false,
              bool* newlyRegistered = nullptr);
    bool Save(AssetStore& assets, const std::filesystem::path& targetPath = {}, bool overwrite = false,
              bool* newlyRegistered = nullptr);
    void RestoreRecovered(AssetStore& assets, const MaterialDocument& document, const std::filesystem::path& path);

    bool SetProperty(std::string_view key, const PropertyValue& value);
    bool BeginEdit() { return ready_ && session_.BeginEdit(); }
    bool CommitEdit() { return ready_ && session_.CommitEdit(); }
    bool CancelEdit() { return ready_ && session_.CancelEdit(); }
    bool Undo() { return ready_ && session_.Undo(); }
    bool Redo() { return ready_ && session_.Redo(); }

    [[nodiscard]] bool Ready() const noexcept { return ready_; }
    [[nodiscard]] bool Dirty() const { return ready_ && session_.Dirty(); }
    [[nodiscard]] bool Editing() const noexcept { return session_.Editing(); }
    [[nodiscard]] const MaterialDocument& Document() const noexcept { return session_.Document(); }
    [[nodiscard]] const std::filesystem::path& ActivePath() const noexcept { return activePath_; }
    [[nodiscard]] const AssetId& ActiveAssetId() const noexcept { return activeId_; }
    [[nodiscard]] std::optional<MaterialPreview> Preview() const;
    [[nodiscard]] const EditorFileStamp& Stamp() const noexcept { return stamp_; }
    [[nodiscard]] const std::string& LastError() const noexcept { return errorKey_; }
    [[nodiscard]] bool ExternalConflict() const noexcept { return externalConflict_; }
    void ClearExternalConflict() noexcept { externalConflict_ = false; }

private:
    MaterialEditSession session_;
    std::filesystem::path activePath_;
    AssetId activeId_;
    EditorFileStamp stamp_;
    bool ready_ = false;
    bool externalConflict_ = false;
    std::string errorKey_;
};

} // namespace flare
