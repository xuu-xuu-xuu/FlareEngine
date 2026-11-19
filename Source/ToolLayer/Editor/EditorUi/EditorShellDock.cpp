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

void EditorShell::RebuildGroupViews()
{
    if (splitterActive_) EndSplitter(false);
    hoveredSplitterIndex_ = -1;
    for (const auto& handle : splitHandles_) document_.DestroyElement(handle.line);
    splitHandles_.clear();
    for (GroupView& group : groups_)
    {
        if (group.tabBar.IsValid())
        {
            document_.DestroyElement(group.tabBar);
        }
    }
    for(const auto& slot:slots_)if(slot.panel.IsValid())document_.SetParent(slot.panel,root_);
    document_.SetParent(dropHighlight_,root_);
    document_.SetParent(tabInsertMarker_,root_);
    document_.SetParent(dragGhost_,root_);
    document_.SetParent(viewportTools_,root_);
    std::erase_if(surfaces_,[this](const auto& surface){
        if(dock_.Surface(surface.id))return false;
        document_.DestroyElement(surface.root);return true;
    });
    for(const auto& window:dock_.Windows())if(!SurfaceRoot(window.id).IsValid())
    {
        const auto element=document_.CreateElement(UiElementKind::Panel);
        document_.SetParent(element,document_.Root());
        UiStyle style=AbsoluteStyle(0,0,640,440);style.background=kContent;
        document_.SetStyle(element,style);surfaces_.push_back({window.id,element,640,440});
    }
    groups_.clear();
    std::uint32_t surface=0;
    const auto walk = [this,&surface](const auto& self, const DockNode& node) -> void {
        if (node.kind == DockNode::Kind::Tabs)
        {
            GroupView group;
            group.surface=surface;
            for(const auto panel:node.tabs)document_.SetParent(Slot(panel).panel,SurfaceRoot(surface));
            group.tabs = node.tabs;
            group.active = node.active;
            groups_.push_back(std::move(group));
            RebuildTabBar(groups_.back());
            return;
        }
        SplitHandle handle;
        handle.surface = surface;
        handle.line = document_.CreateElement(UiElementKind::Panel);
        splitHandles_.push_back(std::move(handle));
        for (const DockNode& child : node.children) self(self, child);
    };
    walk(walk, dock_.Root());
    for(const auto& window:dock_.Windows()){surface=window.id;walk(walk,window.layout);}
    for (const auto& handle : splitHandles_)
        document_.SetParent(handle.line, SurfaceRoot(handle.surface));
    // 重新挂到末尾，保证拖影/落点提示绘制在 tab bar 之上。
    if (dropHighlight_.IsValid())
    {
        document_.SetParent(dropHighlight_, root_);
    }
    if(tabInsertMarker_.IsValid())document_.SetParent(tabInsertMarker_,root_);
    if(dragGhost_.IsValid())document_.SetParent(dragGhost_,root_);
}

void EditorShell::RebuildTabBar(GroupView& group)
{
    group.tabBar = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(group.tabBar, SurfaceRoot(group.surface)))
    {
        return;
    }
    for (const EditorPanelId panel : group.tabs)
    {
        const std::string label = PanelLabel(panel);
        const float width = TabWidth(label);
        const UiElementHandle button = document_.CreateElement(UiElementKind::Button);
        if (!document_.SetParent(button, group.tabBar))
        {
            continue;
        }
        auto tabStyle = ButtonStyle(0, 0, width, kTabHeight, kSurfaceRaised);
        tabStyle.padding.right = 20.0f;
        document_.SetStyle(button, tabStyle);
        document_.SetText(button, label);
        dispatcher_.AddHandler(button, UiEventType::PointerDown, [this, panel](UiInputEvent& event) {
            HandleTabDown(panel, event);
        });
        dispatcher_.AddHandler(button, UiEventType::PointerMove, [this](UiInputEvent& event) {
            HandleTabMove(event);
        });
        dispatcher_.AddHandler(button, UiEventType::PointerUp, [this](UiInputEvent& event) {
            HandleTabUp(event);
        });
        dispatcher_.AddHandler(button,UiEventType::PointerCaptureLost,[this](UiInputEvent&) {
            CancelTabDrag();
        });
        dispatcher_.AddHandler(button,UiEventType::FocusLost,[this](UiInputEvent&) {
            CancelTabDrag();
        });
        const UiElementHandle close = document_.CreateElement(UiElementKind::Button);
        if (document_.SetParent(close, group.tabBar))
        {
            document_.SetStyle(close, ButtonStyle(0, 2.0f, 16.0f, 20.0f, UiColor{0,0,0,0}));
            document_.SetText(close, "×");
            dispatcher_.AddHandler(close, UiEventType::PointerDown, [this, panel](UiInputEvent& event) {
                if (event.button != 0) return;
                TogglePanel(panel);
                event.handled = true;
            });
        }
        group.tabButtons.push_back(button);
        group.closeButtons.push_back(close);
        group.widths.push_back(width);
    }
    group.focusLine = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(group.focusLine, group.tabBar);
    auto indicator = AbsoluteStyle(0, 0, 1, 2);
    indicator.background = UiInk(flare::workspace::Focus);
    indicator.pointerEvents = UiPointerEvents::None;
    document_.SetStyle(group.focusLine, indicator);
    if (group.tabs.size() < 2) return;
    const auto addArrow = [&](int step) {
        const auto arrow = document_.CreateElement(UiElementKind::Button);
        document_.SetParent(arrow, group.tabBar);
        auto style = ButtonStyle(0, 0, kTabArrowWidth, kTabHeight, kSurfaceRaised);
        style.padding.left = 3.0f;
        style.padding.right = 2.0f;
        document_.SetStyle(arrow, style);
        document_.SetText(arrow, step < 0 ? "‹" : "›");
        dispatcher_.AddHandler(arrow, UiEventType::PointerDown,
            [this, member = group.tabs.front(), step](UiInputEvent& event) {
                if (event.button != 0) return;
                const DockNode* node = dock_.Find(member);
                if (!node) return;
                const auto current = std::find(node->tabs.begin(), node->tabs.end(), node->active);
                const auto n = static_cast<std::ptrdiff_t>(node->tabs.size());
                const auto index = (current - node->tabs.begin() + step + n) % n;
                SelectPanel(node->tabs[static_cast<std::size_t>(index)], true);
                event.handled = true;
            });
        return arrow;
    };
    group.previousTab = addArrow(-1);
    group.nextTab = addArrow(1);
}

