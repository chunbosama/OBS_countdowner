<#
    Build script for obs-countdown (Windows x64 only)

    What it does:
      1. Locates Visual Studio 2022 (MSVC x64 toolchain) and the installed OBS Studio.
      2. Makes sure the libobs headers are available (clones a shallow copy of the
         matching obs-studio tag from a mirror when .deps\obs-studio is missing).
      3. Builds an import library (obs.lib) from the installed obs.dll, so the plugin
         can be linked without a full OBS build tree.
      4. Compiles src\*.cpp into build\obs-countdown.dll and stages the locale files.

    Usage:
      powershell -ExecutionPolicy Bypass -File build.ps1
      powershell -ExecutionPolicy Bypass -File build.ps1 -Install
      powershell -ExecutionPolicy Bypass -File build.ps1 -Clean
#>

[CmdletBinding()]
param(
    [string]$ObsDir = "C:\Program Files\obs-studio",
    [string]$ObsSource = "",
    [string]$ObsMirror = "https://gitee.com/mirrors/obs-studio",
    [string]$ApiTag = "",
    [string]$QtDir = "",
    [switch]$NoQuickBar,
    [switch]$Install,
    [switch]$Clean,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$DepsDir = Join-Path $Root ".deps"
$SdkDir = Join-Path $DepsDir "sdk"
$SdkInc = Join-Path $SdkDir "include"
$SdkLib = Join-Path $SdkDir "lib"
$BuildDir = Join-Path $Root "build"
$ObjDir = Join-Path $BuildDir "obj"

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    $msg" -ForegroundColor Green }
function Write-Warn2($msg){ Write-Host "    $msg" -ForegroundColor Yellow }

# ---------------------------------------------------------------------------
# 1. Locate the toolchain
# ---------------------------------------------------------------------------
Write-Step "Locating Visual Studio 2022"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 with the 'Desktop development with C++' workload."
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio installation with the MSVC x64 toolset was found."
}
$msvcRoot = Join-Path $vsPath "VC\Tools\MSVC"
$msvcVer = Get-ChildItem -Path $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
$binDir = Join-Path $msvcVer.FullName "bin\Hostx64\x64"
$dumpbin = Join-Path $binDir "dumpbin.exe"
$libexe = Join-Path $binDir "lib.exe"
$cl = Join-Path $binDir "cl.exe"
if (-not (Test-Path $cl)) { throw "cl.exe not found at $cl" }
Write-Ok "$($msvcVer.Name)"

# Windows SDK (headers + libs). vcvars64.bat is avoided on purpose: it shells out to
# reg.exe, which is blocked in some restricted environments.
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10"
if (-not (Test-Path $sdkRoot)) { throw "Windows 10/11 SDK not found at $sdkRoot" }
$sdkVer = Get-ChildItem -Path (Join-Path $sdkRoot "Include") -Directory |
    Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkVer) { throw "No Windows SDK version found under $(Join-Path $sdkRoot 'Include')" }
