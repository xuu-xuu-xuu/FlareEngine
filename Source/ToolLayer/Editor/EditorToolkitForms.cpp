#include "EditorToolkitForms.h"
#include "EditorAssetActions.h"
#include "EditorCreation.h"
#include "EditorMaterialWorkflow.h"
#include "EditorScriptTools.h"
#include "EditorSessionStore.h"
#include "MaterialThumbnail.h"
#include "ProjectSettingsService.h"
#include "PrefabWorkflow.h"
#include "SceneInspector.h"
#include "TodInspector.h"
#include "UiInspector.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/AudioClip.h"
#include "ResourceLayer/Assets/MaterialMetadata.h"
#include "ResourceLayer/Scenes/PrefabDocument.h"

#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <sstream>

namespace se
{
using ui::editor::EditorPanelId;
using ui::editor::EditorProperty;
using ui::editor::EditorPropertyType;

namespace
{
std::shared_ptr<const AudioClip> ResolveSelectedAudioClip(
    ToolkitWorkspaceContext& ctx, const std::filesystem::path& path, const AssetId& id)
{
    std::vector<AssetDiagnostic> diagnostics;
    if (!id.empty())
    {
        if (auto loaded = ctx.assets.LoadAudioClip(id, diagnostics)) return loaded;
    }
    if (!path.empty() && IsAudioExtension(path))
    {
        auto direct = std::make_shared<AudioClip>();
        diagnostics.clear();
        if (DecodeAudioSource(path, *direct, diagnostics) && direct->Valid()) return direct;
    }
    return nullptr;
}

std::string PropertyText(const PropertyValue& value)
{
    if (const auto* v = std::get_if<bool>(&value)) return *v ? "true" : "false";
    if (const auto* v = std::get_if<float>(&value)) return std::to_string(*v);
    if (const auto* v = std::get_if<Vec3>(&value)) return std::to_string(v->x) + ", " + std::to_string(v->y) + ", " + std::to_string(v->z);
    if (const auto* v = std::get_if<Vec4>(&value)) return std::to_string(v->x) + ", " + std::to_string(v->y) + ", " + std::to_string(v->z) + ", " + std::to_string(v->w);
    if (const auto* v = std::get_if<Quat>(&value)) return std::to_string(v->x) + ", " + std::to_string(v->y) + ", " + std::to_string(v->z) + ", " + std::to_string(v->w);
    if (const auto* v = std::get_if<std::string>(&value)) return *v;
    if (const auto* v = std::get_if<PropertyEntityReference>(&value)) return std::to_string(v->stableId);
    if (const auto* v = std::get_if<std::vector<std::string>>(&value))
    {
        std::string result;
        for (std::size_t index = 0; index < v->size(); ++index)
        {
            if (index) result += ", ";
            result += (*v)[index];
        }
        return result;
    }
    return {};
}

bool ParseFloatList(std::string_view text, float* values, std::size_t count)
{
    std::string normalized(text);
    for (char& ch : normalized)
    {
        if (ch == ',') ch = ' ';
    }
    std::istringstream stream(normalized);
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!(stream >> values[index])) return false;
    }
    return true;
}

bool ApplyProjectSettingsField(EditorWorkspaceHost& host, std::string_view key, const std::string& value)
{
    ProjectSettings updated = host.Project();
    if (key == "name") updated.name = value;
    else if (key == "contentRoot") updated.contentRoot = value;
    else if (key == "entryScene") updated.entryScene = value;
    else if (key == "showPlayerQualityMenu" && (value == "true" || value == "false"))
        updated.showPlayerQualityMenu = (value == "true");
    else if (key == "playerSettingsId") updated.playerSettingsId = value;
    else if (key.starts_with("quality."))
    {
        bool found = false;
        for (size_t i = 0; i < updated.qualityProfiles.size(); ++i)
        {
            const std::string prefix = std::string("quality.") + PlayerQualityIds[i + 1] + ".";
            if (!key.starts_with(prefix)) continue;
            auto& p = updated.qualityProfiles[i];
            const auto field = key.substr(prefix.size());
            if (field == "sampling")
            {
                auto method = ParseQualitySampling(value);
                if (!method) return false;
                p.sampling = *method;
            }
            else if ((field == "shadows" || field == "bloom") && (value == "true" || value == "false"))
            {
                (field == "shadows" ? p.allowShadows : p.allowBloom) = (value == "true");
            }
            else
            {
                return false;
            }
            found = true;
            break;
        }
        if (!found) return false;
    }
    else
    {
        return false;
    }
    return host.SetProjectSettings(updated);
}

bool ApplyBuildSettingsField(ToolkitWorkspaceContext& ctx, std::string_view key, const std::string& value)
{
    if (key == "openOutput")
    {
        const auto output = ctx.host.ExportSnapshot().outputPackage;
        if (output.empty() || !std::filesystem::is_directory(output))
        {
            ctx.shell.SetFormStatus(EditorPanelId::BuildExport, ctx.text.GetText("EditorUi.ExportFirst"));
            return false;
        }
        if (std::filesystem::is_regular_file(output / "clip.mp4")) ctx.host.OpenInExplorer(output / "clip.mp4", true);
        else ctx.host.OpenInExplorer(output);
        return true;
    }
    if (key == "exportName")
    {
        if (value.empty() || value.size() > 128) return false;
        ctx.exportName = value;
        return true;
    }
    BuildSettings updated = ctx.host.Build();
    if (key == "recordVideo") updated.recordVideo = (value == "true");
    else if (key == "videoResolution") updated.videoResolution = value;
    else if (key == "videoFps" || key == "videoSeconds")
    {
        int n = 0;
        auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (status != std::errc{} || end != value.data() + value.size() || n < 1) return false;
        if (key == "videoFps")
        {
            if ((n != 24 && n != 30 && n != 60) || updated.videoSeconds > 100000 / n) return false;
            updated.videoFps = n;
        }
        else
        {
            if (n > 100000 / updated.videoFps) return false;
            updated.videoSeconds = n;
        }
    }
    else if (key == "scriptsProject") updated.scriptsProject = value;
    else if (key == "scriptType") updated.scriptType = value;
    else if (key == "bindings") updated.bindings = value;
    else if (key == "entryUi") updated.entryUi = value;
    else return false;
    return ctx.host.SetBuildSettings(updated);
}

bool ApplyPreferencesField(EditorWorkspaceHost& host, std::string_view key, const std::string& value)
{
    auto& preferences = host.Preferences();
    if (key == "language") preferences.language = value;
    else if (key == "uiScale")
    {
        try
        {
            const float scale = std::stof(value);
            if (!std::isfinite(scale) || scale < 0.5f || scale > 2.0f) return false;
            preferences.uiScale = scale;
        }
        catch (...)
        {
            return false;
        }
    }
    else if (key == "externalEditor") preferences.externalEditor = value;
    else if (key == "colorLut") preferences.colorLut = value;
    else if (key == "restoreLastProject") preferences.restoreLastProject = (value == "true");
    else return false;
    return true;
}

