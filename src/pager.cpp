#include "pager.h"

#include "app.h"

#include <dwmapi.h>
#include <ole2.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kPagerClass[] = L"WinPager";
constexpr wchar_t kTipClass[] = L"WinPager.Tooltip";
constexpr wchar_t kOsdClass[] = L"WinPager.OSD";
constexpr float kRadius = 4;  // taskbar item corner radius (Taskbar.View AEP/button style), logical px
constexpr float kGap = 2;     // space between desktop rects, logical px
enum : UINT_PTR { kTimerTip = 1, kTimerDrop, kTimerOsd };
constexpr ULONGLONG kOsdHold = 1000;  // KWin PopupHideDelay default
constexpr float kOsdCrossfade = 500;  // osd.qml: PopupHideDelay / 2
// Windows motion tokens ("Motion in Windows" on learn.microsoft.com): Direct Entrance / Direct Exit move along
// cubic-bezier(0,0,0,1) in 333 / 167 ms, always paired with the 83 ms linear fade.
constexpr float kOsdEnter = 333, kOsdExit = 167, kOsdFade = 83, kOsdRise = 24;

D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}

D2D1_ROUNDED_RECT rounded(const FRect& r, float inset, float radius) {
    const float rad = std::max(0.f, radius - inset);
    return {{r.l + inset, r.t + inset, r.r - inset, r.b - inset}, rad, rad};
}

enum class Frame { Normal, Hover, Current };

// Desktop frame states of Breeze's widgets/pager.svg. Normal stays KDE's text 10 %; hover and current keep KDE's
// "light fill, stronger edge" shape, but current is a tint with a crisp accent edge instead of a 60 % accent block,
// which outweighed everything else on the Windows taskbar.
void drawFrame(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const Palette& p, const FRect& r, float rad, Frame f, float opacity = 1) {
    if (f == Frame::Normal) {
        br->SetColor(alpha(p.text, .1f * opacity));
        rt->FillRoundedRectangle(rounded(r, 0, rad), br);
        return;
    }
    const bool hover = f == Frame::Hover;
    br->SetColor(alpha(p.highlight, (hover ? .1f : .22f) * opacity));
    rt->FillRoundedRectangle(rounded(r, 1, rad), br);
    br->SetColor(alpha(p.highlight, (hover ? .5f : 1.f) * opacity));
    rt->DrawRoundedRectangle(rounded(r, .5f, rad), br, 1.f);
}

void registerClass(HINSTANCE inst, const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = name;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);  // fails harmlessly once registered
}

ComPtr<IDWriteTextFormat> textFormat(IDWriteFactory* f, const wchar_t* family, DWRITE_FONT_WEIGHT weight, float px, bool center) {
    ComPtr<IDWriteTextFormat> tf;
    if (FAILED(f->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", &tf)))
        return nullptr;
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteInlineObject> ellipsis;
    f->CreateEllipsisTrimmingSign(tf.Get(), &ellipsis);
    const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    tf->SetTrimming(&trim, ellipsis.Get());
    if (center) {
        tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    return tf;
}

template <class T>
T* self(HWND h, UINT msg, LPARAM lp) {
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    return reinterpret_cast<T*>(GetWindowLongPtrW(h, GWLP_USERDATA));
}

}  // namespace

bool Surface::ensure(int cw, int ch) {
    if (dc && cw == w && ch == h) return true;
    release();
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), cw, -ch, 1, 32, BI_RGB};
    void* bits = nullptr;
    dc = CreateCompatibleDC(nullptr);
    bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp) {
        release();
        return false;
    }
    old = SelectObject(dc, bmp);
    w = cw;
    h = ch;
    return true;
}

void Surface::release() {
    if (dc) {
        if (old) SelectObject(dc, old);
        DeleteDC(dc);
    }
    if (bmp) DeleteObject(bmp);
    dc = nullptr;
    bmp = nullptr;
    old = nullptr;
    w = h = 0;
}

// ---------------------------------------------------------------- Tooltip

Tooltip::~Tooltip() {
    if (hwnd_) DestroyWindow(hwnd_);
}

