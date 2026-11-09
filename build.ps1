param([ValidateSet('Release', 'Debug')][string]$Config = 'Release')
$ErrorActionPreference = 'Stop'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC build tools not found (winget install Microsoft.VisualStudio.BuildTools)' }
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$build = Join-Path $PSScriptRoot "build\$Config"
cmake -S $PSScriptRoot -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build
exit $LASTEXITCODE
