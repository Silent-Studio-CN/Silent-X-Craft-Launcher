# (C) Silent X Craft Launcher -- "mods page layout: two filter rows + four-line rows" (user 2026-09-27).
#
# The user's rules, verbatim:
#   * filter area is EXACTLY two rows: row 1 = the name box (it grows, the search button sits right
#     next to it); row 2 = the version box + the mod source boxes. NOTHING else in that area.
#   * the version filter is a typeable INPUT (MC has thousands of versions);
#   * a result row is EXACTLY four lines: (1) name + source on the right, (2) developer + version
#     RANGE ("1.20 - 1.21.4"), (3) description on ONE line with an ellipsis, (4) empty;
#   * the logo is a fixed square: a picture may never make a row taller;
#   * download counts are unit-ised ("3.5万" / "1.2亿"), the update time is relative ("3 小时前");
#   * a row has NO button -- the whole row is the click target (clicking installs).
#
# Evidence: the widget dump (geometry, indentation = the real tree) + the page's own trace lines.
# ASCII-only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM);
# Chinese only ever appears through code points.
$ErrorActionPreference = 'Continue'

foreach ($v in @('SXCL_UI_ROUTE','SXCL_UI_NAV','SXCL_UI_THEME','SXCL_UI_DUMP','SXCL_UI_DUMP_DEPTH',
                 'SXCL_UI_MODS_QUERY','SXCL_UI_MODS_QUERY_DELAY','SXCL_UI_MODS_VERSION',
                 'SXCL_UI_MODS_SOURCES','SXCL_UI_MODS_INSTALL','SXCL_UI_MODS_INSTALL_DELAY',
                 'SXCL_UI_SETTINGS','SXCL_UI_GAME_DIR','SXCL_UI_WINDOW','SXCL_UI_SHOT',
                 'SXCL_UI_SHOT_DELAY','SXCL_MODS_MIRROR','SXCL_UI2')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# The exe can be pointed at any build tree (every agent builds in its own private directory);
# the default is the shared one.
$exe = $env:SXCL_UI_EXE
if (-not $exe -or $exe -eq '') { $exe = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe' }
$work = 'D:\SilentStudio\_test\sxcl_mods_rows'
$fail = 0
$total = 0
function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }
function Ok([bool]$cond, [string]$what) {
  $script:total++
  if ($cond) { Write-Output ('  [ok]   ' + $what) }
  else { Write-Output ('  [FAIL] ' + $what); $script:fail++ }
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
# "ClassName #objectName (x,y WxH) ..." -> x,y,w,h  (0,0,0,0 when the line has no geometry)
function Geometry($line) {
  if ($null -eq $line) { return @(0, 0, 0, 0) }
  $m = [regex]::Match([string]$line, '\((-?\d+),(-?\d+) (\d+)x(\d+)\)')
  if (-not $m.Success) { return @(0, 0, 0, 0) }
  return @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value)
}
function LabelText($line) {
  if ($null -eq $line) { return '' }
  $m = [regex]::Match([string]$line, '"([^"]*)"')
  if ($m.Success) { return $m.Groups[1].Value }
  return ''
}
if (-not (Test-Path $exe)) { Write-Output ('MODS-ROWS: FAIL (exe not found: ' + $exe + ')'); exit 1 }
# Another sxcl-ui.exe (a colleague's acceptance run, or the one the user keeps open) does NOT have
# to stop this script: every number below comes from THIS run's own window (a widget dump), its own
# settings file, its own game dir and its own fixture -- nothing is shared with that instance.
# Still, give a quiet machine a moment (cleaner timings) and say so when we do not get one.
if (@(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue).Count -gt 0) {
  Write-Output 'NOTE another sxcl-ui.exe is running; waiting up to 30s for a quiet machine'
  for ($i = 0; $i -lt 6 -and @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue).Count -gt 0; $i++) {
    Start-Sleep -Seconds 5
  }
  if (@(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue).Count -gt 0) {
    Write-Output 'NOTE still busy -- continuing anyway (nothing is shared with that instance)'
  } else {
    Write-Output 'NOTE the machine is quiet now'
  }
}

