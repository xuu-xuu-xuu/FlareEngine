#include "ToolLayer/Editor/EditorUi/EditorShell.h"
#include "ToolLayer/Editor/EditorUi/EditorShellInternal.h"
#include "ToolLayer/Editor/EditorUi/EditorInk.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Utf.h"
#include "ToolLayer/Editor/FlarePalette.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace flare::ui::editor
{
using namespace detail;

namespace
{
constexpr float kSgToolbarHeight = 30.0f;
constexpr float kSgPropBarHeight = 28.0f;
constexpr float kSgPreviewCardW = 124.0f;
constexpr float kSgPreviewCardH = 140.0f;
constexpr float kSgPreviewSize = 112.0f;

UiColor PortTypeColor(std::uint8_t valueType)
{
    switch (valueType)
    {
    case 0: return UiColor{0.35f, 0.79f, 0.92f, 1.0f}; // Float: Cyan
    case 1: return UiColor{0.42f, 0.86f, 0.48f, 1.0f}; // Vec2: Green
    case 2: return UiColor{0.95f, 0.79f, 0.30f, 1.0f}; // Vec3: Gold
    default: return UiColor{0.91f, 0.48f, 0.98f, 1.0f}; // Vec4/Color: Magenta
    }
}

UiColor CategoryHeaderColor(std::string_view cat)
{
    if (cat == "Master") return UiColor{0.58f, 0.34f, 0.11f, 0.96f};
    if (cat == "Input") return UiColor{0.12f, 0.37f, 0.44f, 0.96f};
    if (cat == "Property") return UiColor{0.13f, 0.42f, 0.27f, 0.96f};
    if (cat == "Texture") return UiColor{0.37f, 0.22f, 0.52f, 0.96f};
    if (cat == "UV") return UiColor{0.14f, 0.40f, 0.38f, 0.96f};
    if (cat == "Math") return UiColor{0.16f, 0.30f, 0.49f, 0.96f};
    if (cat == "Vector") return UiColor{0.24f, 0.28f, 0.52f, 0.96f};
    if (cat == "Color") return UiColor{0.52f, 0.24f, 0.38f, 0.96f};
    if (cat == "Procedural") return UiColor{0.44f, 0.31f, 0.14f, 0.96f};
    return UiColor{0.48f, 0.21f, 0.26f, 0.96f}; // Artistic / Stylized
}

struct SearchCategoryTab
{
    const char* id;
    const char* shortLabel;
};

constexpr SearchCategoryTab kSearchCategoryTabs[10] = {
    {"", "All"},
    {"Input", "In"},
    {"Property", "Prop"},
    {"Texture", "Tex"},
    {"UV", "UV"},
    {"Math", "Math"},
    {"Vector", "Vec"},
    {"Color", "Col"},
    {"Procedural", "Proc"},
    {"Artistic", "Art"},
};

struct NodeLayoutMetrics
{
    UiRect card{};
    float headerH = 24.0f;
    float subH = 0.0f;
    float rowH = 21.0f;
};

NodeLayoutMetrics ComputeNodeMetrics(const EditorShaderNodeView& node, const UiRect& canvas,
                                     float panX, float panY, float zoom)
{
    const float z = std::clamp(zoom, 0.3f, 2.2f);
    const float baseW = node.isMaster ? 206.0f : 184.0f;
    const float headerH = 24.0f * z;
    const bool hasSub = !node.subtitle.empty() || node.swatchRgba[3] > 0.0f;
    const float subH = hasSub ? 20.0f * z : 0.0f;
    const float rowH = 21.0f * z;
    const std::size_t rows = std::max(node.inputs.size(), node.outputs.size());
    const float bodyH = static_cast<float>(std::max<std::size_t>(1, rows)) * rowH + 8.0f * z;
    NodeLayoutMetrics m;
    m.headerH = headerH;
    m.subH = subH;
    m.rowH = rowH;
    m.card = {
        canvas.x + panX + node.x * z,
        canvas.y + panY + node.y * z,
        baseW * z,
        headerH + subH + bodyH,
    };
    return m;
}

void PortCenter(const NodeLayoutMetrics& m, bool isOutput, std::size_t portIndex, float zoom,
                float& outX, float& outY)
{
    const float z = std::clamp(zoom, 0.3f, 2.2f);
    outX = isOutput ? (m.card.x + m.card.width - 9.0f * z) : (m.card.x + 9.0f * z);
    outY = m.card.y + m.headerH + m.subH + 4.0f * z + (static_cast<float>(portIndex) + 0.5f) * m.rowH;
}

void ComputeConnectedBezierControls(float x0, float y0, float x1, float y1, float zoom,
                                    float& c0x, float& c0y, float& c1x, float& c1y)
{
    const float z = std::clamp(zoom, 0.35f, 2.0f);
    const float dist = std::hypot(x1 - x0, y1 - y0);
    float dx = 0.0f;
    float archY = 0.0f;
    if (x1 >= x0)
    {
        dx = std::min(dist * 0.56f, std::max(38.0f * z, (x1 - x0) * 0.48f));
    }
    else
    {
        dx = std::clamp(48.0f * z + (x0 - x1) * 0.42f + std::abs(y1 - y0) * 0.18f, 34.0f * z, 240.0f * z);
        const float adY = std::abs(y1 - y0);
        if (adY < 72.0f * z)
        {
            const float factor = 1.0f - adY / (72.0f * z);
            archY = -48.0f * z * factor;
        }
    }
    c0x = x0 + dx;
    c0y = y0 + archY;
    c1x = x1 - dx;
    c1y = y1 + archY;
}

void ComputeLiveWireBezierControls(float portX, float portY, bool fromOutput,
                                   float curX, float curY, bool snapped, float zoom,
                                   float& x0, float& y0, float& c0x, float& c0y,
                                   float& c1x, float& c1y, float& x1, float& y1)
{
    if (snapped)
    {
        x0 = fromOutput ? portX : curX;
        y0 = fromOutput ? portY : curY;
        x1 = fromOutput ? curX : portX;
        y1 = fromOutput ? curY : portY;
        ComputeConnectedBezierControls(x0, y0, x1, y1, zoom, c0x, c0y, c1x, c1y);
        return;
    }
    const float z = std::clamp(zoom, 0.35f, 2.0f);
    const float dist = std::hypot(curX - portX, curY - portY);
    const float portHandle = std::min(dist * 0.55f, std::max(32.0f * z, std::abs(curX - portX) * 0.45f));
    if (fromOutput)
    {
        x0 = portX;
        y0 = portY;
        x1 = curX;
        y1 = curY;
        const float forwardFactor = std::clamp((curX - portX + 28.0f * z) / (56.0f * z), 0.0f, 1.0f);
        c0x = x0 + portHandle;
        c0y = y0;
        c1x = x1 - portHandle * 0.62f * forwardFactor;
        c1y = y1;
    }
    else
    {
        x0 = curX;
        y0 = curY;
        x1 = portX;
        y1 = portY;
        const float forwardFactor = std::clamp((portX - curX + 28.0f * z) / (56.0f * z), 0.0f, 1.0f);
        c0x = x0 + portHandle * 0.62f * forwardFactor;
        c0y = y0;
        c1x = x1 - portHandle;
        c1y = y1;
    }
}

void DrawSmoothBezier(UiPaintSink& sink, float x0, float y0, float c0x, float c0y,
                      float c1x, float c1y, float x1, float y1,
                      float width, const UiColor& color)
{
    sink.CubicBezier(x0, y0, c0x, c0y, c1x, c1y, x1, y1, width, color);
}

float PointToSegmentDistSq(float px, float py, float ax, float ay, float bx, float by)
{
    const float vx = bx - ax, vy = by - ay;
    const float wx = px - ax, wy = py - ay;
    const float lenSq = vx * vx + vy * vy;
    const float t = lenSq > 1e-5f ? std::clamp((wx * vx + wy * vy) / lenSq, 0.0f, 1.0f) : 0.0f;
    const float dx = px - (ax + t * vx);
    const float dy = py - (ay + t * vy);
    return dx * dx + dy * dy;
}

bool HitCubicBezier(float px, float py, float x0, float y0, float x1, float y1, float zoom, float thresholdPx,
                    float* outDistSq = nullptr)
{
    float c0x = 0, c0y = 0, c1x = 0, c1y = 0;
    ComputeConnectedBezierControls(x0, y0, x1, y1, zoom, c0x, c0y, c1x, c1y);
    constexpr int kSteps = 24;
    float prevX = x0, prevY = y0;
    const float threshSq = thresholdPx * thresholdPx;
    float minSq = 1e30f;
    for (int i = 1; i <= kSteps; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kSteps);
        const float u = 1.0f - t;
        const float nx = u * u * u * x0 + 3.0f * u * u * t * c0x + 3.0f * u * t * t * c1x + t * t * t * x1;
        const float ny = u * u * u * y0 + 3.0f * u * u * t * c0y + 3.0f * u * t * t * c1y + t * t * t * y1;
        minSq = std::min(minSq, PointToSegmentDistSq(px, py, prevX, prevY, nx, ny));
        prevX = nx;
        prevY = ny;
    }
    if (outDistSq) *outDistSq = minSq;
    return minSq <= threshSq;
}

struct PortHitCandidate
{
    std::uint32_t nodeId = 0;
    std::string portId;
    std::uint8_t valueType = 0;
    bool isOutput = false;
    bool connected = false;
    float px = 0.0f;
    float py = 0.0f;
};

bool FindClosestPortAt(const EditorShaderGraphViewState& view, const UiRect& cr, float z,
                       float mx, float my, PortHitCandidate& outHit)
{
    const float circleR = std::max(11.5f, 13.0f * z);
    const float circleRSq = circleR * circleR;
    const float pillInward = 30.0f * z;
    const float pillOutward = 9.0f * z;

    for (auto it = view.nodes.rbegin(); it != view.nodes.rend(); ++it)
    {
        const NodeLayoutMetrics m = ComputeNodeMetrics(*it, cr, view.panX, view.panY, z);
        if (mx < m.card.x - circleR || mx > m.card.x + m.card.width + circleR ||
            my < m.card.y - circleR || my > m.card.y + m.card.height + circleR)
        {
            continue;
        }
        float bestSq = 1e30f;
        bool foundOnNode = false;
        PortHitCandidate best{};

        for (std::size_t oi = 0; oi < it->outputs.size(); ++oi)
        {
            float px = 0, py = 0;
            PortCenter(m, true, oi, z, px, py);
            const float dx = mx - px, dy = my - py;
            const float dsq = dx * dx + dy * dy;
            const bool inPill = std::abs(dy) <= m.rowH * 0.45f && mx >= px - pillInward && mx <= px + pillOutward;
            if ((dsq <= circleRSq || inPill) && dsq < bestSq)
            {
                bestSq = dsq;
                foundOnNode = true;
                best = {it->id, it->outputs[oi].id, it->outputs[oi].valueType, true, it->outputs[oi].connected, px, py};
            }
        }
        for (std::size_t ii = 0; ii < it->inputs.size(); ++ii)
        {
            float px = 0, py = 0;
            PortCenter(m, false, ii, z, px, py);
            const float dx = mx - px, dy = my - py;
            const float dsq = dx * dx + dy * dy;
            const bool inPill = std::abs(dy) <= m.rowH * 0.45f && mx >= px - pillOutward && mx <= px + pillInward;
            if ((dsq <= circleRSq || inPill) && dsq < bestSq)
            {
                bestSq = dsq;
                foundOnNode = true;
                best = {it->id, it->inputs[ii].id, it->inputs[ii].valueType, false, it->inputs[ii].connected, px, py};
            }
        }
        if (foundOnNode)
        {
            outHit = std::move(best);
            return true;
        }
        // 若指针已被前景节点卡片本体遮挡，不再穿透命中底下被遮住节点的端口。
        if (mx >= m.card.x && mx <= m.card.x + m.card.width &&
            my >= m.card.y && my <= m.card.y + m.card.height)
        {
            return false;
        }
    }
    return false;
}

void UpdateMagneticWireSnap(EditorShaderGraphRuntime& s, const UiRect& cr, float z)
{
    s.snapNodeId = 0;
    s.snapPortId.clear();
    if (!s.draggingWire || !s.view.loaded) return;

    const float snapR = std::max(19.0f, 22.0f * z);
    const float snapRSq = snapR * snapR;
    const float pillInward = 38.0f * z;
    const float pillOutward = 12.0f * z;
    float bestSq = 1e30f;

    for (auto it = s.view.nodes.rbegin(); it != s.view.nodes.rend(); ++it)
    {
        if (it->id == s.wireNodeId) continue;
        const NodeLayoutMetrics m = ComputeNodeMetrics(*it, cr, s.view.panX, s.view.panY, z);
        if (s.wireCurX < m.card.x - snapR || s.wireCurX > m.card.x + m.card.width + snapR ||
            s.wireCurY < m.card.y - snapR || s.wireCurY > m.card.y + m.card.height + snapR)
        {
            continue;
        }
        if (s.wireFromOutput)
        {
            for (std::size_t ii = 0; ii < it->inputs.size(); ++ii)
            {
                float px = 0, py = 0;
                PortCenter(m, false, ii, z, px, py);
                const float dx = s.wireCurX - px, dy = s.wireCurY - py;
                const float dsq = dx * dx + dy * dy;
                const bool inPill = std::abs(dy) <= m.rowH * 0.48f &&
                                    s.wireCurX >= px - pillOutward && s.wireCurX <= px + pillInward;
                if ((dsq <= snapRSq || inPill) && dsq < bestSq)
                {
                    bestSq = dsq;
                    s.snapNodeId = it->id;
                    s.snapPortId = it->inputs[ii].id;
                    s.snapIsOutput = false;
                    s.snapPortX = px;
                    s.snapPortY = py;
                }
            }
        }
        else
        {
            for (std::size_t oi = 0; oi < it->outputs.size(); ++oi)
            {
                float px = 0, py = 0;
                PortCenter(m, true, oi, z, px, py);
                const float dx = s.wireCurX - px, dy = s.wireCurY - py;
                const float dsq = dx * dx + dy * dy;
                const bool inPill = std::abs(dy) <= m.rowH * 0.48f &&
                                    s.wireCurX >= px - pillInward && s.wireCurX <= px + pillOutward;
                if ((dsq <= snapRSq || inPill) && dsq < bestSq)
                {
                    bestSq = dsq;
                    s.snapNodeId = it->id;
                    s.snapPortId = it->outputs[oi].id;
                    s.snapIsOutput = true;
                    s.snapPortX = px;
                    s.snapPortY = py;
                }
            }
        }
    }
}

std::size_t FindClosestWireAt(const EditorShaderGraphViewState& view, const UiRect& cr, float z,
                              float mx, float my, float thresholdPx)
{
    std::size_t bestIdx = static_cast<std::size_t>(-1);
    float bestSq = thresholdPx * thresholdPx;
    for (std::size_t li = 0; li < view.links.size(); ++li)
    {
        const auto& lnk = view.links[li];
        const EditorShaderNodeView* fn = nullptr;
        const EditorShaderNodeView* tn = nullptr;
        for (const auto& n : view.nodes)
        {
            if (n.id == lnk.fromNode) fn = &n;
            if (n.id == lnk.toNode) tn = &n;
        }
        if (!fn || !tn) continue;
        std::size_t oi = 0, ii = 0;
        bool foundO = false, foundI = false;
        for (std::size_t k = 0; k < fn->outputs.size(); ++k) if (fn->outputs[k].id == lnk.fromPort) { oi = k; foundO = true; break; }
        for (std::size_t k = 0; k < tn->inputs.size(); ++k) if (tn->inputs[k].id == lnk.toPort) { ii = k; foundI = true; break; }
        if (!foundO || !foundI) continue;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        PortCenter(ComputeNodeMetrics(*fn, cr, view.panX, view.panY, z), true, oi, z, x0, y0);
        PortCenter(ComputeNodeMetrics(*tn, cr, view.panX, view.panY, z), false, ii, z, x1, y1);
        float dsq = 1e30f;
        if (HitCubicBezier(mx, my, x0, y0, x1, y1, z, thresholdPx, &dsq) && dsq <= bestSq)
        {
            bestSq = dsq;
            bestIdx = li;
        }
    }
    return bestIdx;
}

bool ContainsLower(std::string_view hay, std::string_view needle)
{
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    const auto low = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
    {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j)
        {
            if (low(hay[i + j]) != low(needle[j])) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

std::string Fmt2(float a, float b)
{
    char buf[96]{};
    std::snprintf(buf, sizeof(buf), "%.3f,%.3f", a, b);
    return buf;
}

std::string Fmt3(float a, float b, float c)
{
    char buf[128]{};
    std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f", a, b, c);
    return buf;
}
} // namespace

void EditorShell::BuildShaderGraphPanel(PanelSlot& slot)
{
    if (!shaderGraph_) shaderGraph_ = std::make_unique<EditorShaderGraphRuntime>();
    auto& sg = *shaderGraph_;

    // Canvas
    sg.canvas = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(sg.canvas, slot.content);
    UiStyle cs = AbsoluteStyle(0.0f, kSgToolbarHeight, 400.0f, 300.0f);
    cs.background = UiColor{0.055f, 0.062f, 0.078f, 1.0f};
    cs.overflow = UiOverflow::Hidden;
    cs.image = "shader-graph-canvas";
    document_.SetStyle(sg.canvas, cs);

    // Top toolbar
    sg.toolbar = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(sg.toolbar, slot.content);
    UiStyle tbs = AbsoluteStyle(0.0f, 0.0f, 400.0f, kSgToolbarHeight);
    tbs.background = kSurfaceRaised;
    tbs.borderWidth = 1.0f;
    tbs.borderColor = kBorder;
    document_.SetStyle(sg.toolbar, tbs);

    const auto makeTbBtn = [&](UiElementHandle& outBtn, const UiColor& bg, std::function<void()> onClick) {
        outBtn = document_.CreateElement(UiElementKind::Button);
        document_.SetParent(outBtn, sg.toolbar);
        UiStyle bs = ButtonStyle(0.0f, 3.0f, 80.0f, 24.0f, bg);
        bs.cornerRadius = 4.0f;
        bs.borderWidth = 1.0f;
        bs.borderColor = kBorder;
        bs.fontSize = 12.0f;
        document_.SetStyle(outBtn, bs);
        dispatcher_.AddHandler(outBtn, UiEventType::PointerDown, [onClick = std::move(onClick)](UiInputEvent& e) {
            if (e.button == 0 && onClick) onClick();
            e.handled = true;
            e.StopPropagation();
        });
    };

    sg.titleLabel = document_.CreateElement(UiElementKind::Label);
    document_.SetParent(sg.titleLabel, sg.toolbar);
    {
        UiStyle ts = ButtonStyle(8.0f, 3.0f, 150.0f, 24.0f, UiColor{0, 0, 0, 0});
        ts.fontSize = 12.5f;
        ts.color = UiInk(flare::palette::Heading);
        document_.SetStyle(sg.titleLabel, ts);
    }

    makeTbBtn(sg.shadingBtn, kSurface, [this]() {
        if (shaderGraph_ && shaderGraph_->onAction) shaderGraph_->onAction("cycle_shading", {});
    });
    makeTbBtn(sg.blendBtn, kSurface, [this]() {
        if (shaderGraph_ && shaderGraph_->onAction) shaderGraph_->onAction("cycle_blend", {});
    });
    makeTbBtn(sg.doubleSidedBtn, kSurface, [this]() {
        if (shaderGraph_ && shaderGraph_->onAction) shaderGraph_->onAction("toggle_doublesided", {});
    });
    const auto openSearchMenuAt = [this](float localLeft, float localTop) {
        if (!shaderGraph_) return;
        auto& s = *shaderGraph_;
        s.searchEdit->SetText("");
        s.searchCategoryFilter.clear();
        s.searchHighlight = 0;
        s.searchListOffset = 0;
        const UiRect slotRect = layout_.Rect(Slot(EditorPanelId::ShaderGraph).content);
        const float popupW = 328.0f;
        const float popupH = std::clamp(slotRect.height - 38.0f, 178.0f, 310.0f);
        UiStyle sps = document_.Style(s.searchPopup);
        sps.width = UiLength::Px(popupW);
        sps.height = UiLength::Px(popupH);
        sps.left = UiLength::Px(std::clamp(localLeft, 6.0f, std::max(6.0f, slotRect.width - popupW - 6.0f)));
        sps.top = UiLength::Px(std::clamp(localTop, 32.0f, std::max(32.0f, slotRect.height - popupH - 4.0f)));
        document_.SetStyle(s.searchPopup, sps);
        document_.SetVisible(s.searchPopup, true);
        RefreshShaderGraphChrome();
        LayoutShaderGraphPanel(slotRect.width, slotRect.height);
        dispatcher_.SetFocus(s.searchBox);
    };

    makeTbBtn(sg.addNodeBtn, UiColor{0.16f, 0.34f, 0.54f, 1.0f}, [this, openSearchMenuAt]() {
        if (!shaderGraph_ || !shaderGraph_->view.loaded) return;
        const UiRect cr = layout_.Rect(shaderGraph_->canvas);
        const float z = std::clamp(shaderGraph_->view.zoom, 0.3f, 2.2f);
        shaderGraph_->spawnGraphX = (cr.width * 0.42f - shaderGraph_->view.panX) / z;
        shaderGraph_->spawnGraphY = (cr.height * 0.38f - shaderGraph_->view.panY) / z;
        shaderGraph_->pendingWireDir.clear();
        shaderGraph_->pendingWireNode = 0;
        shaderGraph_->pendingWirePort.clear();
        openSearchMenuAt(72.0f, 34.0f);
    });
    makeTbBtn(sg.frameBtn, kSurface, [this]() {
        if (shaderGraph_ && shaderGraph_->onAction) shaderGraph_->onAction("frame_all", {});
    });
    makeTbBtn(sg.saveBtn, UiColor{0.15f, 0.45f, 0.29f, 1.0f}, [this]() {
        if (shaderGraph_ && shaderGraph_->onAction) shaderGraph_->onAction("save", {});
    });
    makeTbBtn(sg.hlslBtn, kSurface, [this]() {
        if (shaderGraph_ && shaderGraph_->onAction) shaderGraph_->onAction("view_hlsl", {});
    });
    makeTbBtn(sg.newMatBtn, kSurface, [this]() {
        if (shaderGraph_ && shaderGraph_->onAction) shaderGraph_->onAction("create_material", {});
    });

    // Selected-node inline property bar
    sg.propBar = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(sg.propBar, slot.content);
    {
        UiStyle ps = AbsoluteStyle(0.0f, kSgToolbarHeight, 400.0f, kSgPropBarHeight);
        ps.background = UiColor{0.10f, 0.12f, 0.15f, 0.96f};
        ps.borderWidth = 1.0f;
        ps.borderColor = kBorder;
        document_.SetStyle(sg.propBar, ps);
        document_.SetVisible(sg.propBar, false);
    }

    sg.exposeBtn = document_.CreateElement(UiElementKind::Button);
    document_.SetParent(sg.exposeBtn, sg.propBar);
    {
        UiStyle es = ButtonStyle(8.0f, 3.0f, 136.0f, 22.0f, kSurface);
        es.cornerRadius = 4.0f;
        es.borderWidth = 1.0f;
        es.borderColor = kBorder;
        es.fontSize = 12.0f;
        document_.SetStyle(sg.exposeBtn, es);
        dispatcher_.AddHandler(sg.exposeBtn, UiEventType::PointerDown, [this](UiInputEvent& e) {
            if (e.button == 0 && shaderGraph_ && shaderGraph_->onAction)
                shaderGraph_->onAction("toggle_expose", {});
            e.handled = true;
            e.StopPropagation();
        });
    }

    const auto wireTextEdit = [&](UiElementHandle& box, UiElementHandle& sel, std::unique_ptr<UiTextEdit>& editPtr,
                                  const char* fieldTag, std::string_view commitAction) {
        box = document_.CreateElement(UiElementKind::TextEdit);
        document_.SetParent(box, sg.propBar);
        UiStyle xs = ButtonStyle(0.0f, 3.0f, 120.0f, 22.0f, kSurface);
        xs.cornerRadius = 3.0f;
        xs.borderWidth = 1.0f;
        xs.borderColor = kBorder;
        xs.fontSize = 12.0f;
        document_.SetStyle(box, xs);
        sel = document_.CreateElement(UiElementKind::Panel);
        document_.SetParent(sel, box);
        document_.SetVisible(sel, false);
        editPtr = std::make_unique<UiTextEdit>(document_, box, text_, PlatformServices());

        const std::string tag(fieldTag);
        const std::string act(commitAction);
        const auto getEdit = [this, tag]() -> UiTextEdit* {
            if (!shaderGraph_) return nullptr;
            return (tag == "param") ? shaderGraph_->paramNameEdit.get() : shaderGraph_->valueEdit.get();
        };
        const auto syncVisual = [this, tag, getEdit]() {
            auto* ed = getEdit();
            if (!ed || !shaderGraph_) return;
            const UiElementHandle b = (tag == "param") ? shaderGraph_->paramNameBox : shaderGraph_->valueBox;
            document_.SetText(b, ed->DisplayText());
        };
        dispatcher_.AddHandler(box, UiEventType::PointerDown, [this, tag, getEdit](UiInputEvent& e) {
            auto* ed = getEdit();
            if (!ed || !shaderGraph_) return;
            shaderGraph_->editingField = tag;
            if (tag == "param") ed->SetText(shaderGraph_->view.primaryParamName);
            else if (tag == "value") ed->SetText(shaderGraph_->view.primaryValueText);
            const UiElementHandle b = (tag == "param") ? shaderGraph_->paramNameBox : shaderGraph_->valueBox;
            document_.SetText(b, ed->DisplayText());
            dispatcher_.SetFocus(b);
            e.handled = true;
            e.StopPropagation();
        });
        dispatcher_.AddHandler(box, UiEventType::KeyDown, [this, act, getEdit, syncVisual](UiInputEvent& e) {
            auto* ed = getEdit();
            if (!ed) return;
            if (e.key == UiKeyEscape)
            {
                if (shaderGraph_) shaderGraph_->editingField.clear();
                dispatcher_.ClearFocus();
                RefreshShaderGraphChrome();
                e.handled = true;
                return;
            }
            if (e.key == UiKeyEnter)
            {
                const std::string committed(ed->Text());
                if (shaderGraph_) shaderGraph_->editingField.clear();
                dispatcher_.ClearFocus();
                if (shaderGraph_ && shaderGraph_->onAction) shaderGraph_->onAction(act, committed);
                e.handled = true;
                return;
            }
            ed->HandleKey(e.key, e.shift, e.control);
            syncVisual();
            e.handled = true;
        });
        dispatcher_.AddHandler(box, UiEventType::TextInput, [getEdit, syncVisual](UiInputEvent& e) {
            auto* ed = getEdit();
            if (!ed || e.codepoint == 0) return;
            const char32_t cp = e.codepoint;
            std::string u8;
            if (EncodeUtf8({&cp, 1}, u8))
            {
                ed->InsertText(u8);
                syncVisual();
                e.handled = true;
            }
        });
        dispatcher_.AddHandler(box, UiEventType::ImeComposition, [getEdit, syncVisual](UiInputEvent& e) {
            if (auto* ed = getEdit()) { ed->SetComposition(e.text); syncVisual(); e.handled = true; }
        });
        dispatcher_.AddHandler(box, UiEventType::ImeCommit, [getEdit, syncVisual](UiInputEvent& e) {
            if (auto* ed = getEdit()) { ed->SetComposition(e.text); ed->CommitComposition(); syncVisual(); e.handled = true; }
        });
        dispatcher_.AddHandler(box, UiEventType::FocusLost, [this, tag, act, getEdit](UiInputEvent&) {
            if (!shaderGraph_ || shaderGraph_->editingField != tag) return;
            auto* ed = getEdit();
            const std::string committed = ed ? std::string(ed->Text()) : std::string{};
            shaderGraph_->editingField.clear();
            if (ed && shaderGraph_->onAction) shaderGraph_->onAction(act, committed);
            else RefreshShaderGraphChrome();
        });
    };

    wireTextEdit(sg.paramNameBox, sg.paramNameSel, sg.paramNameEdit, "param", "set_param_name");
    wireTextEdit(sg.valueBox, sg.valueSel, sg.valueEdit, "value", "set_node_value");

    // Bottom-right mini preview sphere card
    sg.previewCard = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(sg.previewCard, slot.content);
    {
        UiStyle pcs = AbsoluteStyle(0.0f, 0.0f, kSgPreviewCardW, kSgPreviewCardH);
        pcs.background = UiColor{0.08f, 0.095f, 0.12f, 0.94f};
        pcs.borderWidth = 1.0f;
        pcs.borderColor = kBorder;
        pcs.cornerRadius = 6.0f;
        document_.SetStyle(sg.previewCard, pcs);
    }
    sg.previewImage = document_.CreateElement(UiElementKind::Image);
    document_.SetParent(sg.previewImage, sg.previewCard);
    {
        UiStyle pis = AbsoluteStyle(6.0f, 6.0f, kSgPreviewSize, kSgPreviewSize);
        pis.image = "shader-graph-preview";
        document_.SetStyle(sg.previewImage, pis);
    }

    sg.previewCaption = document_.CreateElement(UiElementKind::Label);
    document_.SetParent(sg.previewCaption, sg.previewCard);
    {
        UiStyle cs2 = ButtonStyle(4.0f, 120.0f, kSgPreviewCardW - 8.0f, 18.0f, UiColor{0, 0, 0, 0});
        cs2.fontSize = 10.5f;
        cs2.color = kMuted;
        document_.SetStyle(sg.previewCaption, cs2);
        document_.SetText(sg.previewCaption, Text("EditorUi.ShaderGraphPreviewHint", "Preview (Drag light)"));
    }

    dispatcher_.AddHandler(sg.previewCard, UiEventType::PointerDown, [this](UiInputEvent& e) {
        if (!shaderGraph_) return;
        shaderGraph_->orbitingPreview = true;
        shaderGraph_->orbitLastX = e.x;
        shaderGraph_->orbitLastY = e.y;
        dispatcher_.SetPointerCapture(shaderGraph_->previewCard);
        e.handled = true;
        e.StopPropagation();
    });
    dispatcher_.AddHandler(sg.previewCard, UiEventType::PointerMove, [this](UiInputEvent& e) {
        if (!shaderGraph_ || !shaderGraph_->orbitingPreview) return;
        const float dx = e.x - shaderGraph_->orbitLastX;
        const float dy = e.y - shaderGraph_->orbitLastY;
        shaderGraph_->orbitLastX = e.x;
        shaderGraph_->orbitLastY = e.y;
        if (shaderGraph_->onAction && (std::abs(dx) > 0.1f || std::abs(dy) > 0.1f))
            shaderGraph_->onAction("orbit_preview", Fmt2(dx * 0.022f, -dy * 0.022f));
        e.handled = true;
        e.StopPropagation();
    });
    dispatcher_.AddHandler(sg.previewCard, UiEventType::PointerUp, [this](UiInputEvent& e) {
        if (!shaderGraph_) return;
        shaderGraph_->orbitingPreview = false;
        dispatcher_.ReleasePointerCapture();
        e.handled = true;
        e.StopPropagation();
    });

    // Fuzzy-search & Categorized Node Palette Popup (130+ Unity/UE style nodes, wheel-scrollable)
    sg.searchPopup = document_.CreateElement(UiElementKind::Panel);
    document_.SetParent(sg.searchPopup, slot.content);
    {
        UiStyle sps = AbsoluteStyle(60.0f, 34.0f, 328.0f, 310.0f);
        sps.background = UiColor{0.085f, 0.10f, 0.13f, 0.985f};
        sps.borderWidth = 1.0f;
        sps.borderColor = kAccent;
        sps.cornerRadius = 6.0f;
        sps.overflow = UiOverflow::Hidden;
        document_.SetStyle(sg.searchPopup, sps);
        document_.SetVisible(sg.searchPopup, false);
    }
    sg.searchBox = document_.CreateElement(UiElementKind::TextEdit);
    document_.SetParent(sg.searchBox, sg.searchPopup);
    {
        UiStyle sbs = ButtonStyle(6.0f, 6.0f, 244.0f, 22.0f, kSurface);
        sbs.borderWidth = 1.0f;
        sbs.borderColor = kAccent;
        sbs.cornerRadius = 4.0f;
        sbs.fontSize = 11.5f;
        document_.SetStyle(sg.searchBox, sbs);
    }
    sg.searchCountLabel = document_.CreateElement(UiElementKind::Label);
    document_.SetParent(sg.searchCountLabel, sg.searchPopup);
    {
        UiStyle cls = ButtonStyle(254.0f, 6.0f, 68.0f, 22.0f, UiColor{0, 0, 0, 0});
        cls.fontSize = 10.5f;
        cls.color = kMuted;
        document_.SetStyle(sg.searchCountLabel, cls);
    }
    sg.searchEdit = std::make_unique<UiTextEdit>(document_, sg.searchBox, text_, PlatformServices());

    sg.searchCategoryButtons.clear();
    {
        float cx = 6.0f;
        for (std::size_t ci = 0; ci < 10; ++ci)
        {
            const float cw = (ci == 0 || ci == 1 || ci == 4) ? 26.0f : (ci == 5 || ci == 8 ? 35.0f : 31.0f);
            UiElementHandle cbtn = document_.CreateElement(UiElementKind::Button);
            document_.SetParent(cbtn, sg.searchPopup);
            UiStyle cs2 = ButtonStyle(cx, 31.0f, cw, 19.0f, kSurface);
            cs2.cornerRadius = 3.0f;
            cs2.fontSize = 10.0f;
            document_.SetStyle(cbtn, cs2);
            document_.SetText(cbtn, kSearchCategoryTabs[ci].shortLabel);
            dispatcher_.AddHandler(cbtn, UiEventType::PointerDown, [this, ci](UiInputEvent& e) {
                if (e.button == 0 && shaderGraph_)
                {
                    shaderGraph_->searchCategoryFilter = kSearchCategoryTabs[ci].id;
                    shaderGraph_->searchHighlight = 0;
                    shaderGraph_->searchListOffset = 0;
                    RefreshShaderGraphChrome();
                    dispatcher_.SetFocus(shaderGraph_->searchBox);
                }
                e.handled = true;
                e.StopPropagation();
            });
            sg.searchCategoryButtons.push_back(cbtn);
            cx += cw + 2.0f;
        }
    }

    sg.searchItemButtons.clear();
    for (std::size_t i = 0; i < 10; ++i)
    {
        UiElementHandle item = document_.CreateElement(UiElementKind::Button);
        document_.SetParent(item, sg.searchPopup);
        UiStyle is = ButtonStyle(6.0f, 54.0f + static_cast<float>(i) * 25.0f, 316.0f, 23.0f, kSurface);
        is.cornerRadius = 3.0f;
        is.fontSize = 11.5f;
        document_.SetStyle(item, is);
        dispatcher_.AddHandler(item, UiEventType::PointerDown, [this, i](UiInputEvent& e) {
            if (e.button == 0 && shaderGraph_)
            {
                const std::size_t idx = shaderGraph_->searchListOffset + i;
                if (idx < shaderGraph_->filteredPaletteIndices.size())
                {
                    const auto& entry = shaderGraph_->palette[shaderGraph_->filteredPaletteIndices[idx]];
                    std::string payload = entry.type + "|" + Fmt2(shaderGraph_->spawnGraphX, shaderGraph_->spawnGraphY);
                    if (!shaderGraph_->pendingWireDir.empty())
                    {
                        payload += "|" + shaderGraph_->pendingWireDir + "|" +
                                   std::to_string(shaderGraph_->pendingWireNode) + "|" + shaderGraph_->pendingWirePort;
                    }
                    document_.SetVisible(shaderGraph_->searchPopup, false);
                    shaderGraph_->pendingWireDir.clear();
                    shaderGraph_->pendingWireNode = 0;
                    shaderGraph_->pendingWirePort.clear();
                    dispatcher_.ClearFocus();
                    if (shaderGraph_->onAction) shaderGraph_->onAction("add_node", payload);
                }
            }
            e.handled = true;
            e.StopPropagation();
        });
        sg.searchItemButtons.push_back(item);
    }

    dispatcher_.AddHandler(sg.searchPopup, UiEventType::PointerWheel, [this](UiInputEvent& e) {
        if (!shaderGraph_ || !document_.Visible(shaderGraph_->searchPopup)) return;
        auto& s = *shaderGraph_;
        const float popupH = document_.Style(s.searchPopup).height.value;
        const std::size_t visRows = static_cast<std::size_t>(std::clamp(static_cast<int>((popupH - 56.0f) / 25.0f), 3, 10));
        if (s.filteredPaletteIndices.size() > visRows)
        {
            const std::size_t maxOff = s.filteredPaletteIndices.size() - visRows;
            if (e.wheel < 0.0f) s.searchListOffset = std::min(maxOff, s.searchListOffset + 3);
            else if (e.wheel > 0.0f) s.searchListOffset = s.searchListOffset > 3 ? (s.searchListOffset - 3) : 0;
            s.searchHighlight = std::clamp(s.searchHighlight, s.searchListOffset, s.searchListOffset + visRows - 1);
            RefreshShaderGraphChrome();
        }
        e.handled = true;
        e.StopPropagation();
    });

    const auto syncSearchPopup = [this]() {
        if (!shaderGraph_) return;
        RefreshShaderGraphChrome();
    };
    dispatcher_.AddHandler(sg.searchBox, UiEventType::PointerDown, [this](UiInputEvent& e) {
        if (shaderGraph_) dispatcher_.SetFocus(shaderGraph_->searchBox);
        e.handled = true;
        e.StopPropagation();
    });
    dispatcher_.AddHandler(sg.searchBox, UiEventType::KeyDown, [this, syncSearchPopup](UiInputEvent& e) {
        if (!shaderGraph_) return;
        auto& s = *shaderGraph_;
        const float popupH = document_.Style(s.searchPopup).height.value;
        const std::size_t visRows = static_cast<std::size_t>(std::clamp(static_cast<int>((popupH - 56.0f) / 25.0f), 3, 10));
        if (e.key == UiKeyEscape)
        {
            document_.SetVisible(s.searchPopup, false);
            s.pendingWireDir.clear();
            s.pendingWireNode = 0;
            s.pendingWirePort.clear();
            dispatcher_.ClearFocus();
            e.handled = true;
            return;
        }
        if (e.key == UiKeyDown)
        {
            if (!s.filteredPaletteIndices.empty())
            {
                s.searchHighlight = (s.searchHighlight + 1) % s.filteredPaletteIndices.size();
                if (s.searchHighlight < s.searchListOffset) s.searchListOffset = s.searchHighlight;
                else if (s.searchHighlight >= s.searchListOffset + visRows) s.searchListOffset = s.searchHighlight - visRows + 1;
            }
            syncSearchPopup();
            e.handled = true;
            return;
        }
        if (e.key == UiKeyUp)
        {
            if (!s.filteredPaletteIndices.empty())
            {
                s.searchHighlight = (s.searchHighlight + s.filteredPaletteIndices.size() - 1) % s.filteredPaletteIndices.size();
                if (s.searchHighlight < s.searchListOffset) s.searchListOffset = s.searchHighlight;
                else if (s.searchHighlight >= s.searchListOffset + visRows) s.searchListOffset = s.searchHighlight - visRows + 1;
            }
            syncSearchPopup();
            e.handled = true;
            return;
        }
        if (e.key == UiKeyEnter)
        {
            if (!s.filteredPaletteIndices.empty())
            {
                const std::size_t pick = std::min(s.searchHighlight, s.filteredPaletteIndices.size() - 1);
                const auto& entry = s.palette[s.filteredPaletteIndices[pick]];
                std::string payload = entry.type + "|" + Fmt2(s.spawnGraphX, s.spawnGraphY);
                if (!s.pendingWireDir.empty())
                {
                    payload += "|" + s.pendingWireDir + "|" + std::to_string(s.pendingWireNode) + "|" + s.pendingWirePort;
                }
                document_.SetVisible(s.searchPopup, false);
                s.pendingWireDir.clear();
                s.pendingWireNode = 0;
                s.pendingWirePort.clear();
                dispatcher_.ClearFocus();
                if (s.onAction) s.onAction("add_node", payload);
            }
            e.handled = true;
            return;
        }
        s.searchHighlight = 0;
        s.searchListOffset = 0;
        s.searchEdit->HandleKey(e.key, e.shift, e.control);
        syncSearchPopup();
        e.handled = true;
    });
    dispatcher_.AddHandler(sg.searchBox, UiEventType::TextInput, [this, syncSearchPopup](UiInputEvent& e) {
        if (!shaderGraph_ || e.codepoint == 0) return;
        const char32_t cp = e.codepoint;
        std::string u8;
        if (EncodeUtf8({&cp, 1}, u8))
        {
            shaderGraph_->searchHighlight = 0;
            shaderGraph_->searchListOffset = 0;
            shaderGraph_->searchEdit->InsertText(u8);
            syncSearchPopup();
            e.handled = true;
        }
    });
    dispatcher_.AddHandler(sg.searchBox, UiEventType::ImeComposition, [this, syncSearchPopup](UiInputEvent& e) {
        if (shaderGraph_) { shaderGraph_->searchEdit->SetComposition(e.text); syncSearchPopup(); }
        e.handled = true;
    });
    dispatcher_.AddHandler(sg.searchBox, UiEventType::ImeCommit, [this, syncSearchPopup](UiInputEvent& e) {
        if (shaderGraph_)
        {
            shaderGraph_->searchEdit->SetComposition(e.text);
            shaderGraph_->searchEdit->CommitComposition();
            syncSearchPopup();
        }
        e.handled = true;
    });

    // Canvas mouse & wheel & Space shortcut gestures
    dispatcher_.AddHandler(sg.canvas, UiEventType::PointerWheel, [this](UiInputEvent& e) {
        if (!shaderGraph_ || !shaderGraph_->view.loaded) return;
        auto& s = *shaderGraph_;
        const UiRect cr = layout_.Rect(s.canvas);
        const float oldZoom = std::clamp(s.view.zoom, 0.3f, 2.2f);
        const float factor = e.wheel > 0.0f ? 1.12f : (1.0f / 1.12f);
        const float newZoom = std::clamp(oldZoom * factor, 0.35f, 2.0f);
        const float mx = e.x - cr.x;
        const float my = e.y - cr.y;
        const float gx = (mx - s.view.panX) / oldZoom;
        const float gy = (my - s.view.panY) / oldZoom;
        s.view.panX = mx - gx * newZoom;
        s.view.panY = my - gy * newZoom;
        s.view.zoom = newZoom;
        if (s.onAction) s.onAction("pan_zoom", Fmt3(s.view.panX, s.view.panY, s.view.zoom));
        e.handled = true;
    });

    dispatcher_.AddHandler(sg.canvas, UiEventType::PointerDown, [this](UiInputEvent& e) {
        if (!shaderGraph_) return;
        SetActivePanel(EditorPanelId::ShaderGraph);
        auto& s = *shaderGraph_;
        if (document_.Visible(s.searchPopup))
        {
            document_.SetVisible(s.searchPopup, false);
            s.pendingWireDir.clear();
            s.pendingWireNode = 0;
            s.pendingWirePort.clear();
            dispatcher_.ClearFocus();
        }
        if (!s.view.loaded) { e.handled = true; return; }

        const UiRect cr = layout_.Rect(s.canvas);
        const float z = std::clamp(s.view.zoom, 0.3f, 2.2f);

        // Middle mouse or RMB drag pans (RMB click without drag opens search menu; Alt+LMB on port/wire disconnects!)
        if (e.button == 1 || e.button == 2)
        {
            s.panning = true;
            s.panStartMouseX = e.x;
            s.panStartMouseY = e.y;
            s.panStartX = s.view.panX;
            s.panStartY = s.view.panY;
            dispatcher_.SetPointerCapture(s.canvas);
            e.handled = true;
            return;
        }

        if (e.button != 0) return;

        // 1. Check port circles & port row pills first (closest port on topmost node)
        PortHitCandidate hitPort;
        if (FindClosestPortAt(s.view, cr, z, e.x, e.y, hitPort))
        {
            // Alt + 左键点击任意已连接端口：一键断开该端口上的连线（ASE 经典快捷键）
            if (e.alt)
            {
                if (hitPort.connected && s.onAction)
                {
                    if (!hitPort.isOutput)
                    {
                        s.onAction("disconnect_input", std::to_string(hitPort.nodeId) + "|" + hitPort.portId);
                    }
                    else
                    {
                        std::vector<std::string> targets;
                        for (const auto& lnk : s.view.links)
                        {
                            if (lnk.fromNode == hitPort.nodeId && lnk.fromPort == hitPort.portId)
                                targets.push_back(std::to_string(lnk.toNode) + "|" + lnk.toPort);
                        }
                        for (const auto& t : targets) s.onAction("disconnect_input", t);
                    }
                }
                e.handled = true;
                return;
            }

            if (hitPort.isOutput)
            {
                s.draggingWire = true;
                s.wireReplugging = false;
                s.wireFromOutput = true;
                s.wireNodeId = hitPort.nodeId;
                s.wirePortId = hitPort.portId;
                s.wireValueType = hitPort.valueType;
                s.wireStartX = s.wireCurX = e.x;
                s.wireStartY = s.wireCurY = e.y;
                UpdateMagneticWireSnap(s, cr, z);
                dispatcher_.SetPointerCapture(s.canvas);
                e.handled = true;
                return;
            }

            // 点击输入端口：若已连线，像拔出真实插头一样断开该输入，并将线头锚定在原上游输出端口上继续拖移改插或松手丢弃！
            if (hitPort.connected)
            {
                std::uint32_t upstreamNode = 0;
                std::string upstreamPort;
                std::uint8_t upstreamType = hitPort.valueType;
                for (const auto& lnk : s.view.links)
                {
                    if (lnk.toNode == hitPort.nodeId && lnk.toPort == hitPort.portId)
                    {
                        upstreamNode = lnk.fromNode;
                        upstreamPort = lnk.fromPort;
                        upstreamType = lnk.valueType;
                        break;
                    }
                }
                if (s.onAction)
                {
                    s.onAction("disconnect_input", std::to_string(hitPort.nodeId) + "|" + hitPort.portId);
                }
                if (upstreamNode != 0 && !upstreamPort.empty())
                {
                    s.draggingWire = true;
                    s.wireReplugging = true;
                    s.wireFromOutput = true;
                    s.wireNodeId = upstreamNode;
                    s.wirePortId = upstreamPort;
                    s.wireValueType = upstreamType;
                    s.wireStartX = s.wireCurX = e.x;
                    s.wireStartY = s.wireCurY = e.y;
                    UpdateMagneticWireSnap(s, cr, z);
                    dispatcher_.SetPointerCapture(s.canvas);
                    e.handled = true;
                    return;
                }
            }

            // 点击未连接的输入端口：从该输入口反向拉出连线寻找输出端
            s.draggingWire = true;
            s.wireReplugging = false;
            s.wireFromOutput = false;
            s.wireNodeId = hitPort.nodeId;
            s.wirePortId = hitPort.portId;
            s.wireValueType = hitPort.valueType;
            s.wireStartX = s.wireCurX = e.x;
            s.wireStartY = s.wireCurY = e.y;
            UpdateMagneticWireSnap(s, cr, z);
            dispatcher_.SetPointerCapture(s.canvas);
            e.handled = true;
            return;
        }

        // Alt + 左键空白处平移画布（保留之前习惯），但在命中连线时先允许一键断线！
        const std::size_t hitWire = FindClosestWireAt(s.view, cr, z, e.x, e.y, 8.5f);
        if (e.alt && hitWire != static_cast<std::size_t>(-1))
        {
            const std::string target = std::to_string(s.view.links[hitWire].toNode) + "|" + s.view.links[hitWire].toPort;
            if (s.onAction) s.onAction("disconnect_input", target);
            s.hoveredLink = static_cast<std::size_t>(-1);
            e.handled = true;
            return;
        }
        if (e.alt)
        {
            s.panning = true;
            s.panStartMouseX = e.x;
            s.panStartMouseY = e.y;
            s.panStartX = s.view.panX;
            s.panStartY = s.view.panY;
            dispatcher_.SetPointerCapture(s.canvas);
            e.handled = true;
            return;
        }

        // 2. Check node cards (front-to-back)
        for (auto it = s.view.nodes.rbegin(); it != s.view.nodes.rend(); ++it)
        {
            const NodeLayoutMetrics m = ComputeNodeMetrics(*it, cr, s.view.panX, s.view.panY, z);
            if (e.x >= m.card.x && e.x <= m.card.x + m.card.width &&
                e.y >= m.card.y && e.y <= m.card.y + m.card.height)
            {
                const std::uint32_t hitNodeId = it->id;
                const bool alreadySelected = it->selected;
                if (!alreadySelected)
                {
                    std::string selPayload;
                    if (e.shift || e.control)
                    {
                        for (const auto& existing : s.view.nodes)
                        {
                            if (existing.selected)
                            {
                                if (!selPayload.empty()) selPayload.push_back(',');
                                selPayload += std::to_string(existing.id);
                            }
                        }
                    }
                    if (!selPayload.empty()) selPayload.push_back(',');
                    selPayload += std::to_string(hitNodeId);
                    if (s.onAction) s.onAction("select", selPayload);
                }
                s.draggingNodes = true;
                s.nodesMoved = false;
                s.dragLastMouseX = e.x;
                s.dragLastMouseY = e.y;
                if (s.onAction) s.onAction("move_begin", {});
                dispatcher_.SetPointerCapture(s.canvas);
                e.handled = true;
                return;
            }
        }

        // 3. Check Bezier connection wires
        if (hitWire != static_cast<std::size_t>(-1))
        {
            if (s.onAction) s.onAction("select", "link:" + std::to_string(hitWire));
            e.handled = true;
            return;
        }

        // 4. Empty space: start box marquee selection
        if (!e.shift && !e.control && s.onAction) s.onAction("select", {});
        s.marqueeActive = true;
        s.marqueeStartX = s.marqueeCurX = e.x;
        s.marqueeStartY = s.marqueeCurY = e.y;
        dispatcher_.SetPointerCapture(s.canvas);
        e.handled = true;
    });

    dispatcher_.AddHandler(sg.canvas, UiEventType::PointerMove, [this](UiInputEvent& e) {
        if (!shaderGraph_) return;
        auto& s = *shaderGraph_;
        const UiRect cr = layout_.Rect(s.canvas);
        const float z = std::clamp(s.view.zoom, 0.3f, 2.2f);
        if (s.panning)
        {
            s.view.panX = s.panStartX + (e.x - s.panStartMouseX);
            s.view.panY = s.panStartY + (e.y - s.panStartMouseY);
            e.handled = true;
            return;
        }
        if (s.draggingNodes)
        {
            const float dx = (e.x - s.dragLastMouseX) / z;
            const float dy = (e.y - s.dragLastMouseY) / z;
            s.dragLastMouseX = e.x;
            s.dragLastMouseY = e.y;
            if (std::abs(dx) > 0.01f || std::abs(dy) > 0.01f)
            {
                s.nodesMoved = true;
                if (s.onAction) s.onAction("move_drag", Fmt2(dx, dy));
            }
            e.handled = true;
            return;
        }
        if (s.draggingWire)
        {
            s.wireCurX = e.x;
            s.wireCurY = e.y;
            // 拖线贴边自动平滑滚屏，方便在小面板内连接屏幕外的节点
            constexpr float kEdgeMargin = 26.0f;
            constexpr float kEdgePanStep = 9.0f;
            bool edgePanned = false;
            if (e.x >= cr.x && e.x < cr.x + kEdgeMargin) { s.view.panX += kEdgePanStep; edgePanned = true; }
            else if (e.x <= cr.x + cr.width && e.x > cr.x + cr.width - kEdgeMargin) { s.view.panX -= kEdgePanStep; edgePanned = true; }
            if (e.y >= cr.y && e.y < cr.y + kEdgeMargin) { s.view.panY += kEdgePanStep; edgePanned = true; }
            else if (e.y <= cr.y + cr.height && e.y > cr.y + cr.height - kEdgeMargin) { s.view.panY -= kEdgePanStep; edgePanned = true; }
            if (edgePanned && s.onAction) s.onAction("pan_zoom", Fmt3(s.view.panX, s.view.panY, s.view.zoom));
            UpdateMagneticWireSnap(s, cr, z);
            e.handled = true;
            return;
        }
        if (s.marqueeActive)
        {
            s.marqueeCurX = e.x;
            s.marqueeCurY = e.y;
            e.handled = true;
            return;
        }

        // 空闲悬停检测：预亮端口外环与连线高亮，提升下刀确定感
        s.hoveredNodeId = 0;
        s.hoveredPortId.clear();
        s.hoveredLink = static_cast<std::size_t>(-1);
        if (s.view.loaded && e.x >= cr.x && e.x <= cr.x + cr.width && e.y >= cr.y && e.y <= cr.y + cr.height)
        {
            PortHitCandidate hp;
            if (FindClosestPortAt(s.view, cr, z, e.x, e.y, hp))
            {
                s.hoveredNodeId = hp.nodeId;
                s.hoveredPortId = std::move(hp.portId);
                s.hoveredPortOutput = hp.isOutput;
            }
            else
            {
                s.hoveredLink = FindClosestWireAt(s.view, cr, z, e.x, e.y, 8.5f);
            }
        }
    });

    dispatcher_.AddHandler(sg.canvas, UiEventType::PointerUp, [this, openSearchMenuAt](UiInputEvent& e) {
        if (!shaderGraph_) return;
        auto& s = *shaderGraph_;
        const UiRect cr = layout_.Rect(s.canvas);
        const float z = std::clamp(s.view.zoom, 0.3f, 2.2f);

        if (s.panning)
        {
            const float moved = std::hypot(e.x - s.panStartMouseX, e.y - s.panStartMouseY);
            s.panning = false;
            dispatcher_.ReleasePointerCapture();
            if (e.button == 2 && moved < 5.0f && s.view.loaded)
            {
                // Right-click on wire deletes it immediately; right-click on empty canvas opens fuzzy search menu!
                const std::size_t hitWire = FindClosestWireAt(s.view, cr, z, e.x, e.y, 8.5f);
                if (hitWire != static_cast<std::size_t>(-1))
                {
                    const std::string target = std::to_string(s.view.links[hitWire].toNode) + "|" + s.view.links[hitWire].toPort;
                    if (s.onAction) s.onAction("disconnect_input", target);
                    s.hoveredLink = static_cast<std::size_t>(-1);
                    e.handled = true;
                    return;
                }
                s.spawnGraphX = (e.x - cr.x - s.view.panX) / z;
                s.spawnGraphY = (e.y - cr.y - s.view.panY) / z;
                s.pendingWireDir.clear();
                s.pendingWireNode = 0;
                s.pendingWirePort.clear();
                const UiRect slotRect = layout_.Rect(Slot(EditorPanelId::ShaderGraph).content);
                openSearchMenuAt(e.x - slotRect.x, e.y - slotRect.y);
            }
            else if (s.onAction)
            {
                s.onAction("pan_zoom", Fmt3(s.view.panX, s.view.panY, s.view.zoom));
            }
            e.handled = true;
            return;
        }
        if (s.draggingNodes)
        {
            s.draggingNodes = false;
            dispatcher_.ReleasePointerCapture();
            if (s.onAction) s.onAction(s.nodesMoved ? "move_commit" : "move_cancel", {});
            s.nodesMoved = false;
            e.handled = true;
            return;
        }
        if (s.draggingWire)
        {
            s.wireCurX = e.x;
            s.wireCurY = e.y;
            UpdateMagneticWireSnap(s, cr, z);

            const bool wasReplugging = s.wireReplugging;
            const bool fromOutput = s.wireFromOutput;
            const std::uint32_t srcNodeId = s.wireNodeId;
            const std::string srcPortId = s.wirePortId;
            const std::uint32_t snappedNodeId = s.snapNodeId;
            const std::string snappedPortId = s.snapPortId;
            const float dragDist = std::hypot(e.x - s.wireStartX, e.y - s.wireStartY);

            s.draggingWire = false;
            s.wireReplugging = false;
            s.snapNodeId = 0;
            s.snapPortId.clear();
            dispatcher_.ReleasePointerCapture();

            if (snappedNodeId != 0 && !snappedPortId.empty())
            {
                const std::string connectPayload = fromOutput
                    ? (std::to_string(srcNodeId) + "|" + srcPortId + "|" + std::to_string(snappedNodeId) + "|" + snappedPortId)
                    : (std::to_string(snappedNodeId) + "|" + snappedPortId + "|" + std::to_string(srcNodeId) + "|" + srcPortId);
                if (s.onAction) s.onAction("connect", connectPayload);
                e.handled = true;
                return;
            }

            // 仅当主动从端口新拉出连线（非拔掉旧插头丢弃）且拖拽距离 > 14px 松在空白处时，才自动呼出节点搜索菜单并自动接线！
            if (!wasReplugging && dragDist > 14.0f &&
                e.x >= cr.x && e.x <= cr.x + cr.width && e.y >= cr.y && e.y <= cr.y + cr.height)
            {
                s.spawnGraphX = (e.x - cr.x - s.view.panX) / z;
                s.spawnGraphY = (e.y - cr.y - s.view.panY) / z;
                s.pendingWireDir = fromOutput ? "from_out" : "from_in";
                s.pendingWireNode = srcNodeId;
                s.pendingWirePort = srcPortId;
                const UiRect slotRect = layout_.Rect(Slot(EditorPanelId::ShaderGraph).content);
                openSearchMenuAt(e.x - slotRect.x, e.y - slotRect.y);
            }
            e.handled = true;
            return;
        }
        if (s.marqueeActive)
        {
            s.marqueeActive = false;
            dispatcher_.ReleasePointerCapture();
            const float rx0 = std::min(s.marqueeStartX, e.x), rx1 = std::max(s.marqueeStartX, e.x);
            const float ry0 = std::min(s.marqueeStartY, e.y), ry1 = std::max(s.marqueeStartY, e.y);
            if (rx1 - rx0 > 4.0f || ry1 - ry0 > 4.0f)
            {
                std::string sel;
                for (const auto& n : s.view.nodes)
                {
                    const NodeLayoutMetrics m = ComputeNodeMetrics(n, cr, s.view.panX, s.view.panY, z);
                    const bool overlap = m.card.x + m.card.width >= rx0 && m.card.x <= rx1 &&
                                         m.card.y + m.card.height >= ry0 && m.card.y <= ry1;
                    if (overlap)
                    {
                        if (!sel.empty()) sel.push_back(',');
                        sel += std::to_string(n.id);
                    }
                }
                if (s.onAction) s.onAction("select", sel);
            }
            e.handled = true;
        }
    });

    // Spacebar / F shortcut when ShaderGraph is the active panel
    if (!sg.rootShortcutsBound)
    {
        sg.rootShortcutsBound = true;
        dispatcher_.AddHandler(document_.Root(), UiEventType::KeyDown, [this](UiInputEvent& e) {
            if (RouteShaderGraphKey(e, TextFieldFocused()))
            {
                e.handled = true;
                e.StopPropagation();
            }
        });
    }

    RefreshShaderGraphChrome();
}

bool EditorShell::RouteShaderGraphKey(const UiInputEvent& event, bool textFieldFocused)
{
    if (event.type != UiEventType::KeyDown || !shaderGraph_) return false;
    auto& s = *shaderGraph_;
    const PanelSlot& slot = Slot(EditorPanelId::ShaderGraph);
    if (!slot.panel.IsValid() || !document_.Visible(slot.panel)) return false;

    if (event.key == UiKeyEscape && s.searchPopup.IsValid() && document_.Visible(s.searchPopup))
    {
        document_.SetVisible(s.searchPopup, false);
        s.pendingWireDir.clear();
        s.pendingWireNode = 0;
        s.pendingWirePort.clear();
        if (dispatcher_.Focus() == s.searchBox) dispatcher_.ClearFocus();
        return true;
    }
    if (activePanel_ != EditorPanelId::ShaderGraph || textFieldFocused || !s.view.loaded) return false;

    if (event.key == UiKeyEscape)
    {
        bool cancelled = false;
        if (s.draggingWire || s.panning || s.marqueeActive)
        {
            s.draggingWire = false;
            s.wireReplugging = false;
            s.snapNodeId = 0;
            s.snapPortId.clear();
            s.panning = false;
            s.marqueeActive = false;
            dispatcher_.ReleasePointerCapture();
            cancelled = true;
        }
        if (s.draggingNodes)
        {
            s.draggingNodes = false;
            s.nodesMoved = false;
            dispatcher_.ReleasePointerCapture();
            if (s.onAction) s.onAction("move_cancel", {});
            cancelled = true;
        }
        return cancelled;
    }
    if ((event.key == 'F' || event.key == 'f') && !event.control && !event.alt && !event.shift)
    {
        if (s.onAction) s.onAction("frame_all", {});
        return true;
    }
    if (event.key == UiKeySpace && !event.control && !event.alt)
    {
        const UiRect cr = layout_.Rect(s.canvas);
        const float z = std::clamp(s.view.zoom, 0.3f, 2.2f);
        s.spawnGraphX = (cr.width * 0.42f - s.view.panX) / z;
        s.spawnGraphY = (cr.height * 0.38f - s.view.panY) / z;
        s.pendingWireDir.clear();
        s.pendingWireNode = 0;
        s.pendingWirePort.clear();
        s.searchEdit->SetText("");
        s.searchCategoryFilter.clear();
        s.searchHighlight = 0;
        s.searchListOffset = 0;
        const UiRect slotRect = layout_.Rect(slot.content);
        const float popupW = 328.0f;
        const float popupH = std::clamp(slotRect.height - 38.0f, 178.0f, 310.0f);
        const float localLeft = std::max(12.0f, cr.width * 0.5f - 164.0f);
        UiStyle sps = document_.Style(s.searchPopup);
        sps.width = UiLength::Px(popupW);
        sps.height = UiLength::Px(popupH);
        sps.left = UiLength::Px(std::clamp(localLeft, 6.0f, std::max(6.0f, slotRect.width - popupW - 6.0f)));
        sps.top = UiLength::Px(std::clamp(42.0f, 32.0f, std::max(32.0f, slotRect.height - popupH - 4.0f)));
        document_.SetStyle(s.searchPopup, sps);
        document_.SetVisible(s.searchPopup, true);
        RefreshShaderGraphChrome();
        LayoutShaderGraphPanel(slotRect.width, slotRect.height);
        dispatcher_.SetFocus(s.searchBox);
        return true;
    }
    if (event.key == UiKeyBackspace && !event.control && !event.alt)
    {
        return commands_.Execute({EditorCommandId::Delete, {}});
    }
    return false;
}

void EditorShell::SetShaderGraphPalette(std::vector<EditorShaderPaletteItem> items)
{
    if (!shaderGraph_) shaderGraph_ = std::make_unique<EditorShaderGraphRuntime>();
    shaderGraph_->palette = std::move(items);
    RefreshShaderGraphChrome();
}

void EditorShell::SetShaderGraphView(EditorShaderGraphViewState view)
{
    if (!shaderGraph_) shaderGraph_ = std::make_unique<EditorShaderGraphRuntime>();
    if (shaderGraph_->view == view) return;
    // Preserve live pan while actively dragging so controller echoes don't jitter
    if (shaderGraph_->panning)
    {
        view.panX = shaderGraph_->view.panX;
        view.panY = shaderGraph_->view.panY;
    }
    shaderGraph_->view = std::move(view);
    RefreshShaderGraphChrome();
    if (initialized_)
    {
        const UiRect r = layout_.Rect(Slot(EditorPanelId::ShaderGraph).content);
        if (r.width > 1.0f && r.height > 1.0f) LayoutShaderGraphPanel(r.width, r.height);
    }
}

void EditorShell::SetShaderGraphActionHandler(std::function<void(std::string_view, std::string_view)> handler)
{
    if (!shaderGraph_) shaderGraph_ = std::make_unique<EditorShaderGraphRuntime>();
    shaderGraph_->onAction = std::move(handler);
}

UiElementHandle EditorShell::ShaderGraphCanvas() const noexcept
{
    return shaderGraph_ ? shaderGraph_->canvas : UiElementHandle{};
}

void EditorShell::RefreshShaderGraphChrome()
{
    if (!shaderGraph_ || !shaderGraph_->toolbar.IsValid()) return;
    auto& sg = *shaderGraph_;
    const auto& v = sg.view;

    const std::string title = v.loaded
        ? (v.title + (v.dirty ? " ●" : ""))
        : Text("EditorUi.ShaderGraph", "Shader Graph");
    document_.SetText(sg.titleLabel, title);
    document_.SetText(sg.shadingBtn, Text("EditorUi.ShaderGraphShading", "Shading") + ": " + v.shadingMode);
    document_.SetText(sg.blendBtn, Text("EditorUi.ShaderGraphBlend", "Blend") + ": " + v.blendMode);
    document_.SetText(sg.doubleSidedBtn,
                      (v.doubleSided ? std::string("✓ ") : std::string("  ")) +
                          Text("EditorUi.ShaderGraphDoubleSided", "2-Sided"));
    document_.SetText(sg.addNodeBtn, Text("EditorUi.ShaderGraphAddNode", "+ Add Node"));
    document_.SetText(sg.frameBtn, Text("EditorUi.ShaderGraphFrameAll", "Frame"));
    document_.SetText(sg.saveBtn, Text("EditorUi.ShaderGraphSaveApply", "Save & Apply"));
    document_.SetText(sg.hlslBtn, Text("EditorUi.ShaderGraphViewHlsl", "View HLSL"));
    document_.SetText(sg.newMatBtn, Text("EditorUi.ShaderGraphNewMaterial", "New Material"));

    for (UiElementHandle b : {sg.shadingBtn, sg.blendBtn, sg.doubleSidedBtn, sg.addNodeBtn,
                              sg.frameBtn, sg.saveBtn, sg.hlslBtn, sg.newMatBtn})
    {
        document_.SetVisible(b, v.loaded);
    }
    document_.SetVisible(sg.previewCard, v.loaded && !v.previewImageKey.empty());
    if (!v.previewImageKey.empty())
    {
        UiStyle pis = document_.Style(sg.previewImage);
        if (pis.image != v.previewImageKey)
        {
            pis.image = v.previewImageKey;
            document_.SetStyle(sg.previewImage, pis);
        }
    }

    const bool showPropBar = v.loaded && v.primaryNodeId != 0 && (v.canExpose || v.primaryHasValue);
    document_.SetVisible(sg.propBar, showPropBar);
    if (showPropBar)
    {
        document_.SetVisible(sg.exposeBtn, v.canExpose);
        document_.SetVisible(sg.paramNameBox, v.canExpose && v.primaryExposed);
        document_.SetVisible(sg.valueBox, v.primaryHasValue);
        document_.SetText(sg.exposeBtn,
                          (v.primaryExposed ? std::string("✓ ") : std::string("  ")) +
                              Text("EditorUi.ShaderGraphExpose", "Expose"));
        if (sg.editingField != "param") document_.SetText(sg.paramNameBox, v.primaryParamName);
        if (sg.editingField != "value") document_.SetText(sg.valueBox, v.primaryValueText);
    }

    if (document_.Visible(sg.searchPopup))
    {
        const std::string q(sg.searchEdit->DisplayText());
        document_.SetText(sg.searchBox,
                          q.empty() ? Text("EditorUi.ShaderGraphSearchPlaceholder", "Search nodes...") : q);
        for (std::size_t ci = 0; ci < sg.searchCategoryButtons.size() && ci < 10; ++ci)
        {
            const bool activeCat = sg.searchCategoryFilter == kSearchCategoryTabs[ci].id;
            UiStyle cs2 = document_.Style(sg.searchCategoryButtons[ci]);
            cs2.background = activeCat
                ? (ci == 0 ? kSurfaceActive : CategoryHeaderColor(kSearchCategoryTabs[ci].id))
                : kSurface;
            cs2.borderWidth = activeCat ? 1.0f : 0.0f;
            cs2.borderColor = kAccent;
            document_.SetStyle(sg.searchCategoryButtons[ci], cs2);
        }

        sg.filteredPaletteIndices.clear();
        for (std::size_t i = 0; i < sg.palette.size(); ++i)
        {
            const auto& p = sg.palette[i];
            if (p.type == "MasterSurface") continue;
            if (sg.pendingWireDir == "from_out" && !p.hasInputs) continue;
            if (sg.pendingWireDir == "from_in" && !p.hasOutputs) continue;
            if (!sg.searchCategoryFilter.empty() && p.category != sg.searchCategoryFilter) continue;
            if (ContainsLower(p.title, q) || ContainsLower(p.type, q) || ContainsLower(p.category, q))
            {
                sg.filteredPaletteIndices.push_back(i);
            }
        }

        const float popupH = document_.Style(sg.searchPopup).height.value;
        const std::size_t visRows = static_cast<std::size_t>(std::clamp(static_cast<int>((popupH - 56.0f) / 25.0f), 3, 10));
        if (sg.filteredPaletteIndices.empty())
        {
            sg.searchListOffset = 0;
            sg.searchHighlight = 0;
        }
        else
        {
            const std::size_t maxOff = sg.filteredPaletteIndices.size() > visRows
                ? (sg.filteredPaletteIndices.size() - visRows) : 0;
            sg.searchListOffset = std::min(sg.searchListOffset, maxOff);
            sg.searchHighlight = std::min(sg.searchHighlight, sg.filteredPaletteIndices.size() - 1);
        }

        char countBuf[48]{};
        if (sg.filteredPaletteIndices.empty())
        {
            std::snprintf(countBuf, sizeof(countBuf), "0 / %zu", sg.palette.size());
        }
        else
        {
            const std::size_t endRow = std::min(sg.filteredPaletteIndices.size(), sg.searchListOffset + visRows);
            std::snprintf(countBuf, sizeof(countBuf), "%zu-%zu/%zu",
                          sg.searchListOffset + 1, endRow, sg.filteredPaletteIndices.size());
        }
        document_.SetText(sg.searchCountLabel, countBuf);

        for (std::size_t bi = 0; bi < sg.searchItemButtons.size(); ++bi)
        {
            const UiElementHandle btn = sg.searchItemButtons[bi];
            const std::size_t idx = sg.searchListOffset + bi;
            if (bi < visRows && idx < sg.filteredPaletteIndices.size())
            {
                const auto& item = sg.palette[sg.filteredPaletteIndices[idx]];
                UiStyle bs = document_.Style(btn);
                bs.background = (idx == sg.searchHighlight) ? kSurfaceActive : kSurface;
                bs.borderWidth = 1.0f;
                bs.borderColor = CategoryHeaderColor(item.category);
                document_.SetStyle(btn, bs);
                document_.SetText(btn, "[" + item.category + "] " + item.title);
                document_.SetVisible(btn, true);
            }
            else
            {
                document_.SetVisible(btn, false);
            }
        }
    }
}

void EditorShell::LayoutShaderGraphPanel(float width, float height)
{
    if (!shaderGraph_ || !shaderGraph_->canvas.IsValid() || width <= 1.0f || height <= 1.0f) return;
    auto& sg = *shaderGraph_;

    float x = 8.0f;
    float rowTop = 3.0f;
    {
        const std::string lbl(document_.Text(sg.titleLabel));
        const float maxTitleW = width < 680.0f ? 128.0f : 190.0f;
        const float w = std::clamp(MeasuredWidth(lbl, 12.5f) + 12.0f, 76.0f, maxTitleW);
        UiStyle s = document_.Style(sg.titleLabel);
        s.left = UiLength::Px(x);
        s.top = UiLength::Px(rowTop);
        s.width = UiLength::Px(w);
        document_.SetStyle(sg.titleLabel, s);
        x += w + 6.0f;
    }
    const auto placeBtn = [&](UiElementHandle h, float minW) {
        if (!h.IsValid() || !document_.Visible(h)) return;
        const std::string lbl(document_.Text(h));
        const float w = std::min(std::max(44.0f, width - 16.0f), std::max(minW, MeasuredWidth(lbl, 12.0f) + 14.0f));
        if (x > 12.0f && x + w > width - 6.0f && rowTop < 20.0f)
        {
            x = 8.0f;
            rowTop = 29.0f;
        }
        UiStyle s = document_.Style(h);
        s.left = UiLength::Px(x);
        s.top = UiLength::Px(rowTop);
        s.width = UiLength::Px(w);
        s.height = UiLength::Px(24.0f);
        document_.SetStyle(h, s);
        x += w + 4.0f;
    };
    placeBtn(sg.shadingBtn, 68.0f);
    placeBtn(sg.blendBtn, 78.0f);
    placeBtn(sg.doubleSidedBtn, 64.0f);
    placeBtn(sg.addNodeBtn, 92.0f);
    placeBtn(sg.frameBtn, 52.0f);
    placeBtn(sg.saveBtn, 78.0f);
    placeBtn(sg.hlslBtn, 82.0f);
    placeBtn(sg.newMatBtn, 80.0f);

    const float toolbarH = rowTop > 20.0f ? (kSgToolbarHeight + 26.0f) : kSgToolbarHeight;
    UiStyle tbs = document_.Style(sg.toolbar);
    tbs.width = UiLength::Px(width);
    tbs.height = UiLength::Px(toolbarH);
    document_.SetStyle(sg.toolbar, tbs);

    const bool propVis = document_.Visible(sg.propBar);
    const float canvasTop = toolbarH + (propVis ? kSgPropBarHeight : 0.0f);
    if (propVis)
    {
        UiStyle ps = document_.Style(sg.propBar);
        ps.top = UiLength::Px(toolbarH);
        ps.width = UiLength::Px(width);
        ps.height = UiLength::Px(kSgPropBarHeight);
        document_.SetStyle(sg.propBar, ps);

        float px = 8.0f;
        if (document_.Visible(sg.exposeBtn))
        {
            const float ew = std::max(108.0f, MeasuredWidth(std::string(document_.Text(sg.exposeBtn)), 12.0f) + 18.0f);
            UiStyle es = document_.Style(sg.exposeBtn);
            es.left = UiLength::Px(px);
            es.width = UiLength::Px(ew);
            document_.SetStyle(sg.exposeBtn, es);
            px += ew + 6.0f;
        }
        if (document_.Visible(sg.paramNameBox))
        {
            UiStyle ns = document_.Style(sg.paramNameBox);
            ns.left = UiLength::Px(px);
            ns.width = UiLength::Px(136.0f);
            document_.SetStyle(sg.paramNameBox, ns);
            px += 142.0f;
        }
        if (document_.Visible(sg.valueBox))
        {
            UiStyle vs = document_.Style(sg.valueBox);
            vs.left = UiLength::Px(px);
            vs.width = UiLength::Px(std::min(220.0f, std::max(96.0f, width - px - 12.0f)));
            document_.SetStyle(sg.valueBox, vs);
        }
    }

    UiStyle cs = document_.Style(sg.canvas);
    cs.top = UiLength::Px(canvasTop);
    cs.width = UiLength::Px(width);
    cs.height = UiLength::Px(std::max(1.0f, height - canvasTop));
    document_.SetStyle(sg.canvas, cs);

    if (document_.Visible(sg.previewCard))
    {
        UiStyle pcs = document_.Style(sg.previewCard);
        pcs.left = UiLength::Px(std::max(8.0f, width - kSgPreviewCardW - 12.0f));
        pcs.top = UiLength::Px(std::max(canvasTop + 8.0f, height - kSgPreviewCardH - 12.0f));
        pcs.width = UiLength::Px(kSgPreviewCardW);
        pcs.height = UiLength::Px(kSgPreviewCardH);
        document_.SetStyle(sg.previewCard, pcs);
    }
}

void EditorShell::PaintShaderGraphCanvas(UiPaintSink& sink, UiElementHandle surfaceRoot) const
{
    if (!shaderGraph_ || !shaderGraph_->canvas.IsValid() || !surfaceRoot.IsValid()) return;
    const PanelSlot& slot = Slot(EditorPanelId::ShaderGraph);
    if (!slot.panel.IsValid() || !document_.Visible(slot.panel)) return;
    if (!document_.IsAncestor(surfaceRoot, shaderGraph_->canvas)) return;

    const auto& sg = *shaderGraph_;
    const auto& v = sg.view;
    const UiRect cr = layout_.Rect(sg.canvas);
    if (cr.width <= 4.0f || cr.height <= 4.0f) return;

    sink.PushClip(cr);

    // 1. ASE Blueprint Grid
    const float z = std::clamp(v.zoom, 0.3f, 2.2f);
    const float minorStep = 24.0f * z;
    if (minorStep >= 8.0f)
    {
        const UiColor minorCol{0.105f, 0.12f, 0.148f, 1.0f};
        const UiColor majorCol{0.155f, 0.178f, 0.22f, 1.0f};
        const float majorStep = minorStep * 5.0f;

        const auto modPos = [](float val, float step) {
            float r = std::fmod(val, step);
            if (r < 0.0f) r += step;
            return r;
        };
        for (float gx = cr.x + modPos(v.panX, minorStep); gx < cr.x + cr.width; gx += minorStep)
            sink.Line(gx, cr.y, gx, cr.y + cr.height, 1.0f, minorCol);
        for (float gy = cr.y + modPos(v.panY, minorStep); gy < cr.y + cr.height; gy += minorStep)
            sink.Line(cr.x, gy, cr.x + cr.width, gy, 1.0f, minorCol);

        for (float gx = cr.x + modPos(v.panX, majorStep); gx < cr.x + cr.width; gx += majorStep)
            sink.Line(gx, cr.y, gx, cr.y + cr.height, 1.2f, majorCol);
        for (float gy = cr.y + modPos(v.panY, majorStep); gy < cr.y + cr.height; gy += majorStep)
            sink.Line(cr.x, gy, cr.x + cr.width, gy, 1.2f, majorCol);
    }

    if (!v.loaded)
    {
        const std::string emptyMsg = Text("EditorUi.ShaderGraphEmpty",
                                          "Create or double-click a .fshader.json asset in Assets to edit");
        const float tw = MeasuredWidth(emptyMsg, 13.5f);
        sink.Text({cr.x + std::max(16.0f, (cr.width - tw) * 0.5f), cr.y + cr.height * 0.46f, tw + 24.0f, 26.0f},
                  emptyMsg, 13.5f, kMuted);
        sink.PopClip();
        return;
    }

    // 2. Smooth Anti-Aliased Cubic Bezier Links
    const float wireScale = std::clamp(z, 0.78f, 1.25f);
    for (std::size_t li = 0; li < v.links.size(); ++li)
    {
        const auto& lnk = v.links[li];
        const EditorShaderNodeView* fn = nullptr;
        const EditorShaderNodeView* tn = nullptr;
        for (const auto& n : v.nodes)
        {
            if (n.id == lnk.fromNode) fn = &n;
            if (n.id == lnk.toNode) tn = &n;
        }
        if (!fn || !tn) continue;
        std::size_t oi = 0, ii = 0;
        bool foundO = false, foundI = false;
        for (std::size_t k = 0; k < fn->outputs.size(); ++k) if (fn->outputs[k].id == lnk.fromPort) { oi = k; foundO = true; break; }
        for (std::size_t k = 0; k < tn->inputs.size(); ++k) if (tn->inputs[k].id == lnk.toPort) { ii = k; foundI = true; break; }
        if (!foundO || !foundI) continue;

        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        PortCenter(ComputeNodeMetrics(*fn, cr, v.panX, v.panY, z), true, oi, z, x0, y0);
        PortCenter(ComputeNodeMetrics(*tn, cr, v.panX, v.panY, z), false, ii, z, x1, y1);
        float c0x = 0, c0y = 0, c1x = 0, c1y = 0;
        ComputeConnectedBezierControls(x0, y0, x1, y1, z, c0x, c0y, c1x, c1y);

        const bool hovered = (!sg.draggingWire && sg.hoveredLink == li);
        const UiColor baseCol = PortTypeColor(lnk.valueType);
        const UiColor wireCol = lnk.selected
            ? UiColor{1.0f, 0.86f, 0.30f, 1.0f}
            : (hovered ? UiColor{std::min(1.0f, baseCol.r + 0.18f), std::min(1.0f, baseCol.g + 0.18f),
                                 std::min(1.0f, baseCol.b + 0.18f), 1.0f}
                       : baseCol);
        const float coreW = (lnk.selected ? 3.5f : (hovered ? 3.2f : 2.6f)) * wireScale;

        if (lnk.selected || hovered)
        {
            DrawSmoothBezier(sink, x0, y0, c0x, c0y, c1x, c1y, x1, y1, coreW + 4.4f * wireScale,
                             UiColor{wireCol.r, wireCol.g, wireCol.b, 0.24f});
        }
        DrawSmoothBezier(sink, x0, y0, c0x, c0y, c1x, c1y, x1, y1, coreW + 2.0f * wireScale,
                         UiColor{0.02f, 0.03f, 0.05f, 0.72f});
        DrawSmoothBezier(sink, x0, y0, c0x, c0y, c1x, c1y, x1, y1, coreW, wireCol);
    }

    // Live dragging wire (Drawn OVER node cards below so preview plug is never hidden behind a card!)
    bool hasLiveWire = false;
    float lwX0 = 0, lwY0 = 0, lwC0X = 0, lwC0Y = 0, lwC1X = 0, lwC1Y = 0, lwX1 = 0, lwY1 = 0;
    if (sg.draggingWire)
    {
        for (const auto& n : v.nodes)
        {
            if (n.id != sg.wireNodeId) continue;
            const NodeLayoutMetrics m = ComputeNodeMetrics(n, cr, v.panX, v.panY, z);
            float px = 0, py = 0;
            if (sg.wireFromOutput)
            {
                for (std::size_t k = 0; k < n.outputs.size(); ++k)
                    if (n.outputs[k].id == sg.wirePortId) { PortCenter(m, true, k, z, px, py); hasLiveWire = true; break; }
            }
            else
            {
                for (std::size_t k = 0; k < n.inputs.size(); ++k)
                    if (n.inputs[k].id == sg.wirePortId) { PortCenter(m, false, k, z, px, py); hasLiveWire = true; break; }
            }
            if (hasLiveWire)
            {
                const bool snapped = sg.snapNodeId != 0;
                const float targetX = snapped ? sg.snapPortX : sg.wireCurX;
                const float targetY = snapped ? sg.snapPortY : sg.wireCurY;
                ComputeLiveWireBezierControls(px, py, sg.wireFromOutput, targetX, targetY, snapped, z,
                                              lwX0, lwY0, lwC0X, lwC0Y, lwC1X, lwC1Y, lwX1, lwY1);
            }
            break;
        }
    }

    // 3. Node Cards
    const float fontTitle = std::clamp(12.0f * z, 8.5f, 16.0f);
    const float fontPort = std::clamp(11.0f * z, 8.0f, 14.5f);
    const float portR = std::clamp(4.8f * z, 3.3f, 7.2f);

    for (const auto& n : v.nodes)
    {
        const NodeLayoutMetrics m = ComputeNodeMetrics(n, cr, v.panX, v.panY, z);
        if (m.card.x + m.card.width < cr.x || m.card.x > cr.x + cr.width ||
            m.card.y + m.card.height < cr.y || m.card.y > cr.y + cr.height)
        {
            continue;
        }

        // Shadow & Card body
        sink.Rect({m.card.x + 3.0f, m.card.y + 4.0f, m.card.width, m.card.height}, UiColor{0.0f, 0.0f, 0.0f, 0.42f});
        sink.Rect(m.card, UiColor{0.098f, 0.112f, 0.138f, 0.96f});
        sink.Rect({m.card.x, m.card.y, m.card.width, m.headerH}, CategoryHeaderColor(n.category));

        // Title + Exposed badge
        sink.Text({m.card.x + 8.0f * z, m.card.y + 3.0f * z, m.card.width - 14.0f * z, m.headerH - 4.0f * z},
                  n.title, fontTitle, UiColor{0.96f, 0.97f, 0.99f, 1.0f});
        if (n.exposed)
        {
            const float bw = 18.0f * z;
            sink.Rect({m.card.x + m.card.width - bw - 5.0f * z, m.card.y + 4.0f * z, bw, m.headerH - 8.0f * z},
                      UiColor{0.18f, 0.78f, 0.45f, 0.9f});
            sink.Text({m.card.x + m.card.width - bw - 2.0f * z, m.card.y + 3.0f * z, bw, m.headerH - 6.0f * z},
                      "P", fontPort, UiColor{0.05f, 0.1f, 0.06f, 1.0f});
        }

        // Subtitle or Color Swatch
        if (m.subH > 0.0f)
        {
            const float sy = m.card.y + m.headerH;
            sink.Rect({m.card.x + 1.0f, sy, m.card.width - 2.0f, m.subH}, UiColor{0.072f, 0.082f, 0.102f, 0.95f});
            if (n.swatchRgba[3] > 0.0f)
            {
                const float sw = 28.0f * z;
                sink.Rect({m.card.x + 8.0f * z, sy + 3.0f * z, sw, m.subH - 6.0f * z},
                          UiColor{n.swatchRgba[0], n.swatchRgba[1], n.swatchRgba[2], 1.0f});
                sink.Text({m.card.x + 12.0f * z + sw, sy + 2.0f * z, m.card.width - sw - 18.0f * z, m.subH - 2.0f * z},
                          n.subtitle, fontPort, kMuted);
            }
            else
            {
                sink.Text({m.card.x + 8.0f * z, sy + 2.0f * z, m.card.width - 16.0f * z, m.subH - 2.0f * z},
                          n.subtitle, fontPort, UiColor{0.72f, 0.82f, 0.94f, 1.0f});
            }
        }

        // Input ports
        for (std::size_t ii = 0; ii < n.inputs.size(); ++ii)
        {
            float px = 0, py = 0;
            PortCenter(m, false, ii, z, px, py);
            const bool isSnapTarget = sg.draggingWire && sg.snapNodeId == n.id && !sg.snapIsOutput && sg.snapPortId == n.inputs[ii].id;
            const bool isHover = !sg.draggingWire && sg.hoveredNodeId == n.id && !sg.hoveredPortOutput && sg.hoveredPortId == n.inputs[ii].id;
            const bool compatibleWhileDragging = !sg.draggingWire || (sg.wireFromOutput && n.id != sg.wireNodeId) ||
                                                 (!sg.wireFromOutput && n.id == sg.wireNodeId && sg.wirePortId == n.inputs[ii].id);
            const float alphaMul = compatibleWhileDragging ? 1.0f : 0.32f;
            UiColor pcol = PortTypeColor(n.inputs[ii].valueType);
            pcol.a *= alphaMul;
            const float r = portR + (isSnapTarget ? 1.8f : (isHover ? 1.3f : 0.0f));
            if (isSnapTarget || isHover)
            {
                sink.CanvasCircle(px, py, r + 4.0f, UiColor{pcol.r, pcol.g, pcol.b, isSnapTarget ? 0.34f : 0.22f});
            }
            sink.CanvasCircle(px, py, r + 1.4f, UiColor{0.03f, 0.04f, 0.06f, 0.95f * alphaMul});
            sink.CanvasCircle(px, py, r, pcol);
            if (!n.inputs[ii].connected && !isSnapTarget)
                sink.CanvasCircle(px, py, std::max(1.4f, r - 1.9f), UiColor{0.098f, 0.112f, 0.138f, 1.0f});
            UiColor lblCol = (isSnapTarget || isHover) ? UiColor{1.0f, 1.0f, 1.0f, 1.0f} : kText;
            lblCol.a *= alphaMul;
            sink.Text({px + portR + 5.0f * z, py - m.rowH * 0.45f, m.card.width * 0.56f, m.rowH},
                      n.inputs[ii].label, fontPort, lblCol);
        }

        // Output ports
        for (std::size_t oi = 0; oi < n.outputs.size(); ++oi)
        {
            float px = 0, py = 0;
            PortCenter(m, true, oi, z, px, py);
            const bool isSnapTarget = sg.draggingWire && sg.snapNodeId == n.id && sg.snapIsOutput && sg.snapPortId == n.outputs[oi].id;
            const bool isHover = !sg.draggingWire && sg.hoveredNodeId == n.id && sg.hoveredPortOutput && sg.hoveredPortId == n.outputs[oi].id;
            const bool compatibleWhileDragging = !sg.draggingWire || (!sg.wireFromOutput && n.id != sg.wireNodeId) ||
                                                 (sg.wireFromOutput && n.id == sg.wireNodeId && sg.wirePortId == n.outputs[oi].id);
            const float alphaMul = compatibleWhileDragging ? 1.0f : 0.32f;
            UiColor pcol = PortTypeColor(n.outputs[oi].valueType);
            pcol.a *= alphaMul;
            const float r = portR + (isSnapTarget ? 1.8f : (isHover ? 1.3f : 0.0f));
            if (isSnapTarget || isHover)
            {
                sink.CanvasCircle(px, py, r + 4.0f, UiColor{pcol.r, pcol.g, pcol.b, isSnapTarget ? 0.34f : 0.22f});
            }
            sink.CanvasCircle(px, py, r + 1.4f, UiColor{0.03f, 0.04f, 0.06f, 0.95f * alphaMul});
            sink.CanvasCircle(px, py, r, pcol);
            if (!n.outputs[oi].connected && !isSnapTarget)
                sink.CanvasCircle(px, py, std::max(1.4f, r - 1.9f), UiColor{0.098f, 0.112f, 0.138f, 1.0f});
            UiColor lblCol = (isSnapTarget || isHover) ? UiColor{1.0f, 1.0f, 1.0f, 1.0f} : kText;
            lblCol.a *= alphaMul;
            const float lw = MeasuredWidth(n.outputs[oi].label, fontPort);
            sink.Text({px - portR - 5.0f * z - lw, py - m.rowH * 0.45f, lw + 6.0f, m.rowH},
                      n.outputs[oi].label, fontPort, lblCol);
        }

        sink.Border(m.card, n.selected ? 2.0f : 1.0f,
                    n.selected ? UiColor{0.32f, 0.68f, 1.0f, 1.0f} : UiColor{0.20f, 0.24f, 0.30f, 1.0f});
    }

    // Draw live dragged wire & tip plug ON TOP of node cards so you always see the wire tip clearly!
    if (hasLiveWire)
    {
        const UiColor liveCol = PortTypeColor(sg.wireValueType);
        const float liveW = 3.0f * wireScale;
        DrawSmoothBezier(sink, lwX0, lwY0, lwC0X, lwC0Y, lwC1X, lwC1Y, lwX1, lwY1, liveW + 3.6f * wireScale,
                         UiColor{liveCol.r, liveCol.g, liveCol.b, 0.24f});
        DrawSmoothBezier(sink, lwX0, lwY0, lwC0X, lwC0Y, lwC1X, lwC1Y, lwX1, lwY1, liveW + 1.8f * wireScale,
                         UiColor{0.02f, 0.03f, 0.05f, 0.72f});
        DrawSmoothBezier(sink, lwX0, lwY0, lwC0X, lwC0Y, lwC1X, lwC1Y, lwX1, lwY1, liveW, liveCol);
        const float tipX = sg.wireFromOutput ? lwX1 : lwX0;
        const float tipY = sg.wireFromOutput ? lwY1 : lwY0;
        sink.CanvasCircle(tipX, tipY, sg.snapNodeId != 0 ? (portR + 1.5f) : 4.2f, liveCol);
    }

    // 4. Box selection marquee
    if (sg.marqueeActive)
    {
        const UiRect mr{
            std::min(sg.marqueeStartX, sg.marqueeCurX),
            std::min(sg.marqueeStartY, sg.marqueeCurY),
            std::abs(sg.marqueeCurX - sg.marqueeStartX),
            std::abs(sg.marqueeCurY - sg.marqueeStartY),
        };
        sink.Rect(mr, UiColor{0.25f, 0.56f, 0.96f, 0.16f});
        sink.Border(mr, 1.0f, UiColor{0.35f, 0.68f, 1.0f, 0.85f});
    }

    // 5. Bottom-left zoom & status HUD
    char hud[256]{};
    std::snprintf(hud, sizeof(hud), "%d%%  |  Space / RMB: +Node  |  Del: Delete  |  Ctrl+D: Duplicate%s%s",
                  static_cast<int>(std::lround(z * 100.0f)),
                  v.statusText.empty() ? "" : "  |  ",
                  v.statusText.c_str());
    sink.Rect({cr.x + 8.0f, cr.y + cr.height - 26.0f, std::min(cr.width - 16.0f, MeasuredWidth(hud, 11.0f) + 16.0f), 20.0f},
              UiColor{0.05f, 0.06f, 0.08f, 0.78f});
    sink.Text({cr.x + 15.0f, cr.y + cr.height - 24.0f, cr.width - 30.0f, 18.0f}, hud, 11.0f, kMuted);

    sink.PopClip();
}

} // namespace flare::ui::editor
