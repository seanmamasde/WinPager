#pragma once
#include "vdapi.h"

struct Win {
    HWND hwnd;
    RECT rect;  // visible frame bounds (physical px); restored rect when minimized
    int desktop;  // index into Model::desktops, -1 = shown on all desktops
    bool minimized;
    HMONITOR monitor;
};

struct Model {
    std::vector<vd::Desktop> desktops;
    int current = 0;
    std::vector<Win> windows;  // z-order, topmost first
    HWND active = nullptr;

    // Each returns true when something the pager draws changed.
    bool refreshDesktops(vd::Api& api, HRESULT& hr);
    bool refreshWindows(const vd::Api& api);
    bool refreshGeometry();

    int indexOf(const GUID& id) const;
    std::wstring desktopName(int i) const;
    static std::wstring title(HWND hwnd);
};