# ---- fixture: one instance, its version file says 1.20.1 (that is the default chart's baseline) ----
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
$game = Join-Path $work 'mc'
New-Item -ItemType Directory -Force -Path (Join-Path $game 'versions\114514') | Out-Null
$j = @{ id='114514'; mainClass='net.minecraft.client.main.Main'; type='release'; clientVersion='1.20.1'; libraries=@() }
Set-Content -Path (Join-Path $game 'versions\114514\114514.json') -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8
Set-Content -Path (Join-Path $game 'versions\114514\114514.jar') -Value 'fixture' -Encoding ascii
$ini = Join-Path $work 'mods.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $game) -Encoding utf8
Add-Content -Path $ini -Value 'game.selected_version=114514'
Add-Content -Path $ini -Value 'general.version_isolation=1'

function Invoke-Mods([string]$tag, [int]$waitMs, [string]$query = '', [string]$install = '') {
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = $game
  $env:SXCL_UI_ROUTE = 'download'
  $env:SXCL_UI_NAV = 'download_mod'
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_WINDOW = '1100x750'
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '18'
  $env:SXCL_UI_SHOT = (Join-Path $work ('shot_' + $tag + '.png'))
  $env:SXCL_UI_SHOT_DELAY = [string]$waitMs
  Remove-Item Env:SXCL_UI_MODS_QUERY -ErrorAction SilentlyContinue
  Remove-Item Env:SXCL_UI_MODS_VERSION -ErrorAction SilentlyContinue
  Remove-Item Env:SXCL_UI_MODS_INSTALL -ErrorAction SilentlyContinue
  if ($query -ne '') {
    $env:SXCL_UI_MODS_QUERY = $query
    # the default chart may still be in flight (CF official has to time out before the mirror
    # answers), so a typed search waits for a quiet page before it types.
    $env:SXCL_UI_MODS_QUERY_DELAY = '14000'
  }
  if ($install -ne '') {
    $env:SXCL_UI_MODS_INSTALL = $install
    $env:SXCL_UI_MODS_INSTALL_DELAY = '16000'
  }
  $out = Join-Path $work ('run_' + $tag + '.txt')
  $err = $out + '.err'
  Remove-Item $out,$err -ErrorAction SilentlyContinue
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  return @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) +
         @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
}

# =====================================================================================
# RUN 1: empty search -> the downloads chart, four-line rows, no button in a row
# =====================================================================================
Write-Output ''
Write-Output '== A: filter area = exactly two rows (name+search / version+sources) =='
$lines = Invoke-Mods 'layout' 15000
Ok ((Find-Visible $lines '#modsFilterRow \(') -ne '') 'the filter area is on screen (#modsFilterRow)'

$row1 = Get-DirectChildren $lines 'modsSearchRow'
$row2 = Get-DirectChildren $lines 'modsOptionRow'
Ok ($row1.Count -gt 0) ('row 1 is a container of its own (#modsSearchRow, ' + $row1.Count + ' widgets)')
Ok ($row2.Count -gt 0) ('row 2 is a container of its own (#modsOptionRow, ' + $row2.Count + ' widgets)')
$w1 = @($row1 | Where-Object { $_ -match '\(\d+,\d+ \d+x\d+\)' })
$w2 = @($row2 | Where-Object { $_ -match '\(\d+,\d+ \d+x\d+\)' })
Write-Output ('  row 1: ' + $w1.Count + ' widgets')
foreach ($w in $w1) { Write-Output ('    ' + $w.Trim()) }
Write-Output ('  row 2: ' + $w2.Count + ' widgets')
foreach ($w in $w2) { Write-Output ('    ' + $w.Trim()) }
Ok (@($w1 | Where-Object { $_ -match 'LineEdit\b' }).Count -eq 1 -and
    @($w2 | Where-Object { $_ -match 'LineEdit\b' }).Count -eq 1)
   'exactly two input boxes in the whole area: the name search (row 1) and the version (row 2)'
Ok (@($w1 | Where-Object { $_ -match 'PushButton\b' }).Count -eq 1) 'one button in the whole area, and it is on row 1'
Ok (@($w2 | Where-Object { $_ -match 'CheckBox\b' }).Count -eq 2) 'exactly two source check boxes, both on row 2'
Ok (@($w1 | Where-Object { $_ -match 'ComboBox\b' }).Count -eq 0 -and
    @($w2 | Where-Object { $_ -match 'ComboBox\b' }).Count -eq 0) 'the version filter is NOT a drop-down'
