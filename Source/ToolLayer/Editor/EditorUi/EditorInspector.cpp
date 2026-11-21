#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/EditorUi/EditorShellInternal.h"
#include "ToolLayer/Editor/EditorUi/EditorInk.h"
#include "CoreLayer/Utf.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <utility>

namespace se::ui::editor
{
namespace
{
constexpr float RowHeight = 24.0f;
constexpr float CompactWidth = 224.0f;
constexpr float ScrollMargin = 9.0f;
constexpr float LeftInset = 8.0f;
constexpr float CardInset = 3.0f;
const UiColor Transparent{0, 0, 0, 0};

UiStyle CellStyle(float x, float y, float width, float height, UiColor background)
{
    UiStyle style;
    style.position = UiPosition::Absolute;
    style.left = UiLength::Px(x);
    style.top = UiLength::Px(y);
    style.width = UiLength::Px(std::max(0.0f, width));
    style.height = UiLength::Px(height);
    style.overflow = UiOverflow::Hidden;
    style.textFlow = UiTextFlow::SingleLineEllipsis;
    style.fontSize = 13.0f;
    style.color = UiInk(flare::Text);
    style.background = background;
    style.padding = {3, 0, 3, 0};
    return style;
}

struct FieldGeometry
{
    float labelEnd;
    float axisStart;
    float valueStart;
    float valueWidth;
};

bool RgbAxis(char axis) { return axis == 'R' || axis == 'G' || axis == 'B'; }

FieldGeometry FieldAt(float width, int count, int cell, bool axis, bool rgb = false)
{
    const float right = std::max(LeftInset, width - ScrollMargin - CardInset);
    const float labelEnd = rgb ? std::min(52.0f, right * 0.22f) : std::min(120.0f, right * 0.36f);
    const float span = (right - labelEnd) / count;
    const float axisStart = labelEnd + cell * span;
    const float axisWidth = axis ? std::min(15.0f, span * 0.26f) : 0.0f;
    return {labelEnd, axisStart, axisStart + axisWidth, std::max(0.0f, span - axisWidth - 2.0f)};
}

flare::Color AxisBadgeColor(char axis) noexcept
{
    if (axis == 'X' || axis == 'R') return flare::workspace::AxisX;
    if (axis == 'Y' || axis == 'G') return flare::workspace::AxisY;
    if (axis == 'Z' || axis == 'B') return flare::workspace::AxisZ;
    return flare::workspace::Header;
}

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept
{
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j)
        {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j])))
            {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

bool IsSimpleCSharpIdentifier(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 64) return false;
    const unsigned char c0 = static_cast<unsigned char>(text[0]);
    if (!std::isalpha(c0) && c0 != '_') return false;
    for (std::size_t i = 1; i < text.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (!std::isalnum(c) && c != '_') return false;
    }
    return true;
}

// 轻量四则运算解析器：支持 + - * / 括号与常量 pi，供数值框输入如 360/8、10+2.5*2。
class MathExprParser
{
public:
    explicit MathExprParser(std::string_view s) : src_(s) {}

    bool Parse(double& out)
    {
        out = ParseExpr();
        SkipSpaces();
        return ok_ && pos_ == src_.size() && std::isfinite(out);
    }

private:
    void SkipSpaces()
    {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    }

    double ParseExpr()
    {
        double v = ParseTerm();
        for (;;)
        {
            SkipSpaces();
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
            {
                const char op = src_[pos_++];
                const double rhs = ParseTerm();
                v = (op == '+') ? (v + rhs) : (v - rhs);
            }
            else break;
        }
        return v;
    }

    double ParseTerm()
    {
        double v = ParseFactor();
        for (;;)
        {
            SkipSpaces();
            if (pos_ < src_.size() && (src_[pos_] == '*' || src_[pos_] == '/'))
            {
                const char op = src_[pos_++];
                const double rhs = ParseFactor();
                if (op == '/')
                {
                    if (std::abs(rhs) < 1e-15) { ok_ = false; return 0.0; }
                    v /= rhs;
                }
                else v *= rhs;
            }
            else break;
        }
        return v;
    }

    double ParseFactor()
    {
        SkipSpaces();
        if (pos_ >= src_.size()) { ok_ = false; return 0.0; }
        if (src_[pos_] == '+' || src_[pos_] == '-')
        {
            const char sign = src_[pos_++];
            const double v = ParseFactor();
            return sign == '-' ? -v : v;
        }
        if (src_[pos_] == '(')
        {
            ++pos_;
            const double v = ParseExpr();
            SkipSpaces();
            if (pos_ >= src_.size() || src_[pos_] != ')') { ok_ = false; return 0.0; }
            ++pos_;
            return v;
        }
        if (pos_ + 2 <= src_.size() &&
            std::tolower(static_cast<unsigned char>(src_[pos_])) == 'p' &&
            std::tolower(static_cast<unsigned char>(src_[pos_ + 1])) == 'i')
        {
            pos_ += 2;
            return 3.14159265358979323846;
        }
        const char* begin = src_.data() + pos_;
        const char* end = src_.data() + src_.size();
        double val = 0.0;
        const auto [ptr, ec] = std::from_chars(begin, end, val);
        if (ec != std::errc{} || ptr == begin) { ok_ = false; return 0.0; }
        pos_ = static_cast<std::size_t>(ptr - src_.data());
        return val;
    }

    std::string_view src_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

std::string EvaluateNumericInputIfFormula(const std::string& raw)
{
    double direct = 0.0;
    const char* b = raw.data();
    const char* e = raw.data() + raw.size();
    const auto [p, ec] = std::from_chars(b, e, direct);
    if (ec == std::errc{} && p == e && std::isfinite(direct)) return raw;

    double evaluated = 0.0;
    MathExprParser parser(raw);
    if (!parser.Parse(evaluated)) return raw;

    const float asFloat = static_cast<float>(evaluated);
    if (!std::isfinite(asFloat)) return raw;
    char buf[64]{};
    if (std::abs(evaluated - std::round(evaluated)) < 1e-6 && std::abs(evaluated) < 2e9)
    {
        const long long iv = std::llround(evaluated);
        const auto [ie, iec] = std::to_chars(buf, buf + sizeof(buf), iv);
        if (iec == std::errc{}) return std::string(buf, ie);
    }
    const auto [fe, fec] = std::to_chars(buf, buf + sizeof(buf), asFloat);
    return fec == std::errc{} ? std::string(buf, fe) : raw;
}

UiColor SectionBadgeColor(const std::vector<EditorProperty>& props, const std::string& section)
{
    for (const auto& p : props)
    {
        if (p.section != section) continue;
        if (p.key.starts_with("script.")) return UiColor{0.18f, 0.78f, 0.46f, 0.96f};
        if (p.key.starts_with("position.") || p.key.starts_with("rotation.")) return UiColor{0.28f, 0.62f, 1.0f, 0.96f};
        if (p.key.starts_with("camera.") || p.key.starts_with("light.")) return UiInk(flare::Gold, 0.96f);
        if (p.key.starts_with("mesh.")) return UiColor{0.20f, 0.72f, 0.90f, 0.95f};
        if (p.key.starts_with("prefab.")) return UiColor{0.26f, 0.60f, 1.0f, 0.96f};
        if (p.key.starts_with("feature.")) return UiColor{0.66f, 0.46f, 0.98f, 0.95f};
    }
    return UiColor{0.46f, 0.52f, 0.62f, 0.90f};
}
}

