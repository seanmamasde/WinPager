#include "settings.h"

#include "resource.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kKey[] = L"Software\\WinPager";
constexpr wchar_t kRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"WinPager";

}  // namespace

void Settings::load() {
    auto flag = [](const wchar_t* n, bool& v) { v = readDword(kKey, n, v) != 0; };
    auto choice = [](const wchar_t* n, int& v, int max) {
        const DWORD x = readDword(kKey, n, static_cast<DWORD>(v));
        if (x <= static_cast<DWORD>(max)) v = static_cast<int>(x);
    };
    auto hotkey = [](const wchar_t* n, Hotkey& h) {
        const DWORD x = readDword(kKey, n, MAKELONG(h.vk, h.mods));
        h = {HIWORD(x), LOWORD(x)};
    };
    flag(L"showWindowOutlines", showWindowOutlines);
    flag(L"showWindowIcons", showWindowIcons);
    flag(L"showOnlyCurrentScreen", showOnlyCurrentScreen);
    flag(L"wrapPage", wrapPage);
    choice(L"displayedText", displayedText, 2);
    choice(L"currentDesktopSelected", currentDesktopSelected, 2);
    choice(L"position", position, 1);
    choice(L"maxDesktops", maxDesktops, 99);
    choice(L"theme", theme, 2);
    flag(L"accentHighlight", accentHighlight);
    flag(L"switchOsd", switchOsd);
    flag(L"hideSystemOsd", hideSystemOsd);
    hotkey(L"moveLeft", moveLeft);
    hotkey(L"moveRight", moveRight);
}

void Settings::save() const {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr)) return;
    auto put = [k](const wchar_t* n, DWORD v) {
        RegSetValueExW(k, n, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
    };
    put(L"showWindowOutlines", showWindowOutlines);
    put(L"showWindowIcons", showWindowIcons);
    put(L"showOnlyCurrentScreen", showOnlyCurrentScreen);
    put(L"wrapPage", wrapPage);
    put(L"displayedText", displayedText);
    put(L"currentDesktopSelected", currentDesktopSelected);
    put(L"position", position);
    put(L"maxDesktops", maxDesktops);
    put(L"theme", theme);
    put(L"accentHighlight", accentHighlight);
    put(L"switchOsd", switchOsd);
    put(L"hideSystemOsd", hideSystemOsd);
    put(L"moveLeft", MAKELONG(moveLeft.vk, moveLeft.mods));
    put(L"moveRight", MAKELONG(moveRight.vk, moveRight.mods));
    RegCloseKey(k);
}

namespace {

bool startupEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRun, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void setStartup(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRun, 0, KEY_SET_VALUE, &k)) return;
    if (on) {
        wchar_t path[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        const std::wstring cmd = L"\"" + std::wstring(path, n) + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

bool appsDark() {  // app windows ("App mode"), which the dialog follows
    return readDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) == 0;
}

}  // namespace

// ---------------------------------------------------------------- settings dialog
// Real Win32 controls (keyboard navigation, screen readers, DPI scaling come free), painted
// through NM_CUSTOMDRAW in WinUI/Fluent colors so light and dark both look native.