void EditorShell::LayoutTabBar(GroupView& group)
{
    const UiRect rect = group.rect;
    UiStyle bar = AbsoluteStyle(rect.x, rect.y, rect.width, kTabHeight);
    bar.background = UiInk(flare::workspace::Header);
    bar.overflow = UiOverflow::Hidden;
    document_.SetStyle(group.tabBar, bar);
    const auto metrics = MeasureTabStrip(group.widths, rect.width);
    float x = 0;
    for (std::size_t i = 0; i < group.tabButtons.size(); ++i)
    {
        const bool active = group.tabs[i] == group.active;
        const bool visible = !metrics.paged || active;
        document_.SetVisible(group.tabButtons[i], visible);
        document_.SetVisible(group.closeButtons[i], visible && active);
        if (!visible) continue;
        const float width = metrics.paged ? metrics.width : group.widths[i] * metrics.scale;
        auto tab = document_.Style(group.tabButtons[i]);
        tab.left = UiLength::Px(x);
        tab.width = UiLength::Px(width);
        tab.padding.right = active ? 20.0f : 5.0f;
        const bool hovered = hoveredChrome_ == group.tabButtons[i] || hoveredChrome_ == group.closeButtons[i];
        tab.background = hovered && !tabDragging_ ? UiInk(flare::workspace::Hover) :
                         active ? kContent : bar.background;
        tab.color = active ? kText : kMuted;
        document_.SetStyle(group.tabButtons[i], tab);
        if (active)
        {
            auto close = document_.Style(group.closeButtons[i]);
            close.left = UiLength::Px(x + width - 18.0f);
            close.color = hoveredChrome_ == group.closeButtons[i] ? UiInk(flare::Heading) : kMuted;
            document_.SetStyle(group.closeButtons[i], close);
            auto line = document_.Style(group.focusLine);
            line.left = UiLength::Px(x);
            line.width = UiLength::Px(width);
            document_.SetStyle(group.focusLine, line);
        }
        x += width;
    }
    document_.SetVisible(group.focusLine, group.active != EditorPanelId::None && group.active == activePanel_);
    for (auto arrow : {group.previousTab, group.nextTab})
    {
        if (!arrow.IsValid()) continue;
        document_.SetVisible(arrow, metrics.paged);
        if (!metrics.paged) continue;
        auto style = document_.Style(arrow);
        style.left = UiLength::Px(metrics.width + (arrow == group.nextTab ? kTabArrowWidth : 0.0f));
        document_.SetStyle(arrow, style);
    }
}

