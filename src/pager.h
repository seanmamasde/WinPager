#pragma once
#include "logic.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

class App;
struct Win;

// 32bpp premultiplied DIB selected into a memory DC.
struct Surface {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;
    Surface() = default;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    ~Surface() { release(); }
    bool ensure(int cw, int ch);
    void release();
};

class Tooltip {
public:
    explicit Tooltip(App& app) : app_(app) {}
    ~Tooltip();
    void show(const RECT& anchor, const RECT& bar, int desktop, HMONITOR filter, float scale);
    void hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    int desktop() const { return desktop_; }
    void refresh() { if (visible()) render(); }
    void restyle();

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    void render();

    App& app_;
    HWND hwnd_ = nullptr;
    int desktop_ = -1;
    HMONITOR filter_ = nullptr;
    RECT anchor_{}, bar_{};
    float scale_ = 1;
    Surface surf_;
};

// One pager, embedded as a child of a taskbar window (Shell_TrayWnd / Shell_SecondaryTrayWnd).
class Pager {
public:
    Pager(App& app, HWND taskbar);
    ~Pager();
    HWND taskbar() const { return taskbar_; }
    bool alive() const { return hwnd_ && IsWindow(hwnd_) && IsWindow(taskbar_); }
    void update(bool relayout, bool redraw);
    void restyle();
    void raise() {
        if (alive() && GetWindow(taskbar_, GW_CHILD) != hwnd_)
            SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void layout();
    void render();
    void drawWindow(const Win& w, const FRect& r, bool currentDesktop);
    FRect itemRect(int i) const;
    bool shows(const Win& w, int desktop) const;
    int desktopAt(POINT p) const;
    const Win* windowAt(int desktop, POINT p) const;
    void setHover(int i);
    void showTip(int i);
    void endDrag(bool drop);
    void dragHover(const POINTL* screen);  // OLE drag over the pager (nullptr = left)
    friend class DropTarget;

    App& app_;
    HWND hwnd_ = nullptr;
    HWND taskbar_;
    bool primary_ = false;  // Shell_TrayWnd (hosts the Widgets button) vs Shell_SecondaryTrayWnd
    HMONITOR monitor_ = nullptr;
    RECT area_{};  // screen area each desktop rect represents
    float scale_ = 1;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0, itemW_ = 0, itemH_ = 0, gap_ = 1, count_ = 0;
    int first_ = 0, shown_ = 1;  // desktops drawn: shown_ of the count_, from first_ on
    int hover_ = -1;
    int wheel_ = 0;
    bool tracking_ = false;
    bool tipSuppressed_ = false;
    struct Drag {
        bool pressed = false, active = false;
        int desktop = -1, target = -1;
        HWND hwnd = nullptr;
        POINT start{};
        FRect origin{}, rect{};
    } drag_;
    int dropDesktop_ = -1;
    Surface surf_;
    Tooltip tip_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> label_;
    Microsoft::WRL::ComPtr<IDropTarget> drop_;
};

// KWin's desktop change OSD (kwin src/plugins/desktopchangeosd/osd.qml) without the direction arrows: the new desktop's
// name over a row of desktops whose active frame cross-fades from the previous one; hides 1 s after the last switch.
// The surface is a Windows 11 one: Mica, DWM corners, and Fluent entrance/exit motion.
class Osd {
public:
    explicit Osd(App& app) : app_(app) {}
    ~Osd();
    void show(int from, int to);
    void restyle();

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    void layout();
    void render();
    void tick();
    void place();  // window opacity and position for the current animation state

    App& app_;
    HWND hwnd_ = nullptr;
    HMONITOR monitor_ = nullptr;
    std::vector<float> level_;  // active frame opacity per desktop
    int current_ = -1;
    enum class Phase { Hidden, In, Hold, Out } phase_ = Phase::Hidden;
    ULONGLONG last_ = 0, hideAt_ = 0, phaseStart_ = 0;
    float alpha_ = 0, rise_ = 0, alphaFrom_ = 0, riseFrom_ = 0;  // rise: px below the resting position
    BYTE placedAlpha_ = 0;
    RECT placed_{};
    float scale_ = 0;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0, cellW_ = 0, cellH_ = 0, gap_ = 0, pad_ = 0, head_ = 0;
    Surface surf_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> heading_;
};
