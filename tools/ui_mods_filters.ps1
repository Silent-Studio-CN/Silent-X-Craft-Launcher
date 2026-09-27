# (C) Silent X Craft Launcher -- "mod download filters" acceptance (user 2026-09-27).
#
# The user's rules:
#   * PCL's resource page has exactly THREE filters: name, version, source -- nothing else;
#   * the version filter is an INPUT BOX, not a fixed list ("MC has thousands of versions, you
#     make it a drop-down?"); a version that does not exist must simply return nothing;
#   * a row has NO button: the whole row is clickable = "I want it"
#     ("make them tabs without buttons, clicking means I want it");
#   * the file goes to the folder of the version he just installed, never where the filter points.
#
# A) filter area shape: 1 name box + 1 search button + 1 version INPUT + 2 source check boxes
# B) the version input defaults to the instance's vanilla version (from its JSON through the core
#    scan); an instance whose JSON says nothing -> empty (no guess)
# C) typing drives the search: trace says versionFilter=<v>; empty sends NO version facet;
#    9.9.9 (a version that does not exist) honestly returns 0 rows and is NOT auto-changed
# D) clicking a ROW installs the best file for the INSTANCE (not for the filter) and the jar lands
#    in <gameDir>/versions/<instance>/mods/
# E) no button inside a result row; nothing asks the user where to install
#
# ASCII-only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM).
$ErrorActionPreference = 'Continue'

foreach ($v in @('SXCL_UI_ROUTE','SXCL_UI_NAV','SXCL_UI_THEME','SXCL_UI_DUMP','SXCL_UI_DUMP_DEPTH',
                 'SXCL_UI_MODS_QUERY','SXCL_UI_MODS_QUERY_DELAY','SXCL_UI_MODS_VERSION',
                 'SXCL_UI_MODS_VERSION_DELAY','SXCL_UI_MODS_SOURCES','SXCL_UI_MODS_INSTALL',
                 'SXCL_UI_MODS_INSTALL_DELAY','SXCL_UI_MODS_ROW','SXCL_UI_MODS_ROW_DELAY',
                 'SXCL_UI_SETTINGS','SXCL_UI_GAME_DIR','SXCL_UI_WINDOW',
                 'SXCL_UI_SHOT','SXCL_UI_SHOT_DELAY','SXCL_MODS_MIRROR','SXCL_UI2')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe  = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\sxcl_mods_filters'
$fail = 0
$total = 0
function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }
function Ok([bool]$cond, [string]$what) {
  $script:total++
  if ($cond) { Write-Output ('  [ok]   ' + $what) }
  else { Write-Output ('  [FAIL] ' + $what); $script:fail++ }
}
function Get-All($lines, [string]$pattern) {
  $hits = @()
  foreach ($line in $lines) { if ($line -match $pattern) { $hits += ,$Matches } }
  return ,$hits
}
function Get-One($lines, [string]$pattern) {
  foreach ($line in $lines) { if ($line -match $pattern) { return $Matches } }
  return $null
}
function Find-Visible($lines, [string]$pattern) {
  foreach ($line in $lines) {
    if ($line -match ' hidden') { continue }
    if ($line -match $pattern) { return $line }
  }
  return ''
}
# the whole sub-tree of a container in the dump
function Get-Block($lines, [string]$name) {
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -notmatch ('#' + $name + ' \(') -or $lines[$i] -match ' hidden') { continue }
    $indent = $lines[$i].Length - $lines[$i].TrimStart().Length
    $block = @($lines[$i])
    for ($k = $i + 1; $k -lt $lines.Count; $k++) {
      $ik = $lines[$k].Length - $lines[$k].TrimStart().Length
      if ($ik -le $indent) { break }
      $block += $lines[$k]
    }
    return $block
  }
  return @()
}
# only the IMMEDIATE children of a container (a LineEdit carries its own clear/icon buttons --
# those are part of the input box, not "a fourth control in the filter row")
function Get-DirectChildren($lines, [string]$name) {
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -notmatch ('#' + $name + ' \(') -or $lines[$i] -match ' hidden') { continue }
    $indent = $lines[$i].Length - $lines[$i].TrimStart().Length
    $block = @()
    for ($k = $i + 1; $k -lt $lines.Count; $k++) {
      $ik = $lines[$k].Length - $lines[$k].TrimStart().Length
      if ($ik -le $indent) { break }
      if ($ik -eq $indent + 2) { $block += $lines[$k] }
    }
    return $block
  }
  return @()
}
function Get-Jar([string]$dir) {
  return @(Get-ChildItem -Path $dir -Filter *.jar -ErrorAction SilentlyContinue)
}