LRESULT CALLBACK Tooltip::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* t = self<Tooltip>(h, msg, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (t && t->surf_.dc) BitBlt(dc, 0, 0, t->surf_.w, t->surf_.h, t->surf_.dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCACTIVATE:  // system backdrops go flat on inactive windows, and this one never activates
        return DefWindowProcW(h, msg, TRUE, lp);
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Tooltip::restyle() {
    if (!hwnd_) return;
    const BOOL dark = app_.palette.dark;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
}

void Tooltip::hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    desktop_ = -1;
}

void Tooltip::show(const RECT& anchor, const RECT& bar, int desktop, HMONITOR filter, float scale) {
    if (!hwnd_) {
        registerClass(app_.inst, kTipClass, proc);
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kTipClass, L"",
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, app_.inst, this);
        if (!hwnd_) return;
        // Same recipe as the shell's own flyouts: DWM-rounded, acrylic system backdrop, content drawn with alpha.
        const MARGINS glass{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &glass);
        const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
        const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TRANSIENTWINDOW;
        DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof backdrop);
        DefWindowProcW(hwnd_, WM_NCACTIVATE, TRUE, 0);
        restyle();
    }
    anchor_ = anchor;
    bar_ = bar;
    desktop_ = desktop;
    filter_ = filter;
    scale_ = scale;
    render();
}

void Tooltip::render() {
    const Model& m = app_.model;
    if (desktop_ < 0 || desktop_ >= static_cast<int>(m.desktops.size())) return hide();

    std::vector<std::wstring> shown, minimized;
    for (const Win& w : m.windows)
        if ((w.desktop == desktop_ || w.desktop < 0) && (!filter_ || w.monitor == filter_))
            (w.minimized ? minimized : shown).push_back(Model::title(w.hwnd));
    const auto lines = tooltipLines(shown, minimized);

    const float s = scale_;
    IDWriteFactory* f = app_.dwrite.Get();
    const auto titleFmt = textFormat(f, L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14 * s, false);
    const auto bodyFmt = textFormat(f, L"Segoe UI Variable Small", DWRITE_FONT_WEIGHT_NORMAL, 12 * s, false);
    if (!titleFmt || !bodyFmt) return;
    const float padX = std::round(11 * s), padT = std::round(7 * s), padB = std::round(9 * s);
    const float maxW = std::round(400 * s), indent = std::round(6 * s);

    struct Line {
        ComPtr<IDWriteTextLayout> layout;
        float x, y;
        bool body;
    };
    std::vector<Line> laid;
    float y = padT, width = 0;
    auto add = [&](const std::wstring& text, IDWriteTextFormat* fmt, float x, bool body) {
        ComPtr<IDWriteTextLayout> l;
        const std::wstring t = text.empty() ? L" " : text;
        if (FAILED(f->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), fmt, maxW - (x - padX), 1e4f, &l))) return;
        DWRITE_TEXT_METRICS tm{};
        l->GetMetrics(&tm);
        laid.push_back({l, x, y, body});
        width = std::max(width, x - padX + tm.widthIncludingTrailingWhitespace);
        y += tm.height;
    };
    add(m.desktopName(desktop_), titleFmt.Get(), padX, false);
    if (!lines.empty()) y += std::round(3 * s);
    for (const auto& ln : lines) add(ln.bullet ? L"•  " + ln.text : ln.text, bodyFmt.Get(), ln.bullet ? padX + indent : padX, true);

    const int W = static_cast<int>(std::ceil(std::min(width, maxW) + 2 * padX));
    const int H = static_cast<int>(std::ceil(y + padB));
    if (!surf_.ensure(W, H)) return;

    ID2D1DCRenderTarget* rt = app_.rt.Get();
    ID2D1SolidColorBrush* br = app_.brush.Get();
    const RECT rc{0, 0, W, H};
    if (FAILED(rt->BindDC(surf_.dc, &rc))) return;
    const bool dark = app_.palette.dark;
    // WinUI TextFillColorPrimary / TextFillColorSecondary
    const auto primary = dark ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(0, 0, 0, .894f);
    const auto secondary = dark ? D2D1::ColorF(1, 1, 1, .786f) : D2D1::ColorF(0, 0, 0, .62f);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (const auto& l : laid) {
        br->SetColor(l.body ? secondary : primary);
        rt->DrawTextLayout({l.x, l.y}, l.layout.Get(), br);
    }
    rt->EndDraw();

    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST), &mi);
    const int gap = static_cast<int>(std::round(8 * s));
    const int x = std::clamp(static_cast<int>((anchor_.left + anchor_.right) / 2 - W / 2), static_cast<int>(mi.rcMonitor.left) + gap,
                             std::max(static_cast<int>(mi.rcMonitor.left) + gap, static_cast<int>(mi.rcMonitor.right) - W - gap));
    const bool barAtBottom = bar_.top > (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
    const int top = barAtBottom ? bar_.top - gap - H : bar_.bottom + gap;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, top, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- Osd

Osd::~Osd() {
    if (hwnd_) DestroyWindow(hwnd_);
}

LRESULT CALLBACK Osd::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* o = self<Osd>(h, msg, lp);
    switch (msg) {
    case WM_TIMER:
        if (o) o->tick();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (o && o->surf_.dc) BitBlt(dc, 0, 0, o->surf_.w, o->surf_.h, o->surf_.dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCACTIVATE:  // system backdrops go flat on inactive windows, and this one never activates
        return DefWindowProcW(h, msg, TRUE, lp);
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Osd::show(int from, int to) {
    const int n = static_cast<int>(app_.model.desktops.size());
    if (to < 0 || to >= n) return;
    if (!hwnd_) {  // output only, like KWin's: click-through and never activated
        registerClass(app_.inst, kOsdClass, proc);
        // Layered only for the whole-window fade: DWM still draws the system backdrop and fades it along.
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kOsdClass,
                                L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, app_.inst, this);
        if (!hwnd_) return;
        SetLayeredWindowAttributes(hwnd_, 0, 0, LWA_ALPHA);
        const MARGINS glass{-1, -1, -1, -1};  // content is drawn with alpha over the backdrop
        DwmExtendFrameIntoClientArea(hwnd_, &glass);
        const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
        const DWM_SYSTEMBACKDROP_TYPE mica = DWMSBT_MAINWINDOW;
        DwmSetWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE, &mica, sizeof mica);
        DefWindowProcW(hwnd_, WM_NCACTIVATE, TRUE, 0);
        restyle();
    }
    const bool visible = phase_ != Phase::Hidden;
    const ULONGLONG now = GetTickCount64();
    level_.resize(n, 0.f);
    if (!visible) {  // the previous desktop's frame fades out while the new one fades in
        std::ranges::fill(level_, 0.f);
        if (from >= 0 && from < n) level_[from] = 1;
        last_ = now;
        POINT pt;
        GetCursorPos(&pt);
        monitor_ = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);  // the active screen; kept while visible, as in KWin
    }
    current_ = to;
    hideAt_ = now + kOsdHold;
    layout();
    render();
    if (phase_ == Phase::Hidden || phase_ == Phase::Out) {  // (re)enter from wherever the exit got to
        alphaFrom_ = alpha_;
        riseFrom_ = visible ? rise_ : kOsdRise * scale_;
        rise_ = riseFrom_;
        phase_ = Phase::In;
        phaseStart_ = now;
    }
    place();
    if (!visible) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(hwnd_);
        SetTimer(hwnd_, kTimerOsd, USER_TIMER_MINIMUM, nullptr);
    }
}

