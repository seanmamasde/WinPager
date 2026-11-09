#pragma once
// Pure helpers (no Win32 calls) shared by the app and tests/selftest.cpp.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

// Fluent "Fast Out, Slow In" easing, cubic-bezier(0, 0, 0, 1). With both x control points at 0, x(u) = u^3, so the
// curve has a closed form: y(t) = 3 t^(2/3) - 2 t.
inline float fluentDecelerate(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return 3 * std::cbrt(t * t) - 2 * t;
}

// KDE wheel / shortcut navigation: returns current when there is nowhere to go.
inline int stepDesktop(int current, int count, int delta, bool wrap) {
    if (count <= 0) return current;
    const int t = current + delta;
    if (wrap) return ((t % count) + count) % count;
    return t < 0 || t >= count ? current : t;
}

// First desktop of a `shown`-wide window onto `count` desktops: scrolls from `first` only as far as needed to keep
// `current` in view.
inline int visibleFirst(int first, int current, int shown, int count) {
    first = std::clamp(first, 0, std::max(0, count - shown));
    if (shown < 1 || current < 0 || current >= count) return first;
    return std::clamp(first, current - shown + 1, current);
}

// Desktop rects `item` px wide, `gap` px apart, that fit in `room` px; at least one (the current desktop).
inline int fittingDesktops(int room, int item, int gap) {
    return std::max(1, (room + gap) / std::max(1, item + gap));
}

// Windhawk's hotkey setting, "[ctrl+][alt+][shift+][win+]<virtual-key code>" ("ctrl+shift+win+37"), as {MOD_* flags, vk};
// vk 0 = none.
inline std::pair<UINT, UINT> parseHotkey(const wchar_t* s) {
    UINT mods = 0;
    if (wcsstr(s, L"ctrl+")) mods |= MOD_CONTROL;
    if (wcsstr(s, L"alt+")) mods |= MOD_ALT;
    if (wcsstr(s, L"shift+")) mods |= MOD_SHIFT;
    if (wcsstr(s, L"win+")) mods |= MOD_WIN;
    const wchar_t* key = wcsrchr(s, L'+');
    return {mods, static_cast<UINT>(_wtoi(key ? key + 1 : s))};
}

struct FRect {
    float l, t, r, b;
};

// KDE windowmodel/main.qml: geometry relative to the shown area, scaled, rounded, clamped to the item.
inline FRect mapWindowRect(const RECT& win, const RECT& area, const FRect& item) {
    const float sx = (item.r - item.l) / float(area.right - area.left);
    const float sy = (item.b - item.t) / float(area.bottom - area.top);
    const LONG l = std::max(win.left, area.left), t = std::max(win.top, area.top);
    const LONG r = std::min(win.right, area.right), b = std::min(win.bottom, area.bottom);
    const float x = item.l + std::round((l - area.left) * sx);
    const float y = item.t + std::round((t - area.top) * sy);
    return {x, y, x + std::round((r - l) * sx), y + std::round((b - t) * sy)};
}

struct TipLine {
    std::wstring text;
    bool bullet = false;
};

// KDE pager tooltip sub text: a single title, or "N Windows:" + up to 4 bullets
// (5 when exactly 5, so "…and 1 other" never replaces a line), then minimized windows.
inline std::vector<TipLine> tooltipLines(const std::vector<std::wstring>& visible, const std::vector<std::wstring>& minimized) {
    std::vector<TipLine> out;
    auto list = [&](const std::vector<std::wstring>& w) {
        const size_t max = w.size() == 5 ? 5 : 4;
        for (size_t i = 0; i < w.size() && i < max; ++i) out.push_back({w[i], true});
        if (w.size() > max) {
            const size_t rest = w.size() - max;
            out.push_back({L"\u2026and " + std::to_wstring(rest) + (rest == 1 ? L" other window" : L" other windows")});
        }
    };
    if (visible.size() == 1) {
        out.push_back({visible[0]});
    } else if (visible.size() > 1) {
        out.push_back({std::to_wstring(visible.size()) + L" Windows:"});
        list(visible);
    }
    if (!visible.empty() && !minimized.empty()) out.push_back({});
    if (!minimized.empty()) {
        out.push_back({std::to_wstring(minimized.size()) + (minimized.size() == 1 ? L" Minimized Window:" : L" Minimized Windows:")});
        list(minimized);
    }
    return out;
}
