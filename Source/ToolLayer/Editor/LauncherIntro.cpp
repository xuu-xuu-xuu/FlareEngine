#include "LauncherIntro.h"
#include "FlarePalette.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace se
{
namespace
{
using namespace Gdiplus;
constexpr float FadeStartMs = 640.0f;
constexpr float DurationMs = 900.0f;
float Ease(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 笔尖始终贴着外圆走，笔压决定向圆心方向留下的笔迹宽度。
void DrawRing(Graphics& g, SolidBrush& brush, float progress)
{
    if (progress <= 0.0f) return;
    constexpr float start = -44.0f;
    constexpr float radius = 84.0f;
    constexpr float radians = std::numbers::pi_v<float> / 180.0f;
    const float sweep = -360.0f * progress;
    const int segments = static_cast<int>(std::ceil(-sweep / 2.0f));
    PointF inner[181];
    GraphicsPath stroke;
    if (progress < 1.0f)
        stroke.AddArc(-radius, -radius, 2 * radius, 2 * radius, start, sweep);
    else
        stroke.AddEllipse(-radius, -radius, 2 * radius, 2 * radius);
    for (int i = 0; i <= segments; ++i)
    {
        const float degrees = start + sweep * i / segments;
        const float angle = degrees * radians;
        const float pressure = (1.0f - std::cos((degrees + 45.0f) * radians)) * 0.5f;
        const float nib = 1.1f + 13.3f * pressure;
        const float x = std::cos(angle), y = std::sin(angle);
        inner[segments - i] = PointF((radius - 2 * nib) * x, (radius - 2 * nib) * y);
        if (i == segments && progress < 1.0f)
        {
            stroke.AddArc((radius - nib) * x - nib, (radius - nib) * y - nib,
                          2 * nib, 2 * nib, degrees, -180.0f);
        }
    }
    stroke.AddLines(inner, segments + 1);
    stroke.CloseFigure();
    g.FillPath(&brush, &stroke);
}

void DrawMark(Graphics& g, float time, BYTE opacity)
{
    const auto gold = flare::Gold;
    SolidBrush brush(Color(opacity, gold.r, gold.g, gold.b));
    DrawRing(g, brush, Ease((time - 20.0f) / 360.0f));

    const float starTime = time - 340.0f;
    if (starTime <= 0.0f) return;
    const float scale = Ease(starTime / 220.0f);
    const auto transform = g.Save();
    g.TranslateTransform(65.0f, -59.0f);
    g.RotateTransform(-397.5f * (1.0f - Ease(starTime / 340.0f)));
    g.ScaleTransform(25.0f * scale, 29.0f * scale);
    GraphicsPath flarePath;
    flarePath.AddBezier(0.f,-1.f, .06f,-.18f, .18f,-.06f, 1.f,0.f);
    flarePath.AddBezier(1.f,0.f, .18f,.06f, .06f,.18f, 0.f,1.f);
    flarePath.AddBezier(0.f,1.f, -.06f,.18f, -.18f,.06f, -1.f,0.f);
    flarePath.AddBezier(-1.f,0.f, -.18f,-.06f, -.06f,-.18f, 0.f,-1.f);
    flarePath.CloseFigure();
    SolidBrush highlight(Color(static_cast<BYTE>(opacity * Ease(starTime / 180.0f)), gold.r, gold.g, gold.b));
    g.FillPath(&highlight, &flarePath);
    g.Restore(transform);
}
}

void LauncherIntro::Start(HWND window)
{
    BOOL animate = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0) && !animate) return;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&graphics_, &input, nullptr) != Gdiplus::Ok) return;
    RECT rect{};
    GetClientRect(window, &rect);
    size_ = {rect.right, rect.bottom};
    const HDC target = GetDC(window);
    if (!target) { Release(); return; }
    const HDC dc = CreateCompatibleDC(target);
    const HBITMAP page = CreateCompatibleBitmap(target, size_.cx, size_.cy);
    frame_ = CreateCompatibleBitmap(target, size_.cx, size_.cy);
    ReleaseDC(window, target);
    if (!dc || !page || !frame_)
    {
        if (dc) DeleteDC(dc);
        if (page) DeleteObject(page);
        Release();
        return;
    }
    // 在隐藏控件前绘制真实列表，淡出末帧与用户接手的页面一致。
    const auto old = SelectObject(dc, page);
    SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_ERASEBKGND | PRF_CLIENT | PRF_CHILDREN);
    SelectObject(dc, old);
    DeleteDC(dc);
    page_ = page;
    if (!SetTimer(window, Timer, 16, nullptr)) { Release(); return; }
    hiddenControls_.clear();
    for (HWND h = GetWindow(window, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT))
    {
        if (IsWindowVisible(h))
        {
            hiddenControls_.push_back(h);
            ShowWindow(h, SW_HIDE);
        }
    }
    start_ = GetTickCount64();
}

