#include "app.h"

#include "logic.h"
#include "pager.h"
#include "resource.h"

#include <shellapi.h>
#include <shlobj.h>
// clang-format off
#include <initguid.h>
#include <propkey.h>
// clang-format on

#include <algorithm>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {

App* g_app = nullptr;

enum : UINT_PTR { kTimerRefresh = 1, kTimerReinit, kTimerTaskbars, kTimerActivate, kTimerSlide };
enum : unsigned { kDesktops = 1, kWindows = 2, kGeometry = 4, kLayout = 8, kTaskbars = 16, kTitles = 32 };
enum : int { kHotkeyLeft = 1, kHotkeyRight = 2 };
enum : UINT { IDM_ADD = 1, IDM_REMOVE, IDM_CONFIGURE_VD, IDM_SETTINGS, IDM_EXIT };

// uxtheme ordinals 135/136 (SetPreferredAppMode / FlushMenuThemes)
void setMenuTheme(bool dark) {
    enum class Mode { Default, AllowDark, ForceDark, ForceLight };
    using SetMode = Mode(WINAPI*)(Mode);
    using Flush = void(WINAPI*)();
    static const HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static const auto set = reinterpret_cast<SetMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    static const auto flush = reinterpret_cast<Flush>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (set) set(dark ? Mode::ForceDark : Mode::ForceLight);
    if (flush) flush();
}

// ask the shell for the app's tile icon via its AUMID (UWP apps)
HBITMAP packagedAppIcon(HWND h) {
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&store)))) return nullptr;
    PROPVARIANT v;
    PropVariantInit(&v);
    HBITMAP bmp = nullptr;
    ComPtr<IShellItemImageFactory> f;
    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR &&
        SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, v.pwszVal, IID_PPV_ARGS(&f))))
        f->GetImage({32, 32}, SIIGBF_ICONONLY, &bmp);
    PropVariantClear(&v);
    return bmp;
}

// shared OSD host from explorer (from modernflyout repo)
// ZBID_ABOVELOCK_UX
bool isSystemOsd(HWND h) {
    using GetBand = BOOL(WINAPI*)(HWND, DWORD*);
    static const auto getBand = reinterpret_cast<GetBand>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowBand"));
    constexpr DWORD ZBID_ABOVELOCK_UX = 18;
    DWORD pid = 0, shell = 0, band = 0;
    GetWindowThreadProcessId(h, &pid);
    GetWindowThreadProcessId(GetShellWindow(), &shell);
    wchar_t cls[32];
    return pid == shell && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"XamlExplorerHostIslandWindow") && getBand &&
           getBand(h, &band) && band == ZBID_ABOVELOCK_UX;
}

bool shellOverlayActive() {
    wchar_t cls[32];
    const HWND f = GetForegroundWindow();
    return f && GetClassNameW(f, cls, 32) && !wcscmp(cls, L"XamlExplorerHostIslandWindow");
}

void pressWinTab() {
    const WORD keys[4] = {VK_LWIN, VK_TAB, VK_TAB, VK_LWIN};
    INPUT in[4]{};
    for (int i = 0; i < 4; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = keys[i];
        in[i].ki.wScan = static_cast<WORD>(MapVirtualKeyW(keys[i], MAPVK_VK_TO_VSC));  // RDP sessions drop scan code 0
        in[i].ki.dwFlags = (i >= 2 ? KEYEVENTF_KEYUP : 0) | (keys[i] == VK_LWIN ? KEYEVENTF_EXTENDEDKEY : 0);
    }
    SendInput(4, in, sizeof(INPUT));
}

}  // namespace

App::App() = default;
App::~App() = default;