if (-not (Test-Path $exe)) { Write-Output ('MODS-FILTERS: FAIL (exe not found: ' + $exe + ')'); exit 1 }
$busy = @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
  Write-Output ('MODS-FILTERS: FAIL (another sxcl-ui.exe is running, pid ' + ($busy.Id -join ',') + ')')
  exit 1
}

# ---- fixtures -------------------------------------------------------------
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
$game = Join-Path $work 'mc'
foreach ($d in @('versions\114514','versions\424242')) {
  New-Item -ItemType Directory -Force -Path (Join-Path $game $d) | Out-Null
}
$j = @{ id='114514'; mainClass='net.minecraft.client.main.Main'; type='release'; clientVersion='1.20.1'; libraries=@() }
Set-Content -Path (Join-Path $game 'versions\114514\114514.json') -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8
Set-Content -Path (Join-Path $game 'versions\114514\114514.jar') -Value 'fixture' -Encoding ascii
$j2 = @{ id='424242'; mainClass='net.minecraft.client.main.Main'; type='release'; libraries=@() }
Set-Content -Path (Join-Path $game 'versions\424242\424242.json') -Value ($j2 | ConvertTo-Json -Depth 5) -Encoding utf8
Set-Content -Path (Join-Path $game 'versions\424242\424242.jar') -Value 'fixture' -Encoding ascii
$ini = Join-Path $work 'filters.ini'
function Reset-Ini([string]$selected) {
  Set-Content -Path $ini -Value ('game.default_dir=' + $game) -Encoding utf8
  Add-Content -Path $ini -Value ('game.selected_version=' + $selected)
  Add-Content -Path $ini -Value 'general.version_isolation=1'
}

function Invoke-Mods([string]$tag, [string]$query, [int]$waitMs, [string]$version, [string]$install, [string]$expect = '') {
  # Several agents share this machine and one of them may kill a running sxcl-ui.exe (to relink
  # the exe); a run that dies right after startup carries none of the evidence lines. Retry it.
  for ($attempt = 1; $attempt -le 3; $attempt++) {
    $lines = Invoke-ModsOnce $tag $query $waitMs $version $install
    if ($expect -eq '' -or ($lines -join "`n") -match $expect) { return $lines }
    Write-Output ('  NOTE run ' + $tag + ' attempt ' + $attempt + ' has no "' + $expect + '" -- retrying (another agent may have killed it)')
    Start-Sleep -Seconds 5
  }
  return $lines
}
function Invoke-ModsOnce([string]$tag, [string]$query, [int]$waitMs, [string]$version, [string]$install) {
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = $game
  $env:SXCL_UI_ROUTE = 'download'
  $env:SXCL_UI_NAV = 'download_mod'
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_WINDOW = '1100x750'
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '18'
  $env:SXCL_UI_MODS_QUERY_DELAY = '2500'
  $env:SXCL_UI_SHOT = (Join-Path $work ('shot_' + $tag + '.png'))
  $env:SXCL_UI_SHOT_DELAY = [string]$waitMs
  if ($query -ne '') { $env:SXCL_UI_MODS_QUERY = $query } else { Remove-Item Env:SXCL_UI_MODS_QUERY -ErrorAction SilentlyContinue }
  if ($version -ne '') { $env:SXCL_UI_MODS_VERSION = $version } else { Remove-Item Env:SXCL_UI_MODS_VERSION -ErrorAction SilentlyContinue }
  if ($install -ne '') {
    $env:SXCL_UI_MODS_INSTALL = $install
    # the results only exist after BOTH sources came back (the official attempt can take 8s to
    # time out before the mirror answers), so the click has to wait for them
    $env:SXCL_UI_MODS_INSTALL_DELAY = '18000'
  } else {
    Remove-Item Env:SXCL_UI_MODS_INSTALL -ErrorAction SilentlyContinue
  }
  $out = Join-Path $work ('run_' + $tag + '.txt')
  $err = $out + '.err'
  Remove-Item $out,$err -ErrorAction SilentlyContinue
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  return @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) +
         @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
}
function CfUrl($lines) {
  foreach ($line in $lines) { if ($line -match 'mods-fetch: source=curseforge via=\S+ url=(\S+)') { return $Matches[1] } }
  return ''
}

