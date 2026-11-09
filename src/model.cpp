#include "model.h"

#include <dwmapi.h>

namespace {

bool same(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool same(const std::vector<Win>& a, const std::vector<Win>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].hwnd != b[i].hwnd || !same(a[i].rect, b[i].rect) || a[i].desktop != b[i].desktop ||
            a[i].minimized != b[i].minimized)
            return false;
    return true;
}

RECT frameRect(HWND h, bool minimized) {
    RECT r{};
    if (minimized) {
        WINDOWPLACEMENT wp{sizeof wp};
        GetWindowPlacement(h, &wp);
        // shortcut: workspace coords, off by the taskbar size only when it sits at the top; good enough for screen matching.
        return wp.rcNormalPosition;
    }
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(h, &r);
    return r;
}

// Alt+Tab / taskbar membership rules.
bool isCandidate(HWND h, DWORD selfPid, HWND shell) {
    if (h == shell || !IsWindowVisible(h)) return false;
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_APPWINDOW)) {
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
        if (GetWindow(h, GW_OWNER)) return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == selfPid) return false;
    wchar_t cls[32];
    return !(GetClassNameW(h, cls, 32) && wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0);
}

}  // namespace

int Model::indexOf(const GUID& id) const {
    for (size_t i = 0; i < desktops.size(); ++i)
        if (desktops[i].id == id) return static_cast<int>(i);
    return -1;
}

std::wstring Model::desktopName(int i) const {
    if (i < 0 || i >= static_cast<int>(desktops.size())) return {};
    if (!desktops[i].name.empty()) return desktops[i].name;
    return L"Desktop " + std::to_wstring(i + 1);
}

std::wstring Model::title(HWND hwnd) {
    std::wstring s(static_cast<size_t>(GetWindowTextLengthW(hwnd)) + 1, L'\0');
    s.resize(static_cast<size_t>(GetWindowTextW(hwnd, s.data(), static_cast<int>(s.size()))));
    return s;
}

bool Model::refreshDesktops(vd::Api& api, HRESULT& hr) {
    std::vector<vd::Desktop> next;
    int cur = 0;
    hr = api.desktops(next, cur);
    if (FAILED(hr)) return false;
    bool changed = cur != current || next.size() != desktops.size();
    for (size_t i = 0; !changed && i < next.size(); ++i)
        changed = next[i].id != desktops[i].id || next[i].name != desktops[i].name;
    desktops.swap(next);
    current = cur;
    return changed;
}

bool Model::refreshWindows(const vd::Api& api) {
    std::vector<HWND> all;
    all.reserve(512);
    EnumWindows([](HWND h, LPARAM p) {
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&all));

    const DWORD self = GetCurrentProcessId();
    const HWND shell = GetShellWindow();
    std::vector<Win> next;
    next.reserve(64);
    for (HWND h : all) {
        if (!isCandidate(h, self, shell)) continue;
        const bool minimized = IsIconic(h);
        const RECT r = frameRect(h, minimized);
        if (r.right <= r.left || r.bottom <= r.top) continue;
        DWORD cloaked = 0;
        DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
        const int d = indexOf(api.windowDesktop(h));
        // Windows on other desktops are shell-cloaked; anything else cloaked is not really shown.
        if (cloaked && (d < 0 || d == current || !(cloaked & DWM_CLOAKED_SHELL))) continue;
        next.push_back({h, r, d, minimized, MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST)});
    }

    const HWND fg = GetForegroundWindow();
    const HWND act = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    const bool changed = act != active || !same(next, windows);
    windows.swap(next);
    active = act;
    return changed;
}

bool Model::refreshGeometry() {
    bool changed = false;
    for (auto& w : windows) {
        const bool minimized = IsIconic(w.hwnd);
        const RECT r = frameRect(w.hwnd, minimized);
        if (minimized != w.minimized || !same(r, w.rect)) {
            changed = true;
            w.rect = r;
            w.minimized = minimized;
            w.monitor = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        }
    }
    return changed;
}
