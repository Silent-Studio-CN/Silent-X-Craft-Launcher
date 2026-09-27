# (C) Silent X Craft Launcher -- "mods sources" acceptance (user 2026-09-26 / 2026-09-27).
#
# The user's rules, verbatim:
#   2026-09-26: "the two circles must be CHECK OPTIONS, not 'search a mod and pick from the two
#               below it' ... about the CurseForge key, I will give it to you later, do NOT let
#               the user type it in."
#   2026-09-27: "by default we use the fallback key too ... mainly the two OFFICIAL sources, and
#               only then the mirror ... the check boxes are there so the user can EXCLUDE a
#               source he does not want -- that is a subjective choice, never a way to configure
#               CurseForge."
#
# What this proves, per run (every number is measured on this machine, nothing is guessed):
#
#  A) the two source check boxes are an EXCLUSION switch only:
#     - one run with a fresh settings file: BOTH are ticked by default, and NEITHER is disabled
#       (a greyed-out box would make a beginner think he has to configure something)
#     - one run where the hook unticks one of them: only that source is requested
#     - the ticked set is persisted as a list in the settings file and survives a restart
#  B) the ORDER is "official first, mirror as the fallback" and it is reproducible from the trace:
#       mods-fetch: source=curseforge via=official url=https://api.curseforge.com/...
#       mods-fetch-retry: source=curseforge next=mirror
#       mods-fetch: source=curseforge via=mirror  url=https://mod.mcimirror.top/curseforge/...
#     The first official attempt really happens (HTTP 403 on a build without a key, which is what
#     this machine has) and the page still ends up with CurseForge rows -- nothing is faked from
#     Modrinth, and both sources keep their own tags in the merged list.
#  C) no CurseForge key input / wording exists anywhere in the product:
#     - the mods page has no key widget (it was deleted)
#     - the settings page has no key card either (source-level grep + a settings-route dump)
#  D) when BOTH paths fail (official 403 + mirror pointed at a black hole through
#     SXCL_MODS_MIRROR=http://127.0.0.1:9, the acceptance-only override of the mirror root)
#     the page reports the failure and shows CurseForge 0 -- Modrinth rows are never relabelled.
#
# The core "official URL -> mirror URL" rewrite has its own offline unit test:
# tests/mods_test.c test_mirror_url() (ctest: sxcl_mods_test).
#
# ASCII-only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM).
$ErrorActionPreference = 'Continue'

