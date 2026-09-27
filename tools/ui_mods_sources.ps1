# (C) Silent X Craft Launcher -- "mods sources" acceptance (user 2026-09-26).
#
# What the user asked for, verbatim:
#   "mod download: the two circles must be CHECK OPTIONS, not 'search a mod and pick from the two
#    below it' -- copy the idea; about the CurseForge key, I will give it to you later, do NOT let
#    the user type it in."
#
# So this script proves, per run (every number is measured here, nothing is guessed):
#
# TIER 1 -- the default build (no CurseForge key compiled in):
#   1) both sources are real CHECK BOXES in the widget tree, not a single-choice pivot:
#      #modsSourceModrinth / #modsSourceCurseForge; the dump prints their real checked state
#      (main.cpp prints " checked" / " unchecked" for checkable buttons -- not our own claim)
#   2) with no key compiled in, CurseForge is DISABLED (visible but un-clickable) and Modrinth
#      can be ticked; the product hook SXCL_UI_MODS_SOURCES clicks the REAL widgets
#   3) there is NO key input anywhere: no #modsCfKeyEdit widget and no "API key" wording in the dump
#   4) the search still runs, every result row carries its OWN source tag (#modsSourceTag), and the
#      page says which sources it requested / which one it skipped and why (mods-sources trace)
#   5) the ticked set is persisted as a list in the settings file (mods.source) and survives a restart
#
# TIER 2 -- a temporary build with a DUMMY key, to prove the "both sources" path really fires:
#   6) with a key compiled in, CurseForge becomes checkable and BOTH boxes are checked at once
#   7) both sources are really requested (one mods-fetch trace line per source) and the merged
#      result line lists both sources with their own counts -- a source with no data stays 0,
#      nothing is faked from the other source
#   The build is restored (key removed) at the end, even if an assertion fails.
#
# That the merged LIST can hold rows of BOTH sources at once (two blocks, each row keeping its own
# source, never merged across sources) is proved without network by the unit test
# tests/mods_sources_test.cpp (ctest name: sxcl_mods_sources_test).
#
# ASCII-only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM).
$ErrorActionPreference = 'Continue'

foreach ($v in @('SXCL_UI_ROUTE','SXCL_UI_NAV','SXCL_UI_THEME','SXCL_UI_DUMP','SXCL_UI_DUMP_DEPTH',
                 'SXCL_UI_MODS_QUERY','SXCL_UI_MODS_SOURCES','SXCL_UI_MODS_SOURCES_DELAY',
                 'SXCL_UI_MODS_INSTALL','SXCL_UI_SETTINGS','SXCL_UI_GAME_DIR','SXCL_UI_WINDOW',
                 'SXCL_UI_SHOT','SXCL_UI_SHOT_DELAY','SXCL_UI2')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe  = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\sxcl_mods_sources'
$fail = 0
$total = 0
# Chinese fragments are built from code points: this file is ASCII-only on purpose
# (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM).
function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }
function Ok([bool]$cond, [string]$what) {
  $script:total++
  if ($cond) { Write-Output ('  [ok]   ' + $what) }
  else { Write-Output ('  [FAIL] ' + $what); $script:fail++ }
}

if (-not (Test-Path $exe)) {
  Write-Output ('MODS-SOURCES: FAIL (exe not found: ' + $exe + ')')
  exit 1
}
$busy = @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
  Write-Output ('MODS-SOURCES: FAIL (another sxcl-ui.exe is running, pid ' + ($busy.Id -join ',') + ')')
  exit 1
}

# ---- fixture: one installed instance so the page has a context line, and its own settings file ----
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $work 'mc\versions\acceptance') | Out-Null
$j = @{ id='acceptance'; mainClass='net.minecraft.client.main.Main'; type='release'; clientVersion='1.20.1'; libraries=@() }
Set-Content -Path (Join-Path $work 'mc\versions\acceptance\acceptance.json') -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8
$ini = Join-Path $work 'mods.ini'

function Reset-Ini {
  Set-Content -Path $ini -Value ('game.default_dir=' + (Join-Path $work 'mc')) -Encoding utf8
  Add-Content -Path $ini -Value 'game.selected_version=acceptance'
}

