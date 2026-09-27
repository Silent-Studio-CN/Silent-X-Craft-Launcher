# (C) Silent X Craft Launcher -- UI stall evidence (counts, not just "did it hang").
#
# Why: user report 2026-09-27 -- the launcher "still says Not Responding"; the runtime log
# showed [sxcl-ui] ui-stall repeatedly (1000 / 1001 / 3005 ms) plus dozens of
# "QObject::moveToThread: Cannot move objects with a parent" lines and a 3 s hang report.
# tools/ui_no_hang.ps1 only answers "did it hang for 3 s". This script answers "how much":
# it drives the SAME operations the user does, one product-hook run each, and counts
#   * ui-stall lines (>800 ms of a frozen GUI thread; the watchdog drops one line per stall)
#   * moveToThread errors (a parented object cannot be moved -> the work stays on the GUI thread)
#   * invokeMethod "No such method" (a method that is not a slot/Q_INVOKABLE: the call is lost)
#   * "QIODevice::read (QSslSocket): device not open" (reading a socket that is already closed)
#   * newly written logdir/crashes/sxcl-ui-hang-* reports
# Every run keeps its logs and hang reports under -LogDir (SXCL_LOG_DIR), so the user's own
# logs\crashes are never touched, and every run starts from a fresh icon/data dir.
# Temp/acceptance files live under D:\SilentStudio\_test.
#
# The last run is the socket case: a local HTTP server answers the version-manifest request
# with a Content-Length it never delivers and then closes the connection, which is exactly
# how the "device not open" spam was produced in the field.
#
# Output: one line per run + "STALL-SUMMARY tag=... runs=... stalls=... moveToThread=...".
# Exit code: 1 when any run wrote a hang report or any ui-stall >= 3000 ms happened.
#
# Keep this file ASCII-only + UTF-8 BOM (Windows PowerShell 5.1 parses .ps1 as ANSI without a BOM).
param(
  [Parameter(Mandatory = $true)][string]$Exe,
  [string]$Tag = 'run',
  [string]$Work = 'D:\SilentStudio\_test\ui_no_stall',
  [int]$KeepMs = 14000,
  [string]$Query = 'jei',
  [string[]]$Only = @()          # run only these scenarios (default: all of them)
)
$ErrorActionPreference = 'Continue'
if (-not (Test-Path $Exe)) { Write-Output ('NO-STALL: FAIL (exe not found: ' + $Exe + ')'); exit 1 }
New-Item -ItemType Directory -Force -Path $Work | Out-Null

# ---------------------------------------------------------------- fixture (game dir + settings)
# Two installed instances so the version routes have real content to draw.
$mc = Join-Path $Work 'mc'
$versions = Join-Path $mc '.minecraft\versions'
New-Item -ItemType Directory -Force -Path (Join-Path $versions '1.20.1'), (Join-Path $versions 'forge-47.2.0') | Out-Null
Set-Content -Path (Join-Path $versions '1.20.1\1.20.1.json') -Value '{"id":"1.20.1"}' -Encoding ascii
Set-Content -Path (Join-Path $versions 'forge-47.2.0\forge-47.2.0.json') -Value '{"id":"forge-47.2.0","inheritsFrom":"1.20.1"}' -Encoding ascii
$ini = Join-Path $Work ('ui_no_stall_' + $Tag + '.ini')
Set-Content -Path $ini -Value ('game.default_dir=' + $mc + '\.minecraft') -Encoding utf8

