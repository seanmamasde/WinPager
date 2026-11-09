#include "vdapi.h"

#include <inspectable.h>
#include <objectarray.h>
#include <servprov.h>
#include <shobjidl.h>
#include <winstring.h>

#include <atomic>

using Microsoft::WRL::ComPtr;

// Opaque: only ever passed back to explorer.
struct IApplicationView : IInspectable {};

// Interface layouts for build 26100/26200, cross-checked against MScholtes/VirtualDesktop
// (VirtualDesktop11-24H2.cs) and Ciantic/VirtualDesktopAccessor (interfaces.rs).
MIDL_INTERFACE("3F07F4BE-B107-441A-AF0F-39D82529072C")
IVirtualDesktop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsViewVisible(IApplicationView*, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetId(GUID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetName(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetWallpaperPath(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsRemote(BOOL*) = 0;
};

MIDL_INTERFACE("1841C6D7-4F9D-42C0-AF41-8747538F10E5")
IApplicationViewCollection : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetViews(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(PCWSTR, IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND, IApplicationView**) = 0;
};

MIDL_INTERFACE("53F5CA0B-158F-4124-900C-057158060B27")
IVirtualDesktopManagerInternal : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveViewToDesktop(IApplicationView*, IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CanViewMoveDesktops(IApplicationView*, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentDesktop(IVirtualDesktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesktops(IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAdjacentDesktop(IVirtualDesktop*, UINT, IVirtualDesktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktop(IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktopAndMoveForegroundView(IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDesktop(IVirtualDesktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveDesktop(IVirtualDesktop*, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveDesktop(IVirtualDesktop*, IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindDesktop(const GUID*, IVirtualDesktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDesktopSwitchIncludeExcludeViews(IVirtualDesktop*, IObjectArray**, IObjectArray**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDesktopName(IVirtualDesktop*, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDesktopWallpaper(IVirtualDesktop*, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateWallpaperPathForAllDesktops(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyDesktopState(IApplicationView*, IApplicationView*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateRemoteDesktop(HSTRING, IVirtualDesktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchRemoteDesktop(IVirtualDesktop*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwitchDesktopWithAnimation(IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLastActiveDesktop(IVirtualDesktop**) = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitForAnimationToComplete() = 0;
};

MIDL_INTERFACE("B9E5E94D-233E-49AB-AF5C-2B4541C3AADE")
IVirtualDesktopNotification : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopCreated(IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopDestroyBegin(IVirtualDesktop*, IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopDestroyFailed(IVirtualDesktop*, IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopDestroyed(IVirtualDesktop*, IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopMoved(IVirtualDesktop*, INT64, INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopNameChanged(IVirtualDesktop*, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE ViewVirtualDesktopChanged(IApplicationView*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CurrentVirtualDesktopChanged(IVirtualDesktop*, IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopWallpaperChanged(IVirtualDesktop*, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE VirtualDesktopSwitched(IVirtualDesktop*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoteVirtualDesktopConnected(IVirtualDesktop*) = 0;
};

MIDL_INTERFACE("0CD45E71-D927-4F15-8B0A-8FEF525337BF")
IVirtualDesktopNotificationService : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Register(IVirtualDesktopNotification*, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unregister(DWORD) = 0;
};

// mingw (the Windhawk mod's compiler) ignores MIDL_INTERFACE's ID in __uuidof; keep these in step with the IDs above
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(IVirtualDesktop, 0x3F07F4BE, 0xB107, 0x441A, 0xAF, 0x0F, 0x39, 0xD8, 0x25, 0x29, 0x07, 0x2C)
__CRT_UUID_DECL(IApplicationViewCollection, 0x1841C6D7, 0x4F9D, 0x42C0, 0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5)
__CRT_UUID_DECL(IVirtualDesktopManagerInternal, 0x53F5CA0B, 0x158F, 0x4124, 0x90, 0x0C, 0x05, 0x71, 0x58, 0x06, 0x0B, 0x27)
__CRT_UUID_DECL(IVirtualDesktopNotification, 0xB9E5E94D, 0x233E, 0x49AB, 0xAF, 0x5C, 0x2B, 0x45, 0x41, 0xC3, 0xAA, 0xDE)
__CRT_UUID_DECL(IVirtualDesktopNotificationService, 0x0CD45E71, 0xD927, 0x4F15, 0x8B, 0x0A, 0x8F, 0xEF, 0x52, 0x53, 0x37, 0xBF)
#endif

namespace {

constexpr CLSID CLSID_ImmersiveShell = {0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
constexpr GUID SID_VirtualDesktopManagerInternal = {0xC5E0CDCA, 0x7B6E, 0x41B2, {0x9F, 0xC4, 0xD9, 0x39, 0x75, 0xCC, 0x46, 0x7B}};
constexpr GUID SID_VirtualDesktopNotificationService = {0xA501FDEC, 0x4A09, 0x464C, {0xAE, 0x4E, 0x1B, 0x9C, 0x21, 0xB8, 0x49, 0x18}};
constexpr CLSID CLSID_VirtualDesktopManager_ = {0xAA509086, 0x5CA9, 0x4C25, {0x8F, 0x95, 0x58, 0x9D, 0x3C, 0x07, 0xB4, 0x8A}};

// Callbacks arrive on our STA; only post, never call back into explorer from here.
class Sink final : public IVirtualDesktopNotification {
    std::atomic<ULONG> ref_{1};
    HWND wnd_;
    UINT msg_;
    HRESULT post(vd::Change c) {
        PostMessageW(wnd_, msg_, c, 0);
        return S_OK;
    }

public:
    Sink(HWND w, UINT m) : wnd_(w), msg_(m) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IVirtualDesktopNotification)) {
            *ppv = static_cast<IVirtualDesktopNotification*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP VirtualDesktopCreated(IVirtualDesktop*) override { return post(vd::List); }
    STDMETHODIMP VirtualDesktopDestroyBegin(IVirtualDesktop*, IVirtualDesktop*) override { return S_OK; }
    STDMETHODIMP VirtualDesktopDestroyFailed(IVirtualDesktop*, IVirtualDesktop*) override { return post(vd::List); }
    STDMETHODIMP VirtualDesktopDestroyed(IVirtualDesktop*, IVirtualDesktop*) override { return post(vd::List); }
    STDMETHODIMP VirtualDesktopMoved(IVirtualDesktop*, INT64, INT64) override { return post(vd::List); }
    STDMETHODIMP VirtualDesktopNameChanged(IVirtualDesktop*, HSTRING) override { return post(vd::List); }
    STDMETHODIMP ViewVirtualDesktopChanged(IApplicationView*) override { return post(vd::View); }
    STDMETHODIMP CurrentVirtualDesktopChanged(IVirtualDesktop*, IVirtualDesktop*) override { return post(vd::Current); }
    STDMETHODIMP VirtualDesktopWallpaperChanged(IVirtualDesktop*, HSTRING) override { return S_OK; }
    STDMETHODIMP VirtualDesktopSwitched(IVirtualDesktop*) override { return post(vd::Current); }
    STDMETHODIMP RemoteVirtualDesktopConnected(IVirtualDesktop*) override { return post(vd::List); }
};

}  // namespace

namespace vd {

Api::Api() = default;
Api::~Api() { reset(); }

HRESULT Api::init(HWND notifyWnd, UINT notifyMsg) {
    reset();
    ComPtr<IServiceProvider> shell;
    HRESULT hr = CoCreateInstance(CLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&shell));
    if (SUCCEEDED(hr)) hr = shell->QueryService(SID_VirtualDesktopManagerInternal, IID_PPV_ARGS(&mgr_));
    if (SUCCEEDED(hr)) hr = shell->QueryService(__uuidof(IApplicationViewCollection), IID_PPV_ARGS(&views_));
    if (SUCCEEDED(hr)) hr = shell->QueryService(SID_VirtualDesktopNotificationService, IID_PPV_ARGS(&svc_));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_VirtualDesktopManager_, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pub_));
    if (SUCCEEDED(hr)) {
        ComPtr<Sink> sink;
        sink.Attach(new Sink(notifyWnd, notifyMsg));
        hr = svc_->Register(sink.Get(), &cookie_);
    }
    if (FAILED(hr)) reset();
    return hr;
}

void Api::reset() {
    if (svc_ && cookie_) svc_->Unregister(cookie_);
    cookie_ = 0;
    svc_.Reset();
    pub_.Reset();
    views_.Reset();
    mgr_.Reset();
}

ComPtr<IVirtualDesktop> Api::find(const GUID& id) {
    ComPtr<IVirtualDesktop> d;
    if (mgr_) mgr_->FindDesktop(&id, &d);
    return d;
}

HRESULT Api::desktops(std::vector<Desktop>& out, int& current) {
    if (!mgr_) return E_UNEXPECTED;
    ComPtr<IObjectArray> arr;
    ComPtr<IVirtualDesktop> cur;
    HRESULT hr = mgr_->GetDesktops(&arr);
    if (SUCCEEDED(hr)) hr = mgr_->GetCurrentDesktop(&cur);
    if (FAILED(hr)) return hr;
    GUID curId{};
    cur->GetId(&curId);
    UINT n = 0;
    arr->GetCount(&n);
    out.clear();
    current = 0;
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IVirtualDesktop> d;
        if (FAILED(arr->GetAt(i, IID_PPV_ARGS(&d)))) continue;
        Desktop x;
        d->GetId(&x.id);
        HSTRING s = nullptr;
        if (SUCCEEDED(d->GetName(&s)) && s) {
            UINT32 len = 0;
            PCWSTR p = WindowsGetStringRawBuffer(s, &len);
            x.name.assign(p, len);
            WindowsDeleteString(s);
        }
        if (x.id == curId) current = static_cast<int>(out.size());
        out.push_back(std::move(x));
    }
    return S_OK;
}

HRESULT Api::switchTo(const GUID& id, bool animate) {
    auto d = find(id);
    if (!d) return E_INVALIDARG;
    if (animate) {
        const HRESULT hr = mgr_->SwitchDesktopWithAnimation(d.Get());
        if (SUCCEEDED(hr) || isDisconnected(hr)) return hr;
    }
    return mgr_->SwitchDesktop(d.Get());
}

HRESULT Api::create() {
    if (!mgr_) return E_UNEXPECTED;
    ComPtr<IVirtualDesktop> d;
    return mgr_->CreateDesktop(&d);
}

HRESULT Api::remove(const GUID& id, const GUID& fallback) {
    auto d = find(id), f = find(fallback);
    if (!d || !f) return E_INVALIDARG;
    return mgr_->RemoveDesktop(d.Get(), f.Get());
}

HRESULT Api::moveWindow(HWND hwnd, const GUID& to) {
    auto d = find(to);
    if (!d || !views_) return E_INVALIDARG;
    ComPtr<IApplicationView> view;
    HRESULT hr = views_->GetViewForHwnd(hwnd, &view);
    return SUCCEEDED(hr) ? mgr_->MoveViewToDesktop(view.Get(), d.Get()) : hr;
}

HRESULT Api::moveForegroundAndSwitch(const GUID& to) {
    auto d = find(to);
    return d ? mgr_->SwitchDesktopAndMoveForegroundView(d.Get()) : E_INVALIDARG;
}

GUID Api::windowDesktop(HWND hwnd) const {
    GUID g{};
    if (pub_) pub_->GetWindowDesktopId(hwnd, &g);
    return g;
}

}  // namespace vd
