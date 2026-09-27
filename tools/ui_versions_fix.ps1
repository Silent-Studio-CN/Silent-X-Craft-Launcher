# (C) Silent X Craft Launcher -- "version select ordering + repair" and "launch page rewrite"
# acceptance (user 2026-09-27, four sentences).
#
# What this proves, per run (every number is measured on this machine, nothing is guessed):
#
#   A) version select page: the healthy versions are ordered by MC version number, newest first
#      - the evidence line "[sxcl-ui] version-row select: ..." now carries group= and order=
#        (appended after tip=, so the older scripts' regex still matches every earlier field)
#      - assertion: the first five rows (order 0..4) are the five newest of the fixture, and the
#        last five rows are exactly the broken group, in version order inside the group
#      - every row with group=broken comes AFTER the "[sxcl-ui] version-group select:" line
#      - "MC version number" here means the number written in the version file (core library),
#        falling back to the folder name -- never a guess
#
#   B) broken versions are one separate group at the bottom
#      - the group header is one short line (#sxclVersionGroupTitle) and does not leak
#        implementation details (no JSON / jar / library words in it)
#      - each broken row carries one sentence saying what is missing (missing version file /
#        missing game jar / missing N libraries / missing parent version) plus the repair action
#
#   C) the action word is repair (0x4FEE 0x590D), not the old "go download" -- the user said the
#      old action threw the user onto the download page and left them there
#      - every broken row's action is 0x4FEE 0x590D and healthy rows have no action at all
#
#   D) launch page rewrite: no command line / game output / Java version widgets at all
#      - the dump must contain no QPlainTextEdit, no "final command line" / "game output" /
#        "Java:" text, and must contain the loading spinner (#sxclLaunchSpinner), the progress
#        bar (#sxclLaunchProgress) and the tip label (#sxclLaunchTip)
#
#   E) the tips really rotate: two runs with different dump delays read different tip text
#
# Runs with "-platform offscreen": it never needs the screen and never fights with a launcher
# the user has open for testing (the older scripts drive a real window and would steal focus).
#
# Usage:  pwsh -File tools/ui_versions_fix.ps1 [-Exe <path to sxcl-ui.exe>]
#
# ASCII-only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM),
# so every Chinese string below is built from code points.
param(
  [string]$Exe = ''
)
$ErrorActionPreference = 'Continue'

foreach ($v in @('SXCL_UI_CONFIG','SXCL_UI_NAV','SXCL_UI_COLLAPSE','SXCL_UI_RAILS_TEST','SXCL_UI_EDITION',
                 'SXCL_UI_SCROLL','SXCL_UI_POPUP','SXCL_UI_AUTH_DIALOG','SXCL_UI_JRE_HOSTED','SXCL_UI_MODS_QUERY',
                 'SXCL_UI_LAUNCH','SXCL_UI_DOWNLOAD','SXCL_UI_ICON_POPUP','SXCL_UI_SCROLLBAR_STRESS',
                 'SXCL_UI_ACCENT_APPLY','SXCL_UI_THEME_SWITCH','SXCL_UI_TRACE','SXCL_UI_ACCEPT',
                 'SXCL_UI_SETTINGS_ACCEPT','SXCL_UI_VERSION','SXCL_UI2','SXCL_UI_DUMP','SXCL_UI_DUMP_DEPTH',
                 'SXCL_UI_SHOT','SXCL_UI_SHOT_DELAY','SXCL_UI_ROUTE','SXCL_UI_THEME','SXCL_UI_WINDOW',
                 'SXCL_UI_GAME_DIR','SXCL_UI_MANIFEST','SXCL_UI_LAUNCH_DRY_RUN','SXCL_UI_DATA_DIR')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrEmpty($Exe)) { $Exe = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe' }
$work = 'D:\SilentStudio\_test\sxcl_versions_fix'
$fail = 0

function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }

# ---- Chinese fragments (code points) ----
$T_FIX   = C @(0x4FEE,0x590D)                       # repair
$T_MISS  = C @(0x7F3A)                              # "missing"
$T_LAUNCH = C @(0x542F,0x52A8)                      # "launch"
$T_CMD   = C @(0x6700,0x7EC8,0x547D,0x4EE4,0x884C)  # "final command line"
$T_OUT   = C @(0x6E38,0x620F,0x8F93,0x51FA)         # "game output"

if (-not (Test-Path $Exe)) {
  Write-Output ('VERSIONS-FIX: FAIL (exe not found: ' + $Exe + ')')
  exit 1
}
Write-Output ('exe under test: ' + $Exe)
Write-Output ('  last write    : ' + (Get-Item $Exe).LastWriteTime)

