#include "EditorShaderGraphController.h"

#include "CoreLayer/Log.h"
#include "EditorAssetActions.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/MaterialDocument.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace flare
{
namespace
{
constexpr std::size_t kMaxShaderGraphUndo = 64;

std::vector<std::string> SplitBy(std::string_view text, char sep)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t next = text.find(sep, start);
        if (next == std::string_view::npos)
        {
            parts.emplace_back(text.substr(start));
            break;
        }
        parts.emplace_back(text.substr(start, next - start));
        start = next + 1;
    }
    return parts;
}

bool ParseFloats(std::string_view text, float* out, std::size_t maxCount, std::size_t& outCount)
{
    outCount = 0;
    const char* cur = text.data();
    const char* endAll = text.data() + text.size();
    while (cur < endAll && outCount < maxCount)
    {
        while (cur < endAll && (*cur == ' ' || *cur == ',' || *cur == ';' || *cur == '\t')) ++cur;
        if (cur >= endAll) break;
        std::string tmp(cur, static_cast<std::size_t>(endAll - cur));
        char* numEnd = nullptr;
        const float val = std::strtof(tmp.c_str(), &numEnd);
        if (numEnd == tmp.c_str() || !std::isfinite(val)) break;
        out[outCount++] = val;
        cur += (numEnd - tmp.c_str());
    }
    return outCount > 0;
}

std::string FormatNodeValue(const ShaderGraphNode& node)
{
    char buf[160]{};
    if (node.type == "Float")
    {
        std::snprintf(buf, sizeof(buf), "%.3f", node.constantValue.x);
    }
    else if (node.type == "Range")
    {
        std::snprintf(buf, sizeof(buf), "%.3f, %.2f, %.2f", node.constantValue.x, node.rangeMin, node.rangeMax);
    }
    else if (node.type == "Boolean")
    {
        std::snprintf(buf, sizeof(buf), "%d", node.constantValue.x >= 0.5f ? 1 : 0);
    }
    else if (node.type == "Vector2")
    {
        std::snprintf(buf, sizeof(buf), "%.3f, %.3f", node.constantValue.x, node.constantValue.y);
    }
    else if (node.type == "Vector3")
    {
        std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f",
                      node.constantValue.x, node.constantValue.y, node.constantValue.z);
    }
    else if (node.type == "Color" || node.type == "Vector4")
    {
        std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f, %.3f",
                      node.constantValue.x, node.constantValue.y, node.constantValue.z, node.constantValue.w);
    }
    return buf;
}

bool NodeHasInlineConstant(std::string_view type) noexcept
{
    return type == "Float" || type == "Range" || type == "Boolean" || type == "Color" ||
           type == "Vector2" || type == "Vector3" || type == "Vector4";
}

bool IsAutoWiredInputPort(const ShaderNodePortSpec& p) noexcept
{
    if (p.id == "UV" && p.type == ShaderPortValueType::Vec2) return true;
    if (p.id == "NormalWS" && p.type == ShaderPortValueType::Vec3) return true;
    if (p.id == "CoordsWS" && p.type == ShaderPortValueType::Vec3) return true;
    if (p.id == "HeightWS" && p.type == ShaderPortValueType::Float) return true;
    if (p.id == "Time" && p.type == ShaderPortValueType::Float && p.defaultValue.x < 0.0f) return true;
    return false;
}

std::vector<const ShaderNodePortSpec*> CollectEditableUnconnectedInputs(
    const ShaderGraphDocument& graph, const ShaderGraphNode& node)
{
    std::vector<const ShaderNodePortSpec*> out;
    const auto* spec = FindShaderNodeSpec(node.type);
    if (!spec || spec->isMaster || NodeHasInlineConstant(node.type)) return out;
    for (const auto& p : spec->inputs)
    {
        if (IsAutoWiredInputPort(p)) continue;
        const bool connected = std::any_of(graph.links.begin(), graph.links.end(), [&](const ShaderGraphLink& l) {
            return l.toNode == node.id && l.toPort == p.id;
        });
        if (connected) continue;
        if (out.empty())
        {
            out.push_back(&p);
            if (p.type != ShaderPortValueType::Float) break; // vector port edited alone
        }
        else if (p.type == ShaderPortValueType::Float && out.size() < 4)
        {
            out.push_back(&p);
        }
    }
    return out;
}

std::string FormatEditableInputs(const ShaderGraphDocument& graph, const ShaderGraphNode& node)
{
    const auto ports = CollectEditableUnconnectedInputs(graph, node);
    if (ports.empty()) return {};
    char buf[192]{};
    const auto getDef = [&](const ShaderNodePortSpec* p) -> Vec4 {
        const auto it = node.inputDefaults.find(p->id);
        return it != node.inputDefaults.end() ? it->second : p->defaultValue;
    };
    if (ports[0]->type != ShaderPortValueType::Float)
    {
        const Vec4 v = getDef(ports[0]);
        if (ports[0]->type == ShaderPortValueType::Vec2)
            std::snprintf(buf, sizeof(buf), "%.3f, %.3f", v.x, v.y);
        else if (ports[0]->type == ShaderPortValueType::Vec3)
            std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f", v.x, v.y, v.z);
        else
            std::snprintf(buf, sizeof(buf), "%.3f, %.3f, %.3f, %.3f", v.x, v.y, v.z, v.w);
        return buf;
    }
    std::string res;
    for (std::size_t i = 0; i < ports.size(); ++i)
    {
        std::snprintf(buf, sizeof(buf), "%.3f", getDef(ports[i]).x);
        if (i > 0) res += ", ";
        res += buf;
    }
    return res;
}

