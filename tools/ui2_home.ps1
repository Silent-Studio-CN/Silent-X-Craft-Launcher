# (C) Silent X Craft Launcher -- ui2 home acceptance (docs/27 M2: task primitive + home page).
#
# What it proves, per run, all numbers measured on this machine (nothing guessed):
#   1) SXCL_UI2=1 + 1100x750: the four home blocks are really there and laid out
#      (hero card / player card / version accordion / notice card) -- geometry from
#      SXCL_UI_DUMP, cross-checked against the 24 px page margin and the 16 px gaps.
#   2) player card top-right edition toggle (#sxcl2EditionPremium / #sxcl2EditionOffline):
#      objectName + size (32x32) + gap (8); the CURRENT state carries accent pixels in its
#      outer 4 px ring while the other one carries exactly 0. Clicking the other one moves the
#      highlight AND writes game.edition to the settings file (runs B/C); a restart with no
#      click reads it back (run D).
#   3) hover: SXCL_UI2_HOVER_NAME=1 sends real Enter/Leave to the player-name row; the three
#      action icons (skin / account / rename) flip hidden -> visible -> hidden and neither the
#      row nor the name label moves (that is "normally takes no space").
#   4) accordion: clicking "swap version" expands the list container IN PLACE
#      (height 0 -> >0 -> 0, rows visible at 34 px each, staggered 25 ms/row) and the route
#      string does NOT change; run G clicks "show all versions" and the route DOES change
#      (so the "route unchanged" assertion is not vacuous).
#   5) notice card: with no content the widget is not in the dump at all and the four words
#      "no announcement" filler appear nowhere; with content it shows up with height > 0.
#   6) zero GUI-thread blocking: a 12 s run leaves ZERO new sxcl-ui-hang-* reports and no
#      ui-stall >= 3000 ms; the task criterion lines say the work ran on a DIFFERENT thread
#      than the GUI thread and the callback came back ON the GUI thread.
#   7) pages/** never touches a blocking source directly (static grep) and no
#      MAIN-THREAD-VIOLATION line shows up at runtime (the sources' own guard).
#
# Output: "UI2-HOME: ALL OK (...)" or "UI2-HOME: FAIL (...)" (exit 1).
# Temp/acceptance files always live under D:\SilentStudio\_test (never %TEMP%, never C:).
# Keep this file ASCII-only + UTF-8 BOM (Windows PowerShell 5.1 parses .ps1 as ANSI without a BOM).
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\ui2'
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $work
New-Item -ItemType Directory -Force -Path $work | Out-Null
$crashDir = Join-Path $env:APPDATA 'SilentXCraftLauncher\logs\crashes'
New-Item -ItemType Directory -Force -Path $crashDir | Out-Null
$fail = 0
$checks = 0
Add-Type -AssemblyName System.Drawing

# Chinese strings we must look for / must NOT find, built from code points so this file stays ASCII.
$sNoNotice = [string]([char]0x6682 + [char]0x65E0 + [char]0x516C + [char]0x544A)  # forbidden filler
$sNoticeTitle = [string]([char]0x516C + [char]0x544A)                              # notice title

function Fail([string]$msg) { Write-Output ('  -> FAIL ' + $msg); $script:fail++ }
function Check([bool]$cond, [string]$msg) {
  $script:checks++
  if ($cond) { Write-Output ('  ok   ' + $msg) } else { Fail $msg }
}

if (-not (Test-Path $exe)) {
  Write-Output ('UI2-HOME: FAIL (exe not found: ' + $exe + ')')
  exit 1
}
$busy = @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
  Write-Output ('UI2-HOME: FAIL (another sxcl-ui.exe is running, pid ' + ($busy.Id -join ',') + ')')
  exit 1
}

