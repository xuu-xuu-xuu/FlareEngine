#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/EditorUi/EditorShellInternal.h"
#include "ToolLayer/Editor/EditorUi/EditorInk.h"
#include "ToolLayer/Editor/FlarePalette.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace se::ui::editor
{
using namespace detail;

void EditorShell::RegisterDefaultShortcuts()
{
    router_.RegisterShortcut('S', true, false, false, EditorCommandId::Save);
    router_.RegisterShortcut('S', true, true, true, EditorCommandId::SaveAll);
    router_.RegisterShortcut('Z', true, false, false, EditorCommandId::Undo);
    router_.RegisterShortcut('Z', true, true, false, EditorCommandId::Redo);
    router_.RegisterShortcut('Y', true, false, false, EditorCommandId::Redo);
    router_.RegisterShortcut(VK_F5, false, false, false, EditorCommandId::Play);
    router_.RegisterShortcut(VK_F6, false, false, false, EditorCommandId::Pause);
    router_.RegisterShortcut(VK_F7, false, false, false, EditorCommandId::Step);
    router_.RegisterShortcut(VK_F8, false, false, false, EditorCommandId::Stop);
    router_.RegisterShortcut(VK_DELETE, false, false, false, EditorCommandId::Delete);
    router_.RegisterShortcut('D', true, false, false, EditorCommandId::Duplicate);
    router_.RegisterShortcut(VK_F2, false, false, false, EditorCommandId::Rename);
    router_.RegisterShortcut('I', true, false, false, EditorCommandId::Import);
    router_.RegisterShortcut('E', true, false, false, EditorCommandId::Export);
    router_.RegisterShortcut('B', true, false, false, EditorCommandId::Build);
    router_.RegisterShortcut('O', true, false, false, EditorCommandId::Open);
    router_.RegisterShortcut('N', true, false, false, EditorCommandId::NewScene);
    router_.RegisterShortcut('S', true, true, false, EditorCommandId::SaveAs);
    router_.RegisterShortcut('B', true, true, false, EditorCommandId::TogglePanel, "build-export");
    router_.RegisterShortcut(VK_OEM_COMMA, true, false, false, EditorCommandId::OpenSettings);
    router_.RegisterShortcut('P', true, true, false, EditorCommandId::TogglePanel, "preferences");
    router_.RegisterShortcut(VK_F11, false, false, false, EditorCommandId::FocusViewport);
    router_.RegisterShortcut(VK_F1, false, false, false, EditorCommandId::TogglePanel, "about");
}

void EditorShell::BuildChromeMenus()
{
    const std::string file = Text("EditorUi.File", "File");
    const std::string edit = Text("EditorUi.Edit", "Edit");
    const std::string play = Text("EditorUi.Play", "Play");
    const std::string project = Text("EditorUi.Project", "Project");
    const std::string panels = Text("EditorUi.Panels", "Panels");
    chrome_.AddMenu(file, Text("EditorUi.Open", "Open"), EditorCommandId::Open);
    chrome_.AddMenu(file, Text("EditorUi.NewScene", "New Scene"), EditorCommandId::NewScene);
    chrome_.AddMenu(file, Text("EditorUi.Save", "Save"), EditorCommandId::Save);
    chrome_.AddMenu(file, Text("EditorUi.SaveSceneAs", "Save As"), EditorCommandId::SaveAs);
    chrome_.AddMenu(file, Text("EditorUi.SaveAll", "Save All"), EditorCommandId::SaveAll);
    chrome_.AddMenu(edit, Text("EditorUi.Undo", "Undo"), EditorCommandId::Undo);
    chrome_.AddMenu(edit, Text("EditorUi.Redo", "Redo"), EditorCommandId::Redo);
    chrome_.AddMenu(edit, Text("EditorUi.Preferences", "Preferences"), EditorCommandId::TogglePanel, "preferences");
    chrome_.AddMenu(play, Text("EditorUi.Play", "Play / Stop"), EditorCommandId::Play);
    chrome_.AddMenu(play, Text("EditorUi.Pause", "Pause / Resume"), EditorCommandId::Pause);
    chrome_.AddMenu(play, Text("EditorUi.StepPlay", "Step"), EditorCommandId::Step);
    chrome_.AddMenu(play, Text("EditorUi.Stop", "Stop"), EditorCommandId::Stop);
    chrome_.AddMenu(project, Text("EditorUi.ProjectSettings", "Project Settings"), EditorCommandId::OpenSettings);
    chrome_.AddMenu(project, Text("EditorUi.BuildExport", "Build / Export Panel"), EditorCommandId::TogglePanel,
                    "build-export");
    chrome_.AddMenu(project, Text("EditorUi.Export", "Export"), EditorCommandId::Export);
    chrome_.AddMenu(project, Text("EditorUi.BuildRestart", "Build / Restart"), EditorCommandId::Build);
    const EditorPanelId panelIds[] = {EditorPanelId::Hierarchy, EditorPanelId::Assets, EditorPanelId::Viewport,
                                      EditorPanelId::UiDesigner, EditorPanelId::Inspector, EditorPanelId::Console,
                                      EditorPanelId::Material, EditorPanelId::ProjectSettings,
                                      EditorPanelId::BuildExport, EditorPanelId::AssetTools};
    for (const EditorPanelId panel : panelIds)
    {
        chrome_.AddMenu(panels, PanelLabel(panel), EditorCommandId::TogglePanel, EditorPanelName(panel));
    }
    chrome_.AddMenu(panels, Text("EditorUi.FocusViewport", "Focus Viewport"), EditorCommandId::FocusViewport);
    const std::string help = Text("EditorUi.Help", "Help");
    chrome_.AddMenu(help, Text("EditorUi.About", "About"), EditorCommandId::TogglePanel, "about");
    chrome_.AddMenu(help, Text("EditorUi.Diagnostics", "Diagnostics"), EditorCommandId::TogglePanel, "diagnostics");

    chrome_.AddToolbar(Text("EditorUi.Play", "Play"), EditorCommandId::Play);
    chrome_.AddToolbar(Text("EditorUi.Pause", "Pause"), EditorCommandId::Pause);
    chrome_.AddToolbar(Text("EditorUi.StepPlay", "Step"), EditorCommandId::Step);
}

void EditorShell::BuildChrome()
{
    menuBar_ = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(menuBar_, root_))
    {
        UiStyle style = AbsoluteStyle(0.0f, 0.0f, width_, menuHeight_);
        style.width = UiLength::Percent(100.0f);
        style.background = kSurfaceRaised;
        style.borderColor = kBorder;
        style.borderWidth = 1.0f;
        document_.SetStyle(menuBar_, style);
    }
    toolbar_ = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(toolbar_, root_))
    {
        UiStyle style = AbsoluteStyle(0.0f, menuHeight_, width_, toolbarHeight_);
        style.width = UiLength::Percent(100.0f);
        style.background = kSurface;
        style.borderColor = kBorder;
        style.borderWidth = 1.0f;
        document_.SetStyle(toolbar_, style);
    }
    statusBar_ = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(statusBar_, root_))
    {
        UiStyle style = AbsoluteStyle(0.0f, height_ - statusHeight_, width_, statusHeight_);
        style.width = UiLength::Percent(100.0f);
        style.background = kSurfaceRaised;
        style.color = kMuted;
        style.fontSize = 12.0f;
        style.textFlow = UiTextFlow::SingleLineEllipsis;
        style.padding = {8, 0, 8, 0};
        style.borderColor = kBorder;
        style.borderWidth = 1.0f;
        document_.SetStyle(statusBar_, style);
    }

    const auto& menus = chrome_.Menus();
    menuNames_.clear();
    menuFirstItem_.clear();
    for (std::size_t index = 0; index < menus.size(); ++index)
    {
        if (menuNames_.empty() || menuNames_.back() != menus[index].menu)
        {
            menuNames_.push_back(menus[index].menu);
            menuFirstItem_.push_back(index);
        }
    }
    float menuX = 6.0f;
    for (const std::string& name : menuNames_)
    {
        const float width = MenuWidth(name);
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, menuBar_))
        {
            continue;
        }
        document_.SetStyle(button, ButtonStyle(menuX, 0.0f, width, menuHeight_, kSurfaceRaised));
        document_.SetText(button, name);
        dispatcher_.AddHandler(button, UiEventType::PointerDown, [this, name](UiInputEvent& event) {
            if (openMenu_ == name) CloseMenu();
            else OpenMenu(name);
            event.handled = true;
        });
        menuButtons_.push_back(button);
        menuX += width + 2.0f;
    }

    for (std::size_t index = 0; index < chrome_.Toolbar().size(); ++index)
    {
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, toolbar_))
        {
            continue;
        }
        dispatcher_.AddHandler(button, UiEventType::PointerDown, [this, button](UiInputEvent& event) {
            if (event.button != 0) return;
            pressedTransport_ = button;
            event.handled = true;
        });
        dispatcher_.AddHandler(button, UiEventType::PointerUp, [this, index, button](UiInputEvent& event) {
            if (event.button != 0 || pressedTransport_ != button) return;
            pressedTransport_ = {};
            const auto bounds = layout_.Rect(button);
            const bool inside = event.x >= bounds.x && event.x < bounds.x + bounds.width &&
                                event.y >= bounds.y && event.y < bounds.y + bounds.height;
            if (inside && !chrome_.ActivateToolbar(index))
            {
                SetStatus(Text("EditorUi.CommandUnavailable", "Command unavailable") + ": " +
                          chrome_.Toolbar()[index].label);
            }
            event.handled = true;
        });
        dispatcher_.AddHandler(button, UiEventType::PointerCaptureLost, [this](UiInputEvent&) {
            pressedTransport_ = {};
        });
        dispatcher_.AddHandler(button, UiEventType::KeyDown, [this, index](UiInputEvent& event) {
            if (event.key == UiKeyEnter || event.key == UiKeySpace)
            {
                chrome_.ActivateToolbar(index);
                event.handled = true;
            }
        });
        toolbarButtons_.push_back(button);
    }

    viewportTools_ = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(viewportTools_, root_);
    document_.SetStyle(viewportTools_, AbsoluteStyle(0, 0, 1, 27));
    transformTools_ = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(transformTools_, root_);
    const char* actions[] = {"Q", "W", "E", "R", "X", "Z", "Sky", "Grid", "Gizmos", "FrameAll"};
    for (const char* action : actions)
    {
        const auto button = document_.CreateElement(UiElementKind::Button);
        document_.SetParent(button, viewportToolButtons_.size() < 4 ? transformTools_ : viewportTools_);
        dispatcher_.AddHandler(button, UiEventType::PointerDown, [this, action](UiInputEvent& e) {
            if (e.button == 0 && viewportAction_) viewportAction_(action);
            e.handled = true;
            e.StopPropagation();
        });
        viewportToolButtons_.push_back(button);
    }

    gizmo_ = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(gizmo_, root_))
    {
        UiStyle gizmoStyle = AbsoluteStyle(0.0f, 0.0f, 12.0f, 12.0f);
        gizmoStyle.borderWidth = 2.0f;
        gizmoStyle.borderColor = kAccent;
        document_.SetStyle(gizmo_, gizmoStyle);
        document_.SetVisible(gizmo_, false);
    }
    const UiColor axisColors[] = {{0.95f, 0.25f, 0.25f, 1.0f}, {0.25f, 0.85f, 0.35f, 1.0f},
                                  {0.30f, 0.55f, 1.0f, 1.0f}};
    for (int axis = 0; axis < 3; ++axis)
    {
        const UiElementHandle handle = document_.CreateElement(UiElementKind::Panel);
        if (!document_.SetParent(handle, root_))
        {
            continue;
        }
        UiStyle axisStyle = AbsoluteStyle(0.0f, 0.0f, 8.0f, 8.0f);
        axisStyle.background = axisColors[axis];
        document_.SetStyle(handle, axisStyle);
        document_.SetVisible(handle, false);
        dispatcher_.AddHandler(handle, UiEventType::PointerDown, [this, axis](UiInputEvent& event) {
            gizmoDragAxis_ = axis + 1;
            gizmoLastX_ = event.x;
            gizmoLastY_ = event.y;
        });
        dispatcher_.AddHandler(handle, UiEventType::PointerMove, [this, axis](UiInputEvent& event) {
            if (gizmoDragAxis_ != axis + 1) return;
            if (gizmoDragHandler_) gizmoDragHandler_(axis, event.x - gizmoLastX_, event.y - gizmoLastY_);
            gizmoLastX_ = event.x;
            gizmoLastY_ = event.y;
        });
        dispatcher_.AddHandler(handle, UiEventType::PointerUp, [this, axis](UiInputEvent&) {
            if (gizmoDragAxis_ == axis + 1) gizmoDragAxis_ = 0;
        });
        gizmoAxes_.push_back(handle);
    }

    dragGhost_ = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(dragGhost_, root_))
    {
        UiStyle ghostStyle = ButtonStyle(0, 0, 120.0f, kTabHeight, UiInk(flare::workspace::Header, 0.97f));
        ghostStyle.borderWidth = 1.0f;
        ghostStyle.borderColor = UiInk(flare::workspace::Focus);
        ghostStyle.pointerEvents = UiPointerEvents::None;
        document_.SetStyle(dragGhost_, ghostStyle);
        document_.SetVisible(dragGhost_, false);
    }
    dropHighlight_ = document_.CreateElement(UiElementKind::Panel);
    if (document_.SetParent(dropHighlight_, root_))
    {
        UiStyle highlight = AbsoluteStyle(0.0f, 0.0f, 0.0f, 0.0f);
        highlight.background = UiInk(flare::workspace::Focus, 0.15f);
        highlight.borderWidth = 2.0f;
        highlight.borderColor = UiInk(flare::workspace::Focus);
        highlight.pointerEvents = UiPointerEvents::None;
        document_.SetStyle(dropHighlight_, highlight);
        document_.SetVisible(dropHighlight_, false);
    }
    tabInsertMarker_ = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(tabInsertMarker_, root_);
    UiStyle marker = AbsoluteStyle(0, 0, 2, kTabHeight);
    marker.background = UiInk(flare::workspace::Focus);
    marker.pointerEvents = UiPointerEvents::None;
    document_.SetStyle(tabInsertMarker_, marker);
    document_.SetVisible(tabInsertMarker_, false);

    BindRootInputHandlers();
    if (console_)
    {
        console_->SetItemCount(static_cast<int>(lines_.size()));
        console_->Sync();
    }
    RefreshStatus();
}

