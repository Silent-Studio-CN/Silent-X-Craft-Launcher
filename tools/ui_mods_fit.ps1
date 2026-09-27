# (C) Silent X Craft Launcher -- "filter row fits" acceptance (user 2026-09-27).
#
# The user's rule, verbatim:
#   "all text fields are independent elements; an independent element must be able to show itself
#    COMPLETELY, unless the window is really smaller than what that element needs."
#
# The bug it fixes: the two source check boxes under the name box were squeezed to 41px while they
# need 79/90px, so their text was cut off (dump marker CUT-W).  The filter row is now a FlowLayout:
#   * every control's own sizeHint is its floor -- the row NEVER squeezes a control;
#   * when the row does not fit, the controls WRAP to the next line instead.
#
# Per window size (900x600 / 1100x750 / 1280x800 / 700x560 = narrower than the row needs):
#   A) no filter control carries the CUT-W / CUT-H marker, in BOTH copies the dump prints
#      (the mod pane and the hidden shader pane -- the hidden copy is the "before the layout has
#      settled" geometry the user kept seeing);
#   B) for every control that has a text metric line: got >= need in width AND height;
#   C) the name box is at least its minimum width (it is the only control allowed to grow);
#   D) no control is wider than the filter row container, and in the narrow case the row really
#      WRAPS (its height is at least two rows) instead of clipping.
#
# ASCII-only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM).
$ErrorActionPreference = 'Continue'

foreach ($v in @('SXCL_UI_ROUTE','SXCL_UI_NAV','SXCL_UI_THEME','SXCL_UI_DUMP','SXCL_UI_DUMP_DEPTH',
                 'SXCL_UI_MODS_QUERY','SXCL_UI_MODS_SOURCES','SXCL_UI_SETTINGS','SXCL_UI_GAME_DIR',
                 'SXCL_UI_WINDOW','SXCL_UI_SHOT','SXCL_UI_SHOT_DELAY','SXCL_MODS_MIRROR','SXCL_UI2')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe  = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\sxcl_mods_fit'
$fail = 0
$total = 0
function Ok([bool]$cond, [string]$what) {
  $script:total++
  if ($cond) { Write-Output ('  [ok]   ' + $what) }
  else { Write-Output ('  [FAIL] ' + $what); $script:fail++ }
}

if (-not (Test-Path $exe)) { Write-Output ('MODS-FIT: FAIL (exe not found: ' + $exe + ')'); exit 1 }
$busy = @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
  Write-Output ('MODS-FIT: FAIL (another sxcl-ui.exe is running, pid ' + ($busy.Id -join ',') + ')')
  exit 1
}
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null
$ini = Join-Path $work 'fit.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + (Join-Path $work 'mc')) -Encoding utf8

# the controls that make up the filter row (name box, search button, two source boxes)
$names = @('modsSearchBox','modsSearchButton','modsSourceModrinth','modsSourceCurseForge')

function Invoke-Fit([string]$tag, [string]$size) {
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = (Join-Path $work 'mc')
  $env:SXCL_UI_ROUTE = 'download'
  $env:SXCL_UI_NAV = 'download_mod'
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_WINDOW = $size
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '18'
  $env:SXCL_UI_SHOT = (Join-Path $work ('shot_' + $tag + '.png'))
  $env:SXCL_UI_SHOT_DELAY = '3500'
  Remove-Item Env:SXCL_UI_MODS_QUERY -ErrorAction SilentlyContinue
  Remove-Item Env:SXCL_UI_MODS_SOURCES -ErrorAction SilentlyContinue
  $out = Join-Path $work ('dump_' + $tag + '.txt')
  $err = $out + '.err'
  Remove-Item $out,$err -ErrorAction SilentlyContinue
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  return @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) +
         @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
}