void EditorShell::BuildInspector(PanelSlot& slot)
{
    inspectorList_ = std::make_unique<UiListView>(document_, slot.content);
    inspectorList_->SetRowHeight(RowHeight);
    inspectorList_->SetRowBackgroundProvider([this](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= inspectorRows_.size()) return Transparent;
        const auto& row = inspectorRows_[static_cast<std::size_t>(index)];
        if (row.header || row.fields.empty()) return Transparent;
        const auto& first = inspectorModel_.Properties()[row.fields.front()];
        if (first.section.empty() || first.key == "@add_component_button") return Transparent;
        return UiInk(flare::workspace::Header, 0.22f);
    });
    inspectorList_->SetRowBuilder([this](int index, UiElementHandle row) { BuildInspectorRow(index, row); });
    dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
        if (addCompPopup_.IsValid()) CloseAddComponentPopup();
        inspectorList_->SetScrollOffset(inspectorList_->Offset() - event.wheel * 2 * RowHeight);
        inspectorList_->Sync();
        LayoutInspector(inspectorWidth_);
        event.handled = true;
    }, UiEventPhase::Bubble);
    dispatcher_.AddHandler(slot.content, UiEventType::PointerDown,
        [this](UiInputEvent& event) { ActivateInspectorRow(event); }, UiEventPhase::Bubble);

    // 编辑器是持久控件，滚动回收虚拟行或调整面板宽度不会替换输入法上下文。
    inspectorEditElement_ = document_.CreateElement(UiElementKind::TextEdit);
    document_.SetParent(inspectorEditElement_, slot.panel);
    document_.SetVisible(inspectorEditElement_, false);
    inspectorEditSelection_ = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(inspectorEditSelection_, inspectorEditElement_);
    inspectorEdit_ = std::make_unique<UiTextEdit>(document_, inspectorEditElement_, text_);
    dispatcher_.AddHandler(inspectorEditElement_, UiEventType::PointerDown, [this](UiInputEvent& event) {
        if (event.button != 0) return;
        const auto rect = layout_.Rect(inspectorEditElement_);
        const auto raw = inspectorEdit_->Text();
        const auto byte = text_->HitTestCaret(raw, 13, 100000, false,
            event.x - rect.x - 3 + inspectorEditScroll_, 0);
        std::u32string prefix;
        if (DecodeUtf8(raw.substr(0, byte), prefix))
            inspectorEdit_->SetCaret(static_cast<std::uint32_t>(prefix.size()));
        event.handled = true;
    });
    dispatcher_.AddHandler(inspectorEditElement_, UiEventType::FocusLost, [this](UiInputEvent&) {
        EndInspectorEdit(true);
    });
    dispatcher_.AddHandler(inspectorEditElement_, UiEventType::KeyDown, [this](UiInputEvent& event) {
        if (event.key == UiKeyEnter || event.key == UiKeyEscape)
        {
            EndInspectorEdit(event.key == UiKeyEnter);
            dispatcher_.ClearFocus();
        }
        else
        {
            inspectorEdit_->HandleKey(event.key, event.shift, event.control);
        }
        event.handled = true;
    });
    dispatcher_.AddHandler(inspectorEditElement_, UiEventType::TextInput, [this](UiInputEvent& event) {
        std::string utf8;
        const char32_t codepoint = static_cast<char32_t>(event.codepoint);
        if (codepoint && EncodeUtf8({&codepoint, 1}, utf8))
            event.handled = inspectorEdit_->InsertText(utf8);
    });
    dispatcher_.AddHandler(inspectorEditElement_, UiEventType::ImeComposition, [this](UiInputEvent& event) {
        inspectorEdit_->SetComposition(event.text);
    });
    dispatcher_.AddHandler(inspectorEditElement_, UiEventType::ImeCommit, [this](UiInputEvent& event) {
        inspectorEdit_->SetComposition(event.text);
        inspectorEdit_->CommitComposition();
    });
    dispatcher_.AddHandler(inspectorEditElement_, UiEventType::ImeCancel, [this](UiInputEvent&) {
        inspectorEdit_->CancelComposition();
    });
}

void EditorShell::RefreshInspectorRows()
{
    inspectorRows_.clear();
    addComponentActions_.clear();
    const auto& properties = inspectorModel_.Properties();
    const std::string addSectionTitle = Text("EditorUi.AddComponent", "Add Component");
    if (previewPlaying_ && inspectorModel_.Target() != 0 && !properties.empty())
    {
        inspectorRows_.push_back({"@play_mode_banner", true, true, {}, {}});
    }
    std::vector<std::string> sections;
    for (const auto& property : properties)
        if (std::find(sections.begin(), sections.end(), property.section) == sections.end())
            sections.push_back(property.section);

    // 选择对象后优先展示组件；辅助设置和添加操作位于属性表末尾。
    for (const auto& last : {Text("EditorUi.ObjectDetails", "Object details"),
                             Text("EditorUi.SceneEnvironment", "Scene environment"),
                             addSectionTitle})
    {
        const auto i = std::find(sections.begin(), sections.end(), last);
        if (i != sections.end()) std::rotate(i, i + 1, sections.end());
    }

    for (std::size_t i = 0; i < properties.size(); ++i)
    {
        if (properties[i].section == addSectionTitle && properties[i].type == EditorPropertyType::Action)
            addComponentActions_.push_back(i);
    }

    for (const auto& section : sections)
    {
        if (section == addSectionTitle && inspectorModel_.Target() != 0)
        {
            inspectorRows_.push_back({section, true, true, {}, {}});
            continue;
        }
        bool closed = false;
        if (!section.empty())
        {
            auto first = std::find_if(properties.begin(), properties.end(),
                [&](const auto& p) { return p.section == section; });
            const bool defaultClosed = first != properties.end() ? first->sectionInitiallyClosed : false;
            closed = inspectorFolds_.try_emplace(section, defaultClosed).first->second;
            InspectorRow headerRow{section, true, closed, {}, {}};
            for (std::size_t i = 0; i < properties.size(); ++i)
            {
                const auto& p = properties[i];
                if (p.section != section) continue;
                if (p.sectionEnabled && headerRow.fields.empty()) headerRow.fields.push_back(i);
                if (p.headerMenuAction) headerRow.menuActions.push_back(i);
            }
            inspectorRows_.push_back(std::move(headerRow));
        }
        if (closed) continue;
        for (std::size_t i = 0; i < properties.size(); ++i)
        {
            const auto& property = properties[i];
            if (property.section != section || property.sectionEnabled || property.headerMenuAction) continue;
            if (property.key == "entity.enabled" && i + 1 < properties.size() &&
                properties[i + 1].key == "name" && properties[i + 1].section == section)
            {
                inspectorRows_.push_back({properties[i + 1].label, false, false, {i, i + 1}, {}});
                ++i;
                continue;
            }
            if ((property.axis == 'X' || property.axis == 'R') && inspectorWidth_ >= CompactWidth && i + 2 < properties.size() &&
                properties[i+1].axis == (property.axis == 'X' ? 'Y' : 'G') && properties[i+2].axis == (property.axis == 'X' ? 'Z' : 'B') &&
                properties[i+1].section == section && properties[i+2].section == section &&
                properties[i+1].label == property.label && properties[i+2].label == property.label)
            {
                inspectorRows_.push_back({property.label, false, false, {i, i+1, i+2}, {}});
                i += 2;
                continue;
            }
            inspectorRows_.push_back({property.label, false, false, {i}, {}});
        }
    }
    inspectorList_->SetItemCount(static_cast<int>(inspectorRows_.size()));
    inspectorList_->Invalidate();
    inspectorList_->Sync();
    if (addCompPopup_.IsValid()) RebuildAddComponentPopupItems();
}

