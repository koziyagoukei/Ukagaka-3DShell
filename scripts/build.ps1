<#
.SYNOPSIS
  Builds ukagaka3d with MSVC + CMake (Ninja Multi-Config) without opening a Developer prompt.

.DESCRIPTION
  x64 : 3D runtime (ukagaka3d.exe), tools, x64 tests and an x64 build of the SAORI bridge.
  x86 : ukagaka3d_saori.dll for SSP (32-bit) and the x86 tests. Uses the x64
        runtime and test assets, so x64 is always built first when both are requested.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build.ps1                       # x64 + x86, Debug + Release
  powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release -Test
  powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Clean -Test          # clean build + all tests
  powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Test -NoGuiTests     # skip tests that open windows
  powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Arch x86
#>
param(
    [ValidateSet('Debug', 'Release', 'All')]
    [string]$Config = 'All',
    [ValidateSet('x64', 'x86', 'All')]
    [string]$Arch = 'All',
    [switch]$Test,
    [switch]$NoGuiTests,
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'

if ($Arch -eq 'All') {
    # Each architecture needs its own vcvars environment: run them as child processes.
    foreach ($a in @('x64', 'x86')) {
        $childArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Config', $Config, '-Arch', $a)
        if ($Test) { $childArgs += '-Test' }
        if ($NoGuiTests) { $childArgs += '-NoGuiTests' }
        if ($Clean) { $childArgs += '-Clean' }
        & powershell @childArgs
        if ($LASTEXITCODE -ne 0) { throw "build.ps1 -Arch $a failed with exit code $LASTEXITCODE" }
    }
    exit 0
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found. Install Visual Studio (Build Tools) with the C++ workload.' }
# Prefer a released Visual Studio / Build Tools; use a preview only when nothing else is installed.
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    $vs = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vs) { throw 'No Visual Studio installation with the MSVC toolset was found.' }
Write-Host "Visual Studio: $vs ($Arch)"

# Import the vcvars environment into this PowerShell session.
$bits = $(if ($Arch -eq 'x64') { '64' } else { '32' })
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars$bits.bat"
# Use %ComSpec% explicitly: other "cmd" executables (e.g. MSYS2) may shadow cmd.exe on PATH.
& $env:ComSpec /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
# English compiler messages (stable for log parsing / CI).
$env:VSLANG = '1033'
# Prefer the CMake/Ninja bundled with Visual Studio over other copies on PATH (e.g. MSYS2).
$cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = "$cmakeDir;$ninjaDir;$env:PATH"

function Invoke-Step([string]$exe, [string[]]$arguments) {
    Write-Host "> $exe $($arguments -join ' ')" -ForegroundColor Cyan
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { throw "$exe failed with exit code $LASTEXITCODE" }
}

$root = Split-Path $PSScriptRoot -Parent
$preset = "msvc-$Arch"
$suffix = $(if ($Arch -eq 'x86') { '-x86' } else { '' })
Push-Location $root
try {
    if ($Clean -and (Test-Path "build\$preset")) {
        Write-Host "Removing build\$preset"
        Remove-Item -Recurse -Force "build\$preset"
    }
    Invoke-Step 'cmake' @('--preset', $preset)
    $configs = if ($Config -eq 'All') { @('debug', 'release') } else { @($Config.ToLower()) }
    foreach ($c in $configs) {
        Invoke-Step 'cmake' @('--build', '--preset', "$c$suffix")
        if ($Test) {
            $ctestArgs = @('--preset', "$c$suffix")
            if ($NoGuiTests) { $ctestArgs += @('-LE', 'gui') }
            Invoke-Step 'ctest' $ctestArgs
        }
    }
    Write-Host "Done ($Arch)." -ForegroundColor Green
}
finally {
    Pop-Location
}