$winSdkInc = $sdkVer.FullName
$winSdkLibRoot = Join-Path $sdkRoot ("Lib\" + $sdkVer.Name)
$winSdkBin = Join-Path $sdkRoot ("bin\" + $sdkVer.Name + "\x64")
Write-Ok "Windows SDK $($sdkVer.Name)"

$msvcInc = Join-Path $msvcVer.FullName "include"
$msvcLib = Join-Path $msvcVer.FullName "lib\x64"
$clInc = @(
    $msvcInc,
    (Join-Path $winSdkInc "ucrt"),
    (Join-Path $winSdkInc "um"),
    (Join-Path $winSdkInc "shared"),
    (Join-Path $winSdkInc "winrt"),
    (Join-Path $winSdkInc "cppwinrt")
) -join ";"
$clLib = @(
    $msvcLib,
    (Join-Path $winSdkLibRoot "ucrt\x64"),
    (Join-Path $winSdkLibRoot "um\x64")
) -join ";"

# ---------------------------------------------------------------------------
# 2. Locate OBS and the libobs headers
# ---------------------------------------------------------------------------
Write-Step "Locating OBS Studio"

$obsDll = Join-Path $ObsDir "bin\64bit\obs.dll"
$obsExe = Join-Path $ObsDir "bin\64bit\obs64.exe"
if (-not (Test-Path $obsDll)) { throw "obs.dll not found at $obsDll (use -ObsDir to point at your OBS install)." }

$obsVersion = (Get-Item $obsExe).VersionInfo.FileVersion
if ($obsVersion -match "^\s*([0-9]+\.[0-9]+\.[0-9]+)") { $obsVersion = $matches[1] }
Write-Ok "OBS $obsVersion at $ObsDir"

# API tag 决定编译时使用的 libobs 版本，同时决定插件能加载到哪些 OBS 上：
# 插件返回的 API 版本必须 <= 目标机器的 OBS 版本，否则会被拒绝加载。
$cloneTag = if ($ApiTag) { $ApiTag } else { $obsVersion }
if ($cloneTag -ne $obsVersion) {
    if ([version]$cloneTag -gt [version]$obsVersion) {
        Write-Warn2 "API tag $cloneTag 高于本机 OBS $obsVersion，这样编出来的插件在本机无法加载。"
    } else {
        Write-Ok "API tag $cloneTag (低于本机 $obsVersion，编出的插件可兼容 $cloneTag 及以上的 OBS)"
    }
}

if ([string]::IsNullOrEmpty($ObsSource)) {
    if ($cloneTag -eq $obsVersion) {
        $ObsSource = Join-Path $DepsDir "obs-studio"
    } else {
        $ObsSource = Join-Path $DepsDir ("obs-studio-" + $cloneTag)
    }
}
$libobsInc = Join-Path $ObsSource "libobs"

if (-not (Test-Path (Join-Path $libobsInc "obs.h"))) {
    Write-Step "libobs headers not found, cloning obs-studio $cloneTag (shallow)"
    $git = (Get-Command git -ErrorAction SilentlyContinue).Source
    if (-not $git) {
        $candidate = Join-Path $env:ProgramFiles "Git\cmd\git.exe"
        if (Test-Path $candidate) { $git = $candidate }
    }
    if (-not $git) {
        throw "git not found. Clone obs-studio $cloneTag manually into $ObsSource, or install Git."
    }
    if (-not (Test-Path $DepsDir)) { New-Item -ItemType Directory -Path $DepsDir | Out-Null }
    & $git clone --depth 1 --branch $cloneTag $ObsMirror $ObsSource
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to clone obs-studio. Download the $cloneTag source manually into $ObsSource."
    }
}
Write-Ok "libobs headers: $libobsInc"

# ---------------------------------------------------------------------------
# 3. Prepare directories / generated config
# ---------------------------------------------------------------------------
if ($Clean) {
    Write-Step "Cleaning build output"
    if (Test-Path $BuildDir) {
        [System.IO.Directory]::Delete($BuildDir, $true)
    }
    $staleLib = Join-Path $SdkLib "obs.lib"
    if (Test-Path $staleLib) { [System.IO.File]::Delete($staleLib) }
}

foreach ($d in @($SdkInc, $SdkLib, $BuildDir, $ObjDir)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
}

Write-Step "Writing obsconfig.h"
$obsconfig = @'
#pragma once

#define OBS_DATA_PATH "data"
#define OBS_PLUGIN_PATH "obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins"
#define OBS_INSTALL_PREFIX ""

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
'@
Set-Content -Path (Join-Path $SdkInc "obsconfig.h") -Value $obsconfig -Encoding ASCII

# ---------------------------------------------------------------------------
# 4. Import libraries from the shipped DLLs
#    Plugins are built without the OBS/Qt SDK: only the .dll files are present
#    on a normal install, so the import libraries are regenerated from them.
# ---------------------------------------------------------------------------
function New-ImportLib([string]$dllPath, [string]$libName, [string]$outLib)
{
    $defPath = Join-Path $SdkDir ($libName + ".def")
    $lines = & $dumpbin /exports $dllPath
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $dllPath" }

    $names = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    foreach ($line in $lines) {
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([A-Za-z_][A-Za-z0-9_]*)\s*(=|$)') {
            $n = $matches[1]
            if (-not $seen.ContainsKey($n)) {
                $seen[$n] = $true
                $names.Add($n) | Out-Null
            }
        }
    }
    if ($names.Count -lt 5) { throw "Unexpectedly few exports parsed from $dllPath ($($names.Count))." }

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("LIBRARY $libName.dll")
    [void]$sb.AppendLine("EXPORTS")
    foreach ($n in $names) { [void]$sb.AppendLine($n) }
    Set-Content -Path $defPath -Value $sb.ToString() -Encoding ASCII

    & $libexe /nologo /def:$defPath /machine:x64 /name:$libName.dll /out:$outLib
    if ($LASTEXITCODE -ne 0) { throw "lib.exe failed to build $libName.lib" }
    Write-Ok "$($names.Count) exports -> $libName.lib"
}

$obsLib = Join-Path $SdkLib "obs.lib"
$needLib = $true
if ((Test-Path $obsLib) -and -not $Clean) {
    if ((Get-Item $obsLib).LastWriteTime -ge (Get-Item $obsDll).LastWriteTime) { $needLib = $false }
}

