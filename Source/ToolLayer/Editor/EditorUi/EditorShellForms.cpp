#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/EditorUi/EditorShellInternal.h"
#include "ToolLayer/Editor/EditorUi/EditorDockState.h"
#include "ToolLayer/Editor/EditorUi/EditorInk.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Utf.h"
#include "ToolLayer/Editor/FlarePalette.h"
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace flare::ui::editor
{
using namespace detail;

EditorShell::FormPanel& EditorShell::Form(EditorPanelId panel)
{
    switch (panel)
    {
    case EditorPanelId::Material: return *forms_[0];
    case EditorPanelId::ProjectSettings: return *forms_[1];
    case EditorPanelId::BuildExport: return *forms_[2];
    case EditorPanelId::Preferences: return *forms_[3];
    case EditorPanelId::AssetTools: return *forms_[4];
    default: return *forms_[0];
    }
}

const EditorShell::FormPanel& EditorShell::Form(EditorPanelId panel) const
{
    switch (panel)
    {
    case EditorPanelId::Material: return *forms_[0];
    case EditorPanelId::ProjectSettings: return *forms_[1];
    case EditorPanelId::BuildExport: return *forms_[2];
    case EditorPanelId::Preferences: return *forms_[3];
    case EditorPanelId::AssetTools: return *forms_[4];
    default: return *forms_[0];
    }
}

void EditorShell::BuildFormPanel(PanelSlot& slot)
{
    FormPanel& form = Form(slot.id);
    form.listContent = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(form.listContent, slot.content))
    {
        return;
    }
    UiStyle listStyle = AbsoluteStyle(0.0f, 32.0f, 100.0f, 100.0f);
    listStyle.width = UiLength::Percent(100.0f);
    listStyle.background = kContent;
    listStyle.overflow = UiOverflow::Hidden;
    document_.SetStyle(form.listContent, listStyle);
    form.list = std::make_unique<UiListView>(document_, form.listContent);
    form.list->SetRowTextColor(kText);
    form.list->SetRowTextLayout(UiTextFlow::SingleLineEllipsis, {10, 0, 10, 0});
    form.list->SetRowHeight(26.0f);
    const EditorPanelId panelId = slot.id;
    form.list->SetRowBuilder([this, &form, panelId](int index, UiElementHandle row) {
        if (index < 0 || static_cast<std::size_t>(index) >= form.properties.size()) return;
        const EditorProperty& property = form.properties[static_cast<std::size_t>(index)];
        std::string display = property.displayValue.empty() ? property.value : property.displayValue;
        for (std::size_t i = 0; i < property.options.size() && i < property.optionLabels.size(); ++i)
            if (property.options[i] == property.value) display = property.optionLabels[i];

        float width = 320.0f;
        for (const GroupView& group : groups_)
        {
            if (std::find(group.tabs.begin(), group.tabs.end(), panelId) != group.tabs.end())
            {
                width = std::max(160.0f, group.rect.width);
                break;
            }
        }
        if (property.type == EditorPropertyType::Color)
        {
            document_.SetText(row, property.label + "   " + display);
            return;
        }
        if (property.type == EditorPropertyType::Action)
        {
            const auto pill = document_.CreateElement(UiElementKind::Label);
            if (document_.SetParent(pill, row))
            {
                const float btnW = std::min(width - 20.0f, std::max(140.0f, MeasuredWidth(property.label, 12.5f) + 28.0f));
                auto s = ButtonStyle(10.0f, 2.0f, btnW, 22.0f, kSurfaceRaised);
                s.cornerRadius = 5.0f;
                s.borderWidth = 1.0f;
                s.borderColor = kBorder;
                s.color = UiInk(flare::palette::Gold);
                s.pointerEvents = UiPointerEvents::None;
                document_.SetStyle(pill, s);
                document_.SetText(pill, property.label);
            }
            return;
        }
        if (width < 320.0f)
        {
            const std::string suffix = property.type == EditorPropertyType::Combo ? " ▾" : "";
            document_.SetText(row, property.label + ": " + display + suffix);
            return;
        }
        const float labelW = std::clamp(width * 0.40f, 130.0f, 260.0f);
        const float fieldX = labelW + 12.0f;
        const float fieldW = std::max(60.0f, width - fieldX - 16.0f);
        const auto lbl = document_.CreateElement(UiElementKind::Label);
        if (document_.SetParent(lbl, row))
        {
            auto ls = ButtonStyle(10.0f, 1.0f, labelW - 6.0f, 24.0f, UiColor{0, 0, 0, 0});
            ls.color = property.readOnly ? kMuted : kText;
            ls.fontSize = 12.5f;
            ls.padding = {2.0f, 0.0f, 4.0f, 0.0f};
            ls.pointerEvents = UiPointerEvents::None;
            document_.SetStyle(lbl, ls);
            document_.SetText(lbl, property.label);
        }
        const auto val = document_.CreateElement(UiElementKind::Label);
        if (document_.SetParent(val, row))
        {
            if (property.type == EditorPropertyType::Boolean)
            {
                const bool checked = (property.value == "true");
                auto vs = ButtonStyle(fieldX, 3.0f, 20.0f, 20.0f, checked ? kSurfaceActive : kSurface);
                vs.cornerRadius = 4.0f;
                vs.borderWidth = 1.0f;
                vs.borderColor = checked ? UiInk(flare::palette::workspace::Focus) : kBorder;
                vs.color = checked ? UiInk(flare::palette::Gold) : kMuted;
                vs.padding = {3.0f, 0.0f, 0.0f, 0.0f};
                vs.pointerEvents = UiPointerEvents::None;
                document_.SetStyle(val, vs);
                document_.SetText(val, checked ? "✓" : "");
            }
            else
            {
                const std::string textVal = property.type == EditorPropertyType::Combo ? (display + "  ▾") : display;
                auto vs = ButtonStyle(fieldX, 2.0f, fieldW, 22.0f,
                                      property.readOnly ? UiColor{0, 0, 0, 0} : kSurface);
                vs.cornerRadius = 4.0f;
                vs.borderWidth = property.readOnly ? 0.0f : 1.0f;
                vs.borderColor = kBorder;
                vs.color = property.readOnly ? kMuted : kText;
                vs.fontSize = 12.5f;
                vs.padding = {8.0f, 0.0f, 8.0f, 0.0f};
                vs.pointerEvents = UiPointerEvents::None;
                document_.SetStyle(val, vs);
                document_.SetText(val, textVal);
            }
        }
    });
    const auto commitFormInlineEdit = [this, &form]() {
        if (form.editingKey.empty() || !form.editElement.IsValid() || !document_.Visible(form.editElement))
            return;
        const std::string key = form.editingKey;
        const std::string textVal = form.edit ? std::string(form.edit->Text()) : std::string{};
        form.editingKey.clear();
        document_.SetVisible(form.editElement, false);
        bool committed = false;
        if (form.commit)
            committed = form.commit(key, textVal);
        if (committed)
        {
            for (EditorProperty& property : form.properties)
            {
                if (property.key == key) property.value = textVal;
            }
            form.status = Text("EditorUi.Applied", "Applied");
        }
        else
        {
            form.status = Text("EditorUi.InvalidValue", "Invalid value");
        }
        RefreshForm(form);
    };

    dispatcher_.AddHandler(form.listContent, UiEventType::PointerWheel, [this, &form, commitFormInlineEdit](UiInputEvent& event) {
        commitFormInlineEdit();
        if (dispatcher_.Focus() == form.editElement) dispatcher_.ClearFocus();
        if (form.list)
        {
            form.list->SetScrollOffset(form.list->Offset() - event.wheel * 52.0f);
            form.list->Sync();
        }
        event.handled = true;
    }, UiEventPhase::Bubble);
    dispatcher_.AddHandler(form.listContent, UiEventType::PointerDown, [this, &form, panelId](UiInputEvent& event) {
        const UiRect rect = layout_.Rect(form.listContent);
        const float rowH = form.list ? form.list->RowHeight() : 26.0f;
        const float offset = form.list ? form.list->Offset() : 0.0f;
        const int row = static_cast<int>((event.y - rect.y + offset) / rowH);
        if (row < 0 || static_cast<std::size_t>(row) >= form.properties.size() ||
            form.properties[static_cast<std::size_t>(row)].readOnly)
        {
            event.handled = true;
            return;
        }
        EditorProperty& property = form.properties[static_cast<std::size_t>(row)];
        if(property.type==EditorPropertyType::Action)
        {
            if(form.commit)form.commit(property.key,{});
            event.handled=true;
            return;
        }
        if (property.type == EditorPropertyType::Combo && !property.options.empty())
        {
            ShowFormCombo(panelId, form, row);
            event.handled = true;
            return;
        }
        if (property.type == EditorPropertyType::Boolean)
        {
            const std::string next = property.value == "true" ? "false" : "true";
            if (form.commit && form.commit(property.key, next))
            {
                property.value = next;
                form.status = Text("EditorUi.Applied", "Applied");
            }
            else
            {
                form.status = Text("EditorUi.InvalidValue", "Invalid value");
            }
            RefreshForm(form);
            event.handled = true;
            return;
        }
        form.editingKey = form.properties[static_cast<std::size_t>(row)].key;
        if (form.edit && form.editElement.IsValid())
        {
            const float contentW = std::max(120.0f, rect.width);
            const bool twoCol = contentW >= 320.0f;
            const float labelW = twoCol ? std::clamp(contentW * 0.40f, 130.0f, 260.0f) : 0.0f;
            const float editX = twoCol ? (labelW + 12.0f) : 6.0f;
            const float editW = twoCol ? std::max(60.0f, contentW - editX - 16.0f) : (contentW - 12.0f);
            const UiRect contentRect = layout_.Rect(Slot(panelId).content);
            const float editY = (rect.y - contentRect.y) + static_cast<float>(row) * rowH - offset + 1.0f;
            auto editStyle = ButtonStyle(editX, editY, editW, rowH - 2.0f, kSurface);
            editStyle.cornerRadius = 4.0f;
            editStyle.borderWidth = 1.0f;
            editStyle.borderColor = UiInk(flare::palette::workspace::Focus);
            document_.SetStyle(form.editElement, editStyle);
            form.edit->SetText(form.properties[static_cast<std::size_t>(row)].value);
            document_.SetVisible(form.editElement, true);
            dispatcher_.SetFocus(form.editElement);
        }
        event.handled = true;
    }, UiEventPhase::Bubble);

    form.saveButton = document_.CreateElement(UiElementKind::Button);
    if (document_.SetParent(form.saveButton, slot.content))
    {
        auto saveStyle = ButtonStyle(4.0f, 3.0f, 80.0f, 24.0f, kSurfaceActive);
        saveStyle.cornerRadius = 5.0f;
        saveStyle.borderWidth = 1.0f;
        saveStyle.borderColor = UiInk(flare::palette::workspace::Focus, 0.65f);
        saveStyle.color = UiInk(flare::palette::Gold);
        document_.SetStyle(form.saveButton, saveStyle);
        document_.SetText(form.saveButton, Text("EditorUi.Save", "Save"));
        dispatcher_.AddHandler(form.saveButton, UiEventType::PointerDown, [this, &form](UiInputEvent& event) {
            if (form.save && form.save())
            {
                if(!form.managedStatus)form.status = Text("EditorUi.Saved", "Saved");
            }
            else
            {
                form.status = form.status.empty() ? Text("EditorUi.SaveFailed", "Save failed") : form.status;
            }
            RefreshForm(form);
            event.handled = true;
        });
    }
    form.actionButton = document_.CreateElement(UiElementKind::Button);
    if (document_.SetParent(form.actionButton, slot.content))
    {
        auto actStyle = ButtonStyle(88.0f, 3.0f, 84.0f, 24.0f, kSurfaceRaised);
        actStyle.cornerRadius = 5.0f;
        actStyle.borderWidth = 1.0f;
        actStyle.borderColor = kBorder;
        document_.SetStyle(form.actionButton, actStyle);
        document_.SetText(form.actionButton, "Action");
        document_.SetVisible(form.actionButton, false);
        dispatcher_.AddHandler(form.actionButton, UiEventType::PointerDown, [this, &form, commitFormInlineEdit](UiInputEvent& event) {
            commitFormInlineEdit();
            if (dispatcher_.Focus() == form.editElement) dispatcher_.ClearFocus();
            if (form.action && form.action())
            {
                if(!form.managedStatus)form.status = Text("EditorUi.Running", "Running");
            }
            RefreshForm(form);
            event.handled = true;
        });
    }
    form.statusLabel = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(form.statusLabel, slot.content))
    {
        UiStyle statusStyle = ButtonStyle(178.0f, 3.0f, 100.0f, 24.0f, kContent);
        statusStyle.color = kMuted;
        statusStyle.fontSize = 12.0f;
        document_.SetStyle(form.statusLabel, statusStyle);
    }
    form.comboPopup = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(form.comboPopup, slot.panel))
    {
        UiStyle popup;
        popup.position = UiPosition::Absolute;
        popup.left = UiLength::Px(0.0f);
        popup.top = UiLength::Px(0.0f);
        popup.width = UiLength::Px(180.0f);
        popup.height = UiLength::Px(22.0f);
        popup.background = kSurfaceRaised;
        popup.borderWidth = 1.0f;
        popup.borderColor = kAccent;
        document_.SetStyle(form.comboPopup, popup);
        document_.SetVisible(form.comboPopup, false);
        dispatcher_.AddHandler(form.comboPopup, UiEventType::PointerDown, [](UiInputEvent& event) {
            event.handled = true;
            event.propagationStopped = true;
        });
    }
    form.editElement = document_.CreateElement(UiElementKind::TextEdit);
    if (document_.SetParent(form.editElement, slot.content))
    {
        document_.SetStyle(form.editElement, ButtonStyle(0.0f, 0.0f, 100.0f, 24.0f, kContent));
        document_.SetVisible(form.editElement, false);
        form.edit = std::make_unique<UiTextEdit>(document_, form.editElement, text_, PlatformServices());
        dispatcher_.AddHandler(form.editElement, UiEventType::FocusLost, [commitFormInlineEdit](UiInputEvent&) {
            commitFormInlineEdit();
        });
        dispatcher_.AddHandler(form.editElement, UiEventType::PointerDown, [this, &form](UiInputEvent&) {
            dispatcher_.SetFocus(form.editElement);
        });
        dispatcher_.AddHandler(form.editElement, UiEventType::KeyDown, [this, &form, commitFormInlineEdit](UiInputEvent& event) {
            if (event.key == UiKeyEnter)
            {
                commitFormInlineEdit();
                dispatcher_.ClearFocus();
                event.handled = true;
                return;
            }
            if (event.key == UiKeyEscape)
            {
                form.editingKey.clear();
                document_.SetVisible(form.editElement, false);
                dispatcher_.ClearFocus();
                event.handled = true;
                return;
            }
            if (form.edit)
            {
                form.edit->HandleKey(event.key, event.shift, event.control);
            }
            event.handled = true;
        });
        dispatcher_.AddHandler(form.editElement, UiEventType::TextInput, [this, &form](UiInputEvent& event) {
            if (!form.edit || event.codepoint == 0)
            {
                return;
            }
            const char32_t codepoint = event.codepoint;
            std::string utf8;
            if (!EncodeUtf8({&codepoint, 1}, utf8))
            {
                return;
            }
            form.edit->InsertText(utf8);
            event.handled = true;
        });
        dispatcher_.AddHandler(form.editElement, UiEventType::ImeComposition, [this, &form](UiInputEvent& event) {
            if (form.edit) form.edit->SetComposition(event.text);
        });
        dispatcher_.AddHandler(form.editElement, UiEventType::ImeCommit, [this, &form](UiInputEvent& event) {
            if (form.edit)
            {
                form.edit->SetComposition(event.text);
                form.edit->CommitComposition();
            }
        });
    }
    RefreshForm(form);
}

