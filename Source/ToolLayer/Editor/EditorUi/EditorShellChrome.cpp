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
    router_.RegisterShortcut('S', true, false, true, EditorCommandId::SaveAll);
    router_.RegisterShortcut('Z', true, false, false, EditorCommandId::Undo);
    router_.RegisterShortcut('Z', true, true, false, EditorCommandId::Redo);
    router_.RegisterShortcut('Y', true, false, false, EditorCommandId::Redo);
    router_.RegisterShortcut('P', true, false, false, EditorCommandId::Play);
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
    router_.RegisterShortcut('F', true, true, false, EditorCommandId::SetProperty, "@align-with-view");
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
    chrome_.AddMenu(file, Text("EditorUi.Open", "Open"), EditorCommandId::Open, {}, "Ctrl+O");
    chrome_.AddMenu(file, Text("EditorUi.NewScene", "New Scene"), EditorCommandId::NewScene, {}, "Ctrl+N", true);
    chrome_.AddMenu(file, Text("EditorUi.Save", "Save"), EditorCommandId::Save, {}, "Ctrl+S");
    chrome_.AddMenu(file, Text("EditorUi.SaveSceneAs", "Save As"), EditorCommandId::SaveAs, {}, "Ctrl+Shift+S");
    chrome_.AddMenu(file, Text("EditorUi.SaveAll", "Save All"), EditorCommandId::SaveAll, {}, "Ctrl+Alt+S");

    chrome_.AddMenu(edit, Text("EditorUi.Undo", "Undo"), EditorCommandId::Undo, {}, "Ctrl+Z");
    chrome_.AddMenu(edit, Text("EditorUi.Redo", "Redo"), EditorCommandId::Redo, {}, "Ctrl+Y", true);
    chrome_.AddMenu(edit, Text("EditorUi.Duplicate", "Duplicate"), EditorCommandId::Duplicate, {}, "Ctrl+D");
    chrome_.AddMenu(edit, Text("EditorUi.Delete", "Delete"), EditorCommandId::Delete, {}, "Del", true);
    chrome_.AddMenu(edit, Text("EditorUi.AlignWithView", "Align With View"), EditorCommandId::SetProperty, "@align-with-view", "Ctrl+Shift+F", true);
    chrome_.AddMenu(edit, Text("EditorUi.Preferences", "Preferences"), EditorCommandId::TogglePanel, "preferences", "Ctrl+Shift+P");

    chrome_.AddMenu(play, Text("EditorUi.Play", "Play / Stop"), EditorCommandId::Play, {}, "F5");
    chrome_.AddMenu(play, Text("EditorUi.Pause", "Pause / Resume"), EditorCommandId::Pause, {}, "F6");
    chrome_.AddMenu(play, Text("EditorUi.StepPlay", "Step"), EditorCommandId::Step, {}, "F7");
    chrome_.AddMenu(play, Text("EditorUi.Stop", "Stop"), EditorCommandId::Stop, {}, "F8", true);
    chrome_.AddMenu(play, Text("EditorUi.FullscreenPreview", "Fullscreen Preview"), EditorCommandId::TogglePanel, "@maximize-on-play", "F11");

    chrome_.AddMenu(project, Text("EditorUi.ProjectSettings", "Project Settings"), EditorCommandId::OpenSettings, {}, "Ctrl+,");
    chrome_.AddMenu(project, Text("EditorUi.BuildExport", "Build / Export Panel"), EditorCommandId::TogglePanel,
                    "build-export", "Ctrl+Shift+B", true);
    chrome_.AddMenu(project, Text("EditorUi.CompileScripts", "Compile Scripts"), EditorCommandId::Build, {}, "Ctrl+B");
    chrome_.AddMenu(project, Text("EditorUi.Export", "Export"), EditorCommandId::Export, {}, "Ctrl+E");

    const EditorPanelId workspacePanels[] = {
        EditorPanelId::Viewport, EditorPanelId::Game, EditorPanelId::UiDesigner,
        EditorPanelId::Hierarchy, EditorPanelId::Assets, EditorPanelId::Inspector, EditorPanelId::Console};
    for (std::size_t i = 0; i < std::size(workspacePanels); ++i)
    {
        const EditorPanelId panel = workspacePanels[i];
        chrome_.AddMenu(panels, PanelLabel(panel), EditorCommandId::TogglePanel, EditorPanelName(panel),
                        {}, i + 1 == std::size(workspacePanels));
    }
    const EditorPanelId utilityPanels[] = {
        EditorPanelId::Material, EditorPanelId::AssetTools, EditorPanelId::ProjectSettings, EditorPanelId::BuildExport};
    for (std::size_t i = 0; i < std::size(utilityPanels); ++i)
    {
        const EditorPanelId panel = utilityPanels[i];
        chrome_.AddMenu(panels, PanelLabel(panel), EditorCommandId::TogglePanel, EditorPanelName(panel),
                        {}, i + 1 == std::size(utilityPanels));
    }
    chrome_.AddMenu(panels, Text("EditorUi.FullscreenPreview", "Fullscreen"), EditorCommandId::FocusViewport, {}, "F11");

    const std::string help = Text("EditorUi.Help", "Help");
    chrome_.AddMenu(help, Text("EditorUi.About", "About"), EditorCommandId::TogglePanel, "about", "F1", true);
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
            if (event.button != 0) return;
            if (openMenu_ == name)
            {
                CloseMenu();
                menuPressedFromBar_ = false;
            }
            else
            {
                OpenMenu(name);
                menuPressedFromBar_ = true;
            }
            event.handled = true;
        });
        dispatcher_.AddHandler(button, UiEventType::PointerMove, [this, name](UiInputEvent&) {
            if (!openMenu_.empty() && openMenu_.front() != '@' && openMenu_ != name)
            {
                OpenMenu(name);
            }
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

    const auto addQuickButton = [this](std::string action, std::string label, EditorCommandId cmd,
                                       std::string arg, bool rightAligned) {
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, toolbar_)) return;
        const std::size_t idx = quickButtons_.size();
        dispatcher_.AddHandler(button, UiEventType::PointerDown, [this, idx](UiInputEvent& event) {
            if (event.button != 0) return;
            const auto& q = quickButtons_[idx];
            const auto bounds = layout_.Rect(q.button);
            if (q.action == "create")
            {
                if (openMenu_ == "@create") CloseMenu();
                else OpenCreateMenu(EditorPanelId::None, bounds.x, menuHeight_ + toolbarHeight_);
            }
            else if (q.action == "layout")
            {
                if (openMenu_ == "@layout") CloseMenu();
                else OpenLayoutMenu(bounds.x, menuHeight_ + toolbarHeight_);
            }
            else if (!commands_.CanExecute(q.command))
            {
                SetStatus(Text("EditorUi.CommandUnavailable", "Command unavailable") + ": " + q.label);
            }
            else
            {
                commands_.Execute(EditorCommand{q.command, q.argument});
            }
            event.handled = true;
        });
        quickButtons_.push_back({button, std::move(action), std::move(label), cmd, std::move(arg), rightAligned});
    };
    addQuickButton("save", Text("EditorUi.Save", "Save"), EditorCommandId::Save, {}, false);
    addQuickButton("undo", Text("EditorUi.Undo", "Undo"), EditorCommandId::Undo, {}, false);
    addQuickButton("redo", Text("EditorUi.Redo", "Redo"), EditorCommandId::Redo, {}, false);
    addQuickButton("create", Text("EditorUi.QuickCreate", "+ Create") + " ▾", EditorCommandId::CreateEntity, {}, false);
    addQuickButton("import", Text("EditorUi.Import", "Import"), EditorCommandId::Import, {}, false);
    addQuickButton("compile", Text("EditorUi.CompileScripts", "Compile Scripts"), EditorCommandId::Build, {}, false);

    addQuickButton("layout", Text("EditorUi.Layout", "Layout") + " ▾", EditorCommandId::TogglePanel, {}, true);
    addQuickButton("build", Text("EditorUi.QuickBuild", "Build & Export"), EditorCommandId::TogglePanel, "build-export", true);
    addQuickButton("settings", Text("EditorUi.ProjectSettings", "Project Settings"), EditorCommandId::OpenSettings, {}, true);

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

    gameTools_ = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(gameTools_, Slot(EditorPanelId::Game).panel.IsValid() ? Slot(EditorPanelId::Game).panel : root_);
    document_.SetStyle(gameTools_, AbsoluteStyle(0, 0, 100, 24));
    for (int idx = 0; idx < 2; ++idx)
    {
        const auto button = document_.CreateElement(UiElementKind::Button);
        document_.SetParent(button, gameTools_);
        dispatcher_.AddHandler(button, UiEventType::PointerDown, [this, idx](UiInputEvent& e) {
            if (e.button == 0)
            {
                const char* arg = idx == 0 ? "@preview-windowed" : "@preview-fullscreen";
                if (commands_.CanExecute(EditorCommandId::TogglePanel))
                    (void)commands_.Execute(EditorCommand{EditorCommandId::TogglePanel, arg});
                else
                    SetMaximizeOnPlay(idx == 1);
            }
            e.handled = true;
            e.StopPropagation();
        });
        gameToolButtons_.push_back(button);
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
    RebuildConsoleDisplay();
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
        if (addCompPopup_.IsValid())
        {
            const auto rect = layout_.Rect(addCompPopup_);
            if (event.x < rect.x || event.y < rect.y || event.x >= rect.x + rect.width ||
                event.y >= rect.y + rect.height)
            {
                CloseAddComponentPopup();
                dispatcher_.ClearFocus();
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
            bool onPopupTrigger = (event.target == assetsTypeElement_ || event.target == assetsCreateElement_);
            for (const auto& q : quickButtons_)
                if ((q.action == "create" || q.action == "layout") && event.target == q.button) onPopupTrigger = true;
            bool onMenuButton = false;
            for (const auto& mb : menuButtons_)
                if (event.target == mb) onMenuButton = true;
            if (!insideDropdown && !onPopupTrigger && !onMenuButton)
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
            return;
        }
        int nextHoveredSplitter = -1;
        if (dragTabPanel_ == EditorPanelId::None && !MenuOpen() && !ColorPickerOpen() && !confirmOpen_)
        {
            if (const auto* handle = FindSplitter(event.x, event.y))
                nextHoveredSplitter = static_cast<int>(handle - splitHandles_.data());
        }
        if (nextHoveredSplitter != hoveredSplitterIndex_)
        {
            hoveredSplitterIndex_ = nextHoveredSplitter;
            if (initialized_) UpdateDockRects();
        }
    }, UiEventPhase::Capture);

    dispatcher_.AddHandler(document_.Root(), UiEventType::PointerUp, [this](UiInputEvent& event) {
        menuPressedFromBar_ = false;
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
        style.background = hoveredChrome_ == item ? UiInk(flare::workspace::Selection) : UiInk(flare::workspace::Panel);
        document_.SetStyle(item, style);
    }
    if (toolbar_.IsValid())
    {
        auto barStyle = document_.Style(toolbar_);
        barStyle.background = previewPaused_ ? UiInk(flare::workspace::PauseTint)
            : (previewPlaying_ ? UiInk(flare::workspace::PlayTint) : kSurface);
        document_.SetStyle(toolbar_, barStyle);
    }

    constexpr float cellWidth = 38.0f;
    const float transportSpan = cellWidth * static_cast<float>(toolbarButtons_.size());
    const float left = std::floor((width_ - transportSpan) * 0.5f);
    for (std::size_t i = 0; i < toolbarButtons_.size(); ++i)
    {
        const auto button = toolbarButtons_[i];
        const auto command = chrome_.Toolbar()[i].command;
        const bool play = command == EditorCommandId::Play;
        const bool pause = command == EditorCommandId::Pause;
        const bool enabled = play || (previewPlaying_ && (pause || previewPaused_));
        const bool active = (play && previewPlaying_) || (pause && previewPaused_);
        const char* icon = play ? (previewPlaying_ ? "■" : "▶") : (pause ? "Ⅱ" : "▸│");
        const auto activeColor = play ? flare::Success : flare::Warning;
        const auto base = active ? activeColor :
                          (pressedTransport_ == button ? flare::workspace::Pressed :
                          (hoveredChrome_ == button && enabled ? flare::workspace::Hover : flare::workspace::Header));
        auto style = ButtonStyle(left + cellWidth * i, 4, cellWidth - 2, toolbarHeight_ - 8, UiInk(base));
        style.cornerRadius = 5.0f;
        style.color = active ? UiInk(flare::Heading) : (enabled ? UiInk(play ? flare::Gold : flare::Heading) : kMuted);
        style.borderWidth = 1.0f;
        style.borderColor = active ? UiInk(flare::Heading, 0.35f) : kBorder;
        style.padding.left = std::max(0.0f, (cellWidth - 2 - MeasuredWidth(icon, style.fontSize)) * 0.5f);
        style.padding.right = 0.0f;
        document_.SetStyle(button, style);
        document_.SetText(button, icon);
        document_.SetEnabled(button, enabled);
    }

    float leftX = 8.0f;
    float rightX = width_ - 8.0f;
    const float leftLimit = left - 10.0f;
    const float rightLimit = left + transportSpan + 10.0f;
    for (const auto& q : quickButtons_)
    {
        if (!q.button.IsValid()) continue;
        std::string text = q.label;
        if (q.action == "save" && dirty_) text += " ●";
        else if (q.action == "compile" && scriptsDirty_) text = Text("EditorUi.CompileScriptsDirty", "Compile Scripts ●");
        const float btnW = MeasuredWidth(text, 12.5f) + 16.0f;
        float x = 0.0f;
        bool fits = false;
        if (!q.rightAligned)
        {
            fits = (leftX + btnW <= leftLimit);
            x = leftX;
            if (fits) leftX += btnW + 4.0f;
        }
        else
        {
            fits = (rightX - btnW >= rightLimit);
            x = rightX - btnW;
            if (fits) rightX -= btnW + 4.0f;
        }
        document_.SetVisible(q.button, fits);
        if (!fits) continue;

        const bool menuActive = (q.action == "create" && openMenu_ == "@create") ||
                                (q.action == "layout" && openMenu_ == "@layout");
        const bool accent = (q.action == "save" && dirty_) || (q.action == "compile" && scriptsDirty_) || menuActive;
        const bool disabled = previewPlaying_ && (q.action == "create" || q.action == "import" || q.action == "compile");
        const auto fill = accent ? flare::workspace::Selection :
                          (hoveredChrome_ == q.button && !disabled ? flare::workspace::Hover : flare::workspace::Header);
        auto style = ButtonStyle(x, 4.0f, btnW, toolbarHeight_ - 8.0f, UiInk(fill));
        style.cornerRadius = 5.0f;
        style.fontSize = 12.5f;
        style.color = disabled ? kMuted : UiInk(accent ? flare::Gold : flare::Text);
        style.borderWidth = 1.0f;
        style.borderColor = accent ? UiInk(flare::workspace::Focus, 0.7f) : kBorder;
        document_.SetText(q.button, text);
        document_.SetStyle(q.button, style);
        document_.SetEnabled(q.button, !disabled);
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
                                   bool playing, bool paused, bool sceneViewDuringPlay)
{
    const bool playChanged = (previewPlaying_ != playing);
    previewPlaying_ = playing;
    previewPaused_ = playing && paused;
    if (playChanged && inspectorList_) RefreshInspectorRows();
    const auto surface = SurfaceRoot(dock_.SurfaceFor(EditorPanelId::Viewport));
    const auto viewport = layout_.Rect(ViewportPanel());
    const float available = std::max(0.f, viewport.width - 16.f);
    const bool showViewTools = (!playing || sceneViewDuringPlay) && !gameMaximized_ &&
                               document_.Visible(ViewportPanel()) && viewport.width > 48.f && viewport.height > 36.f;
    const bool showTransformTools = !playing && showViewTools;
    document_.SetParent(viewportTools_, surface.IsValid() ? surface : root_);
    document_.SetParent(transformTools_, surface.IsValid() ? surface : root_);
    document_.SetVisible(viewportTools_, showViewTools);
    document_.SetVisible(transformTools_, showTransformTools);
    if (!showViewTools) return;

    const bool wideTools = available >= 520.f;
    const std::string labels[] = {
        wideTools ? Text("EditorUi.ToolViewShort", "Hand Q") : "✥",
        wideTools ? Text("EditorUi.ToolMoveShort", "Move W") : "↔",
        wideTools ? Text("EditorUi.ToolRotateShort", "Rotate E") : "⟳",
        wideTools ? Text("EditorUi.ToolScaleShort", "Scale R") : "⤢",
        Text(local ? "EditorUi.LocalSpace" : "EditorUi.WorldSpace", local ? "Local" : "World"),
        Text(center ? "EditorUi.Center" : "EditorUi.Pivot", center ? "Center" : "Pivot"),
        Text("EditorUi.ViewportSky", "Sky"),
        Text("EditorUi.ViewportGrid", "Grid"),
        Text("EditorUi.ViewportGizmos", "Gizmos"),
        Text("EditorUi.FrameAll", "Frame All")};
    const bool active[] = {tool == 0, tool == 1, tool == 2, tool == 3, local, center, sky, grid, gizmos, false};

    constexpr float pillH = 24.f;
    float tx = 1.f;
    for (std::size_t i = 0; i < 4 && i < viewportToolButtons_.size(); ++i)
    {
        const auto button = viewportToolButtons_[i];
        document_.SetVisible(button, showTransformTools);
        if (!showTransformTools) continue;
        const auto fill = active[i] ? flare::workspace::Selection :
            (hoveredChrome_ == button ? flare::workspace::Hover : flare::workspace::Header);
        const float w = wideTools ? (MeasuredWidth(labels[i], 12.f) + 14.f) : 28.f;
        auto style = ButtonStyle(tx, 1.f, w, pillH, UiInk(fill));
        style.cornerRadius = 5.0f;
        style.fontSize = 12.f;
        style.color = active[i] ? UiInk(flare::Gold) : kText;
        if (!wideTools)
        {
            style.padding.left = std::max(0.0f, (w - MeasuredWidth(labels[i], 12.f)) * 0.5f);
            style.padding.right = 0.0f;
        }
        document_.SetText(button, labels[i]);
        document_.SetStyle(button, style);
        tx += w + 1.f;
    }
    const float transformW = std::min(tx, available);
    auto tools = AbsoluteStyle(viewport.x + 8.f, viewport.y + 8.f, transformW, pillH + 2.f);
    tools.background = UiInk(flare::Background, 0.82f);
    tools.cornerRadius = 6.0f;
    tools.overflow = UiOverflow::Hidden;
    document_.SetStyle(transformTools_, tools);

    float vx = 1.f, vy = 1.f, maxWidth = 0.f;
    const float effectiveTransformW = showTransformTools ? transformW : 0.f;
    const float sameRowLimit = std::max(0.f, available - (showTransformTools ? (transformW + 8.f) : 0.f));
    const bool placeNextToTransform = !showTransformTools || sameRowLimit >= 220.f;
    const float rowBudget = placeNextToTransform ? sameRowLimit : available;
    for (std::size_t i = 4; i < viewportToolButtons_.size(); ++i)
    {
        const auto button = viewportToolButtons_[i];
        const bool showBtn = (i >= 6) || showTransformTools;
        document_.SetVisible(button, showBtn);
        if (!showBtn) continue;
        const auto fill = active[i] ? flare::workspace::Selection :
            (hoveredChrome_ == button ? flare::workspace::Hover : flare::workspace::Header);
        const float width = std::min(rowBudget, MeasuredWidth(labels[i], 12.f) + 14.f);
        if (vx > 1.f && vx + width > rowBudget) { vx = 1.f; vy += pillH + 1.f; }
        auto style = ButtonStyle(vx, vy, width, pillH, UiInk(fill));
        style.cornerRadius = 5.0f;
        style.fontSize = 12.f;
        style.color = active[i] ? UiInk(flare::Gold) : kText;
        vx += width + 1.f;
        maxWidth = std::max(maxWidth, vx);
        document_.SetText(button, labels[i]);
        document_.SetStyle(button, style);
    }
    const float barX = placeNextToTransform ? (viewport.x + 8.f + (showTransformTools ? (effectiveTransformW + 8.f) : 0.f)) : (viewport.x + 8.f);
    const float barY = placeNextToTransform ? (viewport.y + 8.f) : (viewport.y + 8.f + pillH + 6.f);
    const float height = std::min(vy + pillH + 1.f, std::max(0.f, viewport.height - (barY - viewport.y) - 4.f));
    auto bar = AbsoluteStyle(barX, barY, maxWidth, height);
    bar.background = UiInk(flare::Background, 0.82f);
    bar.cornerRadius = 6.0f;
    bar.overflow = UiOverflow::Hidden;
    document_.SetStyle(viewportTools_, bar);
}

void EditorShell::SetMaximizeOnPlay(bool enabled)
{
    const bool changed = dock_.MaximizeOnPlay() != enabled;
    if (changed)
    {
        dock_.SetMaximizeOnPlay(enabled);
        SaveLayoutIfConfigured();
    }
    if (previewPlaying_)
    {
        SetGameMaximized(enabled);
        if (changed && initialized_) Frame();
    }
    else if (!enabled && gameMaximized_)
    {
        SetGameMaximized(false);
    }
    else if (changed && initialized_)
    {
        Frame();
    }
}

void EditorShell::SetGameMaximized(bool maximized)
{
    if (gameMaximized_ == maximized) return;
    gameMaximized_ = maximized;
    if (gameMaximized_)
    {
        if (!dock_.Find(EditorPanelId::Game))
            dock_.OpenTabNear(EditorPanelId::Game, EditorPanelId::Console);
        dock_.SelectTab(EditorPanelId::Game);
        SetActivePanel(EditorPanelId::Game);
    }
    MarkDockDirty(true, false);
    if (initialized_) Frame();
}

void EditorShell::UpdateGameTools()
{
    if (!gameTools_.IsValid() || gameToolButtons_.size() < 2) return;
    const auto gamePanel = GamePanel();
    const auto gameRect = layout_.Rect(gamePanel);
    const bool visible = document_.Visible(gamePanel) && gameRect.width > 48.f && gameRect.height >= 24.f;
    document_.SetVisible(gameTools_, visible);
    if (!visible) return;

    auto bar = AbsoluteStyle(0.f, 0.f, gameRect.width, 24.f);
    bar.width = UiLength::Percent(100.f);
    bar.background = UiInk(flare::workspace::Header, 0.85f);
    bar.borderWidth = 1.0f;
    bar.borderColor = kBorder;
    bar.overflow = UiOverflow::Hidden;
    document_.SetStyle(gameTools_, bar);

    const std::string labels[2] = {
        Text("EditorUi.WindowedPreview", "Windowed Preview"),
        Text("EditorUi.FullscreenPreview", "Fullscreen Preview")};
    const bool active[2] = {!dock_.MaximizeOnPlay(), dock_.MaximizeOnPlay()};
    constexpr float btnH = 20.f;
    float gx = 6.f;
    const float maxAvail = std::max(32.f, gameRect.width - 12.f);
    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto btn = gameToolButtons_[i];
        document_.SetVisible(btn, true);
        const float textW = MeasuredWidth(labels[i], 12.f);
        const float w = std::min(std::max(24.f, maxAvail * 0.5f), textW + 16.f);
        const auto fill = active[i] ? flare::workspace::Selection :
            (hoveredChrome_ == btn ? flare::workspace::Hover : flare::workspace::Header);
        auto style = ButtonStyle(gx, 2.f, w, btnH, UiInk(fill));
        style.cornerRadius = 4.0f;
        style.fontSize = 12.f;
        style.color = active[i] ? UiInk(flare::Gold) : (hoveredChrome_ == btn ? kText : kMuted);
        style.borderWidth = 1.0f;
        style.borderColor = active[i] ? UiInk(flare::workspace::Focus, 0.72f) : kBorder;
        style.padding.left = std::max(4.f, (w - textW) * 0.5f);
        style.padding.right = 2.f;
        document_.SetText(btn, labels[i]);
        document_.SetStyle(btn, style);
        gx += w + 3.f;
    }
}

