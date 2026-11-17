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

namespace se::ui::editor
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
    form.list->SetRowTextLayout(UiTextFlow::SingleLineEllipsis, {6, 0, 6, 0});
    form.list->SetRowHeight(20.0f);
    form.list->SetRowTextProvider([&form](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= form.properties.size())
        {
            return std::string{};
        }
        const EditorProperty& property = form.properties[static_cast<std::size_t>(index)];
        const std::string suffix = property.type == EditorPropertyType::Combo ? " [v]" : "";
        std::string display=property.displayValue.empty()?property.value:property.displayValue;
        for(std::size_t i=0;i<property.options.size()&&i<property.optionLabels.size();++i)
            if(property.options[i]==property.value)display=property.optionLabels[i];
        if(property.type==EditorPropertyType::Action)return property.label;
        return property.label + ": " + display + suffix;
    });
    dispatcher_.AddHandler(form.listContent, UiEventType::PointerWheel, [this, &form](UiInputEvent& event) {
        if (form.list)
        {
            form.list->SetScrollOffset(form.list->Offset() + event.wheel * 48.0f);
            form.list->Sync();
        }
        event.handled = true;
    }, UiEventPhase::Bubble);
    const EditorPanelId panelId = slot.id;
    dispatcher_.AddHandler(form.listContent, UiEventType::PointerDown, [this, &form, panelId](UiInputEvent& event) {
        const UiRect rect = layout_.Rect(form.listContent);
        const float offset = form.list ? form.list->Offset() : 0.0f;
        const int row = static_cast<int>((event.y - rect.y + offset) / 20.0f);
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
            form.edit->SetText(form.properties[static_cast<std::size_t>(row)].value);
            document_.SetVisible(form.editElement, true);
            dispatcher_.SetFocus(form.editElement);
        }
        event.handled = true;
    }, UiEventPhase::Bubble);

    form.saveButton = document_.CreateElement(UiElementKind::Button);
    if (document_.SetParent(form.saveButton, slot.content))
    {
        document_.SetStyle(form.saveButton, ButtonStyle(0.0f, 0.0f, 80.0f, 24.0f, kSurfaceActive));
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
        document_.SetStyle(form.actionButton, ButtonStyle(84.0f, 0.0f, 80.0f, 24.0f, kSurfaceRaised));
        document_.SetText(form.actionButton, "Action");
        document_.SetVisible(form.actionButton, false);
        dispatcher_.AddHandler(form.actionButton, UiEventType::PointerDown, [this, &form](UiInputEvent& event) {
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
        UiStyle statusStyle = ButtonStyle(168.0f, 0.0f, 100.0f, 24.0f, kContent);
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
        form.edit = std::make_unique<UiTextEdit>(document_, form.editElement, text_);
        dispatcher_.AddHandler(form.editElement, UiEventType::PointerDown, [this, &form](UiInputEvent&) {
            dispatcher_.SetFocus(form.editElement);
        });
        dispatcher_.AddHandler(form.editElement, UiEventType::KeyDown, [this, &form](UiInputEvent& event) {
            if (event.key == UiKeyEnter)
            {
                bool committed = false;
                if (form.commit && form.edit && !form.editingKey.empty())
                {
                    committed = form.commit(form.editingKey, std::string(form.edit->Text()));
                }
                if (committed)
                {
                    for (EditorProperty& property : form.properties)
                    {
                        if (property.key == form.editingKey)
                        {
                            property.value = form.edit ? std::string(form.edit->Text()) : std::string{};
                        }
                    }
                    form.status = Text("EditorUi.Applied", "Applied");
                }
                else
                {
                    form.status = Text("EditorUi.InvalidValue", "Invalid value");
                }
                form.editingKey.clear();
                document_.SetVisible(form.editElement, false);
                dispatcher_.ClearFocus();
                RefreshForm(form);
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
    const float popupWidth = std::max(80.0f, std::min(200.0f, panelRect.width - 8.0f));
    UiStyle popup = document_.Style(form.comboPopup);
    popup.left = UiLength::Px(4.0f);
    popup.top = UiLength::Px(std::max(0.0f, rowTop + form.list->RowHeight() - panelRect.y));
    popup.width = UiLength::Px(popupWidth);
    popup.height = UiLength::Px(static_cast<float>(property.options.size()) * 22.0f);
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
    listStyle.height = UiLength::Px(std::max(0.0f, height - 56.0f-previewHeight));
    document_.SetStyle(form.listContent, listStyle);
    if (form.list)
    {
        form.list->SetViewportHeight(std::max(0.0f, height - 56.0f-previewHeight));
        form.list->Sync();
    }
    if (form.editElement.IsValid())
    {
        UiStyle style = document_.Style(form.editElement);
        style.top = UiLength::Px(std::max(0.0f, height - 24.0f));
        style.width = UiLength::Percent(100.0f);
        document_.SetStyle(form.editElement, style);
    }
    if (form.statusLabel.IsValid())
    {
        UiStyle style = document_.Style(form.statusLabel);
        style.width = UiLength::Px(std::max(0.0f, width - 168.0f));
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

} // namespace se::ui::editor
