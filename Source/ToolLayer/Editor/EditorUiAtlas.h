#pragma once

#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Rendering/GuiCompositor.h"
#include "FunctionLayer/Rendering/UiPaint.h"
#include "PlatformLayer/Windows/UiDirectWriteTextService.h"

#include <cstddef>
#include <vector>

namespace se
{

[[nodiscard]] inline ui::UiGeometryOptions MakeEditorUiGeometryOptions(Vec2 textPixelScale) noexcept
{
    ui::UiGeometryOptions options;
    options.whiteTexture = GuiCompositor::WhiteToken;
    options.fontPageBase = GuiCompositor::FontToken;
    options.fontExtraPageBase = GuiCompositor::FontExtraPageBase;
    options.maxFontPages = GuiCompositor::MaxFontPages;
    options.textPixelScale = textPixelScale;
    return options;
}

[[nodiscard]] inline bool AttachEditorFontPages(
    const UiDirectWriteTextService& text,
    std::vector<UiDirectWriteTextService::FontPageRgba>& cache,
    FrameData& frame)
{
    if (!text.FontPages(cache) || cache.empty())
    {
        return false;
    }
    frame.editorFontPages.clear();
    frame.editorFontPages.reserve(cache.size());
    for (std::size_t index = 0; index < cache.size(); ++index)
    {
        const auto& page = cache[index];
        frame.editorFontPages.push_back({
            page.rgba.empty() ? nullptr : page.rgba.data(),
            page.width,
            page.height,
            GuiCompositor::FontPageToken(static_cast<std::uint32_t>(index)),
            page.revision});
    }
    return true;
}

} // namespace se
