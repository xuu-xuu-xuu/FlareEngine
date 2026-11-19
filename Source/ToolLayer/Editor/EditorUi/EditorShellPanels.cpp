#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/EditorUi/EditorShellInternal.h"
#include "ToolLayer/Editor/EditorUi/EditorInk.h"
#include "CoreLayer/Utf.h"
#include "ToolLayer/Editor/FlarePalette.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>

namespace se::ui::editor
{
using namespace detail;

namespace
{
std::uint64_t NowMilliseconds() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool ContainsToken(std::string_view text, std::string_view token) noexcept
{
    return text.find(token) != std::string_view::npos;
}
} // namespace

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
                if (index < 0 || static_cast<std::size_t>(index) >= rows.size()) return UiColor{0, 0, 0, 0};
                if (hierarchyModel_.Nodes()[rows[index]].id == inspectorModel_.Target())
                {
                    return UiInk(activePanel_ == EditorPanelId::Hierarchy ? flare::workspace::Selection
                                                                          : flare::workspace::InactiveSelection);
                }
                if (index == hoveredHierarchyRow_)
                {
                    return UiInk(flare::workspace::Hover, 0.55f);
                }
                return UiColor{0, 0, 0, 0};
            });
            hierarchyList_->SetRowTextProvider([this](int index) {
                const auto rows = hierarchyModel_.VisibleRows();
                if (index < 0 || static_cast<std::size_t>(index) >= rows.size())
                {
                    return std::string{};
                }
                const EditorHierarchyNode& node =
                    hierarchyModel_.Nodes()[rows[static_cast<std::size_t>(index)]];
                std::string prefix(node.depth * 2, ' ');
                prefix += node.hasChildren ? (node.expanded ? "▾ " : "▸ ") : "  ";
                if (node.prefab) prefix += "◆ ";
                std::string suffix = node.enabled ? std::string{} : Text("EditorUi.DisabledSuffix", " (Disabled)");
                return prefix + node.label + suffix;
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
                const auto& node = hierarchyModel_.Nodes()[nodeIndex];
                dragHierarchyId_ = node.id;
                hierarchyDragStartX_ = event.x;
                hierarchyDragStartY_ = event.y;
                draggingHierarchy_ = false;
                const float arrowRight = 18.0f + static_cast<float>(node.depth) * 12.0f;
                if (event.x - rect.x <= arrowRight && node.hasChildren)
                {
                    hierarchyModel_.Toggle(nodeIndex);
                    if (hierarchyList_)
                    {
                        hierarchyList_->Invalidate();
                        hierarchyList_->SetItemCount(static_cast<int>(hierarchyModel_.VisibleRows().size()));
                        hierarchyList_->Sync();
                    }
                    event.handled = true;
                    return;
                }
                const std::uint64_t id = node.id;
                const std::uint64_t now = NowMilliseconds();
                const bool doubleClick = (id == lastHierarchyClickId_) && (now - lastHierarchyClickTick_ <= 380);
                lastHierarchyClickId_ = id;
                lastHierarchyClickTick_ = doubleClick ? 0 : now;
                hierarchyModel_.Select(id);
                if (selectionHandler_) selectionHandler_(id);
                if (doubleClick && viewportAction_) viewportAction_("Frame");
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerMove, [this](UiInputEvent& event) {
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
            const float offset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
            const int nextHover = (event.x >= rect.x && event.x < rect.x + rect.width &&
                                   event.y >= rect.y && event.y < rect.y + rect.height)
                ? static_cast<int>((event.y - rect.y + offset) / 20.0f) : -1;
            if (nextHover != hoveredHierarchyRow_)
            {
                hoveredHierarchyRow_ = nextHover;
                if (hierarchyList_) { hierarchyList_->Invalidate(); hierarchyList_->Sync(); }
            }
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
                if (index < 0 || static_cast<std::size_t>(index) >= entries.size()) return UiColor{0, 0, 0, 0};
                if (assetsModel_.Selected(entries[index].path))
                {
                    return UiInk(activePanel_ == EditorPanelId::Assets ? flare::workspace::Selection
                                                                       : flare::workspace::InactiveSelection);
                }
                if (index == hoveredAssetRow_)
                {
                    return UiInk(flare::workspace::Hover, 0.55f);
                }
                return UiColor{0, 0, 0, 0};
            });
            assetsList_->SetRowTextProvider([this](int index) {
                const auto& entries = assetsModel_.Filtered();
                if (index < 0 || static_cast<std::size_t>(index) >= entries.size())
                {
                    return std::string{};
                }
                const EditorAssetEntry& entry = entries[static_cast<std::size_t>(index)];
                const auto separator = entry.path.find_last_of("/\\");
                std::string fileName = entry.path.substr(separator == std::string::npos ? 0 : separator + 1);
                if (fileName.ends_with(".material.json")) fileName.erase(fileName.size() - 14);
                else if (fileName.ends_with(".prefab.json")) fileName.erase(fileName.size() - 12);
                else if (fileName.ends_with(".scene.json")) fileName.erase(fileName.size() - 11);

                std::string badge;
                if (entry.type == "model") badge = Text("EditorUi.AssetBadgeModel", "[3D]") + " ";
                else if (entry.type == "material") badge = Text("EditorUi.AssetBadgeMaterial", "[Mat]") + " ";
                else if (entry.type == "texture") badge = Text("EditorUi.AssetBadgeTexture", "[Tex]") + " ";
                else if (entry.type == "prefab") badge = Text("EditorUi.AssetBadgePrefab", "[Prefab]") + " ";
                else if (entry.type == "C#") badge = "[C#] ";
                else if (entry.type == "audio") badge = Text("EditorUi.AssetBadgeAudio", "[Audio]") + " ";
                else if (entry.type == "scene") badge = Text("EditorUi.AssetBadgeScene", "[Scene]") + " ";
                else if (entry.type == "shader") badge = "[HLSL] ";
                else if (!entry.type.empty()) badge = "• ";

                std::string folderHint;
                if (separator != std::string::npos && separator > 0)
                {
                    const auto parentSep = entry.path.find_last_of("/\\", separator - 1);
                    const std::string parentDir = entry.path.substr(
                        parentSep == std::string::npos ? 0 : parentSep + 1,
                        separator - (parentSep == std::string::npos ? 0 : parentSep + 1));
                    if (!parentDir.empty() && parentDir != "." && parentDir != "Content")
                        folderHint = "  · " + parentDir;
                }
                return badge + fileName + folderHint;
            });
        }
        assetsTypeElement_ = document_.CreateElement(UiElementKind::Button);
        if (document_.SetParent(assetsTypeElement_, slot.panel))
        {
            auto typeStyle = ButtonStyle(0.0f, 1.0f, 64.0f, 22.0f, kSurfaceRaised);
            typeStyle.cornerRadius = 4.0f;
            document_.SetStyle(assetsTypeElement_, typeStyle);
            document_.SetText(assetsTypeElement_, Text("EditorUi.FilterAll", "All") + " ▾");
            dispatcher_.AddHandler(assetsTypeElement_, UiEventType::PointerDown, [this](UiInputEvent& event) {
                if (event.button != 0) return;
                if (openMenu_ == "@asset-type") CloseMenu();
                else
                {
                    const auto bounds = layout_.Rect(assetsTypeElement_);
                    OpenAssetTypeMenu(bounds.x, bounds.y + bounds.height);
                }
                event.handled = true;
            });
        }
        assetsFilterElement_ = document_.CreateElement(UiElementKind::TextEdit);
        if (document_.SetParent(assetsFilterElement_, slot.panel))
        {
            auto filterStyle = ButtonStyle(3.0f, 2.0f, 100.0f, 20.0f, kSurface);
            filterStyle.cornerRadius = 4.0f;
            document_.SetStyle(assetsFilterElement_, filterStyle);
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
        assetsCreateElement_ = document_.CreateElement(UiElementKind::Button);
        if (document_.SetParent(assetsCreateElement_, slot.panel))
        {
            auto createStyle = ButtonStyle(2.0f, 1.0f, 24.0f, 22.0f, kSurfaceActive);
            createStyle.cornerRadius = 4.0f;
            createStyle.color = UiInk(flare::Gold);
            createStyle.padding = {5.0f, 0.0f, 0.0f, 0.0f};
            document_.SetStyle(assetsCreateElement_, createStyle);
            document_.SetText(assetsCreateElement_, "＋");
            dispatcher_.AddHandler(assetsCreateElement_, UiEventType::PointerDown, [this](UiInputEvent& event) {
                if (event.button != 0) return;
                if (openMenu_ == "@create") CloseMenu();
                else
                {
                    const auto bounds = layout_.Rect(assetsCreateElement_);
                    OpenCreateMenu(EditorPanelId::Assets, bounds.x, bounds.y + bounds.height);
                }
                event.handled = true;
            });
        }
        assetsImportElement_ = document_.CreateElement(UiElementKind::Button);
        if (document_.SetParent(assetsImportElement_, slot.panel))
        {
            auto importStyle = ButtonStyle(0.0f, 0.0f, 52.0f, 24.0f, kSurfaceActive);
            importStyle.cornerRadius = 4.0f;
            importStyle.color = UiInk(flare::Gold);
            document_.SetStyle(assetsImportElement_, importStyle);
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
                const std::string path = entries[static_cast<std::size_t>(row)].path;
                SelectAsset(path);
                if (event.button == 0)
                {
                    const std::uint64_t now = NowMilliseconds();
                    const bool doubleClick = (path == lastAssetClickPath_) && (now - lastAssetClickTick_ <= 380);
                    if (doubleClick)
                    {
                        lastAssetClickPath_.clear();
                        lastAssetClickTick_ = 0;
                        if (assetActivateHandler_) assetActivateHandler_(path);
                        event.handled = true;
                        return;
                    }
                    lastAssetClickPath_ = path;
                    lastAssetClickTick_ = now;
                    if (!assetDrag_) assetDrag_ = std::make_unique<EditorAssetDragState>();
                    assetDrag_->assetPath = path;
                    assetDrag_->startX = event.x;
                    assetDrag_->startY = event.y;
                    assetDrag_->dragging = false;
                    dispatcher_.SetPointerCapture(Slot(EditorPanelId::Assets).content);
                }
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerMove, [this](UiInputEvent& event) {
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Assets).content);
            const float offset = assetsList_ ? assetsList_->Offset() : 0.0f;
            const int nextHover = (event.x >= rect.x && event.x < rect.x + rect.width &&
                                   event.y >= rect.y && event.y < rect.y + rect.height)
                ? static_cast<int>((event.y - rect.y + offset) / 20.0f) : -1;
            if (nextHover != hoveredAssetRow_)
            {
                hoveredAssetRow_ = nextHover;
                if (assetsList_) { assetsList_->Invalidate(); assetsList_->Sync(); }
            }
            if (!assetDrag_ || assetDrag_->assetPath.empty()) return;
            const float dx = event.x - assetDrag_->startX;
            const float dy = event.y - assetDrag_->startY;
            if (!assetDrag_->dragging && std::abs(dx) + std::abs(dy) > 4.0f)
            {
                assetDrag_->dragging = true;
                lastAssetClickPath_.clear();
                lastAssetClickTick_ = 0;
            }
            if (!assetDrag_->dragging) return;
            if (dragGhost_.IsValid())
            {
                if (CurrentInputRoot().IsValid()) document_.SetParent(dragGhost_, CurrentInputRoot());
                document_.SetVisible(dragGhost_, true);
                UiStyle ghostStyle = document_.Style(dragGhost_);
                ghostStyle.left = UiLength::Px(event.x + 8.0f);
                ghostStyle.top = UiLength::Px(event.y);
                document_.SetStyle(dragGhost_, ghostStyle);
                const auto sep = assetDrag_->assetPath.find_last_of("/\\");
                document_.SetText(dragGhost_, assetDrag_->assetPath.substr(sep == std::string::npos ? 0 : sep + 1));
            }
            const auto target = UiHitTest(document_, layout_).Hit(CurrentInputRoot(), event.x, event.y).target;
            const bool overViewport = target == ViewportPanel();
            if (assetDrag_->hoverHandler) assetDrag_->hoverHandler(assetDrag_->assetPath, event.x, event.y, overViewport);

            const auto insideRect = [&](const UiRect& r) {
                return r.width > 0.0f && r.height > 0.0f &&
                       event.x >= r.x && event.x < r.x + r.width &&
                       event.y >= r.y && event.y < r.y + r.height;
            };
            const UiRect hierContent = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
            const bool overHierarchy = dock_.SurfaceFor(EditorPanelId::Hierarchy) == inputSurface_ &&
                                       document_.Visible(Slot(EditorPanelId::Hierarchy).panel) &&
                                       insideRect(hierContent);
            if (overHierarchy)
            {
                const float hOffset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
                const int hRow = static_cast<int>((event.y - hierContent.y + hOffset) / 20.0f);
                if (hRow != hoveredHierarchyRow_)
                {
                    hoveredHierarchyRow_ = hRow;
                    if (hierarchyList_) { hierarchyList_->Invalidate(); hierarchyList_->Sync(); }
                }
            }
            else if (hoveredHierarchyRow_ != -1)
            {
                hoveredHierarchyRow_ = -1;
                if (hierarchyList_) { hierarchyList_->Invalidate(); hierarchyList_->Sync(); }
            }
            const UiRect inspPanel = layout_.Rect(Slot(EditorPanelId::Inspector).panel);
            const bool overInspector = dock_.SurfaceFor(EditorPanelId::Inspector) == inputSurface_ &&
                                       document_.Visible(Slot(EditorPanelId::Inspector).panel) &&
                                       insideRect(inspPanel);
            if (dropHighlight_.IsValid())
            {
                if (overInspector || overHierarchy)
                {
                    if (CurrentInputRoot().IsValid()) document_.SetParent(dropHighlight_, CurrentInputRoot());
                    const UiRect highlightRect = overInspector ? inspPanel : layout_.Rect(Slot(EditorPanelId::Hierarchy).panel);
                    UiStyle hl = document_.Style(dropHighlight_);
                    hl.left = UiLength::Px(highlightRect.x);
                    hl.top = UiLength::Px(highlightRect.y);
                    hl.width = UiLength::Px(highlightRect.width);
                    hl.height = UiLength::Px(highlightRect.height);
                    document_.SetStyle(dropHighlight_, hl);
                    document_.SetVisible(dropHighlight_, true);
                }
                else
                {
                    document_.SetVisible(dropHighlight_, false);
                }
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerUp, [this](UiInputEvent& event) {
            if (!assetDrag_ || assetDrag_->assetPath.empty()) return;
            const bool dropped = assetDrag_->dragging && event.button == 0;
            const std::string path = assetDrag_->assetPath;
            const auto dropHandler = assetDrag_->dropHandler;
            CancelAssetDrag();
            if (dropped)
            {
                const auto target = UiHitTest(document_, layout_).Hit(CurrentInputRoot(), event.x, event.y).target;
                if (target == ViewportPanel() && dropHandler)
                {
                    dropHandler(path, event.x, event.y);
                }
                else if (assetEntityDropHandler_)
                {
                    const auto insideRect = [&](const UiRect& r) {
                        return r.width > 0.0f && r.height > 0.0f &&
                               event.x >= r.x && event.x < r.x + r.width &&
                               event.y >= r.y && event.y < r.y + r.height;
                    };
                    const UiRect hierPanel = layout_.Rect(Slot(EditorPanelId::Hierarchy).panel);
                    const UiRect inspPanel = layout_.Rect(Slot(EditorPanelId::Inspector).panel);
                    if (dock_.SurfaceFor(EditorPanelId::Hierarchy) == inputSurface_ &&
                        document_.Visible(Slot(EditorPanelId::Hierarchy).panel) && insideRect(hierPanel))
                    {
                        const UiRect hierContent = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
                        const float hOffset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
                        const int hRow = insideRect(hierContent)
                            ? static_cast<int>((event.y - hierContent.y + hOffset) / 20.0f) : -1;
                        const auto rows = hierarchyModel_.VisibleRows();
                        const std::uint64_t entityId = (hRow >= 0 && static_cast<std::size_t>(hRow) < rows.size())
                            ? hierarchyModel_.Nodes()[rows[static_cast<std::size_t>(hRow)]].id : 0;
                        assetEntityDropHandler_(path, entityId, EditorPanelId::Hierarchy);
                    }
                    else if (dock_.SurfaceFor(EditorPanelId::Inspector) == inputSurface_ &&
                             document_.Visible(Slot(EditorPanelId::Inspector).panel) && insideRect(inspPanel))
                    {
                        assetEntityDropHandler_(path, inspectorModel_.Target(), EditorPanelId::Inspector);
                    }
                }
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerCaptureLost, [this](UiInputEvent&) {
            CancelAssetDrag();
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
            console_->SetRowBackgroundProvider([this](int index) {
                if (index < 0 || static_cast<std::size_t>(index) >= lines_.size()) return UiColor{0, 0, 0, 0};
                const std::string_view line = lines_[static_cast<std::size_t>(index)];
                if (ContainsToken(line, "[Error]"))
                    return UiColor{0.42f, 0.11f, 0.13f, 0.42f};
                if (ContainsToken(line, "[Warning]"))
                    return UiColor{0.42f, 0.28f, 0.07f, 0.36f};
                return (index % 2 == 1) ? UiColor{1.0f, 1.0f, 1.0f, 0.018f} : UiColor{0, 0, 0, 0};
            });
            console_->SetRowTextProvider([this](int index) {
                return index >= 0 && static_cast<std::size_t>(index) < lines_.size()
                    ? lines_[static_cast<std::size_t>(index)] : std::string{};
            });
            console_->SetItemCount(0);
        }
        consoleClearButton_ = document_.CreateElement(UiElementKind::Button);
        if (document_.SetParent(consoleClearButton_, slot.panel))
        {
            auto clearStyle = ButtonStyle(0.0f, 2.0f, 50.0f, 20.0f, kSurfaceRaised);
            clearStyle.fontSize = 12.0f;
            clearStyle.borderWidth = 1.0f;
            clearStyle.borderColor = kBorder;
            clearStyle.cornerRadius = 4.0f;
            document_.SetStyle(consoleClearButton_, clearStyle);
            document_.SetText(consoleClearButton_, Text("EditorUi.ClearConsole", "Clear"));
            dispatcher_.AddHandler(consoleClearButton_, UiEventType::PointerDown, [this](UiInputEvent& event) {
                if (event.button != 0) return;
                ClearConsole();
                event.handled = true;
            });
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
    style.width = UiLength::Px(6.0f);
    style.height = UiLength::Px(20.0f);
    style.background = UiInk(flare::Silver, 0.72f);
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
        const float left = std::max(0.0f, contentRect.width - 7.0f);
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
    // EditorApplication 每帧重建同样的列表；内容未变（含保留的折叠状态）时不要销毁行元素，
    // 否则新行在当帧没有布局矩形，且破坏稳定帧零布局契约。
    if (!hierarchyModel_.SetNodes(std::move(nodes))) return;
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

void EditorShell::SetAssetDragHandler(std::function<void(const std::string&, float, float, bool)> handler)
{
    if (!assetDrag_) assetDrag_ = std::make_unique<EditorAssetDragState>();
    assetDrag_->hoverHandler = std::move(handler);
}

void EditorShell::SetAssetDropHandler(std::function<bool(const std::string&, float, float)> handler)
{
    if (!assetDrag_) assetDrag_ = std::make_unique<EditorAssetDragState>();
    assetDrag_->dropHandler = std::move(handler);
}

void EditorShell::CancelAssetDrag()
{
    if (dragGhost_.IsValid()) document_.SetVisible(dragGhost_, false);
    if (dropHighlight_.IsValid() && !tabDragging_) document_.SetVisible(dropHighlight_, false);
    if (hoveredHierarchyRow_ != -1)
    {
        hoveredHierarchyRow_ = -1;
        if (hierarchyList_)
        {
            hierarchyList_->Invalidate();
            hierarchyList_->Sync();
        }
    }
    if (assetDrag_)
    {
        if (assetDrag_->dragging && assetDrag_->hoverHandler)
            assetDrag_->hoverHandler({}, 0.0f, 0.0f, false);
        assetDrag_->assetPath.clear();
        assetDrag_->dragging = false;
    }
    if (dispatcher_.PointerCapture() == Slot(EditorPanelId::Assets).content)
        dispatcher_.ReleasePointerCapture();
}

} // namespace se::ui::editor
