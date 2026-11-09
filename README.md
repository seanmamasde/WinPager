# WinPager

A KDE Plasma–style virtual desktop pager that lives inside the Windows 11 taskbar.
Native C++23 / Win32 / Direct2D, ~240 KB, no runtime dependencies, ~8 MB private memory, 0% CPU when idle.

![icon](assets/icon-256.png)

### Settings (right-click → Configure WinPager…)

| KDE option | here |
|---|---|
| Show window outlines / application icons on outlines | same |
| Show only current screen | same; also puts WinPager on **each monitor's taskbar** (off = one WinPager on the main taskbar showing all monitors) |
| Navigation wraps around | same (wheel and shortcuts) |
| Text display: none / number / name | same (names come from Task View renames) |
| Selecting current virtual desktop | Does nothing / Shows the desktop / **Shows Task View** |

Windows page: position (left edge — after the Widgets button if shown; on a left-aligned taskbar it goes beside
the tray instead — or next to the system tray), theme (follow Windows / light / dark),
accent-colour highlight on/off, start with Windows, desktop switching (show the OSD; hide the Windows popup).
Settings live in `HKCU\Software\WinPager`.

WinPager follows the taskbar's light/dark mode and the accent palette live; the settings window follows the app mode.

## Install

Download `WinPager.exe` from [Releases](https://github.com/seanmamasde/WinPager/releases/latest) and run it
(it isn't code-signed, so SmartScreen may ask once), or use [Scoop](https://scoop.sh):

```powershell
scoop bucket add seanmamasde https://github.com/seanmamasde/scoop-bucket
scoop install seanmamasde/winpager
```

Launching it again while it runs opens the settings; right-click it → Exit to quit.

## Build

Requires the MSVC build tools (VS 2022/2026 Build Tools with the C++ workload; CMake and Ninja ship with them).

```powershell
./build.ps1            # Release -> build/Release/WinPager.exe
./build.ps1 Debug
ctest --test-dir build/Release   # logic self-test (or run build/Release/selftest.exe)
```

`tools/make-icon.ps1` regenerates `res/WinPager.ico` from `assets/*.svg` (needs `scoop install resvg`).

## Limitations

- Windows 11 24H2/25H2 (build 26100+) only; another build with different interface IDs gets a clear error.
- The taskbar doesn't reserve space for WinPager; a centered taskbar crowded enough to reach the left edge
  (or the tray) will draw its buttons under it.
- With "Hide the Windows popup" on, a volume or brightness popup triggered within 0.6 s of a desktop switch is
  hidden too, since explorer shows both in the same window.

## License

[MIT](LICENSE)
