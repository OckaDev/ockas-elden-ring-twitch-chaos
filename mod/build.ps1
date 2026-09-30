# Build EldenChaos.dll (Release x64)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Find-CMake {
    $paths = New-Object System.Collections.Generic.List[string]
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { [void]$paths.Add([string]$cmd.Source) }
    foreach ($p in @(
        "$env:ProgramFiles\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
    )) {
        if (Test-Path $p) { [void]$paths.Add($p) }
    }
    if ($paths.Count -eq 0) {
        throw "CMake not found. Install CMake from https://cmake.org/download/"
    }
    return [string]$paths[0]
}

function Find-Ninja {
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return [string]$cmd.Source }

    $wingetNinja = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter "ninja.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if ($wingetNinja) { return $wingetNinja }

    foreach ($p in @(
        "$env:LOCALAPPDATA\Microsoft\WinGet\Links\ninja.exe",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe",
        "$env:ProgramFiles\Ninja\ninja.exe",
        "$env:ProgramFiles\ninja\ninja.exe"
    )) {
        if (Test-Path $p) { return $p }
    }
    throw "Ninja not found. Install with: winget install Ninja-build.Ninja"
}

function Find-VcVars64 {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "Visual Studio Installer (vswhere) not found."
    }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) {
        throw "Visual Studio 2022 with C++ tools not found. Install 'Desktop development with C++'."
    }
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found at $vcvars"
    }
    return $vcvars
}

function Find-LatestWinSdk {
    $includeRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\Include"
    if (-not (Test-Path $includeRoot)) {
        throw "Windows 10/11 SDK not found under Windows Kits\10\Include"
    }
    $latest = Get-ChildItem $includeRoot -Directory |
        Where-Object { $_.Name -match '^10\.' } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if (-not $latest) {
        throw "No Windows SDK version folders found in $includeRoot"
    }
    return $latest.Name
}

$cmake = Find-CMake
$ninja = Find-Ninja
$vcvars = Find-VcVars64
$sdkVer = Find-LatestWinSdk
$buildDir = Join-Path $PSScriptRoot "build"
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$ninjaDir = Split-Path $ninja -Parent

Write-Host "CMake:  $cmake"
Write-Host "Ninja:  $ninja"
Write-Host "VCVars: $vcvars"
Write-Host "WinSDK: $sdkVer"
Write-Host ""

if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}

$tmp = Join-Path $env:TEMP "elden_chaos_build.cmd"
$cmdText = @"
@echo off
setlocal
call "$vcvars"
if errorlevel 1 exit /b 1
set "WindowsSdkDir=$kits\"
set "WindowsSDKVersion=$sdkVer\"
set "WindowsSDKLibVersion=$sdkVer\"
set "INCLUDE=%INCLUDE%;$kits\Include\$sdkVer\ucrt;$kits\Include\$sdkVer\um;$kits\Include\$sdkVer\shared;$kits\Include\$sdkVer\winrt"
set "LIB=%LIB%;$kits\Lib\$sdkVer\ucrt\x64;$kits\Lib\$sdkVer\um\x64"
set "PATH=%PATH%;$ninjaDir"
"$cmake" -S "$PSScriptRoot" -B "$buildDir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl
if errorlevel 1 exit /b 1
"$cmake" --build "$buildDir"
if errorlevel 1 exit /b 1
"@
Set-Content -Path $tmp -Value $cmdText -Encoding ASCII

& cmd.exe /c $tmp
$exit = $LASTEXITCODE
Remove-Item $tmp -ErrorAction SilentlyContinue

if ($exit -ne 0) {
    throw "Build failed with exit code $exit"
}

$dllCandidates = @(
    (Join-Path $buildDir "EldenChaos.dll"),
    (Join-Path $buildDir "Release\EldenChaos.dll")
)
$dll = $dllCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $dll) {
    throw "Build finished but EldenChaos.dll was not found in $buildDir"
}

$cfgCandidates = @(
    (Join-Path $buildDir "config\signatures.json"),
    (Join-Path $buildDir "Release\config\signatures.json"),
    (Join-Path $PSScriptRoot "config\signatures.json")
)
$cfg = $cfgCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

Write-Host ""
Write-Host "Built successfully:" -ForegroundColor Green
Write-Host "  $dll"
if ($cfg) { Write-Host "  $cfg" }