$other = @($w1 + $w2 | Where-Object { $_ -notmatch 'LineEdit\b' -and $_ -notmatch 'CheckBox\b' -and $_ -notmatch 'PushButton\b' })
Ok ($other.Count -eq 0) ('no fifth control in the filter area (' + $other.Count + ')')

# row 1: the box grows, the button sits right next to it
$boxLine = Find-Visible $lines '#modsSearchBox \('
$btnLine = Find-Visible $lines '#modsSearchButton \('
$gBox = Geometry $boxLine; $gBtn = Geometry $btnLine
$searchRowLine = Find-Visible $lines '#modsSearchRow \('
$gRow1 = Geometry $searchRowLine
Write-Output ('  name box: ' + ($gBox -join ',') + '   search button: ' + ($gBtn -join ',') + '   row 1: ' + ($gRow1 -join ','))
Ok ($gBox[2] -ge 200) ('the name box is at least 200px wide (' + $gBox[2] + ')')
Ok ($gBox[2] -ge ($gRow1[2] - $gBtn[2] - 30)) ('the name box eats the rest of row 1 (box ' + $gBox[2] + ' of row ' + $gRow1[2] + ', button ' + $gBtn[2] + ')')
Ok ($gBtn[0] -ge ($gBox[0] + $gBox[2])) 'the search button is immediately to the right of the box'
Ok ([Math]::Abs($gBtn[1] - $gBox[1]) -le 6) 'box and button sit on the same line'

# row 2 is BELOW row 1
$verLine = Find-Visible $lines '#modsVersionFilter \('
$gVer = Geometry $verLine
$mLine = Find-Visible $lines '#modsSourceModrinth \('
$cLine = Find-Visible $lines '#modsSourceCurseForge \('
$gM = Geometry $mLine; $gC = Geometry $cLine
$gRow2 = Geometry (Find-Visible $lines '#modsOptionRow \(')
Write-Output ('  version box: ' + ($gVer -join ',') + '   sources: ' + ($gM -join ',') + ' / ' + ($gC -join ',') + '   row 2: ' + ($gRow2 -join ','))
Ok ($gRow2[1] -ge ($gRow1[1] + $gRow1[3])) 'row 2 really starts below row 1 (two rows, not one wrapping row)'
Ok ([Math]::Abs($gVer[1] - $gM[1]) -le 8 -and [Math]::Abs($gM[1] - $gC[1]) -le 8) 'version box and both source boxes are on ONE line (row 2)'
Ok ($gM[0] -ge ($gVer[0] + $gVer[2])) 'the source boxes come after the version box'
Ok ($mLine -match ' checked' -and $cLine -match ' checked') 'both sources are ticked by default'
$cut = @($w1 + $w2 | Where-Object { $_ -match 'CUT-W' -or $_ -match 'CUT-H' })
Ok ($cut.Count -eq 0) ('no filter control is cut (' + $cut.Count + ')')

