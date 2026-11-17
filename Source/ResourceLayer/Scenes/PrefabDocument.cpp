#include "ResourceLayer/Scenes/PrefabDocument.h"
#include "ResourceLayer/Scenes/PrefabJson.h"

#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace se
{
namespace
{
using Json = nlohmann::json;
constexpr std::size_t MaxPrefabBytes = 16 * 1024 * 1024;
constexpr std::size_t MaxPrefabNodes = 100000;
std::atomic<std::uint64_t> temporarySequence{1};

bool Fail(std::string& error, const char* key)
{
    error = key;
    return false;
}

bool ValidateNodeSubtree(const PrefabDocument& document, std::string& error)
{
    if (document.version != PrefabDocumentVersion) return Fail(error, "Prefab.UnsupportedVersion");
    if (document.nodes.empty() || document.nodes.size() > MaxPrefabNodes || document.rootLocalNodeId == 0)
    {
        return Fail(error, "Prefab.InvalidDocument");
    }

    std::unordered_map<std::uint64_t, std::size_t> byId;
    byId.reserve(document.nodes.size());
    std::size_t rootCount = 0;
    for (std::size_t i = 0; i < document.nodes.size(); ++i)
    {
        const PrefabNodeRecord& node = document.nodes[i];
        if (node.localNodeId == 0 || !byId.emplace(node.localNodeId, i).second)
        {
            return Fail(error, "Prefab.InvalidDocument");
        }
        if (node.localParentId == 0) ++rootCount;
    }
    if (rootCount != 1) return Fail(error, "Prefab.InvalidDocument");
    const auto rootIt = byId.find(document.rootLocalNodeId);
    if (rootIt == byId.end() || document.nodes[rootIt->second].localParentId != 0)
    {
        return Fail(error, "Prefab.InvalidDocument");
    }

    for (const PrefabNodeRecord& node : document.nodes)
    {
        std::uint64_t cursor = node.localParentId;
        std::unordered_set<std::uint64_t> visited{node.localNodeId};
        while (cursor != 0)
        {
            if (!visited.insert(cursor).second) return Fail(error, "Prefab.CycleParent");
            const auto parentIt = byId.find(cursor);
            if (parentIt == byId.end()) return Fail(error, "Prefab.MissingNode");
            cursor = document.nodes[parentIt->second].localParentId;
        }

        bool hasTransform = false;
        for (const PrefabComponentRecord& component : node.components)
        {
            if (component.type == 0 || component.version < 1) return Fail(error, "Prefab.InvalidDocument");
            if (component.type == 1)
            {
                if (hasTransform) return Fail(error, "Prefab.InvalidDocument");
                hasTransform = true;
            }
            std::unordered_set<std::string> propKeys;
            for (const PrefabPropertyRecord& prop : component.properties)
            {
                if (prop.key.empty() || !propKeys.insert(prop.key).second)
                {
                    return Fail(error, "Prefab.InvalidDocument");
                }
                if (prop.localEntityReference)
                {
                    const auto* ref = std::get_if<PropertyEntityReference>(&prop.value);
                    if (!ref || !byId.contains(ref->stableId))
                    {
                        return Fail(error, "Prefab.MissingNode");
                    }
                }
            }
        }
        if (!hasTransform) return Fail(error, "Prefab.MissingComponent");

        std::unordered_set<std::string> scriptTypes;
        for (const PrefabScriptRecord& script : node.scripts)
        {
            if (script.type.empty() || !scriptTypes.insert(script.type).second)
            {
                return Fail(error, "Prefab.InvalidDocument");
            }
            std::unordered_set<std::string> fieldNames;
            for (const PrefabScriptFieldRecord& field : script.fields)
            {
                if (field.name.empty() || field.kind.empty() || !fieldNames.insert(field.name).second)
                {
                    return Fail(error, "Prefab.InvalidDocument");
                }
                if (field.localNodeRef != 0 && !byId.contains(field.localNodeRef))
                {
                    return Fail(error, "Prefab.MissingNode");
                }
            }
        }
    }

    error.clear();
    return true;
}
} // namespace

bool IsPrefabDocumentPath(const std::filesystem::path& path)
{
    auto name = path.filename().u8string();
    std::string lower(reinterpret_cast<const char*>(name.data()), name.size());
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.size() > PrefabFileExtension.size() && lower.ends_with(PrefabFileExtension);
}

bool IsValidAssetIdString(std::string_view id) noexcept
{
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool ValidatePrefabDocument(const PrefabDocument& document, std::string& error)
{
    return ValidateNodeSubtree(document, error);
}

bool SerializePrefabDocument(const PrefabDocument& document, std::string& jsonText, std::string& error)
{
    if (!ValidateNodeSubtree(document, error)) return false;
    try
    {
        std::vector<const PrefabNodeRecord*> ordered;
        ordered.reserve(document.nodes.size());
        for (const PrefabNodeRecord& node : document.nodes) ordered.push_back(&node);
        std::sort(ordered.begin(), ordered.end(), [](const PrefabNodeRecord* a, const PrefabNodeRecord* b) {
            return a->localNodeId < b->localNodeId;
        });

        Json nodesJson = Json::array();
        for (const PrefabNodeRecord* node : ordered)
        {
            Json componentsJson = Json::array();
            for (const PrefabComponentRecord& component : node->components)
            {
                Json propsJson = Json::array();
                for (const PrefabPropertyRecord& prop : component.properties)
                {
                    Json propEntry{
                        {"key", prop.key},
                        {"type", detail::PropertyTypeJsonName(prop.type)},
                        {"value", WritePropertyJson(prop.value)}
                    };
                    if (prop.localEntityReference) propEntry["localRef"] = true;
                    propsJson.push_back(std::move(propEntry));
                }
                componentsJson.push_back({
                    {"type", std::to_string(component.type)},
                    {"version", component.version},
                    {"enabled", component.enabled},
                    {"properties", std::move(propsJson)}
                });
            }

            Json scriptsJson = Json::array();
            for (const PrefabScriptRecord& script : node->scripts)
            {
                Json fieldsJson = Json::array();
                for (const PrefabScriptFieldRecord& field : script.fields)
                {
                    Json parsedValue = field.valueJson.empty() ? Json(nullptr) : Json::parse(field.valueJson);
                    Json fieldEntry{
                        {"name", field.name},
                        {"kind", field.kind},
                        {"value", std::move(parsedValue)}
                    };
                    if (field.localNodeRef != 0) fieldEntry["localNodeRef"] = std::to_string(field.localNodeRef);
                    fieldsJson.push_back(std::move(fieldEntry));
                }
                scriptsJson.push_back({
                    {"type", script.type},
                    {"enabled", script.enabled},
                    {"fields", std::move(fieldsJson)}
                });
            }

            nodesJson.push_back({
                {"localNodeId", std::to_string(node->localNodeId)},
                {"localParentId", node->localParentId == 0 ? Json(nullptr) : Json(std::to_string(node->localParentId))},
                {"name", node->name},
                {"enabled", node->enabled},
                {"components", std::move(componentsJson)},
                {"scripts", std::move(scriptsJson)}
            });
        }

        const Json root{
            {"format", "SelfEngine.Prefab"},
            {"version", document.version},
            {"rootLocalNodeId", std::to_string(document.rootLocalNodeId)},
            {"nodes", std::move(nodesJson)}
        };
        std::string dumped = root.dump(2) + "\n";
        if (dumped.size() > MaxPrefabBytes) return Fail(error, "Prefab.InvalidDocument");
        jsonText = std::move(dumped);
        error.clear();
        return true;
    }
    catch (const Json::exception&)
    {
        return Fail(error, "Prefab.InvalidDocument");
    }
}

bool ParsePrefabDocument(std::string_view jsonText, PrefabDocument& document, std::string& error)
{
    error.clear();
    if (jsonText.size() > MaxPrefabBytes) return Fail(error, "Prefab.InvalidDocument");
    try
    {
        bool duplicate = false;
        std::vector<std::unordered_map<std::string, bool>> objectKeys;
        const Json root = Json::parse(jsonText.begin(), jsonText.end(), [&](int depth, Json::parse_event_t event, Json& parsed) {
            if (depth > 64) throw std::runtime_error("Prefab.InvalidDocument");
            if (event == Json::parse_event_t::object_start) objectKeys.emplace_back();
            else if (event == Json::parse_event_t::key && !objectKeys.empty())
            {
                if (!objectKeys.back().emplace(parsed.get<std::string>(), true).second) duplicate = true;
            }
            else if (event == Json::parse_event_t::object_end && !objectKeys.empty()) objectKeys.pop_back();
            return true;
        });
        if (duplicate || !detail::OnlyJsonKeys(root, {"format", "version", "rootLocalNodeId", "nodes"}) ||
            root.value("format", std::string{}) != "SelfEngine.Prefab" ||
            !root.contains("version") || !root["version"].is_number_integer())
        {
            return Fail(error, "Prefab.InvalidDocument");
        }
        const auto version = root["version"].get<std::int64_t>();
        if (version != PrefabDocumentVersion) return Fail(error, "Prefab.UnsupportedVersion");

        PrefabDocument parsed;
        parsed.version = static_cast<std::uint32_t>(version);
        if (!root.contains("rootLocalNodeId") || !detail::ReadUint64IdJson(root["rootLocalNodeId"], parsed.rootLocalNodeId) ||
            !root.contains("nodes") || !root["nodes"].is_array() || root["nodes"].empty() ||
            root["nodes"].size() > MaxPrefabNodes)
        {
            return Fail(error, "Prefab.InvalidDocument");
        }

        for (const Json& nodeJson : root["nodes"])
        {
            if (!detail::OnlyJsonKeys(nodeJson, {"localNodeId", "localParentId", "name", "enabled", "components", "scripts"}) ||
                !nodeJson.contains("localNodeId") || !nodeJson.contains("localParentId") ||
                !nodeJson.contains("name") || !nodeJson["name"].is_string() ||
                !nodeJson.contains("enabled") || !nodeJson["enabled"].is_boolean() ||
                !nodeJson.contains("components") || !nodeJson["components"].is_array())
            {
                return Fail(error, "Prefab.InvalidDocument");
            }
            PrefabNodeRecord node;
            if (!detail::ReadUint64IdJson(nodeJson["localNodeId"], node.localNodeId)) return Fail(error, "Prefab.InvalidDocument");
            if (!nodeJson["localParentId"].is_null() && !detail::ReadUint64IdJson(nodeJson["localParentId"], node.localParentId))
            {
                return Fail(error, "Prefab.InvalidDocument");
            }
            node.name = nodeJson["name"].get<std::string>();
            node.enabled = nodeJson["enabled"].get<bool>();

            for (const Json& compJson : nodeJson["components"])
            {
                if (!detail::OnlyJsonKeys(compJson, {"type", "version", "enabled", "properties"}) ||
                    !compJson.contains("type") || !compJson.contains("version") || !compJson["version"].is_number_integer() ||
                    !compJson.contains("enabled") || !compJson["enabled"].is_boolean() ||
                    !compJson.contains("properties") || !compJson["properties"].is_array())
                {
                    return Fail(error, "Prefab.InvalidDocument");
                }
                PrefabComponentRecord comp;
                if (!detail::ReadUint64IdJson(compJson["type"], comp.type)) return Fail(error, "Prefab.InvalidDocument");
                const auto compVer = compJson["version"].get<std::int64_t>();
                if (compVer < 1 || compVer > UINT32_MAX) return Fail(error, "Prefab.InvalidDocument");
                comp.version = static_cast<std::uint32_t>(compVer);
                comp.enabled = compJson["enabled"].get<bool>();

                for (const Json& propJson : compJson["properties"])
                {
                    if (!detail::OnlyJsonKeys(propJson, {"key", "type", "value", "localRef"}) ||
                        !propJson.contains("key") || !propJson["key"].is_string() ||
                        !propJson.contains("type") || !propJson["type"].is_string() ||
                        !propJson.contains("value") ||
                        (propJson.contains("localRef") && !propJson["localRef"].is_boolean()))
                    {
                        return Fail(error, "Prefab.InvalidDocument");
                    }
                    PrefabPropertyRecord prop;
                    prop.key = propJson["key"].get<std::string>();
                    if (!detail::ParsePropertyTypeJsonName(propJson["type"].get<std::string>(), prop.type) ||
                        !detail::ReadTypedPropertyValue(propJson["value"], prop.type, prop.value))
                    {
                        return Fail(error, "Prefab.InvalidDocument");
                    }
                    prop.localEntityReference = propJson.value("localRef", false);
                    if (prop.localEntityReference && prop.type != PropertyType::EntityReference)
                    {
                        return Fail(error, "Prefab.InvalidDocument");
                    }
                    comp.properties.push_back(std::move(prop));
                }
                node.components.push_back(std::move(comp));
            }

            if (nodeJson.contains("scripts"))
            {
                if (!nodeJson["scripts"].is_array()) return Fail(error, "Prefab.InvalidDocument");
                for (const Json& scriptJson : nodeJson["scripts"])
                {
                    if (!detail::OnlyJsonKeys(scriptJson, {"type", "enabled", "fields"}) ||
                        !scriptJson.contains("type") || !scriptJson["type"].is_string() ||
                        !scriptJson.contains("enabled") || !scriptJson["enabled"].is_boolean() ||
                        !scriptJson.contains("fields") || !scriptJson["fields"].is_array())
                    {
                        return Fail(error, "Prefab.InvalidDocument");
                    }
                    PrefabScriptRecord script;
                    script.type = scriptJson["type"].get<std::string>();
                    script.enabled = scriptJson["enabled"].get<bool>();
                    for (const Json& fieldJson : scriptJson["fields"])
                    {
                        if (!detail::OnlyJsonKeys(fieldJson, {"name", "kind", "value", "localNodeRef"}) ||
                            !fieldJson.contains("name") || !fieldJson["name"].is_string() ||
                            !fieldJson.contains("kind") || !fieldJson["kind"].is_string() ||
                            !fieldJson.contains("value"))
                        {
                            return Fail(error, "Prefab.InvalidDocument");
                        }
                        PrefabScriptFieldRecord field;
                        field.name = fieldJson["name"].get<std::string>();
                        field.kind = fieldJson["kind"].get<std::string>();
                        field.valueJson = fieldJson["value"].dump();
                        if (fieldJson.contains("localNodeRef") && !fieldJson["localNodeRef"].is_null())
                        {
                            if (!detail::ReadUint64IdJson(fieldJson["localNodeRef"], field.localNodeRef))
                            {
                                return Fail(error, "Prefab.InvalidDocument");
                            }
                        }
                        script.fields.push_back(std::move(field));
                    }
                    node.scripts.push_back(std::move(script));
                }
            }

            parsed.nodes.push_back(std::move(node));
        }

        if (!ValidateNodeSubtree(parsed, error)) return false;
        document = std::move(parsed);
        return true;
    }
    catch (const Json::exception&)
    {
        return Fail(error, "Prefab.InvalidDocument");
    }
    catch (const std::runtime_error&)
    {
        return Fail(error, "Prefab.InvalidDocument");
    }
}

bool ReadPrefabDocument(const std::filesystem::path& path, PrefabDocument& document, std::string& error)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return Fail(error, "Prefab.ReadFailed");
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > MaxPrefabBytes) return Fail(error, "Prefab.InvalidDocument");
    std::string text(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!text.empty() && !input.read(text.data(), static_cast<std::streamsize>(text.size())))
    {
        return Fail(error, "Prefab.ReadFailed");
    }
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    {
        text.erase(0, 3);
    }
    return ParsePrefabDocument(text, document, error);
}