void LauncherIntro::Paint(HDC target) const
{
    const float time = static_cast<float>(GetTickCount64() - start_);
    const float fade = Ease((time - FadeStartMs) / (DurationMs - FadeStartMs));
    const BYTE opacity = static_cast<BYTE>(255.0f * (1.0f - fade));
    const HDC dc = CreateCompatibleDC(target);
    if (!dc) return;
    const auto old = SelectObject(dc, frame_);
    if (opacity < 255)
    {
        const HDC source = CreateCompatibleDC(target);
        if (source)
        {
            const auto previous = SelectObject(source, page_);
            BitBlt(dc, 0, 0, size_.cx, size_.cy, source, 0, 0, SRCCOPY);
            SelectObject(source, previous);
            DeleteDC(source);
        }
    }
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const auto gray = flare::Background;
        Gdiplus::SolidBrush backdrop(Gdiplus::Color(opacity, gray.r, gray.g, gray.b));
        g.FillRectangle(&backdrop, 0, 0, static_cast<int>(size_.cx), static_cast<int>(size_.cy));
        const float scale = std::min(size_.cx, size_.cy) / 320.0f;
        g.TranslateTransform(size_.cx * 0.5f, size_.cy * 0.5f);
        g.ScaleTransform(scale, scale);
        g.TranslateTransform(-4.0f, 2.0f);
        DrawMark(g, time, opacity);
    }
    BitBlt(target, 0, 0, size_.cx, size_.cy, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteDC(dc);
}

void LauncherIntro::Tick(HWND window)
{
    if (GetTickCount64() - start_ >= static_cast<ULONGLONG>(DurationMs)) Finish(window);
    else InvalidateRect(window, nullptr, FALSE);
}

void LauncherIntro::Finish(HWND window)
{
    if (!Active()) return;
    KillTimer(window, Timer);
    Release();
    for (HWND h : hiddenControls_)
    {
        if (IsWindow(h)) ShowWindow(h, SW_SHOWNA);
    }
    hiddenControls_.clear();
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    SetFocus(GetNextDlgTabItem(window, nullptr, FALSE));
}

void LauncherIntro::Release()
{
    if (page_) { DeleteObject(page_); page_ = nullptr; }
    if (frame_) { DeleteObject(frame_); frame_ = nullptr; }
    if (graphics_) { Gdiplus::GdiplusShutdown(graphics_); graphics_ = 0; }
}
LauncherIntro::~LauncherIntro() { Release(); }

void DrawFlareMark(HDC dc, int centerX, int centerY, int size)
{
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // 标志（含星芒）约占 200 个设计单位；与片头相同的 -4,2 偏移让圆环视觉居中。
    const float scale = size / 200.0f;
    g.TranslateTransform(static_cast<float>(centerX), static_cast<float>(centerY));
    g.ScaleTransform(scale, scale);
    g.TranslateTransform(-4.0f, 2.0f);
    DrawMark(g, 10000.0f, 255);
}
}