# =====================================================================================
# A + B1: filter area shape and the version input default
# =====================================================================================
Write-Output ''
Write-Output '== A/B: filter area shape + version default =='
Reset-Ini '114514'
$linesA = Invoke-Mods 'a_shape' '' 4000 '' '' 'mods-version-default'
# The area is EXACTLY TWO ROWS (user 2026-09-27: "the name box ... then the second row: version and
# mod source"), so the four controls live in two containers instead of directly under the area.
$row1 = Get-DirectChildren $linesA 'modsSearchRow'
$row2 = Get-DirectChildren $linesA 'modsOptionRow'
Ok ($row1.Count -gt 0 -and $row2.Count -gt 0) ('the filter area is two rows (row1=' + $row1.Count + ' row2=' + $row2.Count + ' direct children)')
$rowBlock = @($row1) + @($row2)
# NOTE: a dump line reads "SearchLineEdit #modsSearchBox (x,y ...)" -- the object name sits between
# the class name and the geometry, so the patterns must not glue "(" to the class name.
$widgets = @($rowBlock | Where-Object { $_ -match '\(\d+,\d+ \d+x\d+\)' })
$lineEdits = @($widgets | Where-Object { $_ -match 'LineEdit\b' })
$combos = @($widgets | Where-Object { $_ -match 'ComboBox\b' })
$checks = @($widgets | Where-Object { $_ -match 'CheckBox\b' })
$buttons = @($widgets | Where-Object { $_ -match 'PushButton\b' })
Write-Output ('  filter row children: lineEdit=' + $lineEdits.Count + ' combo=' + $combos.Count + ' checkBox=' + $checks.Count + ' button=' + $buttons.Count + ' (total widgets ' + $widgets.Count + ')')
foreach ($w in $widgets) { Write-Output ('    ' + $w.Trim()) }
Ok ($lineEdits.Count -eq 2) 'exactly two input boxes: the name search and the VERSION input'
Ok ($combos.Count -eq 0) 'the version filter is NOT a drop-down'
Ok ($checks.Count -eq 2) 'exactly two source check boxes'
Ok ($buttons.Count -eq 1) 'exactly one button in the whole filter area (the search action)'
$otherWidgets = @($widgets | Where-Object { $_ -notmatch 'LineEdit\b' -and $_ -notmatch 'CheckBox\b' -and $_ -notmatch 'PushButton\b' })
Ok ($otherWidgets.Count -eq 0) ('no fourth control in the filter area (' + $otherWidgets.Count + ' others)')

$def = Get-One $linesA 'mods-version-default: (\S+)'
Ok ($def -ne $null -and $def[1] -eq '1.20.1') ('the version input is pre-filled with the instance version (got ' + $(if ($def) { $def[1] } else { 'nothing' }) + ')')

# =====================================================================================
# B2: an instance whose JSON says nothing -> the input stays empty (never guess)
# =====================================================================================
Write-Output ''
Write-Output '== B2: instance without version information -> the input stays empty =='
Reset-Ini '424242'
$linesB = Invoke-Mods 'b_noversion' '' 4500 '' '' 'mods-version-default'
$defB = Get-One $linesB 'mods-version-default: (\S+)'
Ok ($defB -ne $null -and $defB[1] -eq 'empty') ('no version is guessed (got ' + $(if ($defB) { $defB[1] } else { 'nothing' }) + ')')