void EditorShell::BindRootInputHandlers()
{
    dispatcher_.AddHandler(document_.Root(), UiEventType::PointerDown, [this](UiInputEvent& event) {
        if (inspectorColor_)
        {
            const auto rect = layout_.Rect(inspectorColor_->panel);
            if (event.x < rect.x || event.y < rect.y || event.x >= rect.x + rect.width ||
                event.y >= rect.y + rect.height)
            {
                CloseInspectorColor(true);
                event.handled = true;
                event.StopPropagation();
            }
            return;
        }
        if (closeModalOpen_ || confirmOpen_)
        {
            event.handled = true;
            return;
        }
        if (event.button == 0 && !MenuOpen()) BeginSplitter(event.x, event.y);
        if (splitterActive_)
        {
            event.handled = true;
            event.StopPropagation();
            return;
        }
        FocusClickedContent(event.target);
        if (!openMenu_.empty())
        {
            bool insideDropdown = false;
            if (menuDropdown_.IsValid())
            {
                const UiRect dropdown = layout_.Rect(menuDropdown_);
                insideDropdown = dropdown.x <= event.x && event.x < dropdown.x + dropdown.width &&
                    dropdown.y <= event.y && event.y < dropdown.y + dropdown.height;
            }
            if (!insideDropdown && (!openMenu_.empty() && (openMenu_.front() == '@' || event.y >= menuHeight_ + toolbarHeight_)))
            {
                CloseMenu();
            }
        }
    }, UiEventPhase::Capture);

    dispatcher_.AddHandler(document_.Root(), UiEventType::PointerMove, [this](UiInputEvent& event) {
        hoveredChrome_ = event.target;
        if (inspectorColor_ && inspectorColor_->dragging)
        {
            MoveInspectorColor(event);
            event.handled = true;
            event.StopPropagation();
            return;
        }
        if (inspectorDrag_)
        {
            UpdateInspectorDrag(event);
            event.handled = true;
            event.StopPropagation();
            return;
        }
        if (closeModalOpen_)
        {
            event.handled = true;
            return;
        }
        if (splitterActive_)
        {
            DragSplitter(event.x, event.y);
            event.handled = true;
            event.StopPropagation();
        }
    }, UiEventPhase::Capture);

    dispatcher_.AddHandler(document_.Root(), UiEventType::PointerUp, [this](UiInputEvent& event) {
        if (inspectorColor_ && inspectorColor_->dragging && event.button == 0)
        {
            MoveInspectorColor(event);
            inspectorColor_->dragging = 0;
            event.handled = true;
            event.StopPropagation();
            return;
        }
        if (inspectorDrag_ && event.button == 0)
        {
            UpdateInspectorDrag(event);
            EndInspectorDrag(true);
            event.handled = true;
            event.StopPropagation();
            return;
        }
        if (closeModalOpen_)
        {
            event.handled = true;
            return;
        }
        if (splitterActive_ && event.button == 0)
        {
            DragSplitter(event.x, event.y);
            EndSplitter(true);
            event.handled = true;
            event.StopPropagation();
        }
    }, UiEventPhase::Capture);

    dispatcher_.AddHandler(document_.Root(), UiEventType::PointerCaptureLost, [this](UiInputEvent& event) {
        if (inspectorColor_) inspectorColor_->dragging = 0;
        if (inspectorDrag_ && event.target == SurfaceRoot(inspectorDrag_->surface)) EndInspectorDrag(false);
        if (splitterActive_ && event.target == SurfaceRoot(splitterSurface_)) EndSplitter(false);
    }, UiEventPhase::Capture);

    dispatcher_.AddHandler(document_.Root(), UiEventType::FocusLost, [this](UiInputEvent& event) {
        if (inspectorDrag_ && event.target == SurfaceRoot(inspectorDrag_->surface)) EndInspectorDrag(false);
    }, UiEventPhase::Capture);

    dispatcher_.AddHandler(document_.Root(), UiEventType::KeyDown, [this](UiInputEvent& event) {
        if (inspectorColor_ && event.key == UiKeyEscape)
        {
            CloseInspectorColor(false);
            event.handled = true;
            event.StopPropagation();
            return;
        }
        if (inspectorDrag_)
        {
            if (event.key == UiKeyEscape) EndInspectorDrag(false);
            event.handled = true;
            event.StopPropagation();
            return;
        }
        if (event.key == UiKeyEscape && dragTabPanel_ != EditorPanelId::None)
        {
            CancelTabDrag();
            event.handled = true;
            event.StopPropagation();
            return;
        }
        if (confirmOpen_)
        {
            if (event.key == UiKeyEscape)
            {
                HideConfirm();
                event.handled = true;
            }
            return;
        }
        if (!closeModalOpen_)
        {
            return;
        }
        if (event.key == UiKeyEscape)
        {
            HideCloseModal();
            event.handled = true;
        }
        else if (event.key == VK_F9)
        {
            // 无鼠标回退：F9 = 保存，F10 = 丢弃。按钮点击仍是主路径。
            if (commands_.Execute(EditorCommand{EditorCommandId::Save, {}}))
            {
                closeApproved_ = true;
                closeRequested_ = true;
                HideCloseModal();
            }
            event.handled = true;
        }
        else if (event.key == VK_F10)
        {
            closeApproved_ = true;
            closeRequested_ = true;
            HideCloseModal();
            event.handled = true;
        }
    }, UiEventPhase::Capture);
}