# ---------------------------------------------------------------- fixture (game dir + settings)
$mc = Join-Path $work 'mc'
$versions = Join-Path $mc '.minecraft\versions'
foreach ($id in @('1.20.1', 'forge-47.2.0', '1.20.1-fabric', '26.3')) {
  New-Item -ItemType Directory -Force -Path (Join-Path $versions $id) | Out-Null
}
# three launchable instances (vanilla + Forge + Fabric) and one broken one (empty JSON), so both
# the "current version" card and the version rows carry real content.
$mcJson = '{"id":"1.20.1","type":"release","mainClass":"net.minecraft.client.main.Main","releaseTime":"2023-06-12T13:25:51+00:00"}'
Set-Content -Path (Join-Path $versions '1.20.1\1.20.1.json') -Value $mcJson -Encoding ascii
Set-Content -Path (Join-Path $versions '1.20.1\1.20.1.jar') -Value 'stub' -Encoding ascii
$forgeJson = '{"id":"forge-47.2.0","type":"release","inheritsFrom":"1.20.1","mainClass":"cpw.mods.bootstraplauncher.BootstrapLauncher","libraries":[{"name":"net.minecraftforge:forge:47.2.0"}]}'
Set-Content -Path (Join-Path $versions 'forge-47.2.0\forge-47.2.0.json') -Value $forgeJson -Encoding ascii
$fabricJson = '{"id":"1.20.1-fabric","type":"release","inheritsFrom":"1.20.1","mainClass":"net.fabricmc.loader.impl.launch.knot.KnotClient","libraries":[{"name":"net.fabricmc:fabric-loader:0.15.11"}]}'
Set-Content -Path (Join-Path $versions '1.20.1-fabric\1.20.1-fabric.json') -Value $fabricJson -Encoding ascii
Set-Content -Path (Join-Path $versions '26.3\26.3.json') -Value '{}' -Encoding ascii
$ini = Join-Path $work 'ui2.ini'
function Write-Ini([string]$edition) {
  Set-Content -Path $ini -Value ('game.default_dir=' + $mc + '\.minecraft') -Encoding utf8
  Add-Content -Path $ini -Value ('game.edition=' + $edition)
  Add-Content -Path $ini -Value 'player.name=Steve'
}
Write-Ini 'offline'
function Get-IniEdition {
  $hit = Select-String -Path $ini -Pattern 'game\.edition=(\w+)' | Select-Object -Last 1
  if ($hit -eq $null) { return '' }
  return $hit.Matches[0].Groups[1].Value
}

function Get-HangNames {
  $out = @()
  Get-ChildItem $crashDir -Filter 'sxcl-ui-hang-*' -ErrorAction SilentlyContinue | ForEach-Object { $out += $_.Name }
  return $out
}

# ---------------------------------------------------------------- run helper
$hookKeys = @('SXCL_UI2_HOVER_NAME', 'SXCL_UI2_CLICK_VERSION', 'SXCL_UI2_CLICK_ALL',
              'SXCL_UI2_EDITION', 'SXCL_UI2_NAV', 'SXCL_UI2_NOTICE')
function Invoke-Ui2Run([string]$tag, [hashtable]$hooks, [string]$shot, [int]$shotDelay) {
  $env:SXCL_UI2 = '1'
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_WINDOW = '1100x750'
  $env:SXCL_UI_DUMP = '1'
  foreach ($k in $hookKeys) { Remove-Item ('Env:' + $k) -ErrorAction SilentlyContinue }
  Remove-Item Env:SXCL_UI_SHOT -ErrorAction SilentlyContinue
  Remove-Item Env:SXCL_UI_SHOT_DELAY -ErrorAction SilentlyContinue
  foreach ($k in $hooks.Keys) { Set-Item ('Env:' + $k) ([string]$hooks[$k]) }
  if ($shot -ne '') {
    Remove-Item -Force -ErrorAction SilentlyContinue $shot
    $env:SXCL_UI_SHOT = $shot
    $env:SXCL_UI_SHOT_DELAY = [string]$shotDelay
  }
  $out = Join-Path $work ($tag + '.out.txt')
  $err = Join-Path $work ($tag + '.err.txt')
  $before = Get-HangNames
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $proc = Start-Process -FilePath $exe -ArgumentList '--ui2' -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -Wait
  $sw.Stop()
  $after = Get-HangNames
  $new = @($after | Where-Object { $before -notcontains $_ })
  $lines = @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) + @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  $stallMax = 0
  foreach ($line in $lines) { if ($line -match 'ui-stall: (\d+) ms') { $v = [int]$Matches[1]; if ($v -gt $stallMax) { $stallMax = $v } } }
  return @{ ms = [int]$sw.ElapsedMilliseconds; exit = $proc.ExitCode; lines = $lines;
            hangs = $new; stall = $stallMax; out = $out; err = $err }
}

