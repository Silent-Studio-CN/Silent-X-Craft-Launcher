# gameproc_accept.ps1 - real-device acceptance for the "game in its own process" architecture.
#
# WHAT EACH STEP PROVES (nothing is inferred; every claim below is followed by the RAW
# command and the RAW device output in accept.log):
#   RUN A  boot switch --es gamestart 1
#          * ps -A | grep sxcl shows TWO pids (host + :game)
#          * the launcher is still VISIBLE, in picture-in-picture (mode=pinned)
#          * tapping the native "back to launcher" button returns to the full UI
#            while the :game process keeps running
#   RUN A2 main process sends the END command -> :game flushes + writes its exit record
#          -> main process confirms the pid is gone -> host process still alive
#   RUN B  crash injection (abort -> SIGABRT): the trap in the game process reports
#          signal=6 / code=134 back over the channel; the host survives
#   RUN C  run-as kill -TERM <game pid>: catchable signal -> signal=15 / code=143
#   RUN D  run-as kill -9 <game pid>: NOT catchable -> the host must not invent a number;
#          it reports channel-closed + no exit record + pid gone (killed)
#   RUN E  Qt-side command path (--es gamestop <ms>) -> GameHost.endGame()
#
# INPUT LATENCY: this device (remote G6012BS) delivers adb input tap ~13 s late, so every
# step WAITS FOR THE EFFECT in logcat instead of sleeping a fixed time.
#
# ASCII only in this file. Device output may contain UTF-8 (logcat lines are Chinese).
param(
  [string]$Serial = '192.168.200.164:5555',
  [string]$Apk    = 'D:\sxcl_local\out\build\outputs\apk\debug\sxcl-debug.apk',
  [string]$Out    = '',
  [int]$EffectTimeoutSec = 120,
  [int]$GameDelayMs = 12000,
  [switch]$SkipInstall
)
$ErrorActionPreference = 'Continue'
$Pkg = 'com.silentstudio.sxcl'
$Act = 'com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity'
function Say($m) { Write-Output ('[game] ' + $m); Log ('[game] ' + $m) }
function Raw($m) { Write-Output ('$ ' + $m); Log ('$ ' + $m) }
function Sub($m) { Write-Output ('  ' + $m); Log ('  ' + $m) }
function AdbShell([string[]]$a) { & adb -s $Serial shell @a 2>&1 }
function AdbRun([string[]]$argv) {
  Raw ('adb -s ' + $Serial + ' ' + ($argv -join ' '))
  $o = & adb -s $Serial @argv 2>&1
  $o | ForEach-Object { Sub $_ }
  return $o
}
function TraceLines([string]$pattern) {
  ((& adb -s $Serial logcat -d -s sxcl sxcl-game 2>&1) | Select-String -Pattern $pattern).Line
}
# wait-for-effect (never a fixed sleep): sets $script:waitOk
function WaitForTrace([string]$pattern, [string]$what) {
  $script:waitOk = $false
  for ($i = 0; $i -lt ($EffectTimeoutSec / 3); $i++) {
    Start-Sleep -Seconds 3
    $hit = TraceLines $pattern
    if ($hit) {
      Say ('EFFECT after ~' + (($i+1)*3) + 's: ' + $what)
      $hit | Select-Object -First 6 | ForEach-Object { Sub $_ }
      $script:waitOk = $true
      return
    }
  }
  Say ('NO EFFECT within ' + $EffectTimeoutSec + 's: ' + $what)
}
function PsSxcl() {
  Raw ('adb -s ' + $Serial + ' shell ps -A | grep sxcl  (raw)')
  # one single argument: adb hands the whole pipeline to the device shell
  $o = AdbShell @('ps -A | grep sxcl')
  $o | ForEach-Object { Sub $_ }
  return ($o | Where-Object { $_ -match 'sxcl' })
}
function GamePid() {
  $o = (& adb -s $Serial shell 'pidof com.silentstudio.sxcl:game' 2>&1) -join ' '
  return $o.Trim()
}
function HostPid() {
  $o = (& adb -s $Serial shell 'pidof com.silentstudio.sxcl' 2>&1) -join ' '
  return $o.Trim()
}
function SessionDir() {
  $hit = (TraceLines 'game-session: 会话已建' | Select-Object -Last 1)
  if (-not $hit) { return '' }
  if ($hit -match 'dir=([^ ]+)') { return $Matches[1] }
  return ''
}
function DumpSession([string]$why) {
  $d = SessionDir
  if ([string]::IsNullOrWhiteSpace($d)) { Say ('session dir unknown, cannot dump (' + $why + ')'); return }
  Say ('session dir = ' + $d + '  (' + $why + ')')
  AdbRun @('shell', 'run-as ' + $Pkg + ' ls -l ' + $d) | Out-Null
  AdbRun @('shell', 'run-as ' + $Pkg + ' cat ' + $d + '/exit.txt') | Out-Null
  AdbRun @('shell', 'run-as ' + $Pkg + ' cat ' + $d + '/host-state.txt') | Out-Null
  Raw ('adb -s ' + $Serial + ' shell run-as ' + $Pkg + ' tail -n 6 ' + $d + '/game.log')
  (AdbShell @('run-as ' + $Pkg + ' tail -n 6 ' + $d + '/game.log')) | ForEach-Object { Sub $_ }
}