void EditorShell::UpdateTransport()
{
    for (auto item = document_.FirstChild(menuDropdown_); item.IsValid(); item = document_.NextSibling(item))
    {
        auto style = document_.Style(item);
        style.background = hoveredChrome_ == item ? UiInk(flare::workspace::Selection) : kSurface;
        document_.SetStyle(item, style);
    }
    constexpr float cellWidth = 35.0f;
    const float left = std::floor((width_ - cellWidth * toolbarButtons_.size()) * 0.5f);
    for (std::size_t i = 0; i < toolbarButtons_.size(); ++i)
    {
        const auto button = toolbarButtons_[i];
        const auto command = chrome_.Toolbar()[i].command;
        const bool play = command == EditorCommandId::Play;
        const bool pause = command == EditorCommandId::Pause;
        const bool enabled = play || (previewPlaying_ && (pause || previewPaused_));
        const bool active = (play && previewPlaying_) || (pause && previewPaused_);
        const char* icon = play ? (previewPlaying_ ? "■" : "▶") : (pause ? "Ⅱ" : "▸│");
        const auto base = active || pressedTransport_ == button ? flare::workspace::Pressed :
                          (hoveredChrome_ == button && enabled ? flare::workspace::Hover : flare::workspace::Header);
        auto style = ButtonStyle(left + cellWidth * i, 4, cellWidth - 1, toolbarHeight_ - 8, UiInk(base));
        style.color = enabled ? kText : kMuted;
        // 传输区使用固定图标格；字符在不同字体中的宽度由文本服务测量。
        style.padding.left = std::max(0.0f, (cellWidth - 1 - MeasuredWidth(icon, style.fontSize)) * 0.5f);
        style.padding.right = 0.0f;
        document_.SetStyle(button, style);
        document_.SetText(button, icon);
        document_.SetEnabled(button, enabled);
    }
    for (std::size_t i = 0; i < menuButtons_.size(); ++i)
    {
        const auto button = menuButtons_[i];
        auto style = document_.Style(button);
        style.background = hoveredChrome_ == button || openMenu_ == menuNames_[i]
            ? UiInk(flare::workspace::Hover) : kSurfaceRaised;
        document_.SetStyle(button, style);
    }
}