foreach ($case in @(@('900x600',$true), @('1100x750',$true), @('1280x800',$true), @('700x560',$false))) {
  $size = $case[0]
  $isWide = $case[1]
  Write-Output ''
  Write-Output ('== window ' + $size + ' ==')
  $lines = Invoke-Fit ('w' + $size) $size

  # the filter row container (the mods pane is printed twice: one copy is hidden)
  $rowLines = @($lines | Where-Object { $_ -match '#modsFilterRow \(' })
  Ok ($rowLines.Count -ge 1) ('the filter row container is in the dump (' + $rowLines.Count + ' copies)')
  # the hidden pane is never laid out at all (its container stays at a placeholder size), so the
  # geometry comparisons only make sense on the copy the user is looking at.
  $rowVisible = $null
  foreach ($row in $rowLines) { if ($row -notmatch ' hidden') { $rowVisible = $row; break } }
  Ok ($rowVisible -ne $null) 'the visible filter row is in the dump'

  $cut = 0
  $tooSmall = 0
  $reported = 0
  foreach ($name in $names) {
    $hits = @($lines | Where-Object { $_ -match ('#' + $name + ' \(') })
    if ($hits.Count -eq 0) { Write-Output ('  -> FAIL no widget #' + $name); $fail++; $total++; continue }
    foreach ($line in $hits) {
      $text = $line.Trim()
      $hidden = ($line -match ' hidden')
      $geo = [regex]::Match($line, '\((\d+),(\d+) (\d+)x(\d+)\)')
      $wx = [int]$geo.Groups[1].Value; $wy = [int]$geo.Groups[2].Value
      $ww = [int]$geo.Groups[3].Value; $wh = [int]$geo.Groups[4].Value
      $needW = 0; $needH = 0; $gotW = 0; $gotH = 0
      $met = [regex]::Match($line, 'need=(\d+)x(\d+) got=(\d+)x(\d+)')
      if ($met.Success) {
        $needW = [int]$met.Groups[1].Value; $needH = [int]$met.Groups[2].Value
        $gotW = [int]$met.Groups[3].Value; $gotH = [int]$met.Groups[4].Value
      }
      $tag = 'visible'
      if ($hidden) { $tag = 'hidden-copy' }
      if ($met.Success) {
        Write-Output ('  #' + $name.PadRight(22) + ' [' + $tag + '] (' + $wx + ',' + $wy + ' ' + $ww + 'x' + $wh + ') need=' + $needW + 'x' + $needH + ' got=' + $gotW + 'x' + $gotH)
      } else {
        Write-Output ('  #' + $name.PadRight(22) + ' [' + $tag + '] (' + $wx + ',' + $wy + ' ' + $ww + 'x' + $wh + ') (no text metrics: not a label)')
      }
      if ($line -match 'CUT-W' -or $line -match 'CUT-H') { $cut++; Write-Output ('    -> CUT marker: ' + $text) }
      if ($met.Success -and ($gotW -lt $needW -or $gotH -lt $needH)) {
        $tooSmall++
        Write-Output ('    -> smaller than it needs: got ' + $gotW + 'x' + $gotH + ' < need ' + $needW + 'x' + $needH)
      }
      if ($name -eq 'modsSearchBox') {
        if ($ww -lt 200) { Write-Output ('    -> the name box is ' + $ww + 'px wide (its own floor is 200)'); $tooSmall++ }
      }
      # no control wider than the container it lives in (visible copy only -- see above)
      if (-not $hidden -and $rowVisible -ne $null) {
        $rg = [regex]::Match($rowVisible, '\((\d+),(\d+) (\d+)x(\d+)\)')
        $rw = [int]$rg.Groups[3].Value
        if ($ww -gt $rw) { Write-Output ('    -> wider than the filter row: ' + $ww + ' > ' + $rw); $tooSmall++ }
      }
    }
  }
  Ok ($cut -eq 0) ('no CUT-W / CUT-H on any filter control (' + $cut + ' markers)')
  Ok ($tooSmall -eq 0) ('every control shows itself completely (' + $tooSmall + ' offenders)')

  # the row really wraps when the window cannot hold it
  $rowGeo = [regex]::Match(($rowVisible), '\((\d+),(\d+) (\d+)x(\d+)\)')
  $rowW = [int]$rowGeo.Groups[3].Value
  $rowH = [int]$rowGeo.Groups[4].Value
  if ($isWide) {
    Ok ($rowH -le 44) ('at ' + $size + ' the row fits on one line (row ' + $rowW + 'x' + $rowH + ')')
  } else {
    Ok ($rowH -ge 60) ('at ' + $size + ' the row wrapped instead of squeezing (row ' + $rowW + 'x' + $rowH + ')')
  }
  Write-Output ('  filter row: ' + $rowW + 'x' + $rowH)
}

Write-Output ''
Write-Output ('  product files: ' + $work)
if ($fail -eq 0) {
  Write-Output ('MODS-FIT: ALL OK (' + $total + '/' + $total + ')')
  exit 0
}
Write-Output ('MODS-FIT: FAIL (' + $fail + ' of ' + $total + ' assertions)')
exit 1
