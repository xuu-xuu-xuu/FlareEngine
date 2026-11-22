#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/EditorUi/EditorShellInternal.h"
#include "ToolLayer/Editor/EditorUi/EditorInk.h"
#include "CoreLayer/Utf.h"
#include "ToolLayer/Editor/FlarePalette.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace flare::ui::editor
{
using namespace detail;

namespace
{
constexpr float kHierarchyRowHeight = 20.0f;
constexpr float kAssetRowHeight = 22.0f;

std::uint64_t NowMilliseconds() noexcept
{
    return static_cast<std::uint64_t>(GetTickCount64());
}

bool ContainsToken(std::string_view text, std::string_view token) noexcept
{
    return text.find(token) != std::string_view::npos;
}

std::string CleanAssetDisplayName(const EditorAssetEntry& entry)
{
    if (!entry.displayName.empty()) return entry.displayName;
    const auto separator = entry.path.find_last_of("/\\");
    std::string fileName = entry.path.substr(separator == std::string::npos ? 0 : separator + 1);
    if (fileName.ends_with(".material.json")) fileName.erase(fileName.size() - 14);
    else if (fileName.ends_with(".prefab.json")) fileName.erase(fileName.size() - 12);
    else if (fileName.ends_with(".scene.json")) fileName.erase(fileName.size() - 11);
    return fileName;
}

constexpr std::uint64_t kFastDoubleClickMs = 340;

std::uint32_t Utf8CodepointCount(std::string_view text) noexcept
{
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < text.size();)
    {
        ++count;
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xc0u) == 0x80u) ++i;
    }
    return count;
}

std::size_t Utf8ByteAtCodepoint(std::string_view s, std::uint32_t count) noexcept
{
    std::size_t i = 0;
    while (i < s.size() && count--)
    {
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0u) == 0x80u) ++i;
    }
    return i;
}

std::uint32_t StemCodepointsForAsset(std::string_view displayName, bool isFolder) noexcept
{
    if (isFolder) return Utf8CodepointCount(displayName);
    const std::size_t dot = displayName.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return Utf8CodepointCount(displayName);
    return Utf8CodepointCount(displayName.substr(0, dot));
}
} // namespace

void EditorShell::PaintInlineTextEdit(UiTextEdit& edit, UiElementHandle element, UiElementHandle selectionEl,
                                      float& scroll, float width, float height)
{
    if (!element.IsValid()) return;
    const auto raw = edit.Text();
    const auto caret = Utf8ByteAtCodepoint(raw, edit.Caret());
    const auto anchor = Utf8ByteAtCodepoint(raw, edit.Anchor());
    const std::string display = edit.DisplayText();
    const bool composing = edit.Composing();
    const std::size_t first = std::min(caret, anchor);
    const std::size_t last = composing ? first + edit.Composition().size() : std::max(caret, anchor);

    const float padX = 4.0f;
    const float available = std::max(1.0f, width - padX * 2.0f);
    const auto at = [&](std::size_t byte) -> float {
        const std::size_t clamped = std::min(byte, display.size());
        if (text_ != nullptr)
        {
            return text_->CaretRect(display, 13.0f, 100000.0f, false, static_cast<std::uint32_t>(clamped)).x;
        }
        return static_cast<float>(Utf8CodepointCount(std::string_view(display).substr(0, clamped))) * 7.2f;
    };

    const std::size_t active = composing ? last : caret;
    const float activeX = std::max(0.0f, at(active));
    scroll = std::clamp(scroll, std::max(0.0f, activeX - std::max(0.0f, available - 2.0f)), activeX);

    std::size_t start = 0;
    std::size_t end = display.size();
    if (text_ != nullptr)
    {
        start = std::min<std::size_t>(display.size(), text_->HitTestCaret(display, 13.0f, 100000.0f, false, scroll, 0.0f));
        scroll = at(start);
        end = std::min<std::size_t>(display.size(), text_->HitTestCaret(display, 13.0f, 100000.0f, false, scroll + available, 0.0f));
        if (end < start) end = start;
    }
    document_.SetText(element, std::string_view(display).substr(start, end - start));

    if (selectionEl.IsValid())
    {
        const float left = std::clamp(at(first) - scroll, 0.0f, available);
        const float right = std::clamp(at(last) - scroll, left, available);
        const bool isCaretOnly = (first == last);
        const float hlHeight = std::max(12.0f, height - 4.0f);
        auto highlight = AbsoluteStyle(padX + left, (height - hlHeight) * 0.5f,
                                       isCaretOnly ? 1.5f : std::max(1.5f, right - left), hlHeight);
        highlight.background = isCaretOnly
            ? UiInk(flare::palette::Heading)
            : UiInk(flare::palette::workspace::Selection, 0.88f);
        highlight.cornerRadius = isCaretOnly ? 0.0f : 2.0f;
        highlight.padding = {};
        highlight.pointerEvents = UiPointerEvents::None;
        document_.SetStyle(selectionEl, highlight);
        document_.SetVisible(selectionEl, true);
    }
}

void EditorShell::PlaceInlineTextCaret(UiTextEdit& edit, UiElementHandle element, UiElementHandle selectionEl,
                                       float& scroll, float clickX, bool extendSelection)
{
    if (!element.IsValid()) return;
    const UiRect rect = layout_.Rect(element);
    const std::string raw(edit.Text());
    const float localX = std::max(0.0f, clickX - rect.x - 4.0f + scroll);
    std::uint32_t targetCp = 0;
    if (text_ != nullptr)
    {
        const std::size_t byte = std::min<std::size_t>(raw.size(),
            text_->HitTestCaret(raw, 13.0f, 100000.0f, false, localX, 0.0f));
        std::u32string prefix;
        if (DecodeUtf8(std::string_view(raw).substr(0, byte), prefix))
            targetCp = static_cast<std::uint32_t>(prefix.size());
    }
    else
    {
        const std::uint32_t total = Utf8CodepointCount(raw);
        targetCp = std::min<std::uint32_t>(total, static_cast<std::uint32_t>(std::lround(localX / 7.2f)));
    }
    if (extendSelection) edit.Select(edit.Anchor(), targetCp);
    else edit.SetCaret(targetCp);
    PaintInlineTextEdit(edit, element, selectionEl, scroll, rect.width, rect.height);
}

bool EditorShell::AssetsHasFolders() const noexcept
{
    return std::any_of(assetsModel_.Entries().begin(), assetsModel_.Entries().end(),
                       [](const EditorAssetEntry& entry) { return entry.type == "folder"; });
}

void EditorShell::BuildHierarchyRow(int index, UiElementHandle row)
{
    const auto rows = hierarchyModel_.VisibleRows();
    if (index < 0 || static_cast<std::size_t>(index) >= rows.size()) return;
    const EditorHierarchyNode& node = hierarchyModel_.Nodes()[rows[static_cast<std::size_t>(index)]];
    const float indent = 6.0f + static_cast<float>(node.depth) * 12.0f;
    const bool hasIcon = node.prefab || !node.iconKind.empty();
    const float textLeft = indent + (hasIcon ? 25.0f : 13.0f);

    if (node.hasChildren)
    {
        const auto arrow = document_.CreateElement(UiElementKind::Label);
        if (document_.SetParent(arrow, row))
        {
            auto as = AbsoluteStyle((indent - 1.0f) - textLeft, 0.0f, 12.0f, kHierarchyRowHeight);
            as.background = UiColor{0, 0, 0, 0};
            as.color = kMuted;
            as.fontSize = 11.5f;
            as.pointerEvents = UiPointerEvents::None;
            document_.SetStyle(arrow, as);
            document_.SetText(arrow, node.expanded ? "▾" : "▸");
        }
    }

    if (hasIcon)
    {
        const auto icon = document_.CreateElement(UiElementKind::Panel);
        if (document_.SetParent(icon, row))
        {
            UiColor badgeColor = UiColor{0.36f, 0.42f, 0.50f, 0.85f};
            float radius = 2.5f;
            if (node.prefab) { badgeColor = UiColor{0.28f, 0.62f, 1.0f, 0.95f}; radius = 2.5f; }
            else if (node.iconKind == "camera") { badgeColor = UiInk(flare::palette::Gold, 0.92f); radius = 3.0f; }
            else if (node.iconKind == "light") { badgeColor = UiColor{0.96f, 0.72f, 0.24f, 0.95f}; radius = 5.0f; }
            else if (node.iconKind == "mesh") { badgeColor = UiColor{0.26f, 0.70f, 0.88f, 0.90f}; radius = 2.5f; }
            else if (node.iconKind == "audio") { badgeColor = UiColor{0.95f, 0.56f, 0.22f, 0.92f}; radius = 3.0f; }
            else if (node.iconKind == "particle") { badgeColor = UiColor{0.68f, 0.44f, 0.98f, 0.92f}; radius = 5.0f; }

            auto is = AbsoluteStyle((indent + 11.0f) - textLeft, 5.0f, 10.0f, 10.0f);
            is.background = badgeColor;
            is.cornerRadius = radius;
            if (!node.prefab && node.iconKind == "empty")
            {
                is.background = UiColor{0, 0, 0, 0};
                is.borderWidth = 1.5f;
                is.borderColor = kMuted;
                is.cornerRadius = 5.0f;
            }
            is.pointerEvents = UiPointerEvents::None;
            document_.SetStyle(icon, is);
        }
    }

    UiStyle rowStyle = document_.Style(row);
    rowStyle.padding = {textLeft, 0.0f, 6.0f, 0.0f};
    rowStyle.fontSize = 13.0f;
    rowStyle.textFlow = UiTextFlow::SingleLineEllipsis;
    rowStyle.color = !node.enabled ? kMuted : (node.prefab ? UiColor{0.62f, 0.82f, 1.0f, 1.0f} : kText);
    document_.SetStyle(row, rowStyle);

    const std::string suffix = node.enabled ? std::string{} : Text("EditorUi.DisabledSuffix", " (Disabled)");
    document_.SetText(row, node.label + suffix);
}

