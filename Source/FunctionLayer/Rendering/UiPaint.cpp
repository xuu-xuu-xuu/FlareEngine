#include "FunctionLayer/Rendering/UiPaint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flare::ui
{
namespace
{
float Srgb(float value) noexcept
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value <= 0.0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

std::uint32_t PackColor(const UiColor& color) noexcept
{
    const auto byte = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return byte(Srgb(color.r)) | (byte(Srgb(color.g)) << 8) | (byte(Srgb(color.b)) << 16) |
        (byte(color.a) << 24);
}

bool Finite(const UiRect& rect) noexcept
{
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
        std::isfinite(rect.height) && std::isfinite(rect.x + rect.width) &&
        std::isfinite(rect.y + rect.height) && rect.width >= 0.0f && rect.height >= 0.0f;
}

UiRect Intersect(const UiRect& lhs, const UiRect& rhs) noexcept
{
    const float x = std::max(lhs.x, rhs.x);
    const float y = std::max(lhs.y, rhs.y);
    return {x, y, std::max(0.0f, std::min(lhs.x + lhs.width, rhs.x + rhs.width) - x),
            std::max(0.0f, std::min(lhs.y + lhs.height, rhs.y + rhs.height) - y)};
}

bool Empty(const UiRect& rect) noexcept
{
    return rect.width <= 0.0f || rect.height <= 0.0f;
}

bool SameClip(const Vec4& clip, const UiRect& rect) noexcept
{
    return clip.x == rect.x && clip.y == rect.y && clip.z == rect.x + rect.width &&
        clip.w == rect.y + rect.height;
}
} // namespace

void UiDisplayList::Clear()
{
    commands.clear();
    width = 0.0f;
    height = 0.0f;
}

void UiDisplayList::PushClip(const UiRect& clip)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::ClipPush;
    command.rect = clip;
    commands.push_back(std::move(command));
}

void UiDisplayList::PopClip()
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::ClipPop;
    commands.push_back(std::move(command));
}

