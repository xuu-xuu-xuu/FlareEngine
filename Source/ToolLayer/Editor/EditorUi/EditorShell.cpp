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

namespace se::ui::editor
{
using namespace detail;

void EditorShell::SetLocalization(const Localization* text) noexcept
{
    localization_ = text;
}

std::string EditorShell::Text(std::string_view key, std::string_view fallback) const
{
    if (localization_ != nullptr)
    {
        std::string value = localization_->GetText(key);
        if (!value.empty() && value != key)
        {
            return value;
        }
    }
    return std::string(fallback);
}

float EditorShell::MeasuredWidth(const std::string& text, float fontSize) const
{
    if (text_ != nullptr && !text.empty())
    {
        const UiTextMeasure measure = text_->Measure(text, fontSize, 100000.0f, false);
        if (std::isfinite(measure.width) && measure.width > 0.0f)
        {
            return measure.width;
        }
    }
    return FallbackTextWidth(text, fontSize);
}

float EditorShell::TabWidth(const std::string& label) const
{
    // 左侧文字内边距和右侧关闭按钮占据同一行。
    return std::clamp(MeasuredWidth(label, 13.0f) + 28.0f, 48.0f, 240.0f);
}

float EditorShell::MenuWidth(const std::string& label) const
{
    return std::max(40.0f, MeasuredWidth(label, 13.0f) + 18.0f);
}

float EditorShell::ToolbarWidth(const std::string& label) const
{
    return std::max(52.0f, MeasuredWidth(label, 13.0f) + 28.0f);
}

std::string EditorShell::PanelLabel(EditorPanelId panel) const
{
    switch (panel)
    {
    case EditorPanelId::Hierarchy: return Text("EditorUi.Hierarchy", "Hierarchy");
    case EditorPanelId::Assets: return Text("EditorUi.Assets", "Assets");
    case EditorPanelId::Viewport: return Text("EditorUi.Viewport", "Viewport");
    case EditorPanelId::UiDesigner: return Text("EditorUi.UiDesigner", "UI Designer");
    case EditorPanelId::Inspector: return Text("EditorUi.Inspector", "Inspector");
    case EditorPanelId::Console: return Text("EditorUi.Console", "Console");
    case EditorPanelId::Material: return Text("EditorUi.MaterialEditor", "Material");
    case EditorPanelId::ProjectSettings: return Text("EditorUi.ProjectSettings", "Project Settings");
    case EditorPanelId::BuildExport: return Text("EditorUi.BuildExport", "Build / Export");
    case EditorPanelId::Preferences: return Text("EditorUi.Preferences", "Preferences");
    case EditorPanelId::About: return Text("EditorUi.About", "About");
    case EditorPanelId::Diagnostics: return Text("EditorUi.Diagnostics", "Diagnostics");
    case EditorPanelId::AssetTools: return Text("Feature.Tools", "Asset and reference tools");
    default: return "Panel";
    }
}

EditorShell::EditorShell(UiTextService* textService)
    : stub_(),
      text_(textService ? textService : &stub_),
      layout_(document_, text_),
      style_(document_),
      dispatcher_(document_, layout_, &style_),
      router_(commands_),
      chrome_(router_, commands_),
      dock_(),
      root_(document_.CreateElement(UiElementKind::Panel))
{
    for (auto& form : forms_) form = std::make_unique<FormPanel>();
    document_.SetParent(root_,document_.Root());
    dispatcher_.SetInputRoot(root_);
    layout_.SetStyleEngine(&style_);
    chrome_.SetStatus("Ready");
}

EditorShell::~EditorShell() { CancelPropertyInteraction(); }

EditorPanelId EditorShell::ActivePanel() const noexcept
{
    return activePanel_;
}

UiElementHandle EditorShell::Root() const noexcept
{
    return root_;
}

UiElementHandle EditorShell::MenuBar() const noexcept
{
    return menuBar_;
}

UiElementHandle EditorShell::Toolbar() const noexcept
{
    return toolbar_;
}

UiElementHandle EditorShell::StatusBar() const noexcept
{
    return statusBar_;
}

UiElementHandle EditorShell::MenuDropdown() const noexcept
{
    return menuDropdown_;
}

bool EditorShell::MenuOpen() const noexcept
{
    return !openMenu_.empty();
}

UiElementHandle EditorShell::Panel(EditorPanelId panel) const noexcept
{
    return Slot(panel).panel;
}

UiElementHandle EditorShell::TabBar(EditorPanelId panel) const noexcept
{
    for (const GroupView& group : groups_)
    {
        if (std::find(group.tabs.begin(), group.tabs.end(), panel) != group.tabs.end())
        {
            return group.tabBar;
        }
    }
    return {};
}

UiElementHandle EditorShell::TabButton(EditorPanelId panel) const noexcept
{
    for (const GroupView& group : groups_)
    {
        for (std::size_t index = 0; index < group.tabs.size() && index < group.tabButtons.size(); ++index)
        {
            if (group.tabs[index] == panel)
            {
                return group.tabButtons[index];
            }
        }
    }
    return {};
}

const std::vector<std::string>& EditorShell::ConsoleLines() const noexcept
{
    return lines_;
}

UiElementHandle EditorShell::ConsolePanel() const noexcept
{
    return Slot(EditorPanelId::Console).panel;
}

UiElementHandle EditorShell::HierarchyContent() const noexcept
{
    return Slot(EditorPanelId::Hierarchy).content;
}

UiElementHandle EditorShell::AssetsContent() const noexcept
{
    return Slot(EditorPanelId::Assets).content;
}

UiElementHandle EditorShell::ViewportPanel() const noexcept
{
    return Slot(EditorPanelId::Viewport).panel;
}

UiElementHandle EditorShell::InspectorContent() const noexcept
{
    return Slot(EditorPanelId::Inspector).content;
}

UiElementHandle EditorShell::InspectorEdit() const noexcept
{
    return inspectorEditElement_;
}

float EditorShell::DpiScale() const noexcept
{
    return dpiScale_;
}

float EditorShell::Width() const noexcept
{
    return width_;
}

float EditorShell::Height() const noexcept
{
    return height_;
}

const std::filesystem::path& EditorShell::LayoutPath() const noexcept
{
    return layoutPath_;
}

EditorShell::PanelSlot& EditorShell::Slot(EditorPanelId panel)
{
    const std::size_t index = static_cast<std::size_t>(panel);
    return slots_[std::min(index, kPanelCount - 1)];
}

const EditorShell::PanelSlot& EditorShell::Slot(EditorPanelId panel) const
{
    const std::size_t index = static_cast<std::size_t>(panel);
    return slots_[std::min(index, kPanelCount - 1)];
}

bool EditorShell::Initialize(float width, float height)
{
    if (initialized_)
    {
        return true;
    }
    width_ = std::max(320.0f, width);
    height_ = std::max(240.0f, height);
    if (!root_.IsValid())
    {
        return false;
    }
    UiStyle rootStyle;
    rootStyle.width = UiLength::Percent(100.0f);
    rootStyle.height = UiLength::Percent(100.0f);
    rootStyle.background = kBackground;
    rootStyle.flexDirection = UiFlexDirection::Column;
    document_.SetStyle(root_, rootStyle);
    BuildPanels();
    BuildChromeMenus();
    BuildChrome();
    chrome_.SetStatus(Text("EditorUi.Ready", "Ready"));
    if (!commands_.CanExecute(EditorCommandId::TogglePanel))
    {
        commands_.Register(EditorCommandId::TogglePanel, [this](const EditorCommand& command) {
            if (command.argument.starts_with("@layout:"))
                return ApplyLayoutPreset(std::string_view(command.argument).substr(8));
            EditorPanelId panel = EditorPanelId::None;
            if (!EditorPanelFromName(command.argument, panel)) return false;
            TogglePanel(panel);
            return true;
        });
    }
    RegisterDefaultShortcuts();
    initialized_ = true;
    if (!layoutPath_.empty())
    {
        LoadLayout();
    }
    Frame();
    return true;
}

void EditorShell::BuildPanels()
{
    for (std::size_t index = 1; index < kPanelCount; ++index)
    {
        PanelSlot& slot = slots_[index];
        slot.id = static_cast<EditorPanelId>(index);
        slot.panel = document_.CreateElement(UiElementKind::Panel);
        if (!slot.panel.IsValid() || !document_.SetParent(slot.panel, root_))
        {
            continue;
        }
        document_.SetStyle(slot.panel, PanelStyle({0.0f, 0.0f, 100.0f, 100.0f}));
        document_.SetVisible(slot.panel, false);
        if (slot.id == EditorPanelId::Viewport)
        {
            continue;
        }
        slot.content = document_.CreateElement(UiElementKind::Panel);
        if (!document_.SetParent(slot.content, slot.panel))
        {
            continue;
        }
        UiStyle contentStyle = AbsoluteStyle(0.0f, 0.0f, 100.0f, 100.0f);
        contentStyle.width = UiLength::Percent(100.0f);
        contentStyle.height = UiLength::Percent(100.0f);
        contentStyle.overflow = UiOverflow::Hidden;
        contentStyle.background = kContent;
        document_.SetStyle(slot.content, contentStyle);
        BuildPanelContent(slot);
    }
}

void EditorShell::SetDpiScale(float scale)
{
    dpiScale_ = scale > 0.0f && std::isfinite(scale) ? scale : 1.0f;
}

void EditorShell::Resize(float width, float height)
{
    width_ = std::max(320.0f, width / dpiScale_);
    height_ = std::max(240.0f, height / dpiScale_);
    if (initialized_)
    {
        Frame();
    }
}

void EditorShell::Frame()
{
    if (!initialized_ || !root_.IsValid())
    {
        return;
    }
    // 结构变化可能发生在事件处理期间；销毁元素统一延迟到帧边界，避免销毁正在分发的目标。
    for (const UiElementHandle element : pendingDestroy_)
    {
        if (element.IsValid())
        {
            document_.DestroyElement(element);
        }
    }
    pendingDestroy_.clear();
    if (builtDockRevision_ != dockRevision_)
    {
        RebuildGroupViews();
        builtDockRevision_ = dockRevision_;
    }
    if (modalBackdrop_.IsValid())
    {
        document_.SetParent(modalBackdrop_, root_);
    }
    if (confirmBackdrop_.IsValid())
    {
        document_.SetParent(confirmBackdrop_, root_);
    }
    UiStyle mainStyle=document_.Style(root_);
    mainStyle.position=UiPosition::Absolute;mainStyle.left=UiLength::Px(0);mainStyle.top=UiLength::Px(0);
    mainStyle.width=UiLength::Px(width_);mainStyle.height=UiLength::Px(height_);
    document_.SetStyle(root_,mainStyle);
    UpdateTransport();
    UpdateDockRects();
    float layoutWidth=width_,layoutHeight=height_;
    for(const auto& surface:surfaces_){layoutWidth=std::max(layoutWidth,surface.width);layoutHeight=std::max(layoutHeight,surface.height);}
    const auto panelHeight = [this](EditorPanelId panel) {
        for (const GroupView& group : groups_)
        {
            if (std::find(group.tabs.begin(), group.tabs.end(), panel) != group.tabs.end())
            {
                return std::max(0.0f, group.rect.height - kTabHeight);
            }
        }
        return 0.0f;
    };
    const auto syncContent = [this, &panelHeight](EditorPanelId panel,
                                                  const std::unique_ptr<UiListView>& list,
                                                  float reserved) {
        if (!list)
        {
            return;
        }
        list->SetViewportHeight(std::max(0.0f, panelHeight(panel) - reserved));
        list->Sync();
    };
    syncContent(EditorPanelId::Console, console_, 24.0f);
    syncContent(EditorPanelId::Hierarchy, hierarchyList_, 0.0f);
    syncContent(EditorPanelId::Assets, assetsList_, assetsPreviewImage_.empty() ? 48.0f : 124.0f);
    syncContent(EditorPanelId::Inspector, inspectorList_, 0.0f);
    UpdatePanelContentRects();
    LayoutInspectorColor();
    RefreshStatus();
    layout_.Layout(layoutWidth, layoutHeight);
    UpdateScrollThumbs();
    // thumb 的 style 变化发生在首帧尾部；立即消化，避免下一帧空闲时仍报告 layout。
    layout_.Layout(layoutWidth, layoutHeight);
}

void EditorShell::SetSelectionHandler(std::function<void(std::uint64_t)> handler)
{
    selectionHandler_ = std::move(handler);
}

void EditorShell::SetPropertyHandler(std::function<bool(std::uint64_t, std::string, std::string)> handler)
{
    propertyHandler_ = std::move(handler);
}
void EditorShell::SetPropertyEditHandler(std::function<bool(std::uint64_t, EditorPropertyEditPhase)> handler)
{
    EndInspectorDrag(false);
    propertyEditHandler_ = std::move(handler);
}

void EditorShell::SetReparentHandler(std::function<bool(std::uint64_t, std::uint64_t)> handler)
{
    reparentHandler_ = std::move(handler);
}

void EditorShell::SetAssetSelectionHandler(std::function<void(std::string)> handler)
{
    assetSelectionHandler_ = std::move(handler);
}

void EditorShell::SetAssetImportHandler(std::function<void()> handler)
{
    assetImportHandler_ = std::move(handler);
}

void EditorShell::SetViewportImage(std::string image)
{
    viewportImage_ = std::move(image);
    if (initialized_)
    {
        Frame();
    }
}

void EditorShell::SetGizmoDragHandler(std::function<void(int, float, float)> handler)
{
    gizmoDragHandler_ = std::move(handler);
}

UiDocument& EditorShell::Document() noexcept
{
    return document_;
}

UiLayoutEngine& EditorShell::Layout() noexcept
{
    return layout_;
}

const UiLayoutEngine& EditorShell::Layout() const noexcept
{
    return layout_;
}

UiDispatcher& EditorShell::Dispatcher() noexcept
{
    return dispatcher_;
}

UiTextService& EditorShell::TextService() noexcept
{
    return *text_;
}

UiListView& EditorShell::ConsoleList() noexcept
{
    return *console_;
}

UiListView& EditorShell::HierarchyList() noexcept
{
    return *hierarchyList_;
}

UiListView& EditorShell::AssetsList() noexcept
{
    return *assetsList_;
}

UiListView& EditorShell::InspectorList() noexcept
{
    return *inspectorList_;
}

EditorDock& EditorShell::Dock() noexcept
{
    return dock_;
}

EditorChrome& EditorShell::Chrome() noexcept
{
    return chrome_;
}

EditorCommandBus& EditorShell::Commands() noexcept
{
    return commands_;
}

EditorCommandRouter& EditorShell::Router() noexcept
{
    return router_;
}

} // namespace se::ui::editor