std::string SanitizeParamIdentifier(std::string_view raw, std::string_view fallback)
{
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw)
    {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (std::isalnum(uc) || ch == '_') out.push_back(ch);
    }
    if (out.empty()) out = std::string(fallback);
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out.front()))) out.insert(out.begin(), '_');
    return out;
}

std::string ShaderDisplayStem(const std::filesystem::path& p)
{
    std::string fname = asset_io::Utf8(p.filename());
    if (fname.size() > 13 && IsSurfaceShaderHlslPath(p)) return fname.substr(0, fname.size() - 13);
    if (fname.size() > 13 && IsShaderGraphPath(p)) return fname.substr(0, fname.size() - 13);
    return asset_io::Utf8(p.stem());
}
} // namespace

bool CreateCompanionShaderMaterial(const std::filesystem::path& projectRoot,
                                   AssetStore& assets,
                                   EditorSession& session,
                                   const std::filesystem::path& shaderPath,
                                   std::filesystem::path& outMaterialPath,
                                   std::string& outError)
{
    outMaterialPath.clear();
    outError.clear();

    AssetId shaderId;
    for (const AssetId& id : assets.ListIds())
    {
        std::error_code ec;
        const auto resolved = assets.Resolve(id);
        if (resolved == shaderPath || (std::filesystem::equivalent(resolved, shaderPath, ec) && !ec))
        {
            shaderId = id;
            break;
        }
    }
    if (shaderId.empty())
    {
        std::vector<AssetDiagnostic> regDiag;
        if (!assets.RegisterSource(shaderPath, {}, shaderId, regDiag))
        {
            outError = regDiag.empty() ? "Assets.WriteFailed" : regDiag.back().key;
            return false;
        }
    }

    MaterialDocument doc;
    doc.shader = shaderId;
    std::vector<AssetDiagnostic> loadDiag;
    UserSurfaceShaderSource parsed;
    if (assets.LoadSurfaceShader(shaderId, parsed, loadDiag))
    {
        if (parsed.metadata.blendOverride.has_value()) doc.alphaMode = *parsed.metadata.blendOverride;
        if (parsed.metadata.doubleSidedOverride.has_value()) doc.doubleSided = *parsed.metadata.doubleSidedOverride;
        for (const auto& param : parsed.metadata.parameters)
        {
            doc.shaderParameters[param.name] = param.defaultValue;
        }
    }

    const std::string baseStem = ShaderDisplayStem(shaderPath) + "_Mat";
    const std::filesystem::path dir = shaderPath.has_parent_path() ? shaderPath.parent_path() : assets.SourceRoot();
    AssetId matId;
    for (unsigned idx = 0; idx < 1000; ++idx)
    {
        const std::string name = idx == 0 ? baseStem : (baseStem + std::to_string(idx));
        doc.name = name;
        const std::filesystem::path candidate = dir / asset_io::FromUtf8(name + ".material.json");
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) continue;
        std::vector<AssetDiagnostic> saveDiag;
        if (!assets.SaveMaterialSource(candidate, doc, matId, saveDiag, false))
        {
            outError = saveDiag.empty() ? "Assets.WriteFailed" : saveDiag.back().key;
            if (outError == "Assets.DestinationExists") continue;
            return false;
        }
        outMaterialPath = assets.Resolve(matId);
        break;
    }
    if (outMaterialPath.empty() || matId.empty())
    {
        outError = "Assets.DestinationExists";
        return false;
    }

    std::vector<EditorAssetIssue> issues;
    (void)EditorAssetActions::SyncProjectAssets(projectRoot, assets, issues);

    // 若当前选中了带网格的实体且未处于运行态，直接为该实体赋上新建的配套材质，立即在左侧视口看到效果！
    if (!session.IsPlaying() && session.Selected() != 0)
    {
        const StableEntityId selId = session.Selected();
        const EntityHandle eh = session.EditWorld().FindByStableId(selId);
        if (const auto* mesh = eh ? session.EditWorld().GetMesh(eh) : nullptr)
        {
            MeshComponent updated = *mesh;
            if (updated.materials.empty()) updated.materials.push_back(matId);
            else updated.materials[0] = matId;
            (void)session.SetMesh(selId, updated);
        }
    }
    return true;
}

EditorShaderGraphController::EditorShaderGraphController(const std::filesystem::path& projectRoot,
                                                         const std::filesystem::path& cacheRoot,
                                                         AssetStore& assets,
                                                         EditorSession& session,
                                                         const Localization& text)
    : projectRoot_(projectRoot),
      cacheRoot_(cacheRoot),
      assets_(assets),
      session_(session),
      text_(text)
{
}