void Osd::restyle() {
    if (!hwnd_) return;
    const BOOL dark = app_.palette.dark;  // dark or light Mica, like the taskbar
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    if (phase_ != Phase::Hidden) render();
}

void Osd::place() {
    const BYTE a = static_cast<BYTE>(std::lround(alpha_ * 255));
    const int y = y_ + static_cast<int>(std::lround(rise_));
    const RECT r{x_, y, x_ + w_, y + h_};
    if (a != placedAlpha_) SetLayeredWindowAttributes(hwnd_, 0, a, LWA_ALPHA);
    if (!EqualRect(&r, &placed_)) SetWindowPos(hwnd_, nullptr, x_, y, w_, h_, SWP_NOZORDER | SWP_NOACTIVATE);
    placedAlpha_ = a;
    placed_ = r;
}

void Osd::layout() {
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(monitor_, &mi);
    UINT dpi = 96, dpiY = 96;
    GetDpiForMonitor(monitor_, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
    if (dpi / 96.f != scale_ || !heading_) {
        scale_ = dpi / 96.f;
        heading_ = textFormat(app_.dwrite.Get(), L"Segoe UI Variable Display", DWRITE_FONT_WEIGHT_NORMAL, 20 * scale_, true);
    }
    const RECT& m = mi.rcMonitor;
    const int sw = m.right - m.left, sh = m.bottom - m.top, n = std::max(1, static_cast<int>(level_.size()));
    // osd.qml: itemWidth = screenWidth * min(0.8 / columns, 0.1), itemHeight keeps the screen's aspect ratio
    cellW_ = static_cast<int>(sw * std::min(0.8 / n, 0.1));
    cellH_ = static_cast<int>(std::min(cellW_ * double(sh) / sw, sh * 0.1));
    const auto px = [this](float v) { return static_cast<int>(std::lround(v * scale_)); };
    gap_ = px(4);
    pad_ = px(12);
    head_ = px(36);  // 28 px heading line + 8 px space
    w_ = n * cellW_ + (n - 1) * gap_ + 2 * pad_;
    h_ = head_ + cellH_ + 2 * pad_;
    x_ = (m.left + m.right - w_) / 2;
    y_ = (m.top + m.bottom - h_) / 2;
}

void Osd::render() {
    if (!surf_.ensure(w_, h_)) return;
    const Palette& p = app_.palette;
    ID2D1DCRenderTarget* rt = app_.rt.Get();
    ID2D1SolidColorBrush* br = app_.brush.Get();
    const RECT rc{0, 0, w_, h_};
    if (FAILED(rt->BindDC(surf_.dc, &rc))) return;
    const FRect panel{0, 0, static_cast<float>(w_), static_cast<float>(h_)};
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));  // DWM draws the Mica panel, its corners and edge behind this
    const float pad = static_cast<float>(pad_);
    if (heading_) {
        const std::wstring name = app_.model.desktopName(current_);
        br->SetColor(p.dark ? D2D1::ColorF(1, 1, 1) : D2D1::ColorF(0, 0, 0, .894f));
        rt->DrawText(name.c_str(), static_cast<UINT32>(name.size()), heading_.Get(),
                     {panel.l + pad, panel.t + pad, panel.r - pad, panel.t + pad + std::round(28 * scale_)}, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    const float cellRad = std::round(kRadius * scale_);
    for (size_t i = 0; i < level_.size(); ++i) {
        const float x = panel.l + pad + static_cast<float>(i * (cellW_ + gap_)), y = panel.t + pad + head_;
        const FRect cell{x, y, x + cellW_, y + cellH_};
        drawFrame(rt, br, p, cell, cellRad, Frame::Normal);
        if (level_[i] > 0) drawFrame(rt, br, p, cell, cellRad, Frame::Current, level_[i]);
    }
    rt->EndDraw();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Osd::tick() {
    const ULONGLONG now = GetTickCount64();
    const float dt = static_cast<float>(now - last_);
    last_ = now;
    bool redraw = false;
    for (size_t i = 0; i < level_.size(); ++i) {  // osd.qml: Behavior on opacity, linear
        const float target = static_cast<int>(i) == current_ ? 1.f : 0.f, step = dt / kOsdCrossfade;
        const float v = level_[i] < target ? std::min(target, level_[i] + step) : std::max(target, level_[i] - step);
        redraw |= v != level_[i];
        level_[i] = v;
    }
    if ((phase_ == Phase::In || phase_ == Phase::Hold) && now >= hideAt_) {  // exit from wherever the entrance got to
        alphaFrom_ = alpha_;
        riseFrom_ = rise_;
        phase_ = Phase::Out;
        phaseStart_ = now;
    }
    const float t = static_cast<float>(now - phaseStart_);
    if (phase_ == Phase::In) {  // rise into place while fading in
        alpha_ = alphaFrom_ + (1 - alphaFrom_) * std::min(1.f, t / kOsdFade);
        rise_ = riseFrom_ * (1 - fluentDecelerate(t / kOsdEnter));
        if (t >= kOsdEnter) phase_ = Phase::Hold;
    } else if (phase_ == Phase::Out) {  // drop back down while fading out
        alpha_ = alphaFrom_ * (1 - std::min(1.f, t / kOsdFade));
        rise_ = riseFrom_ + (kOsdRise * scale_ - riseFrom_) * fluentDecelerate(t / kOsdExit);
        if (t >= kOsdExit) {  // idle again: no timer, no window
            KillTimer(hwnd_, kTimerOsd);
            ShowWindow(hwnd_, SW_HIDE);
            phase_ = Phase::Hidden;
            alpha_ = 0;
            return;
        }
    }
    if (redraw) render();
    place();
}

// ---------------------------------------------------------------- Pager

// KDE: holding a drag (file, text, ...) over a desktop for a second switches to it. Never accepts the drop.
class DropTarget final : public IDropTarget {
    std::atomic<ULONG> ref_{1};
    Pager& pager_;

public:
    explicit DropTarget(Pager& p) : pager_(p) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDropTarget)) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP DragEnter(IDataObject*, DWORD, POINTL pt, DWORD* effect) override { return over(pt, effect); }
    STDMETHODIMP DragOver(DWORD, POINTL pt, DWORD* effect) override { return over(pt, effect); }
    STDMETHODIMP DragLeave() override {
        pager_.dragHover(nullptr);
        return S_OK;
    }
    STDMETHODIMP Drop(IDataObject*, DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        pager_.dragHover(nullptr);
        return S_OK;
    }

private:
    HRESULT over(POINTL pt, DWORD* effect) {
        *effect = DROPEFFECT_NONE;
        pager_.dragHover(&pt);
        return S_OK;
    }
};