void EditorShell::BuildInspectorRow(int index, UiElementHandle row)
{
    const auto& item = inspectorRows_.at(index);
    const auto& fields = inspectorModel_.Properties();
    const float right = std::max(LeftInset, inspectorWidth_ - ScrollMargin - CardInset);
    auto add = [&](std::string_view text, UiStyle style, UiElementKind kind = UiElementKind::Label) {
        auto element = document_.CreateElement(kind);
        document_.SetParent(element, row);
        document_.SetStyle(element, style);
        document_.SetText(element, text);
    };
    auto check = [&](const EditorProperty& p, float x, float yOff = 0.0f) {
        const bool checked = p.value == "true";
        auto style = CellStyle(x, yOff + (RowHeight - yOff - 16.0f) * 0.5f, 16.0f, 16.0f,
                               UiInk(checked && !p.readOnly ? flare::workspace::Selection : flare::workspace::Field));
        style.padding = {2, 0, 0, 0};
        style.cornerRadius = 3.5f;
        style.borderWidth = 1;
        style.borderColor = UiInk(checked && !p.readOnly ? flare::workspace::Focus : flare::workspace::Divider);
        style.color = UiInk(p.readOnly ? flare::Disabled : flare::Heading);
        add(checked ? "✓" : "", style);
    };

    const std::string addSectionTitle = Text("EditorUi.AddComponent", "Add Component");
    if (item.header)
    {
        if (item.title == "@play_mode_banner")
        {
            auto banner = CellStyle(CardInset, 2.0f, std::max(0.0f, right - CardInset), RowHeight - 3.0f,
                                    UiColor{0.08f, 0.28f, 0.18f, 0.85f});
            banner.cornerRadius = 4.0f;
            banner.borderWidth = 1.0f;
            banner.borderColor = UiColor{0.20f, 0.82f, 0.48f, 0.85f};
            banner.color = UiColor{0.62f, 0.96f, 0.76f, 1.0f};
            banner.fontSize = 11.5f;
            banner.padding = {8.0f, 0.0f, 6.0f, 0.0f};
            add("● " + Text("EditorUi.PlayModeActiveHint", "Play Mode: scene edits revert on Stop"), banner);
            return;
        }
        if (item.title == addSectionTitle)
        {
            const float width = std::max(0.0f, std::min(230.0f, right - 2 * LeftInset));
            auto style = CellStyle((right - width + CardInset) * 0.5f, 2.0f, width, RowHeight - 3.0f,
                                   previewPlaying_ ? UiInk(flare::workspace::Header) : UiInk(flare::Accent));
            style.cornerRadius = 5.0f;
            const auto caption = "＋ " + item.title;
            style.padding.left = std::max(4.0f, (width - MeasuredWidth(caption, 13.0f)) * 0.5f);
            style.borderWidth = 1.0f;
            style.borderColor = previewPlaying_ ? UiInk(flare::workspace::Divider) : UiInk(flare::Gold);
            style.color = previewPlaying_ ? UiInk(flare::Disabled) : UiInk(flare::Background);
            add(caption, style);
            return;
        }
        auto cardHeader = CellStyle(CardInset, 3.0f, std::max(0.0f, right - CardInset), RowHeight - 3.0f,
                                    UiInk(flare::workspace::Header));
        cardHeader.borderWidth = 1.0f;
        cardHeader.borderColor = UiInk(flare::workspace::Divider);
        cardHeader.cornerRadius = 4.0f;
        add("", cardHeader, UiElementKind::Panel);

        auto disclosure = CellStyle(LeftInset, 3.0f, 13.0f, RowHeight - 3.0f, Transparent);
        disclosure.padding = {};
        disclosure.fontSize = 10.0f;
        disclosure.color = UiInk(flare::Silver);
        add(item.closed ? "▶" : "▼", disclosure);

        float left = LeftInset + 14.0f;
        auto badge = CellStyle(left, 3.0f + (RowHeight - 3.0f - 9.0f) * 0.5f, 9.0f, 9.0f,
                               SectionBadgeColor(fields, item.title));
        badge.cornerRadius = 2.5f;
        badge.padding = {};
        add("", badge, UiElementKind::Panel);
        left += 13.0f;

        if (!item.fields.empty())
        {
            check(fields[item.fields.front()], left, 3.0f);
            left += 20.0f;
        }
        const float menuW = item.menuActions.empty() ? 0.0f : 22.0f;
        auto title = CellStyle(left, 3.0f, std::max(0.0f, right - left - menuW), RowHeight - 3.0f, Transparent);
        title.color = UiInk(flare::Heading);
        add(item.title, title);

        if (!item.menuActions.empty())
        {
            auto more = CellStyle(right - 21.0f, 5.0f, 18.0f, RowHeight - 7.0f, UiInk(flare::workspace::Field, 0.65f));
            more.cornerRadius = 3.5f;
            more.color = UiInk(flare::Heading);
            more.fontSize = 12.5f;
            more.padding = {5.0f, 0.0f, 0.0f, 0.0f};
            add("⋮", more);
        }
        return;
    }

    const auto& first = fields[item.fields.front()];
    if (first.type == EditorPropertyType::Subheader)
    {
        auto hdr = CellStyle(LeftInset, 2.0f, std::max(0.0f, right - LeftInset - 4.0f), RowHeight - 4.0f, Transparent);
        hdr.color = UiInk(flare::Gold);
        hdr.fontSize = 12.0f;
        add(first.label, hdr);
        auto line = CellStyle(LeftInset, RowHeight - 2.0f, std::max(0.0f, right - LeftInset - 6.0f), 1.0f,
                              UiInk(flare::workspace::Divider));
        line.padding = {};
        add("", line, UiElementKind::Panel);
        return;
    }

    if (first.type == EditorPropertyType::Action)
    {
        const float width = std::min(230.0f, std::max(0.0f, right - 2 * LeftInset));
        auto style = CellStyle((right - width + CardInset) * 0.5f, 2.0f, width, RowHeight - 4.0f,
                               UiInk(flare::workspace::Header));
        style.padding.left = std::max(4.0f, (width - MeasuredWidth(first.label, 13.0f)) * 0.5f);
        style.color = UiInk(first.readOnly ? flare::Disabled : flare::Heading);
        style.borderWidth = 1.0f;
        style.borderColor = UiInk(flare::workspace::Divider);
        style.cornerRadius = 4.0f;
        add(first.label, style);
        return;
    }

    const auto count = static_cast<int>(item.fields.size());
    if (count == 2 && first.key == "entity.enabled" && fields[item.fields[1]].key == "name")
    {
        const auto& nameField = fields[item.fields[1]];
        if (first.overridden || nameField.overridden)
            add("", CellStyle(CardInset + 1.0f, 2.0f, 3.0f, RowHeight - 4.0f, UiInk(flare::workspace::Focus)));
        check(first, LeftInset);
        const float nameLeft = LeftInset + 22.0f;
        auto style = CellStyle(nameLeft, 2.5f, std::max(0.0f, right - nameLeft), RowHeight - 5.0f,
                               UiInk(flare::workspace::Field));
        style.color = UiInk(nameField.readOnly ? flare::Muted : flare::Heading);
        style.borderColor = UiInk(nameField.overridden ? flare::workspace::Focus : flare::workspace::Divider);
        style.borderWidth = 1;
        style.cornerRadius = 3.5f;
        add(nameField.displayValue.empty() ? nameField.value : nameField.displayValue, style);
        return;
    }
    const bool objectName = first.key == "name";
    bool rowOverridden = false;
    for (int i = 0; i < count; ++i) rowOverridden = rowOverridden || fields[item.fields[i]].overridden;
    if (rowOverridden)
    {
        add("", CellStyle(CardInset + 1.0f, 2.0f, 3.0f, RowHeight - 4.0f, UiInk(flare::workspace::Focus)));
    }
    const auto firstCell = FieldAt(inspectorWidth_, count, 0, first.axis != 0, RgbAxis(first.axis));
    if (!objectName)
    {
        auto labelStyle = CellStyle(LeftInset, 0, firstCell.labelEnd - LeftInset, RowHeight, Transparent);
        if (rowOverridden) labelStyle.color = UiInk(flare::workspace::Focus);
        add(first.label, labelStyle);
    }
    for (int i = 0; i < count; ++i)
    {
        const auto& field = fields[item.fields[i]];
        const auto cell = FieldAt(inspectorWidth_, count, i, field.axis != 0, RgbAxis(field.axis));
        if (field.axis)
        {
            const float pillW = std::max(0.0f, cell.valueStart - cell.axisStart);
            auto style = CellStyle(cell.axisStart, 3.0f, pillW, RowHeight - 6.0f,
                                   UiInk(AxisBadgeColor(field.axis), field.readOnly ? 0.45f : 0.92f));
            style.color = UiInk(flare::Heading);
            style.fontSize = 11.5f;
            style.cornerRadius = 3.0f;
            const std::string axisStr(1, field.axis);
            style.padding = {std::max(1.0f, (pillW - MeasuredWidth(axisStr, 11.5f)) * 0.5f), 0.0f, 0.0f, 0.0f};
            add(axisStr, style);
        }
        if (field.type == EditorPropertyType::Boolean)
        {
            check(field, cell.valueStart);
            continue;
        }
        if (field.type == EditorPropertyType::Number && field.slider && count == 1 &&
            field.dragMinimum.has_value() && field.dragMaximum.has_value() &&
            *field.dragMaximum > *field.dragMinimum && cell.valueWidth >= 88.0f)
        {
            const float boxW = std::clamp(cell.valueWidth * 0.34f, 44.0f, 68.0f);
            const float trackW = std::max(24.0f, cell.valueWidth - boxW - 5.0f);
            float curVal = *field.dragMinimum;
            std::from_chars(field.value.data(), field.value.data() + field.value.size(), curVal);
            const float ratio = std::clamp((curVal - *field.dragMinimum) / (*field.dragMaximum - *field.dragMinimum), 0.0f, 1.0f);

            auto groove = CellStyle(cell.valueStart, (RowHeight - 6.0f) * 0.5f, trackW, 6.0f, UiInk(flare::workspace::Field));
            groove.cornerRadius = 3.0f;
            groove.borderWidth = 1.0f;
            groove.borderColor = UiInk(flare::workspace::Divider);
            groove.padding = {};
            add("", groove, UiElementKind::Panel);

            if (ratio > 0.005f)
            {
                auto fill = CellStyle(cell.valueStart, (RowHeight - 6.0f) * 0.5f, trackW * ratio, 6.0f,
                                      UiInk(field.readOnly ? flare::Muted : flare::Gold, 0.88f));
                fill.cornerRadius = 3.0f;
                fill.padding = {};
                add("", fill, UiElementKind::Panel);
            }
            const float knobX = cell.valueStart + std::clamp(trackW * ratio - 4.0f, 0.0f, std::max(0.0f, trackW - 8.0f));
            auto knob = CellStyle(knobX, (RowHeight - 14.0f) * 0.5f, 8.0f, 14.0f, UiInk(flare::Heading));
            knob.cornerRadius = 3.0f;
            knob.borderWidth = 1.0f;
            knob.borderColor = UiInk(flare::Background);
            knob.padding = {};
            add("", knob, UiElementKind::Panel);

            auto box = CellStyle(cell.valueStart + trackW + 5.0f, 2.5f, boxW, RowHeight - 5.0f, UiInk(flare::workspace::Field));
            box.color = UiInk(field.readOnly ? flare::Muted : flare::Heading);
            box.borderColor = UiInk(field.overridden ? flare::workspace::Focus : flare::workspace::Divider);
            box.borderWidth = 1.0f;
            box.cornerRadius = 3.0f;
            add(field.displayValue.empty() ? field.value : field.displayValue, box);
            continue;
        }
        if (field.type == EditorPropertyType::EntityRef)
        {
            const bool assigned = !field.value.empty() && field.value != "0";
            const float clearW = (assigned && !field.readOnly) ? 18.0f : 0.0f;
            auto slotStyle = CellStyle(cell.valueStart, 2.5f, std::max(0.0f, cell.valueWidth - clearW), RowHeight - 5.0f,
                                       UiInk(flare::workspace::Field));
            slotStyle.color = UiInk(!assigned ? flare::Muted : flare::Heading);
            slotStyle.borderWidth = 1.0f;
            slotStyle.borderColor = UiInk(assigned ? flare::Gold : flare::workspace::Divider, assigned ? 0.55f : 1.0f);
            slotStyle.cornerRadius = 3.5f;
            const std::string caption = (assigned ? "◈ " : "○ ") + (field.displayValue.empty() ? field.value : field.displayValue);
            add(caption, slotStyle);
            if (clearW > 0.0f)
            {
                auto clr = CellStyle(cell.valueStart + cell.valueWidth - 17.0f, 2.5f, 17.0f, RowHeight - 5.0f,
                                     UiInk(flare::workspace::Header));
                clr.cornerRadius = 3.5f;
                clr.borderWidth = 1.0f;
                clr.borderColor = UiInk(flare::workspace::Divider);
                clr.color = UiInk(flare::Muted);
                clr.padding = {4.0f, 0.0f, 0.0f, 0.0f};
                add("×", clr);
            }
            continue;
        }
        auto style = CellStyle(objectName ? LeftInset : cell.valueStart, 2.5f,
            objectName ? right - LeftInset : cell.valueWidth, RowHeight - 5.0f, UiInk(flare::workspace::Field));
        style.color = UiInk(field.readOnly ? flare::Muted : flare::Heading);
        style.borderColor = UiInk(field.overridden ? flare::workspace::Focus : flare::workspace::Divider);
        style.borderWidth = 1;
        style.cornerRadius = 3.5f;
        if (field.type == EditorPropertyType::Color)
        {
            float r=0,g=0,b=0;
            std::string cleanColor(field.value);
            for (char& ch : cleanColor) if (ch == ',') ch = ' ';
            std::istringstream value(cleanColor);
            if (value >> r >> g >> b)
                style.background = {std::clamp(r,0.f,1.f),std::clamp(g,0.f,1.f),std::clamp(b,0.f,1.f),1};
            add("", style);
        }
        else if (field.type == EditorPropertyType::Asset)
        {
            const bool hasAsset = !field.value.empty();
            if (!hasAsset) style.color = UiInk(flare::Muted);
            const std::string text = (hasAsset ? "◆ " : "○ ") + (field.displayValue.empty() ? field.value : field.displayValue);
            add(text, style);
        }
        else
            add((field.displayValue.empty() ? field.value : field.displayValue) +
                (field.type == EditorPropertyType::Combo ? "  ▾" : ""), style);
    }
}

