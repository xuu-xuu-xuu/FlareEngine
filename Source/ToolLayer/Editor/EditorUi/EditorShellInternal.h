#pragma once

#include "ToolLayer/Editor/EditorUi/EditorShell.h"

#include "ToolLayer/Editor/EditorUi/EditorDockState.h"
#include "ToolLayer/Editor/EditorUi/EditorInk.h"
#include "ToolLayer/Editor/FlarePalette.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace flare::ui::editor::detail
{

inline constexpr float kTabHeight = 24.0f;
inline constexpr float kMinimumGroupSize = 80.0f;
inline constexpr float kSplitterReach = 3.5f;
inline constexpr float kTabArrowWidth = 18.0f;

inline const auto kBackground = UiInk(flare::palette::Background);
inline const auto kSurface = UiInk(flare::palette::workspace::Field);
inline const auto kSurfaceRaised = UiInk(flare::palette::workspace::Header);
inline const auto kSurfaceActive = UiInk(flare::palette::workspace::Selection);
inline const auto kContent = UiInk(flare::palette::workspace::Panel);
inline const auto kBorder = UiInk(flare::palette::workspace::Divider);
inline const auto kText = UiInk(flare::palette::Text);
inline const auto kMuted = UiInk(flare::palette::Muted);
inline const auto kAccent = UiInk(flare::palette::workspace::Focus);

[[nodiscard]] inline std::array<UiRect, 2> SplitDockRects(const DockNode& node, const UiRect& rect)
{
    const float total = node.axis == DockAxis::Horizontal ? rect.width : rect.height;
    const float minimum = total > 0.0f ? std::min(0.45f, kMinimumGroupSize / total) : 0.0f;
    const float ratio = std::clamp(node.ratio, minimum, 1.0f - minimum);
    std::array<UiRect, 2> children{rect, rect};
    if (node.axis == DockAxis::Horizontal)
    {
        children[0].width = total * ratio;
        children[1].x = rect.x + children[0].width;
        children[1].width = std::max(0.0f, total - children[0].width);
    }
    else
    {
        children[0].height = total * ratio;
        children[1].y = rect.y + children[0].height;
        children[1].height = std::max(0.0f, total - children[0].height);
    }
    return children;
}

[[nodiscard]] inline UiRect DockPanelRect(const DockNode& node, EditorPanelId panel, const UiRect& rect)
{
    if (node.kind == DockNode::Kind::Tabs)
    {
        return std::find(node.tabs.begin(), node.tabs.end(), panel) != node.tabs.end() ? rect : UiRect{};
    }
    if (node.children.size() != 2)
    {
        return {};
    }
    const auto children = SplitDockRects(node, rect);
    for (size_t index = 0; index < 2; ++index)
    {
        const auto found = DockPanelRect(node.children[index], panel, children[index]);
        if (found.width > 0 && found.height > 0)
        {
            return found;
        }
    }
    return {};
}

[[nodiscard]] inline UiStyle AbsoluteStyle(float x, float y, float width, float height)
{
    UiStyle style;
    style.position = UiPosition::Absolute;
    style.left = UiLength::Px(x);
    style.top = UiLength::Px(y);
    style.width = UiLength::Px(width);
    style.height = UiLength::Px(height);
    return style;
}

[[nodiscard]] inline UiStyle PanelStyle(const UiRect& rect)
{
    UiStyle style = AbsoluteStyle(rect.x, rect.y, rect.width, rect.height);
    style.background = kSurface;
    style.borderWidth = 1.0f;
    style.borderColor = kBorder;
    style.flexDirection = UiFlexDirection::Column;
    return style;
}

[[nodiscard]] inline UiStyle ButtonStyle(float x, float y, float width, float height, const UiColor& background)
{
    UiStyle style = AbsoluteStyle(x, y, width, height);
    style.background = background;
    style.color = kText;
    style.fontSize = 13.0f;
    style.textFlow = UiTextFlow::SingleLineEllipsis;
    const float inset = width <= 20.0f ? 2.0f : 6.0f;
    style.padding = {inset, 0.0f, inset, 0.0f};
    return style;
}

// 标题栏太窄时只保留当前页与切换箭头。绘制和插入线使用同一套宽度。
struct TabStripMetrics
{
    float scale = 1.0f;
    float width = 0.0f;
    bool paged = false;
};

[[nodiscard]] inline TabStripMetrics MeasureTabStrip(const std::vector<float>& widths, float area)
{
    if (widths.empty())
    {
        return {};
    }
    const float space = std::max(0.0f, area);
    float natural = 0.0f;
    for (float width : widths)
    {
        natural += width;
    }
    const float scale = std::min(1.0f, space / natural);
    if (natural > space && widths.size() > 1 && *std::min_element(widths.begin(), widths.end()) * scale < 56.0f)
    {
        return {1.0f, std::max(0.0f, area - 2.0f * kTabArrowWidth), true};
    }
    return {scale, area, false};
}

[[nodiscard]] inline std::size_t Utf8Length(std::string_view text)
{
    std::size_t length = 0;
    for (const char ch : text)
    {
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80)
        {
            ++length;
        }
    }
    return length;
}