Pager::Pager(App& app, HWND taskbar) : app_(app), taskbar_(taskbar), tip_(app) {
    wchar_t cls[32];
    primary_ = GetClassNameW(taskbar, cls, 32) && !wcscmp(cls, L"Shell_TrayWnd");
    registerClass(app.inst, kPagerClass, proc);
    // NOPARENTNOTIFY: no WM_PARENTNOTIFY sent into explorer, which could hang us, on create, destroy and clicks
    hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOPARENTNOTIFY, kPagerClass, L"WinPager", WS_CHILD | WS_CLIPSIBLINGS, 0, 0,
                            1, 1, taskbar_, nullptr, app.inst, this);
    if (!hwnd_) return;  // taskbar in a shell band right now; App retries
    drop_.Attach(new DropTarget(*this));
    RegisterDragDrop(hwnd_, drop_.Get());
    update(true, true);
    ShowWindow(hwnd_, SW_SHOWNA);
}

Pager::~Pager() {
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
}

void Pager::dragHover(const POINTL* screen) {
    int d = -1;
    if (screen) {
        POINT p{screen->x, screen->y};
        ScreenToClient(hwnd_, &p);
        d = desktopAt(p);
    }
    if (d == dropDesktop_) return;
    dropDesktop_ = d;
    KillTimer(hwnd_, kTimerDrop);
    if (d >= 0) SetTimer(hwnd_, kTimerDrop, 1000, nullptr);
    tip_.hide();
    render();
}

