#include "LauncherStyle.h"
#include <objidl.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace flare::launcher_style
{
// shortcut: 进程级 GDI+ 令牌不主动关闭，随进程退出回收；LauncherIntro 自己的令牌照常成对开关。
bool GdiplusReady()
{
    static const bool ready = [] {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    return ready;
}

namespace
{
Gdiplus::Color Argb(flare::palette::Color color, BYTE alpha)
{
    return Gdiplus::Color(alpha, color.r, color.g, color.b);
}

void RoundPath(Gdiplus::GraphicsPath& path, float x, float y, float width, float height, float radius)
{
    const float d = std::min(radius * 2.0f, std::min(width, height));
    if (d <= 0.0f) { path.AddRectangle(Gdiplus::RectF(x, y, width, height)); return; }
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + width - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + width - d, y + height - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void Smooth(Gdiplus::Graphics& g)
{
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
}
} // namespace

void Fill(HDC dc, RECT rect, flare::palette::Color color)
{
    SetDCBrushColor(dc, flare::palette::Native(color));
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Frame(HDC dc, RECT rect, flare::palette::Color color, int thickness)
{
    SetDCBrushColor(dc, flare::palette::Native(color));
    const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    for (int i = 0; i < thickness; ++i)
    {
        FrameRect(dc, &rect, brush);
        InflateRect(&rect, -1, -1);
    }
}

void FillRound(HDC dc, RECT rect, flare::palette::Color color, int radius, BYTE alpha)
{
    if (!GdiplusReady() || radius <= 0) { Fill(dc, rect, color); return; }
    Gdiplus::Graphics g(dc);
    Smooth(g);
    Gdiplus::GraphicsPath path;
    RoundPath(path, static_cast<float>(rect.left), static_cast<float>(rect.top),
              static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top), static_cast<float>(radius));
    Gdiplus::SolidBrush brush(Argb(color, alpha));
    g.FillPath(&brush, &path);
}

void FrameRound(HDC dc, RECT rect, flare::palette::Color color, int radius, float thickness, BYTE alpha)
{
    if (!GdiplusReady() || radius <= 0) { Frame(dc, rect, color, std::max(1, static_cast<int>(thickness))); return; }
    Gdiplus::Graphics g(dc);
    Smooth(g);
    const float half = thickness * 0.5f;
    Gdiplus::GraphicsPath path;
    RoundPath(path, rect.left + half, rect.top + half, rect.right - rect.left - thickness,
              rect.bottom - rect.top - thickness, std::max(0.0f, radius - half));
    Gdiplus::Pen pen(Argb(color, alpha), thickness);
    g.DrawPath(&pen, &path);
}

void Glow(HDC dc, int x, int y, int radius, flare::palette::Color color, BYTE alpha)
{
    if (!GdiplusReady() || radius <= 0) return;
    Gdiplus::Graphics g(dc);
    Smooth(g);
    Gdiplus::GraphicsPath path;
    path.AddEllipse(static_cast<float>(x - radius), static_cast<float>(y - radius),
                    static_cast<float>(2 * radius), static_cast<float>(2 * radius));
    Gdiplus::PathGradientBrush brush(&path);
    brush.SetCenterColor(Argb(color, alpha));
    Gdiplus::Color edge = Argb(color, 0);
    INT count = 1;
    brush.SetSurroundColors(&edge, &count);
    g.FillPath(&brush, &path);
}

void FadeLine(HDC dc, RECT rect, flare::palette::Color color, BYTE alpha)
{
    if (!GdiplusReady()) { Fill(dc, rect, color); return; }
    Gdiplus::Graphics g(dc);
    const Gdiplus::RectF area(static_cast<float>(rect.left), static_cast<float>(rect.top),
                              static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
    Gdiplus::LinearGradientBrush brush(area, Argb(color, 0), Argb(color, 0), Gdiplus::LinearGradientModeHorizontal);
    const Gdiplus::Color stops[3] = {Argb(color, 0), Argb(color, alpha), Argb(color, 0)};
    const Gdiplus::REAL positions[3] = {0.0f, 0.35f, 1.0f};
    brush.SetInterpolationColors(stops, positions, 3);
    g.FillRectangle(&brush, area);
}

LRESULT ControlColors(UINT message, HDC dc, bool muted, bool onSurface)
{
    const auto background = onSurface ? flare::palette::Surface
        : (message == WM_CTLCOLORSTATIC ? flare::palette::Background : flare::palette::Surface);
    SetTextColor(dc, flare::palette::Native(muted ? flare::palette::Muted : flare::palette::Text));
    SetBkColor(dc, flare::palette::Native(background));
    SetDCBrushColor(dc, flare::palette::Native(background));
    return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
}

namespace
{
int ScaleForWindow(HWND hwnd, int dip)
{
    const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96u;
    return MulDiv(dip, dpi ? static_cast<int>(dpi) : 96, 96);
}

LRESULT CALLBACK ButtonHover(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (message == WM_MOUSEMOVE && !GetPropW(hwnd, L"FlareHover"))
    {
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
        if (TrackMouseEvent(&tracking))
        {
            SetPropW(hwnd, L"FlareHover", reinterpret_cast<HANDLE>(1));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    if (message == WM_MOUSELEAVE || message == WM_NCDESTROY)
    {
        RemovePropW(hwnd, L"FlareHover");
        if (message == WM_NCDESTROY)
        {
            RemovePropW(hwnd, L"FlareRole");
            RemovePropW(hwnd, L"FlareActive");
            RemovePropW(hwnd, L"FlareBoldFont");
            RemoveWindowSubclass(hwnd, ButtonHover, 1);
        }
        else
        {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    return DefSubclassProc(hwnd, message, wp, lp);
}

LRESULT CALLBACK ListHover(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (message == WM_MOUSEMOVE)
    {
        if (!GetPropW(hwnd, L"FlareTrack"))
        {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
            if (TrackMouseEvent(&tracking))
                SetPropW(hwnd, L"FlareTrack", reinterpret_cast<HANDLE>(1));
        }
        const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
        const int nextRow = HIWORD(hit) == 0 ? static_cast<int>(LOWORD(hit)) + 1 : 0;
        const int prevRow = static_cast<int>(reinterpret_cast< UINT_PTR >(GetPropW(hwnd, L"FlareHoverRow")));
        if (nextRow != prevRow)
        {
            if (nextRow) SetPropW(hwnd, L"FlareHoverRow", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(nextRow)));
            else RemovePropW(hwnd, L"FlareHoverRow");
            if (prevRow > 0)
            {
                RECT r{};
                if (SendMessageW(hwnd, LB_GETITEMRECT, static_cast<WPARAM>(prevRow - 1), reinterpret_cast<LPARAM>(&r)) != LB_ERR)
                    InvalidateRect(hwnd, &r, FALSE);
            }
            if (nextRow > 0)
            {
                RECT r{};
                if (SendMessageW(hwnd, LB_GETITEMRECT, static_cast<WPARAM>(nextRow - 1), reinterpret_cast<LPARAM>(&r)) != LB_ERR)
                    InvalidateRect(hwnd, &r, FALSE);
            }
        }
    }
    else if (message == WM_MOUSELEAVE || message == WM_NCDESTROY)
    {
        const int prevRow = static_cast<int>(reinterpret_cast<UINT_PTR>(GetPropW(hwnd, L"FlareHoverRow")));
        RemovePropW(hwnd, L"FlareTrack");
        RemovePropW(hwnd, L"FlareHoverRow");
        if (message == WM_NCDESTROY)
        {
            RemovePropW(hwnd, L"FlareBoldFont");
            RemovePropW(hwnd, L"FlarePlaceholder");
            RemoveWindowSubclass(hwnd, ListHover, 2);
            return DefSubclassProc(hwnd, message, wp, lp);
        }
        else if (prevRow > 0)
        {
            RECT r{};
            if (SendMessageW(hwnd, LB_GETITEMRECT, static_cast<WPARAM>(prevRow - 1), reinterpret_cast<LPARAM>(&r)) != LB_ERR)
                InvalidateRect(hwnd, &r, FALSE);
        }
    }
    const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
    if ((message == WM_PAINT || message == WM_PRINTCLIENT) && SendMessageW(hwnd, LB_GETCOUNT, 0, 0) == 0)
    {
        const auto* hint = reinterpret_cast<const wchar_t*>(GetPropW(hwnd, L"FlarePlaceholder"));
        if (hint && hint[0])
        {
            const HDC dc = message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wp) : GetDC(hwnd);
            if (dc)
            {
                const int saved = SaveDC(dc);
                RECT client{};
                GetClientRect(hwnd, &client);
                launcher_style::FrameRound(dc, client, flare::palette::Border, ScaleForWindow(hwnd, 10));
                const auto font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
                if (font) SelectObject(dc, font);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, flare::palette::Native(flare::palette::Muted));
                InflateRect(&client, -ScaleForWindow(hwnd, 24), 0);
                DrawTextW(dc, hint, -1, &client, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                RestoreDC(dc, saved);
                if (message != WM_PRINTCLIENT) ReleaseDC(hwnd, dc);
            }
        }
    }
    return result;
}

LRESULT CALLBACK EditPlaceholderProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (message == WM_NCDESTROY)
    {
        RemovePropW(hwnd, L"FlarePlaceholder");
        RemovePropW(hwnd, L"FlareBoldFont");
        RemoveWindowSubclass(hwnd, EditPlaceholderProc, 3);
        return DefSubclassProc(hwnd, message, wp, lp);
    }
    const LRESULT result = DefSubclassProc(hwnd, message, wp, lp);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    else if ((message == WM_PAINT || message == WM_PRINTCLIENT) &&
             GetFocus() != hwnd && GetWindowTextLengthW(hwnd) == 0)
    {
        const auto* hint = reinterpret_cast<const wchar_t*>(GetPropW(hwnd, L"FlarePlaceholder"));
        if (hint && hint[0])
        {
            const HDC dc = message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(wp) : GetDC(hwnd);
            if (dc)
            {
                const int saved = SaveDC(dc);
                RECT client{};
                GetClientRect(hwnd, &client);
                const auto font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
                if (font) SelectObject(dc, font);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, flare::palette::Native(flare::palette::Muted));
                InflateRect(&client, -ScaleForWindow(hwnd, 8), 0);
                DrawTextW(dc, hint, -1, &client, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
                RestoreDC(dc, saved);
                if (message != WM_PRINTCLIENT) ReleaseDC(hwnd, dc);
            }
        }
    }
    return result;
}

std::vector<std::wstring_view> SplitTabs(std::wstring_view text)
{
    std::vector<std::wstring_view> parts;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const auto pos = text.find(L'\t', start);
        if (pos == std::wstring_view::npos)
        {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}
} // namespace

void SetButtonRole(HWND button, ButtonRole role, bool active)
{
    if (!button) return;
    if (role != ButtonRole::Normal)
        SetPropW(button, L"FlareRole", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(role)));
    else
        RemovePropW(button, L"FlareRole");
    if (active)
        SetPropW(button, L"FlareActive", reinterpret_cast<HANDLE>(1));
    else
        RemovePropW(button, L"FlareActive");
    InvalidateRect(button, nullptr, FALSE);
}

void SetPlaceholder(HWND control, const wchar_t* text)
{
    if (!control) return;
    if (text && text[0]) SetPropW(control, L"FlarePlaceholder", reinterpret_cast<HANDLE>(const_cast<wchar_t*>(text)));
    else RemovePropW(control, L"FlarePlaceholder");
    InvalidateRect(control, nullptr, FALSE);
}

void ThemeControl(HWND control, HFONT boldFont)
{
    if (!control) return;
    if (boldFont) SetPropW(control, L"FlareBoldFont", reinterpret_cast<HANDLE>(boldFont));
    wchar_t type[32]{};
    GetClassNameW(control, type, 32);
    if (lstrcmpiW(type, L"BUTTON") == 0)
    {
        SetWindowSubclass(control, ButtonHover, 1, 0);
    }
    else if (lstrcmpiW(type, L"LISTBOX") == 0)
    {
        SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
        SetWindowSubclass(control, ListHover, 2, 0);
    }
    else if (lstrcmpiW(type, L"EDIT") == 0)
    {
        SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
        SetWindowSubclass(control, EditPlaceholderProc, 3, 0);
    }
    else if (lstrcmpiW(type, L"COMBOBOX") == 0)
    {
        SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
    }
}

bool DrawControl(const DRAWITEMSTRUCT& item)
{
    const bool button = item.CtlType == ODT_BUTTON;
    const bool list = item.CtlType == ODT_LISTBOX;
    if (!button && !list && item.CtlType != ODT_COMBOBOX) return false;

    const RECT targetRect = item.rcItem;
    const int width = targetRect.right - targetRect.left;
    const int height = targetRect.bottom - targetRect.top;
    if (width <= 0 || height <= 0) return true;

    const HDC memDc = CreateCompatibleDC(item.hDC);
    const HBITMAP memBmp = memDc ? CreateCompatibleBitmap(item.hDC, width, height) : nullptr;
    const HGDIOBJ oldBmp = (memDc && memBmp) ? SelectObject(memDc, memBmp) : nullptr;
    HDC dc = oldBmp ? memDc : item.hDC;
    RECT row = oldBmp ? RECT{0, 0, width, height} : targetRect;
    const int saved = SaveDC(dc);
    const auto finishDraw = [&] {
        RestoreDC(dc, saved);
        if (oldBmp)
        {
            BitBlt(item.hDC, targetRect.left, targetRect.top, width, height, memDc, 0, 0, SRCCOPY);
            SelectObject(memDc, oldBmp);
        }
        if (memBmp) DeleteObject(memBmp);
        if (memDc) DeleteDC(memDc);
        return true;
    };
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS;
    const auto baseFont = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    const auto boldFont = reinterpret_cast<HFONT>(GetPropW(item.hwndItem, L"FlareBoldFont"));
    if (baseFont) SelectObject(dc, baseFont);
    SetBkMode(dc, TRANSPARENT);

    if (button)
    {
        const bool hovered = GetPropW(item.hwndItem, L"FlareHover") != nullptr;
        const auto role = static_cast<ButtonRole>(reinterpret_cast<UINT_PTR>(GetPropW(item.hwndItem, L"FlareRole")));
        const bool active = GetPropW(item.hwndItem, L"FlareActive") != nullptr;

        wchar_t rawText[512]{};
        GetWindowTextW(item.hwndItem, rawText, 512);
        const std::wstring_view text(rawText);

        if (role == ButtonRole::Card)
        {
            DrawThemeParentBackground(item.hwndItem, dc, &row);
            const int radius = ScaleForWindow(item.hwndItem, 8);
            const auto bg = active ? flare::palette::Selected : (hovered && !disabled ? flare::palette::Hover : flare::palette::Surface);
            FillRound(dc, row, bg, radius);
            FrameRound(dc, row, active ? flare::palette::Gold : (hovered ? flare::palette::Silver : flare::palette::Border), radius,
                       active ? 1.5f : 1.0f, active ? 230 : (hovered ? 120 : 255));
            if (active)
            {
                RECT bar{row.left + ScaleForWindow(item.hwndItem, 5), row.top + ScaleForWindow(item.hwndItem, 12),
                         row.left + ScaleForWindow(item.hwndItem, 8), row.bottom - ScaleForWindow(item.hwndItem, 12)};
                FillRound(dc, bar, flare::palette::Gold, ScaleForWindow(item.hwndItem, 2));
            }

            const int padX = ScaleForWindow(item.hwndItem, 12);
            const int padY = ScaleForWindow(item.hwndItem, 8);
            const auto newline = text.find(L'\n');
            const std::wstring title(newline == std::wstring_view::npos ? text : text.substr(0, newline));
            const std::wstring desc(newline == std::wstring_view::npos ? std::wstring_view{} : text.substr(newline + 1));

            RECT titleRect{row.left + padX, row.top + padY, row.right - padX, row.top + padY + ScaleForWindow(item.hwndItem, 22)};
            if (boldFont) SelectObject(dc, boldFont);
            SetTextColor(dc, flare::palette::Native(disabled ? flare::palette::Disabled : flare::palette::Heading));
            DrawTextW(dc, title.c_str(), -1, &titleRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

            if (active)
            {
                SetTextColor(dc, flare::palette::Native(flare::palette::Gold));
                DrawTextW(dc, L"●", -1, &titleRect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
            }

            if (!desc.empty())
            {
                if (baseFont) SelectObject(dc, baseFont);
                RECT descRect{row.left + padX, titleRect.bottom + ScaleForWindow(item.hwndItem, 3),
                              row.right - padX, row.bottom - padY};
                SetTextColor(dc, flare::palette::Native(active ? flare::palette::Text : flare::palette::Muted));
                DrawTextW(dc, desc.c_str(), -1, &descRect, DT_WORDBREAK | DT_LEFT | DT_NOPREFIX);
            }
            return finishDraw();
        }

        flare::palette::Color bg = flare::palette::Raised;
        flare::palette::Color border = flare::palette::Border;
        flare::palette::Color textColor = flare::palette::Heading;
        BYTE borderAlpha = 255;
        if (disabled)
        {
            bg = flare::palette::Surface;
            textColor = flare::palette::Disabled;
        }
        else if (role == ButtonRole::Primary)
        {
            // 黑字压香槟金：主操作唯一的高亮色块。
            constexpr flare::palette::Color pressedGold{206, 160, 82};
            bg = selected ? pressedGold : (hovered ? flare::palette::AccentHover : flare::palette::Accent);
            border = focused ? flare::palette::Heading : bg;
            textColor = flare::palette::Background;
        }
        else if (role == ButtonRole::Danger)
        {
            bg = selected || hovered ? flare::palette::Danger : flare::palette::Raised;
            border = hovered ? flare::palette::Danger : flare::palette::Border;
        }
        else
        {
            bg = selected ? flare::palette::Selected : (hovered ? flare::palette::Hover : flare::palette::Raised);
            border = focused ? flare::palette::Gold : (hovered ? flare::palette::Silver : flare::palette::Border);
            if (hovered && !focused) borderAlpha = 110;
        }

        DrawThemeParentBackground(item.hwndItem, dc, &row);
        const int radius = ScaleForWindow(item.hwndItem, 6);
        FillRound(dc, row, bg, radius);
        FrameRound(dc, row, border, radius, 1.0f, borderAlpha);
        if (role == ButtonRole::Primary && boldFont && !disabled) SelectObject(dc, boldFont);
        SetTextColor(dc, flare::palette::Native(textColor));
        InflateRect(&row, -ScaleForWindow(item.hwndItem, 10), 0);
        DrawTextW(dc, rawText, -1, &row, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        return finishDraw();
    }

    if (item.itemID != static_cast<UINT>(-1))
    {
        const auto length = SendMessageW(item.hwndItem, list ? LB_GETTEXTLEN : CB_GETLBTEXTLEN, item.itemID, 0);
        if (length >= 0)
        {
            std::wstring label(static_cast<std::size_t>(length) + 1, L'\0');
            SendMessageW(item.hwndItem, list ? LB_GETTEXT : CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(label.data()));
            label.resize(static_cast<std::size_t>(length));

            const auto parts = list ? SplitTabs(label) : std::vector<std::wstring_view>{};
            if (list && parts.size() >= 5)
            {
                Fill(dc, row, flare::palette::Background);
                RECT card = row;
                InflateRect(&card, -ScaleForWindow(item.hwndItem, 2), -ScaleForWindow(item.hwndItem, 2));

                const int hoverRow = static_cast<int>(reinterpret_cast<UINT_PTR>(GetPropW(item.hwndItem, L"FlareHoverRow")));
                const bool hovered = hoverRow == static_cast<int>(item.itemID) + 1;
                const bool available = parts[4] == L"1";

                const auto cardBg = selected ? flare::palette::Selected : (hovered ? flare::palette::Hover : flare::palette::Surface);
                const int cardRadius = ScaleForWindow(item.hwndItem, 8);
                FillRound(dc, card, cardBg, cardRadius);
                FrameRound(dc, card, selected ? flare::palette::Gold : (hovered ? flare::palette::Silver : flare::palette::Border), cardRadius,
                           1.0f, selected ? 200 : (hovered ? 110 : 255));

                if (selected || !available)
                {
                    RECT stripe{card.left + ScaleForWindow(item.hwndItem, 4), card.top + ScaleForWindow(item.hwndItem, 12),
                                card.left + ScaleForWindow(item.hwndItem, 7), card.bottom - ScaleForWindow(item.hwndItem, 12)};
                    FillRound(dc, stripe, !available ? flare::palette::Warning : flare::palette::Gold, ScaleForWindow(item.hwndItem, 2));
                }

                const int pad = ScaleForWindow(item.hwndItem, 10);
                const int tileSize = std::max(20, static_cast<int>((card.bottom - card.top) - 2 * ScaleForWindow(item.hwndItem, 9)));
                RECT tile{card.left + pad + ScaleForWindow(item.hwndItem, 4),
                          card.top + ((card.bottom - card.top) - tileSize) / 2,
                          card.left + pad + ScaleForWindow(item.hwndItem, 4) + tileSize,
                          card.top + ((card.bottom - card.top) + tileSize) / 2};
                const int tileRadius = ScaleForWindow(item.hwndItem, 7);
                FillRound(dc, tile, flare::palette::Background, tileRadius);
                FrameRound(dc, tile, !available ? flare::palette::Warning : flare::palette::Gold, tileRadius, 1.0f, !available ? 255 : 90);

                wchar_t initial[2]{L'F', L'\0'};
                for (wchar_t ch : parts[0])
                {
                    if (!std::iswspace(ch)) { initial[0] = static_cast<wchar_t>(std::towupper(ch)); break; }
                }
                if (boldFont) SelectObject(dc, boldFont);
                SetTextColor(dc, flare::palette::Native(!available ? flare::palette::Warning : flare::palette::Gold));
                DrawTextW(dc, initial, 1, &tile, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

                const std::wstring name(parts[0]);
                const std::wstring path(parts[1]);
                const std::wstring date(parts[2]);
                const std::wstring badge(parts[3]);

                const int textLeft = tile.right + ScaleForWindow(item.hwndItem, 12);
                const int rightEdge = card.right - pad;
                const int midY = card.top + (card.bottom - card.top) / 2;

                // 右上状态徽标与日期
                if (baseFont) SelectObject(dc, baseFont);
                RECT badgeMeasure{0, 0, 200, 24};
                DrawTextW(dc, badge.c_str(), -1, &badgeMeasure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                const int badgeW = (badgeMeasure.right - badgeMeasure.left) + ScaleForWindow(item.hwndItem, 14);
                const int badgeH = ScaleForWindow(item.hwndItem, 19);
                RECT badgeRect{rightEdge - badgeW, card.top + ScaleForWindow(item.hwndItem, 7),
                               rightEdge, card.top + ScaleForWindow(item.hwndItem, 7) + badgeH};
                FillRound(dc, badgeRect, !available ? flare::palette::Danger : flare::palette::Raised, badgeH / 2);
                FrameRound(dc, badgeRect, !available ? flare::palette::Warning : flare::palette::Border, badgeH / 2);
                SetTextColor(dc, flare::palette::Native(!available ? flare::palette::Heading : flare::palette::Silver));
                DrawTextW(dc, badge.c_str(), -1, &badgeRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

                int nameRight = badgeRect.left - ScaleForWindow(item.hwndItem, 8);
                if (!date.empty())
                {
                    RECT dateRect{textLeft, badgeRect.top, nameRight, badgeRect.bottom};
                    SetTextColor(dc, flare::palette::Native(selected ? flare::palette::Silver : flare::palette::Muted));
                    DrawTextW(dc, date.c_str(), -1, &dateRect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
                    RECT dateMeasure{0, 0, 300, 24};
                    DrawTextW(dc, date.c_str(), -1, &dateMeasure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                    nameRight -= (dateMeasure.right - dateMeasure.left) + ScaleForWindow(item.hwndItem, 12);
                }

                RECT nameRect{textLeft, card.top + ScaleForWindow(item.hwndItem, 5),
                              std::max(textLeft + 40, nameRight), midY + ScaleForWindow(item.hwndItem, 1)};
                if (boldFont) SelectObject(dc, boldFont);
                SetTextColor(dc, flare::palette::Native(flare::palette::Heading));
                DrawTextW(dc, name.c_str(), -1, &nameRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

                RECT pathRect{textLeft, midY + ScaleForWindow(item.hwndItem, 1),
                              rightEdge, card.bottom - ScaleForWindow(item.hwndItem, 5)};
                if (baseFont) SelectObject(dc, baseFont);
                SetTextColor(dc, flare::palette::Native(selected ? flare::palette::Text : flare::palette::Muted));
                DrawTextW(dc, path.c_str(), -1, &pathRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_PATH_ELLIPSIS | DT_NOPREFIX);
            }
            else
            {
                Fill(dc, row, selected ? flare::palette::Selected : flare::palette::Surface);
                if (focused) Frame(dc, row, flare::palette::Accent, 1);
                SetTextColor(dc, flare::palette::Native(disabled ? flare::palette::Disabled : flare::palette::Heading));
                InflateRect(&row, -ScaleForWindow(item.hwndItem, 9), 0);
                DrawTextW(dc, label.c_str(), -1, &row, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
    }

    return finishDraw();
}

namespace
{
struct PopupMenuState
{
    const std::vector<PopupMenuItem>* items = nullptr;
    std::vector<RECT> itemRects;
    HFONT font = nullptr;
    int width = 0;
    int height = 0;
    int pad = 5;
    int hover = -1;
    int selectedId = 0;
    bool closing = false;
    bool closed = false;
};

int HitPopupItem(const PopupMenuState& s, int x, int y)
{
    if (!s.items || x < 0 || y < 0 || x >= s.width || y >= s.height) return -1;
    for (std::size_t i = 0; i < s.itemRects.size(); ++i)
    {
        const auto& item = (*s.items)[i];
        if (item.separator || !item.enabled) continue;
        const RECT& r = s.itemRects[i];
        if (y >= r.top && y < r.bottom) return static_cast<int>(i);
    }
    return -1;
}

void StepPopupHover(PopupMenuState& s, HWND hwnd, int delta)
{
    if (!s.items || s.items->empty()) return;
    const int n = static_cast<int>(s.items->size());
    int idx = s.hover;
    for (int step = 0; step < n; ++step)
    {
        idx = (idx + delta + n) % n;
        if (!(*s.items)[static_cast<std::size_t>(idx)].separator && (*s.items)[static_cast<std::size_t>(idx)].enabled)
        {
            if (s.hover != idx)
            {
                s.hover = idx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return;
        }
    }
}

void PaintPopupMenu(HWND hwnd, const PopupMenuState& s, HDC targetDc)
{
    if (s.width <= 0 || s.height <= 0 || !s.items) return;
    const HDC memDc = CreateCompatibleDC(targetDc);
    const HBITMAP memBmp = memDc ? CreateCompatibleBitmap(targetDc, s.width, s.height) : nullptr;
    const HGDIOBJ oldBmp = (memDc && memBmp) ? SelectObject(memDc, memBmp) : nullptr;
    HDC dc = oldBmp ? memDc : targetDc;
    const int saved = SaveDC(dc);

    RECT full{0, 0, s.width, s.height};
    Fill(dc, full, flare::palette::Surface);
    Frame(dc, full, flare::palette::Border, 1);
    RECT topAccent{1, 1, s.width - 1, 2};
    FadeLine(dc, topAccent, flare::palette::Gold, 160);

    if (s.font) SelectObject(dc, s.font);
    SetBkMode(dc, TRANSPARENT);
    const int rowRadius = ScaleForWindow(hwnd, 5);
    const int textPadX = ScaleForWindow(hwnd, 12);

    for (std::size_t i = 0; i < s.items->size(); ++i)
    {
        const auto& item = (*s.items)[i];
        const RECT& r = s.itemRects[i];
        if (item.separator)
        {
            const int midY = (r.top + r.bottom) / 2;
            RECT line{r.left + ScaleForWindow(hwnd, 6), midY, r.right - ScaleForWindow(hwnd, 6), midY + 1};
            Fill(dc, line, flare::palette::Border);
            continue;
        }
        const bool hovered = static_cast<int>(i) == s.hover && item.enabled;
        if (hovered)
        {
            FillRound(dc, r, item.danger ? flare::palette::Danger : flare::palette::Hover, rowRadius);
            if (!item.danger)
            {
                RECT pill{r.left + ScaleForWindow(hwnd, 3), r.top + ScaleForWindow(hwnd, 7),
                          r.left + ScaleForWindow(hwnd, 6), r.bottom - ScaleForWindow(hwnd, 7)};
                FillRound(dc, pill, flare::palette::Gold, ScaleForWindow(hwnd, 2));
            }
        }
        flare::palette::Color textColor = flare::palette::Text;
        if (!item.enabled) textColor = flare::palette::Disabled;
        else if (hovered) textColor = flare::palette::Heading;
        else if (item.danger) textColor = flare::palette::Danger;
        SetTextColor(dc, flare::palette::Native(textColor));

        RECT labelRect{r.left + textPadX, r.top, r.right - textPadX, r.bottom};
        DrawTextW(dc, item.label.c_str(), -1, &labelRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    RestoreDC(dc, saved);
    if (oldBmp)
    {
        BitBlt(targetDc, 0, 0, s.width, s.height, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
    }
    if (memBmp) DeleteObject(memBmp);
    if (memDc) DeleteDC(memDc);
}

void ClosePopupMenu(HWND hwnd, PopupMenuState* s, int selectedId = 0)
{
    if (!s || s->closing) return;
    s->closing = true;
    s->selectedId = selectedId;
    s->closed = true;
    if (GetCapture() == hwnd) ReleaseCapture();
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
}

LRESULT CALLBACK PopupMenuProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    auto* s = reinterpret_cast<PopupMenuState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return DefWindowProcW(hwnd, message, wp, lp);
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        if (s) PaintPopupMenu(hwnd, *s, reinterpret_cast<HDC>(wp));
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        const HDC dc = BeginPaint(hwnd, &ps);
        if (s) PaintPopupMenu(hwnd, *s, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (s)
        {
            const int hit = HitPopupItem(*s, static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
            if (hit != s->hover)
            {
                s->hover = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (s)
        {
            const int x = static_cast<short>(LOWORD(lp));
            const int y = static_cast<short>(HIWORD(lp));
            if (x < 0 || y < 0 || x >= s->width || y >= s->height)
                ClosePopupMenu(hwnd, s, 0);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (s)
        {
            const int x = static_cast<short>(LOWORD(lp));
            const int y = static_cast<short>(HIWORD(lp));
            const int hit = HitPopupItem(*s, x, y);
            if (hit >= 0)
                ClosePopupMenu(hwnd, s, (*s->items)[static_cast<std::size_t>(hit)].id);
            else if (x < 0 || y < 0 || x >= s->width || y >= s->height)
                ClosePopupMenu(hwnd, s, 0);
        }
        return 0;
    case WM_KEYDOWN:
        if (s)
        {
            if (wp == VK_ESCAPE) { ClosePopupMenu(hwnd, s, 0); return 0; }
            if (wp == VK_DOWN) { StepPopupHover(*s, hwnd, 1); return 0; }
            if (wp == VK_UP) { StepPopupHover(*s, hwnd, -1); return 0; }
            if ((wp == VK_RETURN || wp == VK_SPACE) && s->hover >= 0)
            {
                ClosePopupMenu(hwnd, s, (*s->items)[static_cast<std::size_t>(s->hover)].id);
                return 0;
            }
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && s)
        {
            ClosePopupMenu(hwnd, s, 0);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (s && reinterpret_cast<HWND>(lp) != hwnd)
        {
            ClosePopupMenu(hwnd, s, 0);
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace

int ShowPopupMenu(HWND owner, int screenX, int screenY, int minWidth,
                  const std::vector<PopupMenuItem>& items, HFONT font)
{
    if (items.empty()) return 0;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr const wchar_t* className = L"FlareLauncherPopupMenu";
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = PopupMenuProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = className;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 0;

    const UINT dpi = owner ? GetDpiForWindow(owner) : 96u;
    const auto sc = [dpi](int dip) { return MulDiv(dip, dpi ? static_cast<int>(dpi) : 96, 96); };
    HFONT useFont = font;
    if (!useFont && owner) useFont = reinterpret_cast<HFONT>(SendMessageW(owner, WM_GETFONT, 0, 0));
    if (!useFont) useFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    PopupMenuState state;
    state.items = &items;
    state.font = useFont;
    state.pad = sc(5);
    const int rowH = sc(32);
    const int sepH = sc(9);
    const int textPadX = sc(12);

    int maxLabelW = 0;
    const HDC screenDc = GetDC(owner);
    const HDC measureDc = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
    if (measureDc)
    {
        const HGDIOBJ oldFont = SelectObject(measureDc, useFont);
        for (const auto& item : items)
        {
            if (item.separator) continue;
            RECT r{0, 0, 0, 0};
            DrawTextW(measureDc, item.label.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            maxLabelW = std::max<int>(maxLabelW, r.right - r.left);
        }
        SelectObject(measureDc, oldFont);
        DeleteDC(measureDc);
    }
    if (screenDc) ReleaseDC(owner, screenDc);

    state.width = std::max(minWidth, maxLabelW + 2 * textPadX + 2 * state.pad + sc(8));
    int curY = state.pad;
    state.itemRects.reserve(items.size());
    for (const auto& item : items)
    {
        const int h = item.separator ? sepH : rowH;
        state.itemRects.push_back(RECT{state.pad, curY, state.width - state.pad, curY + h});
        curY += h;
    }
    state.height = curY + state.pad;

    POINT anchor{screenX, screenY};
    MONITORINFO mi{sizeof(mi)};
    const HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi))
    {
        if (anchor.x + state.width > mi.rcWork.right) anchor.x = std::max<LONG>(mi.rcWork.left, mi.rcWork.right - state.width);
        if (anchor.y + state.height > mi.rcWork.bottom) anchor.y = std::max<LONG>(mi.rcWork.top, anchor.y - state.height);
        if (anchor.x < mi.rcWork.left) anchor.x = mi.rcWork.left;
        if (anchor.y < mi.rcWork.top) anchor.y = mi.rcWork.top;
    }

    HWND popup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, className, L"", WS_POPUP,
                                 anchor.x, anchor.y, state.width, state.height, owner, nullptr, instance, &state);
    if (!popup) return 0;

    ShowWindow(popup, SW_SHOW);
    UpdateWindow(popup);
    SetFocus(popup);
    SetCapture(popup);

    MSG msg{};
    bool sawQuit = false;
    int quitCode = 0;
    while (!state.closed && IsWindow(popup))
    {
        const BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret <= 0)
        {
            if (ret == 0) { sawQuit = true; quitCode = static_cast<int>(msg.wParam); }
            ClosePopupMenu(popup, &state, 0);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(popup)) DestroyWindow(popup);
    if (owner && IsWindow(owner)) SetForegroundWindow(owner);
    if (sawQuit) PostQuitMessage(quitCode);
    return state.selectedId;
}
} // namespace flare::launcher_style
