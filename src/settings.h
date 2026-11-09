#pragma once
#include <windows.h>
#include <d2d1.h>

struct Hotkey {
    UINT mods;  // MOD_* flags
    UINT vk;    // 0 = none
    bool operator==(const Hotkey&) const = default;
};

// Option names and enums follow KDE's pager main.xml; the last block is Windows-specific.
struct Settings {
    bool showWindowOutlines = true;
    bool showWindowIcons = false;
    bool showOnlyCurrentScreen = true;  // also: one pager per monitor taskbar instead of one on the main taskbar
    bool wrapPage = false;
    int displayedText = 2;           // 0 number, 1 name, 2 none
    int currentDesktopSelected = 2;  // 0 nothing, 1 show desktop, 2 Task View (Win+Tab)

    int position = 0;     // 0 left edge, 1 next to the system tray
    int maxDesktops = 0;  // desktops shown at most, 0 = all; the shown ones follow the current desktop
    int theme = 0;     // 0 follow Windows, 1 light, 2 dark
    bool accentHighlight = true;
    bool switchOsd = true;      // KWin's desktop change OSD in the middle of the screen
    bool hideSystemOsd = true;  // Windows' own "Desktop N" popup above the taskbar
    Hotkey moveLeft{MOD_CONTROL | MOD_SHIFT | MOD_WIN, VK_LEFT};
    Hotkey moveRight{MOD_CONTROL | MOD_SHIFT | MOD_WIN, VK_RIGHT};

    void load();
    void save() const;
    bool operator==(const Settings&) const = default;
};

bool startupEnabled();
void setStartup(bool on);

struct Palette {
    bool dark;
    D2D1_COLOR_F text;       // KDE Theme.textColor
    D2D1_COLOR_F highlight;  // KDE ButtonFocus (used by the hover/active desktop frames)
};

bool systemDark();  // taskbar ("Windows mode")
bool appsDark();    // app windows ("App mode")
COLORREF accentColor(int shade);  // 0 = Light3 ... 3 = accent ... 6 = Dark3
Palette pagerPalette(const Settings& s);

// Settings dialog (modeless). onApply runs after OK/Apply; hotkeyOk[2] reports registration afterwards; desktops is how
// many exist, the largest useful "Desktops shown" limit being one less.
void showSettings(HWND owner, Settings& s, int desktops, const bool* hotkeyOk, void (*onApply)(void*), void* ctx);
HWND settingsWindow();  // for IsDialogMessage; null when closed