# One scenario per user operation. The mods-search one is the heavy one: search JSON first,
# then every result card asks for its icon (a separate network request each).
$scenarios = @(
  @{ name = 'home';        env = @{ SXCL_UI_ROUTE = 'home' } },
  @{ name = 'versions';    env = @{ SXCL_UI_ROUTE = 'versions' } },
  @{ name = 'select';      env = @{ SXCL_UI_ROUTE = 'select' } },
  @{ name = 'settings';    env = @{ SXCL_UI_ROUTE = 'settings' } },
  @{ name = 'download';    env = @{ SXCL_UI_ROUTE = 'download'; SXCL_UI_NAV = 'download_mod' } },
  @{ name = 'mods_search'; env = @{ SXCL_UI_ROUTE = 'download'; SXCL_UI_NAV = 'download_mod';
                                    SXCL_UI_MODS_SOURCES = 'modrinth'; SXCL_UI_MODS_SOURCES_DELAY = '500';
                                    SXCL_UI_MODS_QUERY = ''; SXCL_UI_MODS_QUERY_DELAY = '2500' } },
  # switching routes through the product hook that calls
  # QMetaObject::invokeMethod(window(), "switchToRoute", ...): it has to be a real invokable
  # method, otherwise Qt answers "No such method" and the page switch is silently dropped.
  @{ name = 'route_switch'; env = @{ SXCL_UI_ROUTE = 'home'; SXCL_UI_THEME_SWITCH = 'dark' } }
)
# every hook this script owns must start from a known state
$owned = @('SXCL_UI_ROUTE', 'SXCL_UI_NAV', 'SXCL_UI_MODS_SOURCES', 'SXCL_UI_MODS_SOURCES_DELAY',
           'SXCL_UI_MODS_QUERY', 'SXCL_UI_MODS_QUERY_DELAY', 'SXCL_UI_MODS_VERSION',
           'SXCL_UI_MANIFEST_URL', 'SXCL_UI_VERSION', 'SXCL_UI_LAUNCH', 'SXCL_UI_DOWNLOAD',
           'SXCL_UI_RESIZE', 'SXCL_UI_THEME_SWITCH', 'SXCL_UI_ROW_EVIDENCE',
           'SXCL_UI_AUTH_DIALOG', 'SXCL_UI_AUTH_REPLAY', 'SXCL_UI_AUTH_REPLAY_HOP6')

$totals = @{ runs = 0; stalls = 0; max = 0; move = 0; invoke = 0; socket = 0; bg = 0; hangs = 0;
             teardown = -1 }
$fails = @()

function Write-RunLine($name, $ms, $exit, $stalls, $stallMax, $moves, $invokes, $sockets, $bgWrong, $hangs, $shotOk) {
  Write-Output ('  ' + $name.PadRight(18) +
                ' ms=' + $ms + ' exit=' + $exit +
                ' stall_lines=' + $stalls + ' stall_max=' + $stallMax +
                ' moveToThread=' + $moves + ' invoke_missing=' + $invokes +
                ' socket_closed=' + $sockets + ' bg_wrong_thread=' + $bgWrong +
                ' hangs=' + $hangs + ' shot=' + $(if ($shotOk) { 'OK' } else { 'MISSING' }))
}

# Count what the run left behind. $logDir is where SXCL_LOG_DIR put logs + hang reports.
function Measure-Run($name, $logDir, $out, $err, $ms, $exit, $shot) {
  $lines = @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) +
           @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  $stalls = 0; $stallMax = 0; $moves = 0; $invokes = 0; $sockets = 0; $bgWrong = 0
  foreach ($line in $lines) {
    if ($line -match 'ui-stall: (\d+) ms') {
      $stalls++
      $v = [int]$Matches[1]
      if ($v -gt $stallMax) { $stallMax = $v }
    }
    if ($line -match 'moveToThread') { $moves++ }
    if ($line -match 'No such method') { $invokes++ }
    if ($line -match 'device not open') { $sockets++ }
    # bg-task 的取证行是中文的("[sxcl-ui] bg-task[tag]: ... 同一条线程=是(不对!)"); the
    # file must stay ASCII-only, so match the code points of 同一条线程=是
    if ($line -match '\u540c\u4e00\u6761\u7ebf\u7a0b=\u662f') { $bgWrong++ }
  }
  $hangs = 0
  $crashDir = Join-Path $logDir 'crashes'
  if (Test-Path $crashDir) {
    # one report = one .txt + one .dmp; count the reports (.txt), not the files
    $hangs = @(Get-ChildItem $crashDir -Filter 'sxcl-ui-hang-*.txt' -ErrorAction SilentlyContinue).Count
  }
  $shotOk = (Test-Path $shot) -and ((Get-Item $shot).Length -gt 0)
  Write-RunLine $name $ms $exit $stalls $stallMax $moves $invokes $sockets $bgWrong $hangs $shotOk

  $totals.stalls += $stalls; $totals.move += $moves; $totals.invoke += $invokes
  $totals.socket += $sockets; $totals.bg += $bgWrong; $totals.hangs += $hangs
  $totals.runs += 1
  if ($stallMax -gt $totals.max) { $totals.max = $stallMax }
  if ($stalls -gt 0) { $script:fails += ($name + ': ' + $stalls + ' ui-stall (max ' + $stallMax + ' ms)') }
  if ($hangs -gt 0) { $script:fails += ($name + ': ' + $hangs + ' hang report(s)') }
  if ($stallMax -ge 3000) { $script:fails += ($name + ': ui-stall ' + $stallMax + ' ms >= 3000') }
}

