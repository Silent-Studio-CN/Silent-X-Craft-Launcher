# (C) Silent X Craft Launcher -- UI "no hang" acceptance.
#
# Why: user report 2026-09-26 -- "launching the game, SXCL still shows Not Responding".
# The shell keeps a hang watchdog (src/ui/crash_handler.cpp): if the GUI thread does not run
# its event loop for 3 s it drops logs/crashes/sxcl-ui-hang-<time>.txt (+ .dmp). This script
# turns that into a **number**:
#
#   1) delete every old sxcl-ui-hang-* report;
#   2) run build-ui/src/ui/Release/sxcl-ui.exe once per route (home / versions / download /
#      settings / select) with SXCL_UI_DUMP=1 + SXCL_UI_SHOT=<png> + SXCL_UI_SHOT_DELAY=12000,
#      so every run must stay alive >= 12 s and grab its own screenshot at the end;
#   3) assert: ZERO new sxcl-ui-hang-* files, no "ui-stall" of 3000 ms or more, every run wrote
#      its screenshot and exited with code 0.
#
# Output: "NO-HANG: ALL OK (routes=5 hangs=0)" or "NO-HANG: FAIL ..." (exit 1).
#
# Temp/acceptance files always live under D:\SilentStudio\_test (never %TEMP%, never C:).
# Keep this file ASCII-only + UTF-8 BOM (Windows PowerShell 5.1 parses .ps1 as ANSI without a BOM).
#
# Parameters (all optional; the defaults are exactly the old behaviour):
#   -Exe    <path>  run another build's sxcl-ui.exe (e.g. a private build dir)
#   -Work   <dir>   where temp fixtures/screenshots go
#   -LogDir <dir>   pin SXCL_LOG_DIR for every run, so the logs and any hang report the app
#                   writes land here instead of the user's %APPDATA%\SilentXCraftLauncher\logs
#                   (never delete/overwrite the evidence in the user's own log directory).
param(
  [string]$Exe = '',
  [string]$Work = 'D:\SilentStudio\_test\ui_no_hang',
  [string]$LogDir = ''
)
$ErrorActionPreference = 'Continue'
$exe = if ($Exe -ne '') { $Exe } else { Join-Path $PSScriptRoot '..\build-ui\src\ui\Release\sxcl-ui.exe' }
$work = $Work
New-Item -ItemType Directory -Force -Path $work | Out-Null
$crashDir = if ($LogDir -ne '') { Join-Path $LogDir 'crashes' } else { Join-Path $env:APPDATA 'SilentXCraftLauncher\logs\crashes' }
New-Item -ItemType Directory -Force -Path $crashDir | Out-Null

# ---------------------------------------------------------------- fixture (game dir + settings)
# Two installed instances so the version pages have real content (a loader instance without its
# own jar is the case the core scanner must still count).
$mc = Join-Path $work 'mc'
$versions = Join-Path $mc '.minecraft\versions'
New-Item -ItemType Directory -Force -Path (Join-Path $versions '1.20.1'), (Join-Path $versions 'forge-47.2.0') | Out-Null
Set-Content -Path (Join-Path $versions '1.20.1\1.20.1.json') -Value '{"id":"1.20.1"}' -Encoding ascii
Set-Content -Path (Join-Path $versions 'forge-47.2.0\forge-47.2.0.json') -Value '{"id":"forge-47.2.0","inheritsFrom":"1.20.1"}' -Encoding ascii
$ini = Join-Path $work 'ui_no_hang.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $mc + '\.minecraft') -Encoding utf8

# ---------------------------------------------------------------- 1) delete old hang reports
$kept = 0
Get-ChildItem $crashDir -Filter 'sxcl-ui-hang-*' -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName } | ForEach-Object {
  Remove-Item -Force -ErrorAction SilentlyContinue $_
  $kept++
}
Write-Output ('cleaned old hang files: ' + $kept)

function Get-HangNames {
  $out = @()
  Get-ChildItem $crashDir -Filter 'sxcl-ui-hang-*' -ErrorAction SilentlyContinue | ForEach-Object { $out += $_.Name }
  return $out
}

