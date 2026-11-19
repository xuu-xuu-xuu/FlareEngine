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

namespace se::launcher_style
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
Gdiplus::Color Argb(flare::Color color, BYTE alpha)
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

void Fill(HDC dc, RECT rect, flare::Color color)
{
    SetDCBrushColor(dc, flare::Native(color));
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Frame(HDC dc, RECT rect, flare::Color color, int thickness)
{
    SetDCBrushColor(dc, flare::Native(color));
    const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    for (int i = 0; i < thickness; ++i)
    {
        FrameRect(dc, &rect, brush);
        InflateRect(&rect, -1, -1);
    }
}

void FillRound(HDC dc, RECT rect, flare::Color color, int radius, BYTE alpha)
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

void FrameRound(HDC dc, RECT rect, flare::Color color, int radius, float thickness, BYTE alpha)
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

void Glow(HDC dc, int x, int y, int radius, flare::Color color, BYTE alpha)
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

void FadeLine(HDC dc, RECT rect, flare::Color color, BYTE alpha)
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
    const auto background = onSurface ? flare::Surface
        : (message == WM_CTLCOLORSTATIC ? flare::Background : flare::Surface);
    SetTextColor(dc, flare::Native(muted ? flare::Muted : flare::Text));
    SetBkColor(dc, flare::Native(background));
    SetDCBrushColor(dc, flare::Native(background));
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
                launcher_style::FrameRound(dc, client, flare::Border, ScaleForWindow(hwnd, 10));
                const auto font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
                if (font) SelectObject(dc, font);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, flare::Native(flare::Muted));
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
                SetTextColor(dc, flare::Native(flare::Muted));
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

    HDC dc = item.hDC;
    const int saved = SaveDC(dc);
    RECT row = item.rcItem;
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
            const auto bg = active ? flare::Selected : (hovered && !disabled ? flare::Hover : flare::Surface);
            FillRound(dc, row, bg, radius);
            FrameRound(dc, row, active ? flare::Gold : (hovered ? flare::Silver : flare::Border), radius,
                       active ? 1.5f : 1.0f, active ? 230 : (hovered ? 120 : 255));
            if (active)
            {
                RECT bar{row.left + ScaleForWindow(item.hwndItem, 5), row.top + ScaleForWindow(item.hwndItem, 12),
                         row.left + ScaleForWindow(item.hwndItem, 8), row.bottom - ScaleForWindow(item.hwndItem, 12)};
                FillRound(dc, bar, flare::Gold, ScaleForWindow(item.hwndItem, 2));
            }

            const int padX = ScaleForWindow(item.hwndItem, 12);
            const int padY = ScaleForWindow(item.hwndItem, 8);
            const auto newline = text.find(L'\n');
            const std::wstring title(newline == std::wstring_view::npos ? text : text.substr(0, newline));
            const std::wstring desc(newline == std::wstring_view::npos ? std::wstring_view{} : text.substr(newline + 1));

            RECT titleRect{row.left + padX, row.top + padY, row.right - padX, row.top + padY + ScaleForWindow(item.hwndItem, 22)};
            if (boldFont) SelectObject(dc, boldFont);
            SetTextColor(dc, flare::Native(disabled ? flare::Disabled : flare::Heading));
            DrawTextW(dc, title.c_str(), -1, &titleRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

            if (active)
            {
                SetTextColor(dc, flare::Native(flare::Gold));
                DrawTextW(dc, L"●", -1, &titleRect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
            }

            if (!desc.empty())
            {
                if (baseFont) SelectObject(dc, baseFont);
                RECT descRect{row.left + padX, titleRect.bottom + ScaleForWindow(item.hwndItem, 3),
                              row.right - padX, row.bottom - padY};
                SetTextColor(dc, flare::Native(active ? flare::Text : flare::Muted));
                DrawTextW(dc, desc.c_str(), -1, &descRect, DT_WORDBREAK | DT_LEFT | DT_NOPREFIX);
            }
            RestoreDC(dc, saved);
            return true;
        }

        flare::Color bg = flare::Raised;
        flare::Color border = flare::Border;
        flare::Color textColor = flare::Heading;
        BYTE borderAlpha = 255;
        if (disabled)
        {
            bg = flare::Surface;
            textColor = flare::Disabled;
        }
        else if (role == ButtonRole::Primary)
        {
            // 黑字压香槟金：主操作唯一的高亮色块。
            constexpr flare::Color pressedGold{206, 160, 82};
            bg = selected ? pressedGold : (hovered ? flare::AccentHover : flare::Accent);
            border = focused ? flare::Heading : bg;
            textColor = flare::Background;
        }
        else if (role == ButtonRole::Danger)
        {
            bg = selected || hovered ? flare::Danger : flare::Raised;
            border = hovered ? flare::Danger : flare::Border;
        }
        else
        {
            bg = selected ? flare::Selected : (hovered ? flare::Hover : flare::Raised);
            border = focused ? flare::Gold : (hovered ? flare::Silver : flare::Border);
            if (hovered && !focused) borderAlpha = 110;
        }

        DrawThemeParentBackground(item.hwndItem, dc, &row);
        const int radius = ScaleForWindow(item.hwndItem, 6);
        FillRound(dc, row, bg, radius);
        FrameRound(dc, row, border, radius, 1.0f, borderAlpha);
        if (role == ButtonRole::Primary && boldFont && !disabled) SelectObject(dc, boldFont);
        SetTextColor(dc, flare::Native(textColor));
        InflateRect(&row, -ScaleForWindow(item.hwndItem, 10), 0);
        DrawTextW(dc, rawText, -1, &row, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        RestoreDC(dc, saved);
        return true;
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
                Fill(dc, row, flare::Background);
                RECT card = row;
                InflateRect(&card, -ScaleForWindow(item.hwndItem, 2), -ScaleForWindow(item.hwndItem, 2));

                const int hoverRow = static_cast<int>(reinterpret_cast<UINT_PTR>(GetPropW(item.hwndItem, L"FlareHoverRow")));
                const bool hovered = hoverRow == static_cast<int>(item.itemID) + 1;
                const bool available = parts[4] == L"1";

                const auto cardBg = selected ? flare::Selected : (hovered ? flare::Hover : flare::Surface);
                const int cardRadius = ScaleForWindow(item.hwndItem, 8);
                FillRound(dc, card, cardBg, cardRadius);
                FrameRound(dc, card, selected ? flare::Gold : (hovered ? flare::Silver : flare::Border), cardRadius,
                           1.0f, selected ? 200 : (hovered ? 110 : 255));

                if (selected || !available)
                {
                    RECT stripe{card.left + ScaleForWindow(item.hwndItem, 4), card.top + ScaleForWindow(item.hwndItem, 12),
                                card.left + ScaleForWindow(item.hwndItem, 7), card.bottom - ScaleForWindow(item.hwndItem, 12)};
                    FillRound(dc, stripe, !available ? flare::Warning : flare::Gold, ScaleForWindow(item.hwndItem, 2));
                }

                const int pad = ScaleForWindow(item.hwndItem, 10);
                const int tileSize = std::max(20, static_cast<int>((card.bottom - card.top) - 2 * ScaleForWindow(item.hwndItem, 9)));
                RECT tile{card.left + pad + ScaleForWindow(item.hwndItem, 4),
                          card.top + ((card.bottom - card.top) - tileSize) / 2,
                          card.left + pad + ScaleForWindow(item.hwndItem, 4) + tileSize,
                          card.top + ((card.bottom - card.top) + tileSize) / 2};
                const int tileRadius = ScaleForWindow(item.hwndItem, 7);
                FillRound(dc, tile, flare::Background, tileRadius);
                FrameRound(dc, tile, !available ? flare::Warning : flare::Gold, tileRadius, 1.0f, !available ? 255 : 90);

                wchar_t initial[2]{L'F', L'\0'};
                for (wchar_t ch : parts[0])
                {
                    if (!std::iswspace(ch)) { initial[0] = static_cast<wchar_t>(std::towupper(ch)); break; }
                }
                if (boldFont) SelectObject(dc, boldFont);
                SetTextColor(dc, flare::Native(!available ? flare::Warning : flare::Gold));
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
                FillRound(dc, badgeRect, !available ? flare::Danger : flare::Raised, badgeH / 2);
                FrameRound(dc, badgeRect, !available ? flare::Warning : flare::Border, badgeH / 2);
                SetTextColor(dc, flare::Native(!available ? flare::Heading : flare::Silver));
                DrawTextW(dc, badge.c_str(), -1, &badgeRect, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

                int nameRight = badgeRect.left - ScaleForWindow(item.hwndItem, 8);
                if (!date.empty())
                {
                    RECT dateRect{textLeft, badgeRect.top, nameRight, badgeRect.bottom};
                    SetTextColor(dc, flare::Native(selected ? flare::Silver : flare::Muted));
                    DrawTextW(dc, date.c_str(), -1, &dateRect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
                    RECT dateMeasure{0, 0, 300, 24};
                    DrawTextW(dc, date.c_str(), -1, &dateMeasure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                    nameRight -= (dateMeasure.right - dateMeasure.left) + ScaleForWindow(item.hwndItem, 12);
                }

                RECT nameRect{textLeft, card.top + ScaleForWindow(item.hwndItem, 5),
                              std::max(textLeft + 40, nameRight), midY + ScaleForWindow(item.hwndItem, 1)};
                if (boldFont) SelectObject(dc, boldFont);
                SetTextColor(dc, flare::Native(flare::Heading));
                DrawTextW(dc, name.c_str(), -1, &nameRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

                RECT pathRect{textLeft, midY + ScaleForWindow(item.hwndItem, 1),
                              rightEdge, card.bottom - ScaleForWindow(item.hwndItem, 5)};
                if (baseFont) SelectObject(dc, baseFont);
                SetTextColor(dc, flare::Native(selected ? flare::Text : flare::Muted));
                DrawTextW(dc, path.c_str(), -1, &pathRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_PATH_ELLIPSIS | DT_NOPREFIX);
            }
            else
            {
                Fill(dc, row, selected ? flare::Selected : flare::Surface);
                if (focused) Frame(dc, row, flare::Accent, 1);
                SetTextColor(dc, flare::Native(disabled ? flare::Disabled : flare::Heading));
                InflateRect(&row, -ScaleForWindow(item.hwndItem, 9), 0);
                DrawTextW(dc, label.c_str(), -1, &row, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
    }

    RestoreDC(dc, saved);
    return true;
}
} // namespace se::launcher_style