# ---------------------------------------------------------------- 1) the user's operations
foreach ($sc in $scenarios) {
  $name = $sc.name
  foreach ($k in $owned) { Remove-Item ('Env:' + $k) -ErrorAction SilentlyContinue }
  foreach ($k in $sc.env.Keys) {
    $v = $sc.env[$k]
    if ($k -eq 'SXCL_UI_MODS_QUERY' -and $v -eq '') { $v = $Query }
    Set-Item -Path ('Env:' + $k) -Value $v
  }
  if ($Only.Count -gt 0 -and ($Only -notcontains $name)) { continue }
  $keep = if ($sc.ContainsKey('keep')) { [int]$sc.keep } else { $KeepMs }
  $logDir = Join-Path $Work ('logs_' + $Tag + '_' + $name)
  Remove-Item -Recurse -Force $logDir -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $logDir | Out-Null
  $dataDir = Join-Path $Work ('data_' + $Tag + '_' + $name)
  Remove-Item -Recurse -Force $dataDir -ErrorAction SilentlyContinue   # fresh icon cache: same conditions every run
  New-Item -ItemType Directory -Force -Path $dataDir | Out-Null

  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = (Join-Path $mc '.minecraft')
  # SXCL_LOG_DIR (no "UI" in the name) is the core library's switch: it moves the run log AND
  # the hang reports into this private dir, so the user's own logs\crashes are never touched.
  $env:SXCL_LOG_DIR = $logDir
  $env:SXCL_UI_DATA_DIR = $dataDir
  # no SXCL_UI_DUMP here: dumping the whole widget tree is a GUI-thread operation of its own
  # and would show up as a stall in the very thing we are measuring.
  Remove-Item Env:SXCL_UI_DUMP -ErrorAction SilentlyContinue
  $env:SXCL_UI_SHOT_DELAY = [string]$keep
  $shot = Join-Path $Work ('shot_' + $Tag + '_' + $name + '.png')
  Remove-Item -Force $shot -ErrorAction SilentlyContinue
  $env:SXCL_UI_SHOT = $shot
  Remove-Item Env:SXCL_UI_RAILS_TEST -ErrorAction SilentlyContinue

  $out = Join-Path $Work ('run_' + $Tag + '_' + $name + '.txt')
  $err = $out + '.err'
  Remove-Item -Force $out, $err -ErrorAction SilentlyContinue
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $proc = Start-Process -FilePath $Exe -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -Wait
  $sw.Stop()
  Measure-Run $name $logDir $out $err ([int]$sw.ElapsedMilliseconds) $proc.ExitCode $shot
}