int App::run(HINSTANCE hinst) {
    g_app = this;
    inst = hinst;
    settings.load();
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))) ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !createTarget())
        return 1;

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = kControllerClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassExW(&wc);
    if (!CreateWindowExW(WS_EX_TOOLWINDOW, kControllerClass, L"WinPager", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, this))
        return 1;
    msgTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    applyTheme();
    initApi();
    static constexpr std::pair<DWORD, DWORD> kEvents[] = {
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    };
    for (auto [lo, hi] : kEvents)
        if (HWINEVENTHOOK h = SetWinEventHook(lo, hi, nullptr, onWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
            hooks_.push_back(h);
    registerHotkeys();
    schedule(kTaskbars | kDesktops | kWindows | kLayout);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (HWND dlg = settingsWindow(); dlg && IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (HWINEVENTHOOK h : hooks_) UnhookWinEvent(h);
    if (reorderHook_) UnhookWinEvent(reorderHook_);
    pagers_.clear();
    api.reset();
    g_app = nullptr;
    return static_cast<int>(msg.wParam);
}

bool App::createTarget() {
    icons_.clear();
    layer.Reset();
    brush.Reset();
    rt.Reset();
    // keep D3D/driver DLLs out of the process
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2d->CreateDCRenderTarget(&props, &rt))) return false;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush)) && SUCCEEDED(rt->CreateLayer(&layer));
}

LRESULT CALLBACK App::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* a = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        a->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(a));
    }
    auto* a = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return a ? a->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == msgTaskbarCreated_ && msg) {  // explorer (re)started
        pagers_.clear();
        initApi();
        schedule(kTaskbars | kDesktops | kWindows | kLayout);
        return 0;
    }
    switch (msg) {
    case WM_APP_VD:
        if (wp == vd::Current) {
            switchTick_ = GetTickCount64();
            hideSystemOsd();
        }
        schedule(wp == vd::View ? kWindows : kDesktops | kWindows);
        return 0;
    case WM_APP_TASKBARS:
        schedule(kTaskbars);
        return 0;
    case WM_APP_SETTINGS:
        openSettings();
        return 0;
    case WM_TIMER:
        if (wp == kTimerRefresh) {
            refresh();
        } else if (wp == kTimerReinit) {
            initApi();
        } else if (wp == kTimerTaskbars) {
            KillTimer(hwnd_, kTimerTaskbars);
            schedule(kTaskbars | kLayout);
        } else if (wp == kTimerActivate) {
            KillTimer(hwnd_, kTimerActivate);
            if (activateOn_ >= 0 && activateOn_ == model.current) SetForegroundWindow(GetShellWindow());
            activateOn_ = -1;
        } else if (wp == kTimerSlide) {  // missed the end of the slide animation
            slide_ = nullptr;
            flushOsd();
        }
        return 0;
    case WM_HOTKEY:
        moveForeground(wp == kHotkeyLeft ? -1 : 1);
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet")) applyTheme();
        schedule(kLayout);
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        applyTheme();
        return 0;
    case WM_DISPLAYCHANGE:
        schedule(kTaskbars | kLayout);
        SetTimer(hwnd_, kTimerTaskbars, 2000, nullptr);  // explorer recreates secondary taskbars a bit later
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void CALLBACK App::onWinEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    App* a = g_app;
    if (!a || !h) return;
    if (ev == EVENT_OBJECT_REORDER || ev == EVENT_SYSTEM_FOREGROUND) {
        // Keep above explorer's XAML bridge, and retry embedding
        // while Start/Search/Task View are up the taskbar sits in band ZBID_IMMERSIVE_MOGO
        // and refuses cross-process child creation
        for (auto& p : a->pagers_) p->raise();
        if (std::ranges::any_of(a->pagers_, [](const auto& p) { return !p->alive(); })) a->schedule(kTaskbars);
        if (ev == EVENT_OBJECT_REORDER) return;
    }
    if (obj != OBJID_WINDOW || child != CHILDID_SELF) return;
    if (h == a->slide_ && (ev == EVENT_OBJECT_HIDE || ev == EVENT_OBJECT_DESTROY)) {
        a->slide_ = nullptr;
        a->flushOsd();
        return;
    }
    const bool tracked = std::ranges::any_of(a->model.windows, [h](const Win& w) { return w.hwnd == h; });
    switch (ev) {
    case EVENT_OBJECT_LOCATIONCHANGE:
        if (tracked) {
            a->schedule(kGeometry);
        } else if (std::ranges::any_of(a->pagers_, [h](const auto& p) { return p->taskbar() == h || GetParent(h) == p->taskbar(); })) {
            a->schedule(kLayout);  // taskbar resized or the tray grew
        }
        return;
    case EVENT_OBJECT_NAMECHANGE:
        if (tracked) a->schedule(kTitles);
        return;
    case EVENT_OBJECT_DESTROY:
        if (tracked) a->schedule(kWindows);
        return;
    }
    if (!tracked && GetAncestor(h, GA_PARENT) != GetDesktopWindow()) return;
    if (ev == EVENT_OBJECT_SHOW && !tracked) {
        wchar_t cls[40] = L"";
        GetClassNameW(h, cls, 40);
        if (!wcscmp(cls, L"Shell_SecondaryTrayWnd")) a->schedule(kTaskbars);
        if (!wcscmp(cls, L"VirtualDesktopHotkeySwitcher")) {  // explorer's full-screen slide
            a->slide_ = h;
            return;
        }
        if (isSystemOsd(h)) {
            a->systemOsd_ = h;
            a->hideSystemOsd();
            return;
        }
    }
    a->schedule(kWindows);
}