void EditorShell::UpdateDockRects()
{
    dockRect_ = {0.0f, menuHeight_ + toolbarHeight_, width_,
                 std::max(0.0f, height_ - menuHeight_ - toolbarHeight_ - statusHeight_)};
    std::size_t groupIndex = 0, splitIndex = 0;
    std::vector<std::size_t> path;
    std::uint32_t surface=0;
    const auto walk = [this, &groupIndex, &splitIndex, &path,&surface](const auto& self, const DockNode& node,
                                                 const UiRect& rect) -> void {
        if (node.kind == DockNode::Kind::Tabs)
        {
            if (groupIndex >= groups_.size())
            {
                return;
            }
            GroupView& group = groups_[groupIndex++];
            group.rect = rect;
            group.active = node.active;
            if (group.tabBar.IsValid()) LayoutTabBar(group);
            for (const EditorPanelId panel : group.tabs)
            {
                PanelSlot& slot = Slot(panel);
                if (!slot.panel.IsValid())
                {
                    continue;
                }
                UiStyle style = PanelStyle({rect.x, rect.y + kTabHeight, rect.width,
                                            std::max(0.0f, rect.height - kTabHeight)});
                if (!viewportImage_.empty() && panel == EditorPanelId::Viewport)
                {
                    style.image = viewportImage_;
                }
                document_.SetStyle(slot.panel, style);
            }
            return;
        }
        if (node.children.size() != 2)
        {
            return;
        }
        const float total = node.axis == DockAxis::Horizontal ? rect.width : rect.height;
        const auto children = SplitDockRects(node, rect);
        const UiRect first=children[0],second=children[1];
        if (splitIndex < splitHandles_.size())
        {
            const std::size_t currentSplitIdx = splitIndex++;
            auto& handle = splitHandles_[currentSplitIdx];
            handle.path = path;
            handle.axis = node.axis;
            handle.ratio = node.ratio;
            handle.total = total;
            const bool horizontal = node.axis == DockAxis::Horizontal;
            handle.rect = horizontal ? UiRect{second.x - kSplitterReach, rect.y, 2 * kSplitterReach, rect.height}
                                     : UiRect{rect.x, second.y - kSplitterReach, rect.width, 2 * kSplitterReach};
            const bool active = splitterActive_ && splitterSurface_ == surface && splitterPath_ == path;
            const bool hovered = !splitterActive_ && hoveredSplitterIndex_ == static_cast<int>(currentSplitIdx);
            const bool highlighted = active || hovered;
            const float stroke = highlighted ? 2.0f : 1.0f;
            auto style = horizontal ? AbsoluteStyle(second.x - stroke * .5f, rect.y, stroke, rect.height)
                                    : AbsoluteStyle(rect.x, second.y - stroke * .5f, rect.width, stroke);
            style.background = highlighted ? UiInk(flare::workspace::Focus) : kBorder;
            style.pointerEvents = UiPointerEvents::None;
            document_.SetStyle(handle.line, style);
            document_.SetVisible(handle.line, highlighted);
        }
        path.push_back(0);
        self(self, node.children[0], first);
        path.pop_back();
        path.push_back(1);
        self(self, node.children[1], second);
        path.pop_back();
    };
    walk(walk, dock_.Root(), dockRect_);
    for(const auto& window:dock_.Windows()){surface=window.id;walk(walk,window.layout,SurfaceDockRect(surface));}
    std::array<bool, kPanelCount> visible{};
    for (const GroupView& group : groups_) visible[static_cast<std::size_t>(group.active)] = true;
    for (const PanelSlot& slot : slots_)
        if (slot.panel.IsValid()) document_.SetVisible(slot.panel, visible[static_cast<std::size_t>(slot.id)]);
    if (statusBar_.IsValid())
    {
        UiStyle style = document_.Style(statusBar_);
        style.top = UiLength::Px(height_ - statusHeight_);
        style.width = UiLength::Percent(100.0f);
        document_.SetStyle(statusBar_, style);
    }
}