# ---------------------------------------------------------------- 2) login dialog open + quit
# User-visible requirement: with the device-code login dialog open, quitting the launcher must
# reclaim the login thread **deterministically** -- the process has to be gone within a second of
# the product's own close path, with no hang report and no detached thread left behind.
#
# The trigger is the product path, not a kill: SXCL_UI_WINCHECK runs "probe;probe;probe;quit" and
# the 4th step calls MainWindow::requestClose() (closeEvent -> finishAndQuit), the same thing the
# window's X does. Two runs, so the auth dialog is the only difference:
#   close_plain : settings page, product close, no dialog   -> baseline wall time
#   auth_exit   : same + the login dialog open (device-code login in flight)
# A run that is still alive after -AuthTimeoutMs is killed and reported as a failure (the field
# symptom was "the process lives on for 10+ s after the dialog is gone").
$AuthTimeoutMs = 25000
function Invoke-CloseRun([string]$name, [bool]$withDialog) {
  foreach ($k in $owned) { Remove-Item ('Env:' + $k) -ErrorAction SilentlyContinue }
  $logDir = Join-Path $Work ('logs_' + $Tag + '_' + $name)
  Remove-Item -Recurse -Force $logDir -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $logDir | Out-Null
  $dataDir = Join-Path $Work ('data_' + $Tag + '_' + $name)
  Remove-Item -Recurse -Force $dataDir -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
  $env:SXCL_UI_ROUTE = 'settings'
  $env:SXCL_UI_WINCHECK = 'probe;probe;probe;quit'
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = (Join-Path $mc '.minecraft')
  $env:SXCL_LOG_DIR = $logDir
  $env:SXCL_UI_DATA_DIR = $dataDir
  if ($withDialog) { $env:SXCL_UI_AUTH_DIALOG = '1' }
  else { Remove-Item Env:SXCL_UI_AUTH_DIALOG -ErrorAction SilentlyContinue }
  Remove-Item Env:SXCL_UI_DUMP -ErrorAction SilentlyContinue
  Remove-Item Env:SXCL_UI_SHOT -ErrorAction SilentlyContinue   # no screenshot: the close path is the exit
  Remove-Item Env:SXCL_UI_SHOT_DELAY -ErrorAction SilentlyContinue
  $out = Join-Path $Work ('run_' + $Tag + '_' + $name + '.txt')
  $err = $out + '.err'
  Remove-Item -Force $out, $err -ErrorAction SilentlyContinue
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $proc = Start-Process -FilePath $Exe -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
  while (-not $proc.HasExited -and $sw.ElapsedMilliseconds -lt $AuthTimeoutMs) { Start-Sleep -Milliseconds 50 }
  $timedOut = -not $proc.HasExited
  if ($timedOut) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
  }
  $sw.Stop()
  $ms = [int]$sw.ElapsedMilliseconds
  $exit = if ($timedOut) { -1 } else { $proc.ExitCode }
  Write-Output ('  ' + $name.PadRight(18) + ' ms=' + $ms + ' exit=' + $exit + $(if ($timedOut) { ' (TIMED OUT: had to be killed)' } else { '' }))
  Measure-Run $name $logDir $out $err $ms $exit $out   # $out stands in for "no screenshot expected"
  if ($timedOut) { $script:fails += ($name + ': still alive after ' + $AuthTimeoutMs + ' ms (had to be killed)') }
  # NB: everything this function prints on purpose goes to the script pipeline (that is what the
  # evidence file captures); the number the caller needs travels in a script-scoped map instead of
  # the return value, otherwise the printed lines become part of it.
  $script:closeMs[$name] = $ms
}
$script:closeMs = @{}
if ($Only.Count -eq 0 -or ($Only -contains 'auth_exit')) {
  Invoke-CloseRun 'close_plain' $false
  Invoke-CloseRun 'auth_exit' $true
  $delta = $script:closeMs['auth_exit'] - $script:closeMs['close_plain']
  Write-Output ('  (close with the login dialog open costs ' + $delta + ' ms more than without it)')
  if ($delta -gt 1000) { $fails += ('auth_exit: +' + $delta + ' ms vs the same close without the dialog (> 1000 ms)') }
  $totals.teardown = $delta
}