void HandleAssetSelection(ToolkitWorkspaceContext& ctx, const std::string& path)
{
    auto& state = ctx.assetState;
    Log(LogLevel::Info, "EditorUi.ToolkitAsset: " + path);
    if (ctx.audio && state.previewAudioVoice)
    {
        ctx.audio->Stop(state.previewAudioVoice);
        state.previewAudioVoice = {};
    }
    if (state.inspectUi) ctx.shell.CancelPropertyInteraction();
    state.inspectUi = false;
    if (ctx.featureTools) ctx.featureTools->SelectSource(path);

    const std::filesystem::path selected = asset_io::FromUtf8(path);
    state.assetPath = selected;
    state.targetPath = asset_io::Utf8(selected.lexically_relative(ctx.assets.SourceRoot()));
    state.assetName = asset_io::Utf8(selected.filename());
    state.rows.clear();
    state.modelId.clear();
    ctx.clipPreview.Clear();

    const auto addDetail = [&](const char* key, const char* label, std::string value) {
        state.rows.push_back({key, ctx.text.GetText(label), EditorPropertyType::String, std::move(value), true});
    };
    std::error_code dirEc;
    if (std::filesystem::is_directory(selected, dirEc) && !dirEc)
    {
        addDetail("asset.name", "EditorUi.Name", state.assetName);
        addDetail("asset.type", "Editor.AssetType", ctx.text.GetText("EditorUi.AssetFolder"));
        addDetail("asset.path", "EditorUi.SourcePath", asset_io::Utf8(selected.lexically_relative(ctx.root)));
        state.rows.push_back({"asset.newSubfolder", ctx.text.GetText("EditorUi.NewFolder"),
                              EditorPropertyType::Action, {}, ctx.session.IsPlaying()});
        state.rows.push_back({"asset.showExplorer", ctx.text.GetText("EditorUi.ShowInExplorer"),
                              EditorPropertyType::Action, {}, false});
        ctx.shell.SetAssetsPreviewText(state.assetName);
        ctx.shell.SetAssetPreviewImage({}, 0.0f, 0.0f);
        state.imageInfos.clear();
        state.previewImage.reset();
        state.previewImageKey.clear();
        state.previewToken = 0;
        return;
    }
    auto extension = asset_io::Utf8(selected.extension());
    for (char& ch : extension)
    {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    const bool texture = extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                         extension == ".bmp" || extension == ".tga";
    const bool model = extension == ".fbx" || extension == ".glb" || extension == ".gltf";
    const bool audio = extension == ".wav" || extension == ".ogg";
    const bool prefab = IsPrefabDocumentPath(selected);
    std::string type = extension;
    if (model) type = ctx.text.GetText("EditorUi.AssetModel");
    else if (prefab) type = ctx.text.GetText("EditorUi.AssetPrefab");
    else if (texture) type = ctx.text.GetText("EditorUi.AssetTexture");
    else if (audio) type = ctx.text.GetText("EditorUi.AssetAudio");
    else if (extension == ".cs") type = ctx.text.GetText("EditorUi.AssetScript");
    else if (IsMaterialDocumentPath(selected)) type = ctx.text.GetText("EditorUi.DocumentMaterial");

    addDetail("asset.name", "EditorUi.Name", state.assetName);
    addDetail("asset.type", "Editor.AssetType", type);
    addDetail("asset.path", "EditorUi.SourcePath", asset_io::Utf8(selected.lexically_relative(ctx.root)));
    std::error_code sizeError;
    const auto bytes = std::filesystem::file_size(selected, sizeError);
    if (!sizeError) addDetail("asset.size", "EditorUi.Size", std::to_string(bytes) + " B");
    if (model)
    {
        for (const auto& id : ctx.assets.ListIds())
        {
            if (ctx.assets.Resolve(id) == selected) { state.modelId = id; break; }
        }
        if (!state.modelId.empty())
        {
            state.rows.push_back({"asset.addModel", ctx.text.GetText("EditorUi.AddModelToScene"),
                                  EditorPropertyType::Action, {}, ctx.session.IsPlaying()});
        }
    }
    if (prefab)
    {
        for (const auto& id : ctx.assets.ListIds())
        {
            if (ctx.assets.Resolve(id) == selected) { state.modelId = id; break; }
        }
        if (!state.modelId.empty())
        {
            PrefabDocument doc;
            std::string prefabError;
            if (ReadPrefabDocument(selected, doc, prefabError))
                addDetail("asset.nodes", "EditorUi.PrefabNodes", std::to_string(doc.nodes.size()));
            state.rows.push_back({"asset.addPrefab", ctx.text.GetText("EditorUi.InstantiatePrefab"),
                                  EditorPropertyType::Action, {}, ctx.session.IsPlaying()});
        }
    }
    if (audio)
    {
        for (const auto& id : ctx.assets.ListIds())
        {
            if (ctx.assets.Resolve(id) == selected) { state.modelId = id; break; }
        }
        if (const auto clip = ResolveSelectedAudioClip(ctx, selected, state.modelId))
        {
            const std::string channelLabel = clip->channels == 1
                ? ctx.text.GetText("EditorUi.AudioMono")
                : ctx.text.GetText("EditorUi.AudioStereo");
            addDetail("asset.audioFormat", "EditorUi.AudioFormat",
                      std::to_string(clip->sampleRate) + " Hz | " + channelLabel);
            std::ostringstream duration;
            duration.setf(std::ios::fixed);
            duration.precision(2);
            duration << clip->DurationSeconds() << " s";
            addDetail("asset.audioDuration", "EditorUi.AudioDuration", duration.str());
        }
        state.rows.push_back({"asset.previewAudio", ctx.text.GetText("EditorUi.PreviewAudio"),
                              EditorPropertyType::Action, {}, ctx.session.IsPlaying()});
        state.rows.push_back({"asset.stopAudio", ctx.text.GetText("EditorUi.StopAudio"),
                              EditorPropertyType::Action, {}, ctx.session.IsPlaying()});
    }
    if (extension == ".cs")
    {
        state.rows.push_back({"asset.scriptClass", ctx.text.GetText("Editor.ScriptClassName"),
                              EditorPropertyType::String, ctx.scriptWorkflow.ClassNameForSource(selected),
                              ctx.session.IsPlaying()});
        state.rows.push_back({"asset.openScript", ctx.text.GetText("Editor.ScriptOpenSource"),
                              EditorPropertyType::Action, {}, false});
        state.rows.push_back({"asset.attachScript", ctx.text.GetText("EditorUi.AttachScriptToSelected"),
                              EditorPropertyType::Action, {}, ctx.session.IsPlaying()});
        state.rows.push_back({"asset.compileScripts", ctx.text.GetText("EditorUi.CompileScripts"),
                              EditorPropertyType::Action, {}, ctx.session.IsPlaying()});
        state.rows.push_back({"asset.deleteScript", ctx.text.GetText("Editor.AssetDelete"),
                              EditorPropertyType::Action, {}, ctx.session.IsPlaying()});
    }
    if (IsMaterialDocumentPath(selected))
    {
        state.rows.push_back({"asset.openMaterial", ctx.text.GetText("EditorUi.OpenMaterial"),
                              EditorPropertyType::Action, {}, false});
    }
    if (asset_io::Within(ctx.assets.SourceRoot(), selected))
    {
        state.rows.push_back({"asset.targetPath", ctx.text.GetText("Editor.AssetTarget"),
                              EditorPropertyType::String, state.targetPath, ctx.session.IsPlaying()});
        state.rows.push_back({"asset.move", ctx.text.GetText("Editor.AssetMove"),
                              EditorPropertyType::Action, {}, ctx.session.IsPlaying()});
        state.rows.push_back({"asset.delete", ctx.text.GetText("Editor.AssetDelete"),
                              EditorPropertyType::Action, {}, ctx.session.IsPlaying()});
    }
    state.rows.push_back({"asset.showExplorer", ctx.text.GetText("EditorUi.ShowInExplorer"),
                          EditorPropertyType::Action, {}, false});

    ctx.scriptWorkflow.SelectSource(selected);
    ctx.shell.SetAssetsPreviewText(state.assetName);
    ctx.shell.SetAssetPreviewImage({}, 0.0f, 0.0f);
    state.imageInfos.clear();
    state.previewImage.reset();
    state.previewImageKey.clear();
    state.previewToken = 0;

    if (!IsMaterialDocumentPath(selected))
    {
        if (texture)
        {
            for (const AssetId& id : ctx.assets.ListIds())
            {
                std::error_code error;
                if (!std::filesystem::equivalent(ctx.assets.Resolve(id), selected, error) || error) continue;
                std::vector<AssetDiagnostic> imageDiagnostics;
                auto image = ctx.assets.LoadTextureImage(id, imageDiagnostics);
                if (!image || image->width == 0 || image->height == 0) break;
                addDetail("asset.dimensions", "EditorUi.ImageDimensions",
                          std::to_string(image->width) + " × " + std::to_string(image->height));
                const std::size_t hash = std::hash<std::string>{}(path);
                state.previewToken = 0x100000000ull | (static_cast<std::uint64_t>(hash) & 0xffffffffull);
                state.previewImage = std::move(image);
                state.previewImageKey = path;
                state.imageInfos[path] = {state.previewToken, static_cast<float>(state.previewImage->width),
                                          static_cast<float>(state.previewImage->height)};
                ctx.shell.SetAssetPreviewImage(path, static_cast<float>(state.previewImage->width),
                                               static_cast<float>(state.previewImage->height));
                Log(LogLevel::Info, "EditorUi.ToolkitAssetPreview: " + path);
                break;
            }
        }
        return;
    }

    std::string materialError;
    if (ctx.host.OpenMaterial(ctx.assets, selected, false))
    {
        auto matRows = BuildMaterialFormRows(ctx.host.Material(), ctx.text);
        ctx.shell.SetFormProperties(EditorPanelId::Material, matRows);
        const std::string matSection = ctx.text.GetText("EditorUi.DocumentMaterial");
        for (auto& mr : matRows)
        {
            if (mr.key == "name") continue;
            EditorProperty inlineRow = mr;
            inlineRow.key = "asset.mat." + mr.key;
            inlineRow.section = matSection;
            inlineRow.readOnly = inlineRow.readOnly || ctx.session.IsPlaying();
            if (inlineRow.type == EditorPropertyType::Number)
            {
                inlineRow.dragStep = 0.01f;
                inlineRow.dragMinimum = 0.0f;
                inlineRow.dragMaximum = (mr.key == "normalScale") ? 2.0f : 1.0f;
                inlineRow.slider = true;
            }
            else if (inlineRow.type == EditorPropertyType::Asset)
            {
                if (inlineRow.value.empty()) inlineRow.displayValue = ctx.text.GetText("EditorUi.None");
                else
                {
                    const auto resolved = ctx.assets.Resolve(inlineRow.value);
                    inlineRow.displayValue = resolved.empty() ? inlineRow.value : asset_io::Utf8(resolved.filename());
                }
            }
            state.rows.push_back(std::move(inlineRow));
        }
        EditorProperty assignRow{"asset.mat.assign", ctx.text.GetText("EditorUi.AssignMaterial"),
                                 EditorPropertyType::Action, {}, ctx.session.IsPlaying()};
        assignRow.section = matSection;
        state.rows.push_back(std::move(assignRow));
        ctx.shell.SetFormStatus(EditorPanelId::Material,
                                ctx.text.GetText("EditorUi.OpenedFile") + ": " + path);
        if (ctx.hooks != nullptr || ctx.shell.Dock().Find(EditorPanelId::Material) != nullptr)
        {
            ctx.shell.FocusPanel(EditorPanelId::Material);
        }
        std::string summary = ctx.text.GetText("EditorUi.MaterialPreview") + ": " + path;
        for (const char* key : {"baseColor", "metallic", "roughness"})
        {
            PropertyValue value;
            if (ReadMaterialProperty(ctx.host.Material().Document(), key, value, materialError))
                summary += std::string(" | ") + key + "=" + PropertyText(value);
        }
        ctx.shell.SetAssetsPreviewText(std::move(summary));
        Vec4 baseColor{0.8f, 0.8f, 0.8f, 1.0f};
        float metallic = 0.0f, roughness = 0.5f;
        PropertyValue value;
        if (ReadMaterialProperty(ctx.host.Material().Document(), "baseColor", value, materialError))
            if (const auto* color = std::get_if<Vec4>(&value)) baseColor = *color;
        if (ReadMaterialProperty(ctx.host.Material().Document(), "metallic", value, materialError))
            if (const auto* number = std::get_if<float>(&value)) metallic = *number;
        if (ReadMaterialProperty(ctx.host.Material().Document(), "roughness", value, materialError))
            if (const auto* number = std::get_if<float>(&value)) roughness = *number;
        const MaterialThumbnail thumbnail = RenderMaterialThumbnail(baseColor, metallic, roughness, 64);
        auto image = std::make_shared<ImageData>();
        image->width = thumbnail.width;
        image->height = thumbnail.height;
        image->rgba8 = thumbnail.rgba8;
        const std::string imageKey = "material:" + path;
        const std::size_t hash = std::hash<std::string>{}(imageKey);
        state.previewToken = 0x100000000ull | (static_cast<std::uint64_t>(hash) & 0xffffffffull);
        state.previewImage = std::move(image);
        state.previewImageKey = imageKey;
        state.imageInfos[imageKey] = {state.previewToken, static_cast<float>(state.previewImage->width),
                                      static_cast<float>(state.previewImage->height)};
        ctx.shell.SetAssetPreviewImage(imageKey, static_cast<float>(state.previewImage->width),
                                       static_cast<float>(state.previewImage->height));
        Log(LogLevel::Info, "EditorUi.ToolkitMaterial: opened " + path);
    }
    else
    {
        materialError = ctx.host.LastError();
        ctx.shell.SetFormStatus(EditorPanelId::Material,
                                LocalizedToolkitStatus(ctx.text, materialError, "EditorUi.OpenMaterialFailed"));
        ctx.shell.FocusPanel(EditorPanelId::Material);
        Log(LogLevel::Warning, "EditorUi.ToolkitMaterial: failed " + materialError);
    }
}

bool HandleAssetPropertyAction(ToolkitWorkspaceContext& ctx, const std::string& key, const std::string& value)
{
    auto& state = ctx.assetState;
    if (key == "asset.targetPath")
    {
        state.targetPath = value;
        for (auto& row : state.rows)
        {
            if (row.key == "asset.targetPath") row.value = state.targetPath;
        }
        return true;
    }
    if (key == "asset.move" || key == "asset.delete")
    {
        if (ctx.session.IsPlaying() || ctx.host.ExportBusy())
        {
            ctx.shell.SetStatus(ctx.text.GetText("Editor.AssetOperationBusy"));
            return false;
        }
        if (state.assetPath.empty()) return false;
        std::vector<EditorAssetDocument> editing;
        if (!ctx.host.CaptureEditingDocuments(ctx.session, ctx.assets, &ctx.ui, editing))
        {
            ctx.shell.SetStatus(LocalizedToolkitStatus(ctx.text, ctx.host.LastError(), "Editor.AssetImportFailed"));
            return false;
        }
        const bool isMove = (key == "asset.move");
        const auto plan = EditorAssetActions::Plan({
            isMove ? EditorAssetAction::Move : EditorAssetAction::Delete,
            ctx.root, ctx.assets.SourceRoot(), state.assetPath,
            isMove ? ctx.assets.SourceRoot() / asset_io::FromUtf8(state.targetPath) : std::filesystem::path{},
            std::move(editing)}, ctx.assets);
        const char* titleKey = isMove ? "Editor.AssetMove" : "Editor.AssetDelete";
        std::string detail;
        if (plan.valid)
        {
            detail = ctx.text.GetText(isMove ? "Editor.AssetMoveConfirm" : "Editor.AssetDeleteConfirm");
        }
        else
        {
            const std::string errKey = plan.diagnostics.empty()
                ? std::string{"Editor.AssetImportFailed"} : plan.diagnostics.front().key;
            detail = ctx.text.GetText(errKey);
            if (!plan.diagnostics.empty() && !plan.diagnostics.front().detail.empty())
            {
                detail += ": " + plan.diagnostics.front().detail;
            }
        }
        ctx.shell.ShowConfirm(
            ctx.text.GetText(titleKey), detail, ctx.text.GetText("Editor.AssetConfirm"), {},
            ctx.text.GetText("EditorUi.Cancel"),
            [plan, &ctx]() {
                std::vector<EditorAssetDocument> currentEditing;
                ctx.host.CaptureEditingDocuments(ctx.session, ctx.assets, &ctx.ui, currentEditing);
                std::vector<EditorAssetIssue> issues;
                if (!EditorAssetActions::Execute(plan, ctx.assets, currentEditing, issues))
                {
                    const std::string errKey = issues.empty()
                        ? std::string{"Editor.AssetImportFailed"} : issues.front().key;
                    ctx.importStatus = ctx.text.GetText(errKey);
                    ctx.shell.SetStatus(ctx.importStatus);
                    return;
                }
                std::vector<AssetDiagnostic> scanDiag;
                ctx.assets.Scan(scanDiag);
                std::vector<EditorAssetIssue> syncIssues;
                EditorAssetActions::SyncProjectAssets(ctx.root, ctx.assets, syncIssues);
                ctx.assetState.assetName.clear();
                ctx.assetState.rows.clear();
                ctx.assetState.assetPath.clear();
                ctx.importStatus = ctx.text.GetText("Editor.AssetActionSucceeded");
                ctx.shell.SetStatus(ctx.importStatus);
                ctx.shell.AppendConsoleLine(ctx.importStatus);
            },
            {}, plan.valid);
        return true;
    }
    if (key == "asset.addModel")
    {
        std::string error;
        if (!AddModelToScene(ctx.session, ctx.assets, state.modelId, error))
        {
            ctx.shell.SetStatus(LocalizedToolkitStatus(ctx.text, error, "Editor.AssetImportFailed"));
            return false;
        }
        state.assetName.clear();
        ctx.viewportController.Action("Frame", ctx.session, ctx.assets);
        return true;
    }
    if (key == "asset.addPrefab")
    {
        std::string error;
        StableEntityId rootId = 0;
        PrefabDocument doc;
        Transform rootTransform{};
        if (ReadPrefabDocument(ctx.assets.Resolve(state.modelId), doc, error))
            rootTransform.scale = PrefabRootAuthoredScale(doc);
        if (!InstantiatePrefabIntoScene(ctx.session, ctx.assets, state.modelId, rootTransform,
                                        rootId, error, ctx.hooks))
        {
            ctx.shell.SetStatus(LocalizedToolkitStatus(ctx.text, error, "EditorUi.DropFailed"));
            return false;
        }
        state.assetName.clear();
        ctx.viewportController.Action("Frame", ctx.session, ctx.assets);
        return true;
    }
    if (key == "asset.previewAudio")
    {
        if (!ctx.audio || ctx.session.IsPlaying()) return false;
        if (state.previewAudioVoice)
        {
            ctx.audio->Stop(state.previewAudioVoice);
            state.previewAudioVoice = {};
        }
        const auto clip = ResolveSelectedAudioClip(ctx, state.assetPath, state.modelId);
        if (!clip)
        {
            ctx.shell.SetStatus(ctx.text.GetText("Assets.AudioDecodeFailed"));
            return false;
        }
        auto clipData = std::make_shared<AudioClipData>();
        clipData->sampleRate = clip->sampleRate;
        clipData->channels = clip->channels;
        clipData->samples = clip->samples;
        AudioPlayDesc desc;
        desc.clip = std::move(clipData);
        desc.volume = 1.0f;
        desc.pitch = 1.0f;
        desc.spatialBlend = 0.0f;
        desc.loop = false;
        desc.priority = 10;
        state.previewAudioVoice = ctx.audio->Play(desc);
        return static_cast<bool>(state.previewAudioVoice);
    }
    if (key == "asset.stopAudio")
    {
        if (ctx.audio && state.previewAudioVoice)
        {
            ctx.audio->Stop(state.previewAudioVoice);
            state.previewAudioVoice = {};
        }
        return true;
    }
    if (key == "asset.openScript")
    {
        CodeEditorSettings settings;
        settings.executable = asset_io::FromUtf8(ctx.host.Preferences().externalEditor);
        const bool opened = ctx.scriptWorkflow.OpenSelected(settings);
        if (!opened) ctx.shell.SetStatus(ctx.scriptWorkflow.Status());
        return opened;
    }
    if (key == "asset.scriptClass")
    {
        std::filesystem::path renamed;
        if (!ctx.scriptWorkflow.RenameScriptSource(state.assetPath, value, renamed))
        {
            ctx.shell.SetStatus(ctx.scriptWorkflow.Status());
            return false;
        }
        HandleAssetSelection(ctx, asset_io::Utf8(renamed));
        ctx.shell.SetStatus(ctx.text.GetText("EditorUi.ScriptRenamed") + ": " + value);
        return true;
    }
    if (key == "asset.attachScript")
    {
        const StableEntityId target = ctx.session.Selected();
        if (target == 0)
        {
            ctx.shell.SetStatus(ctx.text.GetText("EditorUi.SelectObjectFirst"));
            return false;
        }
        const std::string scriptId = ctx.scriptWorkflow.ScriptIdForSource(state.assetPath);
        const std::string display = ctx.scriptWorkflow.ClassNameForSource(state.assetPath);
        bool needsCompile = false;
        if (ctx.scriptWorkflow.AttachScript(target, scriptId, needsCompile))
        {
            state.assetName.clear();
            const std::string msg = ctx.text.GetText("EditorUi.ScriptAttached") + ": " + display;
            ctx.shell.SetStatus(msg);
            ctx.shell.AppendConsoleLine(msg);
            return true;
        }
        if (needsCompile && !scriptId.empty())
        {
            state.assetName.clear();
            ctx.shell.Commands().Execute({ui::editor::EditorCommandId::Build, std::to_string(target) + "|" + scriptId});
            return true;
        }
        if (!ctx.scriptWorkflow.Status().empty()) ctx.shell.SetStatus(ctx.scriptWorkflow.Status());
        return false;
    }
    if (key == "asset.compileScripts")
    {
        return ctx.shell.Commands().Execute({ui::editor::EditorCommandId::Build, {}});
    }
    if (key == "asset.deleteScript")
    {
        return ctx.shell.Commands().Execute({ui::editor::EditorCommandId::Delete, {}});
    }
    if (key == "asset.mat.assign")
    {
        const StableEntityId selected = ctx.session.Selected();
        if (!ctx.host.Material().Ready() || selected == 0 || ctx.session.IsPlaying()) return false;
        const EntityHandle entity = ctx.session.EditWorld().FindByStableId(selected);
        const auto* mesh = ctx.session.EditWorld().GetMesh(entity);
        if (!mesh) return false;
        MeshComponent updated = *mesh;
        if (updated.materials.empty()) updated.materials.push_back(ctx.host.Material().ActiveAssetId());
        else updated.materials[0] = ctx.host.Material().ActiveAssetId();
        if (!ctx.session.SetMesh(selected, updated)) return false;
        ctx.shell.SetStatus(ctx.text.GetText("EditorUi.Saved"));
        return true;
    }
    if (key.starts_with("asset.mat."))
    {
        if (ctx.session.IsPlaying() || !ctx.host.Material().Ready()) return false;
        const std::string matKey = key.substr(10);
        const PropertyDescriptor* descriptor = FindMaterialProperty(matKey);
        if (!descriptor) return false;
        PropertyValue parsed;
        if (descriptor->type == PropertyType::Vector4)
        {
            PropertyValue existingVal;
            std::string ignored;
            float curAlpha = 1.0f;
            if (ReadMaterialProperty(ctx.host.Material().Document(), matKey, existingVal, ignored))
                if (const auto* v4 = std::get_if<Vec4>(&existingVal)) curAlpha = v4->w;
            float vals[4]{0.0f, 0.0f, 0.0f, curAlpha};
            if (ParseFloatList(value, vals, 4)) parsed = Vec4{vals[0], vals[1], vals[2], vals[3]};
            else if (ParseFloatList(value, vals, 3)) parsed = Vec4{vals[0], vals[1], vals[2], curAlpha};
            else return false;
            if (!ValidatePropertyValue(*descriptor, parsed)) return false;
        }
        else if (!ParseMaterialFormValue(*descriptor, value, parsed))
        {
            return false;
        }
        if (!ctx.host.Material().SetProperty(matKey, parsed)) return false;
        ctx.shell.SetFormProperties(EditorPanelId::Material, BuildMaterialFormRows(ctx.host.Material(), ctx.text));
        for (auto& row : state.rows)
        {
            if (row.key == key)
            {
                row.value = PropertyText(parsed);
                if (row.type == EditorPropertyType::Asset)
                {
                    if (row.value.empty()) row.displayValue = ctx.text.GetText("EditorUi.None");
                    else
                    {
                        const auto resolved = ctx.assets.Resolve(row.value);
                        row.displayValue = resolved.empty() ? row.value : asset_io::Utf8(resolved.filename());
                    }
                }
            }
        }
        if (!ctx.shell.ColorPickerOpen() && !ctx.shell.Dispatcher().PointerCapture().IsValid())
        {
            if (ctx.host.SaveMaterial(ctx.assets))
                HandleAssetSelection(ctx, asset_io::Utf8(state.assetPath));
        }
        return true;
    }
    if (key == "asset.openMaterial")
    {
        return ctx.shell.Commands().Execute({ui::editor::EditorCommandId::TogglePanel, "material"});
    }
    if (key == "asset.newSubfolder")
    {
        return ctx.shell.Commands().Execute({ui::editor::EditorCommandId::CreateAsset, "folder"});
    }
    if (key == "asset.showExplorer")
    {
        if (!state.assetPath.empty())
        {
            std::error_code ec;
            const bool isDir = std::filesystem::is_directory(state.assetPath, ec);
            ctx.host.OpenInExplorer(state.assetPath, !isDir);
            return true;
        }
        return false;
    }
    return false;
}
} // namespace