Write-Output ''
Write-Output '== B: empty search = the DOWNLOADS chart, filtered by the instance version =='
$chart = Get-One $lines 'mods-default-chart: instance=(\S+) version=(\S+)'
Ok ($chart -ne $null) 'the page pulled the default chart by itself (no keyword was typed)'
if ($chart -ne $null) { Write-Output ('  trace: instance=' + $chart[1] + ' version=' + $chart[2]) }
$srch = Get-One $lines 'mods-search: versionFilter=(\S+) text="([^"]*)" index=(\S+)'
if ($srch -ne $null) {
  Write-Output ('  trace: versionFilter=' + $srch[1] + ' text="' + $srch[2] + '" index=' + $srch[3])
  Ok ($srch[2] -eq '') 'the default chart really is an EMPTY search'
  Ok ($srch[3] -eq 'downloads') 'empty search is sorted by downloads (index=downloads)'
} else { Ok $false 'no mods-search line' }
$mr = ''
foreach ($l in $lines) { if ($l -match 'mods-fetch: source=modrinth via=\S+ url=(\S+)') { $mr = $Matches[1] } }
$cf = ''
foreach ($l in $lines) { if ($l -match 'mods-fetch: source=curseforge via=\S+ url=(\S+)') { $cf = $Matches[1] } }
Write-Output ('  modrinth url: ' + $mr)
Write-Output ('  curseforge url: ' + $cf)
# the MOD column (not the shader one: project_type=mod / classId=6) is what the user is looking at
$mrMod = @()
$cfMod = @()
foreach ($l in $lines) {
  $m = [regex]::Match($l, 'mods-fetch: source=modrinth via=\S+ url=(\S+)')
  if ($m.Success -and $m.Groups[1].Value -match 'project_type(:|%3A)mod') { $mrMod += $m.Groups[1].Value }
  $m = [regex]::Match($l, 'mods-fetch: source=curseforge via=\S+ url=(\S+)')
  if ($m.Success -and $m.Groups[1].Value -match 'classId=6') { $cfMod += $m.Groups[1].Value }
}
Write-Output ('  mod-column modrinth url: ' + $(if ($mrMod.Count -gt 0) { $mrMod[-1] } else { '(none)' }))
Write-Output ('  mod-column curseforge url: ' + $(if ($cfMod.Count -gt 0) { $cfMod[-1] } else { '(none)' }))
Ok ($mrMod.Count -gt 0 -and $mrMod[-1] -match 'index=downloads') 'the MOD column request carries index=downloads'
Ok ($mrMod.Count -gt 0 -and ($mrMod[-1] -match 'versions%3A1\.20\.1' -or $mrMod[-1] -match 'versions:1\.20\.1')) 'the MOD chart is filtered by the instance version (Modrinth facet)'
Ok ($cfMod.Count -gt 0 -and $cfMod[-1] -match 'sortField=2' -and $cfMod[-1] -match 'sortOrder=desc') 'the MOD column CurseForge request carries sortField=2&sortOrder=desc'
Ok ($cfMod.Count -gt 0 -and $cfMod[-1] -match 'gameVersion=1\.20\.1') 'the MOD column CurseForge request is filtered by the same version'
# the pane that is NOT on screen must not pull a chart behind the user's back
$chartLines = @($lines | Where-Object { $_ -match 'mods-default-chart:' })
Ok ($chartLines.Count -eq 1) ('only the pane on screen pulled the default chart (' + $chartLines.Count + ' time(s))')
$merged = Get-One $lines 'mods-merged: sources=(\S+) rows=(\d+) perSource="([^"]*)" index=(\S+)'
if ($merged -ne $null) {
  Write-Output ('  trace: sources=' + $merged[1] + ' rows=' + $merged[2] + ' perSource="' + $merged[3] + '" index=' + $merged[4])
  Ok ([int]$merged[2] -gt 0) ('the default chart really returned rows (' + $merged[2] + ')')
  Ok ($merged[4] -eq 'downloads') 'the merged line says index=downloads'
  Ok ($merged[3] -match 'Modrinth \d+') 'the merged line carries the per-source counts'
} else { Ok $false 'no mods-merged line for the default chart' }