void EditorShell::SetViewportState(int tool, bool local, bool center, bool sky, bool grid, bool gizmos,
                                   bool playing, bool paused)
{
    previewPlaying_ = playing;
    previewPaused_ = playing && paused;
    const auto surface = SurfaceRoot(dock_.SurfaceFor(EditorPanelId::Viewport));
    const auto viewport = layout_.Rect(ViewportPanel());
    const float available = std::max(0.f, viewport.width - 10);
    const bool visible = !playing && document_.Visible(ViewportPanel());
    for (auto group : {viewportTools_, transformTools_})
    {
        document_.SetParent(group, surface.IsValid() ? surface : root_);
        document_.SetVisible(group, visible);
    }
    const std::string labels[] = {
        "✥", "↔", "⟳", "⤢",
        Text(local ? "EditorUi.LocalSpace" : "EditorUi.WorldSpace", local ? "Local" : "World"),
        Text(center ? "EditorUi.Center" : "EditorUi.Pivot", center ? "Center" : "Pivot"),
        Text("EditorUi.ViewportSky", "Sky"), Text("EditorUi.ViewportGrid", "Grid"),
        Text("EditorUi.ViewportGizmos", "Gizmos"), Text("EditorUi.FrameAll", "Frame All")};
    const bool active[] = {tool == 0, tool == 1, tool == 2, tool == 3, local, center, sky, grid, gizmos, false};
    float x = 0, y = 0, maxWidth = 0;
    for (std::size_t i = 0; i < viewportToolButtons_.size(); ++i)
    {
        const auto button = viewportToolButtons_[i];
        const auto fill = active[i] ? flare::workspace::Selection :
            (hoveredChrome_ == button ? flare::workspace::Hover : flare::workspace::Header);
        const float width = i < 4 ? 28.f : std::min(available, MeasuredWidth(labels[i], 13.f) + 16.f);
        if (i >= 4 && x > 0 && x + width > available) { x = 0; y += 24; }
        auto style = ButtonStyle(i < 4 ? 0 : x, i < 4 ? 24.0f * i : y, width, 23, UiInk(fill));
        if (i < 4)
        {
            style.padding.left = std::max(0.0f, (width - MeasuredWidth(labels[i], 13.f)) * 0.5f);
            style.padding.right = 0;
        }
        else
        {
            x += width + 1;
            maxWidth = std::max(maxWidth, x - 1);
        }
        document_.SetText(button, labels[i]);
        document_.SetStyle(button, style);
    }
    const float height = std::min(y + 23, std::max(0.f, viewport.height - 10));
    auto bar = AbsoluteStyle(viewport.x + 5, viewport.y + 5, maxWidth, height);
    bar.background = kBorder;
    bar.overflow = UiOverflow::Hidden;
    document_.SetStyle(viewportTools_, bar);
    auto tools = AbsoluteStyle(viewport.x + 5, viewport.y + 10 + height, std::min(28.f, available),
                               std::clamp(viewport.height - height - 15, 0.f, 95.f));
    tools.background = kBorder;
    tools.overflow = UiOverflow::Hidden;
    document_.SetStyle(transformTools_, tools);
}