std::string LocalizedToolkitStatus(const Localization& text,
                                     std::string_view keyOrMessage,
                                     std::string_view fallbackKey)
{
    if (!keyOrMessage.empty())
    {
        const std::string translated = text.GetText(keyOrMessage);
        if (!translated.empty() && translated != keyOrMessage)
        {
            return translated;
        }
        const bool looksLikeKey = keyOrMessage.find(' ') == std::string_view::npos &&
                                  keyOrMessage.find('.') != std::string_view::npos;
        if (!looksLikeKey)
        {
            return std::string(keyOrMessage);
        }
    }
    const std::string fallback = text.GetText(fallbackKey);
    return fallback.empty() ? std::string(fallbackKey) : fallback;
}

std::vector<EditorProperty> BuildMaterialFormRows(const EditorMaterialWorkflow& material, const Localization& text)
{
    std::vector<EditorProperty> rows;
    if (!material.Ready()) return rows;
    for (const PropertyDescriptor& descriptor : MaterialProperties())
    {
        PropertyValue value;
        std::string error;
        if (!ReadMaterialProperty(material.Document(), descriptor.key, value, error)) continue;
        EditorPropertyType type = EditorPropertyType::String;
        if (descriptor.type == PropertyType::Number) type = EditorPropertyType::Number;
        else if (descriptor.type == PropertyType::Boolean) type = EditorPropertyType::Boolean;
        else if (descriptor.type == PropertyType::AssetId) type = EditorPropertyType::Asset;
        else if (descriptor.type == PropertyType::Vector3 || descriptor.type == PropertyType::Vector4)
            type = EditorPropertyType::Color;
        const std::string label = descriptor.labelKey.empty()
            ? std::string(descriptor.key) : text.GetText(descriptor.labelKey);
        rows.push_back({std::string(descriptor.key), label, type, PropertyText(value), !descriptor.editable});
    }
    return rows;
}