# ---------------------------------------------------------------- 3) the socket case
# A local HTTP server that promises 1 MB and delivers 2 KB, then closes the connection:
# the transport ends up reading a socket that is already closed (the field log line was
# "QIODevice::read (QSslSocket): device not open", repeated). SXCL_UI_MANIFEST_URL is the
# product's own hook for pinning the version-manifest address, so this is a real request
# through the product path -- just aimed at a server that misbehaves on purpose.
$name = 'socket_closed'
foreach ($k in $owned) { Remove-Item ('Env:' + $k) -ErrorAction SilentlyContinue }
$listener = $null
try {
  $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
  $listener.Start()
  $port = $listener.LocalEndpoint.Port
  $logDir = Join-Path $Work ('logs_' + $Tag + '_' + $name)
  Remove-Item -Recurse -Force $logDir -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $logDir | Out-Null
  $dataDir = Join-Path $Work ('data_' + $Tag + '_' + $name)
  Remove-Item -Recurse -Force $dataDir -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path $dataDir | Out-Null

  $env:SXCL_UI_ROUTE = 'versions'
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = (Join-Path $mc '.minecraft')
  # SXCL_LOG_DIR (no "UI" in the name) is the core library's switch: it moves the run log AND
  # the hang reports into this private dir, so the user's own logs\crashes are never touched.
  $env:SXCL_LOG_DIR = $logDir
  $env:SXCL_UI_DATA_DIR = $dataDir
  $env:SXCL_UI_MANIFEST_URL = ('http://127.0.0.1:' + $port + '/version_manifest_v2.json')
  Remove-Item Env:SXCL_UI_DUMP -ErrorAction SilentlyContinue
  $env:SXCL_UI_SHOT_DELAY = [string]$KeepMs
  $shot = Join-Path $Work ('shot_' + $Tag + '_' + $name + '.png')
  Remove-Item -Force $shot -ErrorAction SilentlyContinue
  $env:SXCL_UI_SHOT = $shot

  $out = Join-Path $Work ('run_' + $Tag + '_' + $name + '.txt')
  $err = $out + '.err'
  Remove-Item -Force $out, $err -ErrorAction SilentlyContinue
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $proc = Start-Process -FilePath $Exe -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
  $pending = $listener.BeginAcceptTcpClient($null, $null)
  $served = 0
  while (-not $proc.HasExited) {
    if ($pending -ne $null -and $pending.IsCompleted) {
      try {
        $client = $listener.EndAcceptTcpClient($pending)
        $stream = $client.GetStream()
        $stream.ReadTimeout = 500
        $buf = New-Object byte[] 2048
        try { [void]$stream.Read($buf, 0, $buf.Length) } catch { }
        $head = 'HTTP/1.1 200 OK' + "`r`n" + 'Content-Type: application/json' + "`r`n" +
                'Content-Length: 1048576' + "`r`n" + 'Connection: close' + "`r`n`r`n"
        $body = '{"latest":{"release":"1.21.4"},"versions":[{"id":"1.21.4"'   # deliberately cut short
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($head + $body)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        Start-Sleep -Milliseconds 300
        $client.Close()          # close with 1 MB still promised
        $served++
      } catch { }
      $pending = $listener.BeginAcceptTcpClient($null, $null)
    }
    Start-Sleep -Milliseconds 20
  }
  $proc.WaitForExit()
  $sw.Stop()
  Write-Output ('  (mock server served ' + $served + ' connection(s) on port ' + $port + ')')
  Measure-Run $name $logDir $out $err ([int]$sw.ElapsedMilliseconds) $proc.ExitCode $shot
} finally {
  if ($listener -ne $null) { try { $listener.Stop() } catch { } }
}

# ---------------------------------------------------------------- 3) verdict
$runCount = $totals.runs
Write-Output ('STALL-SUMMARY tag=' + $Tag + ' runs=' + $runCount +
              ' stall_lines=' + $totals.stalls + ' stall_max=' + $totals.max +
              ' moveToThread=' + $totals.move + ' invoke_missing=' + $totals.invoke +
              ' socket_closed=' + $totals.socket + ' bg_wrong_thread=' + $totals.bg +
              ' hangs=' + $totals.hangs + ' auth_teardown_ms=' + $totals.teardown)
if ($fails.Count -gt 0) {
  Write-Output ('NO-STALL: FAIL (' + $fails.Count + ' finding(s))')
  foreach ($f in $fails) { Write-Output ('  - ' + $f) }
  exit 1
}
Write-Output ('NO-STALL: ALL OK (runs=' + $runCount + ' stalls=0 hangs=0)')
exit 0