void EditorShell::UpdatePanelContentRects()
{
    const auto setContent = [this](EditorPanelId panel, float top, float height) {
        PanelSlot& slot = Slot(panel);
        if (!slot.content.IsValid())
        {
            return;
        }
        UiStyle style = AbsoluteStyle(0.0f, top, 100.0f, height);
        style.width = UiLength::Percent(100.0f);
        style.height = UiLength::Px(std::max(0.0f, height));
        style.overflow = UiOverflow::Hidden;
        style.background = kContent;
        document_.SetStyle(slot.content, style);
    };
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
    const auto panelWidth = [this](EditorPanelId panel) {
        for (const GroupView& group : groups_)
        {
            if (std::find(group.tabs.begin(), group.tabs.end(), panel) != group.tabs.end())
            {
                return std::max(0.0f, group.rect.width);
            }
        }
        return 0.0f;
    };
    setContent(EditorPanelId::Hierarchy, 0.0f, panelHeight(EditorPanelId::Hierarchy));
    setContent(EditorPanelId::Console, 24.0f, std::max(0.0f, panelHeight(EditorPanelId::Console) - 24.0f));
    setContent(EditorPanelId::Inspector, 0.0f, panelHeight(EditorPanelId::Inspector));
    setContent(EditorPanelId::Assets, 24.0f, std::max(0.0f, panelHeight(EditorPanelId::Assets) -
        (assetsPreviewImage_.empty() ? 48.0f : 124.0f)));
    for (EditorPanelId panel : {EditorPanelId::UiDesigner, EditorPanelId::Material,
                                EditorPanelId::ProjectSettings, EditorPanelId::BuildExport,
                                EditorPanelId::Preferences, EditorPanelId::About, EditorPanelId::Diagnostics,EditorPanelId::AssetTools})
    {
        setContent(panel, 0.0f, panelHeight(panel));
    }
    UpdateFormRects(EditorPanelId::Material);
    UpdateFormRects(EditorPanelId::ProjectSettings);
    UpdateFormRects(EditorPanelId::BuildExport);
    UpdateFormRects(EditorPanelId::Preferences);
    UpdateFormRects(EditorPanelId::AssetTools);
    if (designerCanvas_.IsValid())
    {
        UiStyle style = document_.Style(designerCanvas_);
        style.left = UiLength::Px(0.0f);
        style.top = UiLength::Px(0.0f);
        style.width = UiLength::Percent(100.0f);
        style.height = UiLength::Percent(100.0f);
        document_.SetStyle(designerCanvas_, style);
    }
    const float assetsW = panelWidth(EditorPanelId::Assets);
    const bool showCreateBtn = assetsCreateElement_.IsValid() && assetsW >= 128.0f;
    if (assetsCreateElement_.IsValid()) document_.SetVisible(assetsCreateElement_, showCreateBtn);
    const bool showTypeFilter = assetsTypeElement_.IsValid() && assetsW >= 168.0f;
    if (assetsTypeElement_.IsValid())
    {
        document_.SetVisible(assetsTypeElement_, showTypeFilter);
        if (showTypeFilter)
        {
            const std::string& tf = assetsModel_.TypeFilter();
            std::string typeLabel = Text("EditorUi.FilterAll", "All");
            if (tf == "scene") typeLabel = Text("EditorUi.DocumentScene", "Scene");
            else if (tf == "model") typeLabel = Text("EditorUi.AssetModel", "Model");
            else if (tf == "material") typeLabel = Text("EditorUi.DocumentMaterial", "Material");
            else if (tf == "texture") typeLabel = Text("EditorUi.AssetTexture", "Texture");
            else if (tf == "prefab") typeLabel = Text("EditorUi.AssetPrefab", "Prefab");
            else if (tf == "C#") typeLabel = Text("EditorUi.AssetScript", "C#");
            else if (tf == "audio") typeLabel = Text("EditorUi.AssetAudio", "Audio");
            typeLabel += " ▾";
            UiStyle style = document_.Style(assetsTypeElement_);
            style.left = UiLength::Px(std::max(0.0f, assetsW - 104.0f));
            style.top = UiLength::Px(1.0f);
            style.width = UiLength::Px(52.0f);
            style.height = UiLength::Px(22.0f);
            style.background = !tf.empty() ? UiInk(flare::workspace::Selection) : kSurfaceRaised;
            style.borderWidth = 1.0f;
            style.borderColor = !tf.empty() ? UiInk(flare::workspace::Focus) : kBorder;
            document_.SetText(assetsTypeElement_, typeLabel);
            document_.SetStyle(assetsTypeElement_, style);
        }
    }
    if (assetsFilterElement_.IsValid())
    {
        UiStyle style = document_.Style(assetsFilterElement_);
        const bool focused = dispatcher_.Focus() == assetsFilterElement_;
        style.borderWidth = 1;
        style.borderColor = focused ? UiInk(flare::workspace::Focus) : kBorder;
        const float filterLeft = showCreateBtn ? 28.0f : 3.0f;
        const float reserveRight = showTypeFilter ? 107.0f : 52.0f;
        style.left = UiLength::Px(filterLeft);
        style.width = UiLength::Px(std::max(0.0f, assetsW - filterLeft - reserveRight));
        const bool placeholder = !focused && assetsFilter_->Text().empty();
        style.color = placeholder ? kMuted : kText;
        document_.SetText(assetsFilterElement_, placeholder ? Text("EditorUi.SearchAssets", "Search assets")
                                                            : assetsFilter_->DisplayText());
        document_.SetStyle(assetsFilterElement_, style);
    }
    if (assetsImportElement_.IsValid())
    {
        UiStyle style = document_.Style(assetsImportElement_);
        style.left = UiLength::Px(std::max(0.0f, assetsW - 49.0f));
        style.top = UiLength::Px(1.0f);
        style.width = UiLength::Px(46.0f);
        style.height = UiLength::Px(22.0f);
        document_.SetStyle(assetsImportElement_, style);
    }
    if (consoleClearButton_.IsValid())
    {
        const float consoleW = panelWidth(EditorPanelId::Console);
        const bool showClear = consoleW >= 120.0f;
        document_.SetVisible(consoleClearButton_, showClear);
        if (showClear)
        {
            UiStyle style = document_.Style(consoleClearButton_);
            style.left = UiLength::Px(std::max(4.0f, consoleW - 58.0f));
            style.top = UiLength::Px(2.0f);
            style.width = UiLength::Px(50.0f);
            style.height = UiLength::Px(20.0f);
            document_.SetStyle(consoleClearButton_, style);
        }
    }
    if (assetsPreviewImageElement_.IsValid())
    {
        UiStyle style = document_.Style(assetsPreviewImageElement_);
        style.top = UiLength::Px(std::max(0.0f, panelHeight(EditorPanelId::Assets) - 100.0f));
        const float scale=std::min(68.0f / std::max(1.0f,assetsPreviewImageHeight_),
            std::max(0.0f,panelWidth(EditorPanelId::Assets)-8.0f) / std::max(1.0f,assetsPreviewImageWidth_));
        style.width = UiLength::Px(assetsPreviewImageWidth_*scale);
        style.height = UiLength::Px(assetsPreviewImageHeight_*scale);
        document_.SetStyle(assetsPreviewImageElement_, style);
        document_.SetVisible(assetsPreviewImageElement_, !assetsPreviewImage_.empty());
    }
    if (assetsPreview_.IsValid())
    {
        UiStyle style = document_.Style(assetsPreview_);
        style.top = UiLength::Px(std::max(0.0f, panelHeight(EditorPanelId::Assets) - 24.0f));
        style.width = UiLength::Percent(100.0f);
        document_.SetStyle(assetsPreview_, style);
    }
    LayoutInspector(panelWidth(EditorPanelId::Inspector));
}

