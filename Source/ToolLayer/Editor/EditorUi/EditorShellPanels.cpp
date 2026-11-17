#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/EditorUi/EditorShellInternal.h"
#include "ToolLayer/Editor/EditorUi/EditorInk.h"
#include "CoreLayer/Utf.h"
#include "ToolLayer/Editor/FlarePalette.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace se::ui::editor
{
using namespace detail;

void EditorShell::BuildPanelContent(PanelSlot& slot)
{
    const auto makeList = [this, &slot](std::unique_ptr<UiListView>& list) {
        list = std::make_unique<UiListView>(document_, slot.content);
        list->SetRowTextColor(kText);
        list->SetRowTextLayout(UiTextFlow::SingleLineEllipsis, {6, 0, 6, 0});
        list->SetRowHeight(20.0f);
    };

    switch (slot.id)
    {
    case EditorPanelId::Hierarchy:
        makeList(hierarchyList_);
        if (hierarchyList_)
        {
            hierarchyList_->SetRowBackgroundProvider([this](int index) {
                const auto rows = hierarchyModel_.VisibleRows();
                return index >= 0 && static_cast<std::size_t>(index) < rows.size() &&
                    hierarchyModel_.Nodes()[rows[index]].id == inspectorModel_.Target()
                    ? UiInk(activePanel_ == EditorPanelId::Hierarchy ? flare::workspace::Selection
                                                                     : flare::workspace::InactiveSelection)
                    : UiColor{0, 0, 0, 0};
            });
            hierarchyList_->SetRowTextProvider([this](int index) {
                const auto rows = hierarchyModel_.VisibleRows();
                if (index < 0 || static_cast<std::size_t>(index) >= rows.size())
                {
                    return std::string{};
                }
                const EditorHierarchyNode& node =
                    hierarchyModel_.Nodes()[rows[static_cast<std::size_t>(index)]];
                return std::string(node.depth * 2, ' ') + node.label;
            });
        }
        dispatcher_.AddHandler(slot.content, UiEventType::PointerDown, [this](UiInputEvent& event) {
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
            const float offset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
            const int row = static_cast<int>((event.y - rect.y + offset) / 20.0f);
            const auto rows = hierarchyModel_.VisibleRows();
            if (event.button == 1)
            {
                const auto parent = row >= 0 && static_cast<std::size_t>(row) < rows.size()
                    ? hierarchyModel_.Nodes()[rows[row]].id : 0;
                if (parent && selectionHandler_) selectionHandler_(parent);
                OpenCreateMenu(EditorPanelId::Hierarchy, event.x, event.y, parent);
                event.handled = true;
                return;
            }
            if (event.button != 0) return;
            if (row >= 0 && static_cast<std::size_t>(row) < rows.size())
            {
                const std::size_t nodeIndex = rows[static_cast<std::size_t>(row)];
                dragHierarchyId_ = hierarchyModel_.Nodes()[nodeIndex].id;
                hierarchyDragStartX_ = event.x;
                hierarchyDragStartY_ = event.y;
                draggingHierarchy_ = false;
                if (event.x - rect.x <= 18.0f && hierarchyModel_.Nodes()[nodeIndex].hasChildren)
                {
                    hierarchyModel_.Toggle(nodeIndex);
                    if (hierarchyList_)
                    {
                        hierarchyList_->Invalidate();
                        hierarchyList_->SetItemCount(static_cast<int>(hierarchyModel_.VisibleRows().size()));
                    }
                    Frame();
                    event.handled = true;
                    return;
                }
                const std::uint64_t id = hierarchyModel_.Nodes()[nodeIndex].id;
                hierarchyModel_.Select(id);
                if (selectionHandler_) selectionHandler_(id);
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerMove, [this](UiInputEvent& event) {
            if (dragHierarchyId_ == 0) return;
            if (std::abs(event.x - hierarchyDragStartX_) + std::abs(event.y - hierarchyDragStartY_) > 4.0f)
            {
                draggingHierarchy_ = true;
                if (dragGhost_.IsValid())
                {
                    document_.SetVisible(dragGhost_, true);
                    UiStyle ghostStyle = document_.Style(dragGhost_);
                    ghostStyle.left = UiLength::Px(event.x + 8.0f);
                    ghostStyle.top = UiLength::Px(event.y);
                    document_.SetStyle(dragGhost_, ghostStyle);
                    for (const EditorHierarchyNode& node : hierarchyModel_.Nodes())
                    {
                        if (node.id == dragHierarchyId_)
                        {
                            document_.SetText(dragGhost_, node.label);
                        }
                    }
                }
            }
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
            if (hierarchyList_)
            {
                hierarchyList_->SetScrollOffset(hierarchyList_->Offset() + event.wheel * 48.0f);
                hierarchyList_->Sync();
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerUp, [this](UiInputEvent& event) {
            if (dragHierarchyId_ == 0 || !draggingHierarchy_)
            {
                dragHierarchyId_ = 0;
                draggingHierarchy_ = false;
                return;
            }
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
            const float offset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
            const int row = static_cast<int>((event.y - rect.y + offset) / 20.0f);
            const auto rows = hierarchyModel_.VisibleRows();
            if (row >= 0 && static_cast<std::size_t>(row) < rows.size())
            {
                const std::uint64_t parent = hierarchyModel_.Nodes()[rows[static_cast<std::size_t>(row)]].id;
                if (parent != dragHierarchyId_ && reparentHandler_)
                {
                    reparentHandler_(dragHierarchyId_, parent);
                }
            }
            if (dragGhost_.IsValid()) document_.SetVisible(dragGhost_, false);
            dragHierarchyId_ = 0;
            draggingHierarchy_ = false;
        }, UiEventPhase::Bubble);
        break;
    case EditorPanelId::Assets: {
        makeList(assetsList_);
        if (assetsList_)
        {
            assetsList_->SetRowBackgroundProvider([this](int index) {
                const auto& entries = assetsModel_.Filtered();
                if (index < 0 || static_cast<std::size_t>(index) >= entries.size() ||
                    !assetsModel_.Selected(entries[index].path)) return UiColor{0, 0, 0, 0};
                return UiInk(activePanel_ == EditorPanelId::Assets ? flare::workspace::Selection
                                                                   : flare::workspace::InactiveSelection);
            });
            assetsList_->SetRowTextProvider([this](int index) {
                const auto& entries = assetsModel_.Filtered();
                if (index < 0 || static_cast<std::size_t>(index) >= entries.size())
                {
                    return std::string{};
                }
                const EditorAssetEntry& entry = entries[static_cast<std::size_t>(index)];
                const auto separator = entry.path.find_last_of("/\\");
                return entry.path.substr(separator == std::string::npos ? 0 : separator + 1);
            });
        }
        assetsFilterElement_ = document_.CreateElement(UiElementKind::TextEdit);
        if (document_.SetParent(assetsFilterElement_, slot.panel))
        {
            document_.SetStyle(assetsFilterElement_, ButtonStyle(3.0f, 2.0f, 100.0f, 20.0f, kSurface));
            assetsFilter_ = std::make_unique<UiTextEdit>(document_, assetsFilterElement_, text_);
            dispatcher_.AddHandler(assetsFilterElement_, UiEventType::PointerDown, [this](UiInputEvent&) {
                dispatcher_.SetFocus(assetsFilterElement_);
            });
            const auto refreshFilter = [this]() {
                if (!assetsFilter_) return;
                assetsModel_.SetFilter(assetsFilter_->Text());
                if (assetsList_)
                {
                    assetsList_->Invalidate();
                    assetsList_->SetItemCount(static_cast<int>(assetsModel_.Filtered().size()));
                }
            };
            dispatcher_.AddHandler(assetsFilterElement_, UiEventType::KeyDown, [this, refreshFilter](UiInputEvent& event) {
                if (event.key == UiKeyEnter || event.key == UiKeyEscape)
                {
                    dispatcher_.ClearFocus();
                    event.handled = true;
                    return;
                }
                if (assetsFilter_) assetsFilter_->HandleKey(event.key, event.shift, event.control);
                refreshFilter();
                event.handled = true;
            });
            dispatcher_.AddHandler(assetsFilterElement_, UiEventType::TextInput,
                                   [this, refreshFilter](UiInputEvent& event) {
                if (!assetsFilter_ || event.codepoint == 0) return;
                const char32_t codepoint = event.codepoint;
                std::string utf8;
                if (!EncodeUtf8({&codepoint, 1}, utf8)) return;
                assetsFilter_->InsertText(utf8);
                refreshFilter();
                event.handled = true;
            });
            dispatcher_.AddHandler(assetsFilterElement_, UiEventType::ImeComposition, [this](UiInputEvent& event) {
                assetsFilter_->SetComposition(event.text);
                event.handled = true;
            });
            dispatcher_.AddHandler(assetsFilterElement_, UiEventType::ImeCommit, [this, refreshFilter](UiInputEvent& event) {
                assetsFilter_->SetComposition(event.text);
                assetsFilter_->CommitComposition();
                refreshFilter();
                event.handled = true;
            });
            dispatcher_.AddHandler(assetsFilterElement_, UiEventType::ImeCancel, [this](UiInputEvent&) {
                assetsFilter_->CancelComposition();
            });
        }
        assetsImportElement_ = document_.CreateElement(UiElementKind::Button);
        if (document_.SetParent(assetsImportElement_, slot.panel))
        {
            document_.SetStyle(assetsImportElement_, ButtonStyle(0.0f, 0.0f, 64.0f, 24.0f, kSurfaceActive));
            document_.SetText(assetsImportElement_, Text("EditorUi.Import", "Import"));
            dispatcher_.AddHandler(assetsImportElement_, UiEventType::PointerDown, [this](UiInputEvent& event) {
                if (event.button != 0) return;
                if (assetImportHandler_) assetImportHandler_();
                event.handled = true;
            });
        }
        assetsPreview_ = document_.CreateElement(UiElementKind::Label);
        if (document_.SetParent(assetsPreview_, slot.panel))
        {
            document_.SetStyle(assetsPreview_, ButtonStyle(0.0f, 0.0f, 100.0f, 24.0f, kSurfaceRaised));
            document_.SetText(assetsPreview_, Text("EditorUi.Preview", "Preview") + ": " + Text("EditorUi.None", "none"));
        }
        assetsPreviewImageElement_ = document_.CreateElement(UiElementKind::Image);
        if (document_.SetParent(assetsPreviewImageElement_, slot.panel))
        {
            UiStyle style;
            style.position = UiPosition::Absolute;
            style.left = UiLength::Px(4.0f);
            style.top = UiLength::Px(4.0f);
            style.width = UiLength::Px(120.0f);
            style.height = UiLength::Px(64.0f);
            document_.SetStyle(assetsPreviewImageElement_, style);
            document_.SetVisible(assetsPreviewImageElement_, false);
        }
        dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
            if (assetsList_)
            {
                assetsList_->SetScrollOffset(assetsList_->Offset() + event.wheel * 48.0f);
                assetsList_->Sync();
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerDown, [this](UiInputEvent& event) {
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Assets).content);
            const float offset = assetsList_ ? assetsList_->Offset() : 0.0f;
            const int row = static_cast<int>((event.y - rect.y + offset) / 20.0f);
            const auto& entries = assetsModel_.Filtered();
            if (event.button != 0 && event.button != 1) return;
            if (row >= 0 && static_cast<std::size_t>(row) < entries.size())
            {
                SelectAsset(entries[static_cast<std::size_t>(row)].path);
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.panel, UiEventType::PointerDown, [this](UiInputEvent& event) {
            if (event.button != 1) return;
            OpenCreateMenu(EditorPanelId::Assets, event.x, event.y);
            event.handled = true;
        }, UiEventPhase::Bubble);
        break;
    }
    case EditorPanelId::Inspector:
        BuildInspector(slot);
        break;
    case EditorPanelId::Console: {
        makeList(console_);
        if (console_)
        {
            console_->SetRowTextProvider([this](int index) {
                return index >= 0 && static_cast<std::size_t>(index) < lines_.size()
                    ? lines_[static_cast<std::size_t>(index)] : std::string{};
            });
            console_->SetItemCount(0);
        }
        dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
            if (console_)
            {
                console_->SetScrollOffset(console_->Offset() + event.wheel * 48.0f);
                console_->Sync();
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        break;
    }
    case EditorPanelId::UiDesigner:
        BuildDesignerPanel(slot);
        break;
    case EditorPanelId::Material:
    case EditorPanelId::ProjectSettings:
    case EditorPanelId::BuildExport:
    case EditorPanelId::Preferences:
    case EditorPanelId::AssetTools:
        BuildFormPanel(slot);
        break;
    case EditorPanelId::About:
        document_.SetText(slot.content, Text("EditorUi.AboutPlaceholder", "Flare Engine Editor"));
        break;
    case EditorPanelId::Diagnostics:
        document_.SetText(slot.content, Text("EditorUi.DiagnosticsPlaceholder", "Diagnostics pending"));
        break;
    default:
        break;
    }
    BuildScrollThumb(slot.id);
}

void EditorShell::BuildScrollThumb(EditorPanelId panel)
{
    const bool hasList = panel == EditorPanelId::Hierarchy || panel == EditorPanelId::Assets ||
                         panel == EditorPanelId::Inspector || panel == EditorPanelId::Console ||
                         panel == EditorPanelId::Material || panel == EditorPanelId::ProjectSettings ||
                         panel == EditorPanelId::BuildExport || panel == EditorPanelId::Preferences;
    if (!hasList) return;
    UiElementHandle thumb = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(thumb, Slot(panel).content)) return;
    UiStyle style;
    style.position = UiPosition::Absolute;
    style.left = UiLength::Px(0.0f);
    style.top = UiLength::Px(0.0f);
    style.width = UiLength::Px(4.0f);
    style.height = UiLength::Px(20.0f);
    style.background = UiInk(flare::Muted, 0.85f);
    style.borderColor = kBorder;
    style.borderWidth = 1.0f;
    document_.SetStyle(thumb, style);
    document_.SetVisible(thumb, false);
    scrollThumbs_[static_cast<std::size_t>(panel)] = thumb;

    dispatcher_.AddHandler(thumb, UiEventType::PointerDown, [this, panel](UiInputEvent& event) {
        UiListView* list = ListFor(panel);
        if (!list) return;
        scrollDragging_ = true;
        scrollDragPanel_ = panel;
        scrollDragStartY_ = event.y;
        scrollDragStartOffset_ = list->Offset();
        dispatcher_.SetPointerCapture(scrollThumbs_[static_cast<std::size_t>(panel)]);
        event.handled = true;
    });
    dispatcher_.AddHandler(thumb, UiEventType::PointerMove, [this](UiInputEvent& event) {
        if (!scrollDragging_) return;
        UiListView* list = ListFor(scrollDragPanel_);
        if (!list) return;
        const float viewport = list->ViewportHeight();
        const float content = static_cast<float>(list->ItemCount()) * list->RowHeight();
        const float thumbHeight = std::max(20.0f, viewport * viewport / std::max(content, 1.0f));
        const float track = std::max(1.0f, viewport - thumbHeight);
        const float maximumOffset = std::max(0.0f, content - viewport);
        const float offset = scrollDragStartOffset_ + (event.y - scrollDragStartY_) / track * maximumOffset;
        list->SetScrollOffset(offset);
        list->Sync();
        event.handled = true;
    });
    dispatcher_.AddHandler(thumb, UiEventType::PointerUp, [this](UiInputEvent& event) {
        scrollDragging_ = false;
        scrollDragPanel_ = EditorPanelId::None;
        dispatcher_.ReleasePointerCapture();
        event.handled = true;
    });
}

UiListView* EditorShell::ListFor(EditorPanelId panel) const
{
    switch (panel)
    {
    case EditorPanelId::Hierarchy: return hierarchyList_.get();
    case EditorPanelId::Assets: return assetsList_.get();
    case EditorPanelId::Inspector: return inspectorList_.get();
    case EditorPanelId::Console: return console_.get();
    case EditorPanelId::Material: return forms_[0]->list.get();
    case EditorPanelId::ProjectSettings: return forms_[1]->list.get();
    case EditorPanelId::BuildExport: return forms_[2]->list.get();
    case EditorPanelId::Preferences: return forms_[3]->list.get();
    case EditorPanelId::AssetTools: return forms_[4]->list.get();
    default: return nullptr;
    }
}

void EditorShell::UpdateScrollThumbs()
{
    for (const EditorPanelId panel : {EditorPanelId::Hierarchy, EditorPanelId::Assets, EditorPanelId::Inspector,
                                      EditorPanelId::Console, EditorPanelId::Material,
                                      EditorPanelId::ProjectSettings, EditorPanelId::BuildExport,
                                      EditorPanelId::Preferences})
    {
        UiElementHandle thumb = scrollThumbs_[static_cast<std::size_t>(panel)];
        UiListView* list = ListFor(panel);
        if (!thumb.IsValid() || !list) continue;
        const float viewport = list->ViewportHeight();
        const float content = static_cast<float>(list->ItemCount()) * list->RowHeight();
        if (viewport <= 1.0f || content <= viewport + 1.0f)
        {
            if (document_.Visible(thumb)) document_.SetVisible(thumb, false);
            continue;
        }
        const float maximumOffset = content - viewport;
        const float thumbHeight = std::max(20.0f, viewport * viewport / content);
        const float progress = std::clamp(list->Offset() / maximumOffset, 0.0f, 1.0f);
        const UiRect contentRect = layout_.Rect(Slot(panel).content);
        const float left = std::max(0.0f, contentRect.width - 6.0f);
        const float top = progress * std::max(0.0f, viewport - thumbHeight);
        UiStyle style = document_.Style(thumb);
        // 只在几何变化时 SetStyle，避免空闲帧把 layout 标脏（UiPerf 要求 stable layout=0）。
        if (style.left.value != left || style.top.value != top || style.height.value != thumbHeight ||
            style.left.unit != UiUnit::Px || style.top.unit != UiUnit::Px || style.height.unit != UiUnit::Px)
        {
            style.left = UiLength::Px(left);
            style.top = UiLength::Px(top);
            style.height = UiLength::Px(thumbHeight);
            document_.SetStyle(thumb, style);
        }
        if (!document_.Visible(thumb)) document_.SetVisible(thumb, true);
    }
}

void EditorShell::SetHierarchy(std::vector<EditorHierarchyNode> nodes)
{
    // EditorApplication 每帧重建同样的列表；内容未变时不要销毁行元素，
    // 否则新行在当帧没有布局矩形，会被画成零尺寸。
    if (nodes == hierarchyModel_.Nodes()) return;
    hierarchyModel_.SetNodes(std::move(nodes));
    if (hierarchyList_)
    {
        hierarchyList_->Invalidate();
        hierarchyList_->SetItemCount(static_cast<int>(hierarchyModel_.VisibleRows().size()));
        hierarchyList_->Sync();
    }
}

void EditorShell::SetAssets(std::vector<EditorAssetEntry> entries)
{
    if (entries == assetsModel_.Entries()) return;
    assetsModel_.SetEntries(std::move(entries));
    if (assetsList_)
    {
        assetsList_->Invalidate();
        assetsList_->SetItemCount(static_cast<int>(assetsModel_.Filtered().size()));
        assetsList_->Sync();
    }
}

void EditorShell::SelectAsset(const std::string& path)
{
    assetsModel_.Select(path);
    if (assetsList_) { assetsList_->Invalidate(); assetsList_->Sync(); }
    if (assetsPreview_.IsValid())
    {
        document_.SetText(assetsPreview_, assetsPreviewText_.empty()
            ? Text("EditorUi.Preview", "Preview") + ": " + path
            : assetsPreviewText_);
    }
    if (assetSelectionHandler_) assetSelectionHandler_(path);
}

void EditorShell::SetAssetPreviewImage(std::string image, float width, float height)
{
    assetsPreviewImage_ = std::move(image);
    assetsPreviewImageWidth_ = width;
    assetsPreviewImageHeight_ = height;
    if (!assetsPreviewImageElement_.IsValid()) return;
    if (assetsPreviewImage_.empty())
    {
        document_.SetVisible(assetsPreviewImageElement_, false);
        return;
    }
    UiStyle style = document_.Style(assetsPreviewImageElement_);
    style.image = assetsPreviewImage_;
    document_.SetStyle(assetsPreviewImageElement_, style);
    document_.SetVisible(assetsPreviewImageElement_, true);
}

void EditorShell::SetAssetsPreviewText(std::string text)
{
    assetsPreviewText_ = std::move(text);
    if (!assetsPreview_.IsValid()) return;
    if (!assetsPreviewText_.empty())
    {
        document_.SetText(assetsPreview_, assetsPreviewText_);
        return;
    }
    const std::string path = assetsModel_.SelectedPath();
    document_.SetText(assetsPreview_, Text("EditorUi.Preview", "Preview") + ": " +
        (path.empty() ? Text("EditorUi.None", "none") : path));
}

void EditorShell::SetPanelText(EditorPanelId panel, std::string text)
{
    PanelSlot& slot = Slot(panel);
    if (slot.content.IsValid()) document_.SetText(slot.content, std::move(text));
}

void EditorShell::SetGizmo(float x, float y, bool visible)
{
    if (!gizmo_.IsValid()) return;
    document_.SetVisible(gizmo_, visible);
    for (UiElementHandle axis : gizmoAxes_) document_.SetVisible(axis, visible);
    if (!visible) return;
    UiStyle style = document_.Style(gizmo_);
    style.left = UiLength::Px(x - 6.0f);
    style.top = UiLength::Px(y - 6.0f);
    document_.SetStyle(gizmo_, style);
    const float offsets[3][2] = {{14.0f, -4.0f}, {-4.0f, 14.0f}, {14.0f, 14.0f}};
    for (std::size_t index = 0; index < gizmoAxes_.size() && index < 3; ++index)
    {
        UiStyle axisStyle = document_.Style(gizmoAxes_[index]);
        axisStyle.left = UiLength::Px(x + offsets[index][0]);
        axisStyle.top = UiLength::Px(y + offsets[index][1]);
        document_.SetStyle(gizmoAxes_[index], axisStyle);
    }
}

UiElementHandle EditorShell::PanelContent(EditorPanelId panel) const noexcept
{
    return Slot(panel).content;
}

UiElementHandle EditorShell::ScrollThumb(EditorPanelId panel) const noexcept
{
    return scrollThumbs_[static_cast<std::size_t>(panel)];
}

} // namespace se::ui::editor