if ([string]::IsNullOrWhiteSpace($Out)) {
  $Out = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'build\_android\out\gameproc'
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$log = Join-Path $Out 'accept.log'
Remove-Item $log -ErrorAction SilentlyContinue
function Log($m) { $m | Out-File -FilePath $log -Append -Encoding UTF8 }
function Shot([string]$name) {
  $p = Join-Path $Out $name
  & adb -s $Serial exec-out screencap -p > $p
  Say ('screenshot ' + $name + ' ' + (Get-Item $p).Length + ' bytes')
}

Say ('serial = ' + $Serial)
Say ('apk    = ' + $Apk)
Say ('outdir = ' + $Out)
AdbRun @('shell', 'wm', 'size') | Out-Null
AdbRun @('shell', 'wm', 'density') | Out-Null
$pip = AdbRun @('shell', 'pm', 'list', 'features')
Say ('device advertises picture-in-picture: ' + [bool]($pip | Select-String -Pattern 'picture_in_picture'))

if (-not $SkipInstall) {
  if (-not (Test-Path $Apk)) { Say ('APK not found: ' + $Apk); exit 2 }
  Say ('apk size = ' + (Get-Item $Apk).Length + '  sha256=' + (Get-FileHash $Apk -Algorithm SHA256).Hash)
  AdbRun @('install', '-r', $Apk) | Out-Null
}
# the manifest must really carry the :game process declaration
Raw 'adb shell dumpsys package com.silentstudio.sxcl | grep -A2 GameActivity (raw)'
AdbShell @('dumpsys package ' + $Pkg + ' | grep -i -A3 GameActivity') | ForEach-Object { Sub $_ }

function StartApp([string]$extraArgs) {
  AdbRun @('shell', 'am', 'force-stop', $Pkg) | Out-Null
  Start-Sleep -Seconds 2
  AdbRun @('logcat', '-c') | Out-Null
  $argv = @('shell', 'am', 'start', '-n', $Act)
  if ($extraArgs) { $argv += ($extraArgs -split ' ') }
  AdbRun $argv | Out-Null
}
# The action extras are only handled in onNewIntent, i.e. the activity must already exist.
# So: make sure the launcher is up (a plain start), then send the action intent.
function NewGame([string]$extra) {
  AdbRun @('shell', 'am', 'start', '-n', $Act) | Out-Null
  Start-Sleep -Seconds 6
  $argv = @('shell', 'am', 'start', '-n', $Act, '--es', 'action', 'gamestart')
  if ($extra) { $argv += ($extra -split ' ') }
  AdbRun $argv | Out-Null
}

# ═══ RUN A: two processes + picture-in-picture ═══════════════════════════════
Say '=== RUN A: gamestart from the boot file (two processes + PiP) ==='
Log '=== RUN A ==='
StartApp ('--es gamestart 1 --es gamedelay ' + $GameDelayMs)
WaitForTrace 'game-session: 运行器报到' 'RUN A: the game process said HELLO over the channel'
$lines = PsSxcl
$gamePidA = GamePid
$hostPidA = HostPid
Say ('host pid = [' + $hostPidA + ']   :game pid = [' + $gamePidA + ']   ps lines = ' + ($lines | Measure-Object).Count)
if (($lines | Measure-Object).Count -ge 2 -and $hostPidA -and $gamePidA) {
  Say 'PASS: ps -A shows TWO sxcl processes (host + :game)'
} else {
  Say 'FAIL: expected two sxcl processes'
}
Say '--- picture-in-picture state (raw dumpsys) ---'
$state = AdbShell @('dumpsys', 'activity', 'activities')
foreach ($pat in @('mode=pinned', 'mLastReportedPictureInPictureMode=true', 'supportsPictureInPicture=true')) {
  $hit = ($state | Select-String -Pattern $pat | Select-Object -First 1).Line
  Say (('  {0,-42} {1}' -f $pat, $(if ($hit) { 'FOUND' } else { 'not found' })))
}
Raw 'adb shell dumpsys activity activities | grep -i GameActivity (raw)'
($state | Select-String -Pattern 'GameActivity' | Select-Object -First 6) | ForEach-Object { Sub $_.Line }
Raw 'adb shell dumpsys activity processes | grep -i sxcl (raw)'
(AdbShell @('dumpsys activity processes | grep -i sxcl')) | ForEach-Object { Sub $_ }
Shot '01_pip_with_game.png'
TraceLines 'game: ' | Select-Object -Last 8 | ForEach-Object { Sub $_ }

# ═══ RUN A2: tap the native "back to launcher" button inside the PiP window ══
Say '=== RUN A2: tap the PiP "back to launcher" button ==='
# The PiP window rectangle: try the activity dump first (mode=pinned ... bounds=), then the
# window dump (mFrame=Rect(...)). Two strategies because the dump format differs by ROM.
function PipBounds() {
  $txt = AdbShell @('dumpsys activity activities')
  $idx = -1
  for ($i = 0; $i -lt $txt.Count; $i++) {
    if ($txt[$i] -match 'mode=pinned') { $idx = $i; break }
  }
  if ($idx -ge 0) {
    for ($j = [Math]::Max(0, $idx - 20); $j -lt [Math]::Min($idx + 90, $txt.Count); $j++) {
      if ($txt[$j] -match 'bounds=\[(-?\d+),(-?\d+)\]\[(-?\d+),(-?\d+)\]') {
        return @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
      }
      if ($txt[$j] -match 'mBounds=Rect\((-?\d+), ?(-?\d+) - (-?\d+), ?(-?\d+)\)') {
        return @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
      }
    }
  }
  $win = AdbShell @('dumpsys window windows')
  $inSxcl = $false
  foreach ($l in $win) {
    if ($l -match 'silentstudio') { $inSxcl = $true; continue }
    if ($inSxcl -and $l -match 'mFrame=Rect\((-?\d+), ?(-?\d+) - (-?\d+), ?(-?\d+)\)') {
      return @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
    }
  }
  return $null
}
$bounds = PipBounds
if ($bounds) {
  $tx = [int](($bounds[0] + $bounds[2]) / 2)
  $ty = [int]($bounds[1] + 45)
  Say ('PiP window rect = ' + ($bounds -join ',') + '  -> tapping its top-centre at (' + $tx + ',' + $ty + ')')
} else {
  Say 'could not parse the PiP window rect: NOT tapping (no guessing)'
}
$tapped = $false
if ($bounds) {
  # The PiP window may show the activity SCALED (the activity keeps its own size), so the button
  # (top-centre of the window) can be within a few pixels of the window's top edge. Sweep a small
  # ladder of y offsets instead of guessing one.
  foreach ($dy in @(30, 45, 20, 60, 80)) {
    $ty = [int]($bounds[1] + $dy)
    Say ('tap attempt dy=' + $dy + ' at (' + $tx + ',' + $ty + ')')
    AdbRun @('shell', 'input', 'tap', "$tx", "$ty") | Out-Null
    WaitForTrace 'pip back-button tapped' ('RUN A2: tap dy=' + $dy + ' reached the native button')
    if ($script:waitOk) { $tapped = $true; break }
  }
  if ($tapped) {
    WaitForTrace '画中画已退出' 'RUN A2: the system expanded the launcher back to full screen'
  } else {
    Say 'NO TAP DELIVERED: adb input tap did not reach the pinned window on this ROM.'
    Say 'Fallback: verify the ACTION the button performs (exitFloating = reorder-to-front intent)'
    AdbRun @('shell', 'am', 'start', '-n', $Act) | Out-Null
    WaitForTrace '画中画已退出' 'RUN A2(fallback): the launcher came back to the front (PiP ended)'
  }
}
Start-Sleep -Seconds 8
$focus = ((AdbShell @('dumpsys', 'window')) | Select-String -Pattern 'mCurrentFocus').Line -join ''
Say ('mCurrentFocus = ' + ($focus -replace '.*mCurrentFocus=', '') + '   (must be the launcher)')
$pipNow = (AdbShell @('dumpsys activity activities') | Select-String -Pattern 'mode=pinned')
Say ('still pinned? ' + [bool]$pipNow + '   (must be False after returning to the launcher)')
$afterTap = PsSxcl
$gamePidAfterTap = GamePid
Say (':game pid after returning to the launcher = [' + $gamePidAfterTap + ']  (must still be alive)')
if ($gamePidAfterTap) { Say 'PASS: the game process kept running while the launcher came back' }
else { Say 'FAIL: the game process disappeared when the launcher came back' }
Shot '02_back_to_launcher.png'

# ═══ RUN A3: main process sends END -> orderly shutdown -> confirmed gone ════
Say '=== RUN A3: end the game from the main process (END over the channel) ==='
AdbRun @('shell', 'am', 'start', '-n', $Act, '--es', 'action', 'endgame') | Out-Null
WaitForTrace '已发出结束命令' 'RUN A3: END written to the channel'
WaitForTrace '运行器已结束:信号=0' 'RUN A3: the game process reported its own exit record'
WaitForTrace '确认 :game 进程已消失' 'RUN A3: the main process confirmed the :game pid is gone'
$linesEnd = PsSxcl
$hostEnd = HostPid
$gameEnd = GamePid
Say ('after END: host pid = [' + $hostEnd + ']   :game pid = [' + $gameEnd + ']')
# NOTE: judged by "the :game pid is gone while the launcher pid is alive", NOT by counting the
# grep lines (the device's ps output also carries a header/self-matching row).
$gameRows = $linesEnd | Where-Object { $_ -match '\.sxcl:game' }
if ($hostEnd -and -not $gameEnd -and -not $gameRows) {
  Say 'PASS: :game is gone, the launcher process is still alive'
} else {
  Say 'FAIL: :game still present or the launcher died'
}
DumpSession 'RUN A3 evidence'
TraceLines 'flush ok|EXIT code|结束游戏|确认' | Select-Object -Last 10 | ForEach-Object { Sub $_ }

# ═══ RUN B: crash injection (SIGABRT is catchable) ══════════════════════════
Say '=== RUN B: crash injection abort() -> signal must come back to the host ==='
NewGame ('--es gamecrash abort')
WaitForTrace 'game-session: :game 报到' 'RUN B: game process up'
WaitForTrace 'game-session: 运行器崩溃兜底回传信号' 'RUN B: the crash guard reported the signal'
WaitForTrace '运行器已结束:信号=6' 'RUN B: host resolved signal=6'
$linesB = PsSxcl
$hostB = HostPid
Say ('after the crash: host pid = [' + $hostB + ']   :game pid = [' + (GamePid) + ']')
if ($hostB -and -not (GamePid)) { Say 'PASS: the host survived the game crash' } else { Say 'FAIL: host/game state unexpected' }
DumpSession 'RUN B evidence'
TraceLines 'SIG signal|崩溃注入|已结束:信号' | Select-Object -Last 8 | ForEach-Object { Sub $_ }

# ═══ RUN C: kill -TERM (catchable) ══════════════════════════════════════════
Say '=== RUN C: run-as kill -TERM <:game pid> ==='
NewGame ''
WaitForTrace 'game-session: 运行器报到' 'RUN C: game process up'
$pidC = GamePid
Say (':game pid before kill = [' + $pidC + ']')
AdbRun @('shell', 'run-as ' + $Pkg + ' kill -TERM ' + $pidC) | Out-Null
WaitForTrace 'game-session: 运行器崩溃兜底回传信号' 'RUN C: the trap reported the signal'
WaitForTrace '运行器已结束:信号=15' 'RUN C: host resolved signal=15'
Say ('host after SIGTERM = [' + (HostPid) + ']   :game = [' + (GamePid) + ']')
DumpSession 'RUN C evidence'
TraceLines 'SIG signal|已结束:信号=15' | Select-Object -Last 6 | ForEach-Object { Sub $_ }

# ═══ RUN D: kill -9 (NOT catchable) ═════════════════════════════════════════
Say '=== RUN D: run-as kill -9 <:game pid> (no record is possible) ==='
NewGame ''
WaitForTrace 'game-session: 运行器报到' 'RUN D: game process up'
$pidD = GamePid
Say (':game pid before kill -9 = [' + $pidD + ']')
AdbRun @('shell', 'run-as ' + $Pkg + ' kill -9 ' + $pidD) | Out-Null
WaitForTrace '没有退出记录' 'RUN D: host reported "no exit record" honestly'
Say ('host after SIGKILL = [' + (HostPid) + ']   :game = [' + (GamePid) + ']')
TraceLines 'channel-closed|没有退出记录|线程' | Select-Object -Last 6 | ForEach-Object { Sub $_ }

# ═══ RUN E: the Qt command path (--es gamestop <ms>) ════════════════════════
Say '=== RUN E: Qt side asks for the end (gamestop) ==='
StartApp ('--es gamestart 1 --es gamedelay ' + $GameDelayMs + ' --es gamestop 20000')
WaitForTrace 'game-session: 运行器报到' 'RUN E: game process up'
WaitForTrace 'game: 结束游戏命令' 'RUN E: the Qt side issued endGameSession()'
WaitForTrace '确认 :game 进程已消失' 'RUN E: confirmed the :game pid is gone'
Say ('host after RUN E = [' + (HostPid) + ']   :game = [' + (GamePid) + ']')

# ═══ containment: nothing outside the app sandbox was touched ═══════════════
Say '=== containment evidence (no system settings, no FCL dir, no MC version) ==='
Raw ('adb -s ' + $Serial + ' shell run-as ' + $Pkg + ' ls -R files/game | head -n 40')
(AdbShell @('run-as ' + $Pkg + ' ls -R files/game')) | Select-Object -First 40 | ForEach-Object { Sub $_ }
Raw 'adb shell ls -ld /storage/emulated/0/FCL (must be untouched by this round)'
(AdbShell @('ls -ld /storage/emulated/0/FCL')) | ForEach-Object { Sub $_ }
Say ('log written to ' + $log)