bool WritePrefabDocument(const std::filesystem::path& path, const PrefabDocument& document,
                         std::string& error, bool replaceExisting)
{
    std::string text;
    if (!SerializePrefabDocument(document, text, error)) return false;
    if (path.empty() || path.filename().empty()) return Fail(error, "Scene.InvalidPath");
    std::error_code fsError;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), fsError);

    const auto temporary = path.parent_path() / (path.filename().wstring() + L".tmp." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(temporarySequence.fetch_add(1)));
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Fail(error, "Prefab.WriteFailed");
    DWORD written = 0;
    const bool stored = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                        written == text.size() && FlushFileBuffers(file);
    const bool closed = CloseHandle(file) != FALSE;
    if (!stored || !closed)
    {
        DeleteFileW(temporary.c_str());
        return Fail(error, "Prefab.WriteFailed");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_WRITE_THROUGH | (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0)))
    {
        DeleteFileW(temporary.c_str());
        return Fail(error, "Prefab.WriteFailed");
    }
    error.clear();
    return true;
}

bool ExtractTransformFromPrefabComponent(const PrefabComponentRecord& component, Transform& outTransform)
{
    outTransform = {};
    for (const PrefabPropertyRecord& prop : component.properties)
    {
        if (prop.key == "position")
        {
            if (const auto* v = std::get_if<Vec3>(&prop.value)) outTransform.position = *v;
            else return false;
        }
        else if (prop.key == "rotation")
        {
            if (const auto* q = std::get_if<Quat>(&prop.value)) outTransform.rotation = *q;
            else return false;
        }
        else if (prop.key == "scale")
        {
            if (const auto* v = std::get_if<Vec3>(&prop.value)) outTransform.scale = *v;
            else return false;
        }
    }
    return IsValid(outTransform);
}
} // namespace se