Write-Output ''
Write-Output '== C: every result row is exactly four lines, fixed height, no button =='
$cards = @($lines | Where-Object { $_ -match '#modsResultCard \(' -and $_ -notmatch ' hidden' })
Ok ($cards.Count -gt 0) ('result cards are on screen (' + $cards.Count + ')')
$heights = @{}
for ($i = 0; $i -lt [Math]::Min(4, $cards.Count); $i++) {
  $g = Geometry $cards[$i]
  $heights[$g[3]] = 1
}
# walk the cards one by one (indentation = that row's own sub-tree)
$cardLines = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
  if ($lines[$i] -match '#modsResultCard \(' -and $lines[$i] -notmatch ' hidden') { $cardLines += $i }
}
Ok ($heights.Keys.Count -eq 1) ('all sampled rows have the SAME fixed height (' + (($heights.Keys) -join ',') + ')')
$checkedCards = 0
foreach ($idx in ($cardLines | Select-Object -First 3)) {
  $card = $lines[$idx]
  $g = Geometry $card
  $indent = $card.Length - $card.TrimStart().Length
  $block = @($card)
  for ($k = $idx + 1; $k -lt $lines.Count; $k++) {
    $ik = $lines[$k].Length - $lines[$k].TrimStart().Length
    if ($ik -le $indent) { break }
    $block += $lines[$k]
  }
  $checkedCards++
  $one = @($block | Where-Object { $_ -match '#modsRowTitle \(' })
  $tag = @($block | Where-Object { $_ -match '#modsSourceTag \(' })
  $dev = @($block | Where-Object { $_ -match '#modsRowDev \(' })
  $stats = @($block | Where-Object { $_ -match '#modsRowStats \(' })
  $desc = @($block | Where-Object { $_ -match '#modsRowDesc \(' })
  $blank = @($block | Where-Object { $_ -match '#modsRowBlank \(' })
  $icon = @($block | Where-Object { $_ -match '#modsRowIcon \(' })
  $buttons = @($block | Where-Object { $_ -match 'PushButton\b' -or $_ -match 'ToolButton\b' -or $_ -match 'QAbstractButton' })
  Ok ($one.Count -eq 1 -and $tag.Count -eq 1 -and $dev.Count -eq 1 -and $stats.Count -eq 1 -and
      $desc.Count -eq 1 -and $blank.Count -eq 1 -and $icon.Count -eq 1)
     ('card ' + $checkedCards + ': four lines + icon exist once each')
  Ok ($buttons.Count -eq 0) ('card ' + $checkedCards + ': NO button anywhere in the row')
  if ($one.Count -eq 1 -and $tag.Count -eq 1 -and $dev.Count -eq 1 -and $desc.Count -eq 1 -and $blank.Count -eq 1) {
    $gTitle = Geometry $one[0]; $gTag = Geometry $tag[0]; $gDev = Geometry $dev[0]
    $gDesc = Geometry $desc[0]; $gBlank = Geometry $blank[0]; $gIcon = Geometry $icon[0]
    Ok ($gTitle[1] -eq $gDev[1] -or $gDev[1] -gt $gTitle[1]) ('card ' + $checkedCards + ': line 2 is below line 1')
    Ok ($gDesc[1] -gt $gDev[1]) ('card ' + $checkedCards + ': line 3 (description) is below line 2')
    Ok ($gBlank[1] -gt $gDesc[1]) ('card ' + $checkedCards + ': line 4 (blank) is below line 3')
    Ok ($gBlank[3] -gt 0 -and (LabelText $blank[0]) -eq '') ('card ' + $checkedCards + ': line 4 is empty but occupies its height')
    Ok ($gTag[0] -gt $gTitle[0] + 40) ('card ' + $checkedCards + ': the source tag is on the RIGHT of line 1')
    Ok ([Math]::Abs($gTag[1] - $gTitle[1]) -le 14) ('card ' + $checkedCards + ': the tag is on the same line as the name')
    Ok ($gIcon[2] -eq $gIcon[3] -and $gIcon[2] -gt 0) ('card ' + $checkedCards + ': the logo is a SQUARE (' + $gIcon[2] + 'x' + $gIcon[3] + ')')
    Ok (($gIcon[3] + 20) -le $g[3]) ('card ' + $checkedCards + ': the logo cannot make the row taller (icon ' + $gIcon[3] + ' < row ' + $g[3] + ')')
    Ok ($gDesc[3] -gt 0 -and $gDesc[3] -le 26) ('card ' + $checkedCards + ': the description is ONE line (' + $gDesc[3] + 'px tall)')
    $descText = LabelText $desc[0]
    $devText = LabelText $dev[0]
    $statsText = LabelText $stats[0]
    Write-Output ('    card ' + $checkedCards + ': name="' + (LabelText $one[0]).Substring(0, [Math]::Min(28, (LabelText $one[0]).Length)) + '" tag="' + (LabelText $tag[0]) + '"')
    Write-Output ('      line2: "' + $devText + '"   stats: "' + $statsText + '"')
    Write-Output ('      line3: "' + $descText.Substring(0, [Math]::Min(46, $descText.Length)) + '"')
    # version RANGE, not a list: no spaces between versions, an en dash in between
    if ($devText -match '\d') {
      Ok ($devText -match '\d+\.\d+') ('card ' + $checkedCards + ': line 2 carries the version range')
      Ok ($devText -notmatch '(\d+\.\d+) (\d+\.\d+)') ('card ' + $checkedCards + ': line 2 is a RANGE, not a version list')
    }
    # unit-ised download count + relative time
    Ok ($statsText -match '(万|亿|\d)\s*' + (C @(0x6B21,0x4E0B,0x8F7D))) ('card ' + $checkedCards + ': the download count is on line 2')
    Ok ($statsText -match (C @(0x5C0F,0x65F6,0x524D)) -or $statsText -match (C @(0x5929,0x524D)) -or
        $statsText -match (C @(0x5206,0x949F,0x524D)) -or $statsText -match (C @(0x5C81)) -or
        $statsText -match (C @(0x4E2A,0x6708,0x524D)) -or $statsText -match (C @(0x521A,0x521A)))
       ('card ' + $checkedCards + ': the update time is RELATIVE ("' + $statsText + '")')
    $cutCard = @($block | Where-Object { $_ -match 'CUT-W' -or $_ -match 'CUT-H' })
    Ok ($cutCard.Count -eq 0) ('card ' + $checkedCards + ': nothing is cut in the row (' + $cutCard.Count + ')')
  }
}
$ell = C @(0x2026)
$descAll = @($lines | Where-Object { $_ -match '#modsRowDesc \(' -and $_ -notmatch ' hidden' })
$elided = @($descAll | Where-Object { (LabelText $_) -like ('*' + $ell) })
Write-Output ('  descriptions shown: ' + $descAll.Count + ', ending in an ellipsis: ' + $elided.Count)
Ok ($elided.Count -gt 0) 'a description that does not fit is elided with an ellipsis'
$units = @($lines | Where-Object { $_ -match '#modsRowStats \(' -and $_ -notmatch ' hidden' -and ($_ -match (C @(0x4EBF)) -or $_ -match (C @(0x4E07))) })
Write-Output ('  rows whose count uses a unit: ' + $units.Count)
Ok ($units.Count -gt 0) 'the top-downloaded chart shows unit-ised counts (万 / 亿)'