void EditorShell::MarkDockDirty(bool structural, bool save)
{
    if (structural)
    {
        ++dockRevision_;
    }
    if (save)
    {
        SaveLayoutIfConfigured();
    }
}

void EditorShell::SetLayoutPath(std::filesystem::path path)
{
    layoutPath_ = std::move(path);
    if (initialized_ && !layoutPath_.empty())
    {
        LoadLayout();
        Frame();
    }
}

bool EditorShell::LoadLayout()
{
    if (layoutPath_.empty())
    {
        return false;
    }
    EditorDock loaded;
    std::string error;
    if (EditorDockState::Load(layoutPath_, loaded, error))
    {
        dock_ = std::move(loaded);
        MarkDockDirty(true, false);
        SetStatus(Text("EditorUi.LayoutLoaded", "Layout loaded"));
        return true;
    }
    if (error != "dock layout not found")
    {
        SetStatus(Text("EditorUi.LayoutFallback", "Layout fallback") + ": " + error);
    }
    MarkDockDirty(true, false);
    return false;
}

bool EditorShell::SaveLayout()
{
    if (layoutPath_.empty())
    {
        return false;
    }
    std::string error;
    if (!EditorDockState::Save(dock_, layoutPath_, error))
    {
        SetStatus(Text("EditorUi.LayoutSaveFailed", "Layout save failed") + ": " + error);
        return false;
    }
    return true;
}

void EditorShell::SaveLayoutIfConfigured()
{
    if (!layoutPath_.empty())
    {
        SaveLayout();
    }
}

std::uint32_t EditorShell::FloatPanel(EditorPanelId panel,int x,int y,int width,int height)
{
    const auto id=dock_.FloatTab(panel,x,y,width,height);
    if(id)MarkDockDirty(true,true);
    return id;
}
void EditorShell::CloseFloatingWindow(std::uint32_t id)
{
    if (!dock_.CloseWindow(id)) return;
    if (!dock_.Find(activePanel_))
    {
        const auto* viewport = dock_.Find(EditorPanelId::Viewport);
        SetActivePanel(viewport ? viewport->active : EditorPanelId::None);
    }
    MarkDockDirty(true,true);
}
void EditorShell::SetFloatingBounds(std::uint32_t id,int x,int y,int width,int height)
{if(dock_.SetWindowBounds(id,x,y,width,height))SaveLayoutIfConfigured();}
void EditorShell::SetSurfaceSize(std::uint32_t id,float width,float height)
{
    for(auto& surface:surfaces_)if(surface.id==id)
    {
        surface.width=std::max(160.0f,width);surface.height=std::max(120.0f,height);
        auto style=document_.Style(surface.root);style.width=UiLength::Px(surface.width);style.height=UiLength::Px(surface.height);
        document_.SetStyle(surface.root,style);return;
    }
}
void EditorShell::SetInputSurface(std::uint32_t id)
{inputSurface_=id;dispatcher_.SetInputRoot(SurfaceRoot(id));}
UiElementHandle EditorShell::SurfaceRoot(std::uint32_t id) const
{
    if(!id)return root_;
    for(const auto& surface:surfaces_)if(surface.id==id)return surface.root;
    return {};
}
std::uint32_t EditorShell::SurfaceForElement(UiElementHandle element)const
{
    while(const auto* node=document_.Element(element))
    {
        if(element==root_)return 0;
        for(const auto& surface:surfaces_)if(element==surface.root)return surface.id;
        element=node->Parent();
    }
    return 0;
}
UiRect EditorShell::SurfaceDockRect(std::uint32_t id) const
{
    if(!id)return dockRect_;
    for(const auto& surface:surfaces_)if(surface.id==id)return {0,0,surface.width,surface.height};
    return {};
}
std::string EditorShell::WindowLabel(std::uint32_t id) const
{
    const auto* tree=dock_.Surface(id);if(!tree)return {};
    while(tree->kind==DockNode::Kind::Split&&!tree->children.empty())tree=&tree->children.front();
    return PanelLabel(tree->active);
}

