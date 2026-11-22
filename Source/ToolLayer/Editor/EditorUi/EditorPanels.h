#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flare::ui::editor
{

struct EditorHierarchyNode
{
    std::uint64_t id = 0;
    std::uint64_t parent = 0;
    std::size_t depth = 0;
    bool hasChildren = false;
    bool expanded = true;
    std::string label;
    bool enabled = true;
    bool prefab = false;
    std::string iconKind;

    bool operator==(const EditorHierarchyNode&) const = default;
};

class EditorHierarchyModel
{
public:
    bool SetNodes(std::vector<EditorHierarchyNode> nodes);
    [[nodiscard]] const std::vector<EditorHierarchyNode>& Nodes() const noexcept;
    [[nodiscard]] std::vector<std::size_t> VisibleRows() const;
    bool Toggle(std::size_t index);
    bool Select(std::uint64_t id);
    bool SetSelection(const std::vector<std::uint64_t>& ids, std::uint64_t primary = 0);
    bool ToggleSelection(std::uint64_t id);
    bool SelectRange(std::uint64_t toId);
    bool ExpandAncestors(std::uint64_t id);
    [[nodiscard]] std::uint64_t Selected() const noexcept;
    [[nodiscard]] const std::vector<std::uint64_t>& SelectedIds() const noexcept { return selectedIds_; }
    [[nodiscard]] bool IsSelected(std::uint64_t id) const noexcept;
    bool Rename(std::uint64_t id, std::string label);
    bool Reparent(std::uint64_t id, std::uint64_t parent);

private:
    [[nodiscard]] bool Visible(std::size_t index) const;
    std::vector<EditorHierarchyNode> nodes_;
    std::uint64_t selected_ = 0;
    std::vector<std::uint64_t> selectedIds_;
};

enum class EditorPropertyType : std::uint8_t
{
    String = 0,
    Number,
    Boolean,
    Asset,
    Color,
    Combo,
    Action,
    Subheader,
    EntityRef,
};

struct EditorProperty
{
    std::string key;
    std::string label;
    EditorPropertyType type = EditorPropertyType::String;
    std::string value;
    bool readOnly = false;
    std::vector<std::string> options;
    // 属性来源描述分组与轴，Inspector 无需解析绑定命令或猜测本地化名称。
    std::string section;
    bool sectionInitiallyClosed = false;
    char axis = 0;
    std::string displayValue;
    bool sectionEnabled = false;
    // 显示标签与持久选项值分开；数值拖动由属性来源显式声明。
    std::vector<std::string> optionLabels;
    float dragStep = 0;
    std::optional<float> dragMinimum;
    std::optional<float> dragMaximum;
    bool overridden = false;
    bool slider = false;
    bool headerMenuAction = false;
    bool mixedValue = false;
    std::string tooltip;
    std::string acceptedAssetKind;
    float spaceBefore = 0.0f;
    std::string unit;
    std::string colorSpace;
    bool deferredCommit = false;
    std::uint8_t diagnosticLevel = 0; // 0=无, 1=信息, 2=警告, 3=错误
    std::string fixActionKey;
    std::string fixActionLabel;
    std::string spatialGizmoKey;
    bool multiFlag = false;

    bool operator==(const EditorProperty&) const = default;
};

[[nodiscard]] bool IsAssetPathCompatible(std::string_view assetPathUtf8, std::string_view acceptedKind);

class EditorInspectorModel
{
public:
    void SetTarget(std::uint64_t id, std::string label);
    [[nodiscard]] std::uint64_t Target() const noexcept;
    [[nodiscard]] const std::string& TargetLabel() const noexcept;
    void SetProperties(std::vector<EditorProperty> properties);
    [[nodiscard]] const std::vector<EditorProperty>& Properties() const noexcept;
    bool SetValue(std::string_view key, std::string value);
    [[nodiscard]] bool ConsumeChanged();
    [[nodiscard]] const std::string& LastChangedKey() const noexcept;

private:
    std::uint64_t target_ = 0;
    std::string targetLabel_;
    std::vector<EditorProperty> properties_;
    bool changed_ = false;
    std::string lastChangedKey_;
};

struct EditorAssetEntry
{
    std::string path;
    std::string type;
    std::string relativeDir;
    std::string displayName;
    std::string thumbnail;
    std::size_t depth = 0;
    bool hasChildren = false;
    bool expanded = true;

    bool operator==(const EditorAssetEntry&) const = default;
};

class EditorAssetsModel
{
public:
    void SetEntries(std::vector<EditorAssetEntry> entries);
    [[nodiscard]] const std::vector<EditorAssetEntry>& Entries() const noexcept;
    void SetFilter(std::string_view filter);
    [[nodiscard]] const std::string& Filter() const noexcept { return filter_; }
    void SetTypeFilter(std::string_view type);
    [[nodiscard]] const std::string& TypeFilter() const noexcept { return typeFilter_; }
    void SetCurrentFolder(std::string folderPath);
    [[nodiscard]] const std::string& CurrentFolder() const noexcept { return currentFolder_; }
    bool ToggleFolder(std::string_view folderPath);
    void ExpandAllFolders();
    [[nodiscard]] bool IsFolderExpanded(std::string_view folderPath) const;
    void SetGridMode(bool grid) noexcept { gridMode_ = grid; }
    [[nodiscard]] bool GridMode() const noexcept { return gridMode_; }
    [[nodiscard]] const std::vector<EditorAssetEntry>& Filtered() const noexcept;
    [[nodiscard]] std::vector<EditorAssetEntry> VisibleRange(std::size_t offset, float viewportHeight,
                                                            float rowHeight, std::size_t bufferRows) const;
    [[nodiscard]] bool Selected(std::string_view path) const noexcept;
    void Select(std::string path);
    [[nodiscard]] const std::string& SelectedPath() const noexcept;

private:
    void RebuildFiltered();
    std::vector<EditorAssetEntry> entries_;
    std::vector<EditorAssetEntry> filtered_;
    std::vector<std::pair<std::string, bool>> folderExpansion_;
    std::string filter_;
    std::string typeFilter_;
    std::string currentFolder_;
    std::string selected_;
    bool gridMode_ = false;
};

} // namespace flare::ui::editor