[[nodiscard]] inline float FallbackTextWidth(const std::string& label, float fontSize)
{
    return static_cast<float>(Utf8Length(label)) * fontSize * 0.62f;
}

[[nodiscard]] inline UiColor RowBackgroundColor(const EditorProperty& property)
{
    if (property.type != EditorPropertyType::Color)
    {
        return UiColor{0.0f, 0.0f, 0.0f, 0.0f};
    }
    float channels[4]{0.0f, 0.0f, 0.0f, 1.0f};
    const char* cursor = property.value.c_str();
    int count = 0;
    while (count < 4)
    {
        char* end = nullptr;
        const float value = std::strtof(cursor, &end);
        if (end == cursor)
        {
            break;
        }
        channels[count++] = std::clamp(value, 0.0f, 1.0f);
        cursor = end;
        while (*cursor == ' ' || *cursor == ',' || *cursor == '\t')
        {
            ++cursor;
        }
    }
    if (count < 3)
    {
        return UiColor{0.0f, 0.0f, 0.0f, 0.0f};
    }
    // 半透明叠加，保留白色行文字可读性；颜色本身仍可见。
    return UiColor{channels[0], channels[1], channels[2], 0.38f};
}

} // namespace flare::ui::editor::detail

namespace flare::ui::editor
{
struct EditorAssetDragState
{
    std::string assetPath;
    float startX = 0.0f;
    float startY = 0.0f;
    bool dragging = false;
    bool isFolder = false;
    std::function<void(const std::string&, float, float, bool)> hoverHandler;
    std::function<bool(const std::string&, float, float)> dropHandler;
};

struct EditorFormPanel
{
    UiElementHandle listContent{};
    UiElementHandle editElement{};
    UiElementHandle saveButton{};
    UiElementHandle actionButton{};
    UiElementHandle statusLabel{};
    std::unique_ptr<UiListView> list;
    std::unique_ptr<UiTextEdit> edit;
    std::vector<EditorProperty> properties;
    std::string editingKey;
    std::function<bool(std::string, std::string)> commit;
    std::function<bool()> save;
    std::function<bool()> action;
    std::string actionLabel;
    std::string status;
    UiElementHandle preview{};
    std::string previewImage;
    float previewAspect = 1.0f;
    bool managedStatus = false;
    UiElementHandle comboPopup{};
    std::vector<UiElementHandle> comboOptions;
    int comboRow = -1;
};

struct EditorInspectorDrag
{
    EditorProperty field;
    std::uint64_t target = 0;
    std::uint32_t surface = 0;
    double original = 0, value = 0, applied = 0;
    float startX = 0, lastX = 0;
    bool active = false;
    bool sliderTrack = false;
    float sliderLeft = 0.0f;
    float sliderWidth = 1.0f;
};

struct EditorInspectorColor
{
    std::uint64_t target = 0;
    std::uint32_t surface = 0;
    std::string key;
    UiColor original{}, current{};
    float hue = 0, saturation = 0, value = 0, paintedHue = -1;
    int dragging = 0;
    UiRect anchor{};
    UiElementHandle backdrop{}, panel{}, square{}, hueBar{}, cursor{}, cursorInner{}, hueCursor{}, title{};
    UiElementHandle previous{}, preview{}, previousLabel{}, previewLabel{}, apply{}, cancel{};
    std::vector<UiElementHandle> tiles, hues;
    std::array<UiElementHandle, 3> channels{}, channelLabels{}, channelSelections{};
    std::array<std::unique_ptr<UiTextEdit>, 3> editors;
};

struct EditorShaderGraphRuntime
{
    EditorShaderGraphViewState view;
    std::vector<EditorShaderPaletteItem> palette;
    std::function<void(std::string_view, std::string_view)> onAction;