UiElementHandle EditorShell::FormComboOption(EditorPanelId panel, std::size_t index) const noexcept
{
    const FormPanel& form = Form(panel);
    return index < form.comboOptions.size() ? form.comboOptions[index] : UiElementHandle{};
}

void EditorShell::HideFormCombo(FormPanel& form)
{
    if (form.comboPopup.IsValid())
    {
        document_.SetVisible(form.comboPopup, false);
        for (const UiElementHandle option : form.comboOptions)
        {
            if (option.IsValid())
            {
                document_.SetVisible(option, false);
                pendingDestroy_.push_back(option);
            }
        }
    }
    form.comboOptions.clear();
    form.comboRow = -1;
}

void EditorShell::ShowFormCombo(EditorPanelId panel, FormPanel& form, int row)
{
    if (!form.comboPopup.IsValid() || row < 0 || static_cast<std::size_t>(row) >= form.properties.size())
    {
        return;
    }
    HideFormCombo(form);
    const EditorProperty& property = form.properties[static_cast<std::size_t>(row)];
    if (property.type != EditorPropertyType::Combo || property.options.empty())
    {
        return;
    }
    const UiRect panelRect = layout_.Rect(Slot(panel).panel);
    const UiRect listRect = layout_.Rect(form.listContent);
    const float rowTop = listRect.y + static_cast<float>(row) * form.list->RowHeight() - form.list->Offset();
    const bool twoCol = panelRect.width >= 320.0f;
    const float comboLeft = twoCol ? (std::clamp(panelRect.width * 0.40f, 130.0f, 260.0f) + 12.0f) : 4.0f;
    const float popupWidth = twoCol
        ? std::max(100.0f, panelRect.width - comboLeft - 16.0f)
        : std::max(80.0f, std::min(220.0f, panelRect.width - 8.0f));
    UiStyle popup = document_.Style(form.comboPopup);
    popup.left = UiLength::Px(comboLeft);
    popup.top = UiLength::Px(std::max(0.0f, rowTop + form.list->RowHeight() - panelRect.y));
    popup.width = UiLength::Px(popupWidth);
    popup.height = UiLength::Px(static_cast<float>(property.options.size()) * 22.0f);
    popup.cornerRadius = 5.0f;
    document_.SetStyle(form.comboPopup, popup);
    document_.SetVisible(form.comboPopup, true);
    form.comboRow = row;
    for (std::size_t index = 0; index < property.options.size(); ++index)
    {
        const UiElementHandle option = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(option, form.comboPopup))
        {
            continue;
        }
        document_.SetStyle(option, ButtonStyle(0.0f, static_cast<float>(index) * 22.0f, popupWidth, 22.0f,
            property.options[index] == property.value ? kSurfaceActive : kSurfaceRaised));
        document_.SetText(option, index<property.optionLabels.size()?property.optionLabels[index]:property.options[index]);
        form.comboOptions.push_back(option);
        const std::string value = property.options[index];
        dispatcher_.AddHandler(option, UiEventType::PointerDown, [this, &form, panel, row, value](UiInputEvent& event) {
            EditorProperty& target = form.properties[static_cast<std::size_t>(row)];
            if (form.commit && form.commit(target.key, value))
            {
                target.value = value;
                form.status = Text("EditorUi.Applied", "Applied");
            }
            else
            {
                form.status = Text("EditorUi.InvalidValue", "Invalid value");
            }
            HideFormCombo(form);
            RefreshForm(form);
            event.handled = true;
        });
    }
}