if ($needLib) {
    Write-Step "Generating obs.lib from obs.dll"
    New-ImportLib $obsDll "obs" $obsLib
}

# ---------------------------------------------------------------------------
# 5. Qt6 + obs-frontend-api (only needed for the quick control dock)
# ---------------------------------------------------------------------------
$useQt = -not $NoQuickBar
$qtRoot = ""
$frontendLib = Join-Path $SdkLib "obs-frontend-api.lib"

if ($useQt) {
    Write-Step "Locating Qt6 (needed by the quick control dock)"

    $candidates = New-Object System.Collections.Generic.List[string]
    if ($QtDir) { $candidates.Add($QtDir) }
    if ($env:QTDIR) { $candidates.Add($env:QTDIR) }
    foreach ($base in @((Join-Path $DepsDir "Qt"), "C:\Qt",
                        (Join-Path $env:USERPROFILE ".workbuddy\binaries\qt"),
                        (Join-Path $env:USERPROFILE "Qt"))) {
        if (-not (Test-Path $base)) { continue }
        foreach ($ver in (Get-ChildItem -Path $base -Directory -ErrorAction SilentlyContinue)) {
            foreach ($kit in (Get-ChildItem -Path $ver.FullName -Directory -ErrorAction SilentlyContinue)) {
                $candidates.Add($kit.FullName)
            }
        }
    }

    foreach ($c in $candidates) {
        if ((Test-Path (Join-Path $c "include\QtCore\QObject")) -and
            (Test-Path (Join-Path $c "lib\Qt6Core.lib"))) { $qtRoot = $c; break }
    }

    if (-not $qtRoot) {
        throw @"
Qt6 development files not found.

The quick control dock needs Qt6 headers and import libraries. The plugin itself
(multi-instance countdowns + per-source hotkeys) builds fine without them:

    powershell -ExecutionPolicy Bypass -File build.ps1 -NoQuickBar

To build the dock as well, install Qt 6.8 (the version OBS ships) and point -QtDir at it:

    pip install aqtinstall
    aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --archives qtbase --outputdir C:\Qt

    powershell -ExecutionPolicy Bypass -File build.ps1 -QtDir "C:\Qt\6.8.3\msvc2022_64"
"@
    }
    Write-Ok $qtRoot

    $frontendDll = Join-Path $ObsDir "bin\64bit\obs-frontend-api.dll"
    if (-not (Test-Path $frontendDll)) {
        throw "obs-frontend-api.dll not found at $frontendDll"
    }
    $needFrontendLib = $true
    if ((Test-Path $frontendLib) -and -not $Clean) {
        if ((Get-Item $frontendLib).LastWriteTime -ge (Get-Item $frontendDll).LastWriteTime) { $needFrontendLib = $false }
    }
    if ($needFrontendLib) {
        Write-Step "Generating obs-frontend-api.lib"
        New-ImportLib $frontendDll "obs-frontend-api" $frontendLib
    }

    $frontendInc = Join-Path $ObsSource "frontend\api"
    if (-not (Test-Path (Join-Path $frontendInc "obs-frontend-api.h"))) {
        throw "obs-frontend-api.h not found at $frontendInc (re-clone obs-studio)."
    }

    $qtInc = @(
        (Join-Path $qtRoot "include"),
        (Join-Path $qtRoot "include\QtCore"),
        (Join-Path $qtRoot "include\QtGui"),
        (Join-Path $qtRoot "include\QtWidgets")
    ) -join ";"
    $qtLib = Join-Path $qtRoot "lib"
} else {
    Write-Ok "Skipping Qt: building without the quick control dock (-NoQuickBar)"
}