void EditorShell::BuildAssetIcon(UiElementHandle parent, float x, float y, float size, const EditorAssetEntry& entry)
{
    const auto addRect = [&](float rx, float ry, float rw, float rh, UiColor bg, float radius = 2.0f) {
        const auto el = document_.CreateElement(UiElementKind::Panel);
        if (!document_.SetParent(el, parent)) return UiElementHandle{};
        auto st = AbsoluteStyle(rx, ry, rw, rh);
        st.background = bg;
        st.cornerRadius = radius;
        st.pointerEvents = UiPointerEvents::None;
        document_.SetStyle(el, st);
        return el;
    };
    const auto addGlyph = [&](float rx, float ry, float rw, float rh, const char* glyph,
                              UiColor fg, float fontSz) {
        const auto el = document_.CreateElement(UiElementKind::Label);
        if (!document_.SetParent(el, parent)) return;
        auto st = AbsoluteStyle(rx, ry, rw, rh);
        st.background = UiColor{0, 0, 0, 0};
        st.color = fg;
        st.fontSize = fontSz;
        st.textFlow = UiTextFlow::SingleLineEllipsis;
        st.padding = {std::max(0.0f, (rw - MeasuredWidth(glyph, fontSz)) * 0.5f), 0.0f, 0.0f, 0.0f};
        st.pointerEvents = UiPointerEvents::None;
        document_.SetStyle(el, st);
        document_.SetText(el, glyph);
    };

    if (entry.type == "folder")
    {
        const UiColor goldTab = UiInk(flare::palette::Gold, 0.82f);
        const UiColor goldBody = UiInk(flare::palette::Gold, 0.96f);
        addRect(x, y + size * 0.12f, size * 0.46f, size * 0.22f, goldTab, 2.0f);
        addRect(x, y + size * 0.26f, size, size * 0.64f, goldBody, 2.5f);
        return;
    }

    if (!entry.thumbnail.empty())
    {
        const auto img = document_.CreateElement(UiElementKind::Image);
        if (document_.SetParent(img, parent))
        {
            auto st = AbsoluteStyle(x, y, size, std::max(1.0f, size - 2.0f));
            st.image = entry.thumbnail;
            st.cornerRadius = entry.type == "material" ? (size * 0.45f) : 3.0f;
            st.pointerEvents = UiPointerEvents::None;
            document_.SetStyle(img, st);
        }
        const UiColor bar = entry.type == "material"
            ? UiColor{0.62f, 0.38f, 1.0f, 0.95f}
            : UiColor{0.16f, 0.78f, 0.64f, 0.95f};
        addRect(x + 1.0f, y + size - 2.0f, std::max(2.0f, size - 2.0f), 2.0f, bar, 1.0f);
        return;
    }

    const UiColor darkGlyph = UiInk(flare::palette::Background);
    const float glyphFont = size >= 28.0f ? 12.0f : 9.5f;

    if (entry.type == "scene")
    {
        addRect(x, y, size, size, UiInk(flare::palette::Gold), 3.5f);
        addGlyph(x, y, size, size, "★", darkGlyph, glyphFont);
    }
    else if (entry.type == "model")
    {
        addRect(x, y, size, size, UiColor{0.16f, 0.64f, 0.86f, 0.95f}, 3.5f);
        addGlyph(x, y, size, size, "◆", UiColor{0.96f, 0.98f, 1.0f, 1.0f}, glyphFont);
    }
    else if (entry.type == "material")
    {
        addRect(x, y, size, size, UiColor{0.58f, 0.34f, 0.96f, 0.95f}, size * 0.5f);
        addRect(x + size * 0.24f, y + size * 0.20f, size * 0.24f, size * 0.24f,
                UiColor{1.0f, 1.0f, 1.0f, 0.55f}, size * 0.12f);
    }
    else if (entry.type == "texture")
    {
        addRect(x, y, size, size, UiColor{0.14f, 0.74f, 0.60f, 0.95f}, 3.5f);
        addRect(x + size * 0.18f, y + size * 0.52f, size * 0.64f, size * 0.30f,
                UiColor{0.05f, 0.24f, 0.20f, 0.65f}, 1.5f);
        addRect(x + size * 0.56f, y + size * 0.20f, size * 0.22f, size * 0.22f,
                UiColor{0.95f, 0.98f, 0.96f, 0.85f}, size * 0.11f);
    }
    else if (entry.type == "prefab")
    {
        addRect(x, y, size, size, UiColor{0.24f, 0.58f, 1.0f, 0.96f}, 3.0f);
        addRect(x + 2.0f, y + 2.0f, std::max(2.0f, size - 4.0f), std::max(2.0f, size * 0.22f),
                UiColor{0.75f, 0.90f, 1.0f, 0.70f}, 1.5f);
    }
    else if (entry.type == "C#")
    {
        addRect(x, y, size, size, UiColor{0.18f, 0.76f, 0.44f, 0.96f}, 3.5f);
        addGlyph(x, y, size, size, "C#", darkGlyph, size >= 28.0f ? 11.5f : 8.5f);
    }
    else if (entry.type == "audio")
    {
        addRect(x, y, size, size, UiColor{0.92f, 0.54f, 0.16f, 0.95f}, 3.5f);
        const float bw = std::max(1.5f, size * 0.13f);
        addRect(x + size * 0.22f, y + size * 0.36f, bw, size * 0.30f, darkGlyph, 1.0f);
        addRect(x + size * 0.44f, y + size * 0.18f, bw, size * 0.64f, darkGlyph, 1.0f);
        addRect(x + size * 0.66f, y + size * 0.28f, bw, size * 0.44f, darkGlyph, 1.0f);
    }
    else if (entry.type == "env" || entry.type == "tod")
    {
        addRect(x, y, size, size, UiColor{0.88f, 0.62f, 0.20f, 0.94f}, 3.5f);
        addRect(x + size * 0.26f, y + size * 0.22f, size * 0.48f, size * 0.48f,
                UiColor{1.0f, 0.95f, 0.78f, 0.90f}, size * 0.24f);
    }
    else if (entry.type == "navmesh")
    {
        addRect(x, y, size, size, UiColor{0.38f, 0.44f, 0.96f, 0.94f}, 3.5f);
        addGlyph(x, y, size, size, "❖", UiColor{0.94f, 0.96f, 1.0f, 1.0f}, glyphFont);
    }
    else if (entry.type == "shader")
    {
        addRect(x, y, size, size, UiColor{0.84f, 0.34f, 0.58f, 0.94f}, 3.5f);
        addGlyph(x, y, size, size, "S", UiColor{0.98f, 0.96f, 1.0f, 1.0f}, glyphFont);
    }
    else
    {
        addRect(x, y, size, size, UiColor{0.42f, 0.46f, 0.54f, 0.90f}, 3.0f);
    }
}

void EditorShell::BuildAssetRow(int index, UiElementHandle row)
{
    const auto& entries = assetsModel_.Filtered();
    if (index < 0 || static_cast<std::size_t>(index) >= entries.size()) return;
    const EditorAssetEntry& entry = entries[static_cast<std::size_t>(index)];
    const bool treeMode = AssetsHasFolders() && assetsModel_.CurrentFolder().empty() &&
                          assetsModel_.Filter().empty() && assetsModel_.TypeFilter().empty();
    const float indent = 6.0f + static_cast<float>(entry.depth) * 14.0f;
    const float iconX = indent + (treeMode ? 12.0f : 2.0f);
    const float textLeft = iconX + 21.0f;

    if (treeMode && entry.type == "folder" && entry.hasChildren)
    {
        const auto arrow = document_.CreateElement(UiElementKind::Label);
        if (document_.SetParent(arrow, row))
        {
            auto as = AbsoluteStyle((indent - 1.0f) - textLeft, 0.0f, 12.0f, kAssetRowHeight);
            as.background = UiColor{0, 0, 0, 0};
            as.color = kMuted;
            as.fontSize = 11.5f;
            as.pointerEvents = UiPointerEvents::None;
            document_.SetStyle(arrow, as);
            document_.SetText(arrow, entry.expanded ? "▾" : "▸");
        }
    }

    BuildAssetIcon(row, iconX - textLeft, 3.0f, 16.0f, entry);

    UiStyle rowStyle = document_.Style(row);
    rowStyle.padding = {textLeft, 0.0f, 6.0f, 0.0f};
    rowStyle.fontSize = 13.0f;
    rowStyle.textFlow = UiTextFlow::SingleLineEllipsis;
    rowStyle.color = entry.type == "folder" ? UiInk(flare::palette::Heading) : kText;
    if (index == hoveredFolderDropRow_ && entry.type == "folder")
    {
        rowStyle.borderWidth = 1.0f;
        rowStyle.borderColor = UiInk(flare::palette::workspace::Focus);
        rowStyle.background = UiInk(flare::palette::workspace::Focus, 0.22f);
    }
    document_.SetStyle(row, rowStyle);
    document_.SetText(row, CleanAssetDisplayName(entry));
}

bool EditorShell::BeginHierarchyRename(std::uint64_t id)
{
    if (id == 0 || !hierarchyList_ || !hierarchyEditElement_.IsValid() || !hierarchyEdit_ || previewPlaying_)
        return false;
    const auto rows = hierarchyModel_.VisibleRows();
    std::size_t visibleIndex = rows.size();
    std::string currentLabel;
    std::size_t depth = 0;
    bool hasIcon = false;
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        const auto& node = hierarchyModel_.Nodes()[rows[i]];
        if (node.id == id)
        {
            visibleIndex = i;
            currentLabel = node.label;
            depth = node.depth;
            hasIcon = node.prefab || !node.iconKind.empty();
            break;
        }
    }
    if (visibleIndex >= rows.size()) return false;

    const float rowTop = static_cast<float>(visibleIndex) * kHierarchyRowHeight;
    if (rowTop < hierarchyList_->Offset())
        hierarchyList_->SetScrollOffset(rowTop);
    else if (rowTop + kHierarchyRowHeight > hierarchyList_->Offset() + hierarchyList_->ViewportHeight())
        hierarchyList_->SetScrollOffset(rowTop + kHierarchyRowHeight - hierarchyList_->ViewportHeight());
    hierarchyList_->Sync();

    const UiRect panelRect = layout_.Rect(Slot(EditorPanelId::Hierarchy).panel);
    const UiRect contentRect = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
    const float indent = 6.0f + static_cast<float>(depth) * 12.0f + (hasIcon ? 23.0f : 11.0f);
    const float y = (contentRect.y - panelRect.y) + rowTop - hierarchyList_->Offset();
    const float w = std::max(48.0f, contentRect.width - indent - 8.0f);

    hierarchyEditingId_ = id;
    pendingHierarchyRenameId_ = 0;
    hierarchyEditScroll_ = 0.0f;
    hierarchyEditSelecting_ = false;
    auto style = ButtonStyle(indent, y, w, kHierarchyRowHeight, kSurface);
    style.cornerRadius = 3.0f;
    style.borderWidth = 1.0f;
    style.borderColor = UiInk(flare::palette::workspace::Focus);
    style.padding = {4.0f, 0.0f, 4.0f, 0.0f};
    document_.SetStyle(hierarchyEditElement_, style);
    hierarchyEdit_->SetText(currentLabel);
    hierarchyEdit_->Select(0, std::numeric_limits<std::uint32_t>::max());
    document_.SetVisible(hierarchyEditElement_, true);
    PaintInlineTextEdit(*hierarchyEdit_, hierarchyEditElement_, hierarchyEditSelection_,
                        hierarchyEditScroll_, w, kHierarchyRowHeight);
    dispatcher_.SetFocus(hierarchyEditElement_);
    return true;
}

void EditorShell::EndHierarchyRename(bool commit)
{
    if (hierarchyEditingId_ == 0) return;
    const std::uint64_t id = hierarchyEditingId_;
    hierarchyEditingId_ = 0;
    hierarchyEditSelecting_ = false;
    if (dispatcher_.PointerCapture() == hierarchyEditElement_)
        dispatcher_.ReleasePointerCapture();
    if (hierarchyEditElement_.IsValid())
        document_.SetVisible(hierarchyEditElement_, false);
    if (!commit || !hierarchyEdit_ || !propertyHandler_) return;
    std::string nextName(hierarchyEdit_->Text());
    while (!nextName.empty() && (nextName.back() == ' ' || nextName.back() == '\t')) nextName.pop_back();
    while (!nextName.empty() && (nextName.front() == ' ' || nextName.front() == '\t')) nextName.erase(nextName.begin());
    if (nextName.empty()) return;
    for (const auto& node : hierarchyModel_.Nodes())
    {
        if (node.id == id && node.label == nextName) return;
    }
    if (propertyHandler_(id, "name", nextName))
    {
        hierarchyModel_.Rename(id, nextName);
        if (hierarchyList_)
        {
            hierarchyList_->Invalidate();
            hierarchyList_->Sync();
        }
    }
}

bool EditorShell::BeginAssetRename(const std::string& path)
{
    if (path.empty() || !assetsRenameElement_.IsValid() || !assetsRename_ || previewPlaying_)
        return false;
    const auto& entries = assetsModel_.Filtered();
    std::size_t rowIndex = entries.size();
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].path == path)
        {
            rowIndex = i;
            break;
        }
    }
    if (rowIndex >= entries.size()) return false;
    const auto& entry = entries[rowIndex];
    const std::string initialName = CleanAssetDisplayName(entry);

    const UiRect panelRect = layout_.Rect(Slot(EditorPanelId::Assets).panel);
    const UiRect contentRect = layout_.Rect(Slot(EditorPanelId::Assets).content);
    float editX = 24.0f;
    float editY = (contentRect.y - panelRect.y);
    float editW = std::max(60.0f, contentRect.width - 32.0f);
    float editH = kAssetRowHeight;

    const bool useGrid = panelRect.width >= 360.0f && assetsModel_.GridMode();
    if (useGrid)
    {
        const int cols = std::max(1, static_cast<int>((contentRect.width - 12.0f) / 84.0f));
        const int col = static_cast<int>(rowIndex) % cols;
        const int gridRow = static_cast<int>(rowIndex) / cols;
        editX = 8.0f + static_cast<float>(col) * 84.0f;
        editY = (contentRect.y - panelRect.y) + 6.0f + static_cast<float>(gridRow) * 88.0f - assetsGridScroll_ + 56.0f;
        editW = 76.0f;
        editH = 22.0f;
    }
    else if (assetsList_)
    {
        const float rowTop = static_cast<float>(rowIndex) * kAssetRowHeight;
        if (rowTop < assetsList_->Offset())
            assetsList_->SetScrollOffset(rowTop);
        else if (rowTop + kAssetRowHeight > assetsList_->Offset() + assetsList_->ViewportHeight())
            assetsList_->SetScrollOffset(rowTop + kAssetRowHeight - assetsList_->ViewportHeight());
        assetsList_->Sync();
        const bool treeMode = AssetsHasFolders() && assetsModel_.CurrentFolder().empty() &&
                              assetsModel_.Filter().empty() && assetsModel_.TypeFilter().empty();
        editX = 6.0f + static_cast<float>(entry.depth) * 14.0f + (treeMode ? 31.0f : 22.0f);
        editY = (contentRect.y - panelRect.y) + rowTop - assetsList_->Offset();
        editW = std::max(48.0f, contentRect.width - editX - 8.0f);
    }

    assetsRenamingPath_ = path;
    pendingAssetRenamePath_.clear();
    assetsRenameScroll_ = 0.0f;
    assetsRenameSelecting_ = false;
    auto style = ButtonStyle(editX, editY, editW, editH, kSurface);
    style.cornerRadius = 3.0f;
    style.borderWidth = 1.0f;
    style.borderColor = UiInk(flare::palette::workspace::Focus);
    style.padding = {4.0f, 0.0f, 4.0f, 0.0f};
    document_.SetStyle(assetsRenameElement_, style);
    assetsRename_->SetText(initialName);
    assetsRename_->Select(0, StemCodepointsForAsset(initialName, entry.type == "folder"));
    document_.SetVisible(assetsRenameElement_, true);
    PaintInlineTextEdit(*assetsRename_, assetsRenameElement_, assetsRenameSelection_,
                        assetsRenameScroll_, editW, editH);
    dispatcher_.SetFocus(assetsRenameElement_);
    return true;
}

