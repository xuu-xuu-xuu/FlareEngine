#include "PrefabWorkflow.h"
#include "EditorApplication.h"
#include "EditorAssetActions.h"
#include "FunctionLayer/Runtime/ComponentMetadata.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Scenes/PrefabDocument.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace se
{
namespace
{
using Json = nlohmann::json;

bool FloatEqual(float a, float b) noexcept
{
    return std::abs(a - b) <= 1e-5f;
}

bool PropertyValuesEqual(const PropertyValue& a, const PropertyValue& b)
{
    if (a.index() != b.index()) return false;
    if (const auto* bv = std::get_if<bool>(&a)) return *bv == std::get<bool>(b);
    if (const auto* fv = std::get_if<float>(&a)) return FloatEqual(*fv, std::get<float>(b));
    if (const auto* v3 = std::get_if<Vec3>(&a))
    {
        const Vec3& o = std::get<Vec3>(b);
        return FloatEqual(v3->x, o.x) && FloatEqual(v3->y, o.y) && FloatEqual(v3->z, o.z);
    }
    if (const auto* q = std::get_if<Quat>(&a))
    {
        const Quat& o = std::get<Quat>(b);
        return std::abs(q->x * o.x + q->y * o.y + q->z * o.z + q->w * o.w) >= 1.0f - 1e-5f;
    }
    if (const auto* v4 = std::get_if<Vec4>(&a))
    {
        const Vec4& o = std::get<Vec4>(b);
        return FloatEqual(v4->x, o.x) && FloatEqual(v4->y, o.y) && FloatEqual(v4->z, o.z) && FloatEqual(v4->w, o.w);
    }
    if (const auto* sv = std::get_if<std::string>(&a)) return *sv == std::get<std::string>(b);
    if (const auto* lv = std::get_if<std::vector<std::string>>(&a)) return *lv == std::get<std::vector<std::string>>(b);
    if (const auto* rv = std::get_if<PropertyEntityReference>(&a))
    {
        return rv->stableId == std::get<PropertyEntityReference>(b).stableId;
    }
    return false;
}

std::string SanitizePrefabFileStem(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (unsigned char ch : raw)
    {
        if (ch < 32 || ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|')
        {
            out.push_back('_');
        }
        else
        {
            out.push_back(static_cast<char>(ch));
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    if (out.empty()) out = "NewPrefab";
    return out;
}

std::vector<StableEntityId> CollectPrefabInstanceSubtreeIds(
    const World& world,
    StableEntityId instanceRootId,
    const std::string& prefabId)
{
    std::vector<StableEntityId> result;
    const EntityHandle root = world.FindByStableId(instanceRootId);
    if (!root) return result;
    const PrefabLink* rootLink = world.GetPrefabLink(root);
    if (!rootLink || rootLink->prefabId != prefabId) return result;

    result.push_back(instanceRootId);
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        const StableEntityId parentId = result[i];
        std::vector<StableEntityId> children;
        for (const EntityHandle candidate : world.Entities())
        {
            const EntityRecord* rec = world.Get(candidate);
            if (!rec || !rec->parent || world.Get(rec->parent)->stableId != parentId) continue;
            const PrefabLink* childLink = world.GetPrefabLink(candidate);
            if (childLink && childLink->prefabId == prefabId)
            {
                children.push_back(rec->stableId);
            }
        }
        std::sort(children.begin(), children.end());
        result.insert(result.end(), children.begin(), children.end());
    }
    return result;
}

std::vector<StableEntityId> CollectRawEntitySubtreeIds(const World& world, StableEntityId rootEntityId)
{
    std::vector<StableEntityId> result;
    if (!world.FindByStableId(rootEntityId)) return result;
    result.push_back(rootEntityId);
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        const StableEntityId parentId = result[i];
        std::vector<StableEntityId> children;
        for (const EntityHandle candidate : world.Entities())
        {
            const EntityRecord* rec = world.Get(candidate);
            if (rec && rec->parent && world.Get(rec->parent)->stableId == parentId)
            {
                children.push_back(rec->stableId);
            }
        }
        std::sort(children.begin(), children.end());
        result.insert(result.end(), children.begin(), children.end());
    }
    return result;
}

bool ReadRawFileBytes(const std::filesystem::path& path, std::string& outBytes)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    outBytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

bool WriteRawFileBytes(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    return file.good();
}

std::vector<StableEntityId> CollectAllPrefabEntityIds(const World& world, const AssetId& prefabId)
{
    std::vector<StableEntityId> ids;
    for (const EntityHandle handle : world.Entities())
    {
        const PrefabLink* link = world.GetPrefabLink(handle);
        if (!link || link->prefabId != prefabId) continue;
        if (const EntityRecord* rec = world.Get(handle))
        {
            ids.push_back(rec->stableId);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

bool ParseEntityScriptsJson(const std::string& jsonText, std::vector<PrefabScriptRecord>& out)
{
    out.clear();
    try
    {
        const Json data = Json::parse(jsonText);
        if (!data.contains("Scripts") || !data["Scripts"].is_array()) return false;
        for (const Json& scriptJson : data["Scripts"])
        {
            if (scriptJson.value("Missing", false)) continue;
            PrefabScriptRecord record;
            record.type = scriptJson.value("Type", std::string{});
            record.enabled = scriptJson.value("Enabled", true);
            if (record.type.empty()) continue;
            if (scriptJson.contains("Fields") && scriptJson["Fields"].is_array())
            {
                for (const Json& fieldJson : scriptJson["Fields"])
                {
                    PrefabScriptFieldRecord field;
                    field.name = fieldJson.value("Name", std::string{});
                    field.kind = fieldJson.value("Kind", std::string{});
                    field.valueJson = fieldJson.contains("Value") ? fieldJson["Value"].dump() : "null";
                    if (!field.name.empty() && !field.kind.empty())
                    {
                        record.fields.push_back(std::move(field));
                    }
                }
            }
            out.push_back(std::move(record));
        }
        return true;
    }
    catch (const Json::exception&)
    {
        return false;
    }
}

bool TryQueryEntityScripts(StableEntityId entityId,
                           EditorApplicationHooks* hooks,
                           std::vector<PrefabScriptRecord>& out)
{
    out.clear();
    if (!hooks || !hooks->scriptQuery) return false;
    std::string jsonText, ignoredError;
    if (!hooks->scriptQuery(entityId, jsonText, ignoredError)) return false;
    return ParseEntityScriptsJson(jsonText, out);
}

std::vector<InstantiatedPrefabScript> QueryScriptsForEntities(
    const std::vector<StableEntityId>& entities,
    EditorApplicationHooks* hooks)
{
    std::vector<InstantiatedPrefabScript> out;
    if (!hooks || !hooks->scriptQuery) return out;
    for (const StableEntityId entityId : entities)
    {
        std::vector<PrefabScriptRecord> entityScripts;
        if (!TryQueryEntityScripts(entityId, hooks, entityScripts)) continue;
        for (PrefabScriptRecord& record : entityScripts)
        {
            out.push_back({entityId, std::move(record)});
        }
    }
    return out;
}

const PrefabScriptRecord* FindScriptByType(const std::vector<PrefabScriptRecord>& list, std::string_view type) noexcept
{
    for (const PrefabScriptRecord& item : list)
    {
        if (item.type == type) return &item;
    }
    return nullptr;
}

bool ScriptFieldValuesMatch(const PrefabScriptRecord* existing, const PrefabScriptFieldRecord& desiredField,
                            const Json& desiredVal)
{
    if (!existing) return false;
    for (const PrefabScriptFieldRecord& field : existing->fields)
    {
        if (field.name == desiredField.name && field.kind == desiredField.kind)
        {
            const Json existingVal = Json::parse(field.valueJson, nullptr, false);
            return !existingVal.is_discarded() && existingVal == desiredVal;
        }
    }
    return false;
}

bool ApplyScriptRecordViaHook(StableEntityId entityId,
                              const PrefabScriptRecord& desired,
                              const PrefabScriptRecord* existing,
                              bool isFreshEntity,
                              EditorApplicationHooks* hooks,
                              std::string& error)
{
    if (!hooks || !hooks->scriptApply) return true;
    if (!existing)
    {
        const Json addReq{{"Type", desired.type}, {"Add", true}};
        if (!hooks->scriptApply(entityId, addReq.dump(), error)) return false;
    }

    const bool needEnabledUpdate = isFreshEntity
        ? !desired.enabled
        : (!existing || existing->enabled != desired.enabled);
    if (needEnabledUpdate)
    {
        const Json enabledReq{{"Type", desired.type}, {"Enabled", desired.enabled}};
        if (!hooks->scriptApply(entityId, enabledReq.dump(), error)) return false;
    }

    for (const PrefabScriptFieldRecord& field : desired.fields)
    {
        Json valueNode = Json::parse(field.valueJson, nullptr, false);
        if (valueNode.is_discarded()) continue;
        if (ScriptFieldValuesMatch(existing, field, valueNode)) continue;
        const Json fieldReq{{"Type", desired.type}, {"Field", field.name}, {"Value", std::move(valueNode)}};
        if (!hooks->scriptApply(entityId, fieldReq.dump(), error)) return false;
    }
    return true;
}

bool ApplyScriptsViaHooks(const std::vector<InstantiatedPrefabScript>& scripts,
                          EditorApplicationHooks* hooks,
                          std::string& error)
{
    if (scripts.empty() || !hooks || !hooks->scriptApply) return true;
    for (const InstantiatedPrefabScript& entry : scripts)
    {
        if (!ApplyScriptRecordViaHook(entry.entity, entry.script, nullptr, true, hooks, error))
        {
            return false;
        }
    }
    return true;
}

bool SyncScriptsViaHooks(const std::vector<StableEntityId>& preSyncEntities,
                         const std::vector<StableEntityId>& postSyncEntities,
                         const std::vector<InstantiatedPrefabScript>& desiredScripts,
                         EditorApplicationHooks* hooks,
                         std::string& error)
{
    if (!hooks || !hooks->scriptApply) return true;

    std::unordered_map<StableEntityId, std::vector<PrefabScriptRecord>> desiredByEntity;
    for (const InstantiatedPrefabScript& entry : desiredScripts)
    {
        desiredByEntity[entry.entity].push_back(entry.script);
    }

    std::vector<StableEntityId> allEntities;
    allEntities.reserve(preSyncEntities.size() + postSyncEntities.size() + desiredByEntity.size());
    allEntities.insert(allEntities.end(), preSyncEntities.begin(), preSyncEntities.end());
    allEntities.insert(allEntities.end(), postSyncEntities.begin(), postSyncEntities.end());
    for (const auto& [entityId, _] : desiredByEntity)
    {
        (void)_;
        allEntities.push_back(entityId);
    }
    std::sort(allEntities.begin(), allEntities.end());
    allEntities.erase(std::unique(allEntities.begin(), allEntities.end()), allEntities.end());

    const std::vector<PrefabScriptRecord> emptyScripts;
    for (const StableEntityId entityId : allEntities)
    {
        const auto desiredIt = desiredByEntity.find(entityId);
        const std::vector<PrefabScriptRecord>& desiredList =
            desiredIt != desiredByEntity.end() ? desiredIt->second : emptyScripts;

        std::vector<PrefabScriptRecord> existingList;
        const bool hasSnapshot = TryQueryEntityScripts(entityId, hooks, existingList);
        if (hasSnapshot)
        {
            for (const PrefabScriptRecord& existing : existingList)
            {
                if (!FindScriptByType(desiredList, existing.type))
                {
                    const Json removeReq{{"Type", existing.type}, {"Remove", true}};
                    if (!hooks->scriptApply(entityId, removeReq.dump(), error)) return false;
                }
            }
        }

        for (const PrefabScriptRecord& desired : desiredList)
        {
            const PrefabScriptRecord* existing = hasSnapshot ? FindScriptByType(existingList, desired.type) : nullptr;
            if (!ApplyScriptRecordViaHook(entityId, desired, existing, false, hooks, error))
            {
                return false;
            }
        }
    }
    return true;
}

std::unordered_map<std::uint64_t, StableEntityId> BuildInstanceNodeMap(
    const World& world,
    StableEntityId rootId,
    const std::string& prefabId)
{
    std::unordered_map<std::uint64_t, StableEntityId> map;
    for (const StableEntityId id : CollectPrefabInstanceSubtreeIds(world, rootId, prefabId))
    {
        if (const PrefabLink* link = world.GetPrefabLink(world.FindByStableId(id)))
        {
            map.emplace(link->localNodeId, id);
        }
    }
    return map;
}

const PrefabNodeRecord* FindPrefabNode(const PrefabDocument& document, std::uint64_t localNodeId) noexcept
{
    for (const PrefabNodeRecord& node : document.nodes)
    {
        if (node.localNodeId == localNodeId) return &node;
    }
    return nullptr;
}
} // namespace

std::string CanonicalPrefabPropertyKey(std::string_view key)
{
    if (key == "position.x" || key == "position.y" || key == "position.z") return "transform.position";
    if (key == "rotation.x" || key == "rotation.y" || key == "rotation.z") return "transform.rotation";
    if (key == "scale.x" || key == "scale.y" || key == "scale.z") return "transform.scale";
    if (key == "light.color.r" || key == "light.color.g" || key == "light.color.b") return "light.color";
    if (key == "light.type") return "light.kind";
    if (key == "mesh.material" || key.starts_with("mesh.material.") || key.starts_with("mesh.slot."))
    {
        return "mesh.materials";
    }
    if (key.size() > 2 && key[key.size() - 2] == '.' &&
        (key.back() == 'x' || key.back() == 'y' || key.back() == 'z'))
    {
        const std::string_view base = key.substr(0, key.size() - 2);
        if (const PropertyDescriptor* desc = FindProperty(base); desc && desc->type == PropertyType::Vector3)
        {
            return std::string(base);
        }
    }
    return std::string(key);
}

StableEntityId FindEnclosingPrefabRoot(const World& world, StableEntityId entityId)
{
    EntityHandle cursor = world.FindByStableId(entityId);
    const PrefabLink* link = world.GetPrefabLink(cursor);
    if (!link) return 0;

    const std::string prefabId = link->prefabId;
    while (cursor)
    {
        const EntityRecord* rec = world.Get(cursor);
        if (!rec || !rec->parent) break;
        const PrefabLink* parentLink = world.GetPrefabLink(rec->parent);
        if (!parentLink || parentLink->prefabId != prefabId) break;
        cursor = rec->parent;
    }
    return cursor ? world.Get(cursor)->stableId : 0;
}

Vec3 PrefabRootAuthoredScale(const PrefabDocument& document)
{
    if (const PrefabNodeRecord* root = FindPrefabNode(document, document.rootLocalNodeId))
    {
        for (const PrefabComponentRecord& comp : root->components)
        {
            if (comp.type != ComponentTypes::Transform) continue;
            for (const PrefabPropertyRecord& prop : comp.properties)
            {
                if (prop.key == "scale")
                {
                    if (const auto* s = std::get_if<Vec3>(&prop.value)) return *s;
                }
            }
        }
    }
    return {1.0f, 1.0f, 1.0f};
}

void RecordOverrideAfterPropertyEdit(World& world, AssetStore* assets, EntityHandle entity,
                                     std::string_view inspectorOrPropertyKey)
{
    const PrefabLink* currentLink = world.GetPrefabLink(entity);
    if (!currentLink) return;

    const std::string key = CanonicalPrefabPropertyKey(inspectorOrPropertyKey);
    const EntityRecord* rec = world.Get(entity);
    const PrefabLink* parentLink = (rec && rec->parent) ? world.GetPrefabLink(rec->parent) : nullptr;
    const bool isRoot = !parentLink || parentLink->prefabId != currentLink->prefabId;
    if (isRoot && (key == "transform.position" || key == "transform.rotation"))
    {
        return;
    }

    PropertyValue currentValue;
    std::string error;
    if (!ReadEntityOverrideValue(world, entity, key, currentValue, error)) return;

    PrefabLink updated = *currentLink;
    auto eraseKey = [&]() {
        updated.overrides.erase(
            std::remove_if(updated.overrides.begin(), updated.overrides.end(),
                           [&](const PropertyOverride& item) { return item.key == key; }),
            updated.overrides.end());
    };

    bool matchesPrototype = false;
    if (assets)
    {
        const std::filesystem::path docPath = assets->Resolve(updated.prefabId);
        PrefabDocument prototype;
        if (!docPath.empty() && ReadPrefabDocument(docPath, prototype, error))
        {
            if (const PrefabNodeRecord* node = FindPrefabNode(prototype, updated.localNodeId))
            {
                PropertyValue protoValue;
                bool isLocalRef = false;
                if (ReadPrefabNodePropertyValue(*node, key, protoValue, isLocalRef))
                {
                    if (isLocalRef)
                    {
                        const StableEntityId rootId = FindEnclosingPrefabRoot(world, rec->stableId);
                        const auto nodeToEntity = BuildInstanceNodeMap(world, rootId, updated.prefabId);
                        if (const auto* ref = std::get_if<PropertyEntityReference>(&protoValue))
                        {
                            if (const auto targetIt = nodeToEntity.find(ref->stableId);
                                targetIt != nodeToEntity.end())
                            {
                                protoValue = PropertyEntityReference{targetIt->second};
                            }
                        }
                    }
                    matchesPrototype = PropertyValuesEqual(currentValue, protoValue);
                }
            }
        }
    }

    if (matchesPrototype)
    {
        eraseKey();
    }
    else
    {
        auto it = std::find_if(updated.overrides.begin(), updated.overrides.end(),
                               [&](const PropertyOverride& item) { return item.key == key; });
        if (it != updated.overrides.end()) it->value = std::move(currentValue);
        else updated.overrides.push_back({key, std::move(currentValue)});
    }
    world.SetPrefabLink(entity, std::move(updated));
}

bool CreatePrefabFromSelection(
    EditorSession& session,
    AssetStore& assets,
    const std::filesystem::path& projectRoot,
    StableEntityId rootEntityId,
    const std::filesystem::path& requestedPath,
    std::filesystem::path& createdPath,
    AssetId& createdAssetId,
    std::string& error,
    EditorApplicationHooks* hooks)
{
    createdPath.clear();
    createdAssetId.clear();
    if (session.IsPlaying()) { error = "Editor.EditWhilePlaying"; return false; }

    const World& readWorld = session.EditWorld();
    const EntityHandle rootHandle = readWorld.FindByStableId(rootEntityId);
    const EntityRecord* rootRecord = readWorld.Get(rootHandle);
    if (!rootRecord) { error = "Prefab.MissingEntity"; return false; }

    std::filesystem::path targetPath = requestedPath;
    if (targetPath.empty())
    {
        const std::string stem = SanitizePrefabFileStem(rootRecord->name);
        for (unsigned index = 0; index < 10000; ++index)
        {
            const std::string suffix = index == 0 ? std::string{} : std::to_string(index);
            const std::filesystem::path candidate = assets.SourceRoot() / (stem + suffix + std::string(PrefabFileExtension));
            const std::filesystem::path meta = assets.SourceRoot() / (stem + suffix + std::string(PrefabMetadataExtension));
            if (!std::filesystem::exists(candidate) && !std::filesystem::exists(meta))
            {
                targetPath = candidate;
                break;
            }
        }
        if (targetPath.empty()) { error = "Assets.DestinationExists"; return false; }
    }
    else
    {
        if (!targetPath.is_absolute()) targetPath = assets.SourceRoot() / targetPath;
        if (!asset_io::Within(assets.SourceRoot(), targetPath) || !IsPrefabDocumentPath(targetPath))
        {
            error = "Assets.ReferenceOutsideRoot";
            return false;
        }
    }

    const std::vector<StableEntityId> subtreeIds = CollectRawEntitySubtreeIds(readWorld, rootEntityId);
    const std::vector<InstantiatedPrefabScript> attachedScripts = QueryScriptsForEntities(subtreeIds, hooks);

    PrefabDocument document;
    std::unordered_map<StableEntityId, std::uint64_t> entityToLocal;
    if (!BuildPrefabDocumentFromEntity(readWorld, rootHandle, document, entityToLocal, error, attachedScripts))
    {
        return false;
    }

    if (!WritePrefabDocument(targetPath, document, error, !requestedPath.empty())) return false;

    std::vector<AssetDiagnostic> diagnostics;
    if (!assets.RegisterSource(targetPath, {}, createdAssetId, diagnostics))
    {
        std::error_code ignored;
        std::filesystem::remove(targetPath, ignored);
        error = diagnostics.empty() ? "Assets.WriteFailed" : diagnostics.back().key;
        return false;
    }

    std::vector<EditorAssetIssue> syncIssues;
    if (!projectRoot.empty() && !EditorAssetActions::SyncProjectAssets(projectRoot, assets, syncIssues))
    {
        error = syncIssues.empty() ? "Assets.ProjectSyncFailed" : syncIssues.front().key;
        return false;
    }

    if (!session.Apply("Editor.CreatePrefab", [&](World& world) {
            for (const auto& [stableId, localId] : entityToLocal)
            {
                const EntityHandle entity = world.FindByStableId(stableId);
                if (!entity || !world.SetPrefabLink(entity, PrefabLink{createdAssetId, localId, {}}))
                {
                    error = world.LastError();
                    return false;
                }
            }
            return true;
        }))
    {
        if (error.empty()) error = session.LastError();
        return false;
    }

    createdPath = targetPath;
    error.clear();
    return true;
}

bool InstantiatePrefabIntoScene(
    EditorSession& session,
    AssetStore& assets,
    const AssetId& prefabId,
    const Transform& rootTransform,
    StableEntityId& createdRootId,
    std::string& error,
    EditorApplicationHooks* hooks,
    StableEntityId rootParent)
{
    createdRootId = 0;
    if (session.IsPlaying()) { error = "Editor.EditWhilePlaying"; return false; }
    if (!IsValidAssetIdString(prefabId)) { error = "Prefab.InvalidDocument"; return false; }

    const std::filesystem::path prefabPath = assets.Resolve(prefabId);
    if (prefabPath.empty()) { error = "Assets.UnknownId"; return false; }

    PrefabDocument document;
    if (!ReadPrefabDocument(prefabPath, document, error)) return false;

    if (!session.BeginEdit("Editor.InstantiatePrefab"))
    {
        error = session.LastError();
        return false;
    }

    PrefabInstanceReport report;
    const bool spawned = session.Apply("Editor.InstantiatePrefab", [&](World& world) {
        createdRootId = InstantiatePrefab(world, document, rootTransform, &error, prefabId, rootParent, &report);
        return createdRootId != 0;
    });

    if (!spawned || !ApplyScriptsViaHooks(report.scripts, hooks, error))
    {
        if (error.empty()) error = session.LastError();
        session.CancelEdit();
        createdRootId = 0;
        return false;
    }

    if (!session.Select(createdRootId) || !session.CommitEdit())
    {
        error = session.LastError();
        session.CancelEdit();
        createdRootId = 0;
        return false;
    }

    error.clear();
    return true;
}

bool RevertPropertyOverride(
    EditorSession& session,
    AssetStore& assets,
    StableEntityId entityId,
    std::string_view inspectorOrPropertyKey,
    std::string& error)
{
    if (session.IsPlaying()) { error = "Editor.EditWhilePlaying"; return false; }
    const World& readWorld = session.EditWorld();
    const EntityHandle readEntity = readWorld.FindByStableId(entityId);
    const PrefabLink* readLink = readWorld.GetPrefabLink(readEntity);
    if (!readLink) { error = "Prefab.MissingEntity"; return false; }

    const std::filesystem::path prefabPath = assets.Resolve(readLink->prefabId);
    if (prefabPath.empty()) { error = "Assets.UnknownId"; return false; }

    PrefabDocument document;
    if (!ReadPrefabDocument(prefabPath, document, error)) return false;

    const PrefabNodeRecord* protoNode = FindPrefabNode(document, readLink->localNodeId);
    if (!protoNode) { error = "Prefab.MissingNode"; return false; }

    const std::string key = CanonicalPrefabPropertyKey(inspectorOrPropertyKey);
    PropertyValue protoValue;
    bool isLocalRef = false;
    if (!ReadPrefabNodePropertyValue(*protoNode, key, protoValue, isLocalRef))
    {
        error = "Metadata.UnknownProperty";
        return false;
    }

    const StableEntityId instanceRoot = FindEnclosingPrefabRoot(readWorld, entityId);
    const auto nodeToEntity = BuildInstanceNodeMap(readWorld, instanceRoot, readLink->prefabId);
    if (isLocalRef)
    {
        const auto* ref = std::get_if<PropertyEntityReference>(&protoValue);
        const auto mappedIt = ref ? nodeToEntity.find(ref->stableId) : nodeToEntity.end();
        if (mappedIt == nodeToEntity.end()) { error = "Prefab.MissingNode"; return false; }
        protoValue = PropertyEntityReference{mappedIt->second};
    }

    const bool ok = session.Apply("Prefab.RevertOverride", [&](World& world) {
        const EntityHandle entity = world.FindByStableId(entityId);
        const PrefabLink* link = world.GetPrefabLink(entity);
        if (!entity || !link) return false;
        if (!ApplyPropertyOverride(world, entity, {key, protoValue}, error)) return false;

        PrefabLink updated = *link;
        updated.overrides.erase(
            std::remove_if(updated.overrides.begin(), updated.overrides.end(),
                           [&](const PropertyOverride& item) { return item.key == key; }),
            updated.overrides.end());
        if (!world.SetPrefabLink(entity, std::move(updated)))
        {
            error = world.LastError();
            return false;
        }
        return true;
    });
    if (!ok && error.empty()) error = session.LastError();
    return ok;
}

bool RevertAllOverrides(
    EditorSession& session,
    AssetStore& assets,
    StableEntityId entityOrRootId,
    std::string& error)
{
    if (session.IsPlaying()) { error = "Editor.EditWhilePlaying"; return false; }
    const World& readWorld = session.EditWorld();
    const StableEntityId rootId = FindEnclosingPrefabRoot(readWorld, entityOrRootId);
    if (rootId == 0) { error = "Prefab.MissingEntity"; return false; }

    const PrefabLink* rootLink = readWorld.GetPrefabLink(readWorld.FindByStableId(rootId));
    const std::string prefabId = rootLink->prefabId;
    const std::filesystem::path prefabPath = assets.Resolve(prefabId);
    if (prefabPath.empty()) { error = "Assets.UnknownId"; return false; }

    PrefabDocument document;
    if (!ReadPrefabDocument(prefabPath, document, error)) return false;

    const bool ok = session.Apply("Prefab.RevertAll", [&](World& world) {
        const std::vector<StableEntityId> subtree = CollectPrefabInstanceSubtreeIds(world, rootId, prefabId);
        if (subtree.empty()) { error = "Prefab.MissingEntity"; return false; }

        std::unordered_map<std::uint64_t, StableEntityId> nodeToEntity;
        for (const StableEntityId id : subtree)
        {
            const EntityHandle entity = world.FindByStableId(id);
            const PrefabLink* link = world.GetPrefabLink(entity);
            if (!link) continue;
            nodeToEntity.emplace(link->localNodeId, id);
            PrefabLink cleared = *link;
            cleared.overrides.clear();
            if (!world.SetPrefabLink(entity, std::move(cleared)))
            {
                error = world.LastError();
                return false;
            }
        }

        // 仅同步当前选中实例本身：临时构建含单节点子树即可复用同步逻辑。
        for (const PrefabNodeRecord& node : document.nodes)
        {
            const auto targetIt = nodeToEntity.find(node.localNodeId);
            if (targetIt == nodeToEntity.end()) continue;
            const EntityHandle entity = world.FindByStableId(targetIt->second);
            const bool isRoot = (targetIt->second == rootId);

            if (!world.Rename(entity, node.name) || !world.SetEnabled(entity, node.enabled))
            {
                error = world.LastError();
                return false;
            }
            for (const PrefabComponentRecord& comp : node.components)
            {
                const ComponentDescriptor* descriptor = FindComponentDescriptor(comp.type);
                if (!descriptor) { error = "Scene.UnknownComponent"; return false; }

                if (comp.type == ComponentTypes::Transform)
                {
                    Transform protoTransform{};
                    if (!ExtractTransformFromPrefabComponent(comp, protoTransform))
                    {
                        error = "Prefab.InvalidDocument";
                        return false;
                    }
                    const Transform* cur = world.GetTransform(entity);
                    Transform next = cur ? *cur : Transform{};
                    next.scale = protoTransform.scale;
                    if (!isRoot)
                    {
                        next.position = protoTransform.position;
                        next.rotation = protoTransform.rotation;
                    }
                    if (!world.SetTransform(entity, next)) { error = world.LastError(); return false; }
                    continue;
                }

                ComponentHandle handle = world.FindComponent(entity, comp.type);
                if (!handle)
                {
                    if (!AddComponent(world, entity, descriptor->key, error)) return false;
                    handle = world.FindComponent(entity, comp.type);
                }
                if (handle)
                {
                    if (const ComponentBase* base = world.GetComponent(handle);
                        base && base->Enabled() != comp.enabled && !world.SetComponentEnabled(handle, comp.enabled))
                    {
                        error = world.LastError();
                        return false;
                    }
                }
                for (const PrefabPropertyRecord& prop : comp.properties)
                {
                    const auto fieldIt = std::find_if(descriptor->properties.begin(), descriptor->properties.end(),
                                                      [&](const PropertyDescriptor& f) {
                        return SerializedPropertyKey(f) == prop.key;
                    });
                    if (fieldIt == descriptor->properties.end() || !fieldIt->editable) continue;
                    PropertyValue value = prop.value;
                    if (prop.localEntityReference)
                    {
                        const auto* ref = std::get_if<PropertyEntityReference>(&prop.value);
                        const auto mappedIt = ref ? nodeToEntity.find(ref->stableId) : nodeToEntity.end();
                        if (mappedIt == nodeToEntity.end()) { error = "Prefab.MissingNode"; return false; }
                        value = PropertyEntityReference{mappedIt->second};
                    }
                    if (!WriteProperty(world, entity, fieldIt->key, value, error)) return false;
                }
            }
        }
        return true;
    });
    if (!ok && error.empty()) error = session.LastError();
    return ok;
}

bool ApplyInstanceToPrefab(
    EditorSession& session,
    AssetStore& assets,
    const std::filesystem::path& projectRoot,
    StableEntityId entityOrRootId,
    std::string& error,
    EditorApplicationHooks* hooks)
{
    if (session.IsPlaying()) { error = "Editor.EditWhilePlaying"; return false; }
    const World& readWorld = session.EditWorld();
    const StableEntityId rootId = FindEnclosingPrefabRoot(readWorld, entityOrRootId);
    if (rootId == 0) { error = "Prefab.MissingEntity"; return false; }

    const EntityHandle rootHandle = readWorld.FindByStableId(rootId);
    const PrefabLink* rootLink = readWorld.GetPrefabLink(rootHandle);
    const std::string prefabId = rootLink->prefabId;
    const std::filesystem::path prefabPath = assets.Resolve(prefabId);
    if (prefabPath.empty()) { error = "Assets.UnknownId"; return false; }

    PrefabDocument originalDoc;
    if (!ReadPrefabDocument(prefabPath, originalDoc, error)) return false;

    const std::vector<StableEntityId> preSyncEntities = CollectAllPrefabEntityIds(readWorld, prefabId);
    const std::vector<StableEntityId> rawSubtree = CollectRawEntitySubtreeIds(readWorld, rootId);
    const std::vector<InstantiatedPrefabScript> attachedScripts = QueryScriptsForEntities(rawSubtree, hooks);

    PrefabDocument updatedDoc;
    std::unordered_map<StableEntityId, std::uint64_t> entityToLocal;
    if (!BuildPrefabDocumentFromEntity(readWorld, rootHandle, updatedDoc, entityToLocal, error, attachedScripts))
    {
        return false;
    }

    // 若原本即保留原有 localNodeId，尽量沿用该实例节点的 localNodeId 以保住场景内其余实例的覆盖键归属。
    std::unordered_map<std::uint64_t, std::uint64_t> remapToExistingId;
    std::unordered_set<std::uint64_t> usedIds;
    for (const StableEntityId stableId : rawSubtree)
    {
        const auto newLocal = entityToLocal.at(stableId);
        if (const PrefabLink* existingLink = readWorld.GetPrefabLink(readWorld.FindByStableId(stableId));
            existingLink && existingLink->prefabId == prefabId && !usedIds.contains(existingLink->localNodeId))
        {
            remapToExistingId[newLocal] = existingLink->localNodeId;
            usedIds.insert(existingLink->localNodeId);
        }
    }
    std::uint64_t nextFresh = 1;
    for (const PrefabNodeRecord& node : updatedDoc.nodes)
    {
        if (remapToExistingId.contains(node.localNodeId)) continue;
        while (usedIds.contains(nextFresh)) ++nextFresh;
        remapToExistingId[node.localNodeId] = nextFresh;
        usedIds.insert(nextFresh++);
    }
    updatedDoc.rootLocalNodeId = remapToExistingId.at(updatedDoc.rootLocalNodeId);
    for (PrefabNodeRecord& node : updatedDoc.nodes)
    {
        node.localNodeId = remapToExistingId.at(node.localNodeId);
        if (node.localParentId != 0) node.localParentId = remapToExistingId.at(node.localParentId);
        for (PrefabComponentRecord& comp : node.components)
        {
            for (PrefabPropertyRecord& prop : comp.properties)
            {
                if (prop.localEntityReference)
                {
                    if (auto* ref = std::get_if<PropertyEntityReference>(&prop.value))
                    {
                        ref->stableId = remapToExistingId.at(ref->stableId);
                    }
                }
            }
        }
        for (PrefabScriptRecord& script : node.scripts)
        {
            for (PrefabScriptFieldRecord& field : script.fields)
            {
                if (field.localNodeRef != 0) field.localNodeRef = remapToExistingId.at(field.localNodeRef);
            }
        }
        if ((!hooks || !hooks->scriptQuery) && node.scripts.empty())
        {
            if (const PrefabNodeRecord* prev = FindPrefabNode(originalDoc, node.localNodeId))
            {
                node.scripts = prev->scripts;
            }
        }
    }

    if (!ValidatePrefabDocument(updatedDoc, error)) return false;

    std::string originalDiskBytes;
    const bool haveOriginalBytes = ReadRawFileBytes(prefabPath, originalDiskBytes);

    // 先开启事务并在内存中校验/同步所有打开实例，确认成功后再写盘，确保场景事务与磁盘资产不发生分裂。
    if (!session.BeginEdit("Prefab.ApplyToAsset"))
    {
        error = session.LastError();
        return false;
    }

    std::vector<InstantiatedPrefabScript> syncedScripts;
    const bool applied = session.Apply("Prefab.ApplyToAsset", [&](World& world) {
        for (const auto& [stableId, newLocal] : entityToLocal)
        {
            const EntityHandle entity = world.FindByStableId(stableId);
            if (!entity || !world.SetPrefabLink(entity, PrefabLink{prefabId, remapToExistingId.at(newLocal), {}}))
            {
                error = world.LastError();
                return false;
            }
        }
        return SyncPrefabInstancesInWorld(world, prefabId, updatedDoc, error, &syncedScripts);
    });
    if (!applied)
    {
        if (error.empty()) error = session.LastError();
        session.CancelEdit();
        return false;
    }

    const std::vector<StableEntityId> postSyncEntities = CollectAllPrefabEntityIds(session.EditWorld(), prefabId);
    auto rollbackAll = [&]() {
        session.CancelEdit();
        if (!haveOriginalBytes || !WriteRawFileBytes(prefabPath, originalDiskBytes))
        {
            std::string ignoredRollbackError;
            const bool restored = WritePrefabDocument(prefabPath, originalDoc, ignoredRollbackError, true);
            (void)restored;
        }
        if (!projectRoot.empty())
        {
            std::vector<EditorAssetIssue> ignoredIssues;
            const bool synced = EditorAssetActions::SyncProjectAssets(projectRoot, assets, ignoredIssues);
            (void)synced;
        }
    };

    if (!WritePrefabDocument(prefabPath, updatedDoc, error, true))
    {
        session.CancelEdit();
        return false;
    }

    std::vector<EditorAssetIssue> syncIssues;
    if (!projectRoot.empty() && !EditorAssetActions::SyncProjectAssets(projectRoot, assets, syncIssues))
    {
        error = syncIssues.empty() ? "Assets.ProjectSyncFailed" : syncIssues.front().key;
        rollbackAll();
        return false;
    }

    if (!SyncScriptsViaHooks(preSyncEntities, postSyncEntities, syncedScripts, hooks, error))
    {
        rollbackAll();
        return false;
    }

    if (!session.CommitEdit())
    {
        error = session.LastError();
        rollbackAll();
        return false;
    }

    error.clear();
    return true;
}

bool SyncOpenScenePrefabInstances(
    EditorSession& session,
    AssetStore& assets,
    const AssetId& prefabId,
    std::string& error,
    EditorApplicationHooks* hooks)
{
    if (session.IsPlaying()) { error = "Editor.EditWhilePlaying"; return false; }
    const std::filesystem::path prefabPath = assets.Resolve(prefabId);
    if (prefabPath.empty()) { error = "Assets.UnknownId"; return false; }

    PrefabDocument document;
    if (!ReadPrefabDocument(prefabPath, document, error)) return false;

    const std::vector<StableEntityId> preSyncEntities = CollectAllPrefabEntityIds(session.EditWorld(), prefabId);
    if (!session.BeginEdit("Prefab.SyncInstances"))
    {
        error = session.LastError();
        return false;
    }

    std::vector<InstantiatedPrefabScript> scripts;
    const bool ok = session.Apply("Prefab.SyncInstances", [&](World& world) {
        return SyncPrefabInstancesInWorld(world, prefabId, document, error, &scripts);
    });
    const std::vector<StableEntityId> postSyncEntities = ok
        ? CollectAllPrefabEntityIds(session.EditWorld(), prefabId)
        : std::vector<StableEntityId>{};
    if (!ok || !SyncScriptsViaHooks(preSyncEntities, postSyncEntities, scripts, hooks, error) || !session.CommitEdit())
    {
        if (error.empty()) error = session.LastError();
        session.CancelEdit();
        return false;
    }
    error.clear();
    return true;
}
} // namespace se
