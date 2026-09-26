# (C) Silent X Craft Launcher -- version-select page acceptance (user 2026-09-26).
# Assertions (dump based, both themes):
#   1) no "current version" TEXT anywhere (neither the tag label nor the name suffix)
#   2) exactly one self-drawn accent mark #sxclCurrentVersionMark, flush at the current row's left edge
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
$work = 'D:\SilentStudio\_test\sxcl_tasks\version_tag'
New-Item -ItemType Directory -Force -Path $work | Out-Null

# fixture: two installed versions (one of them = the selected one)
$game = Join-Path $work 'mc'
foreach ($v in @('1.20.1', '1.19.4')) {
  $dir = Join-Path $game ('versions\' + $v)
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  Set-Content -Path (Join-Path $dir ($v + '.json')) -Value ('{"id":"' + $v + '","mainClass":"net.minecraft.client.main.Main","libraries":[]}') -Encoding utf8
  Set-Content -Path (Join-Path $dir ($v + '.jar')) -Value 'fixture' -Encoding utf8
}
$ini = Join-Path $work 'select.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $game) -Encoding utf8
Add-Content -Path $ini -Value 'game.selected_version=1.20.1'

# text fragments that must NOT be in the tree any more (built from code points -> ASCII file)
function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }
$tag = (C @(0x5F53,0x524D,0x7248,0x672C))                       # "current version"
$suffix = (C @(0x21D0))                                          # the arrow that used to prefix the tag
$selected = '1.20.1'

$cases = 0
$bad = @()
foreach ($theme in @('dark', 'light')) {
  $cases++
  $env:SXCL_UI_WINDOW = '1100x750'
  $env:SXCL_UI_ROUTE = 'select'
  $env:SXCL_UI_THEME = $theme
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '12'   # version rows sit deeper than the default depth 6
  $env:SXCL_UI_SHOT = (Join-Path $work ('shot_' + $theme + '.png'))
  $env:SXCL_UI_SHOT_DELAY = '4000'
  $out = Join-Path $work ('dump_' + $theme + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $txt = @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)

  # (1) the text must be gone
  $hits = @($txt | Select-String -Pattern ([regex]::Escape($tag)))
  $hits2 = @($txt | Select-String -Pattern ([regex]::Escape($suffix)))
  if ($hits.Count -ne 0) { $bad += ('[' + $theme + '] (1) "' + $tag + '" still in the tree: ' + $hits[0].Line.Trim()) }
  if ($hits2.Count -ne 0) { $bad += ('[' + $theme + '] (1) name suffix still in the tree: ' + $hits2[0].Line.Trim()) }

  # (2) exactly one accent mark, flush at the current row's left edge
  $marks = @(0..($txt.Count - 1) | Where-Object { $txt[$_] -match '#sxclCurrentVersionMark' })
  if ($marks.Count -ne 1) {
    $bad += ('[' + $theme + '] (2) accent mark count = ' + $marks.Count + ' (want 1)')
  } else {
    $idx = $marks[0]
    $line = $txt[$idx]
    $null = ($line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
    $mx = [int]$Matches[1]; $my = [int]$Matches[2]; $mw = [int]$Matches[3]; $mh = [int]$Matches[4]
    $indent = $line.Length - $line.TrimStart().Length
    $cardLine = ''
    $cardIdx = -1
    for ($i = $idx - 1; $i -ge 0; $i--) {
      $li = $txt[$i].Length - $txt[$i].TrimStart().Length
      if ($li -lt $indent -and $txt[$i] -match 'CardWidget \(') { $cardLine = $txt[$i]; $cardIdx = $i; break }
    }
    if ($cardLine -eq '') {
      $bad += ('[' + $theme + '] (2) no CardWidget ancestor for the accent mark')
    } else {
      $null = ($cardLine -match '\((\d+),(\d+) (\d+)x(\d+)\)')
      $cx = [int]$Matches[1]; $cy = [int]$Matches[2]; $cw = [int]$Matches[3]; $ch = [int]$Matches[4]
      # the card's subtree = the lines after the card line until the indent drops back
      $cardIndent = $cardLine.Length - $cardLine.TrimStart().Length
      $nameInRow = $false
      for ($k = $cardIdx + 1; $k -lt $txt.Count; $k++) {
        $lk = $txt[$k].Length - $txt[$k].TrimStart().Length
        if ($lk -le $cardIndent) { break }
        if ($txt[$k] -like ('*"' + $selected + '"*')) { $nameInRow = $true }
      }
      $leftGap = $mx - $cx
      $topGap = $my - $cy
      $bottomGap = ($cy + $ch) - ($my + $mh)
      if ($leftGap -ne 2) { $bad += ('[' + $theme + '] (2) mark left gap = ' + $leftGap + ' (want 2)') }
      if ($mw -ne 3) { $bad += ('[' + $theme + '] (2) mark width = ' + $mw + ' (want 3)') }
      if ($topGap -ne 12 -or $bottomGap -ne 12) { $bad += ('[' + $theme + '] (2) mark vertical insets = ' + $topGap + '/' + $bottomGap + ' (want 12/12)') }
      if (-not $nameInRow) { $bad += ('[' + $theme + '] (2) the row carrying the mark is not the selected version ' + $selected) }
      $markCount = $marks.Count
      Write-Output ('  [' + $theme + '] mark=(' + $mx + ',' + $my + ' ' + $mw + 'x' + $mh + ') row=(' + $cx + ',' + $cy + ' ' + $cw + 'x' + $ch + ') leftGap=' + $leftGap + ' insets=' + $topGap + '/' + $bottomGap + ' nameInRow=' + $nameInRow + ' marks=' + $markCount)
    }
  }
}

if ($bad.Count -eq 0) {
  Write-Output ('VERSION-TAG: ALL OK (' + $cases + '/2 themes: the tag text is gone, name suffix gone, exactly one self-drawn accent mark 3px at the selected row left edge)')
  exit 0
}
foreach ($b in $bad) { Write-Output ('  FAIL ' + $b) }
Write-Output ('VERSION-TAG: FAIL (' + $bad.Count + ' assertion(s))')
exit 1