void App::schedule(unsigned d) {
    dirty_ |= d;
    if (!timerPending_) timerPending_ = SetTimer(hwnd_, kTimerRefresh, 16, nullptr) != 0;
}

void App::refresh() {
    KillTimer(hwnd_, kTimerRefresh);
    timerPending_ = false;
    unsigned d = std::exchange(dirty_, 0u);
    if (d & kTaskbars) {
        rebuildPagers();
        if (std::ranges::any_of(pagers_, [](const auto& p) { return !p->alive(); })) SetTimer(hwnd_, kTimerTaskbars, 1000, nullptr);
        d |= kLayout;
    }
    bool changed = d & (kLayout | kTitles);
    if ((d & kDesktops) && api.ready()) {
        HRESULT hr = S_OK;
        const size_t before = model.desktops.size();
        const GUID was = before ? model.desktops[model.current].id : GUID{};
        changed |= model.refreshDesktops(api, hr);
        pending_ = -1;
        if (vd::isDisconnected(hr)) {
            api.reset();
            SetTimer(hwnd_, kTimerReinit, 1000, nullptr);
        }
        if (model.desktops.size() != before) d |= kLayout;
        d |= kWindows;
        if (SUCCEEDED(hr) && before && !model.desktops.empty() && model.desktops[model.current].id != was)
            showOsd(model.indexOf(was), model.current);
    }
    if (d & kWindows) {
        changed |= model.refreshWindows(api);
        if (activateOn_ >= 0 && activateOn_ == model.current) {
            // API switches leave focus on the now-cloaked window
            // do what win+ctrl+arrow does
            const auto top = std::ranges::find_if(model.windows, [&](const Win& w) { return w.desktop == activateOn_ && !w.minimized; });
            if (top != model.windows.end()) {
                SetForegroundWindow(top->hwnd);
                activateOn_ = -1;
                KillTimer(hwnd_, kTimerActivate);
            }
        }
        std::erase_if(icons_, [this](const auto& kv) {
            return std::ranges::none_of(model.windows, [&](const Win& w) { return w.hwnd == kv.first; });
        });
    } else if (d & kGeometry) {
        changed |= model.refreshGeometry();
    }
    for (auto& p : pagers_) p->update(d & kLayout, changed);
}