function Get-States($run) {
  $states = @{}
  $cur = ''
  foreach ($line in $run.lines) {
    if ($line -match '^\[sxcl-ui2\] dump: state=(\S+) route=(\S+) violations=(\d+)') {
      $cur = $Matches[1]
      $states[$cur] = @{ route = $Matches[2]; violations = [int]$Matches[3]; lines = @() }
      continue
    }
    if ($cur -ne '' -and $states.ContainsKey($cur)) { $states[$cur].lines += $line }
  }
  return $states
}
function Get-Rect($states, [string]$state, [string]$name) {
  if (-not $states.ContainsKey($state)) { return $null }
  foreach ($line in $states[$state].lines) {
    if ($line -match ('#' + $name + ' \((\d+),(\d+) (\d+)x(\d+)\)( hidden)?')) {
      return @{ x = [int]$Matches[1]; y = [int]$Matches[2]; w = [int]$Matches[3]; h = [int]$Matches[4];
                hidden = [bool]$Matches[5]; line = $line.Trim() }
    }
  }
  return $null
}
function Get-Rects($states, [string]$state, [string]$name) {
  $out = @()
  if (-not $states.ContainsKey($state)) { return $out }
  foreach ($line in $states[$state].lines) {
    if ($line -match ('#' + $name + ' \((\d+),(\d+) (\d+)x(\d+)\)( hidden)?')) {
      $out += @{ x = [int]$Matches[1]; y = [int]$Matches[2]; w = [int]$Matches[3]; h = [int]$Matches[4];
                 hidden = [bool]$Matches[5] }
    }
  }
  return $out
}
function Rect-Text($r) {
  if ($r -eq $null) { return 'ABSENT' }
  return ('(' + $r.x + ',' + $r.y + ' ' + $r.w + 'x' + $r.h + ')' + $(if ($r.hidden) { ' hidden' } else { '' }))
}
function Get-Violations($states) {
  $n = 0
  foreach ($k in $states.Keys) { $n += $states[$k].violations }
  return $n
}
function Get-Route($states, [string]$state) {
  if (-not $states.ContainsKey($state)) { return '<no such state>' }
  return $states[$state].route
}

# ---------------------------------------------------------------- pixel helpers
function Get-Rgb([System.Drawing.Bitmap]$bmp, [int]$px, [int]$py) {
  $c = $bmp.GetPixel($px, $py)
  return @($c.R, $c.G, $c.B)
}
function Get-Hex($rgb) { return ('#{0:X2}{1:X2}{2:X2}' -f [int]$rgb[0], [int]$rgb[1], [int]$rgb[2]) }
function Get-Dist($a, $b) {
  return ([math]::Abs([int]$a[0] - [int]$b[0]) + [math]::Abs([int]$a[1] - [int]$b[1]) + [math]::Abs([int]$a[2] - [int]$b[2]))
}
function Get-CardRef([string]$png, $card, [double]$dpr) {
  $bmp = New-Object System.Drawing.Bitmap($png)
  $px = [int][math]::Floor(($card.x + 8) * $dpr)
  $py = [int][math]::Floor(($card.y + $card.h - 8) * $dpr)
  $ref = Get-Rgb $bmp $px $py
  $bmp.Dispose()
  return $ref
}
# One icon button: accent pixels inside the outer "band" logical px -- that band never contains
# the 20 px glyph, so the microsoft LOGO's own blue square cannot pollute the selection test --
# plus "ink" pixels in the inner box (proves the icon really rendered).
function Get-IconStats([string]$png, $rect, [double]$dpr, [int]$bandLogical, [int[]]$cardRef) {
  $bmp = New-Object System.Drawing.Bitmap($png)
  $x0 = [int][math]::Floor($rect.x * $dpr); $y0 = [int][math]::Floor($rect.y * $dpr)
  $x1 = [int][math]::Ceiling(($rect.x + $rect.w) * $dpr); $y1 = [int][math]::Ceiling(($rect.y + $rect.h) * $dpr)
  $band = [int][math]::Round($bandLogical * $dpr)
  $accent = 0; $bandTotal = 0; $ink = 0; $inner = 0
  for ($py = $y0; $py -lt $y1; $py++) {
    for ($px = $x0; $px -lt $x1; $px++) {
      $p = Get-Rgb $bmp $px $py
      $inBand = ($px -lt ($x0 + $band)) -or ($px -ge ($x1 - $band)) -or ($py -lt ($y0 + $band)) -or ($py -ge ($y1 - $band))
      if ($inBand) {
        $bandTotal++
        # the accent token is blue-dominant in both themes (#299cff dark / #0f6cbd light)
        if ((($p[2] - $p[0]) -gt 40) -and ($p[2] -gt 110)) { $accent++ }
      } else {
        $inner++
        if ((Get-Dist $p $cardRef) -gt 40) { $ink++ }
      }
    }
  }
  $bmp.Dispose()
  return @{ accent = $accent; bandTotal = $bandTotal; ink = $ink; inner = $inner }
}