void EditorShaderGraphController::Attach(ui::editor::EditorShell& shell)
{
    shell_ = &shell;
    std::vector<ui::editor::EditorShaderPaletteItem> palette;
    for (const auto& spec : BuiltinShaderNodeSpecs())
    {
        if (spec.isMaster) continue;
        palette.push_back({spec.type, spec.category, spec.title, !spec.inputs.empty(), !spec.outputs.empty()});
    }
    shell_->SetShaderGraphPalette(std::move(palette));
    shell_->SetShaderGraphActionHandler([this](std::string_view action, std::string_view payload) {
        HandleAction(action, payload);
    });
    SyncShellView();
}

bool EditorShaderGraphController::Open(const std::filesystem::path& graphPath, bool focusPanel)
{
    std::string err;
    ShaderGraphDocument loaded;
    if (!LoadShaderGraph(graphPath, loaded, err))
    {
        statusText_ = err;
        return false;
    }
    path_ = graphPath;
    std::error_code ec;
    lastDiskMtime_ = std::filesystem::last_write_time(path_, ec);
    graph_ = std::move(loaded);
    loaded_ = true;
    dirty_ = false;
    selectedNodes_.clear();
    selectedLink_.reset();
    undoStack_.clear();
    redoStack_.clear();
    preMoveDoc_.reset();
    previewDirty_ = true;

    std::string hlsl, compileErr;
    statusText_ = CompileShaderGraphToHlsl(graph_, hlsl, compileErr) ? std::string{} : compileErr;

    RebuildPreview(0.0f);
    SyncShellView();
    if (shell_ && focusPanel)
    {
        shell_->FocusPanel(ui::editor::EditorPanelId::ShaderGraph);
    }
    return true;
}

void EditorShaderGraphController::Close()
{
    if (!loaded_ && path_.empty()) return;
    path_.clear();
    lastDiskMtime_ = {};
    graph_ = {};
    loaded_ = false;
    dirty_ = false;
    selectedNodes_.clear();
    selectedLink_.reset();
    undoStack_.clear();
    redoStack_.clear();
    preMoveDoc_.reset();
    preview_ = {};
    statusText_.clear();
    SyncShellView();
}

void EditorShaderGraphController::NotifyAssetMoved(const std::filesystem::path& oldPath,
                                                   const std::filesystem::path& newPath)
{
    if (!loaded_ || path_.empty()) return;
    std::error_code ec;
    if (path_ == oldPath || (std::filesystem::equivalent(path_, oldPath, ec) && !ec))
    {
        path_ = newPath;
        lastDiskMtime_ = std::filesystem::last_write_time(path_, ec);
        SyncShellView();
        return;
    }
    if (asset_io::Within(oldPath, path_))
    {
        path_ = newPath / path_.lexically_relative(oldPath);
        lastDiskMtime_ = std::filesystem::last_write_time(path_, ec);
        SyncShellView();
    }
}

void EditorShaderGraphController::NotifyAssetDeleted(const std::filesystem::path& deletedPath)
{
    if (!loaded_ || path_.empty()) return;
    std::error_code ec;
    if (path_ == deletedPath || (std::filesystem::equivalent(path_, deletedPath, ec) && !ec) ||
        asset_io::Within(deletedPath, path_))
    {
        Close();
    }
}

bool EditorShaderGraphController::ReloadIfModifiedOnDisk()
{
    if (!loaded_ || path_.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec) || ec)
    {
        Close();
        return false;
    }
    const auto mtime = std::filesystem::last_write_time(path_, ec);
    if (ec || mtime == lastDiskMtime_) return false;
    return Open(path_, false);
}

bool EditorShaderGraphController::Save(bool reloadGpu)
{
    if (!loaded_ || path_.empty()) return false;
    std::string err;
    if (!SaveShaderGraph(path_, graph_, err))
    {
        statusText_ = err;
        SyncShellView();
        return false;
    }
    std::error_code ec;
    lastDiskMtime_ = std::filesystem::last_write_time(path_, ec);
    dirty_ = false;

    std::string hlsl, compileErr;
    statusText_ = CompileShaderGraphToHlsl(graph_, hlsl, compileErr) ? std::string{} : compileErr;

    SyncShellView();
    if (reloadGpu && onShaderReloaded_) onShaderReloaded_();
    return true;
}

void EditorShaderGraphController::PushUndoSnapshot()
{
    if (undoStack_.size() >= kMaxShaderGraphUndo) undoStack_.erase(undoStack_.begin());
    undoStack_.push_back(graph_);
    redoStack_.clear();
}

void EditorShaderGraphController::CommitLiveEdit()
{
    dirty_ = true;
    previewDirty_ = true;
    // Auto-save to disk & trigger live D3D12 PSO reload so left Viewport models update in real time!
    (void)Save();
}

bool EditorShaderGraphController::Undo()
{
    if (!loaded_ || undoStack_.empty()) return false;
    redoStack_.push_back(graph_);
    graph_ = std::move(undoStack_.back());
    undoStack_.pop_back();
    selectedNodes_.clear();
    selectedLink_.reset();
    CommitLiveEdit();
    return true;
}