bool ParseMaterialFormValue(const PropertyDescriptor& descriptor, std::string_view text, PropertyValue& output)
{
    try
    {
        switch (descriptor.type)
        {
        case PropertyType::Boolean: output = (text == "true" || text == "1"); break;
        case PropertyType::Number: output = std::stof(std::string(text)); break;
        case PropertyType::Vector3:
        {
            float values[3]{};
            if (!ParseFloatList(text, values, 3)) return false;
            output = Vec3{values[0], values[1], values[2]};
            break;
        }
        case PropertyType::Vector4:
        {
            float values[4]{0.0f, 0.0f, 0.0f, 1.0f};
            if (!ParseFloatList(text, values, 4) && !ParseFloatList(text, values, 3)) return false;
            output = Vec4{values[0], values[1], values[2], values[3]};
            break;
        }
        case PropertyType::Quaternion:
        {
            float values[4]{};
            if (!ParseFloatList(text, values, 4)) return false;
            output = Quat{values[0], values[1], values[2], values[3]};
            break;
        }
        case PropertyType::AssetList:
        {
            std::vector<std::string> values;
            std::string item;
            std::istringstream stream{std::string(text)};
            while (std::getline(stream, item, ','))
            {
                while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
                while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.pop_back();
                if (!item.empty()) values.push_back(item);
            }
            output = std::move(values);
            break;
        }
        case PropertyType::EntityReference:
            output = PropertyEntityReference{std::stoull(std::string(text))};
            break;
        case PropertyType::AssetId:
        case PropertyType::Text:
            output = std::string(text);
            break;
        }
    }
    catch (...)
    {
        return false;
    }
    return ValidatePropertyValue(descriptor, output);
}

