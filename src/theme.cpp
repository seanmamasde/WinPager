#include "settings.h"

namespace {

D2D1_COLOR_F toColor(COLORREF c) {
    return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f);
}

}  // namespace

DWORD readDword(const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = 0, cb = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS ? v : def;
}

bool systemDark() {
    return readDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", 1) == 0;
}

bool appsDark() {
    return readDword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) == 0;
}

COLORREF accentColor(int shade) {
    BYTE p[32];
    DWORD cb = sizeof p;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent", L"AccentPalette",
                     RRF_RT_REG_BINARY, nullptr, p, &cb) == ERROR_SUCCESS && cb >= 28)
        return RGB(p[shade * 4], p[shade * 4 + 1], p[shade * 4 + 2]);
    return readDword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFD47800) & 0xFFFFFF;  // ABGR
}

Palette pagerPalette(const Settings& s) {
    Palette p;
    p.dark = s.theme == 0 ? systemDark() : s.theme == 2;
    p.text = p.dark ? D2D1::ColorF(1, 1, 1) : D2D1::ColorF(0, 0, 0);
    // WinUI AccentFillColorDefault: AccentDark1 on light, AccentLight2 on dark
    p.highlight = s.accentHighlight ? toColor(accentColor(p.dark ? 1 : 4)) : p.text;
    return p;
}