LRESULT CALLBACK Pager::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = self<Pager>(h, msg, lp);
    if (!p) return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_NCCREATE) p->hwnd_ = h;
    return p->handle(msg, wp, lp);
}

// Explorer keeps its hidden legacy Start window over the XAML Start button, but its old layout code also keeps parking it
// at the taskbar's leading edge, where it usually stays on a secondary taskbar: remember where it last sat over the button.
// shortcut: a move that Explorer undoes before this thread reads it is missed until the next one; read the XAML Start
// button through UI Automation if that shows.
void Pager::trackStart() {
    RECT r;
    if (HWND st = FindWindowExW(taskbar_, nullptr, L"Start", nullptr); st && GetWindowRect(st, &r) && r.left < r.right) {
        MapWindowPoints(nullptr, taskbar_, reinterpret_cast<POINT*>(&r), 2);
        if (r.left > 0) startLeft_ = r.left;
    }
}

void Pager::layout() {
    const Settings& s = app_.settings;
    RECT tb{};
    GetClientRect(taskbar_, &tb);
    const float scale = GetDpiForWindow(taskbar_) / 96.f;
    if (scale != scale_ || !label_) {
        scale_ = scale;
        label_ = textFormat(app_.dwrite.Get(), L"Segoe UI Variable Small", DWRITE_FONT_WEIGHT_NORMAL, 12 * scale_, true);
    }
    monitor_ = MonitorFromWindow(taskbar_, MONITOR_DEFAULTTONEAREST);
    if (app_.perScreen) {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(monitor_, &mi);
        area_ = mi.rcMonitor;
    } else {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        area_ = {vx, vy, vx + GetSystemMetrics(SM_CXVIRTUALSCREEN), vy + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    }
    count_ = std::max(1, static_cast<int>(app_.model.desktops.size()));

    const int H = tb.bottom - tb.top;
    const int margin = static_cast<int>(std::lround(4 * scale_));
    itemH_ = std::max(8, H - 2 * margin);
    const double ratio = double(area_.right - area_.left) / std::max(1L, area_.bottom - area_.top);
    itemW_ = std::max(8, static_cast<int>(itemH_ * ratio));
    gap_ = std::max(1, static_cast<int>(std::lround(kGap * scale_)));
    h_ = itemH_;
    y_ = (H - h_) / 2;
    auto advanced = [](const wchar_t* name, DWORD def) {
        DWORD v = def, cb = sizeof v;
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", name, RRF_RT_REG_DWORD,
                     nullptr, &v, &cb);
        return v;
    };
    // Make room for the Widgets entry point (an AEP button in explorer's taskbar XAML, primary taskbar only): it takes
    // the leading edge of a centered taskbar and sits beside the tray on a left-aligned one; margins 6,0,8,0, max 152 px.
    const bool centered = advanced(L"TaskbarAl", 1) != 0;  // missing = centered (Windows 11 default)
    const bool widgets = primary_ && advanced(L"TaskbarDa", 1) != 0;
    const int aep = widgets ? static_cast<int>(std::lround((6 + 152 + 8) * scale_)) : 0;
    const int pad = static_cast<int>(std::lround(6 * scale_));
    const bool leading = s.position == 0 && centered;
    LONG right = tb.right;  // where the tray starts
    if (HWND tray = FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr)) {
        RECT r;
        GetWindowRect(tray, &r);
        MapWindowPoints(nullptr, taskbar_, reinterpret_cast<POINT*>(&r), 2);
        right = r.left;
    }
    shown_ = s.maxDesktops > 0 ? std::min(count_, s.maxDesktops) : count_;
    // The taskbar only makes room for its own items (the Widgets button's fixed slot), so beside a centered row of
    // buttons show just the desktops that fit.
    trackStart();
    if (centered && startLeft_) {
        // shortcut: the row's right end is mirrored from its left end, which is off once a crowded row gets pushed right
        // by the Widgets button; read the buttons through UI Automation if the tray-side position needs that case.
        const int room = leading ? startLeft_ - 2 * pad - aep : right - (tb.right - startLeft_) - 2 * pad;
        shown_ = std::min(shown_, fittingDesktops(room, itemW_, gap_));
    }
    w_ = shown_ * itemW_ + (shown_ - 1) * gap_;
    x_ = leading ? pad + aep : right - pad - w_ - (centered ? 0 : aep);
    SetWindowPos(hwnd_, HWND_TOP, x_, y_, w_, h_, SWP_NOACTIVATE);
}