void EditorShell::RefreshForm(FormPanel& form)
{
    HideFormCombo(form);
    if (form.list)
    {
        form.list->SetRowBackgroundProvider([&form](int index) {
            if (index < 0 || static_cast<std::size_t>(index) >= form.properties.size())
            {
                return UiColor{0.0f, 0.0f, 0.0f, 0.0f};
            }
            return RowBackgroundColor(form.properties[static_cast<std::size_t>(index)]);
        });
        form.list->Invalidate();
        form.list->SetItemCount(static_cast<int>(form.properties.size()));
        form.list->Sync();
    }
    if (form.statusLabel.IsValid())
    {
        document_.SetText(form.statusLabel, form.status);
    }
    if (form.actionButton.IsValid())
    {
        document_.SetVisible(form.actionButton, !form.actionLabel.empty());
        document_.SetText(form.actionButton, form.actionLabel.empty() ? "Action" : form.actionLabel);
    }
    if (form.editElement.IsValid())
    {
        document_.SetVisible(form.editElement, !form.editingKey.empty());
    }
}

void EditorShell::UpdateFormRects(EditorPanelId panel)
{
    FormPanel& form = Form(panel);
    if (!form.listContent.IsValid())
    {
        return;
    }
    float width = 0.0f;
    float height = 0.0f;
    for (const GroupView& group : groups_)
    {
        if (std::find(group.tabs.begin(), group.tabs.end(), panel) != group.tabs.end())
        {
            width = group.rect.width;
            height = std::max(0.0f, group.rect.height - kTabHeight);
            break;
        }
    }
    UiStyle listStyle = document_.Style(form.listContent);
    listStyle.left = UiLength::Px(0.0f);
    const float previewHeight=form.previewImage.empty()?0.f:std::clamp(height-120.f,0.f,144.f);
    if(form.preview.IsValid())
    {
        const float imageWidth=std::max(1.f,std::min(width-8,std::max(0.f,previewHeight-4)*form.previewAspect));
        auto imageStyle=AbsoluteStyle(std::max(4.f,(width-imageWidth)/2),32,imageWidth,imageWidth/form.previewAspect);imageStyle.image=form.previewImage;document_.SetStyle(form.preview,imageStyle);
        document_.SetVisible(form.preview,previewHeight>0);
    }
    listStyle.top = UiLength::Px(32.0f+previewHeight);
    listStyle.width = UiLength::Percent(100.0f);
    listStyle.height = UiLength::Px(std::max(0.0f, height - 36.0f-previewHeight));
    document_.SetStyle(form.listContent, listStyle);
    if (form.list)
    {
        form.list->SetViewportHeight(std::max(0.0f, height - 36.0f-previewHeight));
        form.list->Sync();
    }
    if (form.statusLabel.IsValid())
    {
        UiStyle style = document_.Style(form.statusLabel);
        style.left = UiLength::Px(form.actionLabel.empty() ? 92.0f : 178.0f);
        style.width = UiLength::Px(std::max(0.0f, width - (form.actionLabel.empty() ? 98.0f : 184.0f)));
        document_.SetStyle(form.statusLabel, style);
    }
}

