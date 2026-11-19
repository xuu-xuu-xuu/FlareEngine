#pragma once

#include "EditorWindowStyle.h"

namespace se::launcher_style
{
enum class ButtonRole : unsigned int
{
    Normal = 0,
    Primary = 1,
    Danger = 2,
    Card = 3,
};

// 原生控件保留键盘、辅助功能和通知行为，父窗口负责着色与自绘卡片。
LRESULT ControlColors(UINT message, HDC dc, bool muted, bool onSurface = false);
bool DrawControl(const DRAWITEMSTRUCT& item);
void ThemeControl(HWND control, HFONT boldFont = nullptr);
void SetButtonRole(HWND button, ButtonRole role, bool active = false);
void SetPlaceholder(HWND control, const wchar_t* text);
void Fill(HDC dc, RECT rect, flare::Color color);
void Frame(HDC dc, RECT rect, flare::Color color, int thickness = 1);
}