void EditorShell::AppendConsoleLine(std::string line)
{
    lines_.push_back(std::move(line));
    if (console_)
    {
        console_->Invalidate();
        console_->SetItemCount(static_cast<int>(lines_.size()));
        console_->Sync();
    }
}

void EditorShell::SetStatus(std::string status)
{
    status_ = std::move(status);
    RefreshStatus();
}

void EditorShell::SetDirty(bool dirty)
{
    dirty_ = dirty;
    RefreshStatus();
}

void EditorShell::SetSelectionLabel(std::string label)
{
    selectionLabel_ = std::move(label);
    RefreshStatus();
}

void EditorShell::RefreshStatus()
{
    if (!statusBar_.IsValid()) return;
    std::string text = status_;
    for (std::size_t i = 0; i < toolbarButtons_.size(); ++i)
    {
        if (hoveredChrome_ != toolbarButtons_[i]) continue;
        const auto& item = chrome_.Toolbar()[i];
        text = item.command == EditorCommandId::Play && previewPlaying_ ? Text("EditorUi.Stop", "Stop") : item.label;
        break;
    }
    const char* toolKeys[] = {"EditorUi.ViewTool", "EditorUi.Translate", "EditorUi.Rotate", "EditorUi.ScaleTool"};
    const char* toolNames[] = {"View tool", "Translate", "Rotate", "Scale"};
    for (std::size_t i = 0; i < 4 && i < viewportToolButtons_.size(); ++i)
    {
        if (hoveredChrome_ == viewportToolButtons_[i])
            text = Text(toolKeys[i], toolNames[i]) + " (" + "QWER"[i] + ")";
    }
    if (dirty_) text += "   ·   " + Text("EditorUi.Unsaved", "Unsaved");
    if (!selectionLabel_.empty()) text += "   |   " + selectionLabel_;
    if (text == lastStatusText_) return;
    lastStatusText_ = text;
    document_.SetText(statusBar_, text);
}

