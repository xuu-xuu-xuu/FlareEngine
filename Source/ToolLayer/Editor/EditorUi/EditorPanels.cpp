#include "ToolLayer/Editor/EditorUi/EditorPanels.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>

namespace se::ui::editor
{
namespace
{
char Lower(char value) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool ContainsInsensitive(std::string_view text, std::string_view needle)
{
    if (needle.empty())
    {
        return true;
    }
    if (needle.size() > text.size())
    {
        return false;
    }
    for (std::size_t index = 0; index + needle.size() <= text.size(); ++index)
    {
        bool match = true;
        for (std::size_t offset = 0; offset < needle.size(); ++offset)
        {
            if (Lower(text[index + offset]) != Lower(needle[offset]))
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }
    return false;
}
} // namespace

bool EditorHierarchyModel::SetNodes(std::vector<EditorHierarchyNode> nodes)
{
    const std::uint64_t previous = selected_;
    std::unordered_map<std::uint64_t, bool> expanded;
    for (const EditorHierarchyNode& node : nodes_) expanded[node.id] = node.expanded;
    for (EditorHierarchyNode& node : nodes)
    {
        const auto found = expanded.find(node.id);
        if (found != expanded.end()) node.expanded = found->second;
    }
    const std::uint64_t nextSelected = std::none_of(nodes.begin(), nodes.end(), [previous](const EditorHierarchyNode& node) {
        return node.id == previous;
    }) ? 0 : previous;
    if (nodes == nodes_ && nextSelected == selected_)
    {
        return false;
    }
    nodes_ = std::move(nodes);
    selected_ = nextSelected;
    return true;
}

const std::vector<EditorHierarchyNode>& EditorHierarchyModel::Nodes() const noexcept
{
    return nodes_;
}

bool EditorHierarchyModel::Visible(std::size_t index) const
{
    if (index >= nodes_.size())
    {
        return false;
    }
    std::unordered_map<std::uint64_t, std::uint64_t> parents;
    std::unordered_map<std::uint64_t, bool> expanded;
    for (const EditorHierarchyNode& node : nodes_)
    {
        parents[node.id] = node.parent;
        expanded[node.id] = node.expanded;
    }
    std::uint64_t parent = nodes_[index].parent;
    std::size_t guard = 0;
    while (parent != 0 && guard++ < nodes_.size())
    {
        if (!expanded[parent])
        {
            return false;
        }
        parent = parents[parent];
    }
    return true;
}

std::vector<std::size_t> EditorHierarchyModel::VisibleRows() const
{
    std::vector<std::size_t> rows;
    for (std::size_t index = 0; index < nodes_.size(); ++index)
    {
        if (Visible(index))
        {
            rows.push_back(index);
        }
    }
    return rows;
}

bool EditorHierarchyModel::Toggle(std::size_t index)
{
    if (index >= nodes_.size() || !nodes_[index].hasChildren)
    {
        return false;
    }
    nodes_[index].expanded = !nodes_[index].expanded;
    return true;
}

bool EditorHierarchyModel::Select(std::uint64_t id)
{
    if (id != 0 && std::none_of(nodes_.begin(), nodes_.end(), [id](const EditorHierarchyNode& node) {
            return node.id == id;
        }))
    {
        return false;
    }
    selected_ = id;
    return true;
}

std::uint64_t EditorHierarchyModel::Selected() const noexcept
{
    return selected_;
}

bool EditorHierarchyModel::Rename(std::uint64_t id, std::string label)
{
    for (EditorHierarchyNode& node : nodes_)
    {
        if (node.id == id)
        {
            node.label = std::move(label);
            return true;
        }
    }
    return false;
}

bool EditorHierarchyModel::Reparent(std::uint64_t id, std::uint64_t parent)
{
    if (id == 0 || id == parent)
    {
        return false;
    }
    auto node = std::find_if(nodes_.begin(), nodes_.end(), [id](const EditorHierarchyNode& item) {
        return item.id == id;
    });
    if (node == nodes_.end())
    {
        return false;
    }
    std::uint64_t cursor = parent;
    std::size_t guard = 0;
    while (cursor != 0 && guard++ < nodes_.size())
    {
        if (cursor == id)
        {
            return false;
        }
        const auto found = std::find_if(nodes_.begin(), nodes_.end(), [cursor](const EditorHierarchyNode& item) {
            return item.id == cursor;
        });
        if (found == nodes_.end())
        {
            return false;
        }
        cursor = found->parent;
    }
    node->parent = parent;
    return true;
}

void EditorInspectorModel::SetTarget(std::uint64_t id, std::string label)
{
    target_ = id;
    targetLabel_ = std::move(label);
    properties_.clear();
    changed_ = false;
    lastChangedKey_.clear();
}

std::uint64_t EditorInspectorModel::Target() const noexcept
{
    return target_;
}

const std::string& EditorInspectorModel::TargetLabel() const noexcept
{
    return targetLabel_;
}

void EditorInspectorModel::SetProperties(std::vector<EditorProperty> properties)
{
    properties_ = std::move(properties);
    changed_ = false;
    lastChangedKey_.clear();
}

const std::vector<EditorProperty>& EditorInspectorModel::Properties() const noexcept
{
    return properties_;
}

bool EditorInspectorModel::SetValue(std::string_view key, std::string value)
{
    for (EditorProperty& property : properties_)
    {
        if (property.key == key)
        {
            if (property.readOnly || property.value == value)
            {
                return false;
            }
            property.value = std::move(value);
            changed_ = true;
            lastChangedKey_.assign(key);
            return true;
        }
    }
    return false;
}

bool EditorInspectorModel::ConsumeChanged()
{
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

const std::string& EditorInspectorModel::LastChangedKey() const noexcept
{
    return lastChangedKey_;
}

const std::vector<EditorAssetEntry>& EditorAssetsModel::Entries() const noexcept
{
    return entries_;
}

void EditorAssetsModel::SetEntries(std::vector<EditorAssetEntry> entries)
{
    entries_ = std::move(entries);
    RebuildFiltered();
}

void EditorAssetsModel::SetFilter(std::string_view filter)
{
    filter_.assign(filter);
    RebuildFiltered();
}

void EditorAssetsModel::SetTypeFilter(std::string_view type)
{
    typeFilter_.assign(type);
    RebuildFiltered();
}

void EditorAssetsModel::SetCurrentFolder(std::string folderPath)
{
    if (currentFolder_ == folderPath) return;
    currentFolder_ = std::move(folderPath);
    RebuildFiltered();
}

bool EditorAssetsModel::IsFolderExpanded(std::string_view folderPath) const
{
    for (const auto& [key, open] : folderExpansion_)
    {
        if (key == folderPath) return open;
    }
    return true;
}

bool EditorAssetsModel::ToggleFolder(std::string_view folderPath)
{
    for (auto& [key, open] : folderExpansion_)
    {
        if (key == folderPath)
        {
            open = !open;
            RebuildFiltered();
            return true;
        }
    }
    folderExpansion_.emplace_back(std::string(folderPath), false);
    RebuildFiltered();
    return true;
}

void EditorAssetsModel::RebuildFiltered()
{
    filtered_.clear();
    const bool hasFolders = std::any_of(entries_.begin(), entries_.end(),
                                        [](const EditorAssetEntry& e) { return e.type == "folder"; });
    const bool flatSearch = !filter_.empty() || !typeFilter_.empty();

    if (!hasFolders || flatSearch)
    {
        for (const EditorAssetEntry& entry : entries_)
        {
            if (flatSearch && entry.type == "folder" && !typeFilter_.empty()) continue;
            const bool textMatch = filter_.empty() || ContainsInsensitive(entry.path, filter_) ||
                                   ContainsInsensitive(entry.displayName, filter_);
            const bool typeMatch = typeFilter_.empty() || entry.type == typeFilter_;
            if (textMatch && typeMatch)
            {
                EditorAssetEntry copy = entry;
                copy.depth = 0;
                copy.hasChildren = false;
                filtered_.push_back(std::move(copy));
            }
        }
    }
    else if (!currentFolder_.empty())
    {
        // 面包屑定位到指定目录时，展示该目录的直接子文件夹与直接子资产。
        std::vector<EditorAssetEntry> folders;
        std::vector<EditorAssetEntry> files;
        for (const EditorAssetEntry& entry : entries_)
        {
            if (entry.relativeDir != currentFolder_) continue;
            EditorAssetEntry copy = entry;
            copy.depth = 0;
            copy.hasChildren = false;
            if (copy.type == "folder") folders.push_back(std::move(copy));
            else files.push_back(std::move(copy));
        }
        const auto byName = [](const EditorAssetEntry& a, const EditorAssetEntry& b) {
            const std::string_view an = a.displayName.empty() ? std::string_view(a.path) : std::string_view(a.displayName);
            const std::string_view bn = b.displayName.empty() ? std::string_view(b.path) : std::string_view(b.displayName);
            return an < bn;
        };
        std::sort(folders.begin(), folders.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        for (auto& f : folders) filtered_.push_back(std::move(f));
        for (auto& f : files) filtered_.push_back(std::move(f));
    }
    else
    {
        // 单栏目录树视图：按 relativeDir 层级递归组织文件夹与子资产。
        std::unordered_map<std::string, std::vector<const EditorAssetEntry*>> childFolders;
        std::unordered_map<std::string, std::vector<const EditorAssetEntry*>> childFiles;
        std::unordered_map<std::string, std::string> folderRelKey;
        for (const EditorAssetEntry& entry : entries_)
        {
            if (entry.type == "folder")
            {
                childFolders[entry.relativeDir].push_back(&entry);
                const std::string selfRel = entry.relativeDir.empty()
                    ? entry.displayName
                    : (entry.relativeDir + "/" + entry.displayName);
                folderRelKey[entry.path] = selfRel;
            }
            else
            {
                childFiles[entry.relativeDir].push_back(&entry);
            }
        }
        const auto byPtrName = [](const EditorAssetEntry* a, const EditorAssetEntry* b) {
            const std::string_view an = a->displayName.empty() ? std::string_view(a->path) : std::string_view(a->displayName);
            const std::string_view bn = b->displayName.empty() ? std::string_view(b->path) : std::string_view(b->displayName);
            return an < bn;
        };
        for (auto& [k, vec] : childFolders) std::sort(vec.begin(), vec.end(), byPtrName);
        for (auto& [k, vec] : childFiles) std::sort(vec.begin(), vec.end(), byPtrName);

        const auto emitDir = [&](const auto& self, const std::string& relDir, std::size_t depth) -> void {
            if (const auto fit = childFolders.find(relDir); fit != childFolders.end())
            {
                for (const EditorAssetEntry* folder : fit->second)
                {
                    const std::string& subKey = folderRelKey[folder->path];
                    const bool hasSub = (childFolders.find(subKey) != childFolders.end() && !childFolders[subKey].empty()) ||
                                        (childFiles.find(subKey) != childFiles.end() && !childFiles[subKey].empty());
                    const bool open = IsFolderExpanded(folder->path);
                    EditorAssetEntry row = *folder;
                    row.depth = depth;
                    row.hasChildren = hasSub;
                    row.expanded = open;
                    filtered_.push_back(std::move(row));
                    if (open && depth < 16)
                    {
                        self(self, subKey, depth + 1);
                    }
                }
            }
            if (const auto ait = childFiles.find(relDir); ait != childFiles.end())
            {
                for (const EditorAssetEntry* asset : ait->second)
                {
                    EditorAssetEntry row = *asset;
                    row.depth = depth;
                    row.hasChildren = false;
                    filtered_.push_back(std::move(row));
                }
            }
        };
        emitDir(emitDir, "", 0);
    }

    if (!selected_.empty() &&
        std::none_of(entries_.begin(), entries_.end(), [this](const EditorAssetEntry& entry) {
            return entry.path == selected_;
        }))
    {
        selected_.clear();
    }
}

const std::vector<EditorAssetEntry>& EditorAssetsModel::Filtered() const noexcept
{
    return filtered_;
}

std::vector<EditorAssetEntry> EditorAssetsModel::VisibleRange(std::size_t offset, float viewportHeight,
                                                              float rowHeight, std::size_t bufferRows) const
{
    std::vector<EditorAssetEntry> rows;
    if (rowHeight <= 0.0f || viewportHeight <= 0.0f || filtered_.empty())
    {
        return rows;
    }
    const std::size_t first = offset > 0 ? offset / static_cast<std::size_t>(rowHeight) : 0;
    const std::size_t last = static_cast<std::size_t>(std::ceil((static_cast<float>(offset) + viewportHeight) /
                                                                rowHeight));
    const std::size_t begin = first > bufferRows ? first - bufferRows : 0;
    const std::size_t end = std::min(filtered_.size(), last + bufferRows);
    for (std::size_t index = begin; index < end; ++index)
    {
        rows.push_back(filtered_[index]);
    }
    return rows;
}

bool EditorAssetsModel::Selected(std::string_view path) const noexcept
{
    return selected_ == path;
}

void EditorAssetsModel::Select(std::string path)
{
    selected_ = std::move(path);
}

const std::string& EditorAssetsModel::SelectedPath() const noexcept
{
    return selected_;
}

} // namespace se::ui::editor