void Pager::restyle() {
    tip_.restyle();
    label_.Reset();
    update(true, true);
}

void Pager::update(bool relayout, bool redraw) {
    if (!alive()) return;
    if (relayout || count_ != std::max(1, static_cast<int>(app_.model.desktops.size()))) {
        layout();
        redraw = true;
    }
    if (const int first = visibleFirst(first_, app_.currentDesktop(), shown_, count_); first != first_) {
        first_ = first;  // scrolled to keep the current desktop in view
        tip_.hide();
        redraw = true;
    }
    if (!redraw) return;
    if (hover_ < first_ || hover_ >= first_ + shown_) hover_ = -1;
    if (drag_.pressed && drag_.hwnd &&
        std::ranges::none_of(app_.model.windows, [&](const Win& w) { return w.hwnd == drag_.hwnd; }))
        endDrag(false);
    render();
    tip_.refresh();
}

FRect Pager::itemRect(int i) const {
    const float x = static_cast<float>((i - first_) * (itemW_ + gap_));
    return {x, 0, x + itemW_, static_cast<float>(itemH_)};
}

bool Pager::shows(const Win& w, int desktop) const {
    return (w.desktop == desktop || w.desktop < 0) && (!app_.perScreen || w.monitor == monitor_);
}

int Pager::desktopAt(POINT p) const {
    if (p.x < 0 || p.y < 0 || p.y >= h_) return -1;
    const int i = p.x / (itemW_ + gap_);
    return i < shown_ && p.x - i * (itemW_ + gap_) < itemW_ ? first_ + i : -1;
}

const Win* Pager::windowAt(int desktop, POINT p) const {
    const FRect item = itemRect(desktop);
    for (const Win& w : app_.model.windows) {  // topmost first
        if (w.minimized || !shows(w, desktop)) continue;
        const FRect r = mapWindowRect(w.rect, area_, item);
        if (p.x >= r.l && p.x < r.r && p.y >= r.t && p.y < r.b) return &w;
    }
    return nullptr;
}