function Invoke-Mods([string]$tag, [string]$sources, [string]$query, [int]$waitMs) {
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = (Join-Path $work 'mc')
  $env:SXCL_UI_ROUTE = 'download'
  $env:SXCL_UI_NAV = 'download_mod'
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_WINDOW = '1100x900'
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '16'
  $env:SXCL_UI_MODS_SOURCES_DELAY = '400'
  # the tick hook has to finish before the search hook fires (it waits for the widgets to be born)
  $env:SXCL_UI_MODS_QUERY_DELAY = '2500'
  $env:SXCL_UI_SHOT_DELAY = [string]$waitMs
  $shot = Join-Path $work ('shot_' + $tag + '.png')
  Remove-Item $shot -ErrorAction SilentlyContinue
  $env:SXCL_UI_SHOT = $shot
  if ($sources -ne '') { $env:SXCL_UI_MODS_SOURCES = $sources }
  else { Remove-Item Env:SXCL_UI_MODS_SOURCES -ErrorAction SilentlyContinue }
  if ($query -ne '') { $env:SXCL_UI_MODS_QUERY = $query }
  else { Remove-Item Env:SXCL_UI_MODS_QUERY -ErrorAction SilentlyContinue }
  $out = Join-Path $work ('run_' + $tag + '.txt')
  $err = $out + '.err'
  Remove-Item $out,$err -ErrorAction SilentlyContinue
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  return @{
    lines = @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) + @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
    shot  = $shot
    tag   = $tag
  }
}

function Get-One($lines, [string]$pattern) {
  foreach ($line in $lines) { if ($line -match $pattern) { return $Matches } }
  return $null
}
# NOTE: PowerShell unrolls a 1-element array on return, so Get-All must return ",$hits";
# otherwise the caller gets the hashtable itself and .Count becomes its key count (a real bug
# this script hit: 1 fetch was reported as 2).
function Get-All($lines, [string]$pattern) {
  $hits = @()
  foreach ($line in $lines) { if ($line -match $pattern) { $hits += ,$Matches } }
  return ,$hits
}
function Get-Last($lines, [string]$pattern) {
  $found = $null
  foreach ($line in $lines) { if ($line -match $pattern) { $found = $Matches } }
  return $found
}
# The dump prints BOTH mods panes (mod + shader): one of them is always " hidden".
# Every widget assertion must look at the pane the user is actually looking at.
function Find-Visible($lines, [string]$pattern) {
  foreach ($line in $lines) {
    if ($line -match ' hidden') { continue }
    if ($line -match $pattern) { return $line }
  }
  return ''
}
function Get-StoredValue([string]$key) {
  foreach ($line in (Get-Content $ini -Encoding utf8 -ErrorAction SilentlyContinue)) {
    if ($line -match ('^' + [regex]::Escape($key) + '=(.*)$')) { return $Matches[1].Trim() }
  }
  return $null
}

# =====================================================================================
# TIER 1, run 1: no key compiled in; tick Modrinth only, run a real search
# =====================================================================================
Write-Output ''
Write-Output '== tier 1 / run 1: default build (no CurseForge key), tick modrinth, search =='
Remove-Item $ini -ErrorAction SilentlyContinue
Reset-Ini
$r1 = Invoke-Mods 't1a' 'modrinth' 'jei' 11000
$lines1 = $r1.lines

$pick = Get-One $lines1 'mods-source-pick: want=(\S+) modrinth=(\S+) curseforge=(\S+)'
Ok ($pick -ne $null) 'the source picking hook ran (it clicks the real check boxes)'
if ($pick -ne $null) {
  Write-Output ('  trace: want=' + $pick[1] + ' modrinth=' + $pick[2] + ' curseforge=' + $pick[3])
  Ok ($pick[2] -eq 'checked') 'modrinth ticked through the real widget'
  Ok ($pick[3] -eq 'disabled') 'with no key compiled in, curseforge is visible but disabled (cannot be ticked)'
}

$boxM = @($lines1 | Where-Object { $_ -match '#modsSourceModrinth \(' -and $_ -notmatch ' hidden' })
$boxC = @($lines1 | Where-Object { $_ -match '#modsSourceCurseForge \(' -and $_ -notmatch ' hidden' })
Ok ($boxM.Count -ge 1 -and $boxC.Count -ge 1) ('both source check boxes are in the widget tree (modrinth=' + $boxM.Count + ' curseforge=' + $boxC.Count + ')')
$mLine = Find-Visible $lines1 '#modsSourceModrinth \('
$cLine = Find-Visible $lines1 '#modsSourceCurseForge \('
Write-Output ('  dump modrinth  : ' + $mLine.Trim())
Write-Output ('  dump curseforge: ' + $cLine.Trim())
Ok ($mLine -match ' checked') 'dump says modrinth is checked (real state, not our own claim)'
Ok ($cLine -match ' disabled') 'dump says curseforge is disabled'
Ok ($cLine -notmatch ' checked') 'dump says curseforge is NOT checked'

