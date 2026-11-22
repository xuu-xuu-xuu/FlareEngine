#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/EditorUi/EditorShellInternal.h"
#include "ToolLayer/Editor/EditorUi/EditorDockState.h"
#include "ToolLayer/Editor/EditorUi/EditorInk.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Utf.h"
#include "ToolLayer/Editor/FlarePalette.h"
#include <Windows.h>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace flare::ui::editor
{
using namespace detail;

bool EditorShell::RequestClose()
{
    if (closeApproved_)
    {
        return false;
    }
    if (closeModalOpen_)
    {
        return true;
    }
    if (!dirty_)
    {
        closeApproved_ = true;
        return false;
    }
    ShowCloseModal();
    return true;
}

bool EditorShell::CloseApproved() const noexcept
{
    return closeApproved_;
}

bool EditorShell::CloseModalOpen() const noexcept
{
    return closeModalOpen_;
}

UiElementHandle EditorShell::CloseModalDialog() const noexcept
{
    return modalDialog_;
}

bool EditorShell::ConsumeCloseRequest() noexcept
{
    const bool requested = closeRequested_;
    closeRequested_ = false;
    return requested;
}

void EditorShell::ShowCloseModal()
{
    if (closeModalOpen_ || !root_.IsValid())
    {
        return;
    }
    modalBackdrop_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(modalBackdrop_, root_))
    {
        return;
    }
    UiStyle backdrop = AbsoluteStyle(0.0f, 0.0f, width_, height_);
    backdrop.width = UiLength::Percent(100.0f);
    backdrop.height = UiLength::Percent(100.0f);
    backdrop.background = UiColor{0.0f, 0.0f, 0.0f, 0.55f};
    document_.SetStyle(modalBackdrop_, backdrop);
    dispatcher_.AddHandler(modalBackdrop_, UiEventType::PointerDown, [](UiInputEvent& event) {
        event.handled = true;
        event.propagationStopped = true;
    });

    const float dialogWidth = 440.0f;
    const float dialogHeight = 196.0f;
    const float left = std::max(20.0f, (width_ - dialogWidth) * 0.5f);
    const float top = std::max(60.0f, (height_ - dialogHeight) * 0.5f);
    modalDialog_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(modalDialog_, modalBackdrop_))
    {
        return;
    }
    UiStyle dialog = AbsoluteStyle(left, top, dialogWidth, dialogHeight);
    dialog.background = UiInk(flare::palette::workspace::Panel);
    dialog.borderWidth = 1.0f;
    dialog.borderColor = UiInk(flare::palette::workspace::Focus, 0.45f);
    dialog.cornerRadius = 10.0f;
    document_.SetStyle(modalDialog_, dialog);

    const UiElementHandle title = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(title, modalDialog_))
    {
        document_.SetStyle(title, ButtonStyle(20.0f, 20.0f, dialogWidth - 40.0f, 24.0f, kContent));
        document_.SetText(title, Text("EditorUi.CloseUnsavedTitle", "Unsaved changes"));
    }
    const UiElementHandle message = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(message, modalDialog_))
    {
        UiStyle style = ButtonStyle(20.0f, 56.0f, dialogWidth - 40.0f, 48.0f, kContent);
        style.textFlow = UiTextFlow::Wrap;
        style.padding = {};
        style.color = kMuted;
        document_.SetStyle(message, style);
        document_.SetText(message, Text("EditorUi.CloseUnsavedMessage",
                                        "The current document has unsaved changes. Save before closing?"));
    }

    const auto makeButton = [this, dialogWidth](float x, const char* label, const UiColor& color) {
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, modalDialog_))
        {
            return UiElementHandle{};
        }
        auto style = ButtonStyle(x, 144.0f, 120.0f, 30.0f, color);
        style.cornerRadius = 6.0f;
        document_.SetStyle(button, style);
        document_.SetText(button, label);
        return button;
    };
    modalSave_ = makeButton(20.0f, Text("EditorUi.Save", "Save").c_str(), UiInk(flare::palette::Accent));
    if (modalSave_.IsValid()) { auto style = document_.Style(modalSave_); style.color = kBackground; document_.SetStyle(modalSave_, style); }
    modalDiscard_ = makeButton(154.0f, Text("EditorUi.DiscardChanges", "Discard").c_str(),
                               UiInk(flare::palette::Danger));
    modalCancel_ = makeButton(288.0f, Text("EditorUi.Cancel", "Cancel").c_str(), kSurfaceRaised);
    (void)dialogWidth;

    if (modalSave_.IsValid())
    {
        dispatcher_.AddHandler(modalSave_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            if (commands_.Execute(EditorCommand{EditorCommandId::Save, {}}))
            {
                closeApproved_ = true;
                closeRequested_ = true;
                HideCloseModal();
            }
            else
            {
                SetStatus(Text("EditorUi.SaveFailed", "Save failed") +
                          Text("EditorUi.CloseCancelled", "; close cancelled"));
            }
            event.handled = true;
        });
    }
    if (modalDiscard_.IsValid())
    {
        dispatcher_.AddHandler(modalDiscard_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            closeApproved_ = true;
            closeRequested_ = true;
            HideCloseModal();
            event.handled = true;
        });
    }
    if (modalCancel_.IsValid())
    {
        dispatcher_.AddHandler(modalCancel_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            HideCloseModal();
            event.handled = true;
        });
        dispatcher_.SetFocus(modalCancel_);
    }
    closeModalOpen_ = true;
}