void Pager::drawWindow(const Win& w, const FRect& r, bool currentDesktop) {
    if (r.r - r.l < 1 || r.b - r.t < 1) return;
    ID2D1DCRenderTarget* rt = app_.rt.Get();
    ID2D1SolidColorBrush* br = app_.brush.Get();
    const D2D1_COLOR_F text = app_.palette.text;
    const bool active = w.hwnd == app_.model.active;
    // KDE's fills; on a light taskbar black at 60 % turns a maximized window into the heaviest thing on the bar,
    // so fills are lightened there (outline borders stay KDE's).
    const float k = app_.palette.dark ? 1.f : .55f;
    br->SetColor(alpha(text, k * (currentDesktop ? (active ? .6f : .35f) : (active ? .5f : .17f))));
    rt->FillRectangle({r.l, r.t, r.r, r.b}, br);
    br->SetColor(alpha(text, active ? 1.f : .5f));
    rt->DrawRectangle({r.l + .5f, r.t + .5f, r.r - .5f, r.b - .5f}, br, 1.f);
    if (!app_.settings.showWindowIcons) return;
    const float avail = std::min(r.r - r.l, r.b - r.t), sp = 4 * scale_;
    const float size = std::floor(std::min({32 * scale_, avail, std::max(avail - 2 * sp, 2 * sp)}));
    if (size < 6) return;
    if (ID2D1Bitmap* icon = app_.icon(w.hwnd)) {
        const float x = std::round((r.l + r.r - size) / 2), y = std::round((r.t + r.b - size) / 2);
        rt->DrawBitmap(icon, {x, y, x + size, y + size}, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
}

void Pager::render() {
    if (!alive() || w_ <= 0 || h_ <= 0 || !surf_.ensure(w_, h_)) return;
    const Model& m = app_.model;
    const Settings& s = app_.settings;
    const Palette& p = app_.palette;
    ID2D1DCRenderTarget* rt = app_.rt.Get();
    ID2D1SolidColorBrush* br = app_.brush.Get();
    const RECT rc{0, 0, w_, h_};
    if (FAILED(rt->BindDC(surf_.dc, &rc))) return;

    const float rad = std::round(kRadius * scale_);
    const int current = app_.currentDesktop();
    const Win* dragged = nullptr;
    rt->BeginDraw();
    // alpha 1/255 keeps gaps hit-testable so the tooltip doesn't flicker between rects
    rt->Clear(D2D1::ColorF(0, 0, 0, 1 / 255.f));
    for (int i = first_; i < first_ + shown_; ++i) {
        const FRect r = itemRect(i);
        const bool isCurrent = i == current;
        if (s.showWindowOutlines) {
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            app_.d2d->CreateRoundedRectangleGeometry(rounded(r, 1, rad), &clip);
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), app_.layer.Get());
            for (auto it = m.windows.rbegin(); it != m.windows.rend(); ++it) {  // bottom of the stack first
                if (it->minimized || !shows(*it, i)) continue;
                if (drag_.active && it->hwnd == drag_.hwnd) {
                    dragged = &*it;
                    continue;
                }
                drawWindow(*it, mapWindowRect(it->rect, area_, r), isCurrent);
            }
            rt->PopLayer();
        }
        // KDE state priority: hover > active > normal.
        const bool hover = drag_.active ? drag_.target == i : dropDesktop_ >= 0 ? dropDesktop_ == i : hover_ == i;
        drawFrame(rt, br, p, r, rad, hover ? Frame::Hover : isCurrent ? Frame::Current : Frame::Normal);
        if (s.displayedText != 2 && label_) {
            const std::wstring t = s.displayedText == 0 ? std::to_wstring(i + 1) : m.desktopName(i);
            br->SetColor(p.text);
            rt->DrawText(t.c_str(), static_cast<UINT32>(t.size()), label_.Get(), {r.l + 2, r.t + 1, r.r - 2, r.b - 1}, br,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    if (dragged) drawWindow(*dragged, drag_.rect, drag_.desktop == current);
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) return;

    POINT src{0, 0};
    SIZE size{w_, h_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, surf_.dc, &src, 0, &blend, ULW_ALPHA);
}

void Pager::setHover(int i) {
    if (i == hover_) return;
    hover_ = i;
    render();
    KillTimer(hwnd_, kTimerTip);
    if (i < 0 || tipSuppressed_) return tip_.hide();
    if (tip_.visible()) return showTip(i);
    UINT ms = 400;
    SystemParametersInfoW(SPI_GETMOUSEHOVERTIME, 0, &ms, 0);
    SetTimer(hwnd_, kTimerTip, ms, nullptr);
}

void Pager::showTip(int i) {
    const FRect r = itemRect(i);
    RECT a{static_cast<LONG>(r.l), static_cast<LONG>(r.t), static_cast<LONG>(r.r), static_cast<LONG>(r.b)};
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&a), 2);
    RECT bar;
    GetWindowRect(taskbar_, &bar);
    tip_.show(a, bar, i, app_.perScreen ? monitor_ : nullptr, scale_);
}