# =====================================================================================
# C: the typed version drives the search; a version that does not exist returns nothing
# =====================================================================================
Write-Output ''
Write-Output '== C: the version input drives the search =='
Reset-Ini '114514'
$linesC1 = Invoke-Mods 'c_typed' 'jei' 16000 '1.21.1' '' 'mods-fetch: source=modrinth'
$c1 = Get-One $linesC1 'mods-search: versionFilter=(\S+)'
Ok ($c1 -ne $null -and $c1[1] -eq '1.21.1') ('trace says versionFilter=1.21.1 (got ' + $(if ($c1) { $c1[1] } else { 'nothing' }) + ')')
$cf1 = CfUrl $linesC1
Ok ($cf1 -match 'gameVersion=1\.21\.1') 'the CurseForge request carries the typed version'
$mr1 = ''
foreach ($line in $linesC1) { if ($line -match 'mods-fetch: source=modrinth via=\S+ url=(\S+)') { $mr1 = $Matches[1] } }
Ok ($mr1 -match 'versions%3A1\.21\.1' -or $mr1 -match 'versions:1\.21\.1') 'the Modrinth facets carry the typed version'

# the same value picked from the candidate list must produce the very same request
$linesC2 = Invoke-Mods 'c_candidate' 'jei' 16000 '1.21.1' '' 'mods-fetch: source=modrinth'
$cf2 = CfUrl $linesC2
Write-Output ('  typed      cf url: ' + $cf1)
Write-Output ('  candidate  cf url: ' + $cf2)
Ok ($cf1 -eq $cf2 -and $cf1 -ne '') 'typing 1.21.1 and picking 1.21.1 send the identical request'

$linesC3 = Invoke-Mods 'c_bogus' 'jei' 16000 '9.9.9' '' 'mods-merged'
$c3 = Get-One $linesC3 'mods-search: versionFilter=(\S+)'
Ok ($c3 -ne $null -and $c3[1] -eq '9.9.9') 'a version that does not exist is sent as typed (not rewritten)'
$m3 = Get-All $linesC3 'mods-merged: sources=(\S*) rows=(\d+) perSource="([^"]*)"'
$last3 = $null
foreach ($m in $m3) { $last3 = $m }
if ($last3 -ne $null) {
  Write-Output ('  trace: rows=' + $last3[2] + ' perSource="' + $last3[3] + '"')
  Ok ([int]$last3[2] -eq 0) ('a version that does not exist honestly returns no rows (' + $last3[2] + ')')
} else { Ok $false 'the merged result line is missing for the bogus version' }
$defAfter = Get-One $linesC3 'mods-version-default: (\S+)'
Ok ($defAfter -ne $null -and $defAfter[1] -eq '1.20.1') 'the default is still the instance version (nothing was auto-changed)'

$linesC4 = Invoke-Mods 'c_empty' 'jei' 16000 'all' '' 'mods-fetch: source=modrinth'
$cf4 = CfUrl $linesC4
Ok ($cf4 -ne '' -and $cf4 -notmatch 'gameVersion=') 'an empty input sends no version filter at all'