void EditorShell::AppendConsoleLine(std::string line)
{
    bool followTail = false;
    if (console_)
    {
        const float maxBefore = std::max(0.0f, static_cast<float>(console_->ItemCount()) * console_->RowHeight() - console_->ViewportHeight());
        followTail = (console_->Offset() >= maxBefore - 24.0f);
    }
    lines_.push_back(std::move(line));
    RebuildConsoleDisplay();
    if (console_ && followTail)
    {
        const float maxAfter = std::max(0.0f, static_cast<float>(console_->ItemCount()) * console_->RowHeight() - console_->ViewportHeight());
        console_->SetScrollOffset(maxAfter);
        console_->Sync();
    }
}

void EditorShell::ClearConsole()
{
    lines_.clear();
    if (console_)
    {
        console_->SetScrollOffset(0.0f);
    }
    RebuildConsoleDisplay();
    SetStatus(Text("EditorUi.ConsoleCleared", "Console cleared"));
}

bool EditorShell::ApplyLayoutPreset(std::string_view preset)
{
    DockNode targetLayout = DefaultEditorDockLayout();
    if (preset == "standard") targetLayout = StandardShelfDockLayout();
    else if (preset == "wide") targetLayout = WideViewportDockLayout();
    else if (preset != "reset" && preset != "default" && preset != "2by3") return false;

    const bool keepMaxOnPlay = dock_.MaximizeOnPlay();
    gameMaximized_ = false;
    dock_ = EditorDock(std::move(targetLayout));
    dock_.SetMaximizeOnPlay(keepMaxOnPlay);
    if (!dock_.Find(activePanel_)) SetActivePanel(EditorPanelId::Viewport);
    MarkDockDirty(true, true);
    SetStatus(Text("EditorUi.LayoutApplied", "Workspace layout applied"));
    return true;
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
    std::string hoverHint;
    for (std::size_t i = 0; i < toolbarButtons_.size(); ++i)
    {
        if (hoveredChrome_ != toolbarButtons_[i]) continue;
        const auto& item = chrome_.Toolbar()[i];
        hoverHint = item.command == EditorCommandId::Play && previewPlaying_ ? Text("EditorUi.Stop", "Stop") : item.label;
        break;
    }
    const char* toolKeys[] = {"EditorUi.ViewTool", "EditorUi.Translate", "EditorUi.Rotate", "EditorUi.ScaleTool"};
    const char* toolNames[] = {"View tool", "Translate", "Rotate", "Scale"};
    for (std::size_t i = 0; i < 4 && i < viewportToolButtons_.size(); ++i)
    {
        if (hoveredChrome_ == viewportToolButtons_[i])
            hoverHint = Text(toolKeys[i], toolNames[i]) + " (" + "QWER"[i] + ")";
    }
    const bool idleStatus = status_.empty() || status_ == "Ready" || status_ == Text("EditorUi.Ready", "Ready");
    std::string primary = !hoverHint.empty() ? hoverHint : (!idleStatus ? status_ : std::string{});
    std::string text;
    if (!primary.empty() && !selectionLabel_.empty())
        text = primary + "   |   " + selectionLabel_;
    else if (!primary.empty())
        text = std::move(primary);
    else
        text = selectionLabel_;

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
    {
        if (item.menu != menu) continue;
        EditorMenuItem copy = item;
        if ((copy.argument == "@maximize-on-play" || copy.command == EditorCommandId::FocusViewport) && MaximizeOnPlay())
            copy.label = "✓ " + copy.label;
        items.push_back(std::move(copy));
    }
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
    const auto add = [&](const char* key, const char* fallback, EditorCommandId id, const char* argument,
                         bool sepAfter = false) {
        items.push_back({{}, Text(key, fallback), id, argument, {}, sepAfter});
    };
    if (panel == EditorPanelId::Assets)
    {
        add("EditorUi.NewFolder", "New Folder", EditorCommandId::CreateAsset, "folder");
        add("EditorUi.NewMaterial", "New material", EditorCommandId::CreateAsset, "material");
        add("EditorUi.CreateScriptAsset", "Create C# script", EditorCommandId::CreateAsset, "script", true);
        add("Feature.Tod.Create", "Create day cycle", EditorCommandId::CreateAsset, "tod");
        add("Navigation.BakeScene", "Bake scene NavMesh", EditorCommandId::CreateAsset, "navmesh", true);
        add("EditorUi.Import", "Import", EditorCommandId::Import, "");
        add("Feature.Tools", "Asset and reference tools", EditorCommandId::TogglePanel, "asset-tools");
    }
    else
    {
        if (panel == EditorPanelId::None)
        {
            add("EditorUi.NewFolder", "New Folder", EditorCommandId::CreateAsset, "folder");
            add("EditorUi.CreateScriptAsset", "Create C# script", EditorCommandId::CreateAsset, "script");
            add("EditorUi.NewMaterial", "New material", EditorCommandId::CreateAsset, "material", true);
        }
        add("EditorUi.CreateEmpty", "Create empty object", EditorCommandId::CreateEntity, "empty");
        add("EditorUi.CreateCube", "Create cube", EditorCommandId::CreateEntity, "cube");
        add("EditorUi.CreateSphere", "Create sphere", EditorCommandId::CreateEntity, "sphere", true);
        add("EditorUi.CreateCamera", "Create camera", EditorCommandId::CreateEntity, "camera");
        add("EditorUi.CreateLight", "Create light", EditorCommandId::CreateEntity, "light");
        add("EditorUi.LightPoint", "Point light", EditorCommandId::CreateEntity, "point-light");
        add("EditorUi.LightSpot", "Spot light", EditorCommandId::CreateEntity, "spot-light");
        add("Feature.Rectangle", "Rectangle area light", EditorCommandId::CreateEntity, "rectangle-light", true);
        add("Feature.Render", "Scene rendering settings", EditorCommandId::CreateEntity, "render-settings");
        add("Feature.Environment", "Environment lighting", EditorCommandId::CreateEntity, "environment-settings", true);
        add("Feature.Body", "Physics box", EditorCommandId::CreateEntity, "physics-cube");
        add("EditorUi.CreatePhysicsSphere", "Physics sphere", EditorCommandId::CreateEntity, "physics-sphere");
        add("EditorUi.CreateTriggerBox", "Trigger box", EditorCommandId::CreateEntity, "trigger-box");
        add("Feature.Character", "Capsule character", EditorCommandId::CreateEntity, "character", true);
        add("EditorUi.CreateAudioSource", "Audio source", EditorCommandId::CreateEntity, "audio-source");
        add("Feature.AudioListener", "Audio listener", EditorCommandId::CreateEntity, "audio-listener");
        add("Feature.ParticleEmitter", "Particle emitter", EditorCommandId::CreateEntity, "particle-emitter");
        add("Feature.NavigationAgent", "Navigation agent", EditorCommandId::CreateEntity, "nav-agent",
            panel == EditorPanelId::Hierarchy && parent != 0);
        if (panel == EditorPanelId::Hierarchy && parent != 0)
            add("EditorUi.CreatePrefab", "Create Prefab", EditorCommandId::CreateAsset, "prefab");
    }
    const UiElementHandle menuSurface = panel == EditorPanelId::None ? root_ : SurfaceRoot(dock_.SurfaceFor(panel));
    ShowMenu(menuSurface, x, y, 210.0f, items, parent);
}

