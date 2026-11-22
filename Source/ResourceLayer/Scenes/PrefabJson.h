#pragma once

#include "ResourceLayer/Scenes/PrefabDocument.h"
#include "ResourceLayer/PropertyJson.h"
#include <charconv>
#include <string_view>
#include <unordered_set>

namespace flare::detail
{
inline bool OnlyJsonKeys(const nlohmann::json& object, std::initializer_list<std::string_view> keys)
{
    if (!object.is_object()) return false;
    for (auto it = object.begin(); it != object.end(); ++it)
    {
        bool allowed = false;
        for (const std::string_view key : keys)
        {
            if (it.key() == key) { allowed = true; break; }
        }
        if (!allowed) return false;
    }
    return true;
}

inline std::string_view PropertyTypeJsonName(PropertyType type) noexcept
{
    switch (type)
    {
    case PropertyType::Boolean: return "boolean";
    case PropertyType::Number: return "number";
    case PropertyType::Vector3: return "vector3";
    case PropertyType::Quaternion: return "quaternion";
    case PropertyType::AssetId: return "assetId";
    case PropertyType::AssetList: return "assetList";
    case PropertyType::Text: return "text";
    case PropertyType::Vector4: return "vector4";
    case PropertyType::EntityReference: return "entityReference";
    }
    return "text";
}

inline PropertyType InferPropertyTypeFromValue(const PropertyValue& value) noexcept
{
    if (std::holds_alternative<bool>(value)) return PropertyType::Boolean;
    if (std::holds_alternative<float>(value)) return PropertyType::Number;
    if (std::holds_alternative<Vec3>(value)) return PropertyType::Vector3;
    if (std::holds_alternative<Quat>(value)) return PropertyType::Quaternion;
    if (std::holds_alternative<Vec4>(value)) return PropertyType::Vector4;
    if (std::holds_alternative<std::vector<std::string>>(value)) return PropertyType::AssetList;
    if (std::holds_alternative<PropertyEntityReference>(value)) return PropertyType::EntityReference;
    return PropertyType::Text;
}

inline bool ParsePropertyTypeJsonName(std::string_view name, PropertyType& outType) noexcept
{
    if (name == "boolean") { outType = PropertyType::Boolean; return true; }
    if (name == "number") { outType = PropertyType::Number; return true; }
    if (name == "vector3") { outType = PropertyType::Vector3; return true; }
    if (name == "quaternion") { outType = PropertyType::Quaternion; return true; }
    if (name == "assetId") { outType = PropertyType::AssetId; return true; }
    if (name == "assetList") { outType = PropertyType::AssetList; return true; }
    if (name == "text") { outType = PropertyType::Text; return true; }
    if (name == "vector4") { outType = PropertyType::Vector4; return true; }
    if (name == "entityReference") { outType = PropertyType::EntityReference; return true; }
    return false;
}

inline PropertyValue DefaultValueForPropertyType(PropertyType type)
{
    switch (type)
    {
    case PropertyType::Boolean: return false;
    case PropertyType::Number: return 0.0f;
    case PropertyType::Vector3: return Vec3{};
    case PropertyType::Quaternion: return Quat{};
    case PropertyType::AssetId:
    case PropertyType::Text: return std::string{};
    case PropertyType::AssetList: return std::vector<std::string>{};
    case PropertyType::Vector4: return Vec4{};
    case PropertyType::EntityReference: return PropertyEntityReference{};
    }
    return std::string{};
}

inline bool ReadTypedPropertyValue(const nlohmann::json& jsonValue, PropertyType type, PropertyValue& output)
{
    PropertyDescriptor descriptor{};
    descriptor.type = type;
    descriptor.defaultValue = DefaultValueForPropertyType(type);
    if (type == PropertyType::Quaternion)
    {
        // 与 PropertyJson 对齐：非零四元数要求有效模长。
        descriptor.defaultValue = Quat{0.0f, 0.0f, 0.0f, 1.0f};
    }
    return ReadPropertyJson(jsonValue, descriptor, output);
}

inline nlohmann::json WritePrefabLinkJson(const PrefabLink& link)
{
    nlohmann::json overrides = nlohmann::json::array();
    for (const PropertyOverride& item : link.overrides)
    {
        const PropertyType type = InferPropertyTypeFromValue(item.value);
        overrides.push_back({
            {"key", item.key},
            {"type", PropertyTypeJsonName(type)},
            {"value", WritePropertyJson(item.value)}
        });
    }
    return {
        {"prefabId", link.prefabId},
        {"localNodeId", std::to_string(link.localNodeId)},
        {"overrides", std::move(overrides)}
    };
}

inline bool ReadUint64IdJson(const nlohmann::json& json, std::uint64_t& outId) noexcept
{
    if (!json.is_string()) return false;
    const auto& text = json.get_ref<const std::string&>();
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), outId);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && outId != 0;
}

inline bool ReadPrefabLinkJson(const nlohmann::json& json, PrefabLink& outLink, std::string& error)
{
    if (!OnlyJsonKeys(json, {"prefabId", "localNodeId", "overrides"}) ||
        !json.contains("prefabId") || !json["prefabId"].is_string() ||
        !json.contains("localNodeId") || !json.contains("overrides") || !json["overrides"].is_array())
    {
        error = "Scene.InvalidData";
        return false;
    }
    PrefabLink link;
    link.prefabId = json["prefabId"].get<std::string>();
    if (!IsValidAssetIdString(link.prefabId) || !ReadUint64IdJson(json["localNodeId"], link.localNodeId))
    {
        error = "Scene.InvalidData";
        return false;
    }
    if (json["overrides"].size() > 4096)
    {
        error = "Scene.TooLarge";
        return false;
    }
    std::unordered_set<std::string> seenKeys;
    for (const auto& entry : json["overrides"])
    {
        if (!OnlyJsonKeys(entry, {"key", "type", "value"}) ||
            !entry.contains("key") || !entry["key"].is_string() ||
            !entry.contains("type") || !entry["type"].is_string() ||
            !entry.contains("value"))
        {
            error = "Scene.InvalidData";
            return false;
        }
        PropertyOverride item;
        item.key = entry["key"].get<std::string>();
        PropertyType type = PropertyType::Text;
        if (item.key.empty() || !seenKeys.insert(item.key).second ||
            !ParsePropertyTypeJsonName(entry["type"].get<std::string>(), type) ||
            !ReadTypedPropertyValue(entry["value"], type, item.value))
        {
            error = "Scene.InvalidData";
            return false;
        }
        link.overrides.push_back(std::move(item));
    }
    outLink = std::move(link);
    return true;
}
} // namespace flare::detail