void EditorShell::OpenAddComponentPopup(float anchorX, float anchorY)
{
    if (previewPlaying_ || inspectorModel_.Target() == 0) return;
    CloseMenu();
    CloseAddComponentPopup();
    const auto surface = SurfaceRoot(dock_.SurfaceFor(EditorPanelId::Inspector));
    if (!surface.IsValid()) return;
    const auto bounds = layout_.Rect(surface);
    const float popW = std::min(std::max(210.0f, inspectorWidth_ - 16.0f), std::max(210.0f, bounds.width - 12.0f));
    const float popH = 256.0f;
    const float left = std::clamp(anchorX - bounds.x, 6.0f, std::max(6.0f, bounds.width - popW - 6.0f));
    const float preferBelow = anchorY - bounds.y + 4.0f;
    const float top = (preferBelow + popH <= bounds.height - 6.0f)
        ? preferBelow
        : std::max(6.0f, anchorY - bounds.y - popH - 26.0f);

    addCompPopup_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(addCompPopup_, surface)) { addCompPopup_ = {}; return; }
    auto ps = CellStyle(left, top, popW, popH, UiInk(flare::workspace::Panel));
    ps.borderWidth = 1.0f;
    ps.borderColor = UiInk(flare::workspace::Focus);
    ps.cornerRadius = 7.0f;
    ps.padding = {};
    document_.SetStyle(addCompPopup_, ps);

    addCompSearchElement_ = document_.CreateElement(UiElementKind::TextEdit);
    document_.SetParent(addCompSearchElement_, addCompPopup_);
    auto ss = CellStyle(6.0f, 6.0f, popW - 12.0f, 24.0f, UiInk(flare::workspace::Field));
    ss.borderWidth = 1.0f;
    ss.borderColor = UiInk(flare::workspace::Focus);
    ss.cornerRadius = 4.0f;
    ss.padding = {6.0f, 0.0f, 6.0f, 0.0f};
    document_.SetStyle(addCompSearchElement_, ss);
    addCompSearch_ = std::make_unique<UiTextEdit>(document_, addCompSearchElement_, text_);
    addCompSearch_->SetText({});

    auto syncSearchText = [this]() {
        if (!addCompSearch_ || !addCompSearchElement_.IsValid()) return;
        const std::string q = addCompSearch_->DisplayText();
        auto st = document_.Style(addCompSearchElement_);
        if (q.empty())
        {
            st.color = UiInk(flare::Muted);
            document_.SetStyle(addCompSearchElement_, st);
            document_.SetText(addCompSearchElement_, Text("EditorUi.SearchComponents", "Search components..."));
        }
        else
        {
            st.color = UiInk(flare::Heading);
            document_.SetStyle(addCompSearchElement_, st);
            document_.SetText(addCompSearchElement_, q);
        }
        RebuildAddComponentPopupItems();
    };

    dispatcher_.AddHandler(addCompSearchElement_, UiEventType::PointerDown, [this](UiInputEvent& e) {
        dispatcher_.SetFocus(addCompSearchElement_);
        e.handled = true;
        e.StopPropagation();
    });
    dispatcher_.AddHandler(addCompSearchElement_, UiEventType::KeyDown, [this, syncSearchText](UiInputEvent& e) {
        if (e.key == UiKeyEscape)
        {
            CloseAddComponentPopup();
            dispatcher_.ClearFocus();
            e.handled = true;
            return;
        }
        if (e.key == UiKeyEnter)
        {
            if (!addCompItemButtons_.empty() && addCompItemButtons_.front().IsValid())
            {
                const UiRect r = layout_.Rect(addCompItemButtons_.front());
                UiInputEvent click;
                click.type = UiEventType::PointerDown;
                click.button = 0;
                click.x = r.x + r.width * 0.5f;
                click.y = r.y + r.height * 0.5f;
                dispatcher_.Dispatch(click);
            }
            e.handled = true;
            return;
        }
        if (addCompSearch_) addCompSearch_->HandleKey(e.key, e.shift, e.control);
        syncSearchText();
        e.handled = true;
    });
    dispatcher_.AddHandler(addCompSearchElement_, UiEventType::TextInput, [this, syncSearchText](UiInputEvent& e) {
        if (!addCompSearch_ || e.codepoint == 0) return;
        const char32_t cp = e.codepoint;
        std::string utf8;
        if (EncodeUtf8({&cp, 1}, utf8))
        {
            addCompSearch_->InsertText(utf8);
            syncSearchText();
            e.handled = true;
        }
    });
    dispatcher_.AddHandler(addCompSearchElement_, UiEventType::ImeComposition, [this, syncSearchText](UiInputEvent& e) {
        if (addCompSearch_) { addCompSearch_->SetComposition(e.text); syncSearchText(); }
        e.handled = true;
    });
    dispatcher_.AddHandler(addCompSearchElement_, UiEventType::ImeCommit, [this, syncSearchText](UiInputEvent& e) {
        if (addCompSearch_) { addCompSearch_->SetComposition(e.text); addCompSearch_->CommitComposition(); syncSearchText(); }
        e.handled = true;
    });
    dispatcher_.AddHandler(addCompSearchElement_, UiEventType::ImeCancel, [this, syncSearchText](UiInputEvent&) {
        if (!addCompSearch_) return;
        const std::string before(addCompSearch_->DisplayText());
        addCompSearch_->CancelComposition();
        if (addCompSearch_->DisplayText() != before) syncSearchText();
    });

    syncSearchText();
    dispatcher_.SetFocus(addCompSearchElement_);
}

