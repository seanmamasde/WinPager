# Rasterizes assets/icon*.svg with resvg (scoop install resvg) and packs res/WinPager.ico (PNG entries).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$tmp = Join-Path ([IO.Path]::GetTempPath()) 'WinPager-icon'
New-Item -ItemType Directory -Force $tmp | Out-Null
$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 256
$pngs = foreach ($s in $sizes) {
    $svg = if ($s -le 24) { 'icon-small.svg' } else { 'icon.svg' }
    $out = Join-Path $tmp "$s.png"
    resvg -w $s -h $s (Join-Path $root "assets\$svg") $out
    if ($LASTEXITCODE) { throw "resvg failed for $s" }
    , [IO.File]::ReadAllBytes($out)
}
$ms = New-Object IO.MemoryStream
$bw = New-Object IO.BinaryWriter $ms
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i] % 256
    $bw.Write([byte]$s); $bw.Write([byte]$s); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
[IO.File]::WriteAllBytes((Join-Path $root 'res\WinPager.ico'), $ms.ToArray())
Copy-Item (Join-Path $tmp '256.png') (Join-Path $root 'assets\icon-256.png') -Force
"wrote res\WinPager.ico ($($sizes -join ', '))"