# ---------------------------------------------------------------- 0) static page discipline
Write-Output 'static check: pages/** must not call blocking sources (docs/27 section 6 rule 1)'
$pageDir = Join-Path $root 'src\ui\ui2\pages'
$badPattern = 'sxcl_http_get_text|sxcl_manifest_|sxcl_instance_list|sxcl_instance_scan|QFile|QDir|QTextStream|QNetwork'
$bad = @(Get-ChildItem $pageDir -Include '*.cpp', '*.h' -Recurse | Select-String -Pattern $badPattern)
foreach ($b in $bad) { Write-Output ('  -> ' + $b.Path + ':' + $b.LineNumber + ' ' + $b.Line.Trim()) }
Check ($bad.Count -eq 0) ('pages/** has 0 blocking-source references (' + $bad.Count + ' found)')
$pageFiles = @(Get-ChildItem $pageDir -Include '*.cpp', '*.h' -Recurse | Select-Object -ExpandProperty Name)
Check ($pageFiles.Count -ge 1) ('page files scanned: ' + ($pageFiles -join ','))

Write-Output ('exe: ' + $exe)
Write-Output ('fixture: ' + $versions + ' (4 versions) ; settings ' + $ini)

# ================================================================ run A: four blocks + hover + accordion
Write-Output 'run A: four blocks + hover on the name row + accordion (one 4.5 s run)'
$pngA = Join-Path $work 'runA.png'
$runA = Invoke-Ui2Run 'runA' @{ SXCL_UI2_CLICK_VERSION = 1; SXCL_UI2_HOVER_NAME = 1 } $pngA 4500
$stA = Get-States $runA
Write-Output ('  exit=' + $runA.exit + ' ms=' + $runA.ms + ' hangs=' + $runA.hangs.Count + ' stall_max=' + $runA.stall)
Check ($runA.exit -eq 0) 'run A exit code 0'
Check ($runA.hangs.Count -eq 0) 'run A left no sxcl-ui-hang-* report'
Check ($stA.ContainsKey('idle')) 'run A dumped state=idle'
Check ((Get-Violations $stA) -eq 0) 'run A: MAIN-THREAD-VIOLATION count = 0 (all dumps)'
Check ((Get-Route $stA 'idle') -eq 'home') 'run A: route is "home"'
Check ($stA.ContainsKey('hover') -and $stA.ContainsKey('leave') -and $stA.ContainsKey('accordion-closed') -and
       $stA.ContainsKey('accordion-open') -and $stA.ContainsKey('accordion-close')) 'run A dumped all six hook states'

# ---- four blocks
$hero = Get-Rect $stA 'idle' 'sxcl2HeroCard'
$player = Get-Rect $stA 'idle' 'sxcl2PlayerCard'
$accOpen = Get-Rect $stA 'accordion-open' 'sxcl2VersionAccordion'
$bodyOpen = Get-Rect $stA 'accordion-open' 'sxcl2VersionAccordionBody'
$bodyClosed = Get-Rect $stA 'accordion-closed' 'sxcl2VersionAccordionBody'
$bodyIdle = Get-Rect $stA 'idle' 'sxcl2VersionAccordionBody'
$noticeA = Get-Rect $stA 'idle' 'sxcl2NoticeCard'
Write-Output ('  four blocks (logical px): hero=' + (Rect-Text $hero) + ' player=' + (Rect-Text $player) +
              ' accordion(open)=' + (Rect-Text $accOpen) + ' accordion-body(open)=' + (Rect-Text $bodyOpen) +
              ' notice=' + (Rect-Text $noticeA))
Write-Output ('  accordion body: idle=' + (Rect-Text $bodyIdle) + ' closed=' + (Rect-Text $bodyClosed))
Check ($hero -ne $null -and $hero.w -gt 0 -and $hero.h -gt 0) 'hero card has real geometry'
Check ($player -ne $null -and $player.w -gt 0 -and $player.h -gt 0) 'player card has real geometry'
Check ($player -ne $null -and $player.w -eq 300) ('player card is the fixed 300 px column (got ' + $(if ($player) { $player.w } else { '-' }) + ')')
Check ($hero -ne $null -and $player -ne $null -and $player.x -eq ($hero.x + $hero.w + 16)) 'player column starts one 16 px gap right of the hero card'
Check ($hero -ne $null -and $hero.x -eq 24) 'page margin on the left is 24 px (hero.x)'
Check ($accOpen -ne $null -and $accOpen.h -gt 0) 'accordion has real geometry while open'
Check ($accOpen -ne $null -and $hero -ne $null -and $accOpen.y -eq ($hero.y + $hero.h + 16)) 'accordion sits one 16 px gap below the hero card'
Check ($noticeA -eq $null) 'run A (no notice content): notice card is NOT in the dump'