$keyWidgets = @($lines1 | Where-Object { $_ -match '#modsCfKeyEdit' })
Ok ($keyWidgets.Count -eq 0) ('there is no CurseForge key input widget anywhere (' + $keyWidgets.Count + ' found)')
$keyText = @($lines1 | Where-Object { $_ -match 'API [Kk]ey' -or $_ -match 'x-api-key' })
Ok ($keyText.Count -eq 0) ('the dump never asks the user for a key (' + $keyText.Count + ' lines mention one)')

$src = Get-Last $lines1 'mods-sources: picked=(\S*) searchable=(\S*) skipped=(\S*)'
Ok ($src -ne $null) 'the page reports which sources it picked / searches / skips'
if ($src -ne $null) {
  Write-Output ('  trace: picked=' + $src[1] + ' searchable=' + $src[2] + ' skipped=' + $src[3])
  Ok ($src[1] -eq 'modrinth') 'picked set is the ticked one (modrinth)'
  Ok ($src[3] -like 'curseforge:*') 'the skipped source is named together with the reason'
}
$fetches = Get-All $lines1 'mods-fetch: source=(\S+)'
Ok ($fetches.Count -eq 1 -and $fetches[0][1] -eq 'modrinth') ('exactly one fetch, and it is modrinth (' + $fetches.Count + ' fetches)')

$merge = Get-One $lines1 'mods-merged: sources=(\S*) rows=(\d+) perSource="([^"]*)"'
Ok ($merge -ne $null) 'the page reports the merged result line'
if ($merge -ne $null) {
  Write-Output ('  trace: sources=' + $merge[1] + ' rows=' + $merge[2] + ' perSource="' + $merge[3] + '"')
  Ok ([int]$merge[2] -gt 0) ('the search really returned rows (' + $merge[2] + ')')
  Ok ($merge[3] -match '^Modrinth \d+$') 'per-source counts only list the source that was searched'
}
$tags = @($lines1 | Where-Object { $_ -match '#modsSourceTag \(' -and $_ -notmatch ' hidden' })
Ok ($tags.Count -ge 1) ('every result card carries its own source tag (' + $tags.Count + ' tags)')
$badTags = @($lines1 | Where-Object { $_ -match '#modsSourceTag \(' -and $_ -notmatch ' hidden' -and $_ -notmatch 'Modrinth' })
Ok ($badTags.Count -eq 0) ('no row claims a source it does not come from (' + $badTags.Count + ' mismatches)')

$stored = Get-StoredValue 'mods.source'
Write-Output ('  persisted mods.source = "' + $stored + '" (still unwritten: the default is not a user choice)')

# =====================================================================================
# TIER 1, run 2: untick everything through the real widget -> the EMPTY set is persisted,
#               and the search button goes dead (nothing to search)
# =====================================================================================
Write-Output ''
Write-Output '== tier 1 / run 2: hook wants no known source -> modrinth gets unticked, the empty set is persisted =='
$r2 = Invoke-Mods 't1b' 'none' '' 5000
$lines2 = $r2.lines
$pick2 = Get-One $lines2 'mods-source-pick: want=(\S+) modrinth=(\S+) curseforge=(\S+)'
Ok ($pick2 -ne $null) 'the hook ran on run 2'
if ($pick2 -ne $null) {
  Write-Output ('  trace: want=' + $pick2[1] + ' modrinth=' + $pick2[2] + ' curseforge=' + $pick2[3])
  Ok ($pick2[2] -eq 'unchecked') 'modrinth can be unticked through the real widget (a check box, not a pivot)'
}
$mLine2 = Find-Visible $lines2 '#modsSourceModrinth \('
Write-Output ('  dump modrinth  : ' + $mLine2.Trim())
Ok ($mLine2 -notmatch ' checked') 'the dump agrees: no source is ticked now'
$goLine2 = Find-Visible $lines2 '#modsSearchButton \('
Write-Output ('  dump search btn: ' + $goLine2.Trim())
Ok ($goLine2 -match ' disabled') 'with no source ticked the search button is dead (no "pick a source first" wording needed)'
$stored2 = Get-StoredValue 'mods.source'
Ok ($stored2 -eq '') ('the empty set is persisted as such (mods.source=, got "' + $stored2 + '")')