void UiDisplayList::Rect(const UiRect& rect, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::Rect;
    command.rect = rect;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::Border(const UiRect& rect, float borderWidth, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::Border;
    command.rect = rect;
    command.borderWidth = borderWidth;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::NineSlice(const UiRect& rect,
                              const UiRect& uv,
                              float insetLeft,
                              float insetTop,
                              float insetRight,
                              float insetBottom,
                              std::uint64_t texture,
                              const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::NineSlice;
    command.rect = rect;
    command.uv = uv;
    command.insetLeft = insetLeft;
    command.insetTop = insetTop;
    command.insetRight = insetRight;
    command.insetBottom = insetBottom;
    command.texture = texture;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::Text(const UiRect& rect, std::string_view text, float fontSize, const UiColor& color)
{
    Text(rect, text, fontSize, color, UiTextFlow::Wrap);
}

void UiDisplayList::Text(const UiRect& rect, std::string_view text, float fontSize, const UiColor& color, UiTextFlow flow)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::Text;
    command.rect = rect;
    command.text.assign(text);
    command.fontSize = fontSize;
    command.textFlow = flow;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::Image(const UiRect& rect, std::string_view image, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::Image;
    command.rect = rect;
    command.image.assign(image);
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::Line(float x0, float y0, float x1, float y1, float lineWidth, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::Line;
    command.x0 = x0;
    command.y0 = y0;
    command.x1 = x1;
    command.y1 = y1;
    command.lineWidth = lineWidth;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::CubicBezier(float x0, float y0,
                                float c0x, float c0y,
                                float c1x, float c1y,
                                float x1, float y1,
                                float lineWidth, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::CubicBezier;
    command.x0 = x0;
    command.y0 = y0;
    command.c0x = c0x;
    command.c0y = c0y;
    command.c1x = c1x;
    command.c1y = c1y;
    command.x1 = x1;
    command.y1 = y1;
    command.lineWidth = lineWidth;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::CanvasRect(const UiRect& rect, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::CanvasRect;
    command.rect = rect;
    command.color = color;
    commands.push_back(std::move(command));
}

void UiDisplayList::CanvasCircle(float centerX, float centerY, float radius, const UiColor& color)
{
    UiPaintCommand command;
    command.kind = UiPaintCommandKind::CanvasCircle;
    command.centerX = centerX;
    command.centerY = centerY;
    command.radius = radius;
    command.color = color;
    commands.push_back(std::move(command));
}

UiPainter::UiPainter(UiDocument& document, UiLayoutEngine& layout, UiStyleEngine* styleEngine)
    : document_(&document), layout_(&layout), styleEngine_(styleEngine)
{
}

bool UiPainter::Build(UiElementHandle root, UiDisplayList& displayList, UiPaintStats& stats)
{
    stats = {};
    displayList.Clear();
    if (!document_ || !layout_ || !document_->Element(root))
    {
        return false;
    }
    const UiRect viewport = layout_->Rect(root);
    if (!Finite(viewport) || Empty(viewport))
    {
        return false;
    }
    displayList.width = viewport.width;
    displayList.height = viewport.height;
    displayList.PushClip({0.0f, 0.0f, viewport.width, viewport.height});
    Visit(root, displayList, stats);
    displayList.PopClip();
    stats.commands = static_cast<std::uint32_t>(displayList.commands.size());
    for (const UiPaintCommand& command : displayList.commands)
    {
        if (command.kind == UiPaintCommandKind::ClipPush)
        {
            ++stats.clipPushes;
        }
        else if (command.kind != UiPaintCommandKind::ClipPop)
        {
            ++stats.draws;
        }
    }
    return true;
}

void UiPainter::Visit(UiElementHandle element, UiDisplayList& displayList, UiPaintStats& stats)
{
    const UiElement* node = document_->Element(element);
    if (!node)
    {
        return;
    }
    if (!document_->Visible(element))
    {
        return;
    }
    const UiStyle& style = styleEngine_ ? styleEngine_->Compute(element) : document_->Style(element);
    if (style.display == UiDisplay::None)
    {
        return;
    }
    const UiRect rect = layout_->Rect(element);
    if (Empty(rect) || !Finite(rect))
    {
        return;
    }

    ++stats.nodes;
    const bool clipsChildren = style.overflow != UiOverflow::Visible;
    if (clipsChildren)
    {
        displayList.PushClip(rect);
    }
    if (style.background.a > 0.0f)
    {
        displayList.Rect(rect, style.background);
        displayList.commands.back().radius = std::max(0.0f, style.cornerRadius);
    }
    if (style.borderWidth > 0.0f && style.borderColor.a > 0.0f)
    {
        displayList.Border(rect, style.borderWidth, style.borderColor);
        displayList.commands.back().radius = std::max(0.0f, style.cornerRadius);
    }
    for (UiElementHandle child = document_->FirstChild(element); child.IsValid();
         child = document_->NextSibling(child))
    {
        Visit(child, displayList, stats);
    }
    const std::string_view text = document_->Text(element);
    if (!text.empty())
    {
        UiRect content = rect;
        if (style.textFlow == UiTextFlow::SingleLineEllipsis)
        {
            const float left = std::max(0.0f, style.padding.left);
            const float top = std::max(0.0f, style.padding.top);
            content.x += left;
            content.y += top;
            content.width = std::max(0.0f, rect.width - left - std::max(0.0f, style.padding.right));
            content.height = std::max(0.0f, rect.height - top - std::max(0.0f, style.padding.bottom));
        }
        displayList.Text(content, text, style.fontSize, style.color, style.textFlow);
        ++stats.textRuns;
    }
    if (!style.image.empty())
    {
        displayList.Image(rect, style.image, style.color);
        ++stats.images;
    }
    if (clipsChildren)
    {
        displayList.PopClip();
    }
}

namespace
{
// 用当前窗口的字形度量取前缀。插入光标命中取最近的位置，末字因此可能还需退一格。
std::string_view FitSingleLine(const UiPaintCommand& command, UiTextService& service, std::string& shortened)
{
    const std::string_view text = command.text;
    const std::string_view line = text.substr(0, text.find_first_of("\r\n"));
    const auto widthOf = [&](std::string_view value) {
        return service.Measure(value, command.fontSize, command.rect.width, false).width;
    };
    if (line.size() == text.size() && widthOf(line) <= command.rect.width)
    {
        return line;
    }
    constexpr std::string_view ellipsis = "…";
    const float available = command.rect.width - widthOf(ellipsis);
    if (available < 0.0f)
    {
        return {};
    }
    std::size_t end = std::min<std::size_t>(line.size(),
        service.HitTestCaret(line, command.fontSize, command.rect.width, false, available, 0));
    for (;;)
    {
        // 测试用等宽服务按字节命中；真实服务返回字符边界。
        while (end > 0 && end < line.size() && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80)
            --end;
        shortened.assign(line.substr(0, end));
        shortened.append(ellipsis);
        if (end == 0 || widthOf(shortened) <= command.rect.width)
            return shortened;
        --end;
    }
}

struct GeometryContext
{
    GuiGeometry& geometry;
    UiGeometryStats& stats;
    std::vector<std::string>& errors;
    std::vector<UiRect> clips;
    std::uint64_t whiteTexture = 1;

    bool Reject(const char* message)
    {
        ++stats.rejected;
        errors.emplace_back(message);
        return false;
    }

    bool AppendQuad(const UiRect& rect,
                    const UiRect& uv,
                    const UiColor& color,
                    std::uint64_t texture,
                    const UiRect& clip)
    {
        if (!Finite(rect) || Empty(rect) || Empty(Intersect(rect, clip)) || color.a <= 0.0f)
        {
            return true;
        }
        if (geometry.vertices.size() / 4 >= 65536)
        {
            return Reject("UI.PaintCapacity");
        }
        const std::uint32_t packed = PackColor(color);
        const std::uint32_t base = static_cast<std::uint32_t>(geometry.vertices.size());
        const std::uint32_t first = static_cast<std::uint32_t>(geometry.indices.size());
        geometry.vertices.push_back(GuiVertex{{rect.x, rect.y}, {uv.x, uv.y}, packed});
        geometry.vertices.push_back(GuiVertex{{rect.x + rect.width, rect.y}, {uv.x + uv.width, uv.y}, packed});
        geometry.vertices.push_back(GuiVertex{{rect.x + rect.width, rect.y + rect.height},
                                               {uv.x + uv.width, uv.y + uv.height}, packed});
        geometry.vertices.push_back(GuiVertex{{rect.x, rect.y + rect.height}, {uv.x, uv.y + uv.height}, packed});
        geometry.indices.insert(geometry.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        const Vec4 scissor{clip.x, clip.y, clip.x + clip.width, clip.y + clip.height};
        if (!geometry.commands.empty() && geometry.commands.back().texture == texture &&
            geometry.commands.back().textureIsData == false && SameClip(geometry.commands.back().clip, clip))
        {
            geometry.commands.back().indexCount += 6;
        }
        else
        {
            geometry.commands.push_back({scissor, texture, first, 6, 0, false});
        }
        ++stats.quads;
        return true;
    }

    bool AppendTriangle(const Vec2& a,
                        const Vec2& b,
                        const Vec2& c,
                        const UiColor& color,
                        std::uint64_t texture,
                        const UiRect& clip)
    {
        const std::uint32_t packed = PackColor(color);
        return AppendTriangle(a, packed, b, packed, c, packed, texture, clip);
    }

    bool AppendTriangle(const Vec2& a, std::uint32_t colorA,
                        const Vec2& b, std::uint32_t colorB,
                        const Vec2& c, std::uint32_t colorC,
                        std::uint64_t texture,
                        const UiRect& clip)
    {
        if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y) ||
            !std::isfinite(c.x) || !std::isfinite(c.y) || Empty(clip))
        {
            return true;
        }
        const float minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
        const float minY = std::min({a.y, b.y, c.y}), maxY = std::max({a.y, b.y, c.y});
        if (maxX <= clip.x || minX >= clip.x + clip.width || maxY <= clip.y || minY >= clip.y + clip.height)
        {
            return true;
        }
        if (geometry.vertices.size() / 3 >= 65536 || geometry.indices.size() + 3u > 65536u * 6u)
        {
            return Reject("UI.PaintCapacity");
        }
        const std::uint32_t base = static_cast<std::uint32_t>(geometry.vertices.size());
        const std::uint32_t first = static_cast<std::uint32_t>(geometry.indices.size());
        geometry.vertices.push_back(GuiVertex{a, {}, colorA});
        geometry.vertices.push_back(GuiVertex{b, {}, colorB});
        geometry.vertices.push_back(GuiVertex{c, {}, colorC});
        geometry.indices.insert(geometry.indices.end(), {base, base + 1, base + 2});
        const Vec4 scissor{clip.x, clip.y, clip.x + clip.width, clip.y + clip.height};
        if (!geometry.commands.empty() && geometry.commands.back().texture == texture &&
            geometry.commands.back().textureIsData == false && SameClip(geometry.commands.back().clip, clip))
        {
            geometry.commands.back().indexCount += 3;
        }
        else
        {
            geometry.commands.push_back({scissor, texture, first, 3, 0, false});
        }
        ++stats.quads;
        return true;
    }

    // 圆角矩形按四个圆心生成轮廓；边缘留一个物理像素的透明渐变充当抗锯齿。
    // stroke 为 0 时填充，否则描边。shortcut: 半径被抬到不小于 stroke+feather，足够覆盖编辑器的 2px 以内描边。
    bool AppendRoundedRect(const UiRect& rect, float radius, float stroke, const UiColor& color,
                           std::uint64_t texture, const UiRect& clip, float feather)
    {
        if (!Finite(rect) || Empty(rect) || Empty(Intersect(rect, clip)) || color.a <= 0.0f)
        {
            return true;
        }
        const float halfExtent = std::min(rect.width, rect.height) * 0.5f;
        stroke = std::min(stroke, halfExtent);
        radius = std::min(std::max(radius, stroke + feather), halfExtent);
        constexpr int Arc = 6;
        constexpr int Count = 4 * (Arc + 1);
        std::array<Vec2, Count> centers{};
        std::array<Vec2, Count> normals{};
        const Vec2 corners[4] = {{rect.x + rect.width - radius, rect.y + radius},
                                 {rect.x + rect.width - radius, rect.y + rect.height - radius},
                                 {rect.x + radius, rect.y + rect.height - radius},
                                 {rect.x + radius, rect.y + radius}};
        for (int corner = 0; corner < 4; ++corner)
        {
            for (int step = 0; step <= Arc; ++step)
            {
                const float angle = (-90.0f + 90.0f * corner + 90.0f * step / Arc) * 0.0174532925f;
                const int index = corner * (Arc + 1) + step;
                centers[index] = corners[corner];
                normals[index] = {std::cos(angle), std::sin(angle)};
            }
        }
        const auto point = [&](int index, float offset) {
            return Vec2{centers[index].x + normals[index].x * offset, centers[index].y + normals[index].y * offset};
        };
        const std::uint32_t solid = PackColor(color);
        const std::uint32_t clear = PackColor({color.r, color.g, color.b, 0.0f});
        const auto band = [&](float inner, std::uint32_t innerColor, float outer, std::uint32_t outerColor) {
            if (outer <= inner) return true;
            for (int index = 0; index < Count; ++index)
            {
                const int next = (index + 1) % Count;
                const Vec2 a = point(index, inner), b = point(next, inner);
                const Vec2 c = point(next, outer), d = point(index, outer);
                if (!AppendTriangle(a, innerColor, b, innerColor, c, outerColor, texture, clip) ||
                    !AppendTriangle(a, innerColor, c, outerColor, d, outerColor, texture, clip))
                {
                    return false;
                }
            }
            return true;
        };
        const float half = feather * 0.5f;
        if (stroke <= 0.0f)
        {
            const float edge = std::max(0.0f, radius - half);
            const Vec2 middle{rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
            for (int index = 0; index < Count; ++index)
            {
                if (!AppendTriangle(middle, solid, point(index, edge), solid, point((index + 1) % Count, edge), solid,
                                    texture, clip))
                {
                    return false;
                }
            }
            return band(edge, solid, radius + half, clear);
        }
        const float inner = radius - stroke;
        return band(std::max(0.0f, inner - half), clear, inner + half, solid) &&
               band(inner + half, solid, radius - half, solid) &&
               band(radius - half, solid, radius + half, clear);
    }

    // 连续法线带 + 物理像素羽化边，消除斜线与贝塞尔连线的阶梯锯齿和接头重影。
    bool AppendFeatheredRibbon(const Vec2* points, const Vec2* normals, std::size_t count,
                               float width, const UiColor& color, std::uint64_t texture,
                               const UiRect& clip, float feather)
    {
        if (count < 2 || width <= 0.0f || color.a <= 0.0f || Empty(clip))
        {
            return true;
        }
        const float aa = std::max(0.75f, feather * 1.25f);
        const bool hasSolidCore = width > aa;
        const std::size_t vertsPerPoint = hasSolidCore ? 4u : 3u;
        const std::size_t indicesPerSeg = hasSolidCore ? 18u : 12u;
        if ((geometry.vertices.size() + count * vertsPerPoint) / 3 >= 65536 ||
            geometry.indices.size() + (count - 1u) * indicesPerSeg > 65536u * 6u)
        {
            return Reject("UI.PaintCapacity");
        }
        const std::uint32_t base = static_cast<std::uint32_t>(geometry.vertices.size());
        const std::uint32_t firstIndex = static_cast<std::uint32_t>(geometry.indices.size());
        const std::uint32_t clear = PackColor({color.r, color.g, color.b, 0.0f});
        if (hasSolidCore)
        {
            const float innerHalf = (width - aa) * 0.5f;
            const float outerHalf = (width + aa) * 0.5f;
            const std::uint32_t solid = PackColor(color);
            for (std::size_t i = 0; i < count; ++i)
            {
                const Vec2 p = points[i];
                const Vec2 n = normals[i];
                geometry.vertices.push_back(GuiVertex{{p.x - n.x * outerHalf, p.y - n.y * outerHalf}, {}, clear});
                geometry.vertices.push_back(GuiVertex{{p.x - n.x * innerHalf, p.y - n.y * innerHalf}, {}, solid});
                geometry.vertices.push_back(GuiVertex{{p.x + n.x * innerHalf, p.y + n.y * innerHalf}, {}, solid});
                geometry.vertices.push_back(GuiVertex{{p.x + n.x * outerHalf, p.y + n.y * outerHalf}, {}, clear});
            }
            for (std::size_t i = 0; i + 1 < count; ++i)
            {
                const std::uint32_t b0 = base + static_cast<std::uint32_t>(i * 4u);
                const std::uint32_t b1 = b0 + 4u;
                geometry.indices.insert(geometry.indices.end(), {
                    b0 + 0, b1 + 0, b1 + 1, b0 + 0, b1 + 1, b0 + 1,
                    b0 + 1, b1 + 1, b1 + 2, b0 + 1, b1 + 2, b0 + 2,
                    b0 + 2, b1 + 2, b1 + 3, b0 + 2, b1 + 3, b0 + 3,
                });
                stats.quads += 3;
            }
        }
        else
        {
            const float outerHalf = aa * 0.85f;
            const std::uint32_t peak = PackColor({color.r, color.g, color.b, color.a * std::clamp(width / aa, 0.0f, 1.0f)});
            for (std::size_t i = 0; i < count; ++i)
            {
                const Vec2 p = points[i];
                const Vec2 n = normals[i];
                geometry.vertices.push_back(GuiVertex{{p.x - n.x * outerHalf, p.y - n.y * outerHalf}, {}, clear});
                geometry.vertices.push_back(GuiVertex{p, {}, peak});
                geometry.vertices.push_back(GuiVertex{{p.x + n.x * outerHalf, p.y + n.y * outerHalf}, {}, clear});
            }
            for (std::size_t i = 0; i + 1 < count; ++i)
            {
                const std::uint32_t b0 = base + static_cast<std::uint32_t>(i * 3u);
                const std::uint32_t b1 = b0 + 3u;
                geometry.indices.insert(geometry.indices.end(), {
                    b0 + 0, b1 + 0, b1 + 1, b0 + 0, b1 + 1, b0 + 1,
                    b0 + 1, b1 + 1, b1 + 2, b0 + 1, b1 + 2, b0 + 2,
                });
                stats.quads += 2;
            }
        }
        const std::uint32_t addedIndices = static_cast<std::uint32_t>(geometry.indices.size()) - firstIndex;
        const Vec4 scissor{clip.x, clip.y, clip.x + clip.width, clip.y + clip.height};
        if (!geometry.commands.empty() && geometry.commands.back().texture == texture &&
            geometry.commands.back().textureIsData == false && SameClip(geometry.commands.back().clip, clip))
        {
            geometry.commands.back().indexCount += addedIndices;
        }
        else
        {
            geometry.commands.push_back({scissor, texture, firstIndex, addedIndices, 0, false});
        }
        return true;
    }

    bool AppendCircle(float cx, float cy, float radius, const UiColor& color,
                      std::uint64_t texture, const UiRect& clip, float feather)
    {
        if (radius <= 0.0f || color.a <= 0.0f)
        {
            return true;
        }
        const float aa = std::max(0.75f, feather * 1.15f);
        const float outerR = radius + aa * 0.5f;
        const UiRect bounds{cx - outerR, cy - outerR, outerR * 2.0f, outerR * 2.0f};
        if (!Finite(bounds) || Empty(Intersect(bounds, clip)))
        {
            return true;
        }
        const int segments = radius > 12.0f ? 40 : 28;
        const float innerR = std::max(0.0f, radius - aa * 0.5f);
        const std::uint32_t solid = PackColor(innerR > 0.0f
            ? color
            : UiColor{color.r, color.g, color.b, color.a * std::clamp(radius * 2.0f / aa, 0.0f, 1.0f)});
        const std::uint32_t clear = PackColor({color.r, color.g, color.b, 0.0f});
        const Vec2 center{cx, cy};
        for (int index = 0; index < segments; ++index)
        {
            const float a0 = static_cast<float>(index) * 6.28318530718f / static_cast<float>(segments);
            const float a1 = static_cast<float>(index + 1) * 6.28318530718f / static_cast<float>(segments);
            const float c0 = std::cos(a0), s0 = std::sin(a0);
            const float c1 = std::cos(a1), s1 = std::sin(a1);
            if (innerR > 0.0f)
            {
                const Vec2 i0{cx + c0 * innerR, cy + s0 * innerR};
                const Vec2 i1{cx + c1 * innerR, cy + s1 * innerR};
                const Vec2 o0{cx + c0 * outerR, cy + s0 * outerR};
                const Vec2 o1{cx + c1 * outerR, cy + s1 * outerR};
                if (!AppendTriangle(center, solid, i0, solid, i1, solid, texture, clip) ||
                    !AppendTriangle(i0, solid, i1, solid, o1, clear, texture, clip) ||
                    !AppendTriangle(i0, solid, o1, clear, o0, clear, texture, clip))
                {
                    return false;
                }
            }
            else
            {
                const Vec2 o0{cx + c0 * outerR, cy + s0 * outerR};
                const Vec2 o1{cx + c1 * outerR, cy + s1 * outerR};
                if (!AppendTriangle(center, solid, o0, clear, o1, clear, texture, clip))
                {
                    return false;
                }
            }
        }
        return true;
    }
};
} // namespace

bool UiGeometryBuilder::Build(const UiDisplayList& displayList,
                              UiTextService* textService,
                              UiImageProvider* imageProvider,
                              const UiGeometryOptions& options,
                              GuiGeometry& geometry,
                              UiGeometryStats& stats,
                              std::vector<std::string>& errors)
{
    geometry = {};
    stats = {};
    errors.clear();
    if (!Finite({0.0f, 0.0f, displayList.width, displayList.height}) || displayList.width <= 0.0f ||
        displayList.height <= 0.0f)
    {
        errors.emplace_back("UI.PaintInvalidSize");
        return false;
    }
    geometry.displayPosition = {0.0f, 0.0f};
    geometry.displaySize = {displayList.width, displayList.height};
    geometry.framebufferScale = {1.0f, 1.0f};
    GeometryContext context{geometry, stats, errors, {}, options.whiteTexture};
    context.clips.push_back({0.0f, 0.0f, displayList.width, displayList.height});
    const float feather = options.textPixelScale.x > 0.0f ? 1.0f / options.textPixelScale.x : 1.0f;

    for (const UiPaintCommand& command : displayList.commands)
    {
        ++stats.commands;
        const UiRect clip = context.clips.empty()
            ? UiRect{0.0f, 0.0f, 0.0f, 0.0f}
            : context.clips.back();
        switch (command.kind)
        {
        case UiPaintCommandKind::ClipPush:
            context.clips.push_back(Intersect(clip, command.rect));
            break;
        case UiPaintCommandKind::ClipPop:
            if (context.clips.size() > 1)
            {
                context.clips.pop_back();
            }
            break;
        case UiPaintCommandKind::Rect:
            if (command.radius > 0.0f)
            {
                context.AppendRoundedRect(command.rect, command.radius, 0.0f, command.color, options.whiteTexture, clip, feather);
                break;
            }
            context.AppendQuad(command.rect, {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color,
                               options.whiteTexture, clip);
            break;
        case UiPaintCommandKind::Border:
        {
            const float width = std::min(command.borderWidth,
                                         std::min(command.rect.width, command.rect.height) * 0.5f);
            if (width <= 0.0f)
            {
                break;
            }
            if (command.radius > 0.0f)
            {
                context.AppendRoundedRect(command.rect, command.radius, width, command.color, options.whiteTexture, clip, feather);
                break;
            }
            context.AppendQuad({command.rect.x, command.rect.y, command.rect.width, width},
                               {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color, options.whiteTexture, clip);
            context.AppendQuad({command.rect.x, command.rect.y + command.rect.height - width,
                                command.rect.width, width},
                               {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color, options.whiteTexture, clip);
            context.AppendQuad({command.rect.x, command.rect.y, width, command.rect.height},
                               {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color, options.whiteTexture, clip);
            context.AppendQuad({command.rect.x + command.rect.width - width, command.rect.y,
                                width, command.rect.height},
                               {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color, options.whiteTexture, clip);
            break;
        }
        case UiPaintCommandKind::NineSlice:
        {
            if (command.texture == 0)
            {
                context.Reject("UI.PaintInvalidTextureToken");
                break;
            }
            float left = std::max(0.0f, command.insetLeft);
            float right = std::max(0.0f, command.insetRight);
            float top = std::max(0.0f, command.insetTop);
            float bottom = std::max(0.0f, command.insetBottom);
            const float width = command.rect.width;
            const float height = command.rect.height;
            if (left + right > width)
            {
                const float scale = width / (left + right);
                left *= scale;
                right *= scale;
            }
            if (top + bottom > height)
            {
                const float scale = height / (top + bottom);
                top *= scale;
                bottom *= scale;
            }
            const float x0 = command.rect.x;
            const float x1 = x0 + left;
            const float x2 = x0 + width - right;
            const float y0 = command.rect.y;
            const float y1 = y0 + top;
            const float y2 = y0 + height - bottom;
            const float u0 = command.uv.x;
            const float u1 = u0 + command.uv.width * (left / std::max(width, 1.0f));
            const float u2 = u0 + command.uv.width;
            const float v0 = command.uv.y;
            const float v1 = v0 + command.uv.height * (top / std::max(height, 1.0f));
            const float v2 = v0 + command.uv.height;
            const float xs[4] = {x0, x1, x2, x0 + width};
            const float ys[4] = {y0, y1, y2, y0 + height};
            const float us[3] = {u0, u1, u2};
            const float vs[3] = {v0, v1, v2};
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    const UiRect cell{xs[column], ys[row], xs[column + 1] - xs[column],
                                      ys[row + 1] - ys[row]};
                    const UiRect uv{us[column], vs[row], us[column + 1] - us[column],
                                    vs[row + 1] - vs[row]};
                    context.AppendQuad(cell, uv, command.color, command.texture, clip);
                }
            }
            break;
        }
        case UiPaintCommandKind::Text:
        {
            if (!textService)
            {
                context.Reject("UI.PaintTextUnavailable");
                break;
            }
            const bool wrap = command.textFlow == UiTextFlow::Wrap;
            const UiRect textClip = wrap ? clip : Intersect(clip, command.rect);
            if (Empty(textClip)) break;
            std::string shortened;
            const std::string_view run = wrap ? std::string_view(command.text) : FitSingleLine(command, *textService, shortened);
            if (run.empty()) break;
            const float top = wrap ? command.rect.y :
                command.rect.y + (command.rect.height - textService->Measure(run, command.fontSize, command.rect.width, false).height) * 0.5f;
            std::vector<UiGlyphQuad> glyphs;
            if (!textService->LayoutGlyphs(run, command.fontSize, command.rect.width, wrap, glyphs))
            {
                context.Reject("UI.PaintTextUnavailable");
                break;
            }
            ++stats.textRuns;
            for (const UiGlyphQuad& glyph : glyphs)
            {
                if (glyph.page >= options.maxFontPages)
                {
                    context.Reject("UI.PaintFontPageUnsupported");
                    continue;
                }
                const std::uint64_t texture =
                    (glyph.page == 0 || options.fontExtraPageBase == 0)
                        ? (options.fontPageBase + static_cast<std::uint64_t>(glyph.page))
                        : (options.fontExtraPageBase + static_cast<std::uint64_t>(glyph.page - 1));
                if (texture == 0)
                {
                    context.Reject("UI.PaintInvalidTextureToken");
                    continue;
                }
                UiRect destination{command.rect.x + glyph.x, top + glyph.y, glyph.width, glyph.height};
                if(options.textPixelScale.x>0.0f)
                    destination.x=std::round(destination.x*options.textPixelScale.x)/options.textPixelScale.x;
                if(options.textPixelScale.y>0.0f)
                    destination.y=std::round(destination.y*options.textPixelScale.y)/options.textPixelScale.y;
                context.AppendQuad(destination,
                                   {glyph.u0, glyph.v0, glyph.u1 - glyph.u0, glyph.v1 - glyph.v0},
                                   command.color, texture, textClip);
            }
            break;
        }
        case UiPaintCommandKind::Image:
        {
            UiImageInfo info;
            if (!imageProvider || !imageProvider->Resolve(command.image, info) || info.textureToken == 0)
            {
                context.Reject("UI.PaintInvalidTextureToken");
                break;
            }
            ++stats.images;
            UiRect dst = command.rect;
            UiRect imgClip = clip;
            if (options.textPixelScale.x > 0.0f && options.textPixelScale.y > 0.0f)
            {
                const float sx = options.textPixelScale.x;
                const float sy = options.textPixelScale.y;
                dst.x = static_cast<float>(std::lround(dst.x * sx)) / sx;
                dst.y = static_cast<float>(std::lround(dst.y * sy)) / sy;
                dst.width = static_cast<float>(std::max<long>(1L, std::lround(dst.width * sx))) / sx;
                dst.height = static_cast<float>(std::max<long>(1L, std::lround(dst.height * sy))) / sy;
                if (std::abs(imgClip.x - command.rect.x) < 1e-4f && std::abs(imgClip.width - command.rect.width) < 1e-4f)
                {
                    imgClip.x = dst.x;
                    imgClip.width = dst.width;
                }
                if (std::abs(imgClip.y - command.rect.y) < 1e-4f && std::abs(imgClip.height - command.rect.height) < 1e-4f)
                {
                    imgClip.y = dst.y;
                    imgClip.height = dst.height;
                }
            }
            context.AppendQuad(dst, {0.0f, 0.0f, 1.0f, 1.0f}, command.color, info.textureToken, imgClip);
            break;
        }
        case UiPaintCommandKind::Line:
        {
            if (!std::isfinite(command.x0) || !std::isfinite(command.y0) || !std::isfinite(command.x1) ||
                !std::isfinite(command.y1) || !std::isfinite(command.lineWidth) || command.lineWidth <= 0.0f ||
                command.color.a <= 0.0f)
            {
                break;
            }
            const float pad = command.lineWidth * 0.5f + feather * 2.0f;
            const UiRect bounds{std::min(command.x0, command.x1) - pad,
                                std::min(command.y0, command.y1) - pad,
                                std::abs(command.x1 - command.x0) + pad * 2.0f,
                                std::abs(command.y1 - command.y0) + pad * 2.0f};
            if (!Finite(bounds) || Empty(Intersect(bounds, clip)))
            {
                break;
            }
            const float dx = command.x1 - command.x0;
            const float dy = command.y1 - command.y0;
            const float length = std::hypot(dx, dy);
            if (!std::isfinite(length) || length <= 0.0f)
            {
                break;
            }
            // 纯水平/纯垂直网格线直接走硬边矩形保持 1px 锐度；斜线走羽化抗锯齿带。
            if (std::abs(dx) > 1e-3f && std::abs(dy) > 1e-3f)
            {
                const Vec2 n{-dy / length, dx / length};
                const Vec2 pts[2]{{command.x0, command.y0}, {command.x1, command.y1}};
                const Vec2 nrms[2]{n, n};
                context.AppendFeatheredRibbon(pts, nrms, 2, command.lineWidth, command.color, options.whiteTexture, clip, feather);
                break;
            }
            const float ox = -dy / length * command.lineWidth * 0.5f;
            const float oy = dx / length * command.lineWidth * 0.5f;
            context.AppendTriangle({command.x0 + ox, command.y0 + oy},
                                   {command.x1 + ox, command.y1 + oy},
                                   {command.x1 - ox, command.y1 - oy},
                                   command.color, options.whiteTexture, clip);
            context.AppendTriangle({command.x0 + ox, command.y0 + oy},
                                   {command.x1 - ox, command.y1 - oy},
                                   {command.x0 - ox, command.y0 - oy},
                                   command.color, options.whiteTexture, clip);
            break;
        }
        case UiPaintCommandKind::CubicBezier:
        {
            if (!std::isfinite(command.lineWidth) || command.lineWidth <= 0.0f || command.color.a <= 0.0f)
            {
                break;
            }
            const float pad = command.lineWidth * 0.5f + feather * 2.0f;
            const float minX = std::min({command.x0, command.c0x, command.c1x, command.x1}) - pad;
            const float maxX = std::max({command.x0, command.c0x, command.c1x, command.x1}) + pad;
            const float minY = std::min({command.y0, command.c0y, command.c1y, command.y1}) - pad;
            const float maxY = std::max({command.y0, command.c0y, command.c1y, command.y1}) + pad;
            const UiRect bounds{minX, minY, maxX - minX, maxY - minY};
            if (!Finite(bounds) || Empty(Intersect(bounds, clip)))
            {
                break;
            }
            const float approxLen = std::hypot(command.c0x - command.x0, command.c0y - command.y0) +
                                    std::hypot(command.c1x - command.c0x, command.c1y - command.c0y) +
                                    std::hypot(command.x1 - command.c1x, command.y1 - command.c1y);
            if (!std::isfinite(approxLen) || approxLen <= 1e-3f)
            {
                break;
            }
            const int steps = std::clamp(static_cast<int>(std::ceil(approxLen / 5.0f)), 20, 64);
            std::array<Vec2, 65> pts{};
            std::array<Vec2, 65> nrms{};
            float fallbackDx = command.x1 - command.x0;
            float fallbackDy = command.y1 - command.y0;
            float fallbackLen = std::hypot(fallbackDx, fallbackDy);
            if (fallbackLen <= 1e-4f)
            {
                fallbackDx = command.c0x - command.x0;
                fallbackDy = command.c0y - command.y0;
                fallbackLen = std::hypot(fallbackDx, fallbackDy);
            }
            Vec2 prevNormal = fallbackLen > 1e-4f
                ? Vec2{-fallbackDy / fallbackLen, fallbackDx / fallbackLen}
                : Vec2{0.0f, 1.0f};
            for (int i = 0; i <= steps; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(steps);
                const float u = 1.0f - t;
                pts[i] = {
                    u * u * u * command.x0 + 3.0f * u * u * t * command.c0x +
                        3.0f * u * t * t * command.c1x + t * t * t * command.x1,
                    u * u * u * command.y0 + 3.0f * u * u * t * command.c0y +
                        3.0f * u * t * t * command.c1y + t * t * t * command.y1,
                };
                const float tx = 3.0f * u * u * (command.c0x - command.x0) +
                                 6.0f * u * t * (command.c1x - command.c0x) +
                                 3.0f * t * t * (command.x1 - command.c1x);
                const float ty = 3.0f * u * u * (command.c0y - command.y0) +
                                 6.0f * u * t * (command.c1y - command.c0y) +
                                 3.0f * t * t * (command.y1 - command.c1y);
                const float tlen = std::hypot(tx, ty);
                if (tlen > 1e-4f)
                {
                    Vec2 cand{-ty / tlen, tx / tlen};
                    if (i > 0 && (cand.x * prevNormal.x + cand.y * prevNormal.y) < -0.5f)
                    {
                        cand = {-cand.x, -cand.y};
                    }
                    prevNormal = cand;
                }
                nrms[i] = prevNormal;
            }
            context.AppendFeatheredRibbon(pts.data(), nrms.data(), static_cast<std::size_t>(steps + 1),
                                          command.lineWidth, command.color, options.whiteTexture, clip, feather);
            break;
        }
        case UiPaintCommandKind::CanvasRect:
            context.AppendQuad(command.rect, {options.whiteU, options.whiteV, options.whiteWidth, options.whiteHeight}, command.color,
                               options.whiteTexture, clip);
            break;
        case UiPaintCommandKind::CanvasCircle:
            context.AppendCircle(command.centerX, command.centerY, command.radius, command.color,
                                 options.whiteTexture, clip, feather);
            break;
        }
    }
    stats.drawCalls = static_cast<std::uint32_t>(geometry.commands.size());
    return errors.empty();
}

} // namespace flare::ui