bool EditorShaderGraphController::Redo()
{
    if (!loaded_ || redoStack_.empty()) return false;
    if (undoStack_.size() >= kMaxShaderGraphUndo) undoStack_.erase(undoStack_.begin());
    undoStack_.push_back(graph_);
    graph_ = std::move(redoStack_.back());
    redoStack_.pop_back();
    selectedNodes_.clear();
    selectedLink_.reset();
    CommitLiveEdit();
    return true;
}

bool EditorShaderGraphController::DeleteSelected()
{
    if (!loaded_) return false;
    if (selectedLink_.has_value() && *selectedLink_ < graph_.links.size())
    {
        PushUndoSnapshot();
        graph_.links.erase(graph_.links.begin() + static_cast<std::ptrdiff_t>(*selectedLink_));
        selectedLink_.reset();
        CommitLiveEdit();
        return true;
    }
    std::vector<std::uint32_t> removable;
    for (std::uint32_t nid : selectedNodes_)
    {
        const auto* n = FindNode(nid);
        const auto* spec = n ? FindShaderNodeSpec(n->type) : nullptr;
        if (n && (!spec || !spec->isMaster)) removable.push_back(nid);
    }
    if (removable.empty()) return false;
    PushUndoSnapshot();
    graph_.links.erase(
        std::remove_if(graph_.links.begin(), graph_.links.end(), [&](const ShaderGraphLink& l) {
            return std::find(removable.begin(), removable.end(), l.fromNode) != removable.end() ||
                   std::find(removable.begin(), removable.end(), l.toNode) != removable.end();
        }),
        graph_.links.end());
    graph_.nodes.erase(
        std::remove_if(graph_.nodes.begin(), graph_.nodes.end(), [&](const ShaderGraphNode& n) {
            return std::find(removable.begin(), removable.end(), n.id) != removable.end();
        }),
        graph_.nodes.end());
    selectedNodes_.clear();
    CommitLiveEdit();
    return true;
}

bool EditorShaderGraphController::DuplicateSelected()
{
    if (!loaded_ || selectedNodes_.empty()) return false;
    std::vector<ShaderGraphNode> clones;
    std::unordered_map<std::uint32_t, std::uint32_t> idMap;
    for (std::uint32_t nid : selectedNodes_)
    {
        const auto* src = FindNode(nid);
        const auto* spec = src ? FindShaderNodeSpec(src->type) : nullptr;
        if (!src || (spec && spec->isMaster)) continue;
        ShaderGraphNode copy = *src;
        copy.id = std::max<std::uint32_t>(1u, graph_.nextNodeId++);
        copy.x += 28.0f;
        copy.y += 28.0f;
        if (!copy.paramName.empty()) copy.paramName += "_" + std::to_string(copy.id);
        idMap[src->id] = copy.id;
        clones.push_back(std::move(copy));
    }
    if (clones.empty()) return false;
    PushUndoSnapshot();
    selectedNodes_.clear();
    for (auto& c : clones)
    {
        selectedNodes_.push_back(c.id);
        graph_.nodes.push_back(std::move(c));
    }
    const std::size_t origLinks = graph_.links.size();
    for (std::size_t i = 0; i < origLinks; ++i)
    {
        const auto fIt = idMap.find(graph_.links[i].fromNode);
        const auto tIt = idMap.find(graph_.links[i].toNode);
        if (fIt != idMap.end() && tIt != idMap.end())
        {
            graph_.links.push_back({fIt->second, graph_.links[i].fromPort, tIt->second, graph_.links[i].toPort});
        }
    }
    selectedLink_.reset();
    CommitLiveEdit();
    return true;
}

bool EditorShaderGraphController::GraphUsesTime() const noexcept
{
    for (const auto& n : graph_.nodes)
    {
        if (n.type == "Time" || n.type == "WaveRing" || n.type == "VertexWindWave") return true;
        if (n.type == "Panner" && !n.inputDefaults.contains("Time"))
        {
            const bool timeConnected = std::any_of(graph_.links.begin(), graph_.links.end(), [&](const ShaderGraphLink& l) {
                return l.toNode == n.id && l.toPort == "Time";
            });
            if (!timeConnected) return true;
        }
    }
    return false;
}

void EditorShaderGraphController::TickPreview(double timeSeconds)
{
    if (!loaded_) return;
    const bool animated = GraphUsesTime();
    if (!previewDirty_)
    {
        if (!animated) return;
        if (lastPreviewTickSeconds_ >= 0.0 && (timeSeconds - lastPreviewTickSeconds_) < (1.0 / 30.0)) return;
    }
    lastPreviewTickSeconds_ = timeSeconds;
    RebuildPreview(static_cast<float>(timeSeconds));
}

void EditorShaderGraphController::RebuildPreview(float timeSeconds)
{
    previewDirty_ = false;
    preview_ = RenderShaderGraphPreviewSphere(graph_, timeSeconds, lightYaw_, lightPitch_, {}, 112);
    ++previewRevision_;
}

