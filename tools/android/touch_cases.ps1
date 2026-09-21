# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

param([string]$Tag = 'before', [string]$Dev = '192.168.220.13:5555', [int]$Tries = 4)
$ErrorActionPreference = 'Continue'
$adb = 'D:\AndroidSdk\platform-tools\adb.exe'
$pkg = 'com.silentstudio.sxcl'
$act = $pkg + '/' + $pkg + '.SxclActivity'
$dir = 'D:\sxcl_local\touch'
New-Item -ItemType Directory -Force -Path $dir, 'C:\sxcl_local' | Out-Null

function FocusIsOurs($Dev) {
  $f = ((& $adb -s $Dev shell dumpsys window 2>&1 | Select-String 'mCurrentFocus' | Select-Object -First 1).Line)
  return ($f -ne $null) -and ($f -like ('*' + $pkg + '*'))
}

$cases = @(
  @{ n = 'home_scroll';     route = 'home';     x1 = 1200; y1 = 1300; x2 = 1200; y2 = 400 },
  @{ n = 'settings_scroll'; route = 'settings'; x1 = 1200; y1 = 1300; x2 = 1200; y2 = 400 },
  @{ n = 'versions_list';   route = 'versions'; x1 = 1500; y1 = 900;  x2 = 1500; y2 = 300 },
  @{ n = 'keymap_list';     route = 'keymap';   x1 = 1700; y1 = 900;  x2 = 1700; y2 = 300 },
  @{ n = 'download_config'; route = 'download_config'; x1 = 1200; y1 = 1300; x2 = 1200; y2 = 400 },
  @{ n = 'navpanel';        route = 'home';     x1 = 250;  y1 = 1200; x2 = 250;  y2 = 400 }
)
foreach ($c in $cases) {
  $name = $c.n; $ok = $false
  for ($t = 0; $t -lt $Tries -and -not $ok; $t++) {
    # the notification shade (or another app) may be holding focus: dismiss the shade first
    $f0 = ((& $adb -s $Dev shell dumpsys window 2>&1 | Select-String 'mCurrentFocus' | Select-Object -First 1).Line)
    if ($f0 -like '*NotificationShade*' -or $f0 -like '*StatusBar*') { & $adb -s $Dev shell input keyevent 4 | Out-Null; Start-Sleep -Milliseconds 400 }
    & $adb -s $Dev shell am force-stop $pkg | Out-Null
    Start-Sleep -Milliseconds 500
    & $adb -s $Dev shell am start -n $act --es route $c.route | Out-Null
    Start-Sleep -Milliseconds 2100
    if (-not (FocusIsOurs $Dev)) { continue }
    cmd /c "$adb -s $Dev exec-out screencap -p > C:\sxcl_local\c1.png"
    & $adb -s $Dev shell input swipe $c.x1 $c.y1 $c.x2 $c.y2 400
    Start-Sleep -Milliseconds 350
    if (-not (FocusIsOurs $Dev)) { continue }
    cmd /c "$adb -s $Dev exec-out screencap -p > C:\sxcl_local\c2.png"
    Move-Item 'C:\sxcl_local\c1.png' ($dir + '\' + $name + '_' + $Tag + '_1.png') -Force
    Move-Item 'C:\sxcl_local\c2.png' ($dir + '\' + $name + '_' + $Tag + '_2.png') -Force
    $ok = $true
  }
  Write-Output ("  " + $name.PadRight(18) + $(if ($ok) { ' CAPTURED (our app had focus both frames)' } else { ' FAILED (focus lost ' + $Tries + 'x)' }))
}
Write-Output 'CASES DONE'