std::vector<EditorProperty> BuildProjectSettingsRows(const ProjectSettings& settings,
                                                   const Localization& text,
                                                   const std::filesystem::path& projectRoot)
{
    std::vector<EditorProperty> rows;
    rows.push_back({"name", text.GetText("EditorUi.Name"), EditorPropertyType::String, settings.name, false});
    rows.push_back({"contentRoot", text.GetText("EditorUi.ContentRoot"), EditorPropertyType::String, settings.contentRoot, false});
    rows.push_back({"entryScene", text.GetText("EditorUi.EntryScene"), EditorPropertyType::String, settings.entryScene, false});
    rows.push_back({"showPlayerQualityMenu", text.GetText("PlayerQuality.ShowMenu"), EditorPropertyType::Boolean,
                    settings.showPlayerQualityMenu ? "true" : "false", false});
    rows.push_back({"playerSettingsId", text.GetText("PlayerQuality.GameId"), EditorPropertyType::String, settings.playerSettingsId, false});
    for (size_t i = 0; i < settings.qualityProfiles.size(); ++i)
    {
        const auto& profile = settings.qualityProfiles[i];
        const std::string prefix = std::string("quality.") + PlayerQualityIds[i + 1] + ".";
        const auto label = text.GetText(std::array{"PlayerQuality.Performance", "PlayerQuality.Balanced", "PlayerQuality.High"}[i]) + " · ";
        rows.push_back({prefix + "sampling", label + text.GetText("PlayerQuality.Sampling"), EditorPropertyType::Combo,
                        QualitySamplingIds[static_cast<size_t>(profile.sampling)], false});
        for (const auto* method : QualitySamplingIds)
        {
            rows.back().options.push_back(method);
            rows.back().optionLabels.push_back(text.GetText(std::string("PlayerQuality.Sampling.") + method));
        }
        rows.push_back({prefix + "shadows", label + text.GetText("PlayerQuality.AllowShadows"), EditorPropertyType::Boolean,
                        profile.allowShadows ? "true" : "false", false});
        rows.push_back({prefix + "bloom", label + text.GetText("PlayerQuality.AllowBloom"), EditorPropertyType::Boolean,
                        profile.allowBloom ? "true" : "false", false});
    }
    rows.push_back({"assets", text.GetText("EditorUi.Assets"), EditorPropertyType::String,
                    std::to_string(settings.assets.size()) + " " + text.GetText("EditorUi.Registered"), true});
    if (!projectRoot.empty())
    {
        InputActionDocument inputDoc;
        std::string inputError;
        if (ProjectSettingsService::ReadInputActions(projectRoot, settings, inputDoc, inputError))
        {
            std::string moveDeadZone = "0.15";
            std::string jumpKey = "32";
            for (const InputActionDefinition& action : inputDoc.actions)
            {
                if (action.name == "Move") moveDeadZone = std::to_string(action.deadZone);
                if (action.name == "Jump")
                {
                    for (const InputBinding& b : action.bindings)
                    {
                        if (b.device == InputDeviceKind::Keyboard) { jumpKey = std::to_string(b.keyCode); break; }
                    }
                }
            }
            rows.push_back({"input.actionsCount", text.GetText("EditorUi.InputActionsCount"), EditorPropertyType::String,
                            std::to_string(inputDoc.actions.size()), true});
            rows.push_back({"input.deadZone.Move", text.GetText("EditorUi.InputMoveDeadZone"), EditorPropertyType::Number,
                            moveDeadZone, false});
            rows.back().dragStep = 0.01f;
            rows.back().dragMinimum = 0.0f;
            rows.back().dragMaximum = 0.99f;
            rows.push_back({"input.key.Jump", text.GetText("EditorUi.InputJumpKey"), EditorPropertyType::Number,
                            jumpKey, false});
            rows.back().dragStep = 1.0f;
            rows.back().dragMinimum = 0.0f;
            rows.back().dragMaximum = 255.0f;
            rows.push_back({"input.resetDefaults", text.GetText("EditorUi.InputResetDefaults"), EditorPropertyType::Action,
                            {}, false});
        }
    }
    return rows;
}