namespace {

enum : UINT { WM_APP_KEY = WM_APP + 50, WM_APP_DPI };
enum : UINT_PTR { kTimerRecord = 1 };

constexpr int kGeneral[] = {IDC_LBL_GENERAL, IDC_OUTLINES, IDC_ICONS, IDC_CURSCREEN, IDC_WRAP, IDC_LBL_TEXT, IDC_TEXT0,
                            IDC_TEXT1, IDC_TEXT2, IDC_LBL_SELECT, IDC_SEL0, IDC_SEL1, IDC_SEL2};
constexpr int kWindows[] = {IDC_LBL_POS, IDC_POS0, IDC_POS1, IDC_LBL_MAXDESK, IDC_MAXDESK_LESS, IDC_MAXDESK,
                            IDC_MAXDESK_MORE, IDC_LBL_THEME, IDC_THEME0, IDC_THEME1, IDC_THEME2, IDC_LBL_COLORS,
                            IDC_ACCENT, IDC_LBL_STARTUP, IDC_STARTUP, IDC_LBL_SWITCH, IDC_SWITCHOSD, IDC_HIDESYSOSD};
constexpr int kShortcuts[] = {IDC_LBL_HK0, IDC_HK0, IDC_LBL_HK1, IDC_HK1, IDC_HK_NOTE};
constexpr int kAbout[] = {IDC_ABOUT_ICON, IDC_ABOUT_NAME, IDC_ABOUT_TEXT};
constexpr std::span<const int> kPages[] = {kGeneral, kWindows, kShortcuts, kAbout};
constexpr const wchar_t* kPageNames[] = {L"General", L"Windows", L"Keyboard Shortcuts", L"About"};
constexpr wchar_t kPageGlyphs[] = {0xE713, 0xE771, 0xE765, 0xE946};  // Segoe Fluent Icons: Setting, Personalize, Keyboard, Info

struct Colors {
    COLORREF bg, text, secondary, disabled, divider, strong, boxFill, button, buttonHover, buttonPressed, buttonBorder, accent,
        onAccent, selected;
};

// WinUI 3 theme resources flattened over the solid window background.
Colors colors(bool dark) {
    const COLORREF accent = accentColor(dark ? 1 : 4);
    if (dark)
        return {RGB(32, 32, 32), RGB(255, 255, 255), RGB(206, 206, 206), RGB(113, 113, 113), RGB(51, 51, 51), RGB(154, 154, 154), RGB(39, 39, 39),
                RGB(45, 45, 45), RGB(50, 50, 50), RGB(39, 39, 39), RGB(64, 64, 64), accent, RGB(0, 0, 0), RGB(45, 45, 45)};
    return {RGB(243, 243, 243), RGB(27, 27, 27), RGB(96, 96, 96), RGB(160, 160, 160), RGB(229, 229, 229), RGB(134, 134, 134), RGB(238, 238, 238),
            RGB(251, 251, 251), RGB(246, 246, 246), RGB(245, 245, 245), RGB(214, 214, 214), accent, RGB(255, 255, 255), RGB(234, 234, 234)};
}

struct Dialog {
    HWND hwnd = nullptr;
    Settings* target = nullptr;
    const bool* hotkeyOk = nullptr;
    void (*onApply)(void*) = nullptr;
    void* ctx = nullptr;
    Settings draft;
    int desktops = 0;
    bool startup = false;
    bool dark = false;
    Colors c{};
    HBRUSH bg = nullptr;
    HFONT titleFont = nullptr;
    HICON icon = nullptr;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<ID2D1DCRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
    UINT fontDpi = 0;
    ComPtr<IDWriteTextFormat> body, glyph, navGlyph;
    HHOOK hook = nullptr;
    int recording = -1;  // hotkey slot being recorded
    UINT held = 0;       // modifiers seen by the hook (swallowed keys never reach GetAsyncKeyState)
    bool finishing = false;
} g;

D2D1_COLOR_F d2(COLORREF c, float a = 1) {
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a);
}

ComPtr<IDWriteTextFormat> format(const wchar_t* family, float px) {
    ComPtr<IDWriteTextFormat> f;
    if (SUCCEEDED(g.dw->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         px, L"", &f))) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return f;
}

void ensureFonts(UINT dpi) {
    if (g.fontDpi == dpi) return;
    g.fontDpi = dpi;
    const float s = dpi / 96.f;
    g.body = format(L"Segoe UI", 12 * s);  // matches the template's 9pt dialog font
    g.glyph = format(L"Segoe Fluent Icons", 12 * s);
    g.navGlyph = format(L"Segoe Fluent Icons", 16 * s);
    if (g.glyph) g.glyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
}

