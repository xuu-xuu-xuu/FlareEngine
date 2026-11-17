#pragma once

#include "CoreLayer/PropertyMetadata.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace se
{
inline constexpr std::uint32_t PrefabDocumentVersion = 1;
inline constexpr std::string_view PrefabFileExtension = ".prefab.json";
inline constexpr std::string_view PrefabMetadataExtension = ".prefab.json.asset.json";

struct PropertyOverride
{
    std::string key;
    PropertyValue value;
};

struct PrefabLink
{
    std::string prefabId; // 32 位小写十六进制 AssetId
    std::uint64_t localNodeId = 0;
    std::vector<PropertyOverride> overrides;
};

struct PrefabPropertyRecord
{
    std::string key;
    PropertyType type = PropertyType::Text;
    PropertyValue value;
    bool localEntityReference = false; // true 时 value 中保存同预制体内部节点的 localNodeId
};

struct PrefabComponentRecord
{
    std::uint64_t type = 0;
    std::uint32_t version = 1;
    bool enabled = true;
    std::vector<PrefabPropertyRecord> properties;
};

struct PrefabScriptFieldRecord
{
    std::string name;
    std::string kind;
    std::string valueJson;
    std::uint64_t localNodeRef = 0; // kind=="entity" 且指向同预制体节点时 > 0
};

struct PrefabScriptRecord
{
    std::string type;
    bool enabled = true;
    std::vector<PrefabScriptFieldRecord> fields;
};

struct PrefabNodeRecord
{
    std::uint64_t localNodeId = 0;
    std::uint64_t localParentId = 0;
    std::string name;
    bool enabled = true;
    std::vector<PrefabComponentRecord> components;
    std::vector<PrefabScriptRecord> scripts;
};

struct PrefabDocument
{
    std::uint32_t version = PrefabDocumentVersion;
    std::uint64_t rootLocalNodeId = 0;
    std::vector<PrefabNodeRecord> nodes;

    explicit operator bool() const noexcept { return !nodes.empty(); }
};

[[nodiscard]] bool IsPrefabDocumentPath(const std::filesystem::path& path);
[[nodiscard]] bool IsValidAssetIdString(std::string_view id) noexcept;
[[nodiscard]] bool ValidatePrefabDocument(const PrefabDocument& document, std::string& error);
[[nodiscard]] bool SerializePrefabDocument(const PrefabDocument& document, std::string& jsonText, std::string& error);
[[nodiscard]] bool ParsePrefabDocument(std::string_view jsonText, PrefabDocument& document, std::string& error);
[[nodiscard]] bool ReadPrefabDocument(const std::filesystem::path& path, PrefabDocument& document, std::string& error);
[[nodiscard]] bool WritePrefabDocument(const std::filesystem::path& path, const PrefabDocument& document,
                                       std::string& error, bool replaceExisting = true);
[[nodiscard]] bool ExtractTransformFromPrefabComponent(const PrefabComponentRecord& component, Transform& outTransform);
} // namespace se
