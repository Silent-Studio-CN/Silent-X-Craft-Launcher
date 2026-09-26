# (C) Silent X Craft Launcher -- rail geometry + rail state machine assertions (docs/27 S12).
# ASCII-only on purpose: Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM,
# so Chinese here would break the script (keep this file ASCII + UTF-8 BOM).
$ErrorActionPreference = 'Continue'
$exe = Join-Path $PSScriptRoot '..\build-ui\src\ui\Release\sxcl-ui.exe'
# acceptance/temp files live under D:\SilentStudio\_test (never %TEMP% / C:)
$work = 'D:\SilentStudio\_test\rail_assert'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$fail = 0

# ------------------------------------------------------------------ 1) geometry
# side2 (NavPanel #sxclVersionFolderNav) must be flush with the page (ScrollArea
# #sxclPage_select): dx = nav.x - page.x and dy = nav.y - page.y must BOTH be 0.
# Measured on the real exe at three window sizes (dpr comes from the machine).
$iniPath = Join-Path $work 'rail_assert.ini'
Set-Content -Path $iniPath -Value ('game.default_dir=' + $work + '\.minecraft') -Encoding utf8
foreach ($size in @('900x600','1100x750','1280x800')) {
  $env:SXCL_UI_WINDOW = $size
  $env:SXCL_UI_ROUTE = 'select'
  $env:SXCL_UI_SETTINGS = $iniPath
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_SHOT = (Join-Path $work ('rail_' + $size + '.png'))
  $env:SXCL_UI_SHOT_DELAY = '5000'
  Remove-Item Env:SXCL_UI_RAILS_TEST -ErrorAction SilentlyContinue
  $out = Join-Path $work ('dump_' + $size + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $txt = @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) + @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  $page = ($txt | Select-String -Pattern 'sxclPage_select \((\d+),(\d+) (\d+)x(\d+)\)' | Select-Object -First 1)
  $nav = ($txt | Select-String -Pattern 'sxclVersionFolderNav \((\d+),(\d+) (\d+)x(\d+)\)' | Select-Object -First 1)
  if ((-not $page) -or (-not $nav)) { Write-Output ('  [' + $size + '] dump lines not found -> FAIL'); $fail++; continue }
  $pageLine = $page.Line -replace '\s+',' '
  $navLine = $nav.Line -replace '\s+',' '
  $null = ($page.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
  $px = [int]$Matches[1]; $py = [int]$Matches[2]
  $null = ($nav.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
  $nx = [int]$Matches[1]; $ny = [int]$Matches[2]
  $dx = $nx - $px; $dy = $ny - $py
  $ok = ($dx -eq 0) -and ($dy -eq 0)
  Write-Output ('  [' + $size + '] page=' + $pageLine + ' | sub=' + $navLine + ' | dx=' + $dx + ' dy=' + $dy + ' -> ' + $(if ($ok) { 'OK' } else { 'FAIL' }))
  if (-not $ok) { $fail++ }
}

# ------------------------------------------------- 2) rail state machine (real run)
# SXCL_UI_RAILS_TEST drives the product path through seven steps:
#   1 back to version-select (+ ensure side2)   5 expand side2 again on select
#   2 expand the main rail                      6 switch to download (side2 is hidden now)
#   3 switch to download (page rail is born     7 switch back to select
#     expanded -> the shell must make the
#     main rail give way)
#   4 switch back to select
# Step 1 normalizes the start page, so all three configurations follow one timeline
# (SXCL_UI_ROUTE=download starts on a page whose own rail is born expanded as well).
# Every step waits for the animations to settle and prints:
#   [rails] step=<n> expanded=<count> nav=<w> sub=<w> name=<objectName> route=<route> who=<names>
# expanded counts rails VISIBLE to the window that are not collapsed (same rule as the
# shell): never > 1 on any step. Widths: expanded 322 / collapsed 48 (NavPanel).
# The interesting readings (all measured, see docs/27 S12):
#   step3 expanded=1 nav=48 sub=322 (before the fix this step was 2: two rails at 322)
#   step6 expanded=1 nav=48 sub=322 (the hidden side2 at 322 must NOT be counted)
$railIni = Join-Path $work 'rail_rails.ini'
Set-Content -Path $railIni -Value ('game.default_dir=' + $work + '\.minecraft') -Encoding utf8
# A second game folder: the select page REBUILDS its side rail when the folder changes
# (the state machine must follow the new NavPanel, not the deleted one).
$fixture = Join-Path $work 'mc'
New-Item -ItemType Directory -Force -Path (Join-Path $fixture 'mcA\versions'), (Join-Path $fixture 'mcB\versions') | Out-Null
$rebuildIni = Join-Path $work 'rail_rebuild.ini'
Set-Content -Path $rebuildIni -Value ('game.default_dir=' + $fixture + '\mcB') -Encoding utf8
Add-Content -Path $rebuildIni -Value ('game.known_dirs=' + $fixture + '\mcA')
$rebuildKey = 'folder:' + ($fixture + '/mcA').Replace('\', '/')

function Test-RailsRun([string]$tag, [string]$route, [string]$ini, [string]$nav, [switch]$expectRebuild) {
  $env:SXCL_UI_WINDOW = '1100x750'
  $env:SXCL_UI_ROUTE = $route
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_RAILS_TEST = '1'
  Remove-Item Env:SXCL_UI_DUMP -ErrorAction SilentlyContinue
  Remove-Item Env:SXCL_UI_SHOT -ErrorAction SilentlyContinue
  if ($nav) { $env:SXCL_UI_NAV = $nav } else { Remove-Item Env:SXCL_UI_NAV -ErrorAction SilentlyContinue }
  $out = Join-Path $work ('rails_' + $tag + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $txt = @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) + @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  Write-Output ('  [rails/' + $tag + '] route=' + $route + ' nav=' + $(if ($nav) { $nav } else { '-' }))
  $steps = @()
  $byStep = @{}
  $over = 0
  foreach ($line in $txt) {
    if ($line -notmatch '\[rails\] step=(\d+) expanded=(\d+) nav=(-?\d+) sub=(-?\d+) name=(\S+) route=(\S+)') { continue }
    $st = [int]$Matches[1]; $exp = [int]$Matches[2]; $nw = [int]$Matches[3]; $sw = [int]$Matches[4]
    $nm = $Matches[5]; $rt = $Matches[6]
    Write-Output ('    ' + ($line -replace '\s+',' '))
    $steps += $st
    $byStep[$st] = @($exp, $nw, $sw, $nm, $rt)
    if ($exp -gt 1) { $over++ }
  }
  $bad = @()
  if ($steps.Count -ne 8) { $bad += ('expected 8 steps (0..7), got ' + $steps.Count) }
  if ($over -gt 0) { $bad += ($over.ToString() + ' step(s) with expanded > 1') }
  # step1: normalized to the version-select page, side2 expanded
  if ($byStep.ContainsKey(1) -and $byStep[1][4] -ne 'select') { $bad += ('step1 route=' + $byStep[1][4] + ' (step 1 must normalize back to select)') }
  if ($byStep.ContainsKey(1) -and $byStep[1][2] -ne 322) { $bad += ('step1 sub=' + $byStep[1][2] + ' (expanded side2 must be 322)') }
  # step2: main rail expanded, side2 collapsed by the state machine
  if ($byStep.ContainsKey(2) -and $byStep[2][1] -ne 322) { $bad += ('step2 nav=' + $byStep[2][1] + ' (expanded main rail must be 322)') }
  if ($byStep.ContainsKey(2) -and $byStep[2][2] -ne 48) { $bad += ('step2 sub=' + $byStep[2][2] + ' (state machine must collapse side2 to 48)') }
  # step3: switched to the download page while the main rail was expanded at 322 ->
  # "the page rail wins": the shell must collapse the main rail (this is the new coverage)
  if ($byStep.ContainsKey(3) -and $byStep[3][0] -ne 1) { $bad += ('step3 expanded=' + $byStep[3][0] + ' (page rail wins: exactly 1 visible expanded rail)') }
  if ($byStep.ContainsKey(3) -and $byStep[3][1] -ne 48) { $bad += ('step3 nav=' + $byStep[3][1] + ' (main rail must give way to the page rail -> 48)') }
  if ($byStep.ContainsKey(3) -and $byStep[3][2] -ne 322) { $bad += ('step3 sub=' + $byStep[3][2] + ' (download page rail must stay expanded -> 322)') }
  if ($byStep.ContainsKey(3) -and $byStep[3][4] -ne 'download') { $bad += ('step3 route=' + $byStep[3][4] + ' (must be download)') }
  if ($byStep.ContainsKey(3) -and $byStep[3][3] -ne 'sxclDownloadNav') { $bad += ('step3 name=' + $byStep[3][3] + ' (must be the download page rail)') }
  # step4: back on select; side2 was collapsed at step2 and nothing auto-expands -> 0 visible
  if ($byStep.ContainsKey(4) -and $byStep[4][0] -ne 0) { $bad += ('step4 expanded=' + $byStep[4][0] + ' (both rails are collapsed by then -> 0)') }
  if ($byStep.ContainsKey(4) -and ($byStep[4][1] -ne 48 -or $byStep[4][2] -ne 48)) { $bad += ('step4 nav/sub=' + $byStep[4][1] + '/' + $byStep[4][2] + ' (both must be 48)') }
  if ($byStep.ContainsKey(4) -and $byStep[4][4] -ne 'select') { $bad += ('step4 route=' + $byStep[4][4] + ' (must be select)') }
  # step5: side2 expanded again on the select page
  if ($byStep.ContainsKey(5) -and $byStep[5][2] -ne 322) { $bad += ('step5 sub=' + $byStep[5][2] + ' (side2 expanded again must be 322)') }
  # step6: on download again; side2 is hidden (still 322) and must not be counted
  if ($byStep.ContainsKey(6) -and $byStep[6][0] -ne 1) { $bad += ('step6 expanded=' + $byStep[6][0] + ' (hidden side2 must not be counted -> exactly 1)') }
  if ($byStep.ContainsKey(6) -and $byStep[6][2] -ne 322) { $bad += ('step6 sub=' + $byStep[6][2] + ' (download page rail must be 322)') }
  if ($byStep.ContainsKey(6) -and $byStep[6][3] -ne 'sxclDownloadNav') { $bad += ('step6 name=' + $byStep[6][3] + ' (must be the download page rail)') }
  # step7: back on select, that side2 (hidden the whole time since step5) is still the only one
  if ($byStep.ContainsKey(7) -and $byStep[7][0] -ne 1) { $bad += ('step7 expanded=' + $byStep[7][0] + ' (exactly 1)') }
  if ($byStep.ContainsKey(7) -and $byStep[7][2] -ne 322) { $bad += ('step7 sub=' + $byStep[7][2] + ' (select side2 must still be 322)') }
  if ($byStep.ContainsKey(7) -and $byStep[7][4] -ne 'select') { $bad += ('step7 route=' + $byStep[7][4] + ' (must be select)') }
  if ($expectRebuild) {
    # the folder switch really happened: the page writes the new dir back to the settings file
    $hit = @(Get-Content $rebuildIni -Encoding utf8 -ErrorAction SilentlyContinue | Select-String -Pattern 'game.default_dir=.*mcA')
    if ($hit.Count -lt 1) { $bad += 'folder switch did not happen (game.default_dir still not mcA)' }
  }
  if ($bad.Count -gt 0) {
    foreach ($b in $bad) { Write-Output ('    -> FAIL ' + $b) }
    $fail++
    return
  }
  Write-Output ('    -> OK steps=' + $steps.Count + ' step3 nav/sub=' + $byStep[3][1] + '/' + $byStep[3][2] + ' step7 nav/sub=' + $byStep[7][1] + '/' + $byStep[7][2])
}

Test-RailsRun 'select' 'select' $railIni ''
Test-RailsRun 'download' 'download' $railIni ''
Test-RailsRun 'select_rebuild' 'select' $rebuildIni $rebuildKey -expectRebuild

if ($fail -eq 0) { Write-Output 'RAIL-GEOMETRY: ALL OK' } else { Write-Output ('RAIL-GEOMETRY: ' + $fail + ' FAILED'); exit 1 }
