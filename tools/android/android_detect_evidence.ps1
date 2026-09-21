# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

param([string]$Dev = '192.168.220.33:5555',
      [string]$Apk = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\_android\out\sxcl-debug.apk')
$ErrorActionPreference = 'Continue'
$adb  = 'C:\platform-tools\adb.exe'
if (-not (Test-Path $adb)) { $adb = 'D:\AndroidSdk\platform-tools\adb.exe' }
$pkg  = 'com.silentstudio.sxcl'
$act  = $pkg + '/' + $pkg + '.SxclActivity'
$root = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C'
$out  = Join-Path $root 'build\_android\out\device'
$tmp  = 'C:\sxcl_local'
New-Item -ItemType Directory -Force -Path $out, $tmp | Out-Null
function A([string[]]$argv) { & $adb -s $Dev @argv 2>&1 }
function Log([string]$tag) {
    Write-Output ''
    Write-Output ('---- logcat [' + $tag + '] ----')
    & $adb -s $Dev logcat -d -s sxcl:* 2>&1 |
        Select-String -Pattern 'java-probe|java-discover|gamedir-probe|gamedir-detect|all-files-access|resume:' |
        ForEach-Object { Write-Output $_.Line }
}
function Shot([string]$name) {
    cmd /c "`"$adb`" -s $Dev exec-out screencap -p > `"$tmp\s.png`"";
    if (Test-Path "$tmp\s.png") { Move-Item "$tmp\s.png" (Join-Path $out $name) -Force }
}
function Start-OurApp([string]$route = '') {
    & $adb -s $Dev shell am force-stop $pkg | Out-Null
    Start-Sleep -Milliseconds 600
    if ($route) { & $adb -s $Dev shell am start -n $act --es route $route | Out-Null }
    else        { & $adb -s $Dev shell am start -n $act | Out-Null }
    Start-Sleep -Milliseconds 3500
}

Write-Output '=== 0) fresh install ==='
& $adb -s $Dev uninstall $pkg 2>&1 | Select-Object -Last 1 | ForEach-Object { Write-Output $_ }
& $adb -s $Dev install -r $Apk 2>&1 | Select-Object -Last 1 | ForEach-Object { Write-Output $_ }
Write-Output ('declared permissions: ' + ((& $adb -s $Dev shell dumpsys package $pkg | Select-String 'MANAGE_EXTERNAL_STORAGE|READ_EXTERNAL_STORAGE' | Measure-Object).Count) + ' storage-related line(s)')

Write-Output '=== 1) BEFORE grant: revoke all-files access explicitly ==='
& $adb -s $Dev shell appops set $pkg MANAGE_EXTERNAL_STORAGE deny 2>&1 | Out-Null
& $adb -s $Dev logcat -c | Out-Null
Start-OurApp 'settings'
Shot 'detect_before_grant_settings.png'
Log 'BEFORE grant'

Write-Output '=== 2) GRANT all-files access (same effect as the settings toggle) ==='
& $adb -s $Dev shell appops set $pkg MANAGE_EXTERNAL_STORAGE allow 2>&1 | ForEach-Object { Write-Output $_ }

Write-Output '=== 3) AFTER grant: relaunch and re-scan ==='
& $adb -s $Dev logcat -c | Out-Null
Start-OurApp 'settings'
Shot 'detect_after_grant_settings.png'
Log 'AFTER grant'

Write-Output '=== 4) what the device itself sees now (shell, for cross-check) ==='
& $adb -s $Dev shell "ls -la /storage/emulated/0/FCL/.minecraft/versions/" 2>&1 | ForEach-Object { Write-Output $_ }
Write-Output 'EVIDENCE DONE'
