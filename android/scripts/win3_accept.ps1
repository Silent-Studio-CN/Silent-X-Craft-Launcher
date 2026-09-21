# win3_accept.ps1 - real-device acceptance for the three title-bar buttons on Android.
#
# It taps the Qt-drawn buttons by COORDINATE (uiautomator cannot read the widget tree:
# the whole UI is one SurfaceView) and then verifies the EFFECT with dumpsys / pidof,
# never with "it should have worked". Coordinate rule (docs/08 section 11.2):
#     display = (logical_x * 2, logical_y * 2 + 48)
# Title bar buttons are 46x32 at the right edge, aligned to the top of the 48px bar,
# so with the app's 1200x777 logical window:
#     minimize  logical (1085,16) -> display (2170,  80)
#     maximize  logical (1131,16) -> display (2262,  80)
#     close     logical (1177,16) -> display (2354,  80)
#
# ASCII only. Every step prints the RAW command and the RAW output.
param(
  [string]$Serial = '192.168.200.164:5555',
  [string]$Apk    = 'D:\sxcl_local\out\build\outputs\apk\debug\sxcl-debug.apk',
  [string]$Out    = '',
  [int]$ProgressWaitSec = 20,
  [switch]$SkipInstall
)
$ErrorActionPreference = 'Continue'
function Say($m) { Write-Output ('[win3] ' + $m) }
function Raw($m) { Write-Output ('$ ' + $m) }
function Adb([string[]]$argv) {
  Raw ('adb -s ' + $Serial + ' ' + ($argv -join ' '))
  $o = & adb -s $Serial @argv 2>&1
  $o | ForEach-Object { Write-Output ('  ' + $_) }
  return $o
}
if ([string]::IsNullOrWhiteSpace($Out)) {
  $Out = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'build\_android\out\win3'
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $Out 'accept.log'
function Log($m) { $m | Tee-Object -FilePath $log -Append }

Say ('serial  = ' + $Serial)
Say ('apk     = ' + $Apk)
Say ('outdir  = ' + $Out)

# ---- 0. preconditions -------------------------------------------------------
Adb @('shell','wm','size')      | Out-Null
Adb @('shell','wm','density')   | Out-Null
Adb @('shell','getprop','ro.build.version.release') | Out-Null
$pip = Adb @('shell','pm','list','features')
Say ('device advertises picture-in-picture: ' + [bool]($pip | Select-String -Pattern 'picture_in_picture'))

if (-not $SkipInstall) {
  if (-not (Test-Path $Apk)) { Say ('APK not found: ' + $Apk); exit 2 }
  Say ('apk size = ' + (Get-Item $Apk).Length + '  sha256=' + (Get-FileHash $Apk -Algorithm SHA256).Hash)
  Adb @('install','-r',$Apk) | Out-Null
}
Adb @('shell','am','force-stop','com.silentstudio.sxcl') | Out-Null
Adb @('logcat','-c') | Out-Null
Adb @('shell','am','start','-n','com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity') | Out-Null
Start-Sleep -Seconds 6
$focus = Adb @('shell','dumpsys','window')
Say ('foreground: ' + (($focus | Select-String -Pattern 'mCurrentFocus').Line | Select-Object -First 1))
Adb @('exec-out','screencap','-p') | Out-Null   # warm-up frame
& adb -s $Serial exec-out screencap -p > (Join-Path $Out '00_foreground.png')
Say ('screenshot 00_foreground.png ' + (Get-Item (Join-Path $Out '00_foreground.png')).Length)
$pid0 = (Adb @('shell','pidof','com.silentstudio.sxcl') | Where-Object { $_ -match '^[0-9 ]+$' })
Say ('pid after start = ' + $pid0)

# ---- 1. MINIMISE = move task to back ---------------------------------------
Say '--- 1) MINIMISE (tap 2170 80) ---'
Adb @('shell','input','tap','2170','80') | Out-Null
Start-Sleep -Seconds 3
$acts = Adb @('shell','dumpsys','activity','activities')
$mine = $acts | Select-String -Pattern 'sxcl'
$mine | ForEach-Object { Log ('  ' + $_) }
Say ('still focused? ' + (($focus | Select-String -Pattern 'mCurrentFocus').Line | Select-Object -First 1))
$focus2 = Adb @('shell','dumpsys','window')
Say ('mCurrentFocus now: ' + (($focus2 | Select-String -Pattern 'mCurrentFocus').Line | Select-Object -First 1))
$pid1 = (Adb @('shell','pidof','com.silentstudio.sxcl') | Where-Object { $_ -match '^[0-9 ]+$' })
Say ('pid after minimise = ' + $pid1 + '   (MUST still be alive)')
& adb -s $Serial exec-out screencap -p > (Join-Path $Out '01_minimised.png')
Say ('screenshot 01_minimised.png ' + (Get-Item (Join-Path $Out '01_minimised.png')).Length)

# ---- 1b. is a background task still making progress? -----------------------
Say ('--- 1b) progress while backgrounded (' + $ProgressWaitSec + 's) ---')
function DirStat($p) {
  $o = & adb -s $Serial shell run-as com.silentstudio.sxcl sh -c ("du -sk '" + $p + "' 2>/dev/null; find '" + $p + "' -type f 2>/dev/null | wc -l") 2>&1
  return ($o -join ' | ')
}
$p1 = DirStat 'files'
Say ('t0 files-stat: ' + $p1)
$top1 = & adb -s $Serial shell top -n 1 -b -p $pid1 2>&1 | Select-String -Pattern 'com.silentstudio.sxcl'
Say ('t0 top: ' + ($top1 -join ' '))
Start-Sleep -Seconds $ProgressWaitSec
$p2 = DirStat 'files'
Say ('t1 files-stat: ' + $p2)
$top2 = & adb -s $Serial shell top -n 1 -b -p $pid1 2>&1 | Select-String -Pattern 'com.silentstudio.sxcl'
Say ('t1 top: ' + ($top2 -join ' '))
if ($p1 -ne $p2) { Say 'PROGRESS: app private dir CHANGED while backgrounded (task kept running)' }
else { Say 'PROGRESS: no change in the private dir - either no task was running, or it is idle. Not claimed as evidence.' }
& adb -s $Serial logcat -d -s sxcl | Select-Object -Last 40 | ForEach-Object { Log ('  logcat| ' + $_) }

# ---- 2. return to the foreground ------------------------------------------
Say '--- 2) back to the foreground (am start) ---'
Adb @('shell','am','start','-n','com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity') | Out-Null
Start-Sleep -Seconds 4
$focus3 = Adb @('shell','dumpsys','window')
Say ('mCurrentFocus: ' + (($focus3 | Select-String -Pattern 'mCurrentFocus').Line | Select-Object -First 1))
& adb -s $Serial exec-out screencap -p > (Join-Path $Out '02_restored.png')
Say ('screenshot 02_restored.png ' + (Get-Item (Join-Path $Out '02_restored.png')).Length)

# ---- 3. MAXIMISE = full screen <-> floating window -------------------------
Say '--- 3) MAXIMISE (tap 2262 80) ---'
Adb @('shell','input','tap','2262','80') | Out-Null
Start-Sleep -Seconds 4
$acts2 = Adb @('shell','dumpsys','activity','activities')
$acts2 | Select-String -Pattern 'pipped|PIP|PictureInPicture|sxcl' | ForEach-Object { Log ('  ' + $_) }
$pipState = ($acts2 | Select-String -Pattern 'pipped=')
Say ('pipped state: ' + ($pipState -join ' '))
$ovl = Adb @('shell','appops','get','com.silentstudio.sxcl','SYSTEM_ALERT_WINDOW')
Say ('overlay appop: ' + ($ovl -join ' '))
& adb -s $Serial exec-out screencap -p > (Join-Path $Out '03_maximised.png')
Say ('screenshot 03_maximised.png ' + (Get-Item (Join-Path $Out '03_maximised.png')).Length)
& adb -s $Serial logcat -d -s sxcl | Select-Object -Last 20 | ForEach-Object { Log ('  logcat| ' + $_) }

# ---- 4. back to full screen ------------------------------------------------
Say '--- 4) back to full screen (tap 2262 80 again, or system expand) ---'
Adb @('shell','input','tap','2262','80') | Out-Null
Start-Sleep -Seconds 3
& adb -s $Serial exec-out screencap -p > (Join-Path $Out '04_fullscreen.png')
Say ('screenshot 04_fullscreen.png ' + (Get-Item (Join-Path $Out '04_fullscreen.png')).Length)

# ---- 5. CLOSE = real exit ---------------------------------------------------
Say '--- 5) CLOSE (tap 2354 80) ---'
Adb @('shell','input','tap','2354','80') | Out-Null
Start-Sleep -Seconds 4
$pidEnd = (& adb -s $Serial shell pidof com.silentstudio.sxcl 2>&1) -join ''
Raw ('pidof com.silentstudio.sxcl  ->  ' + $pidEnd)
if ([string]::IsNullOrWhiteSpace($pidEnd)) { Say 'CLOSE: process is GONE -> real exit, as required' }
else { Say ('CLOSE: process STILL ALIVE (pid ' + $pidEnd + ') -> close button did NOT exit') }
Say ('log written to ' + $log)
