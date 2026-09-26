# (C) Silent X Craft Launcher -- download page "one container only" acceptance (user 2026-09-26).
# User quote (2026-09-26, translated): "the menu on the right must not be nested twice;
# nesting twice squeezes it down to 1/4 of the size".
#
# What it asserts (dump based, SXCL_UI_DUMP=1 + SXCL_UI_DUMP_DEPTH=12, dark theme, three sizes):
#   (1) the version box (#versionList) has NO ScrollArea ancestor between it and the page shell
#       -> the download page keeps exactly ONE scroll container (#sxclPage_download / PageShell)
#   (2) the version box fills the right content area:  box.width >= stack.width - 40
#       and box.width >= stack.width - 2*28 - 40   (28 = the page margin token used by PageShell)
#   (3) vertical scrolling inside the page still works: the list's own vertical scrollbar is
#       VISIBLE (Qt only shows an AsNeeded bar when the range is > 0), while the page shell's
#       own SmoothScrollBars stay hidden (nothing left to drag: the content now fits exactly).
#
# Reference note: the "viewport" used for the width rule is the RIGHT CONTENT AREA (the
# QStackedWidget the box lives in), not the whole page viewport -- the page keeps its 322px
# second-level rail on the left (download page = two-level sidebar, by design), so the box can
# never be wider than pageViewport - rail - margin.  The raw numbers of all three references are
# printed for every size so any other reading can be checked from the same output.
$ErrorActionPreference = 'Continue'

# hermetic: clear every SXCL_UI_* hook this script does not set itself.  A leftover from another
# acceptance run in the same shell (e.g. SXCL_UI_CONFIG opens the config page and the rail state
# machine then leaves the second-level rail collapsed) would change the measured geometry.
foreach ($v in @('SXCL_UI_CONFIG', 'SXCL_UI_NAV', 'SXCL_UI_COLLAPSE', 'SXCL_UI_RAILS_TEST',
                 'SXCL_UI_EDITION', 'SXCL_UI_SCROLL', 'SXCL_UI_POPUP', 'SXCL_UI_AUTH_DIALOG',
                 'SXCL_UI_JRE_HOSTED', 'SXCL_UI_MODS_QUERY', 'SXCL_UI_LAUNCH', 'SXCL_UI_DOWNLOAD',
                 'SXCL_UI_ICON_POPUP', 'SXCL_UI_SCROLLBAR_STRESS', 'SXCL_UI_ACCENT_APPLY',
                 'SXCL_UI_THEME_SWITCH', 'SXCL_UI_TRACE', 'SXCL_UI_ACCEPT', 'SXCL_UI_SETTINGS_ACCEPT')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe  = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\sxcl_tasks\download_nest'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$ini  = Join-Path $work 'dl.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $work + '\.minecraft') -Encoding utf8

