#Requires -Version 7.3
# Builds the Windhawk mod: windhawk/winpager.wh.cpp with the files it #includes inlined and only the mod's side of their
# #ifdef/#ifndef WH_MOD blocks, as build/windhawk/winpager.wh.cpp (the single file windhawk-mods takes), then compiles it
# the way Windhawk 1.7 does (windhawk-mods' scripts/compile_mod.py) for every architecture it builds tool mods for.
# 7.3 passes the quoted -D values through intact.
param([string]$Windhawk = "$env:ProgramFiles\Windhawk")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$out = Join-Path $root 'build\windhawk'
$version = [regex]::Match((Get-Content -Raw "$root\CMakeLists.txt"), 'project\(WinPager VERSION ([\d.]+)').Groups[1].Value

$seen = @{}
function Name([string]$file) { [IO.Path]::GetRelativePath($root, $file).Replace('\', '/') }
function Expand([string]$file) {
    $resumed = $true
    $open = [Collections.Generic.List[object]]::new()  # per open #if: whether its WH_MOD branch is kept, $null if not WH_MOD
    foreach ($line in [IO.File]::ReadAllLines($file)) {
        if ($line -match '^\s*#\s*(ifn?def)\s+WH_MOD\b') { $open.Add($Matches[1] -eq 'ifdef'); continue }
        $directive = if ($line -match '^\s*#\s*(\w+)') { $Matches[1] } else { '' }
        $wh = $open.Count -and $open[$open.Count - 1] -is [bool]
        if ($directive -like 'if*') { $open.Add($null) }
        elseif ($directive -eq 'else' -and $wh) { $open[$open.Count - 1] = -not $open[$open.Count - 1]; continue }
        elseif ($directive -eq 'elif' -and $wh) { throw "$file has an #elif in an #ifdef WH_MOD block" }
        elseif ($directive -eq 'endif') { $open.RemoveAt($open.Count - 1); if ($wh) { continue } }
        if ($open -contains $false -or $line -eq '#pragma once') { continue }
        if ($line -notmatch '^#include "(.+)"$') {
            if (-not $resumed -and $line) { "// ---- $(Name $file), continued ----"; $resumed = $true }
            $line
            continue
        }
        $name = $Matches[1]
        $inc = (Join-Path (Split-Path $file) $name), (Join-Path $root "res\$name") | Where-Object { Test-Path $_ } | Select-Object -First 1
        if (-not $inc) { throw "$file includes $name, which is neither next to it nor in res\" }
        $inc = [IO.Path]::GetFullPath($inc)
        if ($seen[$inc]) { continue }
        $seen[$inc] = $true
        "// ---- $(Name $inc) ----"
        Expand $inc
        $resumed = $false
    }
}
$mod = ((Expand "$root\windhawk\winpager.wh.cpp") -join "`n").Replace('@VERSION@', $version) + "`n"
New-Item -ItemType Directory -Force $out | Out-Null
[IO.File]::WriteAllText("$out\winpager.wh.cpp", $mod)  # UTF-8 without BOM, LF

$clang = "$Windhawk\Compiler\bin\clang++.exe"
if (-not (Test-Path $clang)) { throw "Windhawk's compiler isn't at $clang (install Windhawk, or pass -Windhawk <its folder>)" }
$engine = Join-Path $Windhawk ((Get-Content "$Windhawk\windhawk.ini" | Select-String '^EnginePath=(.+)').Matches[0].Groups[1].Value)
$id = [regex]::Match($mod, '// @id\s+(\S+)').Groups[1].Value
$options = [regex]::Match($mod, '// @compilerOptions\s+(.+)').Groups[1].Value -split '\s+'
foreach ($arch in ([ordered]@{ '32' = 'i686'; '64' = 'x86_64'; 'arm64' = 'aarch64' }).GetEnumerator()) {
    & $clang -std=c++23 -O2 -shared -s -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 `
        -DNTDDI_VERSION=0x0A000008 -D__USE_MINGW_ANSI_STDIO=0 -DWH_MOD "-DWH_MOD_ID=L`"$id`"" "-DWH_MOD_VERSION=L`"$version`"" `
        "$engine\$($arch.Key)\windhawk.lib" -x c++ "$out\winpager.wh.cpp" -include windhawk_api.h -target "$($arch.Value)-w64-mingw32" `
        '-Wl,--export-all-symbols' -o "$out\winpager_$($arch.Key).dll" @options
    if ($LASTEXITCODE) { throw "the mod doesn't compile for $($arch.Value)" }
}
"wrote $out\winpager.wh.cpp ($version); it compiles for x86, x64 and ARM64"