void EditorShell::RebuildAddComponentPopupItems()
{
    if (!addCompPopup_.IsValid()) return;
    for (const auto btn : addCompItemButtons_)
    {
        if (btn.IsValid())
        {
            dispatcher_.RemoveHandlers(btn);
            document_.SetVisible(btn, false);
            pendingDestroy_.push_back(btn);
        }
    }
    addCompItemButtons_.clear();

    const std::string query = addCompSearch_ ? std::string(addCompSearch_->Text()) : std::string{};
    const auto& props = inspectorModel_.Properties();
    const std::uint64_t target = inspectorModel_.Target();
    const float popW = document_.Style(addCompPopup_).width.value;

    struct Candidate
    {
        std::string label;
        std::string key;
        bool isNewScript = false;
        bool isScript = false;
    };
    std::vector<Candidate> matches;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (const std::size_t idx : addComponentActions_)
        {
            if (idx >= props.size()) continue;
            const auto& p = props[idx];
            if (p.readOnly) continue;
            const bool isScript = p.key.starts_with("script.");
            if ((pass == 0) != isScript) continue;
            if (!ContainsIgnoreCase(p.label, query)) continue;
            matches.push_back({p.label, p.key, false, isScript});
            if (matches.size() >= 14) break;
        }
        if (matches.size() >= 14) break;
    }

    std::string trimmed = query;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
    const std::string scriptClass = trimmed.empty() ? addCompDefaultClass_ : trimmed;
    if (IsSimpleCSharpIdentifier(scriptClass))
    {
        const std::string createKey = "script.{\"Action\":\"CreateAndAttach\",\"Class\":\"" + scriptClass + "\"}";
        const std::string createLabel = Text("EditorUi.NewScriptNamed", "+ New Script & Attach") + ": " + scriptClass + ".cs";
        matches.push_back({createLabel, createKey, true, true});
    }

    float y = 34.0f;
    for (const auto& item : matches)
    {
        const auto btn = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(btn, addCompPopup_)) continue;
        auto bs = CellStyle(6.0f, y, popW - 12.0f, 22.0f,
                            item.isNewScript ? UiInk(flare::workspace::Selection, 0.72f) : UiInk(flare::workspace::Header, 0.55f));
        bs.cornerRadius = 4.0f;
        bs.borderWidth = 1.0f;
        bs.borderColor = item.isNewScript ? UiInk(flare::Gold, 0.65f) : UiInk(flare::workspace::Divider);
        bs.color = item.isNewScript ? UiInk(flare::Gold) : (item.isScript ? UiColor{0.52f, 0.94f, 0.68f, 1.0f} : UiInk(flare::Heading));
        bs.padding = {8.0f, 0.0f, 6.0f, 0.0f};
        document_.SetStyle(btn, bs);
        document_.SetText(btn, item.label);
        const std::string key = item.key;
        dispatcher_.AddHandler(btn, UiEventType::PointerDown, [this, target, key](UiInputEvent& e) {
            if (e.button != 0) return;
            e.handled = true;
            e.StopPropagation();
            CloseAddComponentPopup();
            dispatcher_.ClearFocus();
            if (propertyHandler_) propertyHandler_(target, key, {});
        });
        addCompItemButtons_.push_back(btn);
        y += 23.5f;
    }

    const auto surface = SurfaceRoot(dock_.SurfaceFor(EditorPanelId::Inspector));
    const float surfaceH = surface.IsValid() ? layout_.Rect(surface).height : height_;
    const float finalH = std::max(44.0f, y + 6.0f);
    auto ps = document_.Style(addCompPopup_);
    ps.height = UiLength::Px(finalH);
    if (ps.top.value + finalH > surfaceH - 6.0f)
        ps.top = UiLength::Px(std::max(6.0f, surfaceH - finalH - 6.0f));
    document_.SetStyle(addCompPopup_, ps);
}

void EditorShell::CloseAddComponentPopup()
{
    for (const auto btn : addCompItemButtons_)
    {
        if (btn.IsValid())
        {
            dispatcher_.RemoveHandlers(btn);
            document_.SetVisible(btn, false);
            pendingDestroy_.push_back(btn);
        }
    }
    addCompItemButtons_.clear();
    if (addCompSearchElement_.IsValid())
    {
        dispatcher_.RemoveHandlers(addCompSearchElement_);
        document_.SetVisible(addCompSearchElement_, false);
        pendingDestroy_.push_back(addCompSearchElement_);
        addCompSearchElement_ = {};
    }
    addCompSearch_.reset();
    if (addCompPopup_.IsValid())
    {
        document_.SetVisible(addCompPopup_, false);
        pendingDestroy_.push_back(addCompPopup_);
        addCompPopup_ = {};
    }
}

std::optional<std::size_t> EditorShell::InspectorSlotAt(float x, float y, bool entitySlotOnly) const
{
    if (!inspectorList_ || SurfaceForElement(InspectorContent()) != inputSurface_) return {};
    const auto content = layout_.Rect(InspectorContent());
    if (x < content.x || x >= content.x + content.width || y < content.y || y >= content.y + content.height) return {};
    const int row = static_cast<int>(std::floor((y - content.y + inspectorList_->Offset()) / RowHeight));
    if (row < 0 || row >= static_cast<int>(inspectorRows_.size()) || inspectorRows_[row].header) return {};
    const auto& item = inspectorRows_[row];
    if (item.fields.empty()) return {};
    const std::size_t idx = item.fields.front();
    const auto& prop = inspectorModel_.Properties()[idx];
    if (prop.readOnly) return {};
    if (entitySlotOnly)
    {
        return prop.type == EditorPropertyType::EntityRef ? std::optional<std::size_t>{idx} : std::nullopt;
    }
    if (prop.type == EditorPropertyType::Asset ||
        (prop.type == EditorPropertyType::Combo && (prop.key == "mesh.asset" || prop.key == "actor.asset")))
    {
        return idx;
    }
    return {};
}