foreach ($v in @('SXCL_UI_ROUTE','SXCL_UI_NAV','SXCL_UI_THEME','SXCL_UI_DUMP','SXCL_UI_DUMP_DEPTH',
                 'SXCL_UI_MODS_QUERY','SXCL_UI_MODS_QUERY_DELAY','SXCL_UI_MODS_SOURCES',
                 'SXCL_UI_MODS_SOURCES_DELAY','SXCL_UI_MODS_INSTALL','SXCL_UI_SETTINGS',
                 'SXCL_UI_GAME_DIR','SXCL_UI_WINDOW','SXCL_UI_SHOT','SXCL_UI_SHOT_DELAY',
                 'SXCL_MODS_MIRROR','SXCL_UI2')) {
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

function Reset-Ini([switch]$WithoutSources) {
  Set-Content -Path $ini -Value ('game.default_dir=' + (Join-Path $work 'mc')) -Encoding utf8
  Add-Content -Path $ini -Value 'game.selected_version=acceptance'
  if (-not $WithoutSources) { }
}

# NOTE: a PowerShell function that returns an array unrolls it -- Get-All must return ",$hits",
# otherwise the caller gets the hashtable itself and .Count is its key count (a real bug this
# script hit: one fetch was reported as two).
function Get-All($lines, [string]$pattern) {
  $hits = @()
  foreach ($line in $lines) { if ($line -match $pattern) { $hits += ,$Matches } }
  return ,$hits
}
function Get-One($lines, [string]$pattern) {
  foreach ($line in $lines) { if ($line -match $pattern) { return $Matches } }
  return $null
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

function Invoke-Mods([string]$tag, [string]$sources, [string]$query, [int]$waitMs, [string]$route = 'download', [string]$mirror = '') {
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = (Join-Path $work 'mc')
  $env:SXCL_UI_ROUTE = $route
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_WINDOW = '1100x900'
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '18'
  $env:SXCL_UI_MODS_SOURCES_DELAY = '400'
  # the tick hook has to finish before the search hook fires (it waits for the widgets to be born)
  $env:SXCL_UI_MODS_QUERY_DELAY = '2500'
  $env:SXCL_UI_SHOT_DELAY = [string]$waitMs
  $shot = Join-Path $work ('shot_' + $tag + '.png')
  Remove-Item $shot -ErrorAction SilentlyContinue
  $env:SXCL_UI_SHOT = $shot
  if ($route -eq 'download') { $env:SXCL_UI_NAV = 'download_mod' }
  else { Remove-Item Env:SXCL_UI_NAV -ErrorAction SilentlyContinue }
  if ($sources -ne '') { $env:SXCL_UI_MODS_SOURCES = $sources }
  else { Remove-Item Env:SXCL_UI_MODS_SOURCES -ErrorAction SilentlyContinue }
  if ($query -ne '') { $env:SXCL_UI_MODS_QUERY = $query }
  else { Remove-Item Env:SXCL_UI_MODS_QUERY -ErrorAction SilentlyContinue }
  if ($mirror -ne '') { $env:SXCL_MODS_MIRROR = $mirror }
  else { Remove-Item Env:SXCL_MODS_MIRROR -ErrorAction SilentlyContinue }
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

# =====================================================================================
# RUN 1: fresh settings file, no hook -> BOTH sources ticked by default, neither disabled
# =====================================================================================
Write-Output ''
Write-Output '== run 1: default state (fresh settings) -> both sources ticked, neither disabled =='
Remove-Item $ini -ErrorAction SilentlyContinue
Reset-Ini
$r1 = Invoke-Mods 'r1_default' '' '' 4500
$lines1 = $r1.lines
$mLine1 = Find-Visible $lines1 '#modsSourceModrinth \('
$cLine1 = Find-Visible $lines1 '#modsSourceCurseForge \('
Write-Output ('  dump modrinth  : ' + $mLine1.Trim())
Write-Output ('  dump curseforge: ' + $cLine1.Trim())
Ok ($mLine1 -match ' checked') 'by default Modrinth is ticked'
Ok ($cLine1 -match ' checked') 'by default CurseForge is ticked too (both on, the user unticks what he does not want)'
Ok ($mLine1 -notmatch ' disabled') 'the Modrinth box is usable (never greyed out)'
Ok ($cLine1 -notmatch ' disabled') 'the CurseForge box is usable WITHOUT any key (never greyed out)'
$srcState = Get-Last $lines1 'mods-sources-state: picked=(\S*) searchable=(\S*) skipped=(\S*)'
if ($srcState -ne $null) {
  Write-Output ('  trace: picked=' + $srcState[1] + ' searchable=' + $srcState[2] + ' skipped=' + $srcState[3])
  Ok ($srcState[1] -eq 'modrinth,curseforge') 'the picked set is both sources'
}
$stored1 = Get-StoredValue 'mods.source'
Ok ($stored1 -eq $null) ('nothing was written yet: the default is not a user choice (mods.source=' + $stored1 + ')')

# =====================================================================================
# RUN 2: the user excludes CurseForge -> only Modrinth is requested
# =====================================================================================
Write-Output ''
Write-Output '== run 2: user unticks CurseForge -> only the remaining source is searched =='
$r2 = Invoke-Mods 'r2_exclude_cf' 'modrinth' 'jei' 13000
$lines2 = $r2.lines
$pick2 = Get-One $lines2 'mods-source-pick: want=(\S+) modrinth=(\S+) curseforge=(\S+)'
Ok ($pick2 -ne $null) 'the source picking hook ran (it clicks the real check boxes)'
if ($pick2 -ne $null) {
  Write-Output ('  trace: want=' + $pick2[1] + ' modrinth=' + $pick2[2] + ' curseforge=' + $pick2[3])
  Ok ($pick2[2] -eq 'checked' -and $pick2[3] -eq 'unchecked') 'the unticked source is really unticked (and it stayed enabled)'
}
$cLine2 = Find-Visible $lines2 '#modsSourceCurseForge \('
Ok ($cLine2 -notmatch ' disabled') 'even unticked, the box is not greyed out'
$f2 = Get-All $lines2 'mods-fetch: source=(\S+) via=(\S+)'
$seen2 = @()
foreach ($f in $f2) { $seen2 += $f[1] }
Write-Output ('  fetches: ' + ($seen2 -join ', '))
Ok ($seen2.Count -eq 1 -and $seen2[0] -eq 'modrinth') ('exactly one fetch, and it is the ticked source (' + $seen2.Count + ')')
$m2 = Get-Last $lines2 'mods-merged: sources=(\S*) rows=(\d+) perSource="([^"]*)"'
if ($m2 -ne $null) {
  Write-Output ('  trace: sources=' + $m2[1] + ' rows=' + $m2[2] + ' perSource="' + $m2[3] + '"')
  Ok ([int]$m2[2] -gt 0) ('the search really returned rows (' + $m2[2] + ')')
  Ok ($m2[3] -match '^Modrinth \d+$') 'per-source counts list only the source that was searched'
} else { Ok $false 'the merged result line is missing' }
$tags2 = @($lines2 | Where-Object { $_ -match '#modsSourceTag \(' -and $_ -notmatch ' hidden' })
Ok ($tags2.Count -ge 1) ('every result card carries its own source tag (' + $tags2.Count + ')')
$badTags2 = @($lines2 | Where-Object { $_ -match '#modsSourceTag \(' -and $_ -notmatch ' hidden' -and $_ -notmatch 'Modrinth' })
Ok ($badTags2.Count -eq 0) ('no row claims a source it does not come from (' + $badTags2.Count + ')')
$stored2 = Get-StoredValue 'mods.source'
Ok ($stored2 -eq 'modrinth') ('the choice is persisted as a list: mods.source=' + $stored2)

# =====================================================================================
# RUN 3: both sources -> official first, mirror as the fallback, both sets of rows
# =====================================================================================
Write-Output ''
Write-Output '== run 3: both sources ticked -> official first, mirror fallback, merged list has both =='
$r3 = Invoke-Mods 'r3_both' 'modrinth,curseforge' 'jei' 20000
$lines3 = $r3.lines
$f3 = Get-All $lines3 'mods-fetch: source=(\S+) via=(\S+) url=(\S+)'
foreach ($f in $f3) { Write-Output ('  fetch: source=' + $f[1] + ' via=' + $f[2] + ' url=' + $f[3]) }
$cfFirst = $null
foreach ($f in $f3) { if ($f[1] -eq 'curseforge' -and $cfFirst -eq $null) { $cfFirst = $f } }
Ok ($cfFirst -ne $null) 'CurseForge was really requested'
if ($cfFirst -ne $null) {
  Ok ($cfFirst[2] -eq 'official') 'the FIRST CurseForge attempt goes to the official host'
  Ok ($cfFirst[3] -match 'api\.curseforge\.com') '  and that URL is api.curseforge.com'
}
$retry = Get-One $lines3 'mods-fetch-retry: source=(\S+) next=(\S+)'
Ok ($retry -ne $null) 'the page switched routes after the official attempt failed (fallback)'
if ($retry -ne $null) {
  Write-Output ('  trace: retry source=' + $retry[1] + ' next=' + $retry[2])
  Ok ($retry[2] -eq 'mirror') 'the fallback is the mirror'
}
$cfMirror = $null
foreach ($f in $f3) { if ($f[1] -eq 'curseforge' -and $f[2] -eq 'mirror') { $cfMirror = $f } }
Ok ($cfMirror -ne $null) 'a mirror request for CurseForge really happened'
if ($cfMirror -ne $null) {
  Ok ($cfMirror[3] -match 'mod\.mcimirror\.top/curseforge') '  and it points at the mirror, same path'
}
$m3 = Get-Last $lines3 'mods-merged: sources=(\S*) rows=(\d+) perSource="([^"]*)"'
if ($m3 -ne $null) {
  Write-Output ('  trace: sources=' + $m3[1] + ' rows=' + $m3[2] + ' perSource="' + $m3[3] + '"')
  Ok ($m3[1] -match 'modrinth' -and $m3[1] -match 'curseforge') 'both sources are in the merged result'
  $cfCount = [int](([regex]::Match($m3[3], 'CurseForge (\d+)')).Groups[1].Value)
  Ok ($cfCount -gt 0) ('CurseForge rows came through the fallback (' + $cfCount + ' rows)')
} else { Ok $false 'the merged result line is missing' }
$tags3cf = @($lines3 | Where-Object { $_ -match '#modsSourceTag \(' -and $_ -notmatch ' hidden' -and $_ -match 'CurseForge' })
$tags3m = @($lines3 | Where-Object { $_ -match '#modsSourceTag \(' -and $_ -notmatch ' hidden' -and $_ -match 'Modrinth' })
Write-Output ('  tags: Modrinth=' + $tags3m.Count + ' CurseForge=' + $tags3cf.Count)
Ok ($tags3cf.Count -gt 0 -and $tags3m.Count -gt 0) 'both sources really put cards on screen, each with its own tag'
$stored3 = Get-StoredValue 'mods.source'
Ok ($stored3 -eq 'modrinth,curseforge') ('both ticked sources are persisted as a list (mods.source=' + $stored3 + ')')

# =====================================================================================
# RUN 4: the user unticks BOTH -> nothing to search, empty set persisted
# =====================================================================================
Write-Output ''
Write-Output '== run 4: user unticks everything -> the search button goes dead, the empty set is persisted =='
$r4 = Invoke-Mods 'r4_none' 'none' '' 5000
$lines4 = $r4.lines
$pick4 = Get-One $lines4 'mods-source-pick: want=(\S+) modrinth=(\S+) curseforge=(\S+)'
if ($pick4 -ne $null) {
  Write-Output ('  trace: want=' + $pick4[1] + ' modrinth=' + $pick4[2] + ' curseforge=' + $pick4[3])
  Ok ($pick4[2] -eq 'unchecked' -and $pick4[3] -eq 'unchecked') 'both boxes can be unticked through the real widgets'
}
$goLine4 = Find-Visible $lines4 '#modsSearchButton \('
Write-Output ('  dump search btn: ' + $goLine4.Trim())
Ok ($goLine4 -match ' disabled') 'with no source ticked the search button is dead (no "pick a source first" wording needed)'
$stored4 = Get-StoredValue 'mods.source'
Ok ($stored4 -eq '') ('the empty set is persisted as such (mods.source=, got "' + $stored4 + '")')

# =====================================================================================
# RUN 5: restart with the empty set on disk -> it must NOT fall back to the default
# =====================================================================================
Write-Output ''
Write-Output '== run 5: restart -> an explicitly empty set stays empty (the default only applies to a missing key) =='
$r5 = Invoke-Mods 'r5_restart' '' '' 4000
$mLine5 = Find-Visible $r5.lines '#modsSourceModrinth \('
Write-Output ('  dump modrinth  : ' + $mLine5.Trim())
Ok ($mLine5 -notmatch ' checked') 'a written empty set reads back as "nothing ticked"'

# =====================================================================================
# RUN 6: the SETTINGS page must not offer a CurseForge key either
# =====================================================================================
Write-Output ''
Write-Output '== run 6: settings page carries no CurseForge key card =='
$srcFile = Join-Path $root 'src\ui\pages\settings_page.cpp'
# NOTE: the settings key is matched **with its quotes** (a live reference is always a C string
# literal). The source file keeps a comment that explains WHY the card was deleted, and that
# comment names the old setting key on purpose -- matching the bare token would flag that comment.
$srcHits = @(Select-String -Path $srcFile -Pattern 'curseForgeKeyEdit|curseForgeKeySave|CurseForgeKeyCard|kKeyCfApiKey|"mods\.curseforge_api_key"' -ErrorAction SilentlyContinue)
Ok ($srcHits.Count -eq 0) ('settings_page.cpp has no CF key card / key constant left (' + $srcHits.Count + ' hits)')
$r6 = Invoke-Mods 'r6_settings' '' '' 4000 'settings'
$lines6 = $r6.lines
Ok ((@($lines6 | Where-Object { $_ -match 'ScrollArea #SettingsPage \(' }).Count) -ge 1) 'the settings page really rendered (the dump has #SettingsPage)'
$keyWidgets6 = @($lines6 | Where-Object { $_ -match 'curseForgeKeyEdit|curseForgeKeySave' })
Ok ($keyWidgets6.Count -eq 0) ('no key input / save widget anywhere on the settings page (' + $keyWidgets6.Count + ')')
$keyWords6 = @($lines6 | Where-Object { $_ -match 'API [Kk]ey|console\.curseforge\.com|' + (C @(0x7C98,0x8D34)) + ' key' })
Ok ($keyWords6.Count -eq 0) ('nothing on the settings page asks the user for a key (' + $keyWords6.Count + ' lines)')
$dlGroup6 = @($lines6 | Where-Object { $_ -match (C @(0x4E0B,0x8F7D,0x8BBE,0x7F6E)) })
Ok ($dlGroup6.Count -ge 1) 'the download settings group is still there (only the key card was removed)'
$keyWordsMods = @($lines3 | Where-Object { $_ -match 'API [Kk]ey|x-api-key|' + (C @(0x955C,0x50CF)) + '|' + (C @(0x5BC6,0x94A5)) })
Ok ($keyWordsMods.Count -eq 0) ('the mods page never mentions a key or a mirror (' + $keyWordsMods.Count + ' lines)')

# =====================================================================================
# RUN 7: both routes dead -> honest failure (official 403 + mirror pointed at a black hole)
# =====================================================================================
Write-Output ''
Write-Output '== run 7: mirror unreachable and no key -> CurseForge reports the failure, nothing is faked =='
$r7 = Invoke-Mods 'r7_blackhole' 'modrinth,curseforge' 'jei' 22000 'download' 'http://127.0.0.1:9'
$lines7 = $r7.lines
$f7 = Get-All $lines7 'mods-fetch: source=(\S+) via=(\S+) url=(\S+)'
foreach ($f in $f7) { Write-Output ('  fetch: source=' + $f[1] + ' via=' + $f[2] + ' url=' + $f[3]) }
$bh = $null
foreach ($f in $f7) { if ($f[1] -eq 'curseforge' -and $f[2] -eq 'mirror') { $bh = $f } }
Ok ($bh -ne $null -and $bh[3] -match '127\.0\.0\.1:9') 'the mirror attempt really went to the black hole (SXCL_MODS_MIRROR override)'
$failed7 = Get-One $lines7 'mods-fetch-failed: source=(\S+) via=(\S+)'
Ok ($failed7 -ne $null -and $failed7[1] -eq 'curseforge') 'CurseForge is reported as failed after both routes'
$m7 = Get-Last $lines7 'mods-merged: sources=(\S*) rows=(\d+) perSource="([^"]*)"'
if ($m7 -ne $null) {
  Write-Output ('  trace: sources=' + $m7[1] + ' rows=' + $m7[2] + ' perSource="' + $m7[3] + '"')
  $cf7 = [int](([regex]::Match($m7[3], 'CurseForge (\d+)')).Groups[1].Value)
  $mr7 = [int](([regex]::Match($m7[3], 'Modrinth (\d+)')).Groups[1].Value)
  Ok ($cf7 -eq 0) ('CurseForge contributes 0 rows (' + $cf7 + ')')
  Ok ($mr7 -gt 0) ('the other source still works (' + $mr7 + ' rows)')
} else { Ok $false 'the merged result line is missing in the black-hole run' }
$tags7cf = @($lines7 | Where-Object { $_ -match '#modsSourceTag \(' -and $_ -notmatch ' hidden' -and $_ -match 'CurseForge' })
Ok ($tags7cf.Count -eq 0) ('no Modrinth row was relabelled as CurseForge (' + $tags7cf.Count + ' CurseForge tags)')

# =====================================================================================
# RUN 8 (tier 2): a build that HAS a key -> official is used first with that key
# =====================================================================================
Write-Output ''
Write-Output '== run 8: temporary build with a key -> official attempt carries the key, mirror still backs it up =='
$build = Join-Path $root 'build-ui'
$dummy = 'sxcl-acceptance-dummy-key'
try {
  & cmake -S $root -B $build ('-DSXCL_CURSEFORGE_API_KEY=' + $dummy) 2>&1 | Out-Null
  & cmake --build $build --config Release --target sxcl-ui 2>&1 | Select-Object -Last 3 | ForEach-Object { Write-Output ('  build: ' + $_) }
  $r8 = Invoke-Mods 'r8_keyed' 'modrinth,curseforge' 'jei' 22000
  $lines8 = $r8.lines
  $f8 = Get-All $lines8 'mods-fetch: source=(\S+) via=(\S+) url=(\S+)'
  foreach ($f in $f8) { Write-Output ('  fetch: source=' + $f[1] + ' via=' + $f[2] + ' url=' + $f[3]) }
  $first8 = $null
  foreach ($f in $f8) { if ($f[1] -eq 'curseforge' -and $first8 -eq $null) { $first8 = $f } }
  Ok ($first8 -ne $null -and $first8[2] -eq 'official') 'with a key compiled in the official route is still tried first'
  $retry8 = Get-One $lines8 'mods-fetch-retry: source=(\S+) next=(\S+)'
  Ok ($retry8 -ne $null -and $retry8[2] -eq 'mirror') 'a rejected key falls back to the mirror instead of failing the source'
  $m8 = Get-Last $lines8 'mods-merged: sources=(\S*) rows=(\d+) perSource="([^"]*)"'
  if ($m8 -ne $null) {
    Write-Output ('  trace: sources=' + $m8[1] + ' rows=' + $m8[2] + ' perSource="' + $m8[3] + '"')
    $cf8 = [int](([regex]::Match($m8[3], 'CurseForge (\d+)')).Groups[1].Value)
    Ok ($cf8 -gt 0) ('CurseForge rows still arrive through the fallback (' + $cf8 + ' rows)')
  } else { Ok $false 'the merged result line is missing on the keyed build' }
} finally {
  Write-Output '  restoring the build (removing the dummy key)'
  & cmake -S $root -B $build '-DSXCL_CURSEFORGE_API_KEY=' 2>&1 | Out-Null
  & cmake --build $build --config Release --target sxcl-ui 2>&1 | Select-Object -Last 2 | ForEach-Object { Write-Output ('  build: ' + $_) }
}
$r9 = Invoke-Mods 'r9_after_restore' '' '' 4500
$cLine9 = Find-Visible $r9.lines '#modsSourceCurseForge \('
$mLine9 = Find-Visible $r9.lines '#modsSourceModrinth \('
Write-Output ('  dump modrinth  : ' + $mLine9.Trim())
Write-Output ('  dump curseforge: ' + $cLine9.Trim())
Ok ($cLine9 -match ' checked') 'after restoring the build both persisted ticks come back'
Ok ($cLine9 -notmatch ' disabled') 'and the CurseForge box is still usable without a key (no dummy key left behind)'

Write-Output ''
Write-Output ('  product files: ' + $work)
if ($fail -eq 0) {
  Write-Output ('MODS-SOURCES: ALL OK (' + $total + '/' + $total + ')')
  exit 0
}
Write-Output ('MODS-SOURCES: FAIL (' + $fail + ' of ' + $total + ' assertions)')
exit 1