# =====================================================================================
# TIER 1, run 3: restart with the empty set on disk -> it must NOT fall back to the default
# =====================================================================================
Write-Output ''
Write-Output '== tier 1 / run 3: restart -> an explicitly empty set stays empty (no silent default) =='
$r3 = Invoke-Mods 't1c' '' '' 4000
$mLine3 = Find-Visible $r3.lines '#modsSourceModrinth \('
Write-Output ('  dump modrinth  : ' + $mLine3.Trim())
Ok ($mLine3 -notmatch ' checked') 'a written empty set reads back as "nothing ticked" (the default only applies when the key was never written)'

# =====================================================================================
# TIER 1, run 4: the SETTINGS page must not ask for a CurseForge key either
#               (the card went away together with the settings key it used to write)
# =====================================================================================
Write-Output ''
Write-Output '== tier 1 / run 4: settings page carries no CurseForge key card =='
$srcFile = Join-Path $root 'src\ui\pages\settings_page.cpp'
# NOTE: the settings key is matched **with its quotes** (a live reference is always a C string
# literal). The source file keeps a comment that explains WHY the card was deleted, and that
# comment names the old setting key on purpose -- matching the bare token would flag that comment.
$srcHits = @(Select-String -Path $srcFile -Pattern 'curseForgeKeyEdit|curseForgeKeySave|CurseForgeKeyCard|kKeyCfApiKey|"mods\.curseforge_api_key"' -ErrorAction SilentlyContinue)
Ok ($srcHits.Count -eq 0) ('settings_page.cpp has no CF key card / key constant left (' + $srcHits.Count + ' hits)')

$env:SXCL_UI_SETTINGS = $ini
$env:SXCL_UI_GAME_DIR = (Join-Path $work 'mc')
$env:SXCL_UI_ROUTE = 'settings'
$env:SXCL_UI_THEME = 'dark'
$env:SXCL_UI_WINDOW = '1100x750'
$env:SXCL_UI_DUMP = '1'
$env:SXCL_UI_DUMP_DEPTH = '18'
$env:SXCL_UI_SHOT = (Join-Path $work 'shot_settings.png')
$env:SXCL_UI_SHOT_DELAY = '4000'
Remove-Item Env:SXCL_UI_NAV -ErrorAction SilentlyContinue
Remove-Item Env:SXCL_UI_MODS_QUERY -ErrorAction SilentlyContinue
Remove-Item Env:SXCL_UI_MODS_SOURCES -ErrorAction SilentlyContinue
$outS = Join-Path $work 'run_settings.txt'
$errS = $outS + '.err'
Remove-Item $outS,$errS -ErrorAction SilentlyContinue
Start-Process -FilePath $exe -RedirectStandardOutput $outS -RedirectStandardError $errS -Wait | Out-Null
$linesS = @(Get-Content $outS -Encoding utf8 -ErrorAction SilentlyContinue) + @(Get-Content $errS -Encoding utf8 -ErrorAction SilentlyContinue)
Ok ((@($linesS | Where-Object { $_ -match 'ScrollArea #SettingsPage \(' }).Count) -ge 1) 'the settings page really rendered (the dump has #SettingsPage)'
$keyWidgetsS = @($linesS | Where-Object { $_ -match 'curseForgeKeyEdit|curseForgeKeySave' })
Ok ($keyWidgetsS.Count -eq 0) ('no key input / save widget anywhere on the settings page (' + $keyWidgetsS.Count + ' found)')
$keyWordsS = @($linesS | Where-Object { $_ -match 'API [Kk]ey|console\.curseforge\.com|' + (C @(0x7C98,0x8D34)) + ' key' })
Ok ($keyWordsS.Count -eq 0) ('nothing on the settings page asks the user for a key (' + $keyWordsS.Count + ' lines)')
$dlGroupS = @($linesS | Where-Object { $_ -match (C @(0x4E0B,0x8F7D,0x8BBE,0x7F6E)) })
Ok ($dlGroupS.Count -ge 1) 'the download settings group is still there (only the key card was removed)'