void Pager::endDrag(bool drop) {
    const Drag d = drag_;
    drag_ = {};
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (drop && d.active && d.target >= 0 && d.target != d.desktop) app_.moveWindow(d.hwnd, d.target);
    render();
}

LRESULT Pager::handle(UINT msg, WPARAM wp, LPARAM lp) {
    const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    switch (msg) {
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
            tracking_ = TrackMouseEvent(&t);
        }
        if (!drag_.pressed) {
            setHover(desktopAt(pt));
            return 0;
        }
        const LONG dx = pt.x - drag_.start.x, dy = pt.y - drag_.start.y;
        if (!drag_.active && drag_.hwnd && (std::abs(dx) > 1 || std::abs(dy) > 1)) drag_.active = true;  // KDE: threshold 1
        if (drag_.active) {
            const FRect& o = drag_.origin;
            const float w = o.r - o.l, h = o.b - o.t;
            const float x = std::clamp(o.l + dx, 0.f, std::max(0.f, w_ - w));
            const float y = std::clamp(o.t + dy, 0.f, std::max(0.f, h_ - h));
            drag_.rect = {x, y, x + w, y + h};
            drag_.target = desktopAt({static_cast<LONG>(x + w / 2), static_cast<LONG>(y + h / 2)});
            render();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        tipSuppressed_ = false;
        if (!drag_.pressed) setHover(-1);
        return 0;
    case WM_LBUTTONDOWN: {
        KillTimer(hwnd_, kTimerTip);
        tip_.hide();
        tipSuppressed_ = true;
        const int d = desktopAt(pt);
        if (d < 0) return 0;
        SetCapture(hwnd_);
        drag_ = {};
        drag_.pressed = true;
        drag_.desktop = d;
        drag_.start = pt;
        if (const Win* w = app_.settings.showWindowOutlines ? windowAt(d, pt) : nullptr) {
            drag_.hwnd = w->hwnd;
            drag_.origin = drag_.rect = mapWindowRect(w->rect, area_, itemRect(d));
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!drag_.pressed) return 0;
        const bool click = !drag_.active && desktopAt(pt) == drag_.desktop;
        const int d = drag_.desktop;
        endDrag(true);
        if (click) app_.switchTo(d);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (drag_.pressed && reinterpret_cast<HWND>(lp) != hwnd_) endDrag(false);
        return 0;
    case WM_RBUTTONUP: {
        POINT s = pt;
        ClientToScreen(hwnd_, &s);
        tip_.hide();
        app_.contextMenu(s);
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        wheel_ += msg == WM_MOUSEHWHEEL ? -delta : delta;
        int steps = 0;
        for (; wheel_ >= WHEEL_DELTA; wheel_ -= WHEEL_DELTA) --steps;  // wheel up = previous desktop
        for (; wheel_ <= -WHEEL_DELTA; wheel_ += WHEEL_DELTA) ++steps;
        if (steps) {
            wheel_ = 0;
            const int n = static_cast<int>(app_.model.desktops.size()), cur = app_.currentDesktop();
            int t = cur;
            for (int i = 0; i < std::abs(steps); ++i) t = stepDesktop(t, n, steps > 0 ? 1 : -1, app_.settings.wrapPage);
            if (t != cur) app_.switchTo(t);
        }
        return msg == WM_MOUSEHWHEEL;
    }
    case WM_TIMER:
        KillTimer(hwnd_, wp);
        if (wp == kTimerTip && hover_ >= 0 && !tipSuppressed_) showTip(hover_);
        if (wp == kTimerDrop && dropDesktop_ >= 0 && dropDesktop_ != app_.currentDesktop())
            app_.switchTo(dropDesktop_, false);  // don't steal focus from the drag source
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DPICHANGED_AFTERPARENT:
        update(true, true);
        return 0;
    case WM_DESTROY:
        RevokeDragDrop(hwnd_);
        break;
    case WM_NCDESTROY: {
        const LRESULT r = DefWindowProcW(hwnd_, msg, wp, lp);
        hwnd_ = nullptr;
        app_.pagerDied();  // e.g. the taskbar went away with its monitor
        return r;
    }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
