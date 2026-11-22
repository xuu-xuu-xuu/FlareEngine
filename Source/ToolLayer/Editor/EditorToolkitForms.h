#pragma once

#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/EditorWorkspaceHost.h"
#include "ToolLayer/Editor/EditorScriptWorkflow.h"
#include "ToolLayer/Editor/EditorClipPreview.h"
#include "ToolLayer/Editor/EditorViewportController.h"
#include "ToolLayer/Editor/EditorFeatureTools.h"
#include "ToolLayer/Editor/ExternalChangeMonitor.h"
#include "ToolLayer/Editor/EditorSession.h"
#include "FunctionLayer/Audio/AudioWorld.h"
#include "FunctionLayer/Rendering/UiPaint.h"
#include "ResourceLayer/Assets/AssetStore.h"

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Localization;

namespace flare
{
class EditorShaderGraphController;

struct ToolkitAssetInspectorState
{
    std::string assetName;
    AssetId modelId;
    std::vector<ui::editor::EditorProperty> rows;
    std::filesystem::path assetPath;
    std::string targetPath;
    bool inspectUi = false;
    bool editTod = false;
    bool isTodAsset = false;
    bool preferScriptUndo = false;
    bool preferMaterialUndo = false;
    bool lockedToEntity = false;
    StableEntityId lockedEntityId = 0;
    std::vector<StableEntityId> lockedSelection;
    bool lockedToAsset = false;
    std::string lockedAssetPath;
    std::unordered_set<StableEntityId> playKeepEntities;
    std::filesystem::path explicitUiPath;
    std::shared_ptr<const ImageData> previewImage;
    std::string previewImageKey;
    std::uint64_t previewToken = 0;
    VoiceHandle previewAudioVoice;
    std::unordered_map<std::string, ui::UiImageInfo> imageInfos;
};

struct ToolkitImportResult
{
    bool success = false;
    std::string key;
    std::string detail;
    std::filesystem::path primary;
};

struct ToolkitWorkspaceContext
{
    const std::filesystem::path& root;
    const std::filesystem::path& assetsRoot;
    AssetStore& assets;
    EditorSession& session;
    EditorWorkspaceHost& host;
    UiTree& ui;
    EditorScriptWorkflow& scriptWorkflow;
    EditorClipPreview& clipPreview;
    EditorViewportController& viewportController;
    ExternalChangeMonitor& externalChange;
    const Localization& text;
    ui::editor::EditorShell& shell;
    EditorFeatureTools* featureTools = nullptr;
    ToolkitAssetInspectorState& assetState;
    std::string& exportName;
    std::string& importStatus;
    std::future<ToolkitImportResult>& importTask;
    EditorApplicationHooks* hooks = nullptr;
    AudioWorld* audio = nullptr;
    EditorShaderGraphController* shaderGraph = nullptr;
};

// 将引擎诊断键（如 Editor.ExternalChange）解析为当前语言文案，未命中键表时使用本地化兜底键。
[[nodiscard]] std::string LocalizedToolkitStatus(const Localization& text,
                                                 std::string_view keyOrMessage,
                                                 std::string_view fallbackKey = "EditorUi.SaveFailed");

[[nodiscard]] std::vector<ui::editor::EditorProperty> BuildMaterialFormRows(const EditorMaterialWorkflow& material,
                                                                            const Localization& text,
                                                                            const AssetStore* assets = nullptr);

[[nodiscard]] bool ParseMaterialFormValue(const PropertyDescriptor& descriptor,
                                          std::string_view text,
                                          PropertyValue& output);

[[nodiscard]] std::vector<ui::editor::EditorProperty> BuildProjectSettingsRows(const ProjectSettings& settings,
                                                                               const Localization& text,
                                                                               const std::filesystem::path& projectRoot = {});

[[nodiscard]] std::vector<ui::editor::EditorProperty> BuildBuildSettingsRows(const BuildSettings& settings,
                                                                             const std::string& exportName,
                                                                             const Localization& text);

[[nodiscard]] std::vector<ui::editor::EditorProperty> BuildPreferencesRows(const EditorPreferences& preferences,
                                                                           const Localization& text);

void BindToolkitWorkspaceForms(ToolkitWorkspaceContext& context,
                               const std::function<bool()>& exportAction);

bool SaveAllToolkitDocuments(ToolkitWorkspaceContext& context);

} // namespace flare