std::vector<EditorProperty> BuildBuildSettingsRows(const BuildSettings& settings,
                                                   const std::string& exportName,
                                                   const Localization& text)
{
    std::vector<EditorProperty> rows;
    rows.push_back({"exportName", text.GetText("EditorUi.ExportName"), EditorPropertyType::String, exportName, false});
    rows.push_back({"recordVideo", text.GetText("EditorUi.RecordVideo"), EditorPropertyType::Boolean,
                    settings.recordVideo ? "true" : "false"});
    rows.push_back({"videoResolution", text.GetText("EditorUi.VideoResolution"), EditorPropertyType::Combo,
                    settings.videoResolution, false, {"640x360", "1280x720", "1920x1080"}});
    rows.push_back({"videoFps", text.GetText("EditorUi.VideoFps"), EditorPropertyType::Combo,
                    std::to_string(settings.videoFps), false, {"24", "30", "60"}});
    rows.push_back({"videoSeconds", text.GetText("EditorUi.VideoSeconds"), EditorPropertyType::Number,
                    std::to_string(settings.videoSeconds)});
    rows.push_back({"openOutput", text.GetText("EditorUi.ExportOpenOutput"), EditorPropertyType::Action, {}});
    rows.push_back({"scriptsProject", text.GetText("EditorUi.ScriptsProject"), EditorPropertyType::String, settings.scriptsProject, false});
    rows.push_back({"scriptType", text.GetText("EditorUi.ScriptType"), EditorPropertyType::String, settings.scriptType, false});
    rows.push_back({"bindings", text.GetText("EditorUi.Bindings"), EditorPropertyType::String, settings.bindings, false});
    rows.push_back({"entryUi", text.GetText("EditorUi.EntryUi"), EditorPropertyType::String, settings.entryUi, false});
    return rows;
}

std::vector<EditorProperty> BuildPreferencesRows(const EditorPreferences& preferences, const Localization& text)
{
    std::vector<EditorProperty> rows;
    rows.push_back({"language", text.GetText("EditorUi.Language"), EditorPropertyType::Combo, preferences.language, false, {"zh-CN", "en-US"}});
    rows.push_back({"uiScale", text.GetText("EditorUi.UiScale"), EditorPropertyType::Number, std::to_string(preferences.uiScale), false});
    rows.push_back({"externalEditor", text.GetText("EditorUi.ExternalEditor"), EditorPropertyType::String, preferences.externalEditor, false});
    rows.push_back({"colorLut", text.GetText("EditorUi.ColorLut"), EditorPropertyType::String, preferences.colorLut, false});
    rows.push_back({"restoreLastProject", text.GetText("EditorUi.RestoreLastProject"), EditorPropertyType::Boolean,
                    preferences.restoreLastProject ? "true" : "false", false});
    rows.push_back({"recentProjects", text.GetText("EditorUi.RecentProjects"), EditorPropertyType::String,
                    std::to_string(preferences.recentProjects.size()), true});
    return rows;
}

bool SaveAllToolkitDocuments(ToolkitWorkspaceContext& ctx)
{
    if (!ctx.scriptWorkflow.Save())
    {
        ctx.shell.SetStatus(ctx.scriptWorkflow.Status());
        return false;
    }
    if (ctx.host.TimeOfDayPreview().Dirty() && !ctx.host.TimeOfDayPreview().Save()) return false;
    if (ctx.host.Material().Dirty() && !ctx.host.SaveMaterial(ctx.assets))
    {
        Log(LogLevel::Warning, "EditorUi.ToolkitSaveMaterial: " + ctx.host.LastError());
        ctx.shell.SetFormStatus(EditorPanelId::Material,
                                LocalizedToolkitStatus(ctx.text, ctx.host.LastError(), "EditorUi.SaveMaterialFailed"));
        return false;
    }
    if (ctx.host.ProjectSettingsDoc().Dirty() && !ctx.host.SaveSettings(true))
    {
        ctx.shell.SetFormStatus(EditorPanelId::ProjectSettings,
                                LocalizedToolkitStatus(ctx.text, ctx.host.LastError(), "EditorUi.SaveFailed"));
        return false;
    }
    if (ctx.host.BuildSettingsDoc().Dirty() && !ctx.host.SaveSettings(false))
    {
        ctx.shell.SetFormStatus(EditorPanelId::BuildExport,
                                LocalizedToolkitStatus(ctx.text, ctx.host.LastError(), "EditorUi.SaveFailed"));
        return false;
    }
    if (ctx.host.Ui().Dirty(&ctx.ui))
    {
        const auto targetUi = ctx.assetState.explicitUiPath.empty()
            ? ctx.host.Ui().EffectivePath() : ctx.assetState.explicitUiPath;
        if (targetUi.empty() || !ctx.host.SaveUi(ctx.ui, targetUi))
        {
            ctx.shell.SetStatus(LocalizedToolkitStatus(ctx.text, ctx.host.LastError(), "EditorUi.UiSaveFailed"));
            return false;
        }
    }
    if (!ctx.session.Dirty()) return true;
    std::filesystem::path scenePath = ctx.session.Path();
    if (scenePath.empty())
    {
        std::error_code dirEc;
        std::filesystem::create_directories(ctx.root / "Scenes", dirEc);
        scenePath = ctx.root / "Scenes" / "Untitled.scene.json";
    }
    const bool saved = ctx.session.Save(scenePath);
    if (saved) ctx.externalChange.Reset(ctx.session.Path());
    return saved;
}