# ---- fixtures: nine healthy versions + five broken ones ----
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
$game = Join-Path $work 'mc'
$healthy = @('26.3','25w14a','1.21.4','1.21.1','1.20.6','1.20.1','1.19.4','1.12.2','1.7.10')
$broken  = @('1.18.2','1.16.5','1.14.4','1.8.9','1.6.4')
foreach ($id in $healthy) {
  New-Item -ItemType Directory -Force -Path (Join-Path $game ('versions\' + $id)) | Out-Null
  $j = @{ id=$id; type='release'; mainClass='net.minecraft.client.main.Main'; clientVersion=$id }
  Set-Content -Path (Join-Path $game ('versions\' + $id + '\' + $id + '.json')) -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8
  Set-Content -Path (Join-Path $game ('versions\' + $id + '\' + $id + '.jar')) -Value 'fixture-jar' -Encoding ascii
}
# 1.18.2: version file present, game jar and one library missing
New-Item -ItemType Directory -Force -Path (Join-Path $game 'versions\1.18.2') | Out-Null
$j = @{ id='1.18.2'; type='release'; mainClass='net.minecraft.client.main.Main'; clientVersion='1.18.2';
        libraries=@(@{ name='org.example:demo:1.0'; downloads=@{ artifact=@{
          path='org/example/demo/1.0/demo-1.0.jar'; url='https://example.invalid/demo.jar';
          sha1='0000000000000000000000000000000000000000'; size=1024 } } }) }
Set-Content -Path (Join-Path $game 'versions\1.18.2\1.18.2.json') -Value ($j | ConvertTo-Json -Depth 8) -Encoding utf8
# 1.16.5: version file present, game jar missing
New-Item -ItemType Directory -Force -Path (Join-Path $game 'versions\1.16.5') | Out-Null
$j = @{ id='1.16.5'; type='release'; mainClass='net.minecraft.client.main.Main'; clientVersion='1.16.5' }
Set-Content -Path (Join-Path $game 'versions\1.16.5\1.16.5.json') -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8
# 1.14.4: parent version 1.13.2 is not installed
New-Item -ItemType Directory -Force -Path (Join-Path $game 'versions\1.14.4') | Out-Null
$j = @{ id='1.14.4'; type='release'; mainClass='net.minecraft.client.main.Main'; inheritsFrom='1.13.2' }
Set-Content -Path (Join-Path $game 'versions\1.14.4\1.14.4.json') -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8
# 1.8.9: no version file at all (a jar only)
New-Item -ItemType Directory -Force -Path (Join-Path $game 'versions\1.8.9') | Out-Null
Set-Content -Path (Join-Path $game 'versions\1.8.9\1.8.9.jar') -Value 'fixture-jar' -Encoding ascii
# 1.6.4: version file present, game jar missing
New-Item -ItemType Directory -Force -Path (Join-Path $game 'versions\1.6.4') | Out-Null
$j = @{ id='1.6.4'; type='release'; mainClass='net.minecraft.client.main.Main'; clientVersion='1.6.4' }
Set-Content -Path (Join-Path $game 'versions\1.6.4\1.6.4.json') -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8

$ini = Join-Path $work 'versions.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $game) -Encoding utf8
Add-Content -Path $ini -Value 'game.selected_version=1.21.4'

$allIds = $healthy + $broken

function Invoke-Run([string]$tag, [string]$route, [int]$delayMs, [string]$selected) {
  if (-not [string]::IsNullOrEmpty($selected)) {
    $lines = @(Get-Content $ini -Encoding utf8 | Where-Object { $_ -notlike 'game.selected_version=*' })
    $lines += ('game.selected_version=' + $selected)
    Set-Content -Path $ini -Value $lines -Encoding utf8
  }
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = $game
  $env:SXCL_UI_WINDOW = '1100x900'
  $env:SXCL_UI_ROUTE = $route
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '14'
  $env:SXCL_UI_SHOT_DELAY = [string]$delayMs
  $env:SXCL_UI_LAUNCH_DRY_RUN = '1'
  $png = Join-Path $work ('shot_' + $tag + '.png')
  $out = Join-Path $work ('dump_' + $tag + '.txt')
  $err = $out + '.err'
  Remove-Item $png -ErrorAction SilentlyContinue
  $env:SXCL_UI_SHOT = $png
  Start-Process -FilePath $Exe -ArgumentList '-platform','offscreen' -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  return @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) + @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
}

# =====================================================================================
# A/B/C -- version select page
# =====================================================================================
Write-Output ''
Write-Output '== A/B/C: version select page (order, grouping, action word) =='
$lines = Invoke-Run 'select' 'select' 4000 ''
$pattern = 'version-row select: id=(?<id>\S+) state=(?<state>\S+) launchable=(?<launchable>\d) problem=(?<problem>\S+) base="(?<base>[^"]*)" baseFrom=(?<baseFrom>\S+) coreReliable=(?<cr>\d) info="(?<info>[^"]*)" note="(?<note>[^"]*)" action="(?<action>[^"]*)" path="(?<path>[^"]*)" tip="(?<tip>[^"]*)" group=(?<group>\S+) order=(?<order>\d+)'
$rows = @{}
foreach ($line in $lines) {
  $m = [regex]::Match($line, $pattern)
  if (-not $m.Success) { continue }
  $rows[[int]$m.Groups['order'].Value] = @{
    id = $m.Groups['id'].Value; state = $m.Groups['state'].Value
    launchable = $m.Groups['launchable'].Value; note = $m.Groups['note'].Value
    action = $m.Groups['action'].Value; group = $m.Groups['group'].Value
    base = $m.Groups['base'].Value
  }
}
$order = @($rows.Keys | Sort-Object)
Write-Output ('rows with an evidence line: ' + $order.Count + ' (want ' + $allIds.Count + ')')
if ($order.Count -ne $allIds.Count) {
  Write-Output ('  -> FAIL expected one evidence line per fixture, got ' + $order.Count); $fail++
}
$ids = @()
foreach ($i in $order) { $ids += $rows[$i].id }
Write-Output ('display order (top to bottom): ' + ($ids -join ' | '))

$wantFirst5 = @('26.3','25w14a','1.21.4','1.21.1','1.20.6')
$gotFirst5 = @($ids[0..4])
if (($gotFirst5 -join ',') -ne ($wantFirst5 -join ',')) {
  Write-Output ('  -> FAIL first five rows: got ' + ($gotFirst5 -join ',') + ' want ' + ($wantFirst5 -join ',')); $fail++
} else {
  Write-Output ('first five (newest first): ' + ($gotFirst5 -join ' > '))
}
$wantLast5 = @('1.18.2','1.16.5','1.14.4','1.8.9','1.6.4')
$gotLast5 = @($ids[($ids.Count - 5)..($ids.Count - 1)])
if (($gotLast5 -join ',') -ne ($wantLast5 -join ',')) {
  Write-Output ('  -> FAIL last five rows: got ' + ($gotLast5 -join ',') + ' want ' + ($wantLast5 -join ',')); $fail++
} else {
  Write-Output ('last five (broken group, newest first): ' + ($gotLast5 -join ' > '))
}

# broken rows must all sit at the end, in version order, with the repair action
$seenBroken = $false
$brokenIds = @()
foreach ($i in $order) {
  $r = $rows[$i]
  if ($r.group -eq 'broken') {
    $seenBroken = $true
    $brokenIds += $r.id
    if ($r.state -ne 'warn') { Write-Output ('  -> FAIL ' + $r.id + ' is in the broken group but its icon is ' + $r.state); $fail++ }
    if ($r.action -ne $T_FIX) { Write-Output ('  -> FAIL ' + $r.id + ' action is "' + $r.action + '" (want repair)'); $fail++ }
    if ($r.note -notlike ($T_MISS + '*')) { Write-Output ('  -> FAIL ' + $r.id + ' does not say what is missing: "' + $r.note + '"'); $fail++ }
  } else {
    if ($seenBroken) { Write-Output ('  -> FAIL ' + $r.id + ' (group=' + $r.group + ') comes after a broken row'); $fail++ }
    if ($r.action -ne '') { Write-Output ('  -> FAIL launchable row ' + $r.id + ' still offers an action: ' + $r.action); $fail++ }
  }
}
Write-Output ('broken rows, in order: ' + ($brokenIds -join ' | '))
if (($brokenIds -join ',') -ne ($wantLast5 -join ',')) {
  Write-Output '  -> FAIL the broken group is not the whole tail in version order'; $fail++
}
$groupLines = @($lines | Select-String -Pattern 'version-group select: broken=(\d+) title="([^"]*)"')
if ($groupLines.Count -ne 1) {
  Write-Output ('  -> FAIL group header lines = ' + $groupLines.Count + ' (want 1)'); $fail++
} else {
  $gm = [regex]::Match($groupLines[0].Line, 'broken=(\d+) title="([^"]*)"')
  $n = [int]$gm.Groups[1].Value
  $title = $gm.Groups[2].Value
  Write-Output ('group header: broken=' + $n + ' title="' + $title + '"')
  if ($n -ne $broken.Count) { Write-Output ('  -> FAIL group header count ' + $n + ' (want ' + $broken.Count + ')'); $fail++ }
  if ($title -notlike ('*' + $T_FIX + '*')) { Write-Output '  -> FAIL group header does not mention the repair action'; $fail++ }
  foreach ($bad in @('JSON', 'jar', $T_MISS)) {
    if ($title -like ('*' + $bad + '*')) { Write-Output ('  -> FAIL group header leaks implementation detail: ' + $title); $fail++ }
  }
  if ($title.Length -gt 40) { Write-Output ('  -> FAIL group header is ' + $title.Length + ' chars (want one short line)'); $fail++ }
}

# =====================================================================================
# D/E -- launch page (rewritten) and the rotating tips
# =====================================================================================
Write-Output ''
Write-Output '== D: launch page dump (no command line / game output / Java version) =='
# the launch page is a temporary page: SXCL_UI_LAUNCH clicks the launch button on the home page
# (that is the only product path that reaches it -- main.cpp has no "open route launch" hook).
$env:SXCL_UI_LAUNCH = '1'
$env:SXCL_UI_LAUNCH_DELAY = '900'
$runA = Invoke-Run 'launch_a' 'home' 3000 '9.9.9-not-installed'
# Only the WIDGET TREE section is inspected: the product's own runtime log on stderr naturally
# mentions Java / command lines (that is for whoever debugs it) -- what the user asked to remove
# are the controls ON SCREEN.  Every dump line carries a geometry "(x,y WxH)", log lines do not.
$dumpA = @($runA | Where-Object { $_ -match '\(-?\d+,-?\d+ \d+x\d+\)' })
$textA = ($dumpA -join "`n")
Write-Output ('dump lines: ' + $dumpA.Count + ' (out of ' + $runA.Count + ' lines of process output)')
$pageLine = @($dumpA | Select-String -Pattern '^ScrollArea #(sxclPage_\w+|LaunchProgressPage)')
Write-Output ('page in the dump: ' + (($pageLine | ForEach-Object { $_.Line.Trim() }) -join ' ; '))
if ($textA -notmatch '#sxclLaunchSpinner') { Write-Output '  -> FAIL no loading spinner in the dump'; $fail++ } else { Write-Output 'loading spinner: #sxclLaunchSpinner present' }
if ($textA -notmatch '#sxclLaunchProgress') { Write-Output '  -> FAIL no progress bar in the dump'; $fail++ } else { Write-Output 'progress bar: #sxclLaunchProgress present' }
$tipA = ''
$tm = [regex]::Match($textA, '#sxclLaunchTip \(\d+,\d+ \d+x\d+\)( hidden)? "([^"]*)"')
if ($tm.Success) { $tipA = $tm.Groups[2].Value }
Write-Output ('tip (run A): "' + $tipA + '"')
if ([string]::IsNullOrEmpty($tipA)) { Write-Output '  -> FAIL no tip text in the dump'; $fail++ }
foreach ($bad in @('QPlainTextEdit', $T_CMD, $T_OUT, 'Java:', 'classpath', 'accessToken')) {
  $hits = @($dumpA | Select-String -Pattern $bad -SimpleMatch)
  if ($hits.Count -ne 0) { Write-Output ('  -> FAIL the dump still contains "' + $bad + '": ' + $hits[0].Line.Trim()); $fail++ }
  else { Write-Output ('dump lines containing "' + $bad + '": 0') }
}
if ($textA -notmatch [regex]::Escape($T_LAUNCH)) { Write-Output '  -> FAIL the page does not say what it is launching'; $fail++ }

Write-Output ''
Write-Output '== E: tips rotate (two dumps at different times differ) =='
$runB = Invoke-Run 'launch_b' 'home' 12000 '9.9.9-not-installed'
$dumpB = @($runB | Where-Object { $_ -match '\(-?\d+,-?\d+ \d+x\d+\)' })
$textB = ($dumpB -join "`n")
$tipB = ''
$tm = [regex]::Match($textB, '#sxclLaunchTip \(\d+,\d+ \d+x\d+\)( hidden)? "([^"]*)"')
if ($tm.Success) { $tipB = $tm.Groups[2].Value }
Write-Output ('tip (run B): "' + $tipB + '"')
if ([string]::IsNullOrEmpty($tipB)) { Write-Output '  -> FAIL no tip text in run B'; $fail++ }
if ($tipA -eq $tipB) { Write-Output '  -> FAIL the tip did not change between the two runs'; $fail++ } else { Write-Output 'tip changed between the two runs: yes' }

Write-Output ''
Write-Output ('product files: ' + $work)
if ($fail -eq 0) {
  Write-Output 'VERSIONS-FIX: ALL OK (ordering newest-first, broken group at the bottom with what is missing + repair action, launch page carries no technical widgets, tips rotate)'
  exit 0
}
Write-Output ('VERSIONS-FIX: FAIL (' + $fail + ' assertion(s))')
exit 1