# ---- task primitive criterion lines
Write-Output '  task criterion lines (work thread id must differ from the gui thread id):'
foreach ($l in @($runA.lines | Select-String -Pattern '\[sxcl-ui2\] task\[' | Select-Object -First 8)) { Write-Output ('    ' + $l.Line.Trim()) }
$taskIds = @{}
foreach ($l in $runA.lines) {
  if ($l -match 'task\[([^\]]+)\]: kind=(\w+) work_thread_id=0x([0-9a-f]+) gui_thread_id=0x([0-9a-f]+) same_thread=(\w+)') {
    $taskIds[$Matches[1]] = @{ kind = $Matches[2]; work = $Matches[3]; gui = $Matches[4]; same = $Matches[5] }
  }
}
Check ($taskIds.ContainsKey('home-instances') -and $taskIds.ContainsKey('home-account')) 'both home tasks printed the thread criterion line'
Check ($taskIds.ContainsKey('home-instances') -and $taskIds['home-instances'].same -eq 'no' -and
       $taskIds['home-instances'].work -ne $taskIds['home-instances'].gui) 'home-instances: work thread id != gui thread id (same_thread=no)'
Check ($taskIds.ContainsKey('home-account') -and $taskIds['home-account'].same -eq 'no' -and
       $taskIds['home-account'].work -ne $taskIds['home-account'].gui) 'home-account: work thread id != gui thread id (same_thread=no)'
$doneOnGui = @{}
foreach ($l in $runA.lines) {
  if ($l -match 'task\[([^\]]+)\]: done_on_gui_thread=(\w+)') { $doneOnGui[$Matches[1]] = $Matches[2] }
}
Check ($doneOnGui.ContainsKey('home-instances') -and $doneOnGui['home-instances'] -eq 'yes') 'home-instances: done callback ran ON the gui thread'
Check ($doneOnGui.ContainsKey('home-account') -and $doneOnGui['home-account'] -eq 'yes') 'home-account: done callback ran ON the gui thread'
Check (@($runA.lines | Select-String -Pattern 'MAIN-THREAD-VIOLATION').Count -eq 0) 'no MAIN-THREAD-VIOLATION line at runtime'

# ---- hover (three action icons)
Write-Output 'hover hook (SXCL_UI2_HOVER_NAME=1): Enter/Leave on #sxcl2NameRow'
$actionNames = @('sxcl2NameActionSkin', 'sxcl2NameActionAccount', 'sxcl2NameActionRename')
$hoverOk = $true
$leaveOk = $true
$sizes = @()
foreach ($n in $actionNames) {
  $rIdle = Get-Rect $stA 'idle' $n
  $rHover = Get-Rect $stA 'hover' $n
  $rLeave = Get-Rect $stA 'leave' $n
  Write-Output ('  ' + $n + ': idle=' + (Rect-Text $rIdle) + ' hover=' + (Rect-Text $rHover) + ' leave=' + (Rect-Text $rLeave))
  if ($rHover -eq $null -or $rHover.hidden -or $rHover.w -ne 32 -or $rHover.h -ne 32) { $hoverOk = $false }
  if ($rLeave -eq $null -or -not $rLeave.hidden) { $leaveOk = $false }
  if ($rIdle -eq $null -or -not $rIdle.hidden) { $hoverOk = $false }
  if ($rHover -ne $null) { $sizes += ($rHover.w.ToString() + 'x' + $rHover.h.ToString()) }
}
Check $hoverOk 'all three action icons are hidden before and VISIBLE at 32x32 while hovering'
Check $leaveOk 'all three action icons are hidden again after Leave'
$rowIdle = Get-Rect $stA 'idle' 'sxcl2NameRow'
$rowHover = Get-Rect $stA 'hover' 'sxcl2NameRow'
$labelIdle = Get-Rect $stA 'idle' 'sxcl2PlayerName'
$labelHover = Get-Rect $stA 'hover' 'sxcl2PlayerName'
Write-Output ('  name row: idle=' + (Rect-Text $rowIdle) + ' hover=' + (Rect-Text $rowHover))
Write-Output ('  name label: idle=' + (Rect-Text $labelIdle) + ' hover=' + (Rect-Text $labelHover) +
              ' (text "' + $stA['idle'].lines.Count + ' lines dumped)')
Check ($rowIdle -ne $null -and $rowHover -ne $null -and $rowIdle.x -eq $rowHover.x -and $rowIdle.y -eq $rowHover.y -and
       $rowIdle.w -eq $rowHover.w -and $rowIdle.h -eq $rowHover.h) 'showing the icons does not move the name row (they take no space when hidden)'