void EditorShell::ActivateInspectorRow(UiInputEvent& event)
{
    if (event.button != 0 && event.button != 1) return;
    if (addCompPopup_.IsValid()) CloseAddComponentPopup();
    const auto content = layout_.Rect(InspectorContent());
    const float x = event.x - content.x;
    const int row = static_cast<int>(std::floor((event.y - content.y + inspectorList_->Offset()) / RowHeight));
    if (row < 0 || row >= static_cast<int>(inspectorRows_.size())) return;
    const auto& item = inspectorRows_[row];

    if (event.button == 0 && !item.header && item.fields.size() == 1)
    {
        const std::size_t fIdx = item.fields.front();
        const auto& prop = inspectorModel_.Properties()[fIdx];
        if (prop.type == EditorPropertyType::Number && prop.slider && !prop.readOnly &&
            prop.dragMinimum.has_value() && prop.dragMaximum.has_value() && *prop.dragMaximum > *prop.dragMinimum)
        {
            const auto cell = FieldAt(inspectorWidth_, 1, 0, false);
            if (cell.valueWidth >= 88.0f)
            {
                const float boxW = std::clamp(cell.valueWidth * 0.34f, 44.0f, 68.0f);
                const float trackW = std::max(24.0f, cell.valueWidth - boxW - 5.0f);
                if (x >= cell.valueStart && x <= cell.valueStart + trackW)
                {
                    EndInspectorEdit(true);
                    dispatcher_.ClearFocus();
                    double orig = *prop.dragMinimum;
                    std::from_chars(prop.value.data(), prop.value.data() + prop.value.size(), orig);
                    const float trackScreenLeft = content.x + cell.valueStart;
                    const double ratio = std::clamp(static_cast<double>((event.x - trackScreenLeft) / trackW), 0.0, 1.0);
                    const double nextVal = *prop.dragMinimum + (*prop.dragMaximum - *prop.dragMinimum) * ratio;
                    inspectorDrag_ = std::make_unique<EditorInspectorDrag>(EditorInspectorDrag{
                        prop, inspectorModel_.Target(), inputSurface_, orig, nextVal, orig,
                        event.x, event.x, false, true, trackScreenLeft, trackW});
                    dispatcher_.SetPointerCapture(SurfaceRoot(inputSurface_));
                    UpdateInspectorDrag(event);
                    event.handled = true;
                    return;
                }
            }
        }
    }

    if (event.button == 0)
    {
        if (const auto field = InspectorNumericAt(event.x, event.y); field && propertyEditHandler_)
        {
            BeginInspectorDrag(*field, event);
            event.handled = true;
            return;
        }
    }

    std::size_t field = 0;
    if (item.header)
    {
        if (item.title == "@play_mode_banner")
        {
            event.handled = true;
            return;
        }
        const std::string addSectionTitle = Text("EditorUi.AddComponent", "Add Component");
        if (item.title == addSectionTitle)
        {
            if (event.button == 0 && !previewPlaying_)
            {
                const float right = std::max(LeftInset, inspectorWidth_ - ScrollMargin - CardInset);
                const float width = std::max(0.0f, std::min(230.0f, right - 2 * LeftInset));
                const float btnX = content.x + (right - width + CardInset) * 0.5f;
                const float btnY = content.y + static_cast<float>(row + 1) * RowHeight - inspectorList_->Offset();
                OpenAddComponentPopup(btnX, btnY);
                event.handled = true;
            }
            return;
        }
        const float right = std::max(LeftInset, inspectorWidth_ - ScrollMargin - CardInset);
        const bool openCompMenu = !item.menuActions.empty() &&
            (event.button == 1 || (event.button == 0 && x >= right - 24.0f));
        if (openCompMenu)
        {
            std::vector<EditorMenuItem> menuItems;
            const auto target = inspectorModel_.Target();
            std::vector<std::size_t> validActions;
            for (const std::size_t aIdx : item.menuActions)
            {
                if (aIdx >= inspectorModel_.Properties().size()) continue;
                const auto& act = inspectorModel_.Properties()[aIdx];
                if (act.readOnly) continue;
                menuItems.push_back({"@comp-menu", act.label, EditorCommandId::SetProperty, {}});
                validActions.push_back(aIdx);
            }
            if (!menuItems.empty())
            {
                CloseMenu();
                const auto surface = SurfaceRoot(dock_.SurfaceFor(EditorPanelId::Inspector));
                ShowMenu(surface, event.x, event.y, 190.0f, menuItems, target);
                openMenu_ = "@comp-menu";
                auto btn = document_.FirstChild(menuDropdown_);
                for (std::size_t mi = 0; mi < validActions.size() && btn.IsValid(); ++mi, btn = document_.NextSibling(btn))
                {
                    const std::string actionKey = inspectorModel_.Properties()[validActions[mi]].key;
                    dispatcher_.RemoveHandlers(btn);
                    dispatcher_.AddHandler(btn, UiEventType::PointerDown, [this, target, actionKey](UiInputEvent& ev) {
                        ev.handled = true;
                        if (ev.button != 0) return;
                        CloseMenu();
                        if (propertyHandler_) propertyHandler_(target, actionKey, {});
                    });
                }
            }
            event.handled = true;
            return;
        }
        if (event.button == 1) return;
        const float checkX = LeftInset + 27.0f;
        if (item.fields.empty() || x < checkX || x >= checkX + 16.0f)
        {
            inspectorFolds_[item.title] = !item.closed;
            RefreshInspectorRows();
            event.handled = true;
            return;
        }
        field = item.fields.front();
    }
    else
    {
        field = item.fields.front();
        const auto& property = inspectorModel_.Properties()[field];
        if (property.type == EditorPropertyType::Subheader) return;
        if (item.fields.size() == 2 && property.key == "entity.enabled" &&
            inspectorModel_.Properties()[item.fields[1]].key == "name")
        {
            const float right = std::max(LeftInset, inspectorWidth_ - ScrollMargin - CardInset);
            if (x >= LeftInset && x < LeftInset + 20.0f)
                field = item.fields[0];
            else if (x >= LeftInset + 22.0f && x < right)
                field = item.fields[1];
            else
                return;
        }
        else if (property.type == EditorPropertyType::Action)
        {
            if (event.button == 1) return;
            const float right = std::max(LeftInset, inspectorWidth_ - ScrollMargin - CardInset);
            const float width = std::min(230.0f, std::max(0.0f, right - 2 * LeftInset));
            if (x < (right - width) * 0.5f || x >= (right + width + CardInset) * 0.5f) return;
        }
        else if (property.key != "name")
        {
            const int count = static_cast<int>(item.fields.size());
            int cell = 0;
            for (; cell < count; ++cell)
            {
                const auto box = FieldAt(inspectorWidth_, count, cell, property.axis != 0, RgbAxis(property.axis));
                if (x >= box.axisStart && x < box.valueStart + box.valueWidth) break;
            }
            if (cell == count && event.button == 1 && count > 0)
            {
                const auto firstBox = FieldAt(inspectorWidth_, count, 0, property.axis != 0, RgbAxis(property.axis));
                if (x >= LeftInset && x < firstBox.labelEnd)
                {
                    for (int c = 0; c < count; ++c)
                    {
                        if (inspectorModel_.Properties()[item.fields[c]].overridden) { cell = c; break; }
                    }
                }
            }
            if (cell == count) return;
            field = item.fields[cell];
        }
    }

    const auto& property = inspectorModel_.Properties()[field];
    if (event.button == 1)
    {
        if (property.overridden && !property.readOnly && inspectorModel_.Target() != 0)
        {
            CloseMenu();
            openMenu_ = "@inspector";
            ShowMenu(SurfaceRoot(dock_.SurfaceFor(EditorPanelId::Inspector)), event.x, event.y, 195.0f,
                     {{"@inspector", Text("EditorUi.PrefabRevertProperty", "Revert property override"),
                       EditorCommandId::SetProperty, "prefab.revertProperty:" + property.key}},
                     inspectorModel_.Target());
            event.handled = true;
        }
        return;
    }
    if (property.readOnly) return;
    if (property.type == EditorPropertyType::EntityRef)
    {
        const auto cell = FieldAt(inspectorWidth_, 1, 0, false);
        const bool assigned = !property.value.empty() && property.value != "0";
        if (assigned && x >= cell.valueStart + cell.valueWidth - 18.0f)
        {
            if (propertyHandler_) propertyHandler_(inspectorModel_.Target(), property.key, "0");
            event.handled = true;
            return;
        }
        if (assigned && selectionHandler_)
        {
            const std::uint64_t refId = static_cast<std::uint64_t>(std::strtoull(property.value.c_str(), nullptr, 10));
            if (refId != 0)
            {
                hierarchyModel_.Select(refId);
                if (hierarchyList_) { hierarchyList_->Invalidate(); hierarchyList_->Sync(); }
            }
        }
        BeginInspectorEdit(field);
        event.handled = true;
        return;
    }
    if (property.type == EditorPropertyType::Color)
        OpenInspectorColor(field);
    else if (property.type == EditorPropertyType::Combo)
        ShowInspectorCombo(field);
    else if (property.type == EditorPropertyType::Boolean || property.type == EditorPropertyType::Action)
    {
        if (propertyHandler_)
        {
            const auto value = property.type == EditorPropertyType::Boolean ?
                (property.value == "true" ? "false" : "true") : property.value;
            propertyHandler_(inspectorModel_.Target(), property.key, value);
        }
    }
    else BeginInspectorEdit(field);
    event.handled = true;
}

void EditorShell::LayoutInspector(float width)
{
    if (!inspectorList_) return;
    if (inspectorWidth_ != width)
    {
        inspectorWidth_ = width;
        RefreshInspectorRows();
    }
    if (editingKey_.empty()) return;
    for (std::size_t row = 0; row < inspectorRows_.size(); ++row)
    {
        const auto& item = inspectorRows_[row];
        for (std::size_t i = 0; i < item.fields.size(); ++i)
        {
            const auto& field = inspectorModel_.Properties()[item.fields[i]];
            if (field.key != editingKey_) continue;
            const float y = static_cast<float>(row) * RowHeight - inspectorList_->Offset() + 2.5f;
            if (y < 0 || y + RowHeight - 5.0f > inspectorList_->ViewportHeight()) break;
            const auto cell = FieldAt(width, static_cast<int>(item.fields.size()), static_cast<int>(i), field.axis != 0, RgbAxis(field.axis));
            const bool name = field.key == "name";
            const float nameLeft = (name && item.fields.size() == 2 &&
                inspectorModel_.Properties()[item.fields[0]].key == "entity.enabled")
                ? (LeftInset + 22.0f) : LeftInset;
            float editLeft = name ? nameLeft : cell.valueStart;
            float editW = name ? std::max(0.0f, width - ScrollMargin - CardInset - nameLeft) : cell.valueWidth;
            if (!name && field.type == EditorPropertyType::Number && field.slider && item.fields.size() == 1 && cell.valueWidth >= 88.0f)
            {
                const float boxW = std::clamp(cell.valueWidth * 0.34f, 44.0f, 68.0f);
                const float trackW = std::max(24.0f, cell.valueWidth - boxW - 5.0f);
                editLeft = cell.valueStart + trackW + 5.0f;
                editW = boxW;
            }
            auto style = CellStyle(editLeft, y, editW, RowHeight - 5.0f, UiInk(flare::workspace::Field));
            style.borderWidth = 1;
            style.borderColor = UiInk(flare::workspace::Focus);
            document_.SetStyle(inspectorEditElement_, style);
            document_.SetVisible(inspectorEditElement_, true);
            PaintInspectorEdit(style.width.value);
            return;
        }
    }
    EndInspectorEdit(true);
}