    UiElementHandle toolbar{};
    UiElementHandle titleLabel{};
    UiElementHandle shadingBtn{};
    UiElementHandle blendBtn{};
    UiElementHandle doubleSidedBtn{};
    UiElementHandle addNodeBtn{};
    UiElementHandle frameBtn{};
    UiElementHandle saveBtn{};
    UiElementHandle hlslBtn{};
    UiElementHandle newMatBtn{};

    UiElementHandle propBar{};
    UiElementHandle exposeBtn{};
    UiElementHandle paramNameBox{}, paramNameSel{};
    std::unique_ptr<UiTextEdit> paramNameEdit;
    float paramNameScroll = 0.0f;
    UiElementHandle valueBox{}, valueSel{};
    std::unique_ptr<UiTextEdit> valueEdit;
    float valueScroll = 0.0f;
    std::string editingField; // "param" | "value" | "search"

    UiElementHandle canvas{};
    UiElementHandle previewCard{};
    UiElementHandle previewImage{};
    UiElementHandle previewCaption{};

    UiElementHandle searchPopup{};
    UiElementHandle searchBox{}, searchSel{};
    UiElementHandle searchCountLabel{};
    std::unique_ptr<UiTextEdit> searchEdit;
    float searchScroll = 0.0f;
    std::string searchCategoryFilter;
    std::vector<UiElementHandle> searchCategoryButtons;
    std::vector<UiElementHandle> searchItemButtons;
    std::vector<std::size_t> filteredPaletteIndices;
    std::size_t searchHighlight = 0;
    std::size_t searchListOffset = 0;
    float spawnGraphX = 220.0f;
    float spawnGraphY = 120.0f;
    std::string pendingWireDir; // "" | "from_out" | "from_in"
    std::uint32_t pendingWireNode = 0;
    std::string pendingWirePort;

    // Interactive canvas state
    bool panning = false;
    float panStartMouseX = 0.0f, panStartMouseY = 0.0f;
    float panStartX = 0.0f, panStartY = 0.0f;

    bool draggingNodes = false;
    bool nodesMoved = false;
    float dragLastMouseX = 0.0f, dragLastMouseY = 0.0f;

    bool marqueeActive = false;
    float marqueeStartX = 0.0f, marqueeStartY = 0.0f;
    float marqueeCurX = 0.0f, marqueeCurY = 0.0f;

    bool draggingWire = false;
    bool wireReplugging = false;
    bool wireFromOutput = true;
    std::uint32_t wireNodeId = 0;
    std::string wirePortId;
    std::uint8_t wireValueType = 0;
    float wireStartX = 0.0f, wireStartY = 0.0f;
    float wireCurX = 0.0f, wireCurY = 0.0f;

    std::uint32_t snapNodeId = 0;
    std::string snapPortId;
    bool snapIsOutput = false;
    float snapPortX = 0.0f, snapPortY = 0.0f;

    std::uint32_t hoveredNodeId = 0;
    std::string hoveredPortId;
    bool hoveredPortOutput = false;
    std::size_t hoveredLink = static_cast<std::size_t>(-1);

    bool orbitingPreview = false;
    float orbitLastX = 0.0f, orbitLastY = 0.0f;
    bool rootShortcutsBound = false;
};
} // namespace flare::ui::editor