void EditorShell::SetFormProperties(EditorPanelId panel, std::vector<EditorProperty> properties)
{
    FormPanel& form = Form(panel);
    bool changed = properties.size() != form.properties.size();
    if (!changed)
    {
        for (std::size_t index = 0; index < properties.size(); ++index)
        {
            if (properties[index].key != form.properties[index].key ||
                properties[index].value != form.properties[index].value ||
                properties[index].label != form.properties[index].label)
            {
                changed = true;
                break;
            }
        }
    }
    if (!changed)
    {
        return;
    }
    form.properties = std::move(properties);
    RefreshForm(form);
}

void EditorShell::SetFormHandlers(EditorPanelId panel, std::function<bool(std::string, std::string)> commit,
                                  std::function<bool()> save)
{
    FormPanel& form = Form(panel);
    form.commit = std::move(commit);
    form.save = std::move(save);
}

void EditorShell::SetFormPreview(EditorPanelId panel,std::string image,float aspect)
{
    auto& form=Form(panel);if(!form.preview.IsValid()){form.preview=document_.CreateElement(UiElementKind::Image);document_.SetParent(form.preview,Slot(panel).content);}
    form.previewImage=std::move(image);form.previewAspect=std::max(.01f,aspect);UpdateFormRects(panel);
}
void EditorShell::SetFormPrimaryLabel(EditorPanelId panel,std::string label){document_.SetText(Form(panel).saveButton,std::move(label));}
void EditorShell::SetFormManagedStatus(EditorPanelId panel,bool managed){Form(panel).managedStatus=managed;}