void EditorShell::OpenAssetItemMenu(const std::string& path, const std::string& type, float x, float y)
{
    if (closeModalOpen_ || confirmOpen_ || previewPlaying_) return;
    CloseMenu();
    openMenu_ = "@asset-item";
    menuTargetAssetPath_ = path;
    (void)type;
    std::vector<EditorMenuItem> items{
        {{}, Text("EditorUi.NewFolder", "New Folder"), EditorCommandId::CreateAsset, "folder"},
        {{}, Text("EditorUi.NewMaterial", "New material"), EditorCommandId::CreateAsset, "material"},
        {{}, Text("EditorUi.CreateScriptAsset", "Create C# script"), EditorCommandId::CreateAsset, "script", {}, true},
        {{}, Text("EditorUi.RenameAsset", "Rename"), EditorCommandId::SetProperty, "@asset-ctx:rename", "F2"},
        {{}, Text("EditorUi.DeleteAsset", "Delete"), EditorCommandId::SetProperty, "@asset-ctx:delete", "Del", true},
        {{}, Text("EditorUi.ShowInExplorer", "Show in Explorer"), EditorCommandId::SetProperty, "@asset-ctx:explorer"},
    };
    ShowMenu(SurfaceRoot(dock_.SurfaceFor(EditorPanelId::Assets)), x, y, 200.0f, items);
}