void App::initApi() {
    KillTimer(hwnd_, kTimerReinit);
    const HRESULT hr = api.init(hwnd_, WM_APP_VD);
    if (SUCCEEDED(hr)) return schedule(kDesktops | kWindows | kLayout);
    if (hr == E_NOINTERFACE) {
        MessageBoxW(nullptr,
                    L"This Windows build exposes a virtual desktop interface WinPager doesn't know.\n"
                    L"WinPager targets Windows 11 24H2/25H2 (build 26100 and later).",
                    L"WinPager", MB_ICONERROR);
        DestroyWindow(hwnd_);
        return;
    }
    SetTimer(hwnd_, kTimerReinit, 2000, nullptr);  // explorer not (yet) running
}

void App::applyTheme() {
    palette = pagerPalette(settings);
    setMenuTheme(palette.dark);
    for (auto& p : pagers_) p->restyle();
    if (osd_) osd_->restyle();
}

void App::showOsd(int from, int to) {
    if (!settings.switchOsd || shellOverlayActive()) return;
    if (slide_ && IsWindowVisible(slide_)) {  // the OSD would play its cross-fade underneath
        if (osdTo_ < 0) osdFrom_ = from;
        osdTo_ = to;
        SetTimer(hwnd_, kTimerSlide, 1000, nullptr);
        return;
    }
    if (!osd_) osd_ = std::make_unique<Osd>(*this);
    osd_->show(from, to);
}

void App::flushOsd() {
    KillTimer(hwnd_, kTimerSlide);
    const int from = std::exchange(osdFrom_, -1), to = std::exchange(osdTo_, -1);
    if (to >= 0 && to == model.current) showOsd(from, to);
}

void App::hideSystemOsd() {
    if (!settings.hideSystemOsd || GetTickCount64() - switchTick_ >= 600) return;
    if (!systemOsd_ || !IsWindow(systemOsd_))  // not seen shown yet this session
        for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"XamlExplorerHostIslandWindow", nullptr));)
            if (isSystemOsd(h)) systemOsd_ = h;
    if (systemOsd_ && IsWindowVisible(systemOsd_)) ShowWindowAsync(systemOsd_, SW_HIDE);
}

void App::applySettings() {
    settings.save();
    registerHotkeys();
    applyTheme();
    schedule(kTaskbars | kLayout | kWindows);
}

void App::rebuildPagers() {
    std::vector<HWND> bars;
    HWND main = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (main) bars.push_back(main);
    DWORD shellPid = 0;
    if (main) GetWindowThreadProcessId(main, &shellPid);
    if (shellPid != reorderPid_) {  // z-order changes only matter inside explorer, keep this hook off busy apps
        if (reorderHook_) UnhookWinEvent(reorderHook_);
        reorderHook_ = shellPid ? SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr, onWinEvent, shellPid, 0,
                                                  WINEVENT_OUTOFCONTEXT)
                                : nullptr;
        reorderPid_ = shellPid;
    }
    if (settings.showOnlyCurrentScreen)
        for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr));) bars.push_back(h);
    std::erase_if(pagers_, [&](const auto& p) { return !p->alive() || std::ranges::find(bars, p->taskbar()) == bars.end(); });
    for (HWND b : bars)
        if (std::ranges::none_of(pagers_, [b](const auto& p) { return p->taskbar() == b; }))
            pagers_.push_back(std::make_unique<Pager>(*this, b));
}

void App::registerHotkeys() {
    const Hotkey* keys[2] = {&settings.moveLeft, &settings.moveRight};
    for (int i = 0; i < 2; ++i) {
        UnregisterHotKey(hwnd_, kHotkeyLeft + i);
        hotkeyOk[i] = !keys[i]->vk || RegisterHotKey(hwnd_, kHotkeyLeft + i, keys[i]->mods | MOD_NOREPEAT, keys[i]->vk);
    }
}