bool EditorShell::BeginInspectorEdit(std::size_t index)
{
    const auto& field = inspectorModel_.Properties().at(index);
    if (!inspectorEdit_ || field.readOnly) return false;
    editingKey_ = field.key;
    inspectorEdit_->SetText(field.value);
    inspectorEditScroll_ = 0;
    if (field.type == EditorPropertyType::Number || field.type == EditorPropertyType::Asset ||
        field.type == EditorPropertyType::EntityRef)
    {
        inspectorEdit_->Select(0, std::numeric_limits<std::uint32_t>::max());
    }
    LayoutInspector(inspectorWidth_);
    if (editingKey_.empty()) return false;
    dispatcher_.SetFocus(inspectorEditElement_);
    return true;
}

void EditorShell::PaintInspectorEdit(float width)
{
    const auto raw = inspectorEdit_->Text();
    const auto byteAt = [](std::string_view s, std::uint32_t count) {
        std::size_t i = 0;
        while (i < s.size() && count--)
        {
            ++i;
            while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0u) == 0x80u) ++i;
        }
        return i;
    };
    const auto caret = byteAt(raw, inspectorEdit_->Caret());
    const auto anchor = byteAt(raw, inspectorEdit_->Anchor());
    const auto display = inspectorEdit_->DisplayText();
    const bool composing = inspectorEdit_->Composing();
    const auto first = std::min(caret, anchor);
    const auto last = composing ? first + inspectorEdit_->Composition().size() : std::max(caret, anchor);
    const auto at = [&](std::size_t byte) {
        return text_->CaretRect(display, 13, 100000, false, static_cast<std::uint32_t>(byte)).x;
    };
    const auto active = composing ? last : caret;
    const auto activeX = std::max(0.f, at(active));
    const float available = std::max(1.0f, width - 6);
    inspectorEditScroll_ = std::clamp(inspectorEditScroll_,
        std::max(0.f, activeX - std::max(0.f, available - 2)), activeX);
    const auto start = text_->HitTestCaret(display, 13, 100000, false, inspectorEditScroll_, 0);
    inspectorEditScroll_ = at(start);
    const auto end = text_->HitTestCaret(display, 13, 100000, false, inspectorEditScroll_ + available, 0);
    document_.SetText(inspectorEditElement_, std::string_view(display).substr(start, end - start));
    const float left = std::clamp(at(first) - inspectorEditScroll_, 0.f, available);
    const float right = std::clamp(at(last) - inspectorEditScroll_, left, available);
    auto highlight = CellStyle(left, 2, std::max(1.f, right-left), 16,
        UiInk(first == last ? flare::Heading : flare::workspace::Selection));
    highlight.padding = {};
    highlight.pointerEvents = UiPointerEvents::None;
    document_.SetStyle(inspectorEditSelection_, highlight);
}

void EditorShell::EndInspectorEdit(bool commit)
{
    if (editingKey_.empty()) return;
    const auto key = std::exchange(editingKey_, {});
    auto value = std::string(inspectorEdit_->Text());
    document_.SetVisible(inspectorEditElement_, false);
    const auto& fields = inspectorModel_.Properties();
    const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& p) { return p.key == key; });
    if (commit && propertyHandler_ && found != fields.end() && !found->readOnly)
    {
        if (found->type == EditorPropertyType::Number)
            value = EvaluateNumericInputIfFormula(value);
        if (!propertyHandler_(inspectorModel_.Target(), key, value))
            SetStatus(Text("EditorUi.InvalidValue", "Invalid value"));
    }
}

void EditorShell::SetInspector(std::uint64_t id, std::string label, std::vector<EditorProperty> properties)
{
    if(inspectorColor_&&(id!=inspectorColor_->target||std::none_of(properties.begin(),properties.end(),[&](const auto& p){
        return p.key==inspectorColor_->key&&!p.readOnly;
    })))CloseInspectorColor(false);
    if (inspectorDrag_ && (id != inspectorDrag_->target ||
        std::none_of(properties.begin(),properties.end(),[&](const auto& p) {
            return p.key == inspectorDrag_->field.key && !p.readOnly && p.dragStep > 0;
        }))) EndInspectorDrag(false);
    if (id != inspectorModel_.Target() && addCompPopup_.IsValid())
        CloseAddComponentPopup();
    if (id == inspectorModel_.Target() && label == inspectorModel_.TargetLabel() &&
        properties == inspectorModel_.Properties()) return;
    if (id != inspectorModel_.Target())
    {
        EndInspectorEdit(false);
        if (hierarchyList_) hierarchyList_->Invalidate();
        if (inspectorList_) inspectorList_->SetScrollOffset(0.0f);
    }
    inspectorModel_.SetTarget(id, std::move(label));
    inspectorModel_.SetProperties(std::move(properties));
    if (inspectorList_) RefreshInspectorRows();
    SetSelectionLabel(inspectorModel_.TargetLabel());
}

void EditorShell::ShowInspectorCombo(std::size_t index)
{
    auto field = inspectorModel_.Properties().at(index);
    if (field.readOnly) return;
    const auto target=inspectorModel_.Target();
    if(field.options.empty()&&propertyChoices_){
        auto status=propertyChoices_(target,field);if(!status.empty())SetStatus(std::move(status));}
    if(field.options.empty())return;
    const auto content=layout_.Rect(InspectorContent());
    std::size_t row=0;
    for (;row<inspectorRows_.size();++row)
        if (std::find(inspectorRows_[row].fields.begin(),inspectorRows_[row].fields.end(),index)!=inspectorRows_[row].fields.end()) break;
    ShowInspectorPage(std::make_shared<EditorProperty>(std::move(field)),target,
        content.x+FieldAt(inspectorWidth_,1,0,false).valueStart,content.y+(row+1)*RowHeight-inspectorList_->Offset(),0);
}

void EditorShell::ShowInspectorPage(std::shared_ptr<const EditorProperty> field,std::uint64_t target,float x,float y,std::size_t first)
{
    if(target!=inspectorModel_.Target())return;
    const auto surface=SurfaceRoot(inputSurface_);
    const auto capacity=static_cast<std::size_t>(std::max(1.f,std::min(14.f,std::floor((layout_.Rect(surface).height-8)/26)-2)));
    const auto end=std::min(first+capacity,field->options.size());
    std::vector<EditorMenuItem> items;
    if(first)items.push_back({"",Text("EditorUi.ChoicePrevious","Previous page"),EditorCommandId::SetProperty,{}});
    for(auto i=first;i<end;++i)
        items.push_back({"",i<field->optionLabels.size()?field->optionLabels[i]:field->options[i],EditorCommandId::SetProperty,field->options[i]});
    if(end<field->options.size())items.push_back({"",Text("EditorUi.ChoiceNext","Next page"),EditorCommandId::SetProperty,{}});
    CloseMenu();
    ShowMenu(surface,x,y,std::min(340.f,std::max(150.f,inspectorWidth_-12)),items,target);
    openMenu_="@inspector";
    auto button=document_.FirstChild(menuDropdown_);
    for (std::size_t row=0;row<items.size()&&button.IsValid();++row,button=document_.NextSibling(button))
    {
        dispatcher_.RemoveHandlers(button);
        dispatcher_.AddHandler(button,UiEventType::PointerDown,[this,target,field,x,y,row,first,end,capacity](UiInputEvent& event) {
            event.handled=true;
            if(event.button!=0)return;
            if(first&&row==0){ShowInspectorPage(field,target,x,y,first>capacity?first-capacity:0);return;}
            const auto i=first+row-(first?1:0);
            if(i==end){ShowInspectorPage(field,target,x,y,end);return;}
            CloseMenu();
            if (target==inspectorModel_.Target() && propertyHandler_ && !propertyHandler_(target,field->key,field->options[i]))
                SetStatus(Text("EditorUi.InvalidValue","Invalid value"));
        });
    }
}

