#pragma once

#include <windows.h>
#include <commctrl.h>
#include <iterator>

namespace vst::client::ui {

inline constexpr COLORREF canvas = RGB(244, 242, 248), surface = RGB(255, 255, 255);
inline constexpr COLORREF ink = RGB(38, 28, 54), muted = RGB(91, 77, 105), edge = RGB(220, 213, 229);
inline constexpr COLORREF banner = RGB(37, 25, 53), banner_muted = RGB(219, 204, 235);
inline constexpr COLORREF accent = RGB(91, 54, 151), soft_accent = RGB(237, 228, 250);
inline constexpr COLORREF gold = RGB(230, 181, 102), positive = RGB(13, 100, 78), warning = RGB(160, 42, 69);
inline constexpr COLORREF row_alt = RGB(249, 247, 252), input = RGB(252, 250, 254);

inline bool HighContrast() noexcept {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON);
}

inline HBRUSH Brush(HDC dc, COLORREF color) {
    SetDCBrushColor(dc, color);
    return static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
}
inline void Fill(HDC dc, const RECT& rect, COLORREF color) {
    FillRect(dc, &rect, Brush(dc, color));
}
inline void Panel(HDC dc, const RECT& rect, int radius, COLORREF fill = surface, COLORREF stroke = edge) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    auto brush = SelectObject(dc, Brush(dc, fill));
    auto pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, stroke);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, pen);
    SelectObject(dc, brush);
}

enum class ButtonRole { Normal, Primary, Danger, Active, Tab, Language, LanguageSelected };
struct ButtonColors { COLORREF fill, stroke, text; };

inline ButtonColors Colors(ButtonRole role, bool disabled, bool hot, bool pressed) {
    if (role == ButtonRole::Tab || role == ButtonRole::Primary) {
        if (disabled && role != ButtonRole::Tab) return {soft_accent, edge, muted};
        return {pressed ? RGB(71, 39, 124) : hot ? RGB(112, 69, 176) : accent, accent, surface};
    }
    if (role == ButtonRole::LanguageSelected) return {soft_accent, soft_accent, accent};
    if (role == ButtonRole::Language) return {hot || pressed ? RGB(75, 53, 99) : RGB(54, 39, 72), RGB(113, 87, 141), banner_muted};
    if (role == ButtonRole::Active) return {hot || pressed ? RGB(202, 235, 220) : RGB(227, 245, 236), RGB(174, 218, 198), positive};
    if (disabled) return {row_alt, edge, muted};
    if (role == ButtonRole::Danger) return {hot || pressed ? RGB(249, 219, 229) : RGB(253, 238, 244), RGB(238, 199, 215), warning};
    return {pressed || hot ? soft_accent : surface, pressed || hot ? accent : edge, pressed || hot ? accent : ink};
}

inline void DrawButton(const NMCUSTOMDRAW& draw, ButtonRole role, int radius, int padding, HFONT font) {
    HWND button = draw.hdr.hwndFrom;
    const UINT state = draw.uItemState;
    const auto colors = Colors(role, !IsWindowEnabled(button), (state & CDIS_HOT) != 0, (state & CDIS_SELECTED) != 0);
    int saved = SaveDC(draw.hdc);
    RECT rect = draw.rc;
    Panel(draw.hdc, rect, radius, colors.fill, colors.stroke);
    rect.left += padding; rect.right -= padding;
    SetBkMode(draw.hdc, TRANSPARENT);
    SetTextColor(draw.hdc, colors.text);
    SelectObject(draw.hdc, font);
    wchar_t text[160]{};
    GetWindowTextW(button, text, static_cast<int>(std::size(text)));
    DrawTextW(draw.hdc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if ((state & CDIS_FOCUS) && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT focus = draw.rc;
        InflateRect(&focus, -padding / 2, -padding / 2);
        DrawFocusRect(draw.hdc, &focus);
    }
    RestoreDC(draw.hdc, saved);
}

} // namespace vst::client::ui
