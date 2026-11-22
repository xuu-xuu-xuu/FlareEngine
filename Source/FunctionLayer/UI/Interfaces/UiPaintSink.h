#pragma once

#include "FunctionLayer/UI/Core/UiLayout.h"
#include "FunctionLayer/UI/Core/UiStyle.h"

#include <cstdint>
#include <string_view>

namespace flare::ui
{

// 绘制命令接收端；UiDisplayList 和测试用 sink 实现它。
// 只传值/句柄，不传文档指针或 GPU 资源。
class UiPaintSink
{
public:
    virtual ~UiPaintSink() = default;

    virtual void PushClip(const UiRect& clip) = 0;
    virtual void PopClip() = 0;
    virtual void Rect(const UiRect& rect, const UiColor& color) = 0;
    virtual void Border(const UiRect& rect, float width, const UiColor& color) = 0;
    virtual void NineSlice(const UiRect& rect,
                           const UiRect& uv,
                           float insetLeft,
                           float insetTop,
                           float insetRight,
                           float insetBottom,
                           std::uint64_t texture,
                           const UiColor& color) = 0;
    virtual void Text(const UiRect& rect, std::string_view text, float fontSize, const UiColor& color) = 0;
    virtual void Image(const UiRect& rect, std::string_view image, const UiColor& color) = 0;
    virtual void Line(float x0, float y0, float x1, float y1, float width, const UiColor& color) = 0;
    virtual void CubicBezier(float x0, float y0,
                             float c0x, float c0y,
                             float c1x, float c1y,
                             float x1, float y1,
                             float width, const UiColor& color)
    {
        (void)c0x;
        (void)c0y;
        (void)c1x;
        (void)c1y;
        Line(x0, y0, x1, y1, width, color);
    }
    virtual void CanvasRect(const UiRect& rect, const UiColor& color) = 0;
    virtual void CanvasCircle(float centerX, float centerY, float radius, const UiColor& color) = 0;
};

} // namespace flare::ui
