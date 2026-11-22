#pragma once

#include "CoreLayer/Localization.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "ResourceLayer/Assets/ShaderGraph.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "ToolLayer/Editor/EditorUi/EditorShell.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flare
{
// 创建引用指定着色器（.fshader.hlsl 或 .fshader.json）的配套材质，并在选中网格实体时自动赋材质。
[[nodiscard]] bool CreateCompanionShaderMaterial(const std::filesystem::path& projectRoot,
                                                 AssetStore& assets,
                                                 EditorSession& session,
                                                 const std::filesystem::path& shaderPath,
                                                 std::filesystem::path& outMaterialPath,
                                                 std::string& outError);

class EditorShaderGraphController
{
public:
    EditorShaderGraphController(const std::filesystem::path& projectRoot,
                                const std::filesystem::path& cacheRoot,
                                AssetStore& assets,
                                EditorSession& session,
                                const Localization& text);

    void Attach(ui::editor::EditorShell& shell);

    bool Open(const std::filesystem::path& graphPath, bool focusPanel = true);
    void Close();
    void NotifyAssetMoved(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
    void NotifyAssetDeleted(const std::filesystem::path& deletedPath);
    bool ReloadIfModifiedOnDisk();
    bool Save(bool reloadGpu = true);

    [[nodiscard]] bool IsOpen() const noexcept { return loaded_; }
    [[nodiscard]] bool Dirty() const noexcept { return dirty_; }
    [[nodiscard]] const std::filesystem::path& ActivePath() const noexcept { return path_; }
    [[nodiscard]] const ShaderGraphDocument& Document() const noexcept { return graph_; }

    bool Undo();
    bool Redo();
    bool DeleteSelected();
    bool DuplicateSelected();

    void TickPreview(double timeSeconds);
    [[nodiscard]] const ShaderPreviewImage* Preview() const noexcept
    {
        return preview_.width > 0 && !preview_.rgba8.empty() ? &preview_ : nullptr;
    }
    [[nodiscard]] std::uint64_t PreviewToken() const noexcept { return previewToken_; }
    [[nodiscard]] std::uint64_t PreviewRevision() const noexcept { return previewRevision_; }

    void SetReloadCallback(std::function<void()> callback) { onShaderReloaded_ = std::move(callback); }
    void SetOpenHlslCallback(std::function<void(const std::filesystem::path&)> callback) { onOpenHlsl_ = std::move(callback); }
    void SetMaterialCreatedCallback(std::function<void(const std::filesystem::path&)> callback)
    {
        onMaterialCreated_ = std::move(callback);
    }

private:
    void HandleAction(std::string_view action, std::string_view payload);
    void PushUndoSnapshot();
    void CommitLiveEdit();
    void SyncShellView();
    void RebuildPreview(float timeSeconds);
    [[nodiscard]] bool GraphUsesTime() const noexcept;
    [[nodiscard]] ShaderGraphNode* FindNode(std::uint32_t id) noexcept;
    [[nodiscard]] const ShaderGraphNode* FindNode(std::uint32_t id) const noexcept;

    const std::filesystem::path& projectRoot_;
    std::filesystem::path cacheRoot_;
    AssetStore& assets_;
    EditorSession& session_;
    const Localization& text_;
    ui::editor::EditorShell* shell_ = nullptr;

    std::filesystem::path path_;
    std::filesystem::file_time_type lastDiskMtime_{};
    ShaderGraphDocument graph_;
    bool loaded_ = false;
    bool dirty_ = false;

    std::vector<std::uint32_t> selectedNodes_;
    std::optional<std::size_t> selectedLink_;

    std::vector<ShaderGraphDocument> undoStack_;
    std::vector<ShaderGraphDocument> redoStack_;
    std::optional<ShaderGraphDocument> preMoveDoc_;
    std::vector<std::pair<std::uint32_t, Vec2>> moveBasePositions_;

    float lightYaw_ = -0.55f;
    float lightPitch_ = 0.52f;
    bool previewDirty_ = true;
    double lastPreviewTickSeconds_ = -1.0;
    ShaderPreviewImage preview_;
    std::uint64_t previewToken_ = 0x17F00001ull;
    std::uint64_t previewRevision_ = 1;
    std::string statusText_;

    std::function<void()> onShaderReloaded_;
    std::function<void(const std::filesystem::path&)> onOpenHlsl_;
    std::function<void(const std::filesystem::path&)> onMaterialCreated_;
};
} // namespace flare
