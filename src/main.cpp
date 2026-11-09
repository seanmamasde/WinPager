#include "app.h"

#include <commctrl.h>
#include <ole2.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\WinPager.instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {  // second launch opens the running instance's settings
        if (HWND h = FindWindowW(kControllerClass, nullptr)) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(h, WM_APP_SETTINGS, 0, 0);
        }
        return 0;
    }
    const INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    if (FAILED(OleInitialize(nullptr))) return 1;  // STA + OLE drag and drop
    int rc;
    {
        App app;
        rc = app.run(inst);
    }
    OleUninitialize();
    if (single) CloseHandle(single);
    return rc;
}