$cases = 0
$bad = @()
foreach ($size in @('900x600', '1100x750', '1280x800')) {
  $cases++
  $env:SXCL_UI_WINDOW = $size
  $env:SXCL_UI_ROUTE = 'download'
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '12'
  $env:SXCL_UI_SHOT = (Join-Path $work ('shot_' + $size + '.png'))
  $env:SXCL_UI_SHOT_DELAY = '4000'
  $out = Join-Path $work ('dump_' + $size + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $txt = @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)

  $iPage = -1; $iList = -1; $iStack = -1; $iVersions = -1
  for ($i = 0; $i -lt $txt.Count; $i++) {
    if ($iPage -lt 0 -and $txt[$i] -match 'ScrollArea #sxclPage_download \(') { $iPage = $i }
    if ($iStack -lt 0 -and $txt[$i] -match '^\s+QStackedWidget \(') { $iStack = $i }
    if ($iVersions -lt 0 -and $txt[$i] -match '#VersionsPage \(') { $iVersions = $i }
    if ($iList -lt 0 -and $txt[$i] -match 'QListView #versionList \(') { $iList = $i }
  }
  if ($iPage -lt 0 -or $iList -lt 0 -or $iStack -lt 0 -or $iVersions -lt 0) {
    $bad += ('[' + $size + '] page / stack / VersionsPage / #versionList line missing')
    continue
  }
  $null = ($txt[$iStack] -match '\((\d+),(\d+) (\d+)x(\d+)\)'); $sw = [int]$Matches[3]; $sh = [int]$Matches[4]
  $null = ($txt[$iList] -match '\((\d+),(\d+) (\d+)x(\d+)\)'); $bx = [int]$Matches[1]; $by = [int]$Matches[2]; $bw = [int]$Matches[3]; $bh = [int]$Matches[4]
  $null = ($txt[$iPage] -match '\((\d+),(\d+) (\d+)x(\d+)\)'); $px = [int]$Matches[1]; $pw = [int]$Matches[3]

  # (1) no ScrollArea between the list and the shell
  $indent = $txt[$iList].Length - $txt[$iList].TrimStart().Length
  $nested = @()
  for ($i = $iList - 1; $i -ge 0; $i--) {
    $li = $txt[$i].Length - $txt[$i].TrimStart().Length
    if ($li -ge $indent) { continue }
    $indent = $li
    if ($txt[$i] -match 'ScrollArea ' -and $txt[$i] -notmatch '#sxclPage_download') { $nested += $txt[$i].Trim() }
    if ($txt[$i] -match '#sxclPage_download') { break }
  }
  $versionsNode = ($txt[$iVersions].Trim() -split '\(')[0].Trim()
  if ($nested.Count -ne 0) { $bad += ('[' + $size + '] (1) nested scroll container(s): ' + ($nested -join ' | ')) }
  if ($versionsNode -notmatch '^QWidget') { $bad += ('[' + $size + '] (1) version page node is "' + $versionsNode + '" (want a plain QWidget)') }

  # (2) the box fills the right content area
  $limit = $sw - 2 * 28 - 40
  if ($bw -lt ($sw - 40)) { $bad += ('[' + $size + '] (2) box width ' + $bw + ' < stack width - 40 = ' + ($sw - 40)) }
  if ($bw -lt $limit) { $bad += ('[' + $size + '] (2) box width ' + $bw + ' < stack width - 2*28 - 40 = ' + $limit) }
  if ($bh -lt ($sh - 140)) { $bad += ('[' + $size + '] (2) box height ' + $bh + ' < stack height - 140 = ' + ($sh - 140)) }

  # (3) the list still scrolls itself; the shell has nothing to drag
  $listBarVisible = $false
  for ($i = $iList; $i -lt [Math]::Min($iList + 8, $txt.Count); $i++) {
    if ($txt[$i] -match 'qt_scrollarea_vcontainer' -and $i + 1 -lt $txt.Count -and $txt[$i + 1] -match 'QScrollBar \(') {
      if ($txt[$i + 1] -notmatch 'hidden') { $listBarVisible = $true }
      break
    }
  }
  if (-not $listBarVisible) { $bad += ('[' + $size + '] (3) version list has no visible vertical scrollbar (cannot scroll)') }
  $shellBars = @($txt | Select-String -Pattern '^  SmoothScrollBar \(' | Where-Object { $_.Line -notmatch 'hidden' })
  if ($shellBars.Count -ne 0) { $bad += ('[' + $size + '] (3) shell scrollbar still visible: ' + $shellBars[0].Line.Trim()) }

  Write-Output ('  [' + $size + '] pageViewport=' + $pw + ' rightStack=' + $sw + 'x' + $sh + ' box=' + $bw + 'x' + $bh + ' (box x=' + $bx + ' y=' + $by + ') listScrollBar=visible shellScrollBar=hidden nestedScrollAreas=' + $nested.Count)
}

if ($bad.Count -eq 0) {
  Write-Output ('DOWNLOAD-NEST: ALL OK (' + $cases + '/3 sizes: one container only -- #versionList fills the right content area, page shell keeps the single scroll layer, the version list itself still scrolls)')
  exit 0
}
foreach ($b in $bad) { Write-Output ('  FAIL ' + $b) }
Write-Output ('DOWNLOAD-NEST: FAIL (' + $bad.Count + ' assertion(s) over ' + $cases + ' cases)')
exit 1