void BindToolkitWorkspaceForms(ToolkitWorkspaceContext& ctx, const std::function<bool()>& exportAction)
{
    ctx.shell.SetSelectionHandler([&ctx](std::uint64_t id) {
        if (ctx.assetState.inspectUi) ctx.shell.CancelPropertyInteraction();
        ctx.assetState.inspectUi = false;
        ctx.assetState.assetName.clear();
        ctx.session.Select(id);
        Log(LogLevel::Info, "EditorUi.ToolkitSelection: " + std::to_string(id));
    });

    ctx.shell.SetAssetSelectionHandler([&ctx](std::string path) {
        HandleAssetSelection(ctx, path);
    });

    ctx.shell.SetDesignerHandlers(
        [&ctx](float x, float y) {
            ctx.assetState.inspectUi = true;
            ctx.assetState.assetName.clear();
            ctx.host.SetDocumentContext(EditorWorkspaceHost::DocumentContext::Ui);
            const UiId hit = ctx.host.Ui().Canvas().Hit(ctx.ui, {x, y});
            ctx.host.Ui().Select(hit);
            if (!hit) return;
            if (!ctx.host.Ui().BeginDrag(ctx.ui, hit, {x, y}))
                ctx.shell.SetStatus(ctx.text.GetText("EditorUi.DesignerDragFailed"));
        },
        [&ctx](float x, float y) {
            if (ctx.host.Ui().Canvas().Active()) ctx.host.Ui().UpdateDrag(ctx.ui, {x, y});
        },
        [&ctx](float, float) {
            if (!ctx.host.Ui().Canvas().Active() || !ctx.host.Ui().EndDrag(ctx.ui, true)) return;
            const auto targetUi = ctx.assetState.explicitUiPath.empty()
                ? ctx.host.Ui().EffectivePath() : ctx.assetState.explicitUiPath;
            if (!targetUi.empty() && ctx.host.SaveUi(ctx.ui, targetUi))
                Log(LogLevel::Info, "EditorUi.ToolkitUi: saved " + asset_io::Utf8(targetUi));
            else
                ctx.shell.SetStatus(LocalizedToolkitStatus(ctx.text, ctx.host.LastError(), "EditorUi.UiSaveFailed"));
        });

    ctx.shell.SetPropertyEditHandler([&ctx](std::uint64_t, ui::editor::EditorPropertyEditPhase phase) {
        using enum ui::editor::EditorPropertyEditPhase;
        if (ctx.assetState.inspectUi)
        {
            if (ctx.session.IsPlaying()) return false;
            if (phase == Begin) ctx.host.Ui().BeginEdit(ctx.ui);
            else ctx.host.Ui().EndEdit(ctx.ui, phase == Commit);
            return true;
        }
        if (!ctx.assetState.assetName.empty() && IsMaterialDocumentPath(ctx.assetState.assetPath))
        {
            if (ctx.session.IsPlaying()) return false;
            if (phase == Begin) return ctx.host.Material().Ready();
            if (phase == Commit)
            {
                if (ctx.host.Material().Dirty() && !ctx.host.SaveMaterial(ctx.assets)) return false;
                HandleAssetSelection(ctx, asset_io::Utf8(ctx.assetState.assetPath));
                return true;
            }
            ctx.host.OpenMaterial(ctx.assets, ctx.assetState.assetPath, false);
            HandleAssetSelection(ctx, asset_io::Utf8(ctx.assetState.assetPath));
            return true;
        }
        if (ctx.session.IsPlaying()) return true;
        if (phase == Begin)
        {
            const bool result = ctx.session.BeginEdit("EditorUi.PropertyDrag");
            if (result) ctx.host.TimeOfDayPreview().BeginEdit();
            return result;
        }
        const auto tod = ctx.host.TimeOfDayPreview().EndEdit(phase == Commit);
        const auto scene = phase == Commit ? ctx.session.CommitEdit() : ctx.session.CancelEdit();
        if (phase == Commit && scene) ctx.assetState.preferScriptUndo = false;
        return tod || scene;
    });

    ctx.shell.SetPropertyChoices([&ctx](std::uint64_t id, EditorProperty& field) {
        return FillSceneInspectorChoices(ctx.session, ctx.assets, ctx.text, id, field);
    });

    ctx.shell.SetPropertyHandler([&ctx](std::uint64_t id, std::string key, std::string value) {
        if (key == "asset.targetPath" || key == "asset.move" || key == "asset.delete" ||
            key == "asset.addModel" || key == "asset.addPrefab" || key == "asset.openScript" ||
            key == "asset.scriptClass" || key == "asset.attachScript" || key == "asset.compileScripts" ||
            key == "asset.deleteScript" || key.starts_with("asset.mat.") ||
            key == "asset.previewAudio" || key == "asset.stopAudio" || key == "asset.openMaterial" ||
            key == "asset.newSubfolder" || key == "asset.showExplorer")
        {
            return HandleAssetPropertyAction(ctx, key, value);
        }
        if (key == "prefab.apply")
        {
            std::string error;
            if (!ApplyInstanceToPrefab(ctx.session, ctx.assets, ctx.root, id, error, ctx.hooks))
            {
                ctx.shell.SetStatus(LocalizedToolkitStatus(ctx.text, error, "EditorUi.SaveFailed"));
                return false;
            }
            const auto msg = ctx.text.GetText("EditorUi.PrefabApplied");
            ctx.shell.SetStatus(msg);
            ctx.shell.AppendConsoleLine(msg);
            return true;
        }
        if (key == "prefab.revert")
        {
            std::string error;
            if (!RevertAllOverrides(ctx.session, ctx.assets, id, error))
            {
                ctx.shell.SetStatus(LocalizedToolkitStatus(ctx.text, error, "EditorUi.SaveFailed"));
                return false;
            }
            ctx.shell.SetStatus(ctx.text.GetText("EditorUi.PrefabReverted"));
            return true;
        }
        if (key.starts_with("prefab.revertProperty:"))
        {
            std::string error;
            if (!RevertPropertyOverride(ctx.session, ctx.assets, id, std::string_view(key).substr(22), error))
            {
                ctx.shell.SetStatus(LocalizedToolkitStatus(ctx.text, error, "EditorUi.SaveFailed"));
                return false;
            }
            ctx.shell.SetStatus(ctx.text.GetText("EditorUi.PrefabReverted"));
            return true;
        }
        if (key == "preview.play" || key == "preview.time")
        {
            std::string error;
            if (!ctx.clipPreview.Select(ctx.session, ctx.assets, id, error))
            {
                ctx.shell.SetStatus(LocalizedToolkitStatus(ctx.text, error, "EditorUi.SaveFailed"));
                return false;
            }
            if (key == "preview.play") ctx.clipPreview.Toggle();
            else
            {
                double seconds = 0;
                auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), seconds);
                if (status != std::errc{} || end != value.data() + value.size() || !std::isfinite(seconds) || seconds < 0)
                    return false;
                ctx.clipPreview.Seek(seconds);
            }
            return true;
        }
        ctx.assetState.editTod = key.starts_with("tod.");
        if (ctx.assetState.editTod)
        {
            if (key == "tod.open" && ctx.host.TimeOfDayPreview().Dirty())
            {
                ctx.shell.SetStatus(ctx.text.GetText("EditorUi.SaveFirst"));
                return false;
            }
            const auto result = ApplyTodInspector(ctx.host.TimeOfDayPreview(), ctx.assets, ctx.session, key, value);
            if (!result)
            {
                ctx.shell.SetStatus(LocalizedToolkitStatus(ctx.text, ctx.host.TimeOfDayPreview().LastError(), "EditorUi.SaveFailed"));
            }
            return result;
        }
        if (key.starts_with("ui."))
        {
            if (ctx.session.IsPlaying()) return false;
            UiId selected = id;
            const bool result = ctx.host.Ui().Edit(ctx.ui, [&](UiTree& tree) {
                return ApplyUiInspector(tree, selected, key, value, selected);
            });
            if (result) ctx.host.Ui().Select(selected);
            return result;
        }
        if (key.starts_with("script."))
        {
            CodeEditorSettings settings;
            settings.executable = asset_io::FromUtf8(ctx.host.Preferences().externalEditor);
            const bool result = ctx.scriptWorkflow.Apply(id, ctx.session.IsPlaying(), key, value, settings);
            if (result && !ctx.session.IsPlaying()) ctx.assetState.preferScriptUndo = true;
            if (!ctx.scriptWorkflow.Status().empty()) ctx.shell.SetStatus(ctx.scriptWorkflow.Status());
            std::uint64_t pendingEntity = 0;
            std::string pendingScriptId;
            std::filesystem::path createdSource;
            if (result && ctx.scriptWorkflow.ConsumeCreatedPendingAttach(pendingEntity, pendingScriptId, createdSource))
            {
                ctx.shell.Commands().Execute(
                    {ui::editor::EditorCommandId::Build, std::to_string(pendingEntity) + "|" + pendingScriptId});
            }
            return result;
        }
        const bool applied = ApplySceneInspector(ctx.session, id, key, value, &ctx.assets);
        if (applied && !ctx.session.IsPlaying()) ctx.assetState.preferScriptUndo = false;
        return applied;
    });

    if (!ctx.host.ProjectSettingsDoc().Loaded() && !ctx.host.LoadProjectSettings(true))
    {
        Log(LogLevel::Warning, "EditorUi.ToolkitProjectSettings: " + ctx.host.LastError());
    }
    if (!ctx.host.BuildSettingsDoc().Loaded())
    {
        if (!ctx.host.LoadBuildSettings(true))
            Log(LogLevel::Warning, "EditorUi.ToolkitBuildSettings: " + ctx.host.LastError());
        else if (!ctx.host.BuildSettingsDoc().SourceExists())
            Log(LogLevel::Warning, "EditorUi.ToolkitBuildSettings: Project.BuildSettingsMissing");
    }
    ctx.shell.SetFormProperties(EditorPanelId::ProjectSettings, BuildProjectSettingsRows(ctx.host.Project(), ctx.text, ctx.root));
    ctx.shell.SetFormProperties(EditorPanelId::BuildExport, BuildBuildSettingsRows(ctx.host.Build(), ctx.exportName, ctx.text));

    ctx.shell.SetFormHandlers(
        EditorPanelId::ProjectSettings,
        [&ctx](std::string key, std::string value) {
            if (key.starts_with("input."))
            {
                std::string inputError;
                if (!ProjectSettingsService::ApplyInputActionSetting(ctx.root, ctx.host.Project(), key, value, inputError))
                {
                    ctx.shell.SetFormStatus(EditorPanelId::ProjectSettings,
                        LocalizedToolkitStatus(ctx.text, inputError, "Input.InvalidDocument"));
                    return false;
                }
                ctx.shell.SetFormProperties(EditorPanelId::ProjectSettings,
                    BuildProjectSettingsRows(ctx.host.Project(), ctx.text, ctx.root));
                ctx.shell.SetFormStatus(EditorPanelId::ProjectSettings, ctx.text.GetText("EditorUi.Saved"));
                return true;
            }
            return ApplyProjectSettingsField(ctx.host, key, value);
        },
        [&ctx]() {
            if (!ctx.host.SaveSettings(true))
            {
                ctx.shell.SetFormStatus(EditorPanelId::ProjectSettings,
                    LocalizedToolkitStatus(ctx.text, ctx.host.LastError(), "EditorUi.SaveFailed"));
                return false;
            }
            ctx.shell.SetFormStatus(EditorPanelId::ProjectSettings, ctx.text.GetText("EditorUi.Saved"));
            Log(LogLevel::Info, "EditorUi.ToolkitProjectSettings: saved");
            return true;
        });

    ctx.shell.SetFormHandlers(
        EditorPanelId::BuildExport,
        [&ctx](std::string key, std::string value) { return ApplyBuildSettingsField(ctx, key, value); },
        [&ctx]() {
            if (!ctx.host.SaveSettings(false))
            {
                ctx.shell.SetFormStatus(EditorPanelId::BuildExport,
                    LocalizedToolkitStatus(ctx.text, ctx.host.LastError(), "EditorUi.SaveFailed"));
                return false;
            }
            ctx.shell.SetFormStatus(EditorPanelId::BuildExport, ctx.text.GetText("EditorUi.Saved"));
            Log(LogLevel::Info, "EditorUi.ToolkitBuildSettings: saved");
            return true;
        });
    ctx.shell.SetFormAction(EditorPanelId::BuildExport, ctx.text.GetText("EditorUi.Export"),
                            [exportAction]() { return exportAction ? exportAction() : false; });

    ctx.shell.SetFormStatus(EditorPanelId::Material, ctx.text.GetText("EditorUi.SelectMaterial"));
    ctx.shell.SetFormHandlers(
        EditorPanelId::Material,
        [&ctx](std::string key, std::string value) {
            const PropertyDescriptor* descriptor = FindMaterialProperty(key);
            if (!descriptor) return false;
            PropertyValue parsed;
            if (!ParseMaterialFormValue(*descriptor, value, parsed)) return false;
            const bool applied = ctx.host.Material().SetProperty(key, parsed);
            if (applied) Log(LogLevel::Info, "EditorUi.ToolkitMaterial: property " + key);
            return applied;
        },
        [&ctx]() {
            if (!ctx.host.SaveMaterial(ctx.assets))
            {
                ctx.shell.SetFormStatus(EditorPanelId::Material,
                    LocalizedToolkitStatus(ctx.text, ctx.host.LastError(), "EditorUi.SaveMaterialFailed"));
                return false;
            }
            ctx.shell.SetFormProperties(EditorPanelId::Material, BuildMaterialFormRows(ctx.host.Material(), ctx.text));
            ctx.shell.SetFormStatus(EditorPanelId::Material, ctx.text.GetText("EditorUi.Saved"));
            Log(LogLevel::Info, "EditorUi.ToolkitMaterial: saved");
            return true;
        });
    ctx.shell.SetFormAction(EditorPanelId::Material, ctx.text.GetText("EditorUi.AssignMaterial"),
        [&ctx]() {
            const StableEntityId selected = ctx.session.Selected();
            if (!ctx.host.Material().Ready() || selected == 0 || ctx.session.IsPlaying()) return false;
            const EntityHandle entity = ctx.session.EditWorld().FindByStableId(selected);
            const auto* mesh = ctx.session.EditWorld().GetMesh(entity);
            if (!mesh) return false;
            MeshComponent updated = *mesh;
            if (updated.materials.empty()) updated.materials.push_back(ctx.host.Material().ActiveAssetId());
            else updated.materials[0] = ctx.host.Material().ActiveAssetId();
            if (!ctx.session.SetMesh(selected, updated)) return false;
            ctx.shell.SetFormStatus(EditorPanelId::Material, ctx.text.GetText("EditorUi.Saved"));
            Log(LogLevel::Info, "EditorUi.ToolkitMaterial: assigned " + ctx.host.Material().ActiveAssetId());
            return true;
        });

    ctx.shell.SetFormProperties(EditorPanelId::Preferences, BuildPreferencesRows(ctx.host.Preferences(), ctx.text));
    ctx.shell.SetFormHandlers(
        EditorPanelId::Preferences,
        [&ctx](std::string key, std::string value) { return ApplyPreferencesField(ctx.host, key, value); },
        [&ctx]() {
            if (!ctx.host.SavePreferences())
            {
                ctx.shell.SetFormStatus(EditorPanelId::Preferences,
                    LocalizedToolkitStatus(ctx.text, ctx.host.LastError(), "EditorUi.SaveFailed"));
                return false;
            }
            ctx.shell.SetFormStatus(EditorPanelId::Preferences,
                ctx.text.GetText("EditorUi.Saved") + " (" + ctx.text.GetText("EditorUi.RestartToApply") + ")");
            Log(LogLevel::Info, "EditorUi.ToolkitPreferences: saved");
            return true;
        });
}

} // namespace se