ShaderGraphNode* EditorShaderGraphController::FindNode(std::uint32_t id) noexcept
{
    for (auto& n : graph_.nodes)
    {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const ShaderGraphNode* EditorShaderGraphController::FindNode(std::uint32_t id) const noexcept
{
    for (const auto& n : graph_.nodes)
    {
        if (n.id == id) return &n;
    }
    return nullptr;
}

void EditorShaderGraphController::HandleAction(std::string_view action, std::string_view payload)
{
    if (!loaded_) return;

    if (action == "pan_zoom")
    {
        float vals[3]{};
        std::size_t n = 0;
        if (ParseFloats(payload, vals, 3, n) && n >= 3)
        {
            graph_.panX = vals[0];
            graph_.panY = vals[1];
            graph_.zoom = std::clamp(vals[2], 0.35f, 2.0f);
            SyncShellView();
        }
        return;
    }
    if (action == "select")
    {
        selectedNodes_.clear();
        selectedLink_.reset();
        if (payload.starts_with("link:"))
        {
            const std::size_t idx = static_cast<std::size_t>(std::strtoull(std::string(payload.substr(5)).c_str(), nullptr, 10));
            if (idx < graph_.links.size()) selectedLink_ = idx;
        }
        else if (!payload.empty())
        {
            for (const auto& part : SplitBy(payload, ','))
            {
                if (part.empty()) continue;
                const auto nid = static_cast<std::uint32_t>(std::strtoul(part.c_str(), nullptr, 10));
                if (FindNode(nid) && std::find(selectedNodes_.begin(), selectedNodes_.end(), nid) == selectedNodes_.end())
                    selectedNodes_.push_back(nid);
            }
        }
        SyncShellView();
        return;
    }
    if (action == "move_begin")
    {
        preMoveDoc_ = graph_;
        return;
    }
    if (action == "move_drag")
    {
        float vals[2]{};
        std::size_t n = 0;
        if (ParseFloats(payload, vals, 2, n) && n >= 2)
        {
            for (std::uint32_t nid : selectedNodes_)
            {
                if (auto* node = FindNode(nid))
                {
                    node->x += vals[0];
                    node->y += vals[1];
                }
            }
            SyncShellView();
        }
        return;
    }
    if (action == "move_commit")
    {
        if (preMoveDoc_.has_value())
        {
            if (undoStack_.size() >= kMaxShaderGraphUndo) undoStack_.erase(undoStack_.begin());
            undoStack_.push_back(std::move(*preMoveDoc_));
            redoStack_.clear();
            preMoveDoc_.reset();
        }
        (void)Save(false);
        return;
    }
    if (action == "move_cancel")
    {
        preMoveDoc_.reset();
        return;
    }
    if (action == "disconnect_input")
    {
        const auto parts = SplitBy(payload, '|');
        if (parts.size() >= 2)
        {
            const auto toNode = static_cast<std::uint32_t>(std::strtoul(parts[0].c_str(), nullptr, 10));
            const std::string& toPort = parts[1];
            const auto before = graph_.links.size();
            PushUndoSnapshot();
            graph_.links.erase(
                std::remove_if(graph_.links.begin(), graph_.links.end(), [&](const ShaderGraphLink& l) {
                    return l.toNode == toNode && l.toPort == toPort;
                }),
                graph_.links.end());
            if (graph_.links.size() != before) CommitLiveEdit();
            else undoStack_.pop_back();
        }
        return;
    }
    if (action == "connect")
    {
        const auto parts = SplitBy(payload, '|');
        if (parts.size() >= 4)
        {
            const auto fromNode = static_cast<std::uint32_t>(std::strtoul(parts[0].c_str(), nullptr, 10));
            const std::string& fromPort = parts[1];
            const auto toNode = static_cast<std::uint32_t>(std::strtoul(parts[2].c_str(), nullptr, 10));
            const std::string& toPort = parts[3];
            if (fromNode != 0 && toNode != 0 && fromNode != toNode)
            {
                ShaderGraphDocument candidate = graph_;
                candidate.links.erase(
                    std::remove_if(candidate.links.begin(), candidate.links.end(), [&](const ShaderGraphLink& l) {
                        return l.toNode == toNode && l.toPort == toPort;
                    }),
                    candidate.links.end());
                candidate.links.push_back({fromNode, fromPort, toNode, toPort});
                std::string hlsl, compileErr;
                if (CompileShaderGraphToHlsl(candidate, hlsl, compileErr))
                {
                    PushUndoSnapshot();
                    graph_ = std::move(candidate);
                    statusText_.clear();
                    CommitLiveEdit();
                }
                else
                {
                    statusText_ = compileErr;
                    SyncShellView();
                }
            }
        }
        return;
    }
    if (action == "add_node")
    {
        const auto parts = SplitBy(payload, '|');
        if (parts.size() >= 2)
        {
            const std::string& type = parts[0];
            const auto* spec = FindShaderNodeSpec(type);
            if (!spec || spec->isMaster) return;
            float coords[2]{120.0f, 120.0f};
            std::size_t cn = 0;
            (void)ParseFloats(parts[1], coords, 2, cn);

            PushUndoSnapshot();
            ShaderGraphNode node;
            node.id = std::max<std::uint32_t>(1u, graph_.nextNodeId++);
            node.type = spec->type;
            node.x = coords[0];
            node.y = coords[1];
            if (node.type == "Color") node.constantValue = {0.85f, 0.35f, 0.20f, 1.0f};
            else if (node.type == "Boolean") node.constantValue = {1.0f, 0.0f, 0.0f, 1.0f};
            else if (node.type == "Float" || node.type == "Range") node.constantValue = {0.5f, 0.0f, 0.0f, 1.0f};
            else if (node.type == "Vector2") node.constantValue = {1.0f, 1.0f, 0.0f, 1.0f};
            else if (node.type == "Vector3") node.constantValue = {0.0f, 1.0f, 0.0f, 1.0f};
            else if (node.type == "Vector4") node.constantValue = {1.0f, 1.0f, 1.0f, 1.0f};
            if (spec->canExposeParam)
            {
                node.paramName = spec->type + "_" + std::to_string(node.id);
                node.paramLabel = node.paramName;
            }
            const std::uint32_t newId = node.id;
            graph_.nodes.push_back(std::move(node));

            // Auto-wire if spawned by dropping a wire on empty canvas!
            if (parts.size() >= 5)
            {
                const std::string& dir = parts[2];
                const auto otherNode = static_cast<std::uint32_t>(std::strtoul(parts[3].c_str(), nullptr, 10));
                const std::string& otherPort = parts[4];
                if (dir == "from_out" && !spec->inputs.empty())
                {
                    graph_.links.erase(
                        std::remove_if(graph_.links.begin(), graph_.links.end(), [&](const ShaderGraphLink& l) {
                            return l.toNode == newId && l.toPort == spec->inputs.front().id;
                        }),
                        graph_.links.end());
                    graph_.links.push_back({otherNode, otherPort, newId, spec->inputs.front().id});
                }
                else if (dir == "from_in" && !spec->outputs.empty())
                {
                    graph_.links.erase(
                        std::remove_if(graph_.links.begin(), graph_.links.end(), [&](const ShaderGraphLink& l) {
                            return l.toNode == otherNode && l.toPort == otherPort;
                        }),
                        graph_.links.end());
                    graph_.links.push_back({newId, spec->outputs.front().id, otherNode, otherPort});
                }
            }
            selectedNodes_ = {newId};
            selectedLink_.reset();
            CommitLiveEdit();
        }
        return;
    }
    if (action == "cycle_shading")
    {
        PushUndoSnapshot();
        switch (graph_.shadingModel)
        {
        case SurfaceShadingModel::Lit: graph_.shadingModel = SurfaceShadingModel::Toon; break;
        case SurfaceShadingModel::Toon: graph_.shadingModel = SurfaceShadingModel::Unlit; break;
        case SurfaceShadingModel::Unlit: graph_.shadingModel = SurfaceShadingModel::Lit; break;
        }
        CommitLiveEdit();
        return;
    }
    if (action == "cycle_blend")
    {
        PushUndoSnapshot();
        switch (graph_.alphaMode)
        {
        case MaterialAlphaMode::Opaque: graph_.alphaMode = MaterialAlphaMode::Mask; break;
        case MaterialAlphaMode::Mask: graph_.alphaMode = MaterialAlphaMode::Blend; break;
        case MaterialAlphaMode::Blend: graph_.alphaMode = MaterialAlphaMode::Opaque; break;
        }
        CommitLiveEdit();
        return;
    }
    if (action == "toggle_doublesided")
    {
        PushUndoSnapshot();
        graph_.doubleSided = !graph_.doubleSided;
        CommitLiveEdit();
        return;
    }
    if (action == "toggle_expose")
    {
        if (selectedNodes_.empty()) return;
        if (auto* node = FindNode(selectedNodes_.back()))
        {
            const auto* spec = FindShaderNodeSpec(node->type);
            if (spec && spec->canExposeParam)
            {
                ShaderGraphDocument candidate = graph_;
                for (auto& cn : candidate.nodes)
                {
                    if (cn.id == node->id)
                    {
                        cn.exposeAsParam = !cn.exposeAsParam;
                        if (cn.paramName.empty())
                        {
                            cn.paramName = cn.type + "_" + std::to_string(cn.id);
                            cn.paramLabel = cn.paramName;
                        }
                        break;
                    }
                }
                std::string hlsl, compileErr;
                if (!CompileShaderGraphToHlsl(candidate, hlsl, compileErr))
                {
                    statusText_ = compileErr;
                    SyncShellView();
                    return;
                }
                PushUndoSnapshot();
                graph_ = std::move(candidate);
                statusText_.clear();
                CommitLiveEdit();
            }
        }
        return;
    }
    if (action == "set_param_name")
    {
        if (selectedNodes_.empty()) return;
        if (auto* node = FindNode(selectedNodes_.back()))
        {
            const std::string sanitized = SanitizeParamIdentifier(payload, node->type + "_" + std::to_string(node->id));
            if (node->paramName == sanitized)
            {
                SyncShellView();
                return;
            }
            PushUndoSnapshot();
            node->paramName = sanitized;
            node->paramLabel = node->paramName;
            CommitLiveEdit();
        }
        return;
    }
    if (action == "set_node_value")
    {
        if (selectedNodes_.empty()) return;
        if (auto* node = FindNode(selectedNodes_.back()))
        {
            const std::string prevFormatted = NodeHasInlineConstant(node->type)
                ? FormatNodeValue(*node)
                : FormatEditableInputs(graph_, *node);
            if (prevFormatted == payload)
            {
                SyncShellView();
                return;
            }
            float vals[4]{0.0f, 0.0f, 0.0f, 1.0f};
            std::size_t count = 0;
            if (!ParseFloats(payload, vals, 4, count)) { SyncShellView(); return; }
            PushUndoSnapshot();
            if (node->type == "Float")
            {
                node->constantValue.x = vals[0];
            }
            else if (node->type == "Range")
            {
                node->constantValue.x = vals[0];
                if (count >= 3 && vals[2] > vals[1])
                {
                    node->rangeMin = vals[1];
                    node->rangeMax = vals[2];
                }
                node->constantValue.x = std::clamp(node->constantValue.x, node->rangeMin, node->rangeMax);
            }
            else if (node->type == "Boolean")
            {
                node->constantValue.x = vals[0] >= 0.5f ? 1.0f : 0.0f;
            }
            else if (node->type == "Vector2")
            {
                node->constantValue.x = vals[0];
                if (count >= 2) node->constantValue.y = vals[1];
            }
            else if (node->type == "Vector3")
            {
                node->constantValue.x = vals[0];
                if (count >= 2) node->constantValue.y = vals[1];
                if (count >= 3) node->constantValue.z = vals[2];
            }
            else if (node->type == "Color" || node->type == "Vector4")
            {
                node->constantValue.x = vals[0];
                if (count >= 2) node->constantValue.y = vals[1];
                if (count >= 3) node->constantValue.z = vals[2];
                node->constantValue.w = (count >= 4) ? vals[3] : 1.0f;
            }
            else
            {
                const auto ports = CollectEditableUnconnectedInputs(graph_, *node);
                if (!ports.empty())
                {
                    if (ports[0]->type != ShaderPortValueType::Float)
                    {
                        Vec4 cur = node->inputDefaults.contains(ports[0]->id)
                            ? node->inputDefaults[ports[0]->id]
                            : ports[0]->defaultValue;
                        cur.x = vals[0];
                        if (count >= 2) cur.y = vals[1];
                        if (count >= 3) cur.z = vals[2];
                        if (count >= 4) cur.w = vals[3];
                        node->inputDefaults[ports[0]->id] = cur;
                    }
                    else
                    {
                        for (std::size_t k = 0; k < std::min(count, ports.size()); ++k)
                        {
                            Vec4 cur = node->inputDefaults.contains(ports[k]->id)
                                ? node->inputDefaults[ports[k]->id]
                                : ports[k]->defaultValue;
                            cur.x = vals[k];
                            node->inputDefaults[ports[k]->id] = cur;
                        }
                    }
                }
            }
            CommitLiveEdit();
        }
        return;
    }
    if (action == "frame_all")
    {
        if (!graph_.nodes.empty())
        {
            float minX = graph_.nodes.front().x, maxX = minX + 180.0f;
            float minY = graph_.nodes.front().y, maxY = minY + 120.0f;
            for (const auto& n : graph_.nodes)
            {
                minX = std::min(minX, n.x);
                minY = std::min(minY, n.y);
                maxX = std::max(maxX, n.x + (n.type == "MasterSurface" ? 216.0f : 176.0f));
                maxY = std::max(maxY, n.y + 130.0f);
            }
            float cw = 760.0f, ch = 360.0f;
            if (shell_ && shell_->ShaderGraphCanvas().IsValid())
            {
                const auto cr = shell_->Layout().Rect(shell_->ShaderGraphCanvas());
                if (cr.width > 40.0f && cr.height > 40.0f) { cw = cr.width; ch = cr.height; }
            }
            const float bw = std::max(120.0f, maxX - minX + 80.0f);
            const float bh = std::max(100.0f, maxY - minY + 80.0f);
            graph_.zoom = std::clamp(std::min(cw / bw, ch / bh), 0.45f, 1.25f);
            graph_.panX = (cw - (maxX - minX) * graph_.zoom) * 0.5f - minX * graph_.zoom;
            graph_.panY = (ch - (maxY - minY) * graph_.zoom) * 0.5f - minY * graph_.zoom;
            SyncShellView();
        }
        return;
    }
    if (action == "orbit_preview")
    {
        float vals[2]{};
        std::size_t n = 0;
        if (ParseFloats(payload, vals, 2, n) && n >= 2)
        {
            lightYaw_ += vals[0];
            lightPitch_ = std::clamp(lightPitch_ + vals[1], -1.35f, 1.35f);
            previewDirty_ = true;
        }
        return;
    }
    if (action == "save")
    {
        if (Save() && shell_)
        {
            shell_->SetStatus(text_.GetText("EditorUi.Saved") + ": " + asset_io::Utf8(path_.filename()));
        }
        return;
    }
    if (action == "view_hlsl")
    {
        std::string hlsl, err;
        if (!CompileShaderGraphToHlsl(graph_, hlsl, err))
        {
            statusText_ = err;
            SyncShellView();
            return;
        }
        std::error_code ec;
        const std::filesystem::path outDir = cacheRoot_ / "shader-graph";
        std::filesystem::create_directories(outDir, ec);
        const std::filesystem::path outPath = outDir / asset_io::FromUtf8(ShaderDisplayStem(path_) + ".generated.hlsl");
        {
            std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
            out.write(hlsl.data(), static_cast<std::streamsize>(hlsl.size()));
        }
        if (onOpenHlsl_) onOpenHlsl_(outPath);
        return;
    }
    if (action == "create_material")
    {
        (void)Save();
        std::filesystem::path matPath;
        std::string err;
        if (CreateCompanionShaderMaterial(projectRoot_, assets_, session_, path_, matPath, err))
        {
            if (onMaterialCreated_) onMaterialCreated_(matPath);
        }
        else if (shell_)
        {
            shell_->SetStatus(text_.GetText(err.empty() ? "Assets.WriteFailed" : err));
        }
        return;
    }
}

void EditorShaderGraphController::SyncShellView()
{
    if (!shell_) return;
    ui::editor::EditorShaderGraphViewState v;
    v.loaded = loaded_;
    v.dirty = dirty_;
    if (!loaded_)
    {
        shell_->SetShaderGraphView(std::move(v));
        return;
    }
    v.title = ShaderDisplayStem(path_);
    v.shadingMode = graph_.shadingModel == SurfaceShadingModel::Toon ? "Toon"
                  : graph_.shadingModel == SurfaceShadingModel::Unlit ? "Unlit"
                                                                      : "Lit";
    v.blendMode = std::string(MaterialAlphaModeName(graph_.alphaMode));
    v.doubleSided = graph_.doubleSided;
    v.panX = graph_.panX;
    v.panY = graph_.panY;
    v.zoom = graph_.zoom;
    v.previewImageKey = "shader-graph-preview";
    v.statusText = statusText_;

    if (!selectedNodes_.empty())
    {
        if (const auto* pri = FindNode(selectedNodes_.back()))
        {
            const auto* spec = FindShaderNodeSpec(pri->type);
            v.primaryNodeId = pri->id;
            v.canExpose = spec && spec->canExposeParam;
            v.primaryExposed = pri->exposeAsParam;
            v.primaryParamName = pri->paramName.empty() ? (pri->type + "_" + std::to_string(pri->id)) : pri->paramName;
            if (NodeHasInlineConstant(pri->type))
            {
                v.primaryHasValue = true;
                v.primaryValueText = FormatNodeValue(*pri);
            }
            else
            {
                v.primaryValueText = FormatEditableInputs(graph_, *pri);
                v.primaryHasValue = !v.primaryValueText.empty();
            }
        }
    }

    for (const auto& n : graph_.nodes)
    {
        const auto* spec = FindShaderNodeSpec(n.type);
        if (!spec) continue;
        ui::editor::EditorShaderNodeView nv;
        nv.id = n.id;
        nv.type = n.type;
        nv.category = spec->category;
        nv.title = (n.exposeAsParam && !n.paramName.empty()) ? (spec->title + " [" + n.paramName + "]") : spec->title;
        nv.x = n.x;
        nv.y = n.y;
        nv.selected = std::find(selectedNodes_.begin(), selectedNodes_.end(), n.id) != selectedNodes_.end();
        nv.isMaster = spec->isMaster;
        nv.exposed = n.exposeAsParam;
        if (n.type == "Color")
        {
            nv.swatchRgba[0] = std::clamp(n.constantValue.x, 0.0f, 1.0f);
            nv.swatchRgba[1] = std::clamp(n.constantValue.y, 0.0f, 1.0f);
            nv.swatchRgba[2] = std::clamp(n.constantValue.z, 0.0f, 1.0f);
            nv.swatchRgba[3] = 1.0f;
        }
        else if (NodeHasInlineConstant(n.type))
        {
            nv.subtitle = FormatNodeValue(n);
        }
        for (const auto& inSpec : spec->inputs)
        {
            const bool conn = std::any_of(graph_.links.begin(), graph_.links.end(), [&](const ShaderGraphLink& l) {
                return l.toNode == n.id && l.toPort == inSpec.id;
            });
            nv.inputs.push_back({inSpec.id, inSpec.label, static_cast<std::uint8_t>(inSpec.type), conn});
        }
        for (const auto& outSpec : spec->outputs)
        {
            const bool conn = std::any_of(graph_.links.begin(), graph_.links.end(), [&](const ShaderGraphLink& l) {
                return l.fromNode == n.id && l.fromPort == outSpec.id;
            });
            nv.outputs.push_back({outSpec.id, outSpec.label, static_cast<std::uint8_t>(outSpec.type), conn});
        }
        v.nodes.push_back(std::move(nv));
    }

    for (std::size_t li = 0; li < graph_.links.size(); ++li)
    {
        const auto& lnk = graph_.links[li];
        std::uint8_t vt = 0;
        if (const auto* fn = FindNode(lnk.fromNode))
        {
            if (const auto* fspec = FindShaderNodeSpec(fn->type))
            {
                for (const auto& o : fspec->outputs)
                {
                    if (o.id == lnk.fromPort) { vt = static_cast<std::uint8_t>(o.type); break; }
                }
            }
        }
        v.links.push_back({lnk.fromNode, lnk.fromPort, lnk.toNode, lnk.toPort, vt,
                           selectedLink_.has_value() && *selectedLink_ == li});
    }

    shell_->SetShaderGraphView(std::move(v));
}
} // namespace flare
