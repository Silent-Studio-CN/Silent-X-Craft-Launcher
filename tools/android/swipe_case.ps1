# swipe_case.ps1 - one controlled device swipe case, with before/after pixels.
#
# Evidence layout (all under build/_android/out/touch/<case>/):
#   before.png, after.png   - device screencaps (2400x1600, landscape)
#   measure.txt             - output of the SAME ruler (measure_swipe.py) every time
#   log.txt                 - the app's own INPUT / scroll MOVE lines for this run
# so "before fix" and "after fix" are always measured by one instrument.
#
# The device screen is kept awake and the keyguard dismissed: a sleeping screen
# returns a 18 KB flat screenshot and input swipe goes nowhere (found the hard
# way - the first run measured a black screen).
#
# ASCII only. Example:
#   pwsh -File build/_android/scripts/swipe_case.ps1 -Route home -Case home_before -Swipe 1200,1250,1200,450,400
param(
  [string]$Route = 'home',
  [Parameter(Mandatory = $true)][string]$Case,
  [Parameter(Mandatory = $true)][string]$Swipe,   # "x1,y1,x2,y2[,ms]"
  [string]$Region = '',                            # "x0,y0,x1,y1" for measure_swipe.py
  [int]$SettleMs = 6000,
  [int]$AfterMs = 1600,
  [string]$Device = ''
)
$ErrorActionPreference = 'Continue'
$Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Repo = Split-Path $Root -Parent
$Out = Join-Path $Root ('out\touch\' + $Case)
New-Item -ItemType Directory -Force -Path $Out | Out-Null

function FindDevice {
  # the USB transport has been re-enumerating with two different serials for the
  # same G6012BS unit; prefer the wifi transport, fall back to any G6012BS*.
  foreach ($s in @('192.168.200.164:5555')) {
    if ((adb devices) -match [regex]::Escape($s) + '\s+device') { return $s }
  }
  $m = (adb devices) | Select-String '^(G6012BS\w+)\s+device' | Select-Object -First 1
  if ($m) { return $m.Matches[0].Groups[1].Value }
  throw 'no G6012BS device on adb'
}
$D = if ($Device) { $Device } else { FindDevice }
Write-Output "device  = $D"
Write-Output "case    = $Case   route=$Route  swipe=$Swipe"
Write-Output "out     = $Out"

# 1. keep the panel on for the whole measurement (a sleeping screen fakes 0 px)
adb -s $D shell svc power stayon true | Out-Null
adb -s $D shell input keyevent KEYCODE_WAKEUP | Out-Null
Start-Sleep -Milliseconds 800
adb -s $D shell wm dismiss-keyguard | Out-Null
Start-Sleep -Milliseconds 500
$wake = (adb -s $D shell dumpsys power | Select-String 'mWakefulness=')
Write-Output ("screen  : {0}" -f ($wake -join ' ').Trim())

# 2. fresh app on the requested route
adb -s $D shell am force-stop com.silentstudio.sxcl | Out-Null
adb -s $D logcat -c
adb -s $D shell am start -n com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity --es route $Route | Out-Null
Start-Sleep -Milliseconds $SettleMs
$focus = (adb -s $D shell dumpsys window | Select-String 'mCurrentFocus')
Write-Output ("focus   : {0}" -f ($focus -join ' ').Trim())

adb -s $D shell screencap -p /sdcard/t/before.png
adb -s $D pull /sdcard/t/before.png (Join-Path $Out 'before.png') | Out-Null

$parts = $Swipe.Split(',')
$ms = if ($parts.Count -ge 5) { $parts[4] } else { '400' }
Write-Output ("swipe   : {0} {1} -> {2} {3} over {4} ms" -f $parts[0], $parts[1], $parts[2], $parts[3], $ms)
adb -s $D shell input swipe $parts[0] $parts[1] $parts[2] $parts[3] $ms
Start-Sleep -Milliseconds $AfterMs

adb -s $D shell screencap -p /sdcard/t/after.png
adb -s $D pull /sdcard/t/after.png (Join-Path $Out 'after.png') | Out-Null

adb -s $D logcat -d -s sxcl:I | Select-String -Pattern 'INPUT |scroll MOVE|scrollarea |touch scroller|popup ' |
  ForEach-Object { $_.Line } | Set-Content -Path (Join-Path $Out 'log.txt') -Encoding UTF8

$margs = @((Join-Path $Out 'before.png'), (Join-Path $Out 'after.png'))
if ($Region) { $margs += @('--region', $Region) }
$margs += @('--json', (Join-Path $Out 'measure.json'))
$measured = & python (Join-Path $PSScriptRoot 'measure_swipe.py') @margs 2>&1
$measured | Set-Content -Path (Join-Path $Out 'measure.txt') -Encoding UTF8
$measured | ForEach-Object { Write-Output $_ }
Write-Output '--- app INPUT / scroll lines this run ---'
Get-Content (Join-Path $Out 'log.txt') -ErrorAction SilentlyContinue |
  Select-String -Pattern 'INPUT |scroll MOVE' | Select-Object -First 25 | ForEach-Object { $_.Line }