void EditorShell::OpenLayoutMenu(float x, float y)
{
    if (closeModalOpen_ || confirmOpen_) return;
    CloseMenu();
    openMenu_ = "@layout";
    std::vector<EditorMenuItem> items{
        {{}, Text("EditorUi.Layout2By3", "2 by 3 Layout"), EditorCommandId::TogglePanel, "@layout:2by3"},
        {{}, Text("EditorUi.LayoutStandard", "Standard Shelf Layout"), EditorCommandId::TogglePanel, "@layout:standard"},
        {{}, Text("EditorUi.LayoutWide", "Wide Viewport Layout"), EditorCommandId::TogglePanel, "@layout:wide", {}, true},
        {{}, Text("EditorUi.LayoutReset", "Reset Default 2 by 3 Layout"), EditorCommandId::TogglePanel, "@layout:reset"},
    };
    ShowMenu(root_, x, y, 220.0f, items);
}

void EditorShell::OpenAssetTypeMenu(float x, float y)
{
    if (closeModalOpen_ || confirmOpen_) return;
    CloseMenu();
    openMenu_ = "@asset-type";
    std::vector<EditorMenuItem> items{
        {{}, Text("EditorUi.FilterAll", "All"), EditorCommandId::SetProperty, "@asset-type:", {}, true},
        {{}, Text("EditorUi.DocumentScene", "Scene"), EditorCommandId::SetProperty, "@asset-type:scene"},
        {{}, Text("EditorUi.AssetModel", "Model"), EditorCommandId::SetProperty, "@asset-type:model"},
        {{}, Text("EditorUi.DocumentMaterial", "Material"), EditorCommandId::SetProperty, "@asset-type:material"},
        {{}, Text("EditorUi.AssetTexture", "Texture"), EditorCommandId::SetProperty, "@asset-type:texture"},
        {{}, Text("EditorUi.AssetPrefab", "Prefab"), EditorCommandId::SetProperty, "@asset-type:prefab"},
        {{}, Text("EditorUi.AssetScript", "C# Script"), EditorCommandId::SetProperty, "@asset-type:C#"},
        {{}, Text("EditorUi.AssetAudio", "Audio"), EditorCommandId::SetProperty, "@asset-type:audio"},
    };
    ShowMenu(SurfaceRoot(dock_.SurfaceFor(EditorPanelId::Assets)), x, y, 168.0f, items);
}