void EditorShell::OpenMenu(const std::string& menu)
{
    CloseMenu();
    openMenu_ = menu;
    std::vector<EditorMenuItem> items;
    for (const auto& item : chrome_.Menus())
        if (item.menu == menu) items.push_back(item);
    float left = 6.0f;
    for (const auto& name : menuNames_)
    {
        const float width = MenuWidth(name);
        if (name == menu) break;
        left += width + 2.0f;
    }
    ShowMenu(root_, left, menuHeight_, 240.0f, items);
}

void EditorShell::OpenCreateMenu(EditorPanelId panel, float x, float y, std::uint64_t parent)
{
    if (closeModalOpen_ || confirmOpen_ || previewPlaying_) return;
    CloseMenu();
    openMenu_ = "@create";
    std::vector<EditorMenuItem> items;
    const auto add = [&](const char* key, const char* fallback, EditorCommandId id, const char* argument) {
        items.push_back({{}, Text(key, fallback), id, argument});
    };
    if (panel == EditorPanelId::Assets)
    {
        add("EditorUi.NewMaterial", "New material", EditorCommandId::CreateAsset, "material");
        add("EditorUi.CreateScriptAsset", "Create C# script", EditorCommandId::CreateAsset, "script");
        add("Feature.Tod.Create", "Create day cycle", EditorCommandId::CreateAsset, "tod");
        add("Navigation.BakeScene", "Bake scene NavMesh", EditorCommandId::CreateAsset, "navmesh");
        add("EditorUi.Import", "Import", EditorCommandId::Import, "");
        add("Feature.Tools", "Asset and reference tools", EditorCommandId::TogglePanel, "asset-tools");
    }
    else
    {
        add("EditorUi.CreateEmpty", "Create empty object", EditorCommandId::CreateEntity, "empty");
        add("EditorUi.CreateCube", "Create cube", EditorCommandId::CreateEntity, "cube");
        add("EditorUi.CreateSphere", "Create sphere", EditorCommandId::CreateEntity, "sphere");
        add("EditorUi.CreateCamera", "Create camera", EditorCommandId::CreateEntity, "camera");
        add("EditorUi.CreateLight", "Create light", EditorCommandId::CreateEntity, "light");
        add("EditorUi.LightPoint", "Point light", EditorCommandId::CreateEntity, "point-light");
        add("EditorUi.LightSpot", "Spot light", EditorCommandId::CreateEntity, "spot-light");
        add("Feature.Rectangle", "Rectangle area light", EditorCommandId::CreateEntity, "rectangle-light");
        add("Feature.Render", "Scene rendering settings", EditorCommandId::CreateEntity, "render-settings");
        add("Feature.Environment", "Environment lighting", EditorCommandId::CreateEntity, "environment-settings");
        add("Feature.Body", "Physics box", EditorCommandId::CreateEntity, "physics-cube");
        add("EditorUi.CreatePhysicsSphere", "Physics sphere", EditorCommandId::CreateEntity, "physics-sphere");
        add("EditorUi.CreateTriggerBox", "Trigger box", EditorCommandId::CreateEntity, "trigger-box");
        add("Feature.Character", "Capsule character", EditorCommandId::CreateEntity, "character");
        add("EditorUi.CreateAudioSource", "Audio source", EditorCommandId::CreateEntity, "audio-source");
        add("Feature.AudioListener", "Audio listener", EditorCommandId::CreateEntity, "audio-listener");
        add("Feature.ParticleEmitter", "Particle emitter", EditorCommandId::CreateEntity, "particle-emitter");
        add("Feature.NavigationAgent", "Navigation agent", EditorCommandId::CreateEntity, "nav-agent");
        if (panel == EditorPanelId::Hierarchy && parent != 0)
            add("EditorUi.CreatePrefab", "Create Prefab", EditorCommandId::CreateAsset, "prefab");
    }
    ShowMenu(SurfaceRoot(dock_.SurfaceFor(panel)), x, y, 190.0f, items, parent);
}

