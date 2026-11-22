#include "FunctionLayer/Runtime/PrefabInstantiator.h"
#include "FunctionLayer/Runtime/ComponentMetadata.h"

#include <algorithm>
#include <charconv>
#include <unordered_set>

namespace flare
{
namespace
{
bool Fail(std::string* error, const char* key)
{
    if (error) *error = key;
    return false;
}

bool FailRef(std::string& error, const char* key)
{
    error = key;
    return false;
}

void CleanupCreatedEntities(World& world, const std::vector<StableEntityId>& createdIds)
{
    for (auto it = createdIds.rbegin(); it != createdIds.rend(); ++it)
    {
        if (const EntityHandle handle = world.FindByStableId(*it))
        {
            world.Destroy(handle);
        }
    }
    world.FlushCommands();
    for (const StableEntityId id : createdIds)
    {
        world.ForgetStableId(id);
    }
}

std::vector<const PrefabNodeRecord*> OrderNodesTopologically(const PrefabDocument& document)
{
    std::unordered_map<std::uint64_t, const PrefabNodeRecord*> byId;
    std::unordered_map<std::uint64_t, std::vector<const PrefabNodeRecord*>> children;
    for (const PrefabNodeRecord& node : document.nodes)
    {
        byId.emplace(node.localNodeId, &node);
        children[node.localParentId].push_back(&node);
    }
    for (auto& [parent, list] : children)
    {
        (void)parent;
        std::sort(list.begin(), list.end(), [](const PrefabNodeRecord* a, const PrefabNodeRecord* b) {
            return a->localNodeId < b->localNodeId;
        });
    }

    std::vector<const PrefabNodeRecord*> ordered;
    ordered.reserve(document.nodes.size());
    if (const auto rootIt = byId.find(document.rootLocalNodeId); rootIt != byId.end())
    {
        ordered.push_back(rootIt->second);
    }
    for (std::size_t i = 0; i < ordered.size(); ++i)
    {
        const auto childIt = children.find(ordered[i]->localNodeId);
        if (childIt == children.end()) continue;
        for (const PrefabNodeRecord* child : childIt->second) ordered.push_back(child);
    }
    return ordered;
}

std::optional<std::uint64_t> ParseDecimalEntityRef(std::string_view jsonText)
{
    if (jsonText.size() >= 2 && jsonText.front() == '"' && jsonText.back() == '"')
    {
        const std::string_view inner = jsonText.substr(1, jsonText.size() - 2);
        std::uint64_t id = 0;
        const auto parsed = std::from_chars(inner.data(), inner.data() + inner.size(), id);
        if (parsed.ec == std::errc{} && parsed.ptr == inner.data() + inner.size() && id != 0)
        {
            return id;
        }
    }
    return std::nullopt;
}

bool CreateMappedComponents(
    World& world,
    EntityHandle entity,
    const PrefabNodeRecord& node,
    bool applyRootTransform,
    const Transform& rootTransform,
    const std::unordered_map<std::uint64_t, StableEntityId>& nodeToEntity,
    std::string& error)
{
    for (const PrefabComponentRecord& comp : node.components)
    {
        if (comp.type == ComponentTypes::Transform)
        {
            Transform target = rootTransform;
            if (!applyRootTransform && !ExtractTransformFromPrefabComponent(comp, target))
            {
                return FailRef(error, "Prefab.InvalidDocument");
            }
            if (!world.SetTransform(entity, target))
            {
                error = world.LastError();
                return false;
            }
            continue;
        }

        const ComponentDescriptor* descriptor = FindComponentDescriptor(comp.type);
        if (!descriptor) return FailRef(error, "Scene.UnknownComponent");

        std::vector<ComponentProperty> properties;
        properties.reserve(comp.properties.size());
        for (const PrefabPropertyRecord& prop : comp.properties)
        {
            PropertyValue value = prop.value;
            if (prop.localEntityReference)
            {
                const auto* ref = std::get_if<PropertyEntityReference>(&prop.value);
                const auto mappedIt = ref ? nodeToEntity.find(ref->stableId) : nodeToEntity.end();
                if (mappedIt == nodeToEntity.end()) return FailRef(error, "Prefab.MissingNode");
                value = PropertyEntityReference{mappedIt->second};
            }
            properties.push_back({prop.key, std::move(value)});
        }

        if (!CreateComponentFromProperties(world, entity, comp.type, comp.version, std::move(properties),
                                           comp.enabled, error))
        {
            return false;
        }
    }
    return true;
}

std::vector<InstantiatedPrefabScript> RemapNodeScripts(
    const PrefabNodeRecord& node,
    StableEntityId targetEntity,
    const std::unordered_map<std::uint64_t, StableEntityId>& nodeToEntity)
{
    std::vector<InstantiatedPrefabScript> out;
    out.reserve(node.scripts.size());
    for (const PrefabScriptRecord& sourceScript : node.scripts)
    {
        PrefabScriptRecord mapped = sourceScript;
        for (PrefabScriptFieldRecord& field : mapped.fields)
        {
            if (field.localNodeRef != 0)
            {
                if (const auto it = nodeToEntity.find(field.localNodeRef); it != nodeToEntity.end())
                {
                    field.valueJson = "\"" + std::to_string(it->second) + "\"";
                }
                field.localNodeRef = 0;
            }
        }
        out.push_back({targetEntity, std::move(mapped)});
    }
    return out;
}

bool HasOverrideKey(const std::vector<PropertyOverride>& overrides, std::string_view key)
{
    return std::any_of(overrides.begin(), overrides.end(), [key](const PropertyOverride& item) {
        return item.key == key;
    });
}
} // namespace

StableEntityId InstantiatePrefab(
    World& world,
    const PrefabDocument& document,
    const Transform& rootTransform,
    std::string* error,
    const std::string& prefabId,
    StableEntityId rootParent,
    PrefabInstanceReport* report,
    const std::unordered_map<std::uint64_t, std::vector<PropertyOverride>>* overridesByNode)
{
    std::string localError;
    if (!ValidatePrefabDocument(document, localError))
    {
        if (error) *error = localError;
        return 0;
    }
    if (world.IsInFrame() || world.IsIterating() || (world.HasPendingCommands() && !world.IsSceneCommitBoundary()))
    {
        Fail(error, "Scene.PendingCommands");
        return 0;
    }
    if (!IsValid(rootTransform))
    {
        Fail(error, "World.InvalidTransform");
        return 0;
    }
    if (!prefabId.empty() && !IsValidAssetIdString(prefabId))
    {
        Fail(error, "Prefab.InvalidDocument");
        return 0;
    }

    EntityHandle parentHandle{};
    if (rootParent != 0)
    {
        parentHandle = world.FindByStableId(rootParent);
        if (!parentHandle)
        {
            Fail(error, "World.InvalidEntity");
            return 0;
        }
    }

    const auto ordered = OrderNodesTopologically(document);
    if (ordered.size() != document.nodes.size())
    {
        Fail(error, "Prefab.InvalidDocument");
        return 0;
    }

    std::vector<StableEntityId> createdIds;
    createdIds.reserve(ordered.size());
    std::unordered_map<std::uint64_t, StableEntityId> nodeToEntity;
    nodeToEntity.reserve(ordered.size());

    auto rollback = [&](const char* key = nullptr) -> StableEntityId {
        if (key && localError.empty()) localError = key;
        if (error) *error = localError;
        CleanupCreatedEntities(world, createdIds);
        return 0;
    };

    for (const PrefabNodeRecord* node : ordered)
    {
        const EntityHandle entity = world.Create(node->name);
        if (!entity) return rollback(world.LastError().c_str());
        const StableEntityId stableId = world.Get(entity)->stableId;
        createdIds.push_back(stableId);
        nodeToEntity.emplace(node->localNodeId, stableId);

        if (!world.SetEnabled(entity, node->enabled))
        {
            return rollback(world.LastError().c_str());
        }
        if (node->tag != "Untagged" && !world.SetTag(entity, node->tag))
        {
            return rollback(world.LastError().c_str());
        }
        if (node->layer != 0 && !world.SetLayer(entity, node->layer))
        {
            return rollback(world.LastError().c_str());
        }
        if (node->staticFlags != 0 && !world.SetStaticFlags(entity, node->staticFlags))
        {
            return rollback(world.LastError().c_str());
        }
        const EntityHandle resolvedParent = node->localParentId == 0
            ? parentHandle
            : world.FindByStableId(nodeToEntity.at(node->localParentId));
        if (resolvedParent && !world.SetParent(entity, resolvedParent))
        {
            return rollback(world.LastError().c_str());
        }
    }

    std::vector<InstantiatedPrefabScript> mappedScripts;
    for (const PrefabNodeRecord* node : ordered)
    {
        const StableEntityId stableId = nodeToEntity.at(node->localNodeId);
        const EntityHandle entity = world.FindByStableId(stableId);
        const bool isRoot = (node->localNodeId == document.rootLocalNodeId);
        if (!CreateMappedComponents(world, entity, *node, isRoot, rootTransform, nodeToEntity, localError))
        {
            return rollback();
        }

        std::vector<PropertyOverride> nodeOverrides;
        if (overridesByNode)
        {
            if (const auto ovIt = overridesByNode->find(node->localNodeId); ovIt != overridesByNode->end())
            {
                nodeOverrides = ovIt->second;
            }
        }
        if (!prefabId.empty())
        {
            if (!world.SetPrefabLink(entity, PrefabLink{prefabId, node->localNodeId, nodeOverrides}))
            {
                return rollback(world.LastError().c_str());
            }
        }
        for (const PropertyOverride& ov : nodeOverrides)
        {
            if (!ApplyPropertyOverride(world, entity, ov, localError))
            {
                return rollback();
            }
        }

        auto nodeScripts = RemapNodeScripts(*node, stableId, nodeToEntity);
        mappedScripts.insert(mappedScripts.end(),
                             std::make_move_iterator(nodeScripts.begin()),
                             std::make_move_iterator(nodeScripts.end()));
    }

    if (!world.FlushCommands())
    {
        localError = world.LastError();
        return rollback();
    }

    const StableEntityId rootEntity = nodeToEntity.at(document.rootLocalNodeId);
    if (report)
    {
        report->rootEntity = rootEntity;
        report->nodeToEntity = std::move(nodeToEntity);
        report->scripts = std::move(mappedScripts);
    }
    if (error) error->clear();
    return rootEntity;
}

bool ReadEntityOverrideValue(const World& world, EntityHandle entity, std::string_view key,
                             PropertyValue& outValue, std::string& error)
{
    const EntityRecord* record = world.Get(entity);
    if (!record) return FailRef(error, "Metadata.InvalidEntity");
    if (key == "name")
    {
        outValue = record->name;
        error.clear();
        return true;
    }
    if (key == "enabled")
    {
        outValue = record->enabled;
        error.clear();
        return true;
    }
    if (key == "tag")
    {
        outValue = record->tag;
        error.clear();
        return true;
    }
    if (key == "layer")
    {
        outValue = static_cast<float>(record->layer);
        error.clear();
        return true;
    }
    if (key == "staticFlags")
    {
        outValue = static_cast<float>(record->staticFlags);
        error.clear();
        return true;
    }
    if (key.ends_with(".enabled"))
    {
        const std::string_view compKey = key.substr(0, key.size() - 8);
        if (const ComponentDescriptor* desc = FindComponentDescriptor(compKey))
        {
            const ComponentHandle ch = world.FindComponent(entity, desc->type);
            if (const ComponentBase* cb = world.GetComponent(ch))
            {
                outValue = cb->Enabled();
                error.clear();
                return true;
            }
        }
    }
    return ReadProperty(world, entity, key, outValue, error);
}

bool ApplyPropertyOverride(World& world, EntityHandle entity,
                           const PropertyOverride& overrideValue, std::string& error)
{
    if (!world.IsAlive(entity)) return FailRef(error, "Metadata.InvalidEntity");
    if (overrideValue.key == "name")
    {
        const auto* text = std::get_if<std::string>(&overrideValue.value);
        if (!text) return FailRef(error, "Metadata.InvalidValue");
        if (world.Rename(entity, *text)) { error.clear(); return true; }
        error = world.LastError();
        return false;
    }
    if (overrideValue.key == "enabled")
    {
        const auto* flag = std::get_if<bool>(&overrideValue.value);
        if (!flag) return FailRef(error, "Metadata.InvalidValue");
        if (world.SetEnabled(entity, *flag)) { error.clear(); return true; }
        error = world.LastError();
        return false;
    }
    if (overrideValue.key == "tag")
    {
        const auto* text = std::get_if<std::string>(&overrideValue.value);
        if (!text) return FailRef(error, "Metadata.InvalidValue");
        if (world.SetTag(entity, *text)) { error.clear(); return true; }
        error = world.LastError();
        return false;
    }
    if (overrideValue.key == "layer")
    {
        const auto* num = std::get_if<float>(&overrideValue.value);
        if (!num || !IsFinite(*num) || *num < 0.0f || *num > 31.0f) return FailRef(error, "Metadata.InvalidValue");
        if (world.SetLayer(entity, static_cast<std::uint32_t>(std::lround(*num)))) { error.clear(); return true; }
        error = world.LastError();
        return false;
    }
    if (overrideValue.key == "staticFlags")
    {
        const auto* num = std::get_if<float>(&overrideValue.value);
        if (!num || !IsFinite(*num) || *num < 0.0f || *num > 255.0f) return FailRef(error, "Metadata.InvalidValue");
        if (world.SetStaticFlags(entity, static_cast<std::uint32_t>(std::lround(*num)))) { error.clear(); return true; }
        error = world.LastError();
        return false;
    }
    if (overrideValue.key.ends_with(".enabled"))
    {
        const auto* flag = std::get_if<bool>(&overrideValue.value);
        if (!flag) return FailRef(error, "Metadata.InvalidValue");
        const std::string_view compKey = std::string_view(overrideValue.key).substr(0, overrideValue.key.size() - 8);
        if (const ComponentDescriptor* desc = FindComponentDescriptor(compKey))
        {
            const ComponentHandle ch = world.FindComponent(entity, desc->type);
            if (ch && world.SetComponentEnabled(ch, *flag)) { error.clear(); return true; }
            error = world.LastError();
            return false;
        }
    }
    return WriteProperty(world, entity, overrideValue.key, overrideValue.value, error);
}

bool ReadPrefabNodePropertyValue(const PrefabNodeRecord& node, std::string_view key,
                                 PropertyValue& outValue, bool& outIsLocalRef)
{
    outIsLocalRef = false;
    if (key == "name") { outValue = node.name; return true; }
    if (key == "enabled") { outValue = node.enabled; return true; }
    if (key == "tag") { outValue = node.tag; return true; }
    if (key == "layer") { outValue = static_cast<float>(node.layer); return true; }
    if (key == "staticFlags") { outValue = static_cast<float>(node.staticFlags); return true; }
    if (key.ends_with(".enabled"))
    {
        const std::string_view compKey = key.substr(0, key.size() - 8);
        if (const ComponentDescriptor* desc = FindComponentDescriptor(compKey))
        {
            for (const PrefabComponentRecord& comp : node.components)
            {
                if (comp.type == desc->type) { outValue = comp.enabled; return true; }
            }
        }
        return false;
    }

    const auto dot = key.find('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view componentKey = key.substr(0, dot);
    const ComponentDescriptor* descriptor = FindComponentDescriptor(componentKey);
    if (!descriptor) return false;

    const auto fieldIt = std::find_if(descriptor->properties.begin(), descriptor->properties.end(),
                                      [key](const PropertyDescriptor& field) { return field.key == key; });
    if (fieldIt == descriptor->properties.end()) return false;
    const std::string_view serialKey = SerializedPropertyKey(*fieldIt);

    for (const PrefabComponentRecord& comp : node.components)
    {
        if (comp.type != descriptor->type) continue;
        for (const PrefabPropertyRecord& prop : comp.properties)
        {
            if (prop.key == serialKey)
            {
                outValue = prop.value;
                outIsLocalRef = prop.localEntityReference;
                return true;
            }
        }
        outValue = fieldIt->defaultValue;
        return true;
    }
    return false;
}

bool BuildPrefabDocumentFromEntity(
    const World& world,
    EntityHandle rootEntity,
    PrefabDocument& outDocument,
    std::unordered_map<StableEntityId, std::uint64_t>& outEntityToLocal,
    std::string& error,
    const std::vector<InstantiatedPrefabScript>& attachedScripts)
{
    outDocument = {};
    outEntityToLocal.clear();
    if (!world.IsAlive(rootEntity)) return FailRef(error, "World.InvalidEntity");

    std::unordered_map<StableEntityId, std::vector<EntityHandle>> childrenByParent;
    for (const EntityHandle candidate : world.Entities())
    {
        const EntityRecord* record = world.Get(candidate);
        if (record && record->parent)
        {
            childrenByParent[world.Get(record->parent)->stableId].push_back(candidate);
        }
    }
    for (auto& [parent, list] : childrenByParent)
    {
        (void)parent;
        std::sort(list.begin(), list.end(), [&](EntityHandle a, EntityHandle b) {
            return world.Get(a)->stableId < world.Get(b)->stableId;
        });
    }

    std::vector<EntityHandle> subtree{rootEntity};
    for (std::size_t i = 0; i < subtree.size(); ++i)
    {
        const StableEntityId id = world.Get(subtree[i])->stableId;
        if (const auto it = childrenByParent.find(id); it != childrenByParent.end())
        {
            subtree.insert(subtree.end(), it->second.begin(), it->second.end());
        }
    }

    outEntityToLocal.reserve(subtree.size());
    std::uint64_t nextLocalId = 1;
    for (const EntityHandle entity : subtree)
    {
        outEntityToLocal.emplace(world.Get(entity)->stableId, nextLocalId++);
    }

    PrefabDocument document;
    document.version = PrefabDocumentVersion;
    document.rootLocalNodeId = outEntityToLocal.at(world.Get(rootEntity)->stableId);
    document.nodes.reserve(subtree.size());

    for (std::size_t idx = 0; idx < subtree.size(); ++idx)
    {
        const EntityHandle entity = subtree[idx];
        const EntityRecord* record = world.Get(entity);
        PrefabNodeRecord node;
        node.localNodeId = outEntityToLocal.at(record->stableId);
        node.localParentId = (idx == 0 || !record->parent)
            ? 0
            : outEntityToLocal.at(world.Get(record->parent)->stableId);
        node.name = record->name;
        node.enabled = record->enabled;
        node.tag = record->tag;
        node.layer = record->layer;
        node.staticFlags = record->staticFlags;

        for (const ComponentHandle handle : world.Components(entity))
        {
            const ComponentBase* component = world.GetComponent(handle);
            if (!component || component->Type() == ComponentTypes::Script) continue;
            const ComponentDescriptor* descriptor = FindComponentDescriptor(component->Type());
            if (!descriptor) return FailRef(error, "Scene.UnknownComponent");

            std::vector<ComponentProperty> fields;
            if (!ReadComponentProperties(world, handle, fields, error)) return false;

            PrefabComponentRecord compRecord;
            compRecord.type = descriptor->type;
            compRecord.version = descriptor->version;
            compRecord.enabled = component->Enabled();
            compRecord.properties.reserve(fields.size());

            for (const ComponentProperty& field : fields)
            {
                const auto propDescIt = std::find_if(descriptor->properties.begin(), descriptor->properties.end(),
                                                     [&](const PropertyDescriptor& p) {
                    return SerializedPropertyKey(p) == field.key;
                });
                if (propDescIt == descriptor->properties.end()) return FailRef(error, "Metadata.UnknownProperty");

                PropertyValue storedValue = field.value;
                bool localRef = false;
                if (idx == 0 && compRecord.type == ComponentTypes::Transform)
                {
                    if (field.key == "position") storedValue = Vec3{};
                    else if (field.key == "rotation") storedValue = Quat{};
                }
                if (const auto* ref = std::get_if<PropertyEntityReference>(&storedValue))
                {
                    if (const auto targetIt = outEntityToLocal.find(ref->stableId); targetIt != outEntityToLocal.end())
                    {
                        storedValue = PropertyEntityReference{targetIt->second};
                        localRef = true;
                    }
                }
                compRecord.properties.push_back({field.key, propDescIt->type, std::move(storedValue), localRef});
            }
            node.components.push_back(std::move(compRecord));
        }

        for (const InstantiatedPrefabScript& attached : attachedScripts)
        {
            if (attached.entity != record->stableId) continue;
            PrefabScriptRecord scriptCopy = attached.script;
            for (PrefabScriptFieldRecord& field : scriptCopy.fields)
            {
                if (field.kind == "entity")
                {
                    if (const auto targetId = ParseDecimalEntityRef(field.valueJson))
                    {
                        if (const auto localIt = outEntityToLocal.find(*targetId); localIt != outEntityToLocal.end())
                        {
                            field.localNodeRef = localIt->second;
                            field.valueJson = "\"0\"";
                        }
                    }
                }
            }
            node.scripts.push_back(std::move(scriptCopy));
        }

        document.nodes.push_back(std::move(node));
    }

    if (!ValidatePrefabDocument(document, error)) return false;
    outDocument = std::move(document);
    error.clear();
    return true;
}

bool SyncPrefabInstancesInWorld(
    World& world,
    const std::string& prefabId,
    const PrefabDocument& document,
    std::string& error,
    std::vector<InstantiatedPrefabScript>* outScripts)
{
    if (!IsValidAssetIdString(prefabId)) return FailRef(error, "Prefab.InvalidDocument");
    if (!ValidatePrefabDocument(document, error)) return false;
    if (world.IsInFrame() || world.IsIterating() || (world.HasPendingCommands() && !world.IsSceneCommitBoundary()))
    {
        return FailRef(error, "Scene.PendingCommands");
    }

    std::vector<EntityHandle> instanceRoots;
    for (const EntityHandle entity : world.Entities())
    {
        const PrefabLink* link = world.GetPrefabLink(entity);
        if (!link || link->prefabId != prefabId) continue;
        const EntityRecord* record = world.Get(entity);
        const PrefabLink* parentLink = record && record->parent ? world.GetPrefabLink(record->parent) : nullptr;
        if (!parentLink || parentLink->prefabId != prefabId)
        {
            instanceRoots.push_back(entity);
        }
    }
    std::sort(instanceRoots.begin(), instanceRoots.end(), [&](EntityHandle a, EntityHandle b) {
        return world.Get(a)->stableId < world.Get(b)->stableId;
    });

    const auto orderedNodes = OrderNodesTopologically(document);
    std::unordered_set<std::uint64_t> docNodeIds;
    for (const PrefabNodeRecord* node : orderedNodes) docNodeIds.insert(node->localNodeId);

    for (const EntityHandle root : instanceRoots)
    {
        std::unordered_map<std::uint64_t, StableEntityId> nodeToEntity;
        std::vector<EntityHandle> subtree{root};
        for (std::size_t i = 0; i < subtree.size(); ++i)
        {
            const EntityHandle current = subtree[i];
            if (const PrefabLink* link = world.GetPrefabLink(current); link && link->prefabId == prefabId)
            {
                nodeToEntity.emplace(link->localNodeId, world.Get(current)->stableId);
            }
            const StableEntityId currentId = world.Get(current)->stableId;
            for (const EntityHandle candidate : world.Entities())
            {
                const EntityRecord* rec = world.Get(candidate);
                if (rec && rec->parent && world.Get(rec->parent)->stableId == currentId)
                {
                    if (const PrefabLink* childLink = world.GetPrefabLink(candidate);
                        childLink && childLink->prefabId == prefabId)
                    {
                        subtree.push_back(candidate);
                    }
                }
            }
        }

        // 先移除原型中已删去的非根节点。
        std::vector<StableEntityId> toDestroy;
        for (const EntityHandle entity : subtree)
        {
            if (entity == root) continue;
            const PrefabLink* link = world.GetPrefabLink(entity);
            if (link && !docNodeIds.contains(link->localNodeId))
            {
                toDestroy.push_back(world.Get(entity)->stableId);
            }
        }
        for (const StableEntityId id : toDestroy)
        {
            if (const EntityHandle handle = world.FindByStableId(id)) world.Destroy(handle);
        }
        if (!toDestroy.empty() && !world.FlushCommands())
        {
            error = world.LastError();
            return false;
        }

        // 创建原型新增节点。
        for (const PrefabNodeRecord* node : orderedNodes)
        {
            if (nodeToEntity.contains(node->localNodeId)) continue;
            const EntityHandle created = world.Create(node->name);
            if (!created) { error = world.LastError(); return false; }
            const StableEntityId createdId = world.Get(created)->stableId;
            nodeToEntity.emplace(node->localNodeId, createdId);
            if (node->localParentId != 0)
            {
                if (const auto parentIt = nodeToEntity.find(node->localParentId); parentIt != nodeToEntity.end())
                {
                    if (!world.SetParent(created, world.FindByStableId(parentIt->second)))
                    {
                        error = world.LastError();
                        return false;
                    }
                }
            }
            if (!world.SetPrefabLink(created, PrefabLink{prefabId, node->localNodeId, {}}))
            {
                error = world.LastError();
                return false;
            }
        }

        // 同步未覆盖字段，随后保留既有覆盖项。
        for (const PrefabNodeRecord* node : orderedNodes)
        {
            const EntityHandle entity = world.FindByStableId(nodeToEntity.at(node->localNodeId));
            const PrefabLink* link = world.GetPrefabLink(entity);
            const std::vector<PropertyOverride> overrides = link ? link->overrides : std::vector<PropertyOverride>{};
            const bool isRoot = (entity == root);

            if (!HasOverrideKey(overrides, "name") && !world.Rename(entity, node->name))
            {
                error = world.LastError();
                return false;
            }
            if (!HasOverrideKey(overrides, "enabled") && !world.SetEnabled(entity, node->enabled))
            {
                error = world.LastError();
                return false;
            }
            if (!HasOverrideKey(overrides, "tag") && !world.SetTag(entity, node->tag))
            {
                error = world.LastError();
                return false;
            }
            if (!HasOverrideKey(overrides, "layer") && !world.SetLayer(entity, node->layer))
            {
                error = world.LastError();
                return false;
            }
            if (!HasOverrideKey(overrides, "staticFlags") && !world.SetStaticFlags(entity, node->staticFlags))
            {
                error = world.LastError();
                return false;
            }

            std::unordered_set<ComponentTypeId> protoTypes;
            for (const PrefabComponentRecord& comp : node->components) protoTypes.insert(comp.type);

            std::vector<ComponentHandle> removeList;
            for (const ComponentHandle ch : world.Components(entity))
            {
                const ComponentBase* comp = world.GetComponent(ch);
                if (!comp || comp->Type() == ComponentTypes::Transform || comp->Type() == ComponentTypes::Script) continue;
                if (!protoTypes.contains(comp->Type())) removeList.push_back(ch);
            }
            for (const ComponentHandle ch : removeList)
            {
                if (!world.RemoveComponent(ch)) { error = world.LastError(); return false; }
            }

            for (const PrefabComponentRecord& comp : node->components)
            {
                if (comp.type == ComponentTypes::Transform)
                {
                    Transform protoTransform{};
                    if (!ExtractTransformFromPrefabComponent(comp, protoTransform))
                    {
                        return FailRef(error, "Prefab.InvalidDocument");
                    }
                    const Transform* currentTransform = world.GetTransform(entity);
                    Transform next = currentTransform ? *currentTransform : Transform{};
                    if (!isRoot && !HasOverrideKey(overrides, "transform.position")) next.position = protoTransform.position;
                    if (!isRoot && !HasOverrideKey(overrides, "transform.rotation")) next.rotation = protoTransform.rotation;
                    if (!HasOverrideKey(overrides, "transform.scale")) next.scale = protoTransform.scale;
                    if (!world.SetTransform(entity, next)) { error = world.LastError(); return false; }
                    continue;
                }

                const ComponentDescriptor* descriptor = FindComponentDescriptor(comp.type);
                if (!descriptor) return FailRef(error, "Scene.UnknownComponent");

                ComponentHandle existing = world.FindComponent(entity, comp.type);
                if (!existing)
                {
                    std::vector<ComponentProperty> initialProps;
                    initialProps.reserve(comp.properties.size());
                    for (const PrefabPropertyRecord& prop : comp.properties)
                    {
                        PropertyValue value = prop.value;
                        if (prop.localEntityReference)
                        {
                            const auto* ref = std::get_if<PropertyEntityReference>(&prop.value);
                            const auto mappedIt = ref ? nodeToEntity.find(ref->stableId) : nodeToEntity.end();
                            if (mappedIt == nodeToEntity.end()) return FailRef(error, "Prefab.MissingNode");
                            value = PropertyEntityReference{mappedIt->second};
                        }
                        initialProps.push_back({prop.key, std::move(value)});
                    }
                    if (!CreateComponentFromProperties(world, entity, comp.type, comp.version,
                                                       std::move(initialProps), comp.enabled, error))
                    {
                        return false;
                    }
                    continue;
                }

                const ComponentBase* compBase = world.GetComponent(existing);
                const std::string enabledOverrideKey = std::string(descriptor->key) + ".enabled";
                if (!HasOverrideKey(overrides, enabledOverrideKey) && compBase &&
                    compBase->Enabled() != comp.enabled && !world.SetComponentEnabled(existing, comp.enabled))
                {
                    error = world.LastError();
                    return false;
                }

                for (const PrefabPropertyRecord& prop : comp.properties)
                {
                    const auto fieldIt = std::find_if(descriptor->properties.begin(), descriptor->properties.end(),
                                                      [&](const PropertyDescriptor& f) {
                        return SerializedPropertyKey(f) == prop.key;
                    });
                    if (fieldIt == descriptor->properties.end()) continue;
                    if (!fieldIt->editable || HasOverrideKey(overrides, fieldIt->key)) continue;

                    PropertyValue value = prop.value;
                    if (prop.localEntityReference)
                    {
                        const auto* ref = std::get_if<PropertyEntityReference>(&prop.value);
                        const auto mappedIt = ref ? nodeToEntity.find(ref->stableId) : nodeToEntity.end();
                        if (mappedIt == nodeToEntity.end()) return FailRef(error, "Prefab.MissingNode");
                        value = PropertyEntityReference{mappedIt->second};
                    }
                    if (!WriteProperty(world, entity, fieldIt->key, value, error)) return false;
                }
            }

            for (const PropertyOverride& ov : overrides)
            {
                if (!ApplyPropertyOverride(world, entity, ov, error)) return false;
            }

            if (outScripts)
            {
                auto scripts = RemapNodeScripts(*node, nodeToEntity.at(node->localNodeId), nodeToEntity);
                outScripts->insert(outScripts->end(),
                                   std::make_move_iterator(scripts.begin()),
                                   std::make_move_iterator(scripts.end()));
            }
        }
    }

    if (!world.FlushCommands())
    {
        error = world.LastError();
        return false;
    }
    error.clear();
    return true;
}
} // namespace flare
