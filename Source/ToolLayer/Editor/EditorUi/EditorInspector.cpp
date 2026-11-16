#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/EditorUi/EditorInk.h"
#include "CoreLayer/Utf.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace se::ui::editor
{
namespace
{
constexpr float RowHeight = 22.0f;
constexpr float CompactWidth = 224.0f;
constexpr float ScrollMargin = 9.0f;
constexpr float LeftInset = 6.0f;
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
    const float right = std::max(LeftInset, width - ScrollMargin);
    const float labelEnd = rgb ? std::min(50.0f, right * 0.22f) : std::min(119.0f, right * 0.36f);
    const float span = (right - labelEnd) / count;
    const float axisStart = labelEnd + cell * span;
    const float axisWidth = axis ? std::min(14.0f, span * 0.25f) : 0.0f;
    return {labelEnd, axisStart, axisStart + axisWidth, std::max(0.0f, span - axisWidth - 2.0f)};
}
}

void EditorShell::BuildInspector(PanelSlot& slot)
{
    inspectorList_ = std::make_unique<UiListView>(document_, slot.content);
    inspectorList_->SetRowHeight(RowHeight);
    inspectorList_->SetRowBuilder([this](int index, UiElementHandle row) { BuildInspectorRow(index, row); });
    dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
        inspectorList_->SetScrollOffset(inspectorList_->Offset() + event.wheel * 2 * RowHeight);
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
    const auto& properties = inspectorModel_.Properties();
    std::vector<std::string> sections;
    for (const auto& property : properties)
        if (std::find(sections.begin(), sections.end(), property.section) == sections.end())
            sections.push_back(property.section);
    // 选择对象后优先展示组件；辅助设置和添加操作位于属性表末尾。
    for (const auto& last : {Text("EditorUi.ObjectDetails", "Object details"),
                             Text("EditorUi.SceneEnvironment", "Scene environment"),
                             Text("EditorUi.AddComponent", "Add Component")})
    {
        const auto i = std::find(sections.begin(), sections.end(), last);
        if (i != sections.end()) std::rotate(i, i + 1, sections.end());
    }

    for (const auto& section : sections)
    {
        bool closed = false;
        if (!section.empty())
        {
            auto first = std::find_if(properties.begin(), properties.end(),
                [&](const auto& p) { return p.section == section; });
            closed = inspectorFolds_.try_emplace(section, first->sectionInitiallyClosed).first->second;
            inspectorRows_.push_back({section, true, closed, {}});
            auto enabled = std::find_if(properties.begin(), properties.end(),
                [&](const auto& p) { return p.section == section && p.sectionEnabled; });
            if (enabled != properties.end())
                inspectorRows_.back().fields.push_back(static_cast<std::size_t>(enabled - properties.begin()));
        }
        if (closed) continue;
        for (std::size_t i = 0; i < properties.size(); ++i)
        {
            const auto& property = properties[i];
            if (property.section != section || property.sectionEnabled) continue;
            if ((property.axis == 'X' || property.axis == 'R') && inspectorWidth_ >= CompactWidth && i + 2 < properties.size() &&
                properties[i+1].axis == (property.axis == 'X' ? 'Y' : 'G') && properties[i+2].axis == (property.axis == 'X' ? 'Z' : 'B') &&
                properties[i+1].section == section && properties[i+2].section == section &&
                properties[i+1].label == property.label && properties[i+2].label == property.label)
            {
                inspectorRows_.push_back({property.label, false, false, {i, i+1, i+2}});
                i += 2;
                continue;
            }
            inspectorRows_.push_back({property.label, false, false, {i}});
        }
    }
    inspectorList_->SetItemCount(static_cast<int>(inspectorRows_.size()));
    inspectorList_->Invalidate();
    inspectorList_->Sync();
}