void App::switchTo(int desktop, bool activate) {
    if (desktop < 0 || desktop >= static_cast<int>(model.desktops.size())) return;
    if (desktop == currentDesktop()) return activateCurrent();
    if (FAILED(api.switchTo(model.desktops[desktop].id))) return;
    pending_ = desktop;
    if (!activate) return;
    activateOn_ = desktop;
    SetTimer(hwnd_, kTimerActivate, 700, nullptr);  // empty desktop
}

void App::activateCurrent() {
    if (settings.currentDesktopSelected == 1) {
        if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) PostMessageW(tray, WM_COMMAND, 407, 0);  // Show desktop toggle
    } else if (settings.currentDesktopSelected == 2) {
        pressWinTab();
    }
}

void App::moveWindow(HWND hwnd, int desktop) {
    if (desktop < 0 || desktop >= static_cast<int>(model.desktops.size())) return;
    api.moveWindow(hwnd, model.desktops[desktop].id);
    schedule(kWindows);
}

void App::moveForeground(int dir) {
    if (timerPending_) refresh();
    const int n = static_cast<int>(model.desktops.size()), cur = currentDesktop();
    const int t = stepDesktop(cur, n, dir, settings.wrapPage);
    if (t == cur) return;
    const GUID id = model.desktops[t].id;
    const HWND fg = GetForegroundWindow();
    const HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    const bool movable = root && std::ranges::any_of(model.windows, [root](const Win& w) { return w.hwnd == root; });
    HRESULT hr = movable ? api.moveForegroundAndSwitch(id) : api.switchTo(id);
    if (FAILED(hr) && movable && SUCCEEDED(hr = api.moveWindow(root, id)) && SUCCEEDED(hr = api.switchTo(id)))
        SetForegroundWindow(root);
    if (SUCCEEDED(hr)) pending_ = t;
}

void App::contextMenu(POINT pt) {
    const int n = static_cast<int>(model.desktops.size());
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_ADD, L"Add Virtual Desktop");
    AppendMenuW(m, MF_STRING | (n > 1 ? 0 : MF_GRAYED), IDM_REMOVE, L"Remove Virtual Desktop");
    AppendMenuW(m, MF_STRING, IDM_CONFIGURE_VD, L"Configure Virtual Desktops…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Configure WinPager…");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case IDM_ADD:
        api.create();
        break;
    case IDM_REMOVE:
        if (n > 1) api.remove(model.desktops[n - 1].id, model.desktops[n - 2].id);
        break;
    case IDM_CONFIGURE_VD:
        ShellExecuteW(nullptr, L"open", L"ms-settings:multitasking", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case IDM_SETTINGS:
        openSettings();
        break;
    case IDM_EXIT:
        DestroyWindow(hwnd_);
        break;
    }
}

void App::openSettings() {
    showSettings(hwnd_, settings, static_cast<int>(model.desktops.size()), hotkeyOk,
                 [](void* ctx) { static_cast<App*>(ctx)->applySettings(); }, this);
}

ID2D1Bitmap* App::icon(HWND h) {
    auto [it, fresh] = icons_.try_emplace(h);
    if (!fresh) return it->second.Get();
    HICON ic = nullptr;
    for (WPARAM type : {WPARAM(ICON_BIG), WPARAM(ICON_SMALL2)}) {
        DWORD_PTR r = 0;
        if (!ic && SendMessageTimeoutW(h, WM_GETICON, type, 0, SMTO_ABORTIFHUNG, 50, &r) && r) ic = reinterpret_cast<HICON>(r);
    }
    if (!ic) ic = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON));
    ComPtr<IWICBitmap> wb;
    if (ic) {
        wic->CreateBitmapFromHICON(ic, &wb);
    } else if (HBITMAP hb = packagedAppIcon(h)) {
        wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUsePremultipliedAlpha, &wb);
        DeleteObject(hb);
    }
    ComPtr<IWICFormatConverter> cv;
    if (wb && SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        rt->CreateBitmapFromWicBitmap(cv.Get(), &it->second);
    return it->second.Get();
}
