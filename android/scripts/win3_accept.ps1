# win3_accept.ps1 - real-device acceptance for the three title-bar buttons on Android.
#
# WHY COORDINATES: uiautomator cannot read the widget tree (the whole UI is one
# SurfaceView), so the Qt-drawn buttons are tapped by coordinate and the EFFECT is
# then verified with dumpsys / pidof - never with "it should have worked".
#
# Button positions were MEASURED, not guessed (pixel analysis of a device
# screenshot, build/_android/out/win3/02_restored.png):
#   minimize  (2170, 80)  21x1  horizontal line
#   maximize  (2262, 80)  21x21 square outline
#   close     (2352, 79)  20x20 X
# (the rule display = (logical_x*2, logical_y*2 + 48) gives the same numbers for a
#  1200x776 logical window: the buttons are 46x32 at the right edge of a 48px bar)
#
# INPUT LATENCY: on this device (remote-controlled G6012BS) adb input tap is
# delivered ~13 s late, so every step WAITS FOR THE EFFECT instead of sleeping a
# fixed time. Do not "fix" this by shortening the timeouts.
#
# ASCII only. Every step prints the RAW command and the RAW output.
param(
  [string]$Serial = '192.168.200.164:5555',
  [string]$Apk    = 'D:\sxcl_local\out\build\outputs\apk\debug\sxcl-debug.apk',
  [string]$Out    = '',
  [int]$EffectTimeoutSec = 60,
  [int]$ProgressWaitSec  = 40,
  [switch]$SkipInstall,
  [switch]$SkipClose
)
$ErrorActionPreference = 'Continue'
function Say($m) { Write-Output ('[win3] ' + $m) }
function Raw($m) { Write-Output ('$ ' + $m) }
function AdbShell([string[]]$a) { & adb -s $Serial shell @a 2>&1 }
function AdbRun([string[]]$argv) {
  Raw ('adb -s ' + $Serial + ' ' + ($argv -join ' '))
  $o = & adb -s $Serial @argv 2>&1
  $o | ForEach-Object { Write-Output ('  ' + $_) }
  return $o
}
function Focus() { ((AdbShell @('dumpsys','window')) | Select-String -Pattern 'mCurrentFocus').Line -join '' }
function TraceLines([string]$pattern) {
  ((& adb -s $Serial logcat -d -s sxcl 2>&1) | Select-String -Pattern $pattern).Line
}
# waits until the app's logcat (tag sxcl) contains $pattern, up to $EffectTimeoutSec
function WaitForTrace([string]$pattern, [string]$what) {
  for ($i = 0; $i -lt ($EffectTimeoutSec / 3); $i++) {
    Start-Sleep -Seconds 3
    $hit = TraceLines $pattern
    if ($hit) { Say ('EFFECT after ~' + (($i+1)*3) + 's: ' + $what); $hit | ForEach-Object { Write-Output ('  ' + $_) }; return $true }
  }
  Say ('NO EFFECT within ' + $EffectTimeoutSec + 's: ' + $what)
  return $false
}
function GameDirKB() {
  $o = (& adb -s $Serial shell run-as com.silentstudio.sxcl du -sk files/.minecraft 2>&1) -join ' '
  return (($o.Trim() -split '\s+')[0])
}
if ([string]::IsNullOrWhiteSpace($Out)) {
  $Out = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'build\_android\out\win3'
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $Out 'accept.log'
Remove-Item $log -ErrorAction SilentlyContinue
function Log($m) { $m | Tee-Object -FilePath $log -Append }

Say ('serial  = ' + $Serial)
Say ('apk     = ' + $Apk)
Say ('outdir  = ' + $Out)

# ---- 0. preconditions -------------------------------------------------------
AdbRun @('shell','wm','size')    | Out-Null
AdbRun @('shell','wm','density') | Out-Null
$pip = AdbRun @('shell','pm','list','features')
Say ('device advertises picture-in-picture: ' + [bool]($pip | Select-String -Pattern 'picture_in_picture'))

if (-not $SkipInstall) {
  if (-not (Test-Path $Apk)) { Say ('APK not found: ' + $Apk); exit 2 }
  Say ('apk size = ' + (Get-Item $Apk).Length + '  sha256=' + (Get-FileHash $Apk -Algorithm SHA256).Hash)
  AdbRun @('install','-r',$Apk) | Out-Null
}
AdbRun @('shell','am','force-stop','com.silentstudio.sxcl') | Out-Null
Start-Sleep -Seconds 2
AdbRun @('logcat','-c') | Out-Null
AdbRun @('shell','am','start','-n','com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity') | Out-Null
Start-Sleep -Seconds 12
Say ('mCurrentFocus = ' + (Focus -replace '.*mCurrentFocus=',''))
& adb -s $Serial exec-out screencap -p > (Join-Path $Out '00_foreground.png')
Say ('screenshot 00_foreground.png ' + (Get-Item (Join-Path $Out '00_foreground.png')).Length)
# ---- 1. MINIMISE = move the whole task to the back -------------------------
Say '--- 1) MINIMISE: tap (2170,80) ---'
$kb0 = GameDirKB
AdbRun @('shell','input','tap','2170','80') | Out-Null
$ok = WaitForTrace '安卓最小化' 'moveTaskToBack'
Say ('mCurrentFocus = ' + (Focus -replace '.*mCurrentFocus=','') + '   (must NOT be sxcl any more)')
$pid1 = (((& adb -s $Serial shell pidof com.silentstudio.sxcl 2>&1) -join '').Trim())
Say ('pidof   = [' + $pid1 + ']   (must still be alive: the process keeps running)')
& adb -s $Serial exec-out screencap -p > (Join-Path $Out '01_minimised.png')
Say ('screenshot 01_minimised.png ' + (Get-Item (Join-Path $Out '01_minimised.png')).Length)
Say ('game dir before minimise = ' + $kb0 + ' KB')
if ($ProgressWaitSec -gt 0) {
  Say ('--- 1b) while backgrounded: sample the game dir for ' + $ProgressWaitSec + 's ---')
  $samples = @()
  for ($i = 1; $i -le ($ProgressWaitSec / 10); $i++) {
    Start-Sleep -Seconds 10
    $k = GameDirKB
    $samples += [int]$k
    Say ('  background t=' + ($i*10) + 's  game-dir=' + $k + ' KB')
  }
  if (($samples | Select-Object -Unique).Count -gt 1) {
    Say 'PROGRESS: the game directory GREW while the launcher was off screen -> a download/install task kept running.'
  } else {
    Say 'PROGRESS: no growth in this window. Either no task was running, or it was between files. NOT claimed as evidence.'
  }
}

# ---- 2. back to the foreground --------------------------------------------
Say '--- 2) back to the foreground (am start) ---'
AdbRun @('shell','am','start','-n','com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity') | Out-Null
Start-Sleep -Seconds 12
Say ('mCurrentFocus = ' + (Focus -replace '.*mCurrentFocus=',''))
& adb -s $Serial exec-out screencap -p > (Join-Path $Out '02_restored.png')
Say ('screenshot 02_restored.png ' + (Get-Item (Join-Path $Out '02_restored.png')).Length)

# ---- 3. MAXIMISE = full screen <-> PiP floating window ---------------------
Say '--- 3) MAXIMISE: tap (2262,80) ---'
AdbRun @('logcat','-c') | Out-Null
AdbRun @('shell','input','tap','2262','80') | Out-Null
$okPip = WaitForTrace '画中画悬浮窗' 'enterPictureInPictureMode'
Start-Sleep -Seconds 2
$state = AdbShell @('dumpsys','activity','activities')
foreach ($pat in @('mode=pinned','mLastReportedPictureInPictureMode=true','supportsPictureInPicture=true','pip_input_consumer')) {
  $hit = ($state | Select-String -Pattern $pat | Select-Object -First 1).Line
  Say (('  {0,-42} {1}' -f $pat, $(if ($hit) { 'FOUND' } else { 'not found' })))
}
& adb -s $Serial exec-out screencap -p > (Join-Path $Out '03_maximised_pip.png')
Say ('screenshot 03_maximised_pip.png ' + (Get-Item (Join-Path $Out '03_maximised_pip.png')).Length)

# ---- 4. back to full screen ------------------------------------------------
Say '--- 4) back to full screen (am start / system expand) ---'
AdbRun @('shell','am','start','-n','com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity') | Out-Null
Start-Sleep -Seconds 12
$st2 = AdbShell @('dumpsys','activity','activities')
Say ('  pinned still? ' + [bool]($st2 | Select-String -Pattern 'mode=pinned'))
& adb -s $Serial exec-out screencap -p > (Join-Path $Out '04_fullscreen.png')
Say ('screenshot 04_fullscreen.png ' + (Get-Item (Join-Path $Out '04_fullscreen.png')).Length)

if ($SkipClose) { Say 'close step skipped (-SkipClose)'; Say ('log: ' + $log); exit 0 }
# ---- 5. CLOSE = real exit ---------------------------------------------------
Say '--- 5) CLOSE: tap (2352,79) ---'
$f = Focus
if ($f -notmatch 'silentstudio.sxcl') { Say 'ABORT close step: the launcher is not focused, the tap would land elsewhere'; Say ('log: ' + $log); exit 1 }
$pidBefore = (((& adb -s $Serial shell pidof com.silentstudio.sxcl 2>&1) -join '').Trim())
Say ('pid before close = [' + $pidBefore + ']')
AdbRun @('shell','input','tap','2352','79') | Out-Null
$gone = $false
for ($i = 0; $i -lt ($EffectTimeoutSec / 3); $i++) {
  Start-Sleep -Seconds 3
  $now = (((& adb -s $Serial shell pidof com.silentstudio.sxcl 2>&1) -join '').Trim())
  if ([string]::IsNullOrWhiteSpace($now)) { $gone = $true; Say ('EFFECT after ~' + (($i+1)*3) + 's: pidof is EMPTY'); break }
}
if ($gone) { Say 'CLOSE: process is GONE -> real exit, as required' }
else { Say 'CLOSE: process STILL ALIVE -> the close button did NOT exit' }
Say ('log written to ' + $log)