void EditorShell::ShowConfirm(std::string title, std::string message, std::string primaryLabel,
    std::string secondaryLabel, std::string cancelLabel, std::function<void()> onPrimary,
    std::function<void()> onSecondary, bool primaryEnabled)
{
    if (confirmOpen_ || !root_.IsValid())
    {
        return;
    }
    confirmPrimaryAction_ = std::move(onPrimary);
    confirmSecondaryAction_ = std::move(onSecondary);
    confirmBackdrop_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(confirmBackdrop_, root_))
    {
        return;
    }
    UiStyle backdrop = AbsoluteStyle(0.0f, 0.0f, width_, height_);
    backdrop.width = UiLength::Percent(100.0f);
    backdrop.height = UiLength::Percent(100.0f);
    backdrop.background = UiColor{0.0f, 0.0f, 0.0f, 0.55f};
    document_.SetStyle(confirmBackdrop_, backdrop);
    dispatcher_.AddHandler(confirmBackdrop_, UiEventType::PointerDown, [](UiInputEvent& event) {
        event.handled = true;
        event.propagationStopped = true;
    });

    const float dialogWidth = 440.0f;
    const float dialogHeight = 196.0f;
    const float left = std::max(20.0f, (width_ - dialogWidth) * 0.5f);
    const float top = std::max(60.0f, (height_ - dialogHeight) * 0.5f);
    confirmDialog_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(confirmDialog_, confirmBackdrop_))
    {
        return;
    }
    UiStyle dialog = AbsoluteStyle(left, top, dialogWidth, dialogHeight);
    dialog.background = UiInk(flare::palette::workspace::Panel);
    dialog.borderWidth = 1.0f;
    dialog.borderColor = UiInk(flare::palette::workspace::Focus, 0.45f);
    dialog.cornerRadius = 10.0f;
    document_.SetStyle(confirmDialog_, dialog);

    const UiElementHandle titleLabel = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(titleLabel, confirmDialog_))
    {
        document_.SetStyle(titleLabel, ButtonStyle(20.0f, 20.0f, dialogWidth - 40.0f, 24.0f, kContent));
        document_.SetText(titleLabel, title);
    }
    const UiElementHandle messageLabel = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(messageLabel, confirmDialog_))
    {
        UiStyle style = ButtonStyle(20.0f, 56.0f, dialogWidth - 40.0f, 48.0f, kContent);
        style.textFlow = UiTextFlow::Wrap;
        style.padding = {};
        style.color = kMuted;
        document_.SetStyle(messageLabel, style);
        document_.SetText(messageLabel, message);
    }

    const auto makeButton = [this, dialogWidth](float x, const std::string& label, const UiColor& color) {
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, confirmDialog_))
        {
            return UiElementHandle{};
        }
        auto style = ButtonStyle(x, 144.0f, 120.0f, 30.0f, color);
        style.cornerRadius = 6.0f;
        document_.SetStyle(button, style);
        document_.SetText(button, label);
        return button;
    };
    confirmPrimary_ = makeButton(20.0f, primaryLabel, primaryEnabled ? UiInk(flare::palette::Accent) : kSurfaceRaised);
    if (primaryEnabled && confirmPrimary_.IsValid()) { auto style = document_.Style(confirmPrimary_); style.color = kBackground; document_.SetStyle(confirmPrimary_, style); }
    if (!primaryEnabled && confirmPrimary_.IsValid())
    {
        UiStyle disabledStyle = document_.Style(confirmPrimary_);
        disabledStyle.color = kMuted;
        document_.SetStyle(confirmPrimary_, disabledStyle);
    }
    confirmSecondary_ = makeButton(154.0f, secondaryLabel, UiInk(flare::palette::Danger));
    confirmCancel_ = makeButton(288.0f, cancelLabel, kSurfaceRaised);
    (void)dialogWidth;
    if (secondaryLabel.empty())
    {
        document_.SetVisible(confirmSecondary_, false);
    }

    if (confirmPrimary_.IsValid() && primaryEnabled)
    {
        dispatcher_.AddHandler(confirmPrimary_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            auto action = confirmPrimaryAction_;
            HideConfirm();
            if (action) action();
            event.handled = true;
        });
    }
    if (confirmSecondary_.IsValid() && !secondaryLabel.empty())
    {
        dispatcher_.AddHandler(confirmSecondary_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            auto action = confirmSecondaryAction_;
            HideConfirm();
            if (action) action();
            event.handled = true;
        });
    }
    if (confirmCancel_.IsValid())
    {
        dispatcher_.AddHandler(confirmCancel_, UiEventType::PointerDown, [this](UiInputEvent& event) {
            HideConfirm();
            event.handled = true;
        });
        dispatcher_.SetFocus(confirmCancel_);
    }
    confirmOpen_ = true;
}