# ---------------------------------------------------------------- 2) run every route
$routes = @('home', 'versions', 'download', 'settings', 'select')
$hangs = 0
$fails = @()
foreach ($route in $routes) {
  $env:SXCL_UI_ROUTE = $route
  $env:SXCL_UI_SETTINGS = $ini
  # pin the app's log dir when asked to: logs + any hang report stay out of the user's own logs
  if ($LogDir -ne '') { $env:SXCL_LOG_DIR = $LogDir } else { Remove-Item Env:SXCL_LOG_DIR -ErrorAction SilentlyContinue }
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_SHOT_DELAY = '12000'
  $shot = Join-Path $work ($route + '.png')
  Remove-Item -Force -ErrorAction SilentlyContinue $shot
  $env:SXCL_UI_SHOT = $shot
  # no other acceptance hook may interfere with this run. Clearing only RAILS_TEST was not
  # enough: when this script runs in the same shell right after another acceptance script,
  # hooks that one set (e.g. SXCL_UI_WINCHECK=...;quit) leak into these runs and make the app
  # quit on its own -- 5 s runs with no screenshot, which looks like a "NO-HANG: FAIL" that has
  # nothing to do with hangs. Every hook this script does not own is cleared here.
  foreach ($v in @('SXCL_UI_RAILS_TEST', 'SXCL_UI_WINCHECK', 'SXCL_UI_NAV', 'SXCL_UI_MODS_QUERY',
                   'SXCL_UI_MODS_SOURCES', 'SXCL_UI_THEME_SWITCH', 'SXCL_UI_RESIZE',
                   'SXCL_UI_AUTH_DIALOG', 'SXCL_UI_MANIFEST_URL', 'SXCL_UI_ROW_EVIDENCE')) {
    Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
  }

  $before = Get-HangNames
  $out = Join-Path $work ($route + '.out.txt')
  $err = Join-Path $work ($route + '.err.txt')
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $proc = Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -Wait
  $sw.Stop()
  $after = Get-HangNames
  $new = @($after | Where-Object { $before -notcontains $_ })
  $hangs += $new.Count

  # the 12 s life + screenshot + exit code are part of "the UI really stayed responsive"
  $ms = [int]$sw.ElapsedMilliseconds
  $shotOk = (Test-Path $shot) -and ((Get-Item $shot).Length -gt 0)
  $exitOk = ($proc.ExitCode -eq 0)
  $stallMax = 0
  foreach ($line in (Get-Content $err -ErrorAction SilentlyContinue)) {
    if ($line -match 'ui-stall: (\d+) ms') {
      $v = [int]$Matches[1]
      if ($v -gt $stallMax) { $stallMax = $v }
    }
  }
  $ok = ($new.Count -eq 0) -and ($ms -ge 12000) -and $shotOk -and $exitOk -and ($stallMax -lt 3000)
  if (-not $ok) {
    if ($new.Count -gt 0) { $fails += ($route + ': ' + $new.Count + ' new hang file(s): ' + ($new -join ',')) }
    if ($ms -lt 12000) { $fails += ($route + ': exited after ' + $ms + ' ms (< 12000)') }
    if (-not $shotOk) { $fails += ($route + ': screenshot missing/empty') }
    if (-not $exitOk) { $fails += ($route + ': exit code ' + $proc.ExitCode) }
    if ($stallMax -ge 3000) { $fails += ($route + ': ui-stall ' + $stallMax + ' ms') }
  }
  Write-Output ('  route=' + $route + ' ms=' + $ms + ' exit=' + $proc.ExitCode + ' hangs=' + $new.Count + ' stall_max=' + $stallMax + ' shot=' + $(if ($shotOk) { 'OK' } else { 'MISSING' }) + ' -> ' + $(if ($ok) { 'OK' } else { 'FAIL' }))
}

# ---------------------------------------------------------------- 3) verdict
$total = $routes.Count
if (($hangs -eq 0) -and ($fails.Count -eq 0)) {
  Write-Output ('NO-HANG: ALL OK (routes=' + $total + ' hangs=0)')
  exit 0
}
Write-Output ('NO-HANG: FAIL (routes=' + $total + ' hangs=' + $hangs + ')')
foreach ($f in $fails) { Write-Output ('  - ' + $f) }
exit 1