Check ($labelIdle -ne $null -and $labelHover -ne $null -and $labelIdle.x -eq $labelHover.x -and $labelIdle.w -eq $labelHover.w) 'showing the icons does not move the player name label'

# ---- accordion
Write-Output 'accordion hook (SXCL_UI2_CLICK_VERSION=1): swap version expands IN PLACE'
$rowsClosed = @(Get-Rects $stA 'accordion-closed' 'sxcl2VersionRow')
$rowsOpen = @(Get-Rects $stA 'accordion-open' 'sxcl2VersionRow')
$rowsAfterClose = @(Get-Rects $stA 'accordion-close' 'sxcl2VersionRow')
Write-Output ('  body height: closed=' + $(if ($bodyClosed) { $bodyClosed.h } else { '-' }) +
              ' open=' + $(if ($bodyOpen) { $bodyOpen.h } else { '-' }) +
              ' after close=' + $(if ((Get-Rect $stA 'accordion-close' 'sxcl2VersionAccordionBody')) { (Get-Rect $stA 'accordion-close' 'sxcl2VersionAccordionBody').h } else { '-' }))
Write-Output ('  rows: closed=' + $rowsClosed.Count + ' open=' + $rowsOpen.Count + ' after close=' + $rowsAfterClose.Count)
$bodyAfterClose = Get-Rect $stA 'accordion-close' 'sxcl2VersionAccordionBody'
$expectedBody = 4 * 34 + 3 * 4
Check ($bodyClosed -ne $null -and $bodyClosed.h -eq 0) 'collapsed: list container height == 0'
Check ($bodyOpen -ne $null -and $bodyOpen.h -gt 0) 'expanded: list container height > 0'
Check ($bodyOpen -ne $null -and $bodyOpen.h -eq $expectedBody) ('expanded: list container height == 4*34+3*4 = ' + $expectedBody)
Check ($bodyAfterClose -ne $null -and $bodyAfterClose.h -eq 0) 'after the second click: list container height back to 0'
Check ($rowsOpen.Count -eq 4) ('expanded: 4 version rows (got ' + $rowsOpen.Count + ')')
$rowsVisible = $true
foreach ($r in $rowsOpen) { if ($r.hidden -or $r.h -ne 34) { $rowsVisible = $false } }
Check $rowsVisible 'expanded: every row is visible at 34 px'
$rowsHiddenClosed = $true
foreach ($r in $rowsClosed) { if (-not $r.hidden) { $rowsHiddenClosed = $false } }
Check ($rowsClosed.Count -eq 4 -and $rowsHiddenClosed) 'collapsed: the four rows are hidden'
Check ((Get-Route $stA 'accordion-closed') -eq 'home' -and (Get-Route $stA 'accordion-open') -eq 'home' -and
       (Get-Route $stA 'accordion-close') -eq 'home') 'no route change while expanding/collapsing (route stays "home")'
$stagger = @($runA.lines | Select-String -Pattern 'delays=\[([0-9,]+)\]' | Select-Object -First 1)
$staggerOk = $false
if ($stagger.Count -gt 0) {
  $null = ($stagger[0].Line -match 'delays=\[([0-9,]+)\]')
  $delays = $Matches[1].Split(',')
  $staggerOk = ($delays.Count -eq 4 -and $delays[0] -eq '0' -and $delays[1] -eq '25' -and $delays[2] -eq '50' -and $delays[3] -eq '75')
  Write-Output ('  stagger delays line: ' + $stagger[0].Line.Trim())
}
Check $staggerOk 'stagger is a 25 ms/row delay table (delays=[0,25,50,75])'

