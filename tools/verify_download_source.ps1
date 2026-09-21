<#
(C) Silent X Craft Launcher
Copyright by SilentStudio.
All rights reserved.
#>
# verify_download_source.ps1 —— 下载源/设置持久化的**确定性**验收(桌面离屏,不污染用户配置)
#
# 验四件事:
#   1) 默认(设置里没写过 download.source) -> BMCLAPI 排第一,且真能拿到清单
#   2) download.source=mojang             -> 官方排第一
#   3) download.source=mojang + 官方地址被换成连不上的地址
#        -> 自动切 BMCLAPI,并给出"官方源超时,已切换"的提示(notice)
#   4) 设置文件路径的唯一权威:读的和写的是**同一个文件**
#        (SXCL_CONFIG_DIR 钉住目录 -> 启动恢复读得到 theme / 版本页读得到 source)
#
# 为什么要有:SXCL 的设置在 Android 上曾经"读一份、写另一份",用户看到"设置关闭重开打回原形"。
# 这个脚本把"路径一致性 + 源顺序 + 超时切换"变成可重复的三条命令。
param(
  [string]$Exe = "$PSScriptRoot\..\build-ui\src\ui\Release\sxcl-ui.exe",
  [string]$Evidence = "D:\SilentStudio\_clip_evidence\src_verify"
)
$ErrorActionPreference = 'Continue'
$Exe = (Resolve-Path $Exe).Path
Remove-Item $Evidence -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $Evidence | Out-Null

function Run-Case([string]$name, [string]$settingsBody, [string]$officialOverride) {
  $dir = Join-Path $Evidence $name
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $sp = Join-Path $dir 'settings.conf'
  if ($settingsBody) { Set-Content -LiteralPath $sp -Value $settingsBody -Encoding UTF8 }
  $env:SXCL_CONFIG_DIR = $dir          # 核心库的配置目录(= 唯一权威)
  $env:SXCL_UI_ROUTE = 'versions'
  $env:SXCL_UI_TRACE = '1'
  $env:SXCL_UI_GAME_DIR = (Join-Path $dir 'gamedir')
  if ($officialOverride) { $env:SXCL_UI_MANIFEST_OFFICIAL = $officialOverride }
  else { Remove-Item Env:SXCL_UI_MANIFEST_OFFICIAL -ErrorAction SilentlyContinue }
  Remove-Item Env:SXCL_UI_MANIFEST_URL -ErrorAction SilentlyContinue
  Remove-Item Env:SXCL_UI_DATA_DIR -ErrorAction SilentlyContinue
  Remove-Item Env:SXCL_UI_SETTINGS -ErrorAction SilentlyContinue
  Remove-Item Env:SXCL_UI_DOWNLOAD_SOURCE -ErrorAction SilentlyContinue
  $log = Join-Path $dir 'stderr.log'
  $p = Start-Process -FilePath $Exe -ArgumentList '-platform','offscreen' -PassThru -WorkingDirectory (Split-Path $Exe) -RedirectStandardError $log
  try { Start-Sleep -Seconds 16 } finally { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
  Write-Output "===== CASE $name ====="
  Select-String -Path $log -Pattern 'settings \| 文件=|清单源=|官方源超时|版本页:' |
    Select-Object -Last 4 | ForEach-Object { $_.Line.Trim() }
}

Run-Case 'default'        ''                                    $null
Run-Case 'mojang'         'download.source=mojang'               $null
Run-Case 'mojang-timeout' 'download.source=mojang'               'http://127.0.0.1:9/dead'

# 4) 设置页真的往"同一个文件"里写
$dir = Join-Path $Evidence 'write-check'
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$env:SXCL_CONFIG_DIR = $dir
$env:SXCL_UI_ROUTE = 'settings'
$env:SXCL_UI_TRACE = '1'
Remove-Item Env:SXCL_UI_SETTINGS -ErrorAction SilentlyContinue
$log = Join-Path $dir 'stderr.log'
$p = Start-Process -FilePath $Exe -ArgumentList '-platform','offscreen' -PassThru -WorkingDirectory (Split-Path $Exe) -RedirectStandardError $log
try { Start-Sleep -Seconds 14 } finally { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
Write-Output "===== CASE write-check(设置页构造时就会落盘几个键)====="
Select-String -Path $log -Pattern 'settings \| 文件=' | Select-Object -Last 1 | ForEach-Object { $_.Line.Trim() }
$written = Join-Path $dir 'settings.conf'
Write-Output ("WROTE=" + (Test-Path $written) + "  PATH=" + $written)
if (Test-Path $written) { Get-Content $written | Select-Object -First 12 }