Write-Output ''
Write-Output '== D: no technical detail anywhere on screen =='
# "no technical detail" (the user's rule): a URL, a host, an API key, an HTTP status or a hash
# must never reach the screen. Matched as whole technical tokens -- plain English words in a mod
# description are NOT a violation ("compatibility" is not ".com/").
$tech = @('http://', 'https://', 'www.', '.com/', '.net/', '.top/', 'mcimirror', 'api key',
          'x-api-key', 'curseforge.com', 'modrinth.com', 'sha1', 'sha-1', 'json')
$techHits = @()
foreach ($line in $lines) {
  if ($line -match ' hidden') { continue }
  if ($line -notmatch '#mods(ResultCard|RowTitle|RowDev|RowStats|RowDesc|RowBlank|SourceTag|ContextLine|VersionFilter) \(') { continue }
  foreach ($t in $tech) {
    if ($line -match [regex]::Escape($t)) { $techHits += ($t + '  <-  ' + $line.Trim()) }
  }
}
Ok ($techHits.Count -eq 0) ('no URL / key / http code in the visible rows (' + $techHits.Count + ' lines)')
foreach ($h in ($techHits | Select-Object -First 3)) { Write-Output ('    ' + $h) }

Write-Output ''
Write-Output '== E: clicking a row installs it (the whole row is the click target) =='
$lineInst = Invoke-Mods 'install' 42000 '' '1'
$installed = @(Get-ChildItem -Path (Join-Path $game 'versions\114514\mods') -Filter *.jar -ErrorAction SilentlyContinue)
Write-Output ('  files in <game>\versions\114514\mods: ' + (($installed | ForEach-Object { $_.Name + ' (' + $_.Length + ' bytes)' }) -join ', '))
Ok ($installed.Count -ge 1) 'the first row of the DEFAULT chart installed into the instance folder'
Ok (@(Get-ChildItem -Path (Join-Path $game 'mods') -Filter *.jar -ErrorAction SilentlyContinue).Count -eq 0) 'nothing landed in the shared <game>\mods'
$clickedLine = Get-One $lineInst ([regex]::Escape((C @(0x5DF2,0x70B9,0x7B2C))) + ' (\d+) ' + (C @(0x5F20,0x7ED3,0x679C,0x5361)))
Ok ($clickedLine -ne $null) 'the acceptance hook clicked the ROW itself'

Write-Output ''
Write-Output ('  product files: ' + $work)
if ($fail -eq 0) {
  Write-Output ('MODS-ROWS: ALL OK (' + $total + '/' + $total + ')')
  exit 0
}
Write-Output ('MODS-ROWS: FAIL (' + $fail + ' of ' + $total + ' assertions)')
exit 1
