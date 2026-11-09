#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <string>
#include <vector>

struct IServiceProvider;
struct IVirtualDesktopManager;
struct IVirtualDesktopManagerInternal;
struct IApplicationViewCollection;
struct IVirtualDesktopNotificationService;
struct IVirtualDesktop;

namespace vd {

struct Desktop {
    GUID id{};
    std::wstring name;  // empty = default name ("Desktop N")
};

// wParam of the notify message
enum Change : WPARAM { List = 0, Current = 1, View = 2 };

// Undocumented Windows 11 24H2/25H2 (build 26100+) virtual desktop API hosted by explorer.
class Api {
public:
    Api();
    ~Api();
    HRESULT init(HWND notifyWnd, UINT notifyMsg);
    void reset();
    bool ready() const { return mgr_ != nullptr; }

    HRESULT desktops(std::vector<Desktop>& out, int& current);
    HRESULT switchTo(const GUID& id, bool animate = true);
    HRESULT create();
    HRESULT remove(const GUID& id, const GUID& fallback);
    HRESULT moveWindow(HWND hwnd, const GUID& to);
    HRESULT moveForegroundAndSwitch(const GUID& to);
    GUID windowDesktop(HWND hwnd) const;

private:
    Microsoft::WRL::ComPtr<IVirtualDesktop> find(const GUID& id);

    Microsoft::WRL::ComPtr<IVirtualDesktopManagerInternal> mgr_;
    Microsoft::WRL::ComPtr<IApplicationViewCollection> views_;
    Microsoft::WRL::ComPtr<IVirtualDesktopManager> pub_;
    Microsoft::WRL::ComPtr<IVirtualDesktopNotificationService> svc_;
    DWORD cookie_ = 0;
};

// explorer restarted or the API object died
inline bool isDisconnected(HRESULT hr) {
    return hr == RPC_E_DISCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
           hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED) || hr == CO_E_OBJNOTCONNECTED;
}

}  // namespace vd
