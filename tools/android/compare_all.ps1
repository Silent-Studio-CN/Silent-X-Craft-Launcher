# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

param([string]$Device = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\_android\out\device')
$ErrorActionPreference = 'Continue'
$ref = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\ref'
$cmp = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\tools\ui_compare.py'
$routes = @('home','versions','tasks','keymap','multiplayer','settings','download_config','download_progress','launch')
$lines = @()
foreach ($r in $routes) {
  $a = Join-Path $ref ('py_' + $r + '.png')
  $b = Join-Path $Device ($r + '.png')
  if (-not (Test-Path $b)) { $lines += ($r + ': NO DEVICE SHOT'); Write-Output ($r + ': NO DEVICE SHOT'); continue }
  Write-Output ('===== ' + $r + ' =====')
  $o = (& python $cmp $a $b) 2>&1 | Out-String
  Write-Output $o
  $d = ($o -split [Environment]::NewLine) | Where-Object { $_ -match 'DIFF>' } | Select-Object -First 1
  $lines += ($r + ': ' + $d)
}
Write-Output '===== SUMMARY ====='
$lines | ForEach-Object { Write-Output $_ }