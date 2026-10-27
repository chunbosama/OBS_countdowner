<#
    打包脚本：编译 -> 组装 dist\obs-countdown-<版本>\ -> 压成 zip

    用法：
      powershell -ExecutionPolicy Bypass -File .\package.ps1
      powershell -ExecutionPolicy Bypass -File .\package.ps1 -SkipBuild   # 复用已编译的 dll

    产出的 zip 直接发给别人，对方解压后右键 install.bat「以管理员身份运行」即可。
#>

[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [string]$ObsDir = "C:\Program Files\obs-studio",
    [string]$QtDir = "",
    [switch]$NoQuickBar
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$DistRoot = Join-Path $Root "dist"

function Write-Head($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg) { Write-Host "    $msg" -ForegroundColor Green }

# --- 版本号 ---
$verFile = Join-Path $Root "src\version.h"
$ver = $null
if ((Get-Content $verFile -Raw) -match 'PLUGIN_VERSION\s+"([^"]+)"') { $ver = $matches[1] }
if (-not $ver) { throw "无法从 src\version.h 读取 PLUGIN_VERSION。" }
Write-Head "obs-countdown $ver"

# --- 编译 ---
if (-not $SkipBuild) {
    $buildArgs = @{ ObsDir = $ObsDir }
    if ($QtDir) { $buildArgs["QtDir"] = $QtDir }
    if ($NoQuickBar) { $buildArgs["NoQuickBar"] = $true }
    & (Join-Path $Root "build.ps1") @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "编译失败，终止打包。" }
}

$dll = Join-Path $Root "build\obs-countdown.dll"
if (-not (Test-Path $dll)) { throw "找不到 build\obs-countdown.dll，请先运行 build.ps1。" }

# --- 组装 ---
Write-Head "组装发布目录"
$stage = Join-Path $DistRoot ("obs-countdown-" + $ver)
if (Test-Path $stage) { [System.IO.Directory]::Delete($stage, $true) }

$dirs = @(
    (Join-Path $stage "obs-plugins\64bit"),
    (Join-Path $stage "data\obs-plugins\obs-countdown\locale")
)
foreach ($d in $dirs) { New-Item -ItemType Directory -Path $d -Force | Out-Null }

Copy-Item -Path $dll -Destination (Join-Path $stage "obs-plugins\64bit\obs-countdown.dll") -Force
Copy-Item -Path (Join-Path $Root "data\locale\*.ini") -Destination (Join-Path $stage "data\obs-plugins\obs-countdown\locale") -Force

foreach ($f in @("install.ps1", "install.bat", "INSTALL.txt", "LICENSE.txt")) {
    $src = Join-Path $Root $f
    if (Test-Path $src) { Copy-Item -Path $src -Destination $stage -Force }
    else { Write-Host "    [!] 缺少 $f，已跳过" -ForegroundColor Yellow }
}
Write-Ok $stage

# --- 压缩 ---
Write-Head "生成 zip"
$zip = Join-Path $DistRoot ("obs-countdown-" + $ver + "-win64.zip")
if (Test-Path $zip) { [System.IO.File]::Delete($zip) }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal
$hash = (Get-FileHash -Path $zip -Algorithm SHA256).Hash
Write-Ok $zip
Write-Ok ("SHA256 " + $hash)
Write-Host ""
Write-Host "分享时请连同源码地址一起给出（本插件为 GPL-2.0，分发二进制需提供源码获取方式）。" -ForegroundColor Cyan