void EditorShell::ShowMenu(UiElementHandle surface, float x, float y, float width,
                           const std::vector<EditorMenuItem>& items, std::uint64_t target)
{
    const auto bounds = layout_.Rect(surface);
    if (items.empty() || !surface.IsValid()) { CloseMenu(); return; }
    width = std::min(width, bounds.width);
    const float height = 8.0f + static_cast<float>(items.size()) * 26.0f;
    const auto left = std::clamp(x - bounds.x, 0.0f, std::max(0.0f, bounds.width - width));
    const auto top = std::clamp(y - bounds.y, 0.0f, std::max(0.0f, bounds.height - height));
    menuDropdown_ = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(menuDropdown_, surface);
    auto panelStyle = AbsoluteStyle(left, top, width, height);
    panelStyle.background = kSurface;
    panelStyle.borderWidth = 1.0f;
    panelStyle.borderColor = kBorder;
    document_.SetStyle(menuDropdown_, panelStyle);
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];
        const auto button = document_.CreateElement(UiElementKind::Button);
        document_.SetParent(button, menuDropdown_);
        document_.SetStyle(button, ButtonStyle(4.0f, 4.0f + static_cast<float>(i) * 26.0f,
                                               width - 8.0f, 24.0f, kSurface));
        document_.SetText(button, item.label);
        dispatcher_.AddHandler(button, UiEventType::PointerDown,
            [this, command = EditorCommand{item.command, item.argument, target}, label = item.label](UiInputEvent& event) {
                event.handled = true;
                if (event.button != 0) return;
                CloseMenu();
                if (!commands_.CanExecute(command.id))
                    SetStatus(Text("EditorUi.CommandUnavailable", "Command unavailable") + ": " + label);
                else commands_.Execute(command);
            });
    }
}