# =====================================================================================
# TIER 2: temporary build with a dummy key -> both sources at once
# =====================================================================================
Write-Output ''
Write-Output '== tier 2: temporary build with a dummy key -> both sources ticked and both requested =='
$build = Join-Path $root 'build-ui'
$dummy = 'sxcl-acceptance-dummy-key'
try {
  & cmake -S $root -B $build ('-DSXCL_CURSEFORGE_API_KEY=' + $dummy) 2>&1 | Out-Null
  & cmake --build $build --config Release --target sxcl-ui 2>&1 | Select-Object -Last 4 | ForEach-Object { Write-Output ('  build: ' + $_) }
  $r3 = Invoke-Mods 't2' 'modrinth,curseforge' 'jei' 14000
  $lines3 = $r3.lines
  $pick3 = Get-One $lines3 'mods-source-pick: want=(\S+) modrinth=(\S+) curseforge=(\S+)'
  Ok ($pick3 -ne $null) 'the hook ran on the keyed build'
  if ($pick3 -ne $null) {
    Write-Output ('  trace: want=' + $pick3[1] + ' modrinth=' + $pick3[2] + ' curseforge=' + $pick3[3])
    Ok ($pick3[2] -eq 'checked' -and $pick3[3] -eq 'checked') 'BOTH sources are ticked at the same time (check boxes, not a pivot)'
  }
  $src3 = Get-One $lines3 'mods-sources: picked=(\S*) searchable=(\S*) skipped=(\S*)'
  if ($src3 -ne $null) {
    Write-Output ('  trace: picked=' + $src3[1] + ' searchable=' + $src3[2] + ' skipped=' + $src3[3])
    Ok (($src3[1] -match 'modrinth') -and ($src3[1] -match 'curseforge')) 'both ticked sources are in the picked set'
    Ok ($src3[3] -eq 'none') 'nothing is skipped when a key is compiled in'
  } else { Ok $false 'the page reports picked / searchable / skipped on the keyed build' }
  $f3 = Get-All $lines3 'mods-fetch: source=(\S+)'
  $seen = @()
  foreach ($f in $f3) { $seen += $f[1] }
  Write-Output ('  fetches: ' + ($seen -join ', '))
  Ok (($seen -contains 'modrinth') -and ($seen -contains 'curseforge')) 'both sources are really requested (one fetch each)'
  $m3 = Get-Last $lines3 'mods-merged: sources=(\S*) rows=(\d+) perSource="([^"]*)"'
  if ($m3 -ne $null) {
    Write-Output ('  trace: sources=' + $m3[1] + ' rows=' + $m3[2] + ' perSource="' + $m3[3] + '"')
    Ok (($m3[3] -match 'Modrinth \d+') -and ($m3[3] -match 'CurseForge \d+')) 'the merged line lists both sources with their own counts'
  } else { Ok $false 'the merged result line names both sources' }
  $stored3 = Get-StoredValue 'mods.source'
  Write-Output ('  persisted mods.source = "' + $stored3 + '"')
  Ok ($stored3 -eq 'modrinth,curseforge') 'both ticked sources are persisted as a LIST (modrinth,curseforge)'
} finally {
  Write-Output '  restoring the build (removing the dummy key)'
  & cmake -S $root -B $build '-DSXCL_CURSEFORGE_API_KEY=' 2>&1 | Out-Null
  & cmake --build $build --config Release --target sxcl-ui 2>&1 | Select-Object -Last 2 | ForEach-Object { Write-Output ('  build: ' + $_) }
}
$r4 = Invoke-Mods 't1d' '' '' 5000
$cLine4 = Find-Visible $r4.lines '#modsSourceCurseForge \('
$mLine4 = Find-Visible $r4.lines '#modsSourceModrinth \('
Write-Output ('  dump modrinth  : ' + $mLine4.Trim())
Write-Output ('  dump curseforge: ' + $cLine4.Trim())
Ok ($cLine4 -match ' disabled') 'after restoring the build, curseforge is disabled again (no dummy key left behind)'
Ok ($cLine4 -notmatch ' checked') 'the persisted curseforge tick is dropped in a build that cannot search it'
Ok ($mLine4 -match ' checked') 'the other persisted tick (modrinth) is read back from mods.source'

Write-Output ''
Write-Output ('  product files: ' + $work)
if ($fail -eq 0) {
  Write-Output ('MODS-SOURCES: ALL OK (' + $total + '/' + $total + ')')
  exit 0
}
Write-Output ('MODS-SOURCES: FAIL (' + $fail + ' of ' + $total + ' assertions)')
exit 1