void EditorShell::HandleTabDown(EditorPanelId panel, UiInputEvent& event)
{
    if(event.button!=0)return;
    dragTabPanel_ = panel;
    tabDragging_ = false;
    tabDragStartX_ = event.x;
    tabDragStartY_ = event.y;
    const auto* current = dock_.Find(panel);
    SelectPanel(panel, current && current->active != panel);
    event.handled = true;
    event.StopPropagation();
}

EditorShell::DockZone EditorShell::ContentDropZone(const UiRect& group, float x, float y, DockZone previous)
{
    constexpr float slack = 8.0f;
    const float contentTop = group.y + kTabHeight;
    const float contentHeight = std::max(0.0f, group.height - kTabHeight);
    const float sideBand = std::clamp(group.width * 0.22f, 24.0f, 64.0f);
    const float endBand = std::clamp(contentHeight * 0.22f, 24.0f, 64.0f);
    const bool canSplitWidth = group.width >= 2 * kMinimumGroupSize;
    const bool canSplitHeight = group.height >= 2 * kMinimumGroupSize;
    struct Edge { DockZone zone; float distance, band; bool allowed; };
    const Edge edges[] = {
        {DockZone::Left, x - group.x, sideBand, canSplitWidth},
        {DockZone::Right, group.x + group.width - x, sideBand, canSplitWidth},
        {DockZone::Top, y - contentTop, endBand, canSplitHeight},
        {DockZone::Bottom, group.y + group.height - y, endBand, canSplitHeight},
    };
    // 给当前落点保留一小段移动余量；角落轻微抖动不会反复在两种分屏之间跳转。
    const Edge* best = nullptr;
    float nearest = 0;
    for (const auto& edge : edges)
    {
        const float distance = edge.distance - (previous == edge.zone ? slack : 0.0f);
        if (edge.allowed && distance < edge.band && (!best || distance < nearest))
        {
            best = &edge;
            nearest = distance;
        }
    }
    return best ? best->zone : DockZone::Merge;
}

