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

void EditorShell::BuildDesignerPanel(PanelSlot& slot)
{
    designerCanvas_ = document_.CreateElement(UiElementKind::Panel);
    if (!document_.SetParent(designerCanvas_, slot.content))
    {
        return;
    }
    UiStyle canvasStyle = AbsoluteStyle(0.0f, 0.0f, 100.0f, 100.0f);
    canvasStyle.width = UiLength::Percent(100.0f);
    canvasStyle.height = UiLength::Percent(100.0f);
    canvasStyle.background = UiColor{0.035f, 0.04f, 0.05f, 1.0f};
    canvasStyle.overflow = UiOverflow::Hidden;
    document_.SetStyle(designerCanvas_, canvasStyle);
    designerPlaceholder_ = document_.CreateElement(UiElementKind::Label);
    if (document_.SetParent(designerPlaceholder_, designerCanvas_))
    {
        document_.SetStyle(designerPlaceholder_, ButtonStyle(16.0f, 16.0f, 360.0f, 24.0f,
                                                             UiColor{0.035f, 0.04f, 0.05f, 0.0f}));
        document_.SetText(designerPlaceholder_, Text("EditorUi.NoUiDocument", "No UI document loaded"));
    }
    dispatcher_.AddHandler(designerCanvas_, UiEventType::PointerDown, [this](UiInputEvent& event) {
        const UiRect canvas = layout_.Rect(designerCanvas_);
        const float x = event.x - canvas.x;
        const float y = event.y - canvas.y;
        bool hit = false;
        for (auto node = designerNodes_.rbegin(); node != designerNodes_.rend(); ++node)
        {
            if (node->visible && x >= node->rect.x && x < node->rect.x + node->rect.width &&
                y >= node->rect.y && y < node->rect.y + node->rect.height)
            {
                hit = true;
                break;
            }
        }
        designerDragging_ = hit && designerDown_ != nullptr;
        if (event.button==0 && designerDown_)
        {
            designerDown_(event.x, event.y);
        }
        event.handled = true;
    }, UiEventPhase::Bubble);
    dispatcher_.AddHandler(designerCanvas_, UiEventType::PointerMove, [this](UiInputEvent& event) {
        if (designerDragging_ && designerMove_)
        {
            designerMove_(event.x, event.y);
            event.handled = true;
        }
    }, UiEventPhase::Bubble);
    dispatcher_.AddHandler(designerCanvas_, UiEventType::PointerUp, [this](UiInputEvent& event) {
        if (designerDragging_ && designerUp_)
        {
            designerUp_(event.x, event.y);
        }
        designerDragging_ = false;
        event.handled = true;
    }, UiEventPhase::Bubble);
}

void EditorShell::SetDesignerNodes(std::vector<EditorDesignerNode> nodes)
{
    if (nodes == designerNodes_)
    {
        return;
    }
    designerNodes_ = std::move(nodes);
    RefreshDesignerNodes();
}

void EditorShell::SetDesignerHandlers(std::function<void(float, float)> down,
                                      std::function<void(float, float)> move,
                                      std::function<void(float, float)> up)
{
    designerDown_ = std::move(down);
    designerMove_ = std::move(move);
    designerUp_ = std::move(up);
}

void EditorShell::RefreshDesignerNodes()
{
    for (const UiElementHandle element : designerNodeElements_)
    {
        if (element.IsValid())
        {
            document_.DestroyElement(element);
        }
    }
    designerNodeElements_.clear();
    if (designerPlaceholder_.IsValid())
    {
        document_.SetVisible(designerPlaceholder_, designerNodes_.empty());
    }
    for (const EditorDesignerNode& node : designerNodes_)
    {
        const UiElementHandle element = document_.CreateElement(UiElementKind::Panel);
        if (!document_.SetParent(element, designerCanvas_))
        {
            continue;
        }
        UiStyle style = AbsoluteStyle(node.rect.x, node.rect.y, node.rect.width, node.rect.height);
        style.background = node.color.a > 0.0f ? node.color : kSurfaceActive;
        style.borderWidth = node.selected ? 2.0f : 1.0f;
        style.borderColor = node.selected ? kAccent : kBorder;
        style.color = kText;
        style.fontSize = 13.0f;
        style.overflow = UiOverflow::Hidden;
        document_.SetStyle(element, style);
        document_.SetText(element, node.text);
        document_.SetVisible(element, node.visible);
        designerNodeElements_.push_back(element);
    }
}

UiElementHandle EditorShell::UiDesignerContent() const noexcept
{
    return Slot(EditorPanelId::UiDesigner).content;
}

UiElementHandle EditorShell::DesignerNodeElement(std::size_t index) const noexcept
{
    return index < designerNodeElements_.size() ? designerNodeElements_[index] : UiElementHandle{};
}

} // namespace flare::ui::editor