# ================================================================ runs B/C/D: edition toggle
function Invoke-EditionRun([string]$tag, [string]$pick) {
  if ($pick -eq '') { return Invoke-Ui2Run $tag @{} (Join-Path $work ($tag + '.png')) 4500 }
  return Invoke-Ui2Run $tag @{ SXCL_UI2_EDITION = $pick } (Join-Path $work ($tag + '.png')) 4500
}
function Get-Dpr([string]$png) {
  $bmp = New-Object System.Drawing.Bitmap($png)
  $dpr = [math]::Round($bmp.Width / 1100.0, 2)
  $bmp.Dispose()
  return $dpr
}
function Invoke-EditionCheck([string]$tag, [string]$expectEdition, [string]$expectHighlight) {
  $png = Join-Path $work ($tag + '.png')
  $states = Get-States $run
  $stateName = 'edition-' + $expectEdition
  if ($expectEdition -eq '') { $stateName = 'idle' }
  if (-not $states.ContainsKey($stateName)) {
    Fail ($tag + ': dump state ' + $stateName + ' missing')
    return
  }
  $prem = Get-Rect $states $stateName 'sxcl2EditionPremium'
  $offl = Get-Rect $states $stateName 'sxcl2EditionOffline'
  $card = Get-Rect $states $stateName 'sxcl2PlayerCard'
  Write-Output ('  [' + $tag + '] premium=' + (Rect-Text $prem) + ' offline=' + (Rect-Text $offl))
  if ($prem -eq $null -or $offl -eq $null) { Fail ($tag + ': edition buttons missing in the dump'); return }
  Check ($prem.w -eq 32 -and $prem.h -eq 32 -and $offl.w -eq 32 -and $offl.h -eq 32) ($tag + ': both edition icons are 32x32')
  Check ($offl.x -eq ($prem.x + $prem.w + 8)) ($tag + ': the two icons are 8 px apart (gap = ' + ($offl.x - ($prem.x + $prem.w)) + ')')
  Check ($card -ne $null -and $prem.x -ge $card.x -and ($offl.x + $offl.w) -le ($card.x + $card.w)) ($tag + ': the toggle group sits inside the player card')
  if (-not (Test-Path $png)) { Fail ($tag + ': screenshot missing'); return }
  $dpr = Get-Dpr $png
  $cardRef = Get-CardRef $png $card $dpr
  $sPrem = Get-IconStats $png $prem $dpr 4 $cardRef
  $sOffl = Get-IconStats $png $offl $dpr 4 $cardRef
  Write-Output ('  [' + $tag + '] dpr=' + $dpr + ' card_bg=' + (Get-Hex $cardRef) +
                ' premium: ringAccent=' + $sPrem.accent + '/' + $sPrem.bandTotal + ' ink=' + $sPrem.ink +
                ' | offline: ringAccent=' + $sOffl.accent + '/' + $sOffl.bandTotal + ' ink=' + $sOffl.ink)
  Check ($sPrem.ink -gt 40 -and $sOffl.ink -gt 40) ($tag + ': both icons really rendered (ink pixels)')
  if ($expectHighlight -eq 'premium') {
    Check ($sPrem.accent -ge 100) ($tag + ': the CURRENT state (premium) carries accent in its ring (' + $sPrem.accent + ' px)')
    Check ($sOffl.accent -eq 0) ($tag + ': the other one (offline) carries exactly 0 accent pixels (' + $sOffl.accent + ')')
  } else {
    Check ($sOffl.accent -ge 100) ($tag + ': the CURRENT state (offline) carries accent in its ring (' + $sOffl.accent + ' px)')
    Check ($sPrem.accent -eq 0) ($tag + ': the other one (premium) carries exactly 0 accent pixels (' + $sPrem.accent + ')')
  }
}

Write-Output 'runs B/C/D: click premium -> highlight moves + game.edition lands on disk; click offline; then restart with no click'
Write-Ini 'offline'
$runB = Invoke-EditionRun 'runB' 'premium'
Check ($runB.exit -eq 0) 'run B exit code 0'
Check ((Get-IniEdition) -eq 'premium') ('run B: settings file says game.edition=premium (got ' + (Get-IniEdition) + ')')
$run = $runB
Invoke-EditionCheck 'runB' 'premium' 'premium'
$runC = Invoke-EditionRun 'runC' 'offline'
Check ($runC.exit -eq 0) 'run C exit code 0'
Check ((Get-IniEdition) -eq 'offline') ('run C: settings file says game.edition=offline (got ' + (Get-IniEdition) + ')')
$run = $runC
Invoke-EditionCheck 'runC' 'offline' 'offline'
# run D: no click at all -- the saved value must come back by itself
Write-Ini 'premium'
Write-Output ('  settings before run D: game.edition=' + (Get-IniEdition))
$runD = Invoke-EditionRun 'runD' ''
Check ($runD.exit -eq 0) 'run D exit code 0'
$run = $runD
Invoke-EditionCheck 'runD' '' 'premium'
Check ((Get-Violations (Get-States $runD)) -eq 0) 'run D: violations = 0'
$stD = Get-States $runD
$noticeD = Get-Rect $stD 'idle' 'sxcl2NoticeCard'
Check ($noticeD -eq $null) 'run D (no notice content): notice card is NOT in the dump'