void EditorShell::HandleTabMove(UiInputEvent& event)
{
    if (dragTabPanel_ == EditorPanelId::None)
    {
        return;
    }
    event.handled=true;
    event.StopPropagation();
    const float dx=event.x-tabDragStartX_,dy=event.y-tabDragStartY_;
    if (!tabDragging_ && (inputSurface_ != dock_.SurfaceFor(dragTabPanel_) || dx*dx+dy*dy>36.0f))
    {
        tabDragging_ = true;
        if (dragGhost_.IsValid())
        {
            document_.SetVisible(dragGhost_, true);
            document_.SetText(dragGhost_, PanelLabel(dragTabPanel_));
        }
    }
    if (!tabDragging_)
    {
        return;
    }
    const auto previousGroup = dropGroup_;
    const auto previousZone = dropZone_;
    const bool wasOnTabs = dropOnTabBar_;
    dropGroup_ = EditorPanelId::None;
    dropOnTabBar_ = false;
    dropTarget_=EditorPanelId::None;
    dropZone_=DockZone::Merge;
    dropBefore_=false;
    for (std::size_t index = 0; index < groups_.size(); ++index)
    {
        const auto& group=groups_[index];
        if(group.surface!=inputSurface_)continue;
        const UiRect rect = group.rect;
        if (event.x < rect.x || event.x >= rect.x + rect.width ||
            event.y < rect.y || event.y >= rect.y + rect.height)
        {
            continue;
        }
        if(group.tabs.empty()&&!inputSurface_){dropTarget_=dragTabPanel_;break;}
        if (group.tabs.empty()) break;
        dropGroup_ = group.tabs.front();
        const bool sameGroup = dropGroup_ == previousGroup;
        const float tabBoundary = rect.y + kTabHeight;
        const float tolerance = !sameGroup ? 0.0f : (wasOnTabs ? 4.0f : -4.0f);
        dropOnTabBar_ = event.y < tabBoundary + tolerance;
        const bool own=std::find(group.tabs.begin(),group.tabs.end(),dragTabPanel_)!=group.tabs.end();
        if(own && group.tabs.size()==1)break;
        if(dropOnTabBar_)
        {
            for(size_t tab=0;tab<group.tabs.size();++tab)
            {
                if(group.tabs[tab]==dragTabPanel_ || !document_.Visible(group.tabButtons[tab]))continue;
                dropTarget_=group.tabs[tab];
                const auto button=layout_.Rect(group.tabButtons[tab]);
                dropBefore_=event.x<button.x+button.width*0.5f;
                if(dropBefore_)break;
            }
        }
        else
        {
            dropZone_ = ContentDropZone(rect, event.x, event.y,
                sameGroup && !wasOnTabs ? previousZone : DockZone::Merge);
            if(own && dropZone_==DockZone::Merge)break;
            for(const auto panel:group.tabs)
                if(panel!=dragTabPanel_)dropTarget_=panel;
        }
        break;
    }

    EditorDock preview=dock_;
    const bool valid=ApplyTabDrop(preview) && preview!=dock_;
    if(!valid)dropTarget_=EditorPanelId::None;
    const auto targetRoot=SurfaceRoot(inputSurface_);
    if(targetRoot.IsValid()){document_.SetParent(dropHighlight_,targetRoot);document_.SetParent(tabInsertMarker_,targetRoot);document_.SetParent(dragGhost_,targetRoot);}
    document_.SetVisible(dragGhost_,targetRoot.IsValid());
    document_.SetVisible(dropHighlight_,valid);
    document_.SetVisible(tabInsertMarker_,valid && dropZone_==DockZone::Merge);
    if(valid)
    {
        // 和提交使用同一树变换，再按真实分隔比例计算落点，覆盖空组折叠后的面积。
        const UiRect rect=DockPanelRect(*preview.Surface(inputSurface_),dragTabPanel_,SurfaceDockRect(inputSurface_));
        UiStyle style=document_.Style(dropHighlight_);
        style.left=UiLength::Px(rect.x);style.top=UiLength::Px(rect.y);
        style.width=UiLength::Px(rect.width);style.height=UiLength::Px(rect.height);
        document_.SetStyle(dropHighlight_,style);
        if(dropZone_==DockZone::Merge)
        {
            float x=rect.x;
            const auto& tabs = preview.Find(dragTabPanel_)->tabs;
            std::vector<float> widths;
            for (auto panel : tabs) widths.push_back(TabWidth(PanelLabel(panel)));
            const auto strip = MeasureTabStrip(widths, rect.width);
            if (!strip.paged)
                for (std::size_t i = 0; tabs[i] != dragTabPanel_; ++i)
                    x += widths[i] * strip.scale;
            UiStyle marker=document_.Style(tabInsertMarker_);
            marker.left=UiLength::Px(x);marker.top=UiLength::Px(rect.y+2);
            marker.height=UiLength::Px(kTabHeight-4);
            document_.SetStyle(tabInsertMarker_,marker);
        }
    }
    if(dragGhost_.IsValid())
    {
        const char* keys[]={"EditorUi.DockMerge","EditorUi.DockLeft","EditorUi.DockRight","EditorUi.DockTop","EditorUi.DockBottom"};
        const char* labels[]={"Merge as tab","Dock left","Dock right","Dock above","Dock below"};
        const auto zone = static_cast<std::size_t>(dropZone_);
        const std::string label=PanelLabel(dragTabPanel_)+(valid?" · "+Text(keys[zone],labels[zone]):"");
        document_.SetText(dragGhost_,label);
        UiStyle style=document_.Style(dragGhost_);
        const auto area=SurfaceDockRect(inputSurface_);
        const float surfaceWidth=std::max(1.0f,area.width),surfaceHeight=std::max(24.0f,area.y+area.height);
        const float width=std::min(surfaceWidth,MeasuredWidth(label,13.0f)+16.0f);
        style.width=UiLength::Px(width);
        style.left=UiLength::Px(std::clamp(event.x+10,0.0f,surfaceWidth-width));
        style.top=UiLength::Px(std::clamp(event.y+8,0.0f,surfaceHeight-24.0f));
        document_.SetStyle(dragGhost_,style);
    }
}

void EditorShell::HandleTabUp(UiInputEvent& event)
{
    if(dragTabPanel_==EditorPanelId::None || event.button!=0)return;
    if (tabDragging_)
    {
        HandleTabMove(event);
        if(inputSurface_==UINT32_MAX&&floatRequest_)floatRequest_(dragTabPanel_);
        else ApplyTabDrop();
    }
    event.handled=true;
    event.StopPropagation();
    CancelTabDrag();
}

