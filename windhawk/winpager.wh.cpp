// ==WindhawkMod==
// @id              winpager
// @name            WinPager
// @description     A KDE Plasma-style virtual desktop pager in the Windows 11 taskbar: live window outlines, click or scroll to switch, drag windows between desktops
// @version         @VERSION@
// @author          seanmamasde
// @github          https://github.com/seanmamasde
// @homepage        https://github.com/seanmamasde/WinPager
// @include         windhawk.exe
// @compilerOptions -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -luxtheme -lcomctl32 -lshcore -lshlwapi -lruntimeobject -lole32 -loleaut32 -luuid -lgdi32
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# WinPager

A virtual desktop pager like KDE Plasma's, living in the Windows 11 taskbar.

![WinPager in the taskbar, showing a desktop's windows](https://raw.githubusercontent.com/seanmamasde/WinPager/main/assets/screenshot1.png)

![The on-screen display when switching desktops](https://raw.githubusercontent.com/seanmamasde/WinPager/main/assets/screenshot2.png)

- Shows every virtual desktop with live window outlines, optionally with application icons
- Click a desktop to switch to it; click the current one to open Task View (configurable)
- Scroll over it to flip through desktops
- Drag a window outline onto another desktop to move that window there
- Hover a desktop for a list of its windows; right-click to add or remove desktops
- Ctrl+Shift+Win+Left/Right moves the active window to the neighbouring desktop and follows it
- KWin-style on-screen display when switching, and an option to hide Windows' own "Desktop N" popup
- A pager on each monitor's taskbar, or one on the main taskbar showing all monitors
- Can show only a few desktops at a time, scrolling along with the current one
- Follows light/dark mode and the accent color

The settings mirror those of Plasma's Pager widget, plus position, theme and the switching popups.

The mod runs in its own process; it isn't injected into explorer.exe.

## Notes

- Windows 11 24H2/25H2 (build 26100 and later) only. Like other virtual desktop tools, it relies on undocumented
  virtual desktop interfaces, so a Windows update can break it. On a build it doesn't know, it shows an error.
- The taskbar doesn't reserve space for it. Next to a centered row of buttons it shows only as many desktops as fit
  (at least the current one); on a left-aligned taskbar, a row of buttons long enough to reach the tray runs under it.
- WinPager is also a standalone app, [WinPager.exe](https://github.com/seanmamasde/WinPager), built from the same
  source. Only one of the two runs at a time: if you switch to the mod, quit WinPager.exe and turn off its
  "Start with Windows".
- Bugs and ideas: [github.com/seanmamasde/WinPager/issues](https://github.com/seanmamasde/WinPager/issues)
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- showWindowOutlines: true
  $name: Show window outlines
- showWindowIcons: false
  $name: Show application icons on window outlines
- showOnlyCurrentScreen: true
  $name: Show only current screen
  $description: Puts a pager on each monitor's taskbar. Off shows one pager on the main taskbar with every monitor.
- wrapPage: false
  $name: Navigation wraps around
- displayedText: none
  $name: Text display
  $options:
  - none: No text
  - number: Desktop number
  - name: Desktop name
- currentDesktopSelected: taskView
  $name: Selecting current virtual desktop
  $options:
  - nothing: Does nothing
  - showDesktop: Shows the desktop
  - taskView: Shows Task View
- position: left
  $name: Position
  $description: The left edge comes after the Widgets button if it's shown; on a left-aligned taskbar the pager goes beside the tray instead.
  $options:
  - left: Left edge of the taskbar
  - tray: Next to the system tray
- maxDesktops: 0
  $name: Desktops shown
  $description: Show at most this many desktops (0 = all); the shown ones scroll along to keep the current desktop in view.
- theme: windows
  $name: Theme
  $options:
  - windows: Follow Windows
  - light: Light
  - dark: Dark
- accentHighlight: true
  $name: Highlight with the Windows accent color
- switchOsd: true
  $name: Show on-screen display when switching
- hideSystemOsd: true
  $name: Hide the Windows "Desktop N" popup
- moveLeft: ctrl+shift+win+37
  $name: Move window one desktop to the left
  $description: Moves the active window to the neighbouring desktop and follows it there. Modifiers, then the key's virtual-key code (37 is the Left arrow); empty turns it off.
  #! $format: hotkey
- moveRight: ctrl+shift+win+39
  $name: Move window one desktop to the right
  $description: As above; 39 is the Right arrow.
  #! $format: hotkey
*/
// ==/WindhawkModSettings==

// Source: https://github.com/seanmamasde/WinPager. This mod is windhawk/winpager.wh.cpp there, with the files it
// #includes inlined by tools/make-windhawk-mod.ps1; WinPager.exe is built from the same files.

#include <commctrl.h>
#include <cwchar>

#include "../src/vdapi.cpp"
#include "../src/model.cpp"
#include "../src/theme.cpp"
#include "../src/pager.cpp"
#include "../src/app.cpp"

// ---------------------------------------------------------------- Windhawk
// What src/settings.cpp and src/main.cpp do for WinPager.exe: the settings are the mod's, and the pager runs on a
// thread of the tool mod process.

void Settings::load() {
    auto flag = [](const wchar_t* name, bool& v) { v = Wh_GetIntSetting(name) != 0; };
    auto choice = [](const wchar_t* name, int& v, std::initializer_list<const wchar_t*> options) {
        const wchar_t* s = Wh_GetStringSetting(name);
        for (int i = 0; const wchar_t* o : options) {
            if (!wcscmp(s, o)) v = i;
            ++i;
        }
        Wh_FreeStringSetting(s);
    };
    auto hotkey = [](const wchar_t* name, Hotkey& h) {
        const wchar_t* s = Wh_GetStringSetting(name);
        const auto [mods, vk] = parseHotkey(s);
        h = {mods, vk};
        Wh_FreeStringSetting(s);
    };
    flag(L"showWindowOutlines", showWindowOutlines);
    flag(L"showWindowIcons", showWindowIcons);
    flag(L"showOnlyCurrentScreen", showOnlyCurrentScreen);
    flag(L"wrapPage", wrapPage);
    choice(L"displayedText", displayedText, {L"number", L"name", L"none"});
    choice(L"currentDesktopSelected", currentDesktopSelected, {L"nothing", L"showDesktop", L"taskView"});
    choice(L"position", position, {L"left", L"tray"});
    maxDesktops = std::clamp(Wh_GetIntSetting(L"maxDesktops"), 0, 99);
    choice(L"theme", theme, {L"windows", L"light", L"dark"});
    flag(L"accentHighlight", accentHighlight);
    flag(L"switchOsd", switchOsd);
    flag(L"hideSystemOsd", hideSystemOsd);
    hotkey(L"moveLeft", moveLeft);
    hotkey(L"moveRight", moveRight);
}

void Settings::save() const {}  // Windhawk keeps them

// "Configure WinPager…" opens Windhawk, where the settings are (Windhawk 2.0 hosts tool mods in windhawk-mod.exe beside it)
void showSettings(HWND, Settings&, int, const bool*, void (*)(void*), void*) {
    std::wstring exe(MAX_PATH, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), MAX_PATH));
    exe = exe.substr(0, exe.rfind(L'\\') + 1) + L"windhawk.exe";
    ShellExecuteW(nullptr, nullptr, exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

HWND settingsWindow() {
    return nullptr;
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

HANDLE g_pagerThread;
DWORD g_pagerThreadId;

BOOL CALLBACK closeController(HWND h, LPARAM) {
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64) && !wcscmp(cls, kControllerClass)) DestroyWindow(h);
    return TRUE;
}

DWORD WINAPI pagerThread(void*) {
    // WinPager.exe's mutex: only one of the two draws
    const HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\WinPager.instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Wh_Log(L"WinPager.exe is running");
        CloseHandle(single);
        return 0;
    }
    const INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    if (SUCCEEDED(OleInitialize(nullptr))) {  // STA + OLE drag and drop
        {
            App app;
            app.run(reinterpret_cast<HINSTANCE>(&__ImageBase));
            // stopPager's WM_QUIT leaves the controller window up: close it while App is still alive
            EnumThreadWindows(GetCurrentThreadId(), closeController, 0);
        }
        OleUninitialize();
    }
    if (single) CloseHandle(single);
    return 0;
}

void startPager() {
    g_pagerThread = CreateThread(nullptr, 0, pagerThread, nullptr, 0, &g_pagerThreadId);
}

void stopPager() {
    if (!g_pagerThread) return;
    // ends App::run's message loop; PostThreadMessage fails until the thread has a message queue
    while (!PostThreadMessageW(g_pagerThreadId, WM_QUIT, 0, 0) && WaitForSingleObject(g_pagerThread, 10) == WAIT_TIMEOUT) {
    }
    WaitForSingleObject(g_pagerThread, INFINITE);
    CloseHandle(g_pagerThread);
    g_pagerThread = nullptr;
}

}  // namespace

BOOL WhTool_ModInit() {
    startPager();
    return g_pagerThread != nullptr;
}

void WhTool_ModSettingsChanged() {  // start over with the new settings
    stopPager();
    startPager();
}

void WhTool_ModUninit() {
    stopPager();
}

////////////////////////////////////////////////////////////////////////////////
// Windhawk tool mod implementation for mods which don't need to inject to other
// processes or hook other functions. Context:
// https://github.com/ramensoftware/windhawk/wiki/Mods-as-tools:-Running-mods-in-a-dedicated-process
//
// The mod will load and run in a dedicated windhawk.exe process.
//
// Paste the code below as part of the mod code, and use these callbacks:
// * WhTool_ModInit
// * WhTool_ModSettingsChanged
// * WhTool_ModUninit
//
// Currently, other callbacks are not supported.

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
    DWORD sessionId;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) &&
        sessionId == 0) {
        return FALSE;
    }

    bool isExcluded = false;
    bool isToolModProcess = false;
    bool isCurrentToolModProcess = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    if (!argv) {
        Wh_Log(L"CommandLineToArgvW failed");
        return FALSE;
    }

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-service") == 0 ||
            wcscmp(argv[i], L"-service-start") == 0 ||
            wcscmp(argv[i], L"-service-stop") == 0) {
            isExcluded = true;
            break;
        }
    }

    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-tool-mod") == 0) {
            isToolModProcess = true;
            if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) {
                isCurrentToolModProcess = true;
            }
            break;
        }
    }

    LocalFree(argv);

    if (isExcluded) {
        return FALSE;
    }

    if (isCurrentToolModProcess) {
        g_toolModProcessMutex =
            CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolModProcessMutex) {
            Wh_Log(L"CreateMutex failed");
            ExitProcess(1);
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
            ExitProcess(1);
        }

        if (!WhTool_ModInit()) {
            ExitProcess(1);
        }

        IMAGE_DOS_HEADER* dosHeader =
            (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
        IMAGE_NT_HEADERS* ntHeaders =
            (IMAGE_NT_HEADERS*)((BYTE*)dosHeader + dosHeader->e_lfanew);

        DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
        void* entryPoint = (BYTE*)dosHeader + entryPointRVA;

        Wh_SetFunctionHook(entryPoint, (void*)EntryPoint_Hook, nullptr);
        return TRUE;
    }

    if (isToolModProcess) {
        return FALSE;
    }

    g_isToolModProcessLauncher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isToolModProcessLauncher) {
        return;
    }

    WCHAR currentProcessPath[MAX_PATH];
    switch (GetModuleFileName(nullptr, currentProcessPath,
                              ARRAYSIZE(currentProcessPath))) {
        case 0:
        case ARRAYSIZE(currentProcessPath):
            Wh_Log(L"GetModuleFileName failed");
            return;
    }

    WCHAR
    commandLine[MAX_PATH + 2 +
                (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
    swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath,
               WH_MOD_ID);

    HMODULE kernelModule = GetModuleHandle(L"kernelbase.dll");
    if (!kernelModule) {
        kernelModule = GetModuleHandle(L"kernel32.dll");
        if (!kernelModule) {
            Wh_Log(L"No kernelbase.dll/kernel32.dll");
            return;
        }
    }

    using CreateProcessInternalW_t = BOOL(WINAPI*)(
        HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes,
        LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles,
        DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
        LPSTARTUPINFOW lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation,
        PHANDLE hRestrictedUserToken);
    CreateProcessInternalW_t pCreateProcessInternalW =
        (CreateProcessInternalW_t)GetProcAddress(kernelModule,
                                                 "CreateProcessInternalW");
    if (!pCreateProcessInternalW) {
        Wh_Log(L"No CreateProcessInternalW");
        return;
    }

    STARTUPINFO si{
        .cb = sizeof(STARTUPINFO),
        .dwFlags = STARTF_FORCEOFFFEEDBACK,
    };
    PROCESS_INFORMATION pi;
    if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine,
                                 nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi, nullptr)) {
        Wh_Log(L"CreateProcess failed");
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModUninit();
    ExitProcess(0);
}