# ================================================================ runs E/F: notice card
Write-Output 'runs E: notice card appears ONLY when there is content'
$noticeText = '2026-09-27 ' + [char]0x66F4 + [char]0x65B0 + ' | ' + [char]0x670D + [char]0x52A1 + [char]0x5668 + ' ok'
$runE = Invoke-Ui2Run 'runE' @{ SXCL_UI2_NOTICE = $noticeText } (Join-Path $work 'runE.png') 3000
Check ($runE.exit -eq 0) 'run E exit code 0'
$stE = Get-States $runE
$noticeE = Get-Rect $stE 'idle' 'sxcl2NoticeCard'
$noticeBody = Get-Rect $stE 'idle' 'sxcl2NoticeBody'
Write-Output ('  notice card (with content): ' + (Rect-Text $noticeE) + ' body=' + (Rect-Text $noticeBody))
Check ($noticeE -ne $null -and $noticeE.h -gt 0) 'with content: the notice card is present with height > 0'
Check ($noticeBody -ne $null -and $noticeBody.h -gt 0) 'with content: the notice body has height > 0'
Check (-not ($runE.lines -match $sNoNotice)) 'the forbidden "no announcement" filler text appears nowhere (run E)'
Check (-not ($runA.lines -match $sNoNotice)) 'the forbidden "no announcement" filler text appears nowhere (run A)'

# ================================================================ run G: route liveness
Write-Output 'run G: "show all versions" really does switch the route (so run A is not vacuously true)'
$runG = Invoke-Ui2Run 'runG' @{ SXCL_UI2_CLICK_VERSION = 1; SXCL_UI2_CLICK_ALL = 1 } (Join-Path $work 'runG.png') 3500
Check ($runG.exit -eq 0) 'run G exit code 0'
$stG = Get-States $runG
Write-Output ('  route before=' + (Get-Route $stG 'accordion-open') + ' after the click=' + (Get-Route $stG 'all'))
Check ((Get-Route $stG 'accordion-open') -eq 'home') 'run G: route is home before the click'
Check ((Get-Route $stG 'all') -eq 'versions') 'run G: clicking "show all versions" switched the route to "versions"'

# ================================================================ run H: 12 s zero-hang
Write-Output 'run H: 12 s zero-hang run (accordion + edition clicks in the middle)'
Write-Ini 'offline'
$runH = Invoke-Ui2Run 'runH' @{ SXCL_UI2_CLICK_VERSION = 1; SXCL_UI2_EDITION = 'premium' } (Join-Path $work 'runH.png') 12000
$stH = Get-States $runH
Write-Output ('  exit=' + $runH.exit + ' ms=' + $runH.ms + ' hangs=' + $runH.hangs.Count + ' stall_max=' + $runH.stall)
Check ($runH.exit -eq 0) 'run H exit code 0'
Check ($runH.ms -ge 12000) ('run H lived >= 12 s (ms=' + $runH.ms + ')')
Check ($runH.hangs.Count -eq 0) ('run H left 0 new sxcl-ui-hang-* reports (' + $runH.hangs.Count + ')')
Check ($runH.stall -lt 3000) ('run H: no UI stall >= 3000 ms (max ' + $runH.stall + ' ms)')
Check ((Get-Violations $stH) -eq 0) 'run H: violations = 0'
$shotH = Join-Path $work 'runH.png'
Check ((Test-Path $shotH) -and ((Get-Item $shotH).Length -gt 0)) 'run H wrote its screenshot after 12 s'
$taskIdsH = @()
foreach ($l in $runH.lines) { if ($l -match 'task\[[^\]]+\]: kind=\w+ work_thread_id=0x[0-9a-f]+ gui_thread_id=0x[0-9a-f]+ same_thread=no') { $taskIdsH += 1 } }
Check ($taskIdsH.Count -ge 2) ('run H: both tasks reported same_thread=no (' + $taskIdsH.Count + ' lines)')

# ================================================================ verdict
Write-Output ('screenshots: ' + ((Get-ChildItem $work -Filter '*.png' | ForEach-Object { $_.Name }) -join ', '))
Write-Output ('dumps: ' + ((Get-ChildItem $work -Filter '*.err.txt' | ForEach-Object { $_.Name }) -join ', '))
if ($fail -eq 0) {
  Write-Output ('UI2-HOME: ALL OK (blocks=4 hover_icons=3 accordion_rows=4 edition_runs=3 notice=2 runs=8 checks=' + $checks + ' hangs=0 stall_max=' + $runH.stall + 'ms)')
  exit 0
}
Write-Output ('UI2-HOME: FAIL (' + $fail + ' of ' + $checks + ' checks failed)')
exit 1