void EditorShell::BuildInspectorRow(int index, UiElementHandle row)
{
    const auto& item = inspectorRows_.at(index);
    const auto& fields = inspectorModel_.Properties();
    const float right = std::max(LeftInset, inspectorWidth_ - ScrollMargin);
    auto add = [&](std::string_view text, UiStyle style, UiElementKind kind = UiElementKind::Label) {
        auto element = document_.CreateElement(kind);
        document_.SetParent(element, row);
        document_.SetStyle(element, style);
        document_.SetText(element, text);
    };
    auto check = [&](const EditorProperty& p, float x) {
        auto style = CellStyle(x, (RowHeight - 16) * 0.5f, 16, 16, UiInk(flare::workspace::Field));
        style.padding = {2,0,0,0};
        style.borderWidth = 1;
        style.borderColor = UiInk(flare::workspace::Divider);
        style.color = UiInk(p.readOnly ? flare::Disabled : flare::Heading);
        add(p.value == "true" ? "✓" : "", style);
    };
    if (item.header)
    {
        if (item.title == Text("EditorUi.AddComponent", "Add Component"))
        {
            const float width = std::max(0.0f, std::min(210.0f, right - 2 * LeftInset));
            auto style = CellStyle((right-width)*0.5f, 1, width, RowHeight - 2, UiInk(flare::workspace::Header));
            const auto caption = item.title + (item.closed ? "  ▾" : "  ▴");
            style.padding.left = std::max(3.0f, (width - MeasuredWidth(caption, 13.0f))*0.5f);
            style.borderWidth = 1;
            style.borderColor = UiInk(flare::workspace::Divider);
            add(caption, style);
            return;
        }
        auto background = document_.Style(row);
        background.background = UiInk(flare::workspace::Header);
        document_.SetStyle(row, background);
        auto disclosure = CellStyle(LeftInset, 0, 15, RowHeight, Transparent);
        disclosure.padding = {};
        disclosure.fontSize = 10;
        add(item.closed ? "▶" : "▼", disclosure);
        float left = LeftInset + 15;
        if (!item.fields.empty())
        {
            check(fields[item.fields.front()], left);
            left += 19;
        }
        auto title = CellStyle(left, 0, right-left, RowHeight, Transparent);
        title.color = UiInk(flare::Heading);
        add(item.title, title);
        return;
    }

    const auto& first = fields[item.fields.front()];
    if (first.type == EditorPropertyType::Action)
    {
        const float width = std::min(230.0f, std::max(0.0f, right - 2*LeftInset));
        auto style = CellStyle((right-width)*0.5f, 1, width, RowHeight - 2, UiInk(flare::workspace::Header));
        style.padding.left = std::max(4.0f, (width - MeasuredWidth(first.label, 13.0f))*0.5f);
        style.color = UiInk(first.readOnly ? flare::Disabled : flare::Heading);
        add(first.label, style);
        return;
    }
    const auto count = static_cast<int>(item.fields.size());
    const bool objectName = first.key == "name";
    const auto firstCell = FieldAt(inspectorWidth_, count, 0, first.axis != 0, RgbAxis(first.axis));
    if (!objectName)
        add(first.label, CellStyle(LeftInset, 0, firstCell.labelEnd - LeftInset, RowHeight, Transparent));
    for (int i = 0; i < count; ++i)
    {
        const auto& field = fields[item.fields[i]];
        const auto cell = FieldAt(inspectorWidth_, count, i, field.axis != 0, RgbAxis(field.axis));
        if (field.axis)
        {
            auto style = CellStyle(cell.axisStart, 0, cell.valueStart-cell.axisStart, RowHeight, Transparent);
            style.color = UiInk(flare::Muted);
            style.padding = {};
            add(std::string(1, field.axis), style);
        }
        if (field.type == EditorPropertyType::Boolean)
        {
            check(field, cell.valueStart);
            continue;
        }
        auto style = CellStyle(objectName ? LeftInset : cell.valueStart, 2,
            objectName ? right - LeftInset : cell.valueWidth, RowHeight - 4.0f, UiInk(flare::workspace::Field));
        style.color = UiInk(field.readOnly ? flare::Muted : flare::Heading);
        style.borderColor = UiInk(flare::workspace::Divider);
        style.borderWidth = 1;
        if (field.type == EditorPropertyType::Color)
        {
            float r=0,g=0,b=0;std::istringstream value(field.value);
            if (value >> r >> g >> b)
                style.background = {std::clamp(r,0.f,1.f),std::clamp(g,0.f,1.f),std::clamp(b,0.f,1.f),1};
            add("", style);
        }
        else
            add((field.displayValue.empty() ? field.value : field.displayValue) +
                (field.type == EditorPropertyType::Combo ? "  ▾" : ""), style);
    }
}