void EditorShell::EndAssetRename(bool commit)
{
    if (assetsRenamingPath_.empty()) return;
    const std::string targetPath = assetsRenamingPath_;
    assetsRenamingPath_.clear();
    assetsRenameSelecting_ = false;
    if (dispatcher_.PointerCapture() == assetsRenameElement_)
        dispatcher_.ReleasePointerCapture();
    if (assetsRenameElement_.IsValid())
        document_.SetVisible(assetsRenameElement_, false);
    if (!commit || !assetsRename_ || !assetRenameHandler_) return;
    std::string nextName(assetsRename_->Text());
    while (!nextName.empty() && (nextName.back() == ' ' || nextName.back() == '\t')) nextName.pop_back();
    while (!nextName.empty() && (nextName.front() == ' ' || nextName.front() == '\t')) nextName.erase(nextName.begin());
    if (nextName.empty()) return;
    assetRenameHandler_(targetPath, nextName);
}

void EditorShell::SetCurrentAssetFolder(std::string folderPath)
{
    assetsModel_.SetCurrentFolder(std::move(folderPath));
    assetsGridScroll_ = 0.0f;
    assetsGridDirty_ = true;
    if (assetsList_)
    {
        assetsList_->SetScrollOffset(0.0f);
        assetsList_->Invalidate();
        assetsList_->SetItemCount(static_cast<int>(assetsModel_.Filtered().size()));
        assetsList_->Sync();
    }
    if (initialized_) UpdatePanelContentRects();
}