void EditorShell::CancelTabDrag()
{
    const bool tabPressed=dragTabPanel_!=EditorPanelId::None;
    dragTabPanel_ = EditorPanelId::None;
    tabDragging_ = false;
    dropTarget_=EditorPanelId::None;
    dropGroup_ = EditorPanelId::None;
    dropZone_ = DockZone::Merge;
    dropOnTabBar_ = false;
    document_.SetVisible(tabInsertMarker_,false);
    if (dragGhost_.IsValid())
    {
        document_.SetVisible(dragGhost_, false);
    }
    if (dropHighlight_.IsValid())
    {
        document_.SetVisible(dropHighlight_, false);
    }
    if(tabPressed && dispatcher_.PointerCapture().IsValid())dispatcher_.ReleasePointerCapture();
}

bool EditorShell::ApplyTabDrop(EditorDock& dock) const
{
    if(dragTabPanel_==EditorPanelId::None || dropTarget_==EditorPanelId::None)return false;
    if(!inputSurface_&&dock.Root().kind==DockNode::Kind::Tabs&&dock.Root().tabs.empty())
    {return dock.CloseTab(dragTabPanel_)&&dock.OpenTab(dragTabPanel_);}
    if(dropZone_==DockZone::Merge)return dock.MoveTab(dragTabPanel_,dropTarget_,dropBefore_);
    const bool horizontal = dropZone_ == DockZone::Left || dropZone_ == DockZone::Right;
    const bool before = dropZone_ == DockZone::Left || dropZone_ == DockZone::Top;
    return dock.SplitTab(dragTabPanel_,dropTarget_,horizontal ? DockAxis::Horizontal : DockAxis::Vertical,before);
}

void EditorShell::ApplyTabDrop()
{
    EditorDock next=dock_;
    if(ApplyTabDrop(next) && next!=dock_)
    {
        dock_=std::move(next);
        SetActivePanel(dragTabPanel_);
        MarkDockDirty(true, true);
    }
}

const EditorShell::SplitHandle* EditorShell::FindSplitter(float x, float y) const
{
    const SplitHandle* closest = nullptr;
    float nearest = kSplitterReach + 1;
    for (const SplitHandle& handle : splitHandles_)
    {
        if (handle.surface != inputSurface_) continue;
        if (x < handle.rect.x || x >= handle.rect.x + handle.rect.width ||
            y < handle.rect.y || y >= handle.rect.y + handle.rect.height)
            continue;
        const float distance = handle.axis == DockAxis::Horizontal
            ? std::abs(x - handle.rect.x - kSplitterReach)
            : std::abs(y - handle.rect.y - kSplitterReach);
        if (distance < nearest)
        {
            closest = &handle;
            nearest = distance;
        }
    }
    return closest;
}

std::optional<DockAxis> EditorShell::SplitterCursorAt(float x, float y) const
{
    if (splitterActive_) return splitterAxis_;
    if (dragTabPanel_ != EditorPanelId::None || closeModalOpen_ || confirmOpen_ || MenuOpen() || ColorPickerOpen())
        return std::nullopt;
    if (const auto* handle = FindSplitter(x, y)) return handle->axis;
    return std::nullopt;
}

void EditorShell::BeginSplitter(float x, float y)
{
    const auto* handle = FindSplitter(x, y);
    if (!handle || splitterActive_) return;
    // 捕获在窗口根上。缩放时字段、滚动列表和页签都可能重建。
    dispatcher_.SetPointerCapture(SurfaceRoot(inputSurface_));
    splitterSurface_ = handle->surface;
    splitterPath_ = handle->path;
    splitterAxis_ = handle->axis;
    splitterOriginalRatio_ = handle->ratio;
    splitterTotal_ = std::max(1.0f, handle->total);
    const float minimum = std::min(0.45f, kMinimumGroupSize / splitterTotal_);
    splitterStartRatio_ = std::clamp(handle->ratio, minimum, 1.0f - minimum);
    splitterStartCoordinate_ = handle->axis == DockAxis::Horizontal ? x : y;
    splitterActive_ = true;
}

void EditorShell::DragSplitter(float x, float y)
{
    if (!splitterActive_)
    {
        return;
    }
    const float coordinate = splitterAxis_ == DockAxis::Horizontal ? x : y;
    const float minimum = std::min(0.45f, kMinimumGroupSize / splitterTotal_);
    float ratio = splitterStartRatio_ + (coordinate - splitterStartCoordinate_) / splitterTotal_;
    ratio = std::clamp(ratio, minimum, 1.0f - minimum);
    if (dock_.SetSplitRatio(splitterSurface_,splitterPath_, ratio) && initialized_)
    {
        UpdateDockRects();
        UpdatePanelContentRects();
    }
}

void EditorShell::EndSplitter(bool commit)
{
    splitterActive_ = false;
    hoveredSplitterIndex_ = -1;
    if (commit)
        SaveLayoutIfConfigured();
    else
        dock_.SetSplitRatio(splitterSurface_, splitterPath_, splitterOriginalRatio_);
    if (dispatcher_.PointerCapture() == SurfaceRoot(splitterSurface_))
        dispatcher_.ReleasePointerCapture();
}

} // namespace se::ui::editor