std::optional<std::size_t> EditorShell::InspectorNumericAt(float x,float y) const
{
    if (!inspectorList_ || ColorPickerOpen() || MenuOpen() || AddComponentPopupOpen() || closeModalOpen_ || confirmOpen_ ||
        SurfaceForElement(InspectorContent())!=inputSurface_) return {};
    const auto content=layout_.Rect(InspectorContent());
    if(x<content.x||x>=content.x+content.width||y<content.y||y>=content.y+content.height)return {};
    const float localY=y-content.y+inspectorList_->Offset();
    const int row=static_cast<int>(localY/RowHeight);
    if(row<0||row>=static_cast<int>(inspectorRows_.size())||inspectorRows_[row].header)return {};
    const auto& item=inspectorRows_[row];
    for(std::size_t i=0;i<item.fields.size();++i)
    {
        const auto index=item.fields[i];const auto& field=inspectorModel_.Properties()[index];
        if(field.type!=EditorPropertyType::Number||field.readOnly||!(field.dragStep>0))continue;
        const auto cell=FieldAt(inspectorWidth_,static_cast<int>(item.fields.size()),static_cast<int>(i),field.axis!=0,RgbAxis(field.axis));
        const float left=item.fields.size()==1?LeftInset:cell.axisStart;
        const float localX=x-content.x;
        if(field.slider && item.fields.size()==1 && cell.valueWidth>=88.0f)
        {
            if(localX>=LeftInset && localX<cell.valueStart) return index;
            continue;
        }
        if(localX>=left&&localX<cell.valueStart+cell.valueWidth&&
            (localX<cell.valueStart+4||localX>=cell.valueStart+cell.valueWidth-4||
             localY-row*RowHeight<5||localY-row*RowHeight>RowHeight-5))return index;
    }
    return {};
}

bool EditorShell::InspectorNumericCursorAt(float x,float y) const
{return inspectorDrag_ != nullptr || (propertyEditHandler_ && InspectorNumericAt(x,y).has_value());}

void EditorShell::BeginInspectorDrag(std::size_t index,const UiInputEvent& event)
{
    const auto field=inspectorModel_.Properties().at(index);
    double value=0;const auto end=field.value.data()+field.value.size();
    const auto parsed=std::from_chars(field.value.data(),end,value);
    if(parsed.ec!=std::errc{}||parsed.ptr!=end||!std::isfinite(value))return;
    EndInspectorEdit(true);dispatcher_.ClearFocus();
    value=static_cast<float>(value);
    inspectorDrag_=std::make_unique<EditorInspectorDrag>(EditorInspectorDrag{field,inspectorModel_.Target(),inputSurface_,value,value,value,event.x,event.x,false});
    // 持久表面根持有捕获，属性变化导致虚拟行重建时不打断拖动。
    dispatcher_.SetPointerCapture(SurfaceRoot(inputSurface_));
}

void EditorShell::UpdateInspectorDrag(const UiInputEvent& event)
{
    if(!inspectorDrag_)return;
    auto& drag=*inspectorDrag_;
    const double minimum=drag.field.dragMinimum.value_or(-std::numeric_limits<float>::max());
    const double maximum=drag.field.dragMaximum.value_or(std::numeric_limits<float>::max());
    if(minimum>maximum)return;

    if(drag.sliderTrack)
    {
        if(!drag.active)
        {
            if(propertyEditHandler_&&!propertyEditHandler_(drag.target,EditorPropertyEditPhase::Begin))
            {EndInspectorDrag(false);return;}
            drag.active=true;
        }
        const double ratio=std::clamp(static_cast<double>((event.x-drag.sliderLeft)/std::max(1.0f,drag.sliderWidth)),0.0,1.0);
        drag.value=minimum+(maximum-minimum)*ratio;
    }
    else
    {
        if(!drag.active)
        {
            if(std::abs(event.x-drag.startX)<3)return;
            if(!propertyEditHandler_||!propertyEditHandler_(drag.target,EditorPropertyEditPhase::Begin))
            {EndInspectorDrag(false);return;}
            drag.active=true;
        }
        const double factor=(event.shift?10.0:1.0)*(event.control?.1:1.0);
        drag.value+=(event.x-drag.lastX)*drag.field.dragStep*factor;drag.lastX=event.x;
        drag.value=std::clamp(drag.value,minimum,maximum);
    }

    // 用十进制精度吸收 DIP 换算噪声，避免 0.4 被显示成 0.39999998。
    const double quantum = drag.field.dragStep >= 1.0f ? 1.0
        : std::pow(10.0,std::floor(std::log10(std::max(1e-5f, drag.field.dragStep))+1e-6)-2);
    const float next=static_cast<float>(std::clamp(std::round(drag.value/quantum)*quantum,minimum,maximum));
    if(next==drag.applied && !drag.sliderTrack)return;
    char buffer[64];
    std::string textVal;
    if (drag.field.dragStep >= 1.0f && std::abs(next - std::round(next)) < 1e-4f)
    {
        const long long iv = std::llround(next);
        const auto [end,status]=std::to_chars(buffer,buffer+sizeof(buffer),iv);
        if(status==std::errc{}) textVal.assign(buffer,end);
    }
    if (textVal.empty())
    {
        const auto [end,status]=std::to_chars(buffer,buffer+sizeof(buffer),next);
        if(status==std::errc{}) textVal.assign(buffer,end);
    }
    if(!textVal.empty()&&propertyHandler_&&propertyHandler_(drag.target,drag.field.key,textVal))
        drag.applied=next;
}

void EditorShell::EndInspectorDrag(bool commit)
{
    if(!inspectorDrag_)return;
    auto drag=std::move(*inspectorDrag_);inspectorDrag_.reset();
    dispatcher_.ReleasePointerCapture();
    if(drag.active)
    {
        const bool changed=drag.applied!=drag.original;
        const auto phase=commit&&changed?EditorPropertyEditPhase::Commit:EditorPropertyEditPhase::Cancel;
        if(propertyEditHandler_&&!propertyEditHandler_(drag.target,phase))
        {
            if(phase==EditorPropertyEditPhase::Commit)propertyEditHandler_(drag.target,EditorPropertyEditPhase::Cancel);
            SetStatus(Text("EditorUi.InvalidValue","Invalid value"));
        }
    }
    else if(commit && !drag.sliderTrack)
    {
        const auto& fields=inspectorModel_.Properties();
        const auto found=std::find_if(fields.begin(),fields.end(),[&](const auto& p){return p.key==drag.field.key;});
        if(found!=fields.end())BeginInspectorEdit(static_cast<std::size_t>(found-fields.begin()));
    }
}

bool EditorShell::BeginRename()
{
    if (activePanel_ == EditorPanelId::Hierarchy && hierarchyModel_.Selected() != 0 &&
        BeginHierarchyRename(hierarchyModel_.Selected()))
    {
        return true;
    }
    if (activePanel_ == EditorPanelId::Assets && !assetsModel_.SelectedPath().empty() &&
        BeginAssetRename(assetsModel_.SelectedPath()))
    {
        return true;
    }
    const auto& fields = inspectorModel_.Properties();
    const auto name = std::find_if(fields.begin(), fields.end(), [](const auto& p) { return p.key == "name"; });
    if (name == fields.end() || !inspectorList_) return false;
    inspectorList_->SetScrollOffset(0.0f);
    inspectorList_->Sync();
    return BeginInspectorEdit(static_cast<std::size_t>(name - fields.begin()));
}

UiRect EditorShell::InspectorFieldRect(std::string_view key) const
{
    if (!inspectorList_) return {};
    const auto content = layout_.Rect(InspectorContent());
    if (content.width <= 1.0f || content.height <= 1.0f) return {};
    const auto& fields = inspectorModel_.Properties();
    for (std::size_t row = 0; row < inspectorRows_.size(); ++row)
    {
        const auto& item = inspectorRows_[row];
        if (item.header) continue;
        for (std::size_t i = 0; i < item.fields.size(); ++i)
        {
            const auto& field = fields[item.fields[i]];
            if (field.key != key) continue;
            const float y = content.y + static_cast<float>(row) * RowHeight - inspectorList_->Offset();
            if (y < content.y || y + RowHeight > content.y + content.height) return {};
            if (field.type == EditorPropertyType::Action)
            {
                const float right = std::max(LeftInset, inspectorWidth_ - ScrollMargin - CardInset);
                const float width = std::min(230.0f, std::max(0.0f, right - 2.0f * LeftInset));
                return {content.x + (right - width + CardInset) * 0.5f, y + 2.0f, width, RowHeight - 4.0f};
            }
            if (field.key == "entity.enabled" && item.fields.size() == 2 &&
                fields[item.fields[1]].key == "name")
            {
                return {content.x + LeftInset, y + (RowHeight - 16.0f) * 0.5f, 16.0f, 16.0f};
            }
            if (field.key == "name")
            {
                const float nameLeft = (item.fields.size() == 2 && fields[item.fields[0]].key == "entity.enabled")
                    ? (LeftInset + 22.0f) : LeftInset;
                const float right = std::max(nameLeft, inspectorWidth_ - ScrollMargin - CardInset);
                return {content.x + nameLeft, y + 2.5f, std::max(0.0f, right - nameLeft), RowHeight - 5.0f};
            }
            const auto cell = FieldAt(inspectorWidth_, static_cast<int>(item.fields.size()), static_cast<int>(i),
                                      field.axis != 0, RgbAxis(field.axis));
            return {content.x + cell.valueStart, y + 2.5f, cell.valueWidth, RowHeight - 5.0f};
        }
    }
    return {};
}
}