void EditorShell::HideConfirm()
{
    if (confirmBackdrop_.IsValid())
    {
        document_.SetVisible(confirmBackdrop_, false);
        pendingDestroy_.push_back(confirmBackdrop_);
    }
    confirmBackdrop_ = {};
    confirmDialog_ = {};
    confirmPrimary_ = {};
    confirmSecondary_ = {};
    confirmCancel_ = {};
    confirmPrimaryAction_ = {};
    confirmSecondaryAction_ = {};
    confirmOpen_ = false;
    dispatcher_.ClearFocus();
}

void EditorShell::HideCloseModal()
{
    if (modalBackdrop_.IsValid())
    {
        document_.SetVisible(modalBackdrop_, false);
        pendingDestroy_.push_back(modalBackdrop_);
    }
    modalBackdrop_ = {};
    modalDialog_ = {};
    modalSave_ = {};
    modalDiscard_ = {};
    modalCancel_ = {};
    closeModalOpen_ = false;
    dispatcher_.ClearFocus();
}

void EditorShell::SetActivePanel(EditorPanelId panel)
{
    if (activePanel_ == panel) return;
    activePanel_ = panel;
    if (hierarchyList_) hierarchyList_->Invalidate();
    if (assetsList_) assetsList_->Invalidate();
}

void EditorShell::FocusClickedContent(UiElementHandle target)
{
    while (const UiElement* element = document_.Element(target))
    {
        if (target == viewportTools_ || target == transformTools_ || target == gizmo_ ||
            std::find(gizmoAxes_.begin(), gizmoAxes_.end(), target) != gizmoAxes_.end())
        {
            SetActivePanel(EditorPanelId::Viewport);
            return;
        }
        for (const PanelSlot& slot : slots_)
        {
            if (target == slot.panel)
            {
                SetActivePanel(slot.id);
                return;
            }
        }
        target = element->Parent();
    }
}

void EditorShell::SelectPanel(EditorPanelId panel, bool save)
{
    SetActivePanel(panel);
    dock_.SelectTab(panel);
    if (initialized_)
    {
        UpdateDockRects();
        UpdatePanelContentRects();
    }
    if (save)
    {
        SaveLayoutIfConfigured();
    }
}

void EditorShell::TogglePanel(EditorPanelId panel)
{
    if (panel == EditorPanelId::None)
    {
        return;
    }
    gameMaximized_ = false;
    if (dock_.Find(panel) != nullptr)
    {
        const DockNode& group = *dock_.Find(panel);
        const auto neighbour = std::find_if(group.tabs.begin(), group.tabs.end(),
            [panel](EditorPanelId other) { return other != panel; });
        const auto next = neighbour != group.tabs.end() ? *neighbour : EditorPanelId::Viewport;
        dock_.CloseTab(panel);
        if (activePanel_ == panel)
        {
            const auto* selected = dock_.Find(next);
            SetActivePanel(selected ? selected->active : EditorPanelId::None);
        }
    }
    else
    {
        EditorPanelId nearPanel = Slot(panel).openNear;
        if (panel == EditorPanelId::Preferences || panel == EditorPanelId::About ||
            panel == EditorPanelId::Diagnostics)
        {
            nearPanel = EditorPanelId::Viewport;
        }
        dock_.OpenTabNear(panel, nearPanel);
        SetActivePanel(panel);
    }
    MarkDockDirty(true, true);
}

void EditorShell::FocusPanel(EditorPanelId panel)
{
    if (panel == EditorPanelId::None)
    {
        return;
    }
    bool dockChanged = false;
    if (gameMaximized_ && panel != EditorPanelId::Game)
    {
        gameMaximized_ = false;
        dockChanged = true;
    }
    if (dock_.Find(panel) == nullptr)
    {
        EditorPanelId nearPanel = Slot(panel).openNear;
        if (panel == EditorPanelId::Preferences || panel == EditorPanelId::About ||
            panel == EditorPanelId::Diagnostics)
        {
            nearPanel = EditorPanelId::Viewport;
        }
        dockChanged = dock_.OpenTabNear(panel, nearPanel) || dockChanged;
    }
    SelectPanel(panel, true);
    if (dockChanged)
    {
        MarkDockDirty(true, false);
    }
}

bool EditorShell::TextFieldFocused() const noexcept
{
    const UiElement* element = document_.Element(dispatcher_.Focus());
    return element != nullptr && element->Kind() == UiElementKind::TextEdit;
}

} // namespace flare::ui::editor
