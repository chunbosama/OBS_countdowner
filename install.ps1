<#
    obs-countdown 安装脚本

    用法（二选一）：
      1. 双击同目录下的 install.bat
      2. 管理员 PowerShell 里执行：
         powershell -ExecutionPolicy Bypass -File .\install.ps1

    卸载：
         powershell -ExecutionPolicy Bypass -File .\install.ps1 -Uninstall

    这个脚本只做三件事：找到 OBS 安装目录、把 dll 和语言文件复制进去、告诉你要重启 OBS。
    不会修改 OBS 的任何配置，也不会碰场景集合。
#>

[CmdletBinding()]
param(
    [string]$ObsDir = "",
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot
$PluginFolder = "obs-plugins\64bit"
$DataFolder = "data\obs-plugins\obs-countdown"
$DllName = "obs-countdown.dll"

function Write-Head($msg) { Write-Host ""; Write-Host $msg -ForegroundColor Cyan }
function Write-Ok($msg) { Write-Host "  [OK] $msg" -ForegroundColor Green }
function Write-WarnLine($msg) { Write-Host "  [!] $msg" -ForegroundColor Yellow }
function Fail($msg) {
    Write-Host ""
    Write-Host "安装失败：$msg" -ForegroundColor Red
    Write-Host ""
    Read-Host "按回车键关闭"
    exit 1
}

# --- 管理员检查 ---
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Fail "需要管理员权限（要写入 C:\Program Files）。请右键 install.bat -> 以管理员身份运行。"
}

# --- 定位 OBS ---
function Find-ObsDir {
    $candidates = @()

    foreach ($key in @("HKLM:\SOFTWARE\OBS Studio", "HKLM:\SOFTWARE\WOW6432Node\OBS Studio")) {
        if (Test-Path $key) {
            $loc = (Get-ItemProperty -Path $key -Name InstallLocation -ErrorAction SilentlyContinue).InstallLocation
            if ($loc) { $candidates += $loc }
        }
    }

    if ($env:ProgramFiles) { $candidates += (Join-Path $env:ProgramFiles "obs-studio") }
    if (${env:ProgramFiles(x86)}) { $candidates += (Join-Path ${env:ProgramFiles(x86)} "obs-studio") }

    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c "bin\64bit\obs64.exe"))) { return $c }
    }
    return $null
}

if ($ObsDir) {
    if (-not (Test-Path (Join-Path $ObsDir "bin\64bit\obs64.exe"))) {
        Fail "在 $ObsDir 找不到 OBS（缺 bin\64bit\obs64.exe）。"
    }
} else {
    $ObsDir = Find-ObsDir
    if (-not $ObsDir) {
        Fail "没找到 OBS Studio。请用 -ObsDir 参数指定安装目录，例如 -ObsDir 'D:\OBS Studio'。"
    }
}

Write-Head "OBS 安装目录：$ObsDir"

# --- OBS 是否在运行 ---
$running = Get-Process -Name "obs64" -ErrorAction SilentlyContinue
if ($running) {
    Write-WarnLine "OBS Studio 正在运行，安装后需要重启 OBS 才会生效。"
}

$dstDll = Join-Path (Join-Path $ObsDir $PluginFolder) $DllName
$dstData = Join-Path $ObsDir $DataFolder

if ($Uninstall) {
    Write-Head "正在卸载"
    if (Test-Path $dstDll) { [System.IO.File]::Delete($dstDll); Write-Ok "已删除 $dstDll" }
    if (Test-Path $dstData) { [System.IO.Directory]::Delete($dstData, $true); Write-Ok "已删除 $dstData" }
    Write-Host ""
    Write-Host "卸载完成，重启 OBS 即可。" -ForegroundColor Green
    Read-Host "按回车键关闭"
    exit 0
}

# --- 安装 ---
$srcDll = Join-Path $Here "obs-plugins\64bit\$DllName"
if (-not (Test-Path $srcDll)) {
    Fail "安装包不完整，缺少 obs-plugins\64bit\$DllName。"
}

Write-Head "正在安装"

$dstPluginDir = Join-Path $ObsDir $PluginFolder
if (-not (Test-Path $dstPluginDir)) { New-Item -ItemType Directory -Path $dstPluginDir -Force | Out-Null }

try {
    Copy-Item -Path $srcDll -Destination $dstPluginDir -Force
    Write-Ok "obs-countdown.dll -> $dstPluginDir"
} catch {
    Fail "复制 dll 失败（OBS 可能正在占用它，请先关闭 OBS 再试）：$($_.Exception.Message)"
}

if (-not (Test-Path $dstData)) { New-Item -ItemType Directory -Path $dstData -Force | Out-Null }
$srcData = Join-Path $Here "data\obs-plugins\obs-countdown"
if (Test-Path $srcData) {
    Copy-Item -Path (Join-Path $srcData "*") -Destination $dstData -Recurse -Force
    Write-Ok "语言文件 -> $dstData"
} else {
    Write-WarnLine "安装包里没有语言文件，界面会显示英文键名。"
}

Write-Host ""
Write-Host "安装完成。" -ForegroundColor Green
Write-Host ""
Write-Host "接下来：" -ForegroundColor Cyan
Write-Host "  1. 完全退出并重新打开 OBS Studio"
Write-Host "  2. 在来源列表里添加「倒计时」"
Write-Host "  3. 设置 -> 热键，找到「倒计时」分类，绑定开始 / 暂停 / 停止 / 重置"
Write-Host ""
Read-Host "按回车键关闭"
