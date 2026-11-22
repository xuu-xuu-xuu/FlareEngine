#pragma once

#include "EditorWindowStyle.h"
#include <string>
#include <vector>

namespace flare::launcher_style
{
enum class ButtonRole : unsigned int
{
    Normal = 0,
    Primary = 1,
    Danger = 2,
    Card = 3,
};

struct PopupMenuItem
{
    int id = 0;
    std::wstring label;
    bool enabled = true;
    bool separator = false;
    bool danger = false;
};

// 原生控件保留键盘、辅助功能和通知行为，父窗口负责着色与自绘卡片。
LRESULT ControlColors(UINT message, HDC dc, bool muted, bool onSurface = false);
bool DrawControl(const DRAWITEMSTRUCT& item);
void ThemeControl(HWND control, HFONT boldFont = nullptr);
void SetButtonRole(HWND button, ButtonRole role, bool active = false);
void SetPlaceholder(HWND control, const wchar_t* text);
int ShowPopupMenu(HWND owner, int screenX, int screenY, int minWidth,
                  const std::vector<PopupMenuItem>& items, HFONT font = nullptr);
void Fill(HDC dc, RECT rect, flare::palette::Color color);
void Frame(HDC dc, RECT rect, flare::palette::Color color, int thickness = 1);
// GDI+ 抗锯齿圆角；GDI+ 不可用时退回直角 Fill/Frame。
bool GdiplusReady();
void FillRound(HDC dc, RECT rect, flare::palette::Color color, int radius, BYTE alpha = 255);
void FrameRound(HDC dc, RECT rect, flare::palette::Color color, int radius, float thickness = 1.0f, BYTE alpha = 255);
// 以 (x, y) 为中心的径向柔光，中心 alpha 向外渐隐为 0。
void Glow(HDC dc, int x, int y, int radius, flare::palette::Color color, BYTE alpha);
// 水平细线，两端渐隐。
void FadeLine(HDC dc, RECT rect, flare::palette::Color color, BYTE alpha = 255);
}