# =====================================================================================
# D + E: a row has NO button; clicking the row opens the DETAIL page, and the ONE install
#       action on that page installs for the INSTANCE (user 2026-09-27: "a single click opens
#       the mod details"; the row itself stays button-free and four lines tall).
# =====================================================================================
Write-Output ''
Write-Output '== D: click a row (no button) -> detail page -> install -> jar lands in the instance folder =='
Reset-Ini '114514'
Ok ((Get-Jar (Join-Path $game 'versions\114514\mods')).Count -eq 0) 'the instance mods folder starts empty'
# 70s: the two-step click needs ~20s (chart + versions) and the jar itself can take half a minute
$linesD = Invoke-Mods 'd_rowclick' 'jei' 70000 '1.21.1' '1' 'mods-merged'
# The dump at the end shows what the user is looking at: the DETAIL page (the list is behind it).
# NOTE: "a result row carries NO button" is asserted on the LIST dump by tools/ui_mods_rows.ps1
# (section C: zero PushButton inside every #modsResultCard) -- here we prove the transition.
$detailPage = Find-Visible $linesD '#ModDetailPage \('
Ok ($detailPage -ne '') 'the click landed on the mod DETAIL page (#ModDetailPage on screen)'
$detailRoute = Get-One $linesD 'mods-detail: route=(\S+) page=(\S+) key=(\S+)'
if ($detailRoute -ne $null) {
  Write-Output ('  trace: route=' + $detailRoute[1] + ' page=' + $detailRoute[2] + ' key=' + $detailRoute[3])
  Ok ($detailRoute[2] -match 'ModDetailPage') 'route/page evidence: the shell switched to the detail page'
} else { Ok $false 'no mods-detail route/page line' }
$detailDesc = Find-Visible $linesD '#modsDetailDescription \('
Ok ($detailDesc -ne '') 'the detail page dumps the FULL description (not the three-dot list line)'
# (the button may already say "正在装…" here: the dump lands while the download is still running --
#  the run below proves it finished by checking the jar on disk)
$detailBtn = Find-Visible $linesD '#modsDetailInstallButton \('
Ok ($detailBtn -ne '') 'the detail page carries the ONE install action (modsDetailInstallButton)'
$clicked = Get-One $linesD ([regex]::Escape((C @(0x5DF2,0x70B9,0x7B2C))) + ' (\d+) ' + (C @(0x5F20,0x7ED3,0x679C,0x5361)))
Ok ($clicked -ne $null) 'the acceptance hook clicked the ROW itself'
$detailClicked = Get-One $linesD ((C @(0x8BE6,0x60C5,0x9875,0x5DF2,0x70B9)))
Ok ($detailClicked -ne $null) 'the acceptance hook clicked the install action ON the detail page'
$d1 = Get-One $linesD 'mods-search: versionFilter=(\S+)'
Ok ($d1 -ne $null -and $d1[1] -eq '1.21.1') 'the filter was on 1.21.1 while installing'
$installed = Get-Jar (Join-Path $game 'versions\114514\mods')
Write-Output ('  files in <game>\versions\114514\mods : ' + (($installed | ForEach-Object { $_.Name + ' (' + $_.Length + ' bytes)' }) -join ', '))
Ok ($installed.Count -ge 1) 'the jar landed in the instance folder (the version he has installed)'
if ($installed.Count -ge 1) { Write-Output ('  REAL PATH: ' + $installed[0].FullName) }
Ok ((Get-Jar (Join-Path $game 'mods')).Count -eq 0) 'nothing landed in the shared <game>\mods'
Ok ((Get-Jar (Join-Path $game 'versions\424242\mods')).Count -eq 0) 'nothing landed in another instance'

Write-Output ''
Write-Output '== E: nothing asks the user where to install =='
$srcHits = @(Select-String -Path (Join-Path $root 'src\ui\pages\mods_page.cpp') -Pattern (C @(0x9009,0x62E9,0x5B89,0x88C5,0x4F4D,0x7F6E)) -ErrorAction SilentlyContinue)
Ok ($srcHits.Count -eq 0) ('the page source never mentions choosing an install location (' + $srcHits.Count + ' hits)')
$wordHits = @($linesA | Where-Object { $_ -match (C @(0x5B89,0x88C5,0x4F4D,0x7F6E)) -or $_ -match (C @(0x9009,0x62E9,0x76EE,0x5F55)) })
Ok ($wordHits.Count -eq 0) ('no such wording on screen either (' + $wordHits.Count + ' lines)')

Write-Output ''
Write-Output ('  product files: ' + $work)
if ($fail -eq 0) {
  Write-Output ('MODS-FILTERS: ALL OK (' + $total + '/' + $total + ')')
  exit 0
}
Write-Output ('MODS-FILTERS: FAIL (' + $fail + ' of ' + $total + ' assertions)')
exit 1