void EditorShell::ShowMenu(UiElementHandle surface, float x, float y, float width,
                           const std::vector<EditorMenuItem>& items, std::uint64_t target)
{
    const auto bounds = layout_.Rect(surface);
    if (items.empty() || !surface.IsValid()) { CloseMenu(); return; }
    float naturalWidth = width;
    std::size_t sepCount = 0;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];
        const float labelW = MeasuredWidth(item.label, 13.0f);
        const float shortW = item.shortcut.empty() ? 0.0f : (MeasuredWidth(item.shortcut, 11.5f) + 22.0f);
        naturalWidth = std::max(naturalWidth, labelW + shortW + 30.0f);
        if (item.separatorAfter && i + 1 < items.size()) ++sepCount;
    }
    width = std::min(naturalWidth, bounds.width);
    const float height = 10.0f + static_cast<float>(items.size()) * 26.0f + static_cast<float>(sepCount) * 7.0f;
    const auto left = std::clamp(x - bounds.x, 0.0f, std::max(0.0f, bounds.width - width));
    const auto top = std::clamp(y - bounds.y, 0.0f, std::max(0.0f, bounds.height - height));
    menuDropdown_ = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(menuDropdown_, surface);
    auto panelStyle = AbsoluteStyle(left, top, width, height);
    panelStyle.background = UiInk(flare::workspace::Panel);
    panelStyle.borderWidth = 1.0f;
    panelStyle.borderColor = UiInk(flare::workspace::Divider);
    panelStyle.cornerRadius = 8.0f;
    document_.SetStyle(menuDropdown_, panelStyle);

    float itemY = 5.0f;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];
        const bool hasSep = item.separatorAfter && i + 1 < items.size();
        const auto button = document_.CreateElement(UiElementKind::Button);
        document_.SetParent(button, menuDropdown_);
        auto itemStyle = ButtonStyle(5.0f, itemY, width - 10.0f, 24.0f, UiInk(flare::workspace::Panel));
        itemStyle.cornerRadius = 4.0f;
        itemStyle.padding.left = 10.0f;
        itemStyle.padding.right = item.shortcut.empty() ? 10.0f : (MeasuredWidth(item.shortcut, 11.5f) + 18.0f);
        document_.SetStyle(button, itemStyle);
        document_.SetText(button, item.label);

        if (!item.shortcut.empty())
        {
            const auto hint = document_.CreateElement(UiElementKind::Label);
            if (document_.SetParent(hint, button))
            {
                const float sw = MeasuredWidth(item.shortcut, 11.5f) + 4.0f;
                auto hintStyle = AbsoluteStyle(std::max(0.0f, width - 10.0f - sw - 8.0f), 0.0f, sw, 24.0f);
                hintStyle.background = UiColor{0, 0, 0, 0};
                hintStyle.color = kMuted;
                hintStyle.fontSize = 11.5f;
                hintStyle.textFlow = UiTextFlow::SingleLineEllipsis;
                hintStyle.pointerEvents = UiPointerEvents::None;
                document_.SetStyle(hint, hintStyle);
                document_.SetText(hint, item.shortcut);
            }
        }
        if (hasSep)
        {
            const auto divider = document_.CreateElement(UiElementKind::Panel);
            if (document_.SetParent(divider, button))
            {
                auto divStyle = AbsoluteStyle(6.0f, 27.0f, std::max(0.0f, width - 22.0f), 1.0f);
                divStyle.background = kBorder;
                divStyle.pointerEvents = UiPointerEvents::None;
                document_.SetStyle(divider, divStyle);
            }
        }

        const auto executeItem = [this, command = EditorCommand{item.command, item.argument, target},
                                  label = item.label](UiInputEvent& event) {
            event.handled = true;
            if (event.button != 0) return;
            const std::string targetPath = menuTargetAssetPath_;
            menuPressedFromBar_ = false;
            CloseMenu();
            if (command.argument.starts_with("@layout:"))
            {
                ApplyLayoutPreset(std::string_view(command.argument).substr(8));
                return;
            }
            if (command.argument.starts_with("@asset-type:"))
            {
                assetsModel_.SetTypeFilter(std::string_view(command.argument).substr(12));
                assetsGridDirty_ = true;
                if (assetsList_)
                {
                    assetsList_->SetScrollOffset(0.0f);
                    assetsList_->Invalidate();
                    assetsList_->SetItemCount(static_cast<int>(assetsModel_.Filtered().size()));
                    assetsList_->Sync();
                }
                UpdatePanelContentRects();
                return;
            }
            if (command.argument.starts_with("@asset-ctx:"))
            {
                const std::string_view action = std::string_view(command.argument).substr(11);
                if (action == "rename")
                {
                    BeginAssetRename(targetPath);
                }
                else if (assetContextActionHandler_)
                {
                    assetContextActionHandler_(targetPath, action);
                }
                return;
            }
            if (!commands_.CanExecute(command.id))
                SetStatus(Text("EditorUi.CommandUnavailable", "Command unavailable") + ": " + label);
            else commands_.Execute(command);
        };
        dispatcher_.AddHandler(button, UiEventType::PointerDown, executeItem);
        dispatcher_.AddHandler(button, UiEventType::PointerUp, [this, executeItem](UiInputEvent& event) {
            if (menuPressedFromBar_) executeItem(event);
        });
        itemY += 26.0f + (hasSep ? 7.0f : 0.0f);
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
    menuTargetAssetPath_.clear();
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
    if (AddComponentPopupOpen() && event.type == UiEventType::KeyDown && event.key == UiKeyEscape)
    {
        CloseAddComponentPopup();
        dispatcher_.ClearFocus();
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