void EditorShell::ActivateInspectorRow(UiInputEvent& event)
{
    if (event.button != 0) return;
    if (const auto field = InspectorNumericAt(event.x,event.y); field && propertyEditHandler_)
    {
        BeginInspectorDrag(*field,event);event.handled=true;return;
    }
    const auto content = layout_.Rect(InspectorContent());
    const float x = event.x - content.x;
    const int row = static_cast<int>(std::floor((event.y - content.y + inspectorList_->Offset()) / RowHeight));
    if (row < 0 || row >= static_cast<int>(inspectorRows_.size())) return;
    const auto& item = inspectorRows_[row];
    std::size_t field = 0;
    if (item.header)
    {
        const float checkX = LeftInset + 15;
        if (item.fields.empty() || x < checkX || x >= checkX + 16)
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
        if (property.type == EditorPropertyType::Action)
        {
            const float right = std::max(LeftInset, inspectorWidth_ - ScrollMargin);
            const float width = std::min(230.0f, std::max(0.0f, right - 2*LeftInset));
            if (x < (right-width)*0.5f || x >= (right+width)*0.5f) return;
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
            if (cell == count) return;
            field = item.fields[cell];
        }
    }
    const auto& property = inspectorModel_.Properties()[field];
    if (property.readOnly) return;
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
            const float y = static_cast<float>(row)*RowHeight - inspectorList_->Offset() + 2;
            if (y < 0 || y + RowHeight - 4 > inspectorList_->ViewportHeight()) break;
            const auto cell = FieldAt(width, static_cast<int>(item.fields.size()), static_cast<int>(i), field.axis != 0, RgbAxis(field.axis));
            const bool name = field.key == "name";
            auto style = CellStyle(name ? LeftInset : cell.valueStart, y,
                name ? width - ScrollMargin - LeftInset : cell.valueWidth, RowHeight-4, UiInk(flare::workspace::Field));
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
    if (field.type == EditorPropertyType::Number || field.type == EditorPropertyType::Asset)
        inspectorEdit_->Select(0, std::numeric_limits<std::uint32_t>::max());
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
    const auto value = std::string(inspectorEdit_->Text());
    document_.SetVisible(inspectorEditElement_, false);
    const auto& fields = inspectorModel_.Properties();
    const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& p) { return p.key == key; });
    if (commit && propertyHandler_ && found != fields.end() && !found->readOnly &&
        !propertyHandler_(inspectorModel_.Target(), key, value))
        SetStatus(Text("EditorUi.InvalidValue", "Invalid value"));
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
    if (!inspectorList_ || ColorPickerOpen() || MenuOpen() || closeModalOpen_ || confirmOpen_ ||
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
        if(localX>=left&&localX<cell.valueStart+cell.valueWidth&&
            (localX<cell.valueStart+4||localX>=cell.valueStart+cell.valueWidth-4||
             localY-row*RowHeight<5||localY-row*RowHeight>RowHeight-5))return index;
    }
    return {};
}

bool EditorShell::InspectorNumericCursorAt(float x,float y) const
{return inspectorDrag_.has_value() || (propertyEditHandler_ && InspectorNumericAt(x,y).has_value());}

void EditorShell::BeginInspectorDrag(std::size_t index,const UiInputEvent& event)
{
    const auto field=inspectorModel_.Properties().at(index);
    double value=0;const auto end=field.value.data()+field.value.size();
    const auto parsed=std::from_chars(field.value.data(),end,value);
    if(parsed.ec!=std::errc{}||parsed.ptr!=end||!std::isfinite(value))return;
    EndInspectorEdit(true);dispatcher_.ClearFocus();
    value=static_cast<float>(value);
    inspectorDrag_=InspectorDrag{field,inspectorModel_.Target(),inputSurface_,value,value,value,event.x,event.x,false};
    // 持久表面根持有捕获，属性变化导致虚拟行重建时不打断拖动。
    dispatcher_.SetPointerCapture(SurfaceRoot(inputSurface_));
}

void EditorShell::UpdateInspectorDrag(const UiInputEvent& event)
{
    if(!inspectorDrag_)return;
    auto& drag=*inspectorDrag_;
    if(!drag.active)
    {
        if(std::abs(event.x-drag.startX)<3)return;
        if(!propertyEditHandler_||!propertyEditHandler_(drag.target,EditorPropertyEditPhase::Begin))
        {EndInspectorDrag(false);return;}
        drag.active=true;
    }
    const double factor=(event.shift?10.0:1.0)*(event.control?.1:1.0);
    drag.value+=(event.x-drag.lastX)*drag.field.dragStep*factor;drag.lastX=event.x;
    const double minimum=drag.field.dragMinimum.value_or(-std::numeric_limits<float>::max());
    const double maximum=drag.field.dragMaximum.value_or(std::numeric_limits<float>::max());
    if(minimum>maximum)return;
    drag.value=std::clamp(drag.value,minimum,maximum);
    // 用十进制精度吸收 DIP 换算噪声，避免 0.4 被显示成 0.39999998。
    const double quantum=std::pow(10.0,std::floor(std::log10(drag.field.dragStep)+1e-6)-2);
    const float next=static_cast<float>(std::clamp(std::round(drag.value/quantum)*quantum,minimum,maximum));
    if(next==drag.applied)return;
    char buffer[64];const auto [end,status]=std::to_chars(buffer,buffer+sizeof(buffer),next);
    if(status==std::errc{}&&propertyHandler_&&propertyHandler_(drag.target,drag.field.key,std::string(buffer,end)))
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
    else if(commit)
    {
        const auto& fields=inspectorModel_.Properties();
        const auto found=std::find_if(fields.begin(),fields.end(),[&](const auto& p){return p.key==drag.field.key;});
        if(found!=fields.end())BeginInspectorEdit(static_cast<std::size_t>(found-fields.begin()));
    }
}

bool EditorShell::BeginRename()
{
    const auto& fields = inspectorModel_.Properties();
    const auto name = std::find_if(fields.begin(), fields.end(), [](const auto& p) { return p.key == "name"; });
    if (name == fields.end() || !inspectorList_) return false;
    inspectorList_->SetScrollOffset(0.0f);
    inspectorList_->Sync();
    return BeginInspectorEdit(static_cast<std::size_t>(name - fields.begin()));
}
}