void EditorShell::CloseMenu()
{
    if (menuDropdown_.IsValid())
    {
        for (auto item = document_.FirstChild(menuDropdown_); item.IsValid(); item = document_.NextSibling(item))
            dispatcher_.RemoveHandlers(item);
        document_.SetVisible(menuDropdown_, false);
        pendingDestroy_.push_back(menuDropdown_);
        menuDropdown_ = {};
    }
    openMenu_.clear();
}

bool EditorShell::RouteShortcut(const UiInputEvent& event, bool textFieldFocused)
{
    if (inspectorColor_)
    {
        if (event.type == UiEventType::KeyDown && event.key == UiKeyEscape) { CloseInspectorColor(false); return true; }
        return !TextFieldFocused();
    }
    if (inspectorDrag_)
    {
        if (event.type == UiEventType::KeyDown && event.key == UiKeyEscape) EndInspectorDrag(false);
        return true;
    }
    if (splitterActive_ && event.type == UiEventType::KeyDown && event.key == UiKeyEscape)
    {
        EndSplitter(false);
        return true;
    }
    if (MenuOpen() && event.type == UiEventType::KeyDown && event.key == UiKeyEscape)
    {
        CloseMenu();
        return true;
    }
    return router_.RouteKey(event, textFieldFocused);
}

} // namespace se::ui::editor