void EditorShell::RefreshAssetsBreadcrumb(float width)
{
    if (!assetsBreadcrumbBar_.IsValid()) return;
    const bool hasFolders = AssetsHasFolders();
    document_.SetVisible(assetsBreadcrumbBar_, hasFolders);
    if (!hasFolders) return;

    auto barStyle = AbsoluteStyle(0.0f, 24.0f, width, 20.0f);
    barStyle.width = UiLength::Percent(100.0f);
    barStyle.background = UiInk(flare::palette::workspace::Header, 0.72f);
    barStyle.borderWidth = 1.0f;
    barStyle.borderColor = kBorder;
    document_.SetStyle(assetsBreadcrumbBar_, barStyle);

    std::vector<std::pair<std::string, std::string>> crumbs;
    crumbs.emplace_back(Text("EditorUi.AllAssets", "All Assets"), std::string{});
    std::string current = assetsModel_.CurrentFolder();
    if (current.empty() && !assetsModel_.SelectedPath().empty())
    {
        for (const auto& entry : assetsModel_.Entries())
        {
            if (entry.path == assetsModel_.SelectedPath())
            {
                if (entry.type == "folder")
                    current = entry.relativeDir.empty() ? entry.displayName : (entry.relativeDir + "/" + entry.displayName);
                else
                    current = entry.relativeDir;
                break;
            }
        }
    }
    if (!current.empty())
    {
        std::string accum;
        std::size_t pos = 0;
        while (pos < current.size())
        {
            const auto slash = current.find('/', pos);
            const std::string part = current.substr(pos, slash == std::string::npos ? std::string::npos : (slash - pos));
            if (!part.empty())
            {
                accum = accum.empty() ? part : (accum + "/" + part);
                crumbs.emplace_back(part, accum);
            }
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
    }

    std::vector<std::string> desiredPaths;
    desiredPaths.reserve(crumbs.size());
    for (const auto& c : crumbs) desiredPaths.push_back(c.second);

    if (desiredPaths != assetsBreadcrumbPaths_ || assetsBreadcrumbButtons_.size() != crumbs.size())
    {
        for (const auto btn : assetsBreadcrumbButtons_)
        {
            if (btn.IsValid())
            {
                dispatcher_.RemoveHandlers(btn);
                document_.SetVisible(btn, false);
                pendingDestroy_.push_back(btn);
            }
        }
        assetsBreadcrumbButtons_.clear();
        assetsBreadcrumbPaths_ = desiredPaths;
        for (std::size_t i = 0; i < crumbs.size(); ++i)
        {
            const auto btn = document_.CreateElement(UiElementKind::Button);
            if (!document_.SetParent(btn, assetsBreadcrumbBar_)) continue;
            const std::string targetRel = crumbs[i].second;
            dispatcher_.AddHandler(btn, UiEventType::PointerDown, [this, targetRel](UiInputEvent& event) {
                if (event.button != 0) return;
                SetCurrentAssetFolder(targetRel);
                event.handled = true;
            });
            assetsBreadcrumbButtons_.push_back(btn);
        }
    }

    float x = 4.0f;
    for (std::size_t i = 0; i < assetsBreadcrumbButtons_.size() && i < crumbs.size(); ++i)
    {
        const std::string text = (i == 0 ? "" : "▸ ") + crumbs[i].first;
        const float bw = std::min(std::max(28.0f, width - x - 4.0f), MeasuredWidth(text, 11.5f) + 10.0f);
        const bool isActiveScope = (crumbs[i].second == assetsModel_.CurrentFolder());
        auto bs = ButtonStyle(x, 1.0f, bw, 18.0f, UiColor{0, 0, 0, 0});
        bs.fontSize = 11.5f;
        bs.cornerRadius = 3.0f;
        bs.padding = {4.0f, 0.0f, 4.0f, 0.0f};
        bs.color = isActiveScope ? UiInk(flare::palette::Gold) : kMuted;
        document_.SetText(assetsBreadcrumbButtons_[i], text);
        document_.SetStyle(assetsBreadcrumbButtons_[i], bs);
        x += bw + 1.0f;
    }
}

void EditorShell::RebuildAssetsGrid(float width, float height)
{
    if (!assetsGridContainer_.IsValid()) return;
    auto cs = AbsoluteStyle(0.0f, 0.0f, width, height);
    cs.width = UiLength::Percent(100.0f);
    cs.height = UiLength::Percent(100.0f);
    cs.background = kContent;
    cs.overflow = UiOverflow::Hidden;
    document_.SetStyle(assetsGridContainer_, cs);

    if (!assetsGridDirty_ && std::abs(width - lastAssetsGridWidth_) < 1.0f &&
        std::abs(height - lastAssetsGridHeight_) < 1.0f)
    {
        return;
    }
    assetsGridDirty_ = false;
    lastAssetsGridWidth_ = width;
    lastAssetsGridHeight_ = height;

    for (const auto card : assetsGridCards_)
    {
        if (card.IsValid())
        {
            dispatcher_.RemoveHandlers(card);
            document_.SetVisible(card, false);
            pendingDestroy_.push_back(card);
        }
    }
    assetsGridCards_.clear();

    const auto& entries = assetsModel_.Filtered();
    constexpr float cardW = 78.0f;
    constexpr float cardH = 84.0f;
    constexpr float gap = 6.0f;
    const int cols = std::max(1, static_cast<int>((std::max(cardW, width - 12.0f)) / (cardW + gap)));
    const int totalRows = (static_cast<int>(entries.size()) + cols - 1) / cols;
    const float maxScroll = std::max(0.0f, static_cast<float>(totalRows) * (cardH + gap) + 8.0f - height);
    assetsGridScroll_ = std::clamp(assetsGridScroll_, 0.0f, maxScroll);

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const int col = static_cast<int>(i) % cols;
        const int r = static_cast<int>(i) / cols;
        const float cx = 8.0f + static_cast<float>(col) * (cardW + gap);
        const float cy = 6.0f + static_cast<float>(r) * (cardH + gap) - assetsGridScroll_;
        if (cy + cardH < -20.0f || cy > height + 20.0f) continue;

        const auto& entry = entries[i];
        const bool selected = assetsModel_.Selected(entry.path);
        const auto card = document_.CreateElement(UiElementKind::Panel);
        if (!document_.SetParent(card, assetsGridContainer_)) continue;

        const bool folderDropHover = (static_cast<int>(i) == hoveredFolderDropRow_ && entry.type == "folder");
        auto st = AbsoluteStyle(cx, cy, cardW, cardH);
        st.background = folderDropHover
            ? UiInk(flare::palette::workspace::Focus, 0.25f)
            : (selected
                ? UiInk(activePanel_ == EditorPanelId::Assets ? flare::palette::workspace::Selection : flare::palette::workspace::InactiveSelection)
                : UiInk(flare::palette::workspace::Header, 0.55f));
        st.cornerRadius = 6.0f;
        st.borderWidth = 1.0f;
        st.borderColor = (selected || folderDropHover) ? UiInk(flare::palette::workspace::Focus) : kBorder;
        document_.SetStyle(card, st);

        BuildAssetIcon(card, (cardW - 38.0f) * 0.5f, 8.0f, 38.0f, entry);

        const auto lbl = document_.CreateElement(UiElementKind::Label);
        if (document_.SetParent(lbl, card))
        {
            const std::string name = CleanAssetDisplayName(entry);
            auto ls = AbsoluteStyle(4.0f, 52.0f, cardW - 8.0f, 28.0f);
            ls.background = UiColor{0, 0, 0, 0};
            ls.color = entry.type == "folder" ? UiInk(flare::palette::Heading) : kText;
            ls.fontSize = 11.5f;
            ls.textFlow = UiTextFlow::SingleLineEllipsis;
            ls.padding = {std::max(0.0f, (cardW - 8.0f - MeasuredWidth(name, 11.5f)) * 0.5f), 0.0f, 2.0f, 0.0f};
            ls.pointerEvents = UiPointerEvents::None;
            document_.SetStyle(lbl, ls);
            document_.SetText(lbl, name);
        }

        const std::string path = entry.path;
        const std::string type = entry.type;
        const std::string folderRel = entry.relativeDir.empty() ? entry.displayName : (entry.relativeDir + "/" + entry.displayName);
        dispatcher_.AddHandler(card, UiEventType::PointerDown, [this, path, type, folderRel](UiInputEvent& event) {
            if (!assetsRenamingPath_.empty() && assetsRenamingPath_ != path) EndAssetRename(true);
            const bool panelWasActive = (panelWasActiveBeforePointerDown_ == EditorPanelId::Assets);
            const bool wasSelectedBefore = assetsModel_.Selected(path);
            const bool alreadySelected = wasSelectedBefore && panelWasActive;
            SelectAsset(path);
            if (!wasSelectedBefore) assetsGridDirty_ = true;
            if (event.button == 1)
            {
                pendingAssetRenamePath_.clear();
                wasSelectedOnAssetDown_ = false;
                OpenAssetItemMenu(path, type, event.x, event.y);
                event.handled = true;
                event.StopPropagation();
                return;
            }
            if (event.button != 0) return;
            const std::uint64_t now = NowMilliseconds();
            const bool doubleClick = (path == lastAssetClickPath_) && (now - lastAssetClickTick_ <= kFastDoubleClickMs);
            if (doubleClick)
            {
                pendingAssetRenamePath_.clear();
                wasSelectedOnAssetDown_ = false;
                lastAssetClickPath_.clear();
                lastAssetClickTick_ = 0;
                if (type == "folder") SetCurrentAssetFolder(folderRel);
                else if (assetActivateHandler_) assetActivateHandler_(path);
                event.handled = true;
                event.StopPropagation();
                return;
            }
            wasSelectedOnAssetDown_ = alreadySelected;
            if (!alreadySelected) pendingAssetRenamePath_.clear();
            assetRenameDownTick_ = now;
            lastAssetClickPath_ = path;
            lastAssetClickTick_ = now;
            if (!assetDrag_) assetDrag_ = std::make_unique<EditorAssetDragState>();
            assetDrag_->assetPath = path;
            assetDrag_->startX = event.x;
            assetDrag_->startY = event.y;
            assetDrag_->dragging = false;
            assetDrag_->isFolder = (type == "folder");
            dispatcher_.SetPointerCapture(Slot(EditorPanelId::Assets).content);
            event.handled = true;
            event.StopPropagation();
        });
        assetsGridCards_.push_back(card);
    }
}

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
    case EditorPanelId::Game: {
        gamePlaceholder_ = document_.CreateElement(UiElementKind::Label);
        if (document_.SetParent(gamePlaceholder_, slot.content))
        {
            auto ps = ButtonStyle(12.0f, 12.0f, 220.0f, 28.0f, UiInk(flare::palette::workspace::Header, 0.75f));
            ps.color = kMuted;
            ps.cornerRadius = 6.0f;
            document_.SetStyle(gamePlaceholder_, ps);
            document_.SetText(gamePlaceholder_, Text("EditorUi.NoActiveCamera", "No active camera in scene"));
        }
        break;
    }
    case EditorPanelId::Hierarchy:
        makeList(hierarchyList_);
        if (hierarchyList_)
        {
            hierarchyList_->SetRowHeight(kHierarchyRowHeight);
            hierarchyList_->SetRowBackgroundProvider([this](int index) {
                const auto rows = hierarchyModel_.VisibleRows();
                if (index < 0 || static_cast<std::size_t>(index) >= rows.size()) return UiColor{0, 0, 0, 0};
                const std::uint64_t rowId = hierarchyModel_.Nodes()[rows[index]].id;
                if (pingedHierarchyId_ != 0 && rowId == pingedHierarchyId_ && NowMilliseconds() < pingUntilTick_)
                {
                    return UiColor{0.20f, 0.50f, 0.86f, 0.72f};
                }
                if (inspectorModel_.Target() != 0 && (rowId == inspectorModel_.Target() || hierarchyModel_.IsSelected(rowId)))
                {
                    return UiInk(activePanel_ == EditorPanelId::Hierarchy ? flare::palette::workspace::Selection
                                                                          : flare::palette::workspace::InactiveSelection);
                }
                if (index == hoveredHierarchyRow_)
                {
                    return UiInk(flare::palette::workspace::Hover, 0.55f);
                }
                return UiColor{0, 0, 0, 0};
            });
            hierarchyList_->SetRowBuilder([this](int index, UiElementHandle row) {
                BuildHierarchyRow(index, row);
            });
        }
        hierarchyEditElement_ = document_.CreateElement(UiElementKind::TextEdit);
        if (document_.SetParent(hierarchyEditElement_, slot.panel))
        {
            document_.SetStyle(hierarchyEditElement_, ButtonStyle(0.0f, 0.0f, 120.0f, kHierarchyRowHeight, kSurface));
            document_.SetVisible(hierarchyEditElement_, false);
            hierarchyEditSelection_ = document_.CreateElement(UiElementKind::Panel);
            document_.SetParent(hierarchyEditSelection_, hierarchyEditElement_);
            hierarchyEdit_ = std::make_unique<UiTextEdit>(document_, hierarchyEditElement_, text_, PlatformServices());
            const auto repaintHierarchyEdit = [this]() {
                if (!hierarchyEdit_ || !hierarchyEditElement_.IsValid()) return;
                const UiStyle st = document_.Style(hierarchyEditElement_);
                PaintInlineTextEdit(*hierarchyEdit_, hierarchyEditElement_, hierarchyEditSelection_,
                                    hierarchyEditScroll_, st.width.value, st.height.value);
            };
            dispatcher_.AddHandler(hierarchyEditElement_, UiEventType::PointerDown, [this](UiInputEvent& event) {
                dispatcher_.SetFocus(hierarchyEditElement_);
                if (event.button == 0 && hierarchyEdit_)
                {
                    const std::uint64_t now = NowMilliseconds();
                    if (now - hierarchyEditLastClickTick_ <= kFastDoubleClickMs)
                    {
                        hierarchyEdit_->Select(0, std::numeric_limits<std::uint32_t>::max());
                        const UiStyle st = document_.Style(hierarchyEditElement_);
                        PaintInlineTextEdit(*hierarchyEdit_, hierarchyEditElement_, hierarchyEditSelection_,
                                            hierarchyEditScroll_, st.width.value, st.height.value);
                        hierarchyEditLastClickTick_ = 0;
                    }
                    else
                    {
                        hierarchyEditLastClickTick_ = now;
                        hierarchyEditSelecting_ = true;
                        dispatcher_.SetPointerCapture(hierarchyEditElement_);
                        PlaceInlineTextCaret(*hierarchyEdit_, hierarchyEditElement_, hierarchyEditSelection_,
                                             hierarchyEditScroll_, event.x, event.shift);
                    }
                }
                event.handled = true;
                event.StopPropagation();
            });
            dispatcher_.AddHandler(hierarchyEditElement_, UiEventType::PointerMove, [this](UiInputEvent& event) {
                if (!hierarchyEditSelecting_ || !hierarchyEdit_) return;
                PlaceInlineTextCaret(*hierarchyEdit_, hierarchyEditElement_, hierarchyEditSelection_,
                                     hierarchyEditScroll_, event.x, true);
                event.handled = true;
            });
            dispatcher_.AddHandler(hierarchyEditElement_, UiEventType::PointerUp, [this](UiInputEvent& event) {
                if (!hierarchyEditSelecting_) return;
                hierarchyEditSelecting_ = false;
                if (dispatcher_.PointerCapture() == hierarchyEditElement_)
                    dispatcher_.ReleasePointerCapture();
                event.handled = true;
            });
            dispatcher_.AddHandler(hierarchyEditElement_, UiEventType::PointerCaptureLost, [this](UiInputEvent&) {
                hierarchyEditSelecting_ = false;
            });
            dispatcher_.AddHandler(hierarchyEditElement_, UiEventType::FocusLost, [this](UiInputEvent&) {
                EndHierarchyRename(true);
            });
            dispatcher_.AddHandler(hierarchyEditElement_, UiEventType::KeyDown, [this, repaintHierarchyEdit](UiInputEvent& event) {
                if (event.key == UiKeyEnter || event.key == UiKeyEscape)
                {
                    EndHierarchyRename(event.key == UiKeyEnter);
                    dispatcher_.ClearFocus();
                }
                else if (hierarchyEdit_)
                {
                    hierarchyEdit_->HandleKey(event.key, event.shift, event.control);
                    repaintHierarchyEdit();
                }
                event.handled = true;
            });
            dispatcher_.AddHandler(hierarchyEditElement_, UiEventType::TextInput, [this, repaintHierarchyEdit](UiInputEvent& event) {
                if (!hierarchyEdit_ || event.codepoint == 0) return;
                const char32_t cp = event.codepoint;
                std::string utf8;
                if (EncodeUtf8({&cp, 1}, utf8))
                {
                    event.handled = hierarchyEdit_->InsertText(utf8);
                    repaintHierarchyEdit();
                }
            });
            dispatcher_.AddHandler(hierarchyEditElement_, UiEventType::ImeComposition, [this, repaintHierarchyEdit](UiInputEvent& event) {
                if (hierarchyEdit_)
                {
                    hierarchyEdit_->SetComposition(event.text);
                    repaintHierarchyEdit();
                }
                event.handled = true;
            });
            dispatcher_.AddHandler(hierarchyEditElement_, UiEventType::ImeCommit, [this, repaintHierarchyEdit](UiInputEvent& event) {
                if (hierarchyEdit_)
                {
                    hierarchyEdit_->SetComposition(event.text);
                    hierarchyEdit_->CommitComposition();
                    repaintHierarchyEdit();
                }
                event.handled = true;
            });
            dispatcher_.AddHandler(hierarchyEditElement_, UiEventType::ImeCancel, [this, repaintHierarchyEdit](UiInputEvent&) {
                if (hierarchyEdit_)
                {
                    hierarchyEdit_->CancelComposition();
                    repaintHierarchyEdit();
                }
            });
        }
        dispatcher_.AddHandler(slot.content, UiEventType::PointerDown, [this](UiInputEvent& event) {
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
            const float offset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
            const int row = static_cast<int>((event.y - rect.y + offset) / kHierarchyRowHeight);
            const auto rows = hierarchyModel_.VisibleRows();
            if (hierarchyEditingId_ != 0) EndHierarchyRename(true);
            if (event.button == 1)
            {
                pendingHierarchyRenameId_ = 0;
                const auto parent = row >= 0 && static_cast<std::size_t>(row) < rows.size()
                    ? hierarchyModel_.Nodes()[rows[row]].id : 0;
                if (parent && selectionHandler_ && !hierarchyModel_.IsSelected(parent))
                    selectionHandler_(parent);
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
                    pendingHierarchyRenameId_ = 0;
                    wasSelectedOnHierarchyDown_ = false;
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
                if (event.control || event.shift)
                {
                    pendingHierarchyRenameId_ = 0;
                    wasSelectedOnHierarchyDown_ = false;
                    lastHierarchyClickId_ = 0;
                    lastHierarchyClickTick_ = 0;
                    if (event.control) hierarchyModel_.ToggleSelection(id);
                    else hierarchyModel_.SelectRange(id);
                    if (hierarchyList_) { hierarchyList_->Invalidate(); hierarchyList_->Sync(); }
                    if (multiSelectionHandler_)
                        multiSelectionHandler_(hierarchyModel_.SelectedIds(), hierarchyModel_.Selected());
                    else if (selectionHandler_)
                        selectionHandler_(hierarchyModel_.Selected());
                    event.handled = true;
                    return;
                }
                const bool panelWasActive = (panelWasActiveBeforePointerDown_ == EditorPanelId::Hierarchy);
                const bool alreadySelected = (hierarchyModel_.Selected() == id &&
                                              hierarchyModel_.SelectedIds().size() <= 1 &&
                                              panelWasActive);
                const bool hasIcon = node.prefab || !node.iconKind.empty();
                const float labelStart = 6.0f + static_cast<float>(node.depth) * 12.0f + (hasIcon ? 22.0f : 10.0f);
                const float labelEnd = labelStart + std::max(44.0f, MeasuredWidth(node.label, 13.0f) + 24.0f);
                const float localX = event.x - rect.x;
                const bool hitLabel = (localX >= labelStart - 4.0f && localX <= labelEnd);
                const bool doubleClick = (id == lastHierarchyClickId_) && (now - lastHierarchyClickTick_ <= kFastDoubleClickMs);
                lastHierarchyClickId_ = id;
                lastHierarchyClickTick_ = doubleClick ? 0 : now;
                if (doubleClick)
                {
                    pendingHierarchyRenameId_ = 0;
                    wasSelectedOnHierarchyDown_ = false;
                }
                else
                {
                    wasSelectedOnHierarchyDown_ = alreadySelected && hitLabel;
                    hierarchyRenameDownTick_ = now;
                    if (!wasSelectedOnHierarchyDown_) pendingHierarchyRenameId_ = 0;
                }
                hierarchyModel_.Select(id);
                if (hierarchyList_) { hierarchyList_->Invalidate(); hierarchyList_->Sync(); }
                dispatcher_.SetPointerCapture(Slot(EditorPanelId::Hierarchy).content);
                if (selectionHandler_) selectionHandler_(id);
                if (doubleClick && viewportAction_) viewportAction_("Frame");
            }
            else
            {
                pendingHierarchyRenameId_ = 0;
                wasSelectedOnHierarchyDown_ = false;
                if (!event.control && !event.shift)
                {
                    hierarchyModel_.Select(0);
                    if (hierarchyList_) { hierarchyList_->Invalidate(); hierarchyList_->Sync(); }
                    if (selectionHandler_) selectionHandler_(0);
                }
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerMove, [this](UiInputEvent& event) {
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
            const float offset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
            const int nextHover = (event.x >= rect.x && event.x < rect.x + rect.width &&
                                   event.y >= rect.y && event.y < rect.y + rect.height)
                ? static_cast<int>((event.y - rect.y + offset) / kHierarchyRowHeight) : -1;
            if (nextHover != hoveredHierarchyRow_)
            {
                hoveredHierarchyRow_ = nextHover;
                if (hierarchyList_) { hierarchyList_->Invalidate(); hierarchyList_->Sync(); }
            }
            if (dragHierarchyId_ == 0) return;
            if (!draggingHierarchy_ &&
                std::abs(event.x - hierarchyDragStartX_) + std::abs(event.y - hierarchyDragStartY_) > 4.0f)
            {
                draggingHierarchy_ = true;
                wasSelectedOnHierarchyDown_ = false;
                pendingHierarchyRenameId_ = 0;
                dispatcher_.SetPointerCapture(Slot(EditorPanelId::Hierarchy).content);
            }
            if (draggingHierarchy_)
            {
                if (dragGhost_.IsValid())
                {
                    if (CurrentInputRoot().IsValid()) document_.SetParent(dragGhost_, CurrentInputRoot());
                    document_.SetVisible(dragGhost_, true);
                    std::string ghostLabel;
                    for (const EditorHierarchyNode& node : hierarchyModel_.Nodes())
                    {
                        if (node.id == dragHierarchyId_)
                        {
                            ghostLabel = "◈ " + node.label;
                            break;
                        }
                    }
                    const float ghostW = std::clamp(MeasuredWidth(ghostLabel, 12.5f) + 20.0f, 80.0f, 240.0f);
                    UiStyle ghostStyle = document_.Style(dragGhost_);
                    ghostStyle.left = UiLength::Px(event.x + 8.0f);
                    ghostStyle.top = UiLength::Px(event.y);
                    ghostStyle.width = UiLength::Px(ghostW);
                    document_.SetStyle(dragGhost_, ghostStyle);
                    document_.SetText(dragGhost_, ghostLabel);
                }
                const auto insideRect = [&](const UiRect& r) {
                    return r.width > 0.0f && r.height > 0.0f &&
                           event.x >= r.x && event.x < r.x + r.width &&
                           event.y >= r.y && event.y < r.y + r.height;
                };
                UiRect highlightRect{};
                if (dock_.SurfaceFor(EditorPanelId::Inspector) == inputSurface_ &&
                    document_.Visible(Slot(EditorPanelId::Inspector).panel) &&
                    insideRect(layout_.Rect(Slot(EditorPanelId::Inspector).panel)))
                {
                    if (const auto slotIdx = InspectorSlotAt(event.x, event.y, true))
                    {
                        highlightRect = InspectorFieldRect(inspectorModel_.Properties()[*slotIdx].key);
                    }
                }
                else if (dock_.SurfaceFor(EditorPanelId::Assets) == inputSurface_ &&
                         document_.Visible(Slot(EditorPanelId::Assets).panel) &&
                         insideRect(layout_.Rect(Slot(EditorPanelId::Assets).panel)))
                {
                    highlightRect = layout_.Rect(Slot(EditorPanelId::Assets).panel);
                }
                if (dropHighlight_.IsValid())
                {
                    if (highlightRect.width > 0.0f && highlightRect.height > 0.0f)
                    {
                        if (CurrentInputRoot().IsValid()) document_.SetParent(dropHighlight_, CurrentInputRoot());
                        UiStyle hl = document_.Style(dropHighlight_);
                        hl.left = UiLength::Px(highlightRect.x);
                        hl.top = UiLength::Px(highlightRect.y);
                        hl.width = UiLength::Px(highlightRect.width);
                        hl.height = UiLength::Px(highlightRect.height);
                        document_.SetStyle(dropHighlight_, hl);
                        document_.SetVisible(dropHighlight_, true);
                    }
                    else if (!tabDragging_)
                    {
                        document_.SetVisible(dropHighlight_, false);
                    }
                }
            }
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
            if (hierarchyEditingId_ != 0) EndHierarchyRename(true);
            if (hierarchyList_)
            {
                hierarchyList_->SetScrollOffset(hierarchyList_->Offset() - event.wheel * 48.0f);
                hierarchyList_->Sync();
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerUp, [this](UiInputEvent& event) {
            const std::uint64_t draggedId = dragHierarchyId_;
            const bool wasDragging = draggingHierarchy_;
            const bool wasSelDown = wasSelectedOnHierarchyDown_;
            wasSelectedOnHierarchyDown_ = false;
            dragHierarchyId_ = 0;
            draggingHierarchy_ = false;
            if (dispatcher_.PointerCapture() == Slot(EditorPanelId::Hierarchy).content)
                dispatcher_.ReleasePointerCapture();
            if (dragGhost_.IsValid()) document_.SetVisible(dragGhost_, false);
            if (dropHighlight_.IsValid() && !tabDragging_) document_.SetVisible(dropHighlight_, false);
            if (draggedId == 0 || !wasDragging)
            {
                if (event.button == 0 && wasSelDown && draggedId != 0)
                {
                    pendingHierarchyRenameId_ = draggedId;
                    pendingHierarchyRenameDownTick_ = hierarchyRenameDownTick_;
                    pendingHierarchyRenameTick_ = NowMilliseconds();
                }
                return;
            }
            pendingHierarchyRenameId_ = 0;
            if (event.button != 0) return;

            const auto insideRect = [&](const UiRect& r) {
                return r.width > 0.0f && r.height > 0.0f &&
                       event.x >= r.x && event.x < r.x + r.width &&
                       event.y >= r.y && event.y < r.y + r.height;
            };
            if (dock_.SurfaceFor(EditorPanelId::Inspector) == inputSurface_ &&
                document_.Visible(Slot(EditorPanelId::Inspector).panel) &&
                insideRect(layout_.Rect(Slot(EditorPanelId::Inspector).panel)))
            {
                if (const auto slotIdx = InspectorSlotAt(event.x, event.y, true);
                    slotIdx && propertyHandler_ && inspectorModel_.Target() != 0)
                {
                    const std::string propKey = inspectorModel_.Properties()[*slotIdx].key;
                    propertyHandler_(inspectorModel_.Target(), propKey, std::to_string(draggedId));
                    event.handled = true;
                    return;
                }
            }
            if (dock_.SurfaceFor(EditorPanelId::Assets) == inputSurface_ &&
                document_.Visible(Slot(EditorPanelId::Assets).panel) &&
                insideRect(layout_.Rect(Slot(EditorPanelId::Assets).panel)) &&
                hierarchyAssetDropHandler_)
            {
                hierarchyAssetDropHandler_(draggedId);
                event.handled = true;
                return;
            }
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
            if (insideRect(rect))
            {
                const float offset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
                const int row = static_cast<int>((event.y - rect.y + offset) / kHierarchyRowHeight);
                const auto rows = hierarchyModel_.VisibleRows();
                if (row >= 0 && static_cast<std::size_t>(row) < rows.size())
                {
                    const std::uint64_t parent = hierarchyModel_.Nodes()[rows[static_cast<std::size_t>(row)]].id;
                    if (parent != draggedId && reparentHandler_)
                    {
                        reparentHandler_(draggedId, parent);
                    }
                }
                else if (reparentHandler_)
                {
                    reparentHandler_(draggedId, 0);
                }
            }
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerCaptureLost, [this](UiInputEvent&) {
            if (dragGhost_.IsValid()) document_.SetVisible(dragGhost_, false);
            if (dropHighlight_.IsValid() && !tabDragging_) document_.SetVisible(dropHighlight_, false);
            dragHierarchyId_ = 0;
            draggingHierarchy_ = false;
            wasSelectedOnHierarchyDown_ = false;
        }, UiEventPhase::Target);
        break;
    case EditorPanelId::Assets: {
        makeList(assetsList_);
        if (assetsList_)
        {
            assetsList_->SetRowHeight(kAssetRowHeight);
            assetsList_->SetRowBackgroundProvider([this](int index) {
                const auto& entries = assetsModel_.Filtered();
                if (index < 0 || static_cast<std::size_t>(index) >= entries.size()) return UiColor{0, 0, 0, 0};
                if (!pingedAssetPath_.empty() && entries[index].path == pingedAssetPath_ && NowMilliseconds() < pingUntilTick_)
                {
                    return UiColor{0.20f, 0.50f, 0.86f, 0.72f};
                }
                if (assetsModel_.Selected(entries[index].path))
                {
                    return UiInk(activePanel_ == EditorPanelId::Assets ? flare::palette::workspace::Selection
                                                                       : flare::palette::workspace::InactiveSelection);
                }
                if (index == hoveredAssetRow_)
                {
                    return UiInk(flare::palette::workspace::Hover, 0.55f);
                }
                return UiColor{0, 0, 0, 0};
            });
            assetsList_->SetRowBuilder([this](int index, UiElementHandle row) {
                BuildAssetRow(index, row);
            });
        }
        assetsGridContainer_ = document_.CreateElement(UiElementKind::Panel);
        if (document_.SetParent(assetsGridContainer_, slot.content))
        {
            document_.SetStyle(assetsGridContainer_, AbsoluteStyle(0.0f, 0.0f, 100.0f, 100.0f));
            document_.SetVisible(assetsGridContainer_, false);
        }
        assetsBreadcrumbBar_ = document_.CreateElement(UiElementKind::Panel);
        if (document_.SetParent(assetsBreadcrumbBar_, slot.panel))
        {
            document_.SetStyle(assetsBreadcrumbBar_, AbsoluteStyle(0.0f, 24.0f, 100.0f, 20.0f));
            document_.SetVisible(assetsBreadcrumbBar_, false);
        }
        assetsViewToggleElement_ = document_.CreateElement(UiElementKind::Button);
        if (document_.SetParent(assetsViewToggleElement_, slot.panel))
        {
            auto viewStyle = ButtonStyle(0.0f, 1.0f, 44.0f, 22.0f, kSurfaceRaised);
            viewStyle.cornerRadius = 4.0f;
            viewStyle.fontSize = 12.0f;
            document_.SetStyle(assetsViewToggleElement_, viewStyle);
            document_.SetText(assetsViewToggleElement_, Text("EditorUi.ViewList", "List"));
            document_.SetVisible(assetsViewToggleElement_, false);
            dispatcher_.AddHandler(assetsViewToggleElement_, UiEventType::PointerDown, [this](UiInputEvent& event) {
                if (event.button != 0) return;
                assetsModel_.SetGridMode(!assetsModel_.GridMode());
                assetsGridDirty_ = true;
                UpdatePanelContentRects();
                event.handled = true;
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
            assetsFilter_ = std::make_unique<UiTextEdit>(document_, assetsFilterElement_, text_, PlatformServices());
            dispatcher_.AddHandler(assetsFilterElement_, UiEventType::PointerDown, [this](UiInputEvent&) {
                dispatcher_.SetFocus(assetsFilterElement_);
            });
            const auto refreshFilter = [this]() {
                if (!assetsFilter_) return;
                assetsModel_.SetFilter(assetsFilter_->Text());
                assetsGridDirty_ = true;
                if (assetsList_)
                {
                    assetsList_->Invalidate();
                    assetsList_->SetItemCount(static_cast<int>(assetsModel_.Filtered().size()));
                }
                UpdatePanelContentRects();
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
            createStyle.color = UiInk(flare::palette::Gold);
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
            importStyle.color = UiInk(flare::palette::Gold);
            document_.SetStyle(assetsImportElement_, importStyle);
            document_.SetText(assetsImportElement_, Text("EditorUi.Import", "Import"));
            dispatcher_.AddHandler(assetsImportElement_, UiEventType::PointerDown, [this](UiInputEvent& event) {
                if (event.button != 0) return;
                if (assetImportHandler_) assetImportHandler_();
                event.handled = true;
            });
        }
        assetsRenameElement_ = document_.CreateElement(UiElementKind::TextEdit);
        if (document_.SetParent(assetsRenameElement_, slot.panel))
        {
            document_.SetStyle(assetsRenameElement_, ButtonStyle(0.0f, 0.0f, 120.0f, kAssetRowHeight, kSurface));
            document_.SetVisible(assetsRenameElement_, false);
            assetsRenameSelection_ = document_.CreateElement(UiElementKind::Panel);
            document_.SetParent(assetsRenameSelection_, assetsRenameElement_);
            assetsRename_ = std::make_unique<UiTextEdit>(document_, assetsRenameElement_, text_, PlatformServices());
            const auto repaintAssetRename = [this]() {
                if (!assetsRename_ || !assetsRenameElement_.IsValid()) return;
                const UiStyle st = document_.Style(assetsRenameElement_);
                PaintInlineTextEdit(*assetsRename_, assetsRenameElement_, assetsRenameSelection_,
                                    assetsRenameScroll_, st.width.value, st.height.value);
            };
            dispatcher_.AddHandler(assetsRenameElement_, UiEventType::PointerDown, [this](UiInputEvent& event) {
                dispatcher_.SetFocus(assetsRenameElement_);
                if (event.button == 0 && assetsRename_)
                {
                    const std::uint64_t now = NowMilliseconds();
                    if (now - assetsRenameLastClickTick_ <= kFastDoubleClickMs)
                    {
                        assetsRename_->Select(0, std::numeric_limits<std::uint32_t>::max());
                        const UiStyle st = document_.Style(assetsRenameElement_);
                        PaintInlineTextEdit(*assetsRename_, assetsRenameElement_, assetsRenameSelection_,
                                            assetsRenameScroll_, st.width.value, st.height.value);
                        assetsRenameLastClickTick_ = 0;
                    }
                    else
                    {
                        assetsRenameLastClickTick_ = now;
                        assetsRenameSelecting_ = true;
                        dispatcher_.SetPointerCapture(assetsRenameElement_);
                        PlaceInlineTextCaret(*assetsRename_, assetsRenameElement_, assetsRenameSelection_,
                                             assetsRenameScroll_, event.x, event.shift);
                    }
                }
                event.handled = true;
                event.StopPropagation();
            });
            dispatcher_.AddHandler(assetsRenameElement_, UiEventType::PointerMove, [this](UiInputEvent& event) {
                if (!assetsRenameSelecting_ || !assetsRename_) return;
                PlaceInlineTextCaret(*assetsRename_, assetsRenameElement_, assetsRenameSelection_,
                                     assetsRenameScroll_, event.x, true);
                event.handled = true;
            });
            dispatcher_.AddHandler(assetsRenameElement_, UiEventType::PointerUp, [this](UiInputEvent& event) {
                if (!assetsRenameSelecting_) return;
                assetsRenameSelecting_ = false;
                if (dispatcher_.PointerCapture() == assetsRenameElement_)
                    dispatcher_.ReleasePointerCapture();
                event.handled = true;
            });
            dispatcher_.AddHandler(assetsRenameElement_, UiEventType::PointerCaptureLost, [this](UiInputEvent&) {
                assetsRenameSelecting_ = false;
            });
            dispatcher_.AddHandler(assetsRenameElement_, UiEventType::FocusLost, [this](UiInputEvent&) {
                EndAssetRename(true);
            });
            dispatcher_.AddHandler(assetsRenameElement_, UiEventType::KeyDown, [this, repaintAssetRename](UiInputEvent& event) {
                if (event.key == UiKeyEnter || event.key == UiKeyEscape)
                {
                    EndAssetRename(event.key == UiKeyEnter);
                    dispatcher_.ClearFocus();
                }
                else if (assetsRename_)
                {
                    assetsRename_->HandleKey(event.key, event.shift, event.control);
                    repaintAssetRename();
                }
                event.handled = true;
            });
            dispatcher_.AddHandler(assetsRenameElement_, UiEventType::TextInput, [this, repaintAssetRename](UiInputEvent& event) {
                if (!assetsRename_ || event.codepoint == 0) return;
                const char32_t cp = event.codepoint;
                std::string utf8;
                if (EncodeUtf8({&cp, 1}, utf8))
                {
                    event.handled = assetsRename_->InsertText(utf8);
                    repaintAssetRename();
                }
            });
            dispatcher_.AddHandler(assetsRenameElement_, UiEventType::ImeComposition, [this, repaintAssetRename](UiInputEvent& event) {
                if (assetsRename_)
                {
                    assetsRename_->SetComposition(event.text);
                    repaintAssetRename();
                }
                event.handled = true;
            });
            dispatcher_.AddHandler(assetsRenameElement_, UiEventType::ImeCommit, [this, repaintAssetRename](UiInputEvent& event) {
                if (assetsRename_)
                {
                    assetsRename_->SetComposition(event.text);
                    assetsRename_->CommitComposition();
                    repaintAssetRename();
                }
                event.handled = true;
            });
            dispatcher_.AddHandler(assetsRenameElement_, UiEventType::ImeCancel, [this, repaintAssetRename](UiInputEvent&) {
                if (assetsRename_)
                {
                    assetsRename_->CancelComposition();
                    repaintAssetRename();
                }
            });
        }
        assetsPreview_ = document_.CreateElement(UiElementKind::Label);
        if (document_.SetParent(assetsPreview_, slot.panel))
        {
            document_.SetStyle(assetsPreview_, ButtonStyle(0.0f, 0.0f, 100.0f, 0.0f, kSurfaceRaised));
            document_.SetText(assetsPreview_, Text("EditorUi.Preview", "Preview") + ": " + Text("EditorUi.None", "none"));
            document_.SetVisible(assetsPreview_, false);
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
            if (!assetsRenamingPath_.empty()) EndAssetRename(true);
            const UiRect panelRect = layout_.Rect(Slot(EditorPanelId::Assets).panel);
            if (event.control && panelRect.width >= 360.0f)
            {
                assetsModel_.SetGridMode(event.wheel > 0.0f);
                assetsGridDirty_ = true;
                UpdatePanelContentRects();
                event.handled = true;
                return;
            }
            if (panelRect.width >= 360.0f && assetsModel_.GridMode())
            {
                assetsGridScroll_ = std::max(0.0f, assetsGridScroll_ - event.wheel * 56.0f);
                assetsGridDirty_ = true;
                UpdatePanelContentRects();
                event.handled = true;
                return;
            }
            if (assetsList_)
            {
                assetsList_->SetScrollOffset(assetsList_->Offset() - event.wheel * 48.0f);
                assetsList_->Sync();
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerDown, [this](UiInputEvent& event) {
            if (!assetsRenamingPath_.empty()) EndAssetRename(true);
            const UiRect panelRect = layout_.Rect(Slot(EditorPanelId::Assets).panel);
            if (panelRect.width >= 360.0f && assetsModel_.GridMode())
            {
                if (event.button == 1)
                {
                    OpenCreateMenu(EditorPanelId::Assets, event.x, event.y);
                    event.handled = true;
                }
                return;
            }
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Assets).content);
            const float offset = assetsList_ ? assetsList_->Offset() : 0.0f;
            const int row = static_cast<int>((event.y - rect.y + offset) / kAssetRowHeight);
            const auto& entries = assetsModel_.Filtered();
            if (event.button != 0 && event.button != 1) return;
            if (row >= 0 && static_cast<std::size_t>(row) < entries.size())
            {
                const auto& entry = entries[static_cast<std::size_t>(row)];
                const std::string path = entry.path;
                const std::string type = entry.type;
                const bool treeMode = AssetsHasFolders() && assetsModel_.CurrentFolder().empty() &&
                                      assetsModel_.Filter().empty() && assetsModel_.TypeFilter().empty();
                const float arrowRight = 18.0f + static_cast<float>(entry.depth) * 14.0f;
                if (event.button == 0 && treeMode && type == "folder" && entry.hasChildren &&
                    event.x - rect.x <= arrowRight)
                {
                    pendingAssetRenamePath_.clear();
                    wasSelectedOnAssetDown_ = false;
                    assetsModel_.ToggleFolder(path);
                    if (assetsList_)
                    {
                        assetsList_->Invalidate();
                        assetsList_->SetItemCount(static_cast<int>(assetsModel_.Filtered().size()));
                        assetsList_->Sync();
                    }
                    event.handled = true;
                    return;
                }
                const bool panelWasActive = (panelWasActiveBeforePointerDown_ == EditorPanelId::Assets);
                const bool alreadySelected = assetsModel_.Selected(path) && panelWasActive;
                const float indent = 6.0f + static_cast<float>(entry.depth) * 14.0f;
                const float iconX = indent + (treeMode ? 12.0f : 2.0f);
                const float labelStart = iconX + 19.0f;
                const float labelEnd = labelStart + std::max(48.0f, MeasuredWidth(CleanAssetDisplayName(entry), 13.0f) + 24.0f);
                const float localX = event.x - rect.x;
                const bool hitLabel = (localX >= labelStart - 4.0f && localX <= labelEnd);
                SelectAsset(path);
                if (event.button == 1)
                {
                    pendingAssetRenamePath_.clear();
                    wasSelectedOnAssetDown_ = false;
                    OpenAssetItemMenu(path, type, event.x, event.y);
                    event.handled = true;
                    return;
                }
                const std::uint64_t now = NowMilliseconds();
                const bool doubleClick = (path == lastAssetClickPath_) && (now - lastAssetClickTick_ <= kFastDoubleClickMs);
                if (doubleClick)
                {
                    pendingAssetRenamePath_.clear();
                    wasSelectedOnAssetDown_ = false;
                    lastAssetClickPath_.clear();
                    lastAssetClickTick_ = 0;
                    if (type == "folder")
                    {
                        if (treeMode)
                        {
                            assetsModel_.ToggleFolder(path);
                            if (assetsList_)
                            {
                                assetsList_->Invalidate();
                                assetsList_->SetItemCount(static_cast<int>(assetsModel_.Filtered().size()));
                                assetsList_->Sync();
                            }
                        }
                        else
                        {
                            const std::string folderRel = entry.relativeDir.empty()
                                ? entry.displayName : (entry.relativeDir + "/" + entry.displayName);
                            SetCurrentAssetFolder(folderRel);
                        }
                    }
                    else if (assetActivateHandler_)
                    {
                        assetActivateHandler_(path);
                    }
                    event.handled = true;
                    return;
                }
                wasSelectedOnAssetDown_ = alreadySelected && hitLabel;
                if (!wasSelectedOnAssetDown_) pendingAssetRenamePath_.clear();
                assetRenameDownTick_ = now;
                lastAssetClickPath_ = path;
                lastAssetClickTick_ = now;
                if (!assetDrag_) assetDrag_ = std::make_unique<EditorAssetDragState>();
                assetDrag_->assetPath = path;
                assetDrag_->startX = event.x;
                assetDrag_->startY = event.y;
                assetDrag_->dragging = false;
                assetDrag_->isFolder = (type == "folder");
                dispatcher_.SetPointerCapture(Slot(EditorPanelId::Assets).content);
                event.handled = true;
                return;
            }
            if (event.button == 1)
            {
                pendingAssetRenamePath_.clear();
                OpenCreateMenu(EditorPanelId::Assets, event.x, event.y);
                event.handled = true;
            }
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerMove, [this](UiInputEvent& event) {
            const UiRect panelRect = layout_.Rect(Slot(EditorPanelId::Assets).panel);
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Assets).content);
            const bool useGrid = panelRect.width >= 360.0f && assetsModel_.GridMode();
            int nextHover = -1;
            if (event.x >= rect.x && event.x < rect.x + rect.width &&
                event.y >= rect.y && event.y < rect.y + rect.height)
            {
                if (useGrid)
                {
                    constexpr float cardW = 78.0f, cardH = 84.0f, gap = 6.0f;
                    const int cols = std::max(1, static_cast<int>((std::max(cardW, rect.width - 12.0f)) / (cardW + gap)));
                    const float gx = event.x - rect.x - 8.0f;
                    const float gy = event.y - rect.y - 6.0f + assetsGridScroll_;
                    if (gx >= 0.0f && gy >= 0.0f)
                    {
                        const int col = static_cast<int>(gx / (cardW + gap));
                        const int rowIdx = static_cast<int>(gy / (cardH + gap));
                        if (col >= 0 && col < cols &&
                            (gx - static_cast<float>(col) * (cardW + gap)) <= cardW &&
                            (gy - static_cast<float>(rowIdx) * (cardH + gap)) <= cardH)
                        {
                            nextHover = rowIdx * cols + col;
                        }
                    }
                }
                else
                {
                    const float offset = assetsList_ ? assetsList_->Offset() : 0.0f;
                    nextHover = static_cast<int>((event.y - rect.y + offset) / kAssetRowHeight);
                }
            }
            if (nextHover != hoveredAssetRow_)
            {
                hoveredAssetRow_ = nextHover;
                if (!useGrid && assetsList_) { assetsList_->Invalidate(); assetsList_->Sync(); }
            }
            if (!assetDrag_ || assetDrag_->assetPath.empty()) return;
            const float dx = event.x - assetDrag_->startX;
            const float dy = event.y - assetDrag_->startY;
            if (!assetDrag_->dragging && std::abs(dx) + std::abs(dy) > 4.0f)
            {
                assetDrag_->dragging = true;
                wasSelectedOnAssetDown_ = false;
                pendingAssetRenamePath_.clear();
                lastAssetClickPath_.clear();
                lastAssetClickTick_ = 0;
            }
            if (!assetDrag_->dragging || assetDrag_->isFolder) return;
            if (dragGhost_.IsValid())
            {
                if (CurrentInputRoot().IsValid()) document_.SetParent(dragGhost_, CurrentInputRoot());
                document_.SetVisible(dragGhost_, true);
                const auto sep = assetDrag_->assetPath.find_last_of("/\\");
                const std::string ghostLabel = assetDrag_->assetPath.substr(sep == std::string::npos ? 0 : sep + 1);
                const float ghostW = std::clamp(MeasuredWidth(ghostLabel, 12.5f) + 20.0f, 80.0f, 240.0f);
                UiStyle ghostStyle = document_.Style(dragGhost_);
                ghostStyle.left = UiLength::Px(event.x + 16.0f);
                ghostStyle.top = UiLength::Px(event.y + 18.0f);
                ghostStyle.width = UiLength::Px(ghostW);
                document_.SetStyle(dragGhost_, ghostStyle);
                document_.SetText(dragGhost_, ghostLabel);
            }
            const auto target = UiHitTest(document_, layout_).Hit(CurrentInputRoot(), event.x, event.y).target;
            const bool overViewport = target == ViewportPanel();
            if (assetDrag_->hoverHandler) assetDrag_->hoverHandler(assetDrag_->assetPath, event.x, event.y, overViewport);

            const auto insideRect = [&](const UiRect& r) {
                return r.width > 0.0f && r.height > 0.0f &&
                       event.x >= r.x && event.x < r.x + r.width &&
                       event.y >= r.y && event.y < r.y + r.height;
            };

            int nextFolderRow = -1;
            if (dock_.SurfaceFor(EditorPanelId::Assets) == inputSurface_ &&
                document_.Visible(Slot(EditorPanelId::Assets).panel) && insideRect(rect))
            {
                const auto& entries = assetsModel_.Filtered();
                if (nextHover >= 0 && static_cast<std::size_t>(nextHover) < entries.size() &&
                    entries[static_cast<std::size_t>(nextHover)].type == "folder")
                {
                    nextFolderRow = nextHover;
                }
            }
            if (nextFolderRow != hoveredFolderDropRow_)
            {
                hoveredFolderDropRow_ = nextFolderRow;
                if (useGrid) { assetsGridDirty_ = true; UpdatePanelContentRects(); }
                else if (assetsList_) { assetsList_->Invalidate(); assetsList_->Sync(); }
            }

            const UiRect hierPanel = layout_.Rect(Slot(EditorPanelId::Hierarchy).panel);
            const UiRect hierContent = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
            const bool overHierarchyPanel = dock_.SurfaceFor(EditorPanelId::Hierarchy) == inputSurface_ &&
                                            document_.Visible(Slot(EditorPanelId::Hierarchy).panel) &&
                                            insideRect(hierPanel);
            const bool overHierarchy = overHierarchyPanel && insideRect(hierContent);
            const float hOffset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
            const auto visibleHierarchyRows = hierarchyModel_.VisibleRows();
            int hoveredEntityRow = -1;
            if (overHierarchy)
            {
                const int rawRow = static_cast<int>((event.y - hierContent.y + hOffset) / kHierarchyRowHeight);
                if (rawRow >= 0 && static_cast<std::size_t>(rawRow) < visibleHierarchyRows.size())
                {
                    hoveredEntityRow = rawRow;
                }
            }
            if (hoveredEntityRow != hoveredHierarchyRow_)
            {
                hoveredHierarchyRow_ = hoveredEntityRow;
                if (hierarchyList_) { hierarchyList_->Invalidate(); hierarchyList_->Sync(); }
            }
            const UiRect inspPanel = layout_.Rect(Slot(EditorPanelId::Inspector).panel);
            const bool overInspector = dock_.SurfaceFor(EditorPanelId::Inspector) == inputSurface_ &&
                                       document_.Visible(Slot(EditorPanelId::Inspector).panel) &&
                                       insideRect(inspPanel);
            assetDragCursor_ = EditorDragCursor::None;
            if (nextFolderRow >= 0)
            {
                assetDragCursor_ = EditorDragCursor::Allow;
            }
            else if (overViewport)
            {
                const bool canDropViewport =
                    IsAssetPathCompatible(assetDrag_->assetPath, "model") ||
                    IsAssetPathCompatible(assetDrag_->assetPath, "prefab") ||
                    IsAssetPathCompatible(assetDrag_->assetPath, "material") ||
                    IsAssetPathCompatible(assetDrag_->assetPath, "tod") ||
                    IsAssetPathCompatible(assetDrag_->assetPath, "scene") ||
                    IsAssetPathCompatible(assetDrag_->assetPath, "script");
                assetDragCursor_ = canDropViewport ? EditorDragCursor::Allow : EditorDragCursor::Forbid;
            }
            if (dropHighlight_.IsValid())
            {
                if (overInspector || overHierarchyPanel)
                {
                    bool showHighlight = false;
                    bool compatible = false;
                    bool outlineOnly = false;
                    UiRect highlightRect{};
                    if (overInspector)
                    {
                        if (const auto slotIdx = InspectorSlotAt(event.x, event.y, false))
                        {
                            const auto& slotProp = inspectorModel_.Properties()[*slotIdx];
                            const UiRect slotRect = InspectorFieldRect(slotProp.key);
                            if (slotRect.width > 0.0f && slotRect.height > 0.0f)
                            {
                                highlightRect = slotRect;
                                showHighlight = true;
                                compatible = IsAssetPathCompatible(assetDrag_->assetPath, slotProp.acceptedAssetKind);
                            }
                        }
                        else if (inspectorModel_.Target() != 0 &&
                                 (IsAssetPathCompatible(assetDrag_->assetPath, "script") ||
                                  IsAssetPathCompatible(assetDrag_->assetPath, "material")))
                        {
                            highlightRect = inspPanel;
                            showHighlight = true;
                            compatible = true;
                            outlineOnly = true;
                        }
                        assetDragCursor_ = compatible ? EditorDragCursor::Allow : EditorDragCursor::Forbid;
                    }
                    else
                    {
                        const bool isEntityAttachAsset = IsAssetPathCompatible(assetDrag_->assetPath, "script") ||
                                                         IsAssetPathCompatible(assetDrag_->assetPath, "material");
                        const bool isSceneCreateAsset = IsAssetPathCompatible(assetDrag_->assetPath, "model") ||
                                                        IsAssetPathCompatible(assetDrag_->assetPath, "prefab");
                        if (hoveredEntityRow >= 0 && (isEntityAttachAsset || isSceneCreateAsset))
                        {
                            const float rowTop = hierContent.y + static_cast<float>(hoveredEntityRow) * kHierarchyRowHeight - hOffset;
                            const float clipTop = std::max(hierContent.y, rowTop);
                            const float clipBottom = std::min(hierContent.y + hierContent.height, rowTop + kHierarchyRowHeight);
                            if (clipBottom > clipTop)
                            {
                                highlightRect = UiRect{hierContent.x + 1.0f, clipTop,
                                                       std::max(0.0f, hierContent.width - 2.0f), clipBottom - clipTop};
                                showHighlight = true;
                                compatible = true;
                            }
                        }
                        else if (overHierarchy && isSceneCreateAsset)
                        {
                            highlightRect = hierContent;
                            showHighlight = true;
                            compatible = true;
                            outlineOnly = true;
                        }
                        assetDragCursor_ = compatible ? EditorDragCursor::Allow : EditorDragCursor::Forbid;
                    }
                    if (showHighlight)
                    {
                        if (CurrentInputRoot().IsValid()) document_.SetParent(dropHighlight_, CurrentInputRoot());
                        UiStyle hl = document_.Style(dropHighlight_);
                        hl.left = UiLength::Px(highlightRect.x);
                        hl.top = UiLength::Px(highlightRect.y);
                        hl.width = UiLength::Px(highlightRect.width);
                        hl.height = UiLength::Px(highlightRect.height);
                        hl.borderWidth = 1.5f;
                        if (compatible)
                        {
                            hl.background = outlineOnly ? UiColor{0.0f, 0.0f, 0.0f, 0.0f}
                                                        : UiColor{0.18f, 0.72f, 0.46f, 0.10f};
                            hl.borderColor = UiColor{0.24f, 0.82f, 0.52f, 0.92f};
                        }
                        else
                        {
                            hl.background = UiColor{0.84f, 0.24f, 0.24f, 0.12f};
                            hl.borderColor = UiColor{0.88f, 0.30f, 0.30f, 0.92f};
                        }
                        document_.SetStyle(dropHighlight_, hl);
                        document_.SetVisible(dropHighlight_, true);
                    }
                    else
                    {
                        document_.SetVisible(dropHighlight_, false);
                    }
                }
                else
                {
                    document_.SetVisible(dropHighlight_, false);
                }
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerUp, [this](UiInputEvent& event) {
            if (!assetDrag_ || assetDrag_->assetPath.empty())
            {
                if (event.button == 0 && wasSelectedOnAssetDown_ && !assetsModel_.SelectedPath().empty())
                {
                    pendingAssetRenamePath_ = assetsModel_.SelectedPath();
                    pendingAssetRenameDownTick_ = assetRenameDownTick_;
                    pendingAssetRenameTick_ = NowMilliseconds();
                }
                wasSelectedOnAssetDown_ = false;
                return;
            }
            const bool dropped = assetDrag_->dragging && !assetDrag_->isFolder && event.button == 0;
            const bool wasFolder = assetDrag_->isFolder;
            const bool wasDragged = assetDrag_->dragging;
            const std::string path = assetDrag_->assetPath;
            const auto dropHandler = assetDrag_->dropHandler;
            const int folderDropIdx = hoveredFolderDropRow_;
            CancelAssetDrag();
            if (!dropped)
            {
                if (event.button == 0 && !wasDragged && wasSelectedOnAssetDown_ && !path.empty())
                {
                    pendingAssetRenamePath_ = path;
                    pendingAssetRenameDownTick_ = assetRenameDownTick_;
                    pendingAssetRenameTick_ = NowMilliseconds();
                }
                (void)wasFolder;
                wasSelectedOnAssetDown_ = false;
                event.handled = true;
                return;
            }
            wasSelectedOnAssetDown_ = false;

            const auto insideRect = [&](const UiRect& r) {
                return r.width > 0.0f && r.height > 0.0f &&
                       event.x >= r.x && event.x < r.x + r.width &&
                       event.y >= r.y && event.y < r.y + r.height;
            };
            if (assetFolderDropHandler_)
            {
                const auto& entries = assetsModel_.Filtered();
                if (folderDropIdx >= 0 && static_cast<std::size_t>(folderDropIdx) < entries.size() &&
                    entries[static_cast<std::size_t>(folderDropIdx)].type == "folder")
                {
                    assetFolderDropHandler_(path, entries[static_cast<std::size_t>(folderDropIdx)].path);
                    event.handled = true;
                    return;
                }
                if (assetsBreadcrumbBar_.IsValid() && document_.Visible(assetsBreadcrumbBar_))
                {
                    for (std::size_t bi = 0; bi < assetsBreadcrumbButtons_.size() && bi < assetsBreadcrumbPaths_.size(); ++bi)
                    {
                        if (insideRect(layout_.Rect(assetsBreadcrumbButtons_[bi])))
                        {
                            assetFolderDropHandler_(path, "@crumb:" + assetsBreadcrumbPaths_[bi]);
                            event.handled = true;
                            return;
                        }
                    }
                }
            }

            const auto target = UiHitTest(document_, layout_).Hit(CurrentInputRoot(), event.x, event.y).target;
            if (target == ViewportPanel() && dropHandler)
            {
                dropHandler(path, event.x, event.y);
            }
            else if (assetEntityDropHandler_ || assetInspectorSlotDropHandler_)
            {
                const UiRect hierPanel = layout_.Rect(Slot(EditorPanelId::Hierarchy).panel);
                const UiRect inspPanel = layout_.Rect(Slot(EditorPanelId::Inspector).panel);
                if (assetEntityDropHandler_ &&
                    dock_.SurfaceFor(EditorPanelId::Hierarchy) == inputSurface_ &&
                    document_.Visible(Slot(EditorPanelId::Hierarchy).panel) && insideRect(hierPanel))
                {
                    const UiRect hierContent = layout_.Rect(Slot(EditorPanelId::Hierarchy).content);
                    const float hOffset = hierarchyList_ ? hierarchyList_->Offset() : 0.0f;
                    const int hRow = insideRect(hierContent)
                        ? static_cast<int>((event.y - hierContent.y + hOffset) / kHierarchyRowHeight) : -1;
                    const auto rows = hierarchyModel_.VisibleRows();
                    const std::uint64_t entityId = (hRow >= 0 && static_cast<std::size_t>(hRow) < rows.size())
                        ? hierarchyModel_.Nodes()[rows[static_cast<std::size_t>(hRow)]].id : 0;
                    assetEntityDropHandler_(path, entityId, EditorPanelId::Hierarchy);
                }
                else if (dock_.SurfaceFor(EditorPanelId::Inspector) == inputSurface_ &&
                         document_.Visible(Slot(EditorPanelId::Inspector).panel) && insideRect(inspPanel))
                {
                    bool slotHandled = false;
                    bool slotRejected = false;
                    if (assetInspectorSlotDropHandler_)
                    {
                        if (const auto slotIdx = InspectorSlotAt(event.x, event.y, false))
                        {
                            const auto& slotProp = inspectorModel_.Properties()[*slotIdx];
                            if (!IsAssetPathCompatible(path, slotProp.acceptedAssetKind))
                            {
                                SetStatus(Text("EditorUi.DropIncompatible", "Asset type does not match target slot"));
                                slotRejected = true;
                            }
                            else
                            {
                                slotHandled = assetInspectorSlotDropHandler_(
                                    path, inspectorModel_.Target(), slotProp.key);
                            }
                        }
                    }
                    if (!slotHandled && !slotRejected && assetEntityDropHandler_)
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
                if (index < 0 || static_cast<std::size_t>(index) >= consoleDisplay_.size()) return UiColor{0, 0, 0, 0};
                const auto& row = consoleDisplay_[static_cast<std::size_t>(index)];
                if (row.level == 2)
                    return UiColor{0.42f, 0.11f, 0.13f, 0.42f};
                if (row.level == 1)
                    return UiColor{0.42f, 0.28f, 0.07f, 0.36f};
                return (index % 2 == 1) ? UiColor{1.0f, 1.0f, 1.0f, 0.018f} : UiColor{0, 0, 0, 0};
            });
            console_->SetRowTextProvider([this](int index) {
                if (index < 0 || static_cast<std::size_t>(index) >= consoleDisplay_.size()) return std::string{};
                const auto& row = consoleDisplay_[static_cast<std::size_t>(index)];
                return row.count > 1 ? (row.text + "  (×" + std::to_string(row.count) + ")") : row.text;
            });
            console_->SetItemCount(0);
        }
        const auto makeConsoleBtn = [&](UiElementHandle& handle, const std::string& label, auto onClick) {
            handle = document_.CreateElement(UiElementKind::Button);
            if (!document_.SetParent(handle, slot.panel)) return;
            auto st = ButtonStyle(0.0f, 2.0f, 52.0f, 20.0f, kSurfaceRaised);
            st.fontSize = 11.5f;
            st.borderWidth = 1.0f;
            st.borderColor = kBorder;
            st.cornerRadius = 4.0f;
            document_.SetStyle(handle, st);
            document_.SetText(handle, label);
            dispatcher_.AddHandler(handle, UiEventType::PointerDown, [onClick = std::move(onClick)](UiInputEvent& event) {
                if (event.button != 0) return;
                onClick();
                event.handled = true;
            });
        };
        makeConsoleBtn(consoleClearButton_, Text("EditorUi.ClearConsole", "Clear"), [this]() {
            ClearConsole();
        });
        makeConsoleBtn(consoleCollapseButton_, Text("EditorUi.ConsoleCollapse", "Collapse"), [this]() {
            consoleCollapse_ = !consoleCollapse_;
            RebuildConsoleDisplay();
        });
        makeConsoleBtn(consoleClearOnPlayButton_, Text("EditorUi.ConsoleClearOnPlay", "Clear on Play"), [this]() {
            consoleClearOnPlay_ = !consoleClearOnPlay_;
            LayoutConsoleToolbar(layout_.Rect(Slot(EditorPanelId::Console).panel).width);
        });
        makeConsoleBtn(consoleInfoButton_, Text("EditorUi.ConsoleInfo", "Info") + " 0", [this]() {
            consoleShowInfo_ = !consoleShowInfo_;
            RebuildConsoleDisplay();
        });
        makeConsoleBtn(consoleWarnButton_, Text("EditorUi.ConsoleWarn", "Warn") + " 0", [this]() {
            consoleShowWarn_ = !consoleShowWarn_;
            RebuildConsoleDisplay();
        });
        makeConsoleBtn(consoleErrorButton_, Text("EditorUi.ConsoleError", "Error") + " 0", [this]() {
            consoleShowError_ = !consoleShowError_;
            RebuildConsoleDisplay();
        });
        dispatcher_.AddHandler(slot.content, UiEventType::PointerDown, [this](UiInputEvent& event) {
            if (event.button != 0 || !console_) return;
            const UiRect rect = layout_.Rect(Slot(EditorPanelId::Console).content);
            const int row = static_cast<int>((event.y - rect.y + console_->Offset()) / console_->RowHeight());
            if (row < 0 || static_cast<std::size_t>(row) >= consoleDisplay_.size()) return;
            const std::uint64_t now = NowMilliseconds();
            const bool doubleClick = (row == lastConsoleClickRow_) && (now - lastConsoleClickTick_ <= 380);
            lastConsoleClickRow_ = row;
            lastConsoleClickTick_ = doubleClick ? 0 : now;
            if (doubleClick && consoleActivateHandler_)
            {
                consoleActivateHandler_(consoleDisplay_[static_cast<std::size_t>(row)].text);
            }
            event.handled = true;
        }, UiEventPhase::Bubble);
        dispatcher_.AddHandler(slot.content, UiEventType::PointerWheel, [this](UiInputEvent& event) {
            if (console_)
            {
                console_->SetScrollOffset(console_->Offset() - event.wheel * 48.0f);
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
    style.background = UiInk(flare::palette::Silver, 0.72f);
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
        if (panel == EditorPanelId::Assets && assetsGridContainer_.IsValid() && document_.Visible(assetsGridContainer_))
        {
            if (document_.Visible(thumb)) document_.SetVisible(thumb, false);
            continue;
        }
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
    if (!hierarchyModel_.SetNodes(std::move(nodes))) return;
    if (hierarchyList_)
    {
        hierarchyList_->Invalidate();
        hierarchyList_->SetItemCount(static_cast<int>(hierarchyModel_.VisibleRows().size()));
        hierarchyList_->Sync();
    }
}

void EditorShell::SetHierarchySelection(const std::vector<std::uint64_t>& ids, std::uint64_t primary)
{
    if (hierarchyModel_.SelectedIds() == ids && hierarchyModel_.Selected() == primary) return;
    hierarchyModel_.SetSelection(ids, primary);
    if (hierarchyList_)
    {
        hierarchyList_->Invalidate();
        hierarchyList_->Sync();
    }
}

void EditorShell::SetAssets(std::vector<EditorAssetEntry> entries)
{
    if (entries == assetsModel_.Entries()) return;
    assetsModel_.SetEntries(std::move(entries));
    assetsGridDirty_ = true;
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
    assetsGridDirty_ = true;
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
    if (slot.content.IsValid())
    {
        UiStyle st = document_.Style(slot.content);
        st.padding = {16.0f, 14.0f, 16.0f, 14.0f};
        st.textFlow = UiTextFlow::Wrap;
        document_.SetStyle(slot.content, st);
        document_.SetText(slot.content, std::move(text));
    }
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
    assetDragCursor_ = EditorDragCursor::None;
    if (dragGhost_.IsValid()) document_.SetVisible(dragGhost_, false);
    if (dropHighlight_.IsValid() && !tabDragging_)
    {
        UiStyle hl = document_.Style(dropHighlight_);
        hl.background = UiInk(flare::palette::workspace::Focus, 0.15f);
        hl.borderWidth = 2.0f;
        hl.borderColor = UiInk(flare::palette::workspace::Focus);
        document_.SetStyle(dropHighlight_, hl);
        document_.SetVisible(dropHighlight_, false);
    }
    if (hoveredFolderDropRow_ != -1)
    {
        hoveredFolderDropRow_ = -1;
        if (assetsList_)
        {
            assetsList_->Invalidate();
            assetsList_->Sync();
        }
    }
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
        if (assetDrag_->dragging && !assetDrag_->isFolder && assetDrag_->hoverHandler)
            assetDrag_->hoverHandler({}, 0.0f, 0.0f, false);
        assetDrag_->assetPath.clear();
        assetDrag_->dragging = false;
        assetDrag_->isFolder = false;
    }
    if (dispatcher_.PointerCapture() == Slot(EditorPanelId::Assets).content)
        dispatcher_.ReleasePointerCapture();
}

void EditorShell::RebuildConsoleDisplay()
{
    consoleDisplay_.clear();
    consoleInfoCount_ = 0;
    consoleWarnCount_ = 0;
    consoleErrorCount_ = 0;
    for (std::size_t i = 0; i < lines_.size(); ++i)
    {
        const std::string& line = lines_[i];
        int level = 0;
        if (ContainsToken(line, "[Error]") || ContainsToken(line, ": error "))
        {
            level = 2;
            ++consoleErrorCount_;
        }
        else if (ContainsToken(line, "[Warning]") || ContainsToken(line, ": warning "))
        {
            level = 1;
            ++consoleWarnCount_;
        }
        else
        {
            ++consoleInfoCount_;
        }

        if ((level == 0 && !consoleShowInfo_) ||
            (level == 1 && !consoleShowWarn_) ||
            (level == 2 && !consoleShowError_))
        {
            continue;
        }

        if (consoleCollapse_)
        {
            auto existing = std::find_if(consoleDisplay_.begin(), consoleDisplay_.end(),
                                         [&line](const ConsoleDisplayRow& item) { return item.text == line; });
            if (existing != consoleDisplay_.end())
            {
                ++existing->count;
                continue;
            }
        }
        consoleDisplay_.push_back({line, level, 1, i});
    }

    if (console_)
    {
        console_->Invalidate();
        console_->SetItemCount(static_cast<int>(consoleDisplay_.size()));
        console_->Sync();
    }
    LayoutConsoleToolbar(layout_.Rect(Slot(EditorPanelId::Console).panel).width);
}

void EditorShell::LayoutConsoleToolbar(float panelWidth)
{
    const auto stylePill = [this](UiElementHandle btn, float x, float w, const std::string& label,
                                  bool active, const UiColor& activeTextColor) {
        if (!btn.IsValid()) return;
        document_.SetVisible(btn, true);
        UiStyle st = ButtonStyle(x, 2.0f, w, 20.0f, active ? UiInk(flare::palette::workspace::Selection) : kSurfaceRaised);
        st.fontSize = 11.5f;
        st.cornerRadius = 4.0f;
        st.borderWidth = 1.0f;
        st.borderColor = active ? UiInk(flare::palette::workspace::Focus, 0.72f) : kBorder;
        st.color = active ? activeTextColor : kMuted;
        const float tw = MeasuredWidth(label, 11.5f);
        st.padding.left = std::max(4.0f, (w - tw) * 0.5f);
        st.padding.right = 2.0f;
        document_.SetText(btn, label);
        document_.SetStyle(btn, st);
    };

    const bool showClear = panelWidth >= 100.0f;
    const bool showFilters = panelWidth >= 290.0f;
    const bool showToggles = panelWidth >= 470.0f;

    float lx = 6.0f;
    if (consoleClearButton_.IsValid())
    {
        document_.SetVisible(consoleClearButton_, showClear);
        if (showClear)
        {
            const std::string lbl = Text("EditorUi.ClearConsole", "Clear");
            const float w = std::max(46.0f, MeasuredWidth(lbl, 11.5f) + 14.0f);
            UiStyle st = ButtonStyle(lx, 2.0f, w, 20.0f, kSurfaceRaised);
            st.fontSize = 11.5f;
            st.cornerRadius = 4.0f;
            st.borderWidth = 1.0f;
            st.borderColor = kBorder;
            st.color = kText;
            st.padding.left = std::max(4.0f, (w - MeasuredWidth(lbl, 11.5f)) * 0.5f);
            st.padding.right = 2.0f;
            document_.SetText(consoleClearButton_, lbl);
            document_.SetStyle(consoleClearButton_, st);
            lx += w + 4.0f;
        }
    }

    if (consoleCollapseButton_.IsValid())
    {
        if (showToggles)
        {
            const std::string lbl = Text("EditorUi.ConsoleCollapse", "Collapse");
            const float w = std::max(54.0f, MeasuredWidth(lbl, 11.5f) + 14.0f);
            stylePill(consoleCollapseButton_, lx, w, lbl, consoleCollapse_, UiInk(flare::palette::Gold));
            lx += w + 4.0f;
        }
        else document_.SetVisible(consoleCollapseButton_, false);
    }

    if (consoleClearOnPlayButton_.IsValid())
    {
        if (showToggles)
        {
            const std::string lbl = Text("EditorUi.ConsoleClearOnPlay", "Clear on Play");
            const float w = std::max(60.0f, MeasuredWidth(lbl, 11.5f) + 14.0f);
            stylePill(consoleClearOnPlayButton_, lx, w, lbl, consoleClearOnPlay_, UiInk(flare::palette::Gold));
            lx += w + 4.0f;
        }
        else document_.SetVisible(consoleClearOnPlayButton_, false);
    }

    float rx = panelWidth - 6.0f;
    const auto placeRight = [&](UiElementHandle btn, const std::string& lbl, bool active, const UiColor& activeColor) {
        if (!btn.IsValid()) return;
        if (!showFilters)
        {
            document_.SetVisible(btn, false);
            return;
        }
        const float w = std::max(48.0f, MeasuredWidth(lbl, 11.5f) + 14.0f);
        if (rx - w < lx + 4.0f)
        {
            document_.SetVisible(btn, false);
            return;
        }
        rx -= w;
        stylePill(btn, rx, w, lbl, active, activeColor);
        rx -= 4.0f;
    };

    placeRight(consoleErrorButton_,
               Text("EditorUi.ConsoleError", "Error") + " " + std::to_string(consoleErrorCount_),
               consoleShowError_,
               consoleErrorCount_ > 0 ? UiColor{0.98f, 0.48f, 0.48f, 1.0f} : kText);
    placeRight(consoleWarnButton_,
               Text("EditorUi.ConsoleWarn", "Warn") + " " + std::to_string(consoleWarnCount_),
               consoleShowWarn_,
               consoleWarnCount_ > 0 ? UiColor{0.98f, 0.78f, 0.34f, 1.0f} : kText);
    placeRight(consoleInfoButton_,
               Text("EditorUi.ConsoleInfo", "Info") + " " + std::to_string(consoleInfoCount_),
               consoleShowInfo_,
               kText);
}

} // namespace flare::ui::editor