# ---------------------------------------------------------------------------
# 6. Core logic tests (纯 C++，不依赖 libobs，秒级跑完)
# ---------------------------------------------------------------------------
if (-not $SkipTests) {
    $testSrc = Join-Path $Root "tests\core_test.cpp"
    if (Test-Path $testSrc) {
        Write-Step "Running core logic tests"
        $testExe = Join-Path $BuildDir "core_test.exe"
        $testBat = Join-Path $BuildDir "run_tests.bat"
        $tbat = @"
@echo off
setlocal
set "PATH=$binDir;$winSdkBin;%PATH%"
set "INCLUDE=$clInc"
set "LIB=$clLib"
cl /nologo /EHsc /std:c++20 /utf-8 /MD /O2 /Fe:"$testExe" /Fo"$ObjDir\\" "$testSrc"
if errorlevel 1 exit /b 1
"$testExe"
exit /b %errorlevel%
"@
        Set-Content -Path $testBat -Value $tbat -Encoding ASCII
        & cmd /c "`"$testBat`"" | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Core logic tests failed, build aborted." }
        Write-Ok "tests passed"
    }
}

# ---------------------------------------------------------------------------
# 7. Compile plugin
# ---------------------------------------------------------------------------
Write-Step "Compiling plugin"

$sources = @(Get-ChildItem -Path (Join-Path $Root "src") -Filter *.cpp | ForEach-Object { $_.FullName })
if (-not $sources) { throw "No sources found in src\" }

# 版本资源单独用 rc.exe 编译成 .res，再交给链接器（cl 不认 .rc 作为源文件）
$rcFile = Join-Path $Root "src\plugin.rc"
$resPath = Join-Path $ObjDir "plugin.res"
$rcStep = ""
$resArg = ""
if (Test-Path $rcFile) {
    $rcStep = 'rc /nologo /fo"' + $resPath + '" "' + $rcFile + '"'
    $resArg = '"' + $resPath + '"'
}

# Qt 相关参数：不建 Dock 时只加一个宏，其余一概不引
if ($useQt) {
    $qtDefs = ""
    $qtIncArgs = ' /I"' + (Join-Path $qtRoot "include") + '"' +
                 ' /I"' + (Join-Path $qtRoot "include\QtCore") + '"' +
                 ' /I"' + (Join-Path $qtRoot "include\QtGui") + '"' +
                 ' /I"' + (Join-Path $qtRoot "include\QtWidgets") + '"' +
                 ' /I"' + $frontendInc + '"'
    $qtLibArgs = ' Qt6Core.lib Qt6Gui.lib Qt6Widgets.lib obs-frontend-api.lib' +
                 ' user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib winmm.lib imm32.lib dwmapi.lib' +
                 ' /LIBPATH:"' + $qtLib + '"'
} else {
    $qtDefs = ' /DCD_NO_QUICKBAR'
    $qtIncArgs = ""
    $qtLibArgs = ""
}

$outDll = Join-Path $BuildDir "obs-countdown.dll"
$batPath = Join-Path $BuildDir "compile.bat"

$srcArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "
$bat = @"
@echo off
setlocal
set "PATH=$binDir;$winSdkBin;%PATH%"
set "INCLUDE=$clInc"
set "LIB=$clLib"
$rcStep
if errorlevel 1 exit /b 1
cl /nologo /LD /EHsc /std:c++20 /utf-8 /MD /O2 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /Zc:__cplusplus$qtDefs ^
   /I"$SdkInc" /I"$libobsInc"$qtIncArgs ^
   /Fo"$ObjDir\\" $srcArgs $resArg ^
   /link /LIBPATH:"$SdkLib" obs.lib$qtLibArgs /OUT:"$outDll" /IMPLIB:"$ObjDir\obs-countdown.lib" /PDB:"$ObjDir\obs-countdown.pdb"
exit /b %errorlevel%
"@
Set-Content -Path $batPath -Value $bat -Encoding ASCII

& cmd /c "`"$batPath`"" | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Compilation failed." }
Write-Ok "Built $outDll"

# ---------------------------------------------------------------------------
# 8. Stage locale files
# ---------------------------------------------------------------------------
Write-Step "Staging data files"
$stageData = Join-Path $BuildDir "data\obs-plugins\obs-countdown\locale"
if (-not (Test-Path $stageData)) { New-Item -ItemType Directory -Path $stageData -Force | Out-Null }
Copy-Item -Path (Join-Path $Root "data\locale\*.ini") -Destination $stageData -Force
Write-Ok "$stageData"

# ---------------------------------------------------------------------------
# 9. Optional install
# ---------------------------------------------------------------------------
if ($Install) {
    Write-Step "Installing into $ObsDir"
    $dstPlugin = Join-Path $ObsDir "obs-plugins\64bit"
    $dstData = Join-Path $ObsDir "data\obs-plugins\obs-countdown\locale"
    if (-not (Test-Path $dstData)) { New-Item -ItemType Directory -Path $dstData -Force | Out-Null }
    try {
        Copy-Item -Path $outDll -Destination $dstPlugin -Force
        Copy-Item -Path (Join-Path $Root "data\locale\*.ini") -Destination $dstData -Force
    } catch {
        throw "Copy failed. Close OBS Studio and run this script from an elevated shell (Run as administrator)."
    }
    Write-Ok "Installed. Restart OBS Studio."
}

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "  dll : $outDll"
Write-Host "  data: $(Join-Path $BuildDir 'data')"
Write-Host ""
Write-Host "Manual install: copy obs-countdown.dll to <OBS>\obs-plugins\64bit\ and"
Write-Host "the data folder to <OBS>\data\obs-plugins\obs-countdown\."
