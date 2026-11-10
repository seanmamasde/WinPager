#pragma once
#include "model.h"
#include "settings.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <memory>
#include <unordered_map>
#include <unordered_set>

class Pager;
class Osd;

enum : UINT {
    WM_APP_VD = WM_APP + 1,  // vd::Change in wParam
    WM_APP_SETTINGS,         // sent by a second instance
    WM_APP_TASKBARS,         // a pager's taskbar went away
    WM_APP_ICON,             // a window's icon, looked up off this thread (app.cpp fetchIcon)
};

inline constexpr wchar_t kControllerClass[] = L"WinPager.Controller";

class App {
public:
    App();
    ~App();
    int run(HINSTANCE inst);

    HINSTANCE inst = nullptr;
    Model model;
    Settings settings;
    Palette palette{};
    vd::Api api;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    Microsoft::WRL::ComPtr<ID2D1Layer> layer;
    bool hotkeyOk[2] = {true, true};
    bool perScreen = false;  // each pager shows just its own screen (see rebuildPagers)

    int currentDesktop() const { return pending_ >= 0 ? pending_ : model.current; }
    void pagerDied() { PostMessageW(hwnd_, WM_APP_TASKBARS, 0, 0); }
    void switchTo(int desktop, bool activate = true);
    void activateCurrent();
    void moveWindow(HWND hwnd, int desktop);
    void contextMenu(POINT screen);
    ID2D1Bitmap* icon(HWND hwnd);
#ifndef WH_MOD
    void applySettings();  // after the settings dialog changed `settings`
#endif

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK onWinEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void schedule(unsigned dirty);
    void refresh();
    void initApi();
    bool createTarget();
    void applyTheme();
    void rebuildPagers();
    void registerHotkeys();
    void moveForeground(int dir);
    void openSettings();
    void hideSystemOsd();
    void showOsd(int from, int to);
    void flushOsd();

    HWND hwnd_ = nullptr;
    UINT msgTaskbarCreated_ = 0;
    unsigned dirty_ = 0;
    bool timerPending_ = false;
    int pending_ = -1;     // desktop we asked explorer to switch to
    int activateOn_ = -1;  // focus that desktop's top window once the switch lands
    std::vector<HWINEVENTHOOK> hooks_;
    HWINEVENTHOOK reorderHook_ = nullptr;
    DWORD reorderPid_ = 0;
    std::vector<std::unique_ptr<Pager>> pagers_;
    std::unique_ptr<Osd> osd_;
    int osdFrom_ = -1, osdTo_ = -1;  // OSD held back while explorer's slide animation covers the screen
    HWND slide_ = nullptr;           // that animation surface (VirtualDesktopHotkeySwitcher), while it plays
    HWND systemOsd_ = nullptr;       // explorer's shared OSD host (volume, brightness, "Desktop N")
    ULONGLONG switchTick_ = 0;
    std::unordered_map<HWND, Microsoft::WRL::ComPtr<ID2D1Bitmap>> icons_;
    std::unordered_set<LPARAM> iconRequests_;  // fetchIcon's in flight: a WM_APP_ICON lParam that isn't one isn't ours
};