void text(const wchar_t* s, IDWriteTextFormat* f, D2D1_RECT_F r, COLORREF c, DWRITE_TEXT_ALIGNMENT align) {
    if (!f) return;
    f->SetTextAlignment(align);
    g.br->SetColor(d2(c));
    g.rt->DrawText(s, static_cast<UINT32>(wcslen(s)), f, r, g.br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void focusRing(D2D1_RECT_F r, float s) {
    g.br->SetColor(d2(g.c.text));
    g.rt->DrawRoundedRectangle({{r.left + s, r.top + s, r.right - s, r.bottom - s}, 4 * s, 4 * s}, g.br.Get(), 2 * s);
}

void paintButton(const NMCUSTOMDRAW& cd) {
    HWND h = cd.hdr.hwndFrom;
    const LONG type = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    const float s = GetDpiForWindow(h) / 96.f;
    ensureFonts(GetDpiForWindow(h));
    RECT rc;
    GetClientRect(h, &rc);
    const D2D1_RECT_F r{0, 0, static_cast<float>(rc.right), static_cast<float>(rc.bottom)};
    const bool off = !IsWindowEnabled(h), hot = cd.uItemState & CDIS_HOT, down = cd.uItemState & CDIS_SELECTED;
    const bool ring = (cd.uItemState & CDIS_FOCUS) && (cd.uItemState & CDIS_SHOWKEYBOARDCUES);
    wchar_t label[128];
    GetWindowTextW(h, label, 128);
    if (FAILED(g.rt->BindDC(cd.hdc, &rc))) return;
    ID2D1DCRenderTarget* rt = g.rt.Get();
    ID2D1SolidColorBrush* br = g.br.Get();
    rt->BeginDraw();
    rt->Clear(d2(g.c.bg));
    if (type == BS_AUTOCHECKBOX || type == BS_AUTORADIOBUTTON) {
        const bool checked = Button_GetCheck(h) == BST_CHECKED;
        const float box = std::round(18 * s), y = std::round((r.bottom - box) / 2);
        const D2D1_RECT_F b{2 * s + .5f, y + .5f, 2 * s + box - .5f, y + box - .5f};
        const COLORREF fill = off ? g.c.disabled : g.c.accent;
        if (type == BS_AUTOCHECKBOX) {
            const D2D1_ROUNDED_RECT rr{b, 3 * s, 3 * s};
            if (checked) {
                br->SetColor(d2(fill, hot && !off ? .9f : 1));
                rt->FillRoundedRectangle(rr, br);
                text(L"\uE73E", g.glyph.Get(), b, g.c.onAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                br->SetColor(d2(hot ? g.c.buttonHover : g.c.boxFill));
                rt->FillRoundedRectangle(rr, br);
                br->SetColor(d2(off ? g.c.disabled : g.c.strong));
                rt->DrawRoundedRectangle(rr, br, 1);
            }
        } else {
            const D2D1_POINT_2F mid{(b.left + b.right) / 2, (b.top + b.bottom) / 2};
            const float rad = (b.right - b.left) / 2;
            if (checked) {
                br->SetColor(d2(fill));
                rt->FillEllipse({mid, rad, rad}, br);
                const float dot = rad * (down ? .42f : hot ? .58f : .5f);
                br->SetColor(d2(g.c.onAccent));
                rt->FillEllipse({mid, dot, dot}, br);
            } else {
                br->SetColor(d2(hot ? g.c.buttonHover : g.c.boxFill));
                rt->FillEllipse({mid, rad, rad}, br);
                br->SetColor(d2(off ? g.c.disabled : g.c.strong));
                rt->DrawEllipse({mid, rad, rad}, br, 1);
            }
        }
        const float tx = 2 * s + box + 8 * s;
        text(label, g.body.Get(), {tx, 0, r.right, r.bottom}, off ? g.c.disabled : g.c.text, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (ring) focusRing(r, s);
    } else {
        // Win32 makes whichever push button has focus the default; WinUI keeps the accent on OK and marks focus with the ring
        const bool primary = GetDlgCtrlID(h) == IDOK && !off;
        const D2D1_ROUNDED_RECT rr{{r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f}, 4 * s, 4 * s};
        if (primary) {
            br->SetColor(d2(g.c.accent, down ? .8f : hot ? .9f : 1));
            rt->FillRoundedRectangle(rr, br);
        } else {
            br->SetColor(d2(down ? g.c.buttonPressed : hot ? g.c.buttonHover : g.c.button));
            rt->FillRoundedRectangle(rr, br);
            br->SetColor(d2(g.c.buttonBorder));
            rt->DrawRoundedRectangle(rr, br, 1);
        }
        text(label, g.body.Get(), r, off ? g.c.disabled : primary ? g.c.onAccent : g.c.text, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (ring) focusRing({r.left - 1, r.top - 1, r.right + 1, r.bottom + 1}, s);
    }
    rt->EndDraw();
}

void paintNav(const DRAWITEMSTRUCT& di) {
    if (di.itemID >= std::size(kPageNames)) return;
    const float s = GetDpiForWindow(di.hwndItem) / 96.f;
    ensureFonts(GetDpiForWindow(di.hwndItem));
    const RECT& rc = di.rcItem;
    const D2D1_RECT_F r{0, 0, static_cast<float>(rc.right - rc.left), static_cast<float>(rc.bottom - rc.top)};
    if (FAILED(g.rt->BindDC(di.hDC, &rc))) return;
    g.rt->BeginDraw();
    g.rt->Clear(d2(g.c.bg));
    const D2D1_RECT_F item{r.left + 2 * s, r.top + 2 * s, r.right - 2 * s, r.bottom - 2 * s};
    if (di.itemState & ODS_SELECTED) {  // Windows 11 Settings-style selection: subtle fill + accent pill
        g.br->SetColor(d2(g.c.selected));
        g.rt->FillRoundedRectangle({item, 4 * s, 4 * s}, g.br.Get());
        const float mid = (r.top + r.bottom) / 2;
        g.br->SetColor(d2(g.c.accent));
        g.rt->FillRoundedRectangle({{item.left, mid - 8 * s, item.left + 3 * s, mid + 8 * s}, 1.5f * s, 1.5f * s}, g.br.Get());
    }
    text(std::wstring(1, kPageGlyphs[di.itemID]).c_str(), g.navGlyph.Get(), {item.left + 10 * s, r.top, item.left + 30 * s, r.bottom},
         g.c.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    text(kPageNames[di.itemID], g.body.Get(), {item.left + 38 * s, r.top, item.right, r.bottom}, g.c.text, DWRITE_TEXT_ALIGNMENT_LEADING);
    if ((di.itemState & ODS_FOCUS) && !(di.itemState & ODS_NOFOCUSRECT)) focusRing(item, s);
    g.rt->EndDraw();
}

bool extendedKey(UINT vk) {
    return (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE || vk == VK_NUMLOCK;
}

std::wstring hotkeyText(const Hotkey& h) {
    if (!h.vk) return L"None";
    std::wstring s;
    if (h.mods & MOD_CONTROL) s += L"Ctrl+";
    if (h.mods & MOD_ALT) s += L"Alt+";
    if (h.mods & MOD_SHIFT) s += L"Shift+";
    if (h.mods & MOD_WIN) s += L"Win+";
    wchar_t name[64] = L"?";
    GetKeyNameTextW(static_cast<LONG>((MapVirtualKeyW(h.vk, MAPVK_VK_TO_VSC) << 16) | (extendedKey(h.vk) ? 1u << 24 : 0u)), name, 64);
    return s + name;
}

UINT modifierOf(UINT vk) {
    switch (vk) {
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return MOD_CONTROL;
    case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return MOD_SHIFT;
    case VK_LMENU: case VK_RMENU: case VK_MENU: return MOD_ALT;
    case VK_LWIN: case VK_RWIN: return MOD_WIN;
    }
    return 0;
}

void showHotkeys() {
    Hotkey* keys[2] = {&g.draft.moveLeft, &g.draft.moveRight};
    for (int i = 0; i < 2; ++i)
        SetDlgItemTextW(g.hwnd, IDC_HK0 + 2 * i, g.recording == i ? L"Press a shortcut…" : hotkeyText(*keys[i]).c_str());
    std::wstring note = L"Moves the active window to the neighbouring virtual desktop and follows it there.\n\n"
                        L"Click a shortcut, then press the new key combination. Esc cancels, Backspace clears.";
    if (!g.hotkeyOk[0] || !g.hotkeyOk[1]) note += L"\n\nA shortcut couldn't be registered: another application already uses it.";
    SetDlgItemTextW(g.hwnd, IDC_HK_NOTE, note.c_str());
}

void showMaxDesktops() {
    const int v = g.draft.maxDesktops;
    SetDlgItemTextW(g.hwnd, IDC_MAXDESK, v ? std::to_wstring(v).c_str() : L"All");
    HWND less = GetDlgItem(g.hwnd, IDC_MAXDESK_LESS), more = GetDlgItem(g.hwnd, IDC_MAXDESK_MORE);
    // a button that just hit its end hands keyboard focus to the other one
    if (GetFocus() == less && v == 1) SendMessageW(g.hwnd, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(more), TRUE);
    if (GetFocus() == more && !v) SendMessageW(g.hwnd, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(less), TRUE);
    EnableWindow(less, v != 1);
    EnableWindow(more, v != 0);
}

// Swallows every key while recording so Win-combos don't reach the shell, then waits for modifiers to be released.
LRESULT CALLBACK recordProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION || !g.hook) return CallNextHookEx(nullptr, code, wp, lp);
    const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
    const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
    if (const UINT mod = modifierOf(k->vkCode)) {
        g.held = down ? g.held | mod : g.held & ~mod;
    } else if (down && !g.finishing) {
        PostMessageW(g.hwnd, WM_APP_KEY, k->vkCode, g.held);
    }
    if (g.finishing && !g.held) PostMessageW(g.hwnd, WM_APP_KEY, 0, 0);  // all released: unhook
    return 1;
}

void stopRecording() {
    if (g.hook) UnhookWindowsHookEx(g.hook);
    g.hook = nullptr;
    g.recording = -1;
    g.finishing = false;
    if (g.hwnd) {
        KillTimer(g.hwnd, kTimerRecord);
        showHotkeys();
    }
}

void startRecording(int slot) {
    stopRecording();
    g.recording = slot;
    g.held = 0;
    g.hook = SetWindowsHookExW(WH_KEYBOARD_LL, recordProc, GetModuleHandleW(nullptr), 0);
    if (!g.hook) g.recording = -1;
    SetTimer(g.hwnd, kTimerRecord, 10000, nullptr);  // never leave the keyboard captured
    showHotkeys();
}

void applyColors() {
    g.dark = g.draft.theme == 0 ? appsDark() : g.draft.theme == 2;
    g.c = colors(g.dark);
    if (g.bg) DeleteObject(g.bg);
    g.bg = CreateSolidBrush(g.c.bg);
    const BOOL dark = g.dark;
    DwmSetWindowAttribute(g.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void applyFonts() {
    const UINT dpi = GetDpiForWindow(g.hwnd);
    if (g.titleFont) DeleteObject(g.titleFont);
    g.titleFont = CreateFontW(-MulDiv(20, dpi, 96), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                              L"Segoe UI Variable Display");
    SetWindowFont(GetDlgItem(g.hwnd, IDC_TITLE), g.titleFont, TRUE);
    SetWindowFont(GetDlgItem(g.hwnd, IDC_ABOUT_NAME), g.titleFont, TRUE);
    ListBox_SetItemHeight(GetDlgItem(g.hwnd, IDC_NAV), 0, MulDiv(36, dpi, 96));
    if (g.icon) DestroyIcon(g.icon);
    const int size = MulDiv(48, dpi, 96);
    LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), size, size, &g.icon);
    HWND ic = GetDlgItem(g.hwnd, IDC_ABOUT_ICON);
    SetWindowPos(ic, nullptr, 0, 0, size, size, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(ic, STM_SETICON, reinterpret_cast<WPARAM>(g.icon), 0);
}

void toControls() {
    const Settings& d = g.draft;
    HWND h = g.hwnd;
    CheckDlgButton(h, IDC_OUTLINES, d.showWindowOutlines);
    CheckDlgButton(h, IDC_ICONS, d.showWindowIcons);
    CheckDlgButton(h, IDC_CURSCREEN, d.showOnlyCurrentScreen);
    CheckDlgButton(h, IDC_WRAP, d.wrapPage);
    CheckRadioButton(h, IDC_TEXT0, IDC_TEXT2, IDC_TEXT0 + d.displayedText);
    CheckRadioButton(h, IDC_SEL0, IDC_SEL2, IDC_SEL0 + d.currentDesktopSelected);
    CheckRadioButton(h, IDC_POS0, IDC_POS1, IDC_POS0 + d.position);
    CheckRadioButton(h, IDC_THEME0, IDC_THEME2, IDC_THEME0 + d.theme);
    CheckDlgButton(h, IDC_ACCENT, d.accentHighlight);
    CheckDlgButton(h, IDC_STARTUP, g.startup);
    CheckDlgButton(h, IDC_SWITCHOSD, d.switchOsd);
    CheckDlgButton(h, IDC_HIDESYSOSD, d.hideSystemOsd);
    EnableWindow(GetDlgItem(h, IDC_ICONS), d.showWindowOutlines);  // as in KDE
    showHotkeys();
    showMaxDesktops();
}

int checkedOf(int first, int last) {
    for (int id = first; id <= last; ++id)
        if (IsDlgButtonChecked(g.hwnd, id)) return id - first;
    return 0;
}

void updateApply() {
    EnableWindow(GetDlgItem(g.hwnd, IDC_APPLY), !(g.draft == *g.target) || g.startup != startupEnabled());
}

void fromControls() {
    Settings& d = g.draft;
    HWND h = g.hwnd;
    const int theme = d.theme;
    d.showWindowOutlines = IsDlgButtonChecked(h, IDC_OUTLINES);
    d.showWindowIcons = IsDlgButtonChecked(h, IDC_ICONS);
    d.showOnlyCurrentScreen = IsDlgButtonChecked(h, IDC_CURSCREEN);
    d.wrapPage = IsDlgButtonChecked(h, IDC_WRAP);
    d.displayedText = checkedOf(IDC_TEXT0, IDC_TEXT2);
    d.currentDesktopSelected = checkedOf(IDC_SEL0, IDC_SEL2);
    d.position = checkedOf(IDC_POS0, IDC_POS1);
    d.theme = checkedOf(IDC_THEME0, IDC_THEME2);
    d.accentHighlight = IsDlgButtonChecked(h, IDC_ACCENT);
    d.switchOsd = IsDlgButtonChecked(h, IDC_SWITCHOSD);
    d.hideSystemOsd = IsDlgButtonChecked(h, IDC_HIDESYSOSD);
    g.startup = IsDlgButtonChecked(h, IDC_STARTUP);
    EnableWindow(GetDlgItem(h, IDC_ICONS), d.showWindowOutlines);
    if (theme != d.theme) applyColors();  // live preview
    updateApply();
}

void apply() {
    *g.target = g.draft;
    if (g.startup != startupEnabled()) setStartup(g.startup);
    g.onApply(g.ctx);
    showHotkeys();
    updateApply();
}

void setPage(int page) {
    for (int p = 0; p < static_cast<int>(std::size(kPages)); ++p)
        for (int id : kPages[p]) ShowWindow(GetDlgItem(g.hwnd, id), p == page ? SW_SHOW : SW_HIDE);
    SetDlgItemTextW(g.hwnd, IDC_TITLE, kPageNames[page]);
    ListBox_SetCurSel(GetDlgItem(g.hwnd, IDC_NAV), page);
}

INT_PTR CALLBACK dialogProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        g.hwnd = h;
        HWND nav = GetDlgItem(h, IDC_NAV);
        for (const wchar_t* name : kPageNames) ListBox_AddString(nav, name);
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        SendMessageW(h, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP))));
        HICON smallIcon = nullptr;
        LoadIconWithScaleDown(inst, MAKEINTRESOURCEW(IDI_APP), GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), &smallIcon);
        SendMessageW(h, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        const DWM_SYSTEMBACKDROP_TYPE mica = DWMSBT_MAINWINDOW;
        DwmSetWindowAttribute(h, DWMWA_SYSTEMBACKDROP_TYPE, &mica, sizeof mica);
        SetDlgItemTextW(h, IDC_MAXDESK_LESS, L"\u2212");  // the .rc can't hold the minus sign
        SetDlgItemTextW(h, IDC_ABOUT_NAME, L"WinPager " WINPAGER_VERSION);
        SetDlgItemTextW(h, IDC_ABOUT_TEXT,
                        L"A KDE Plasma-style virtual desktop pager for the Windows 11 taskbar.\n\n"
                        L"•  Click a desktop to switch to it; click the current one for Task View.\n"
                        L"•  Scroll over WinPager to change desktops.\n"
                        L"•  Drag window outlines between desktops to move windows.\n"
                        L"•  Right-click for desktop and WinPager options.\n\n"
                        L"Options and behaviour follow Plasma's Pager widget (plasma-desktop, applets/pager). "
                        L"Uses the virtual desktop interfaces of Windows 11 24H2/25H2 (build 26100+).");
        applyFonts();
        toControls();
        applyColors();
        setPage(0);
        updateApply();
        SendMessageW(h, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);  // focus cues appear on keyboard use
        return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return reinterpret_cast<INT_PTR>(g.bg);
    case WM_CTLCOLORSTATIC: {
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        SetTextColor(reinterpret_cast<HDC>(wp), id == IDC_HK_NOTE || id == IDC_ABOUT_TEXT ? g.c.secondary : g.c.text);
        SetBkColor(reinterpret_cast<HDC>(wp), g.c.bg);
        return reinterpret_cast<INT_PTR>(g.bg);
    }
    case WM_PAINT: {  // KDE-style divider between the sidebar and the page
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT nav;
        GetWindowRect(GetDlgItem(h, IDC_NAV), &nav);
        MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&nav), 2);
        RECT client;
        GetClientRect(h, &client);
        const RECT line{nav.right + MulDiv(4, GetDpiForWindow(h), 96), 0, nav.right + MulDiv(4, GetDpiForWindow(h), 96) + 1, client.bottom};
        HBRUSH b = CreateSolidBrush(g.c.divider);
        FillRect(dc, &line, b);
        DeleteObject(b);
        EndPaint(h, &ps);
        return TRUE;
    }
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = MulDiv(36, GetDpiForWindow(h), 96);
        return TRUE;
    case WM_DRAWITEM:
        if (wp == IDC_NAV) paintNav(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));
        return TRUE;
    case WM_NOTIFY: {
        const auto* nm = reinterpret_cast<const NMHDR*>(lp);
        if (nm->code == NM_CUSTOMDRAW && nm->idFrom != IDC_NAV) {
            const auto* cd = reinterpret_cast<const NMCUSTOMDRAW*>(lp);
            if (cd->dwDrawStage == CDDS_PREPAINT) {
                paintButton(*cd);
                SetWindowLongPtrW(h, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                return TRUE;
            }
        }
        return FALSE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDC_NAV) {
            if (HIWORD(wp) == LBN_SELCHANGE) setPage(ListBox_GetCurSel(GetDlgItem(h, IDC_NAV)));
        } else if (id == IDOK || id == IDCANCEL) {
            if (id == IDOK && IsWindowEnabled(GetDlgItem(h, IDC_APPLY))) apply();
            DestroyWindow(h);
        } else if (id == IDC_APPLY) {
            apply();
        } else if (id == IDC_HK0 || id == IDC_HK1) {
            startRecording(id == IDC_HK1);
        } else if (id == IDC_MAXDESK_LESS || id == IDC_MAXDESK_MORE) {
            int& v = g.draft.maxDesktops;  // 1 … one less than there are desktops, then All
            if (id == IDC_MAXDESK_LESS) v = v ? std::max(1, v - 1) : std::max(1, g.desktops - 1);
            else v = v && v + 1 < g.desktops ? v + 1 : 0;
            showMaxDesktops();
            updateApply();
        } else if (HIWORD(wp) == BN_CLICKED) {
            fromControls();
        }
        return TRUE;
    }
    case WM_APP_KEY:
        if (!g.hook) return TRUE;
        if (!wp) {
            stopRecording();
        } else if (!g.finishing) {
            Hotkey& k = g.recording == 1 ? g.draft.moveRight : g.draft.moveLeft;
            const UINT vk = static_cast<UINT>(wp), mods = static_cast<UINT>(lp);
            if (vk == VK_BACK || vk == VK_DELETE) k = {0, 0};
            else if (vk != VK_ESCAPE && mods) k = {mods, vk};
            else if (vk != VK_ESCAPE) return TRUE;  // a bare key would hijack normal typing; keep listening
            g.finishing = true;
            if (!g.held) stopRecording();
            updateApply();
        }
        return TRUE;
    case WM_TIMER:
        if (wp == kTimerRecord) stopRecording();
        return TRUE;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && g.hook) stopRecording();
        return FALSE;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet")) applyColors();
        return FALSE;
    case WM_DPICHANGED:
        PostMessageW(h, WM_APP_DPI, 0, 0);  // after the dialog manager rescales controls and fonts
        return FALSE;
    case WM_APP_DPI:
        applyFonts();
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        return TRUE;
    case WM_DESTROY:
        stopRecording();
        if (g.titleFont) DeleteObject(g.titleFont);
        if (g.icon) DestroyIcon(g.icon);
        if (g.bg) DeleteObject(g.bg);
        g.titleFont = nullptr;
        g.icon = nullptr;
        g.bg = nullptr;
        g.hwnd = nullptr;
        return FALSE;
    }
    return FALSE;
}

}  // namespace

void showSettings(HWND owner, Settings& s, int desktops, const bool* hotkeyOk, void (*onApply)(void*), void* ctx) {
    if (g.hwnd) {
        ShowWindow(g.hwnd, SW_RESTORE);
        SetForegroundWindow(g.hwnd);
        return;
    }
    if (!g.rt) {
        // Opaque target so DirectWrite can use ClearType like the native controls.
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g.d2d.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(g.dw.GetAddressOf()))) ||
            FAILED(g.d2d->CreateDCRenderTarget(&props, &g.rt)) || FAILED(g.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g.br))) {
            g.rt.Reset();
            return;
        }
    }
    g.target = &s;
    g.draft = s;
    g.desktops = desktops;
    g.startup = startupEnabled();
    g.hotkeyOk = hotkeyOk;
    g.onApply = onApply;
    g.ctx = ctx;
    g.fontDpi = 0;
    if (!CreateDialogParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), owner, dialogProc, 0)) return;
    ShowWindow(g.hwnd, SW_SHOW);
    SetForegroundWindow(g.hwnd);
}

HWND settingsWindow() { return g.hwnd; }