void EditorShell::SetFormAction(EditorPanelId panel, std::string label, std::function<bool()> action)
{
    FormPanel& form = Form(panel);
    form.actionLabel = std::move(label);
    form.action = std::move(action);
    RefreshForm(form);
}

void EditorShell::SetFormActionLabel(EditorPanelId panel, std::string label)
{
    FormPanel& form = Form(panel);
    if (form.actionLabel == label)
    {
        return;
    }
    form.actionLabel = std::move(label);
    RefreshForm(form);
}

void EditorShell::SetFormStatus(EditorPanelId panel, std::string status)
{
    FormPanel& form = Form(panel);
    if (form.status == status)
    {
        return;
    }
    form.status = std::move(status);
    RefreshForm(form);
}

UiElementHandle EditorShell::FormRow(EditorPanelId panel, std::size_t index) const noexcept
{
    const FormPanel& form = Form(panel);
    if (!form.list || index >= form.list->Rows().size())
    {
        return {};
    }
    return form.list->Rows()[index];
}

UiElementHandle EditorShell::FormSaveButton(EditorPanelId panel) const noexcept
{
    return Form(panel).saveButton;
}

UiElementHandle EditorShell::FormActionButton(EditorPanelId panel) const noexcept
{
    return Form(panel).actionButton;
}

UiElementHandle EditorShell::FormListContent(EditorPanelId panel) const noexcept
{
    return Form(panel).listContent;
}

UiElementHandle EditorShell::FormComboPopup(EditorPanelId panel) const noexcept
{
    return Form(panel).comboPopup;
}

} // namespace flare::ui::editor
