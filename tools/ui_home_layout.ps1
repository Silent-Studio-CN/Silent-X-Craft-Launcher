# (C) Silent X Craft Launcher -- "home layout" acceptance (user 2026-09-26).
# User asked (paraphrased, ASCII): make the home page use the layout of the cancelled new UI
# (not the new UI itself), and render the premium / offline choice with the two logos that new UI
# used (the Microsoft squares and the disconnected chain).
# So the home page is now TWO COLUMNS (new layout, old widgets):
#   left  = game    (#homeGameColumn, stretch)
#   right = account (#homeAccountColumn, FIXED 300 logical px)
# and the premium/offline picker is those two logos:
#   #homeEditionPremiumButton = assets/icons/ui/microsoft.svg    (official four colours, NEVER tinted)
#   #homeEditionOfflineButton = assets/icons/ui/disconnected.svg (monochrome line art, token-tinted)
# What it proves, per run (all numbers measured here, nothing guessed):
#   1) the two columns really are two columns: account column is exactly 300 wide, both columns start
#      at the same y, the gap is 16, the left column starts at the page margin (24) and the pair
#      fills the page width (checked at 900x600 / 1100x750 / 1280x800; left column never collapses)
#   2) both logo buttons exist and are CLICKABLE through the product path (SXCL_UI_HOME_EDITION ->
#      button->click()): the selected one draws the accent indicator bar under its icon, the other
#      draws exactly 0 accent pixels (same criterion as tools/ui_edition_icons.ps1)
#   3) the Microsoft logo keeps its FOUR official colours on screen (#F25022 / #7FBA00 / #00A4EF /
#      #FFB900, each non-zero) -- i.e. it was NOT flattened into a monochrome icon
#   4) nothing was lost in the rewrite: the single hero launch button (#homeLaunchButton), the
#      offline ID field (#homeOfflineNameEdit) and the version name (#homeVersionName) are present,
#      and SXCL_UI_LAUNCH still reaches the page (it switches to offline first, then clicks launch)
#
# ASCII-only on purpose: Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM,
# so non-ASCII here would break the script (keep this file ASCII + UTF-8 BOM).
$ErrorActionPreference = 'Continue'
$exe = Join-Path $PSScriptRoot '..\build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\home_layout'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$fail = 0
$script:colFail = 0
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $exe)) {
  Write-Output ('HOME-LAYOUT: FAIL (exe not found: ' + $exe + ')')
  exit 1
}
# a leftover instance keeps running and can write the same settings file while we measure
$busy = @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
  Write-Output ('HOME-LAYOUT: FAIL (another sxcl-ui.exe is running, pid ' + ($busy.Id -join ',') +
                ' -- close it or wait, then re-run)')
  exit 1
}

$ini = Join-Path $work 'home.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $work + '\.minecraft') -Encoding utf8

function Get-Rgb([System.Drawing.Bitmap]$bmp, [int]$px, [int]$py) {
  $c = $bmp.GetPixel($px, $py)
  return @($c.R, $c.G, $c.B)
}
function Get-Hex($rgb) { return ('#{0:X2}{1:X2}{2:X2}' -f [int]$rgb[0], [int]$rgb[1], [int]$rgb[2]) }

# "#name (x,y WxH)" out of the SXCL_UI_DUMP text -> @(x,y,w,h) or $null
function Get-Rect($txt, [string]$name) {
  $hit = $txt | Select-String -Pattern ('#' + $name + ' \((\d+),(\d+) (\d+)x(\d+)\)') | Select-Object -First 1
  if (-not $hit) { return $null }
  $null = ($hit.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
  return @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
}
function Get-DumpLine($txt, [string]$name) {
  $hit = $txt | Select-String -Pattern ('#' + $name + '[ \(]') | Select-Object -First 1
  if (-not $hit) { return '' }
  return (($hit.Line -replace '\s+', ' ').Trim())
}

# selected state = the accent indicator bar UNDER the icon: IconSelectButton draws it at
# "icon bottom +1 +4 .. +2", and the button is iconH + 10 tall -> it is the last 3 logical rows.
function Get-BandAccent([string]$png, $rect, [double]$dpr) {
  $bmp = New-Object System.Drawing.Bitmap($png)
  $x0 = [int][math]::Floor($rect[0] * $dpr); $x1 = [int][math]::Ceiling(($rect[0] + $rect[2]) * $dpr)
  $y0 = [int][math]::Floor(($rect[1] + $rect[3] - 3) * $dpr)
  $y1 = [int][math]::Ceiling(($rect[1] + $rect[3]) * $dpr)
  $accent = 0; $total = 0
  for ($py = $y0; $py -lt $y1; $py++) {
    for ($px = $x0; $px -lt $x1; $px++) {
      $p = Get-Rgb $bmp $px $py
      $total++
      # accent (theme token) is blue-dominant: #4CC2FF in dark mode, #0067C0 in light mode
      if (($p[2] - $p[0]) -gt 40 -and $p[2] -gt 110) { $accent++ }
    }
  }
  $bmp.Dispose()
  return @($accent, $total)
}

# the four official Microsoft square colours, counted inside the icon box only
# (button minus 2 px pads; icon box height = button height - 10)
function Get-FourColors([string]$png, $rect, [double]$dpr) {
  $bmp = New-Object System.Drawing.Bitmap($png)
  $x0 = [int][math]::Floor(($rect[0] + 2) * $dpr); $x1 = [int][math]::Ceiling(($rect[0] + $rect[2] - 2) * $dpr)
  $y0 = [int][math]::Floor(($rect[1] + 2) * $dpr)
  $y1 = [int][math]::Ceiling(($rect[1] + 2 + ($rect[3] - 10)) * $dpr)
  $targets = @(@(0xF2, 0x50, 0x22), @(0x7F, 0xBA, 0x00), @(0x00, 0xA4, 0xEF), @(0xFF, 0xB9, 0x00))
  $counts = @(0, 0, 0, 0)
  for ($py = $y0; $py -lt $y1; $py++) {
    for ($px = $x0; $px -lt $x1; $px++) {
      $p = Get-Rgb $bmp $px $py
      for ($i = 0; $i -lt 4; $i++) {
        $d = [math]::Abs($p[0] - $targets[$i][0]) + [math]::Abs($p[1] - $targets[$i][1]) +
             [math]::Abs($p[2] - $targets[$i][2])
        if ($d -le 60) { $counts[$i]++ }
      }
    }
  }
  $bmp.Dispose()
  return $counts
}

# $pick = 'premium' / 'offline' clicks that logo; '' = no click at all (default stays offline)
function Invoke-HomeRun([string]$tag, [string]$window, [string]$pick, [int]$shotDelay) {
  $env:SXCL_UI_ROUTE = 'home'
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_WINDOW = $window
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '10'
  if ($pick -ne '') {
    $env:SXCL_UI_HOME_EDITION = $pick
  } else {
    Remove-Item Env:SXCL_UI_HOME_EDITION -ErrorAction SilentlyContinue
  }
  $png = Join-Path $work ('home_' + $tag + '.png')
  Remove-Item $png -ErrorAction SilentlyContinue
  $env:SXCL_UI_SHOT = $png
  $env:SXCL_UI_SHOT_DELAY = '' + $shotDelay
  $out = Join-Path $work ('dump_' + $tag + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $txt = @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) +
         @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  $shotW = 0; $shotH = 0; $dpr = 1.0
  if (Test-Path $png) {
    $bmpInfo = New-Object System.Drawing.Bitmap($png)
    $shotW = $bmpInfo.Width; $shotH = $bmpInfo.Height
    $bmpInfo.Dispose()
    $logicalW = [int]($window.Split('x')[0])
    $dpr = [math]::Round($shotW / [double]$logicalW, 2)
  }
  return @{ tag = $tag; txt = $txt; png = $png; shot = @($shotW, $shotH); dpr = $dpr }
}

function Test-Columns([string]$tag, $run, [string]$window) {
  $hp = Get-Rect $run.txt 'HomePage'
  $gc = Get-Rect $run.txt 'homeGameColumn'
  $ac = Get-Rect $run.txt 'homeAccountColumn'
  Write-Output ('  [' + $tag + '] window ' + $window + ' shot ' + ($run.shot -join 'x') + ' dpr=' + $run.dpr)
  $missing = 0
  foreach ($pair in @(@('HomePage', $hp), @('homeGameColumn', $gc), @('homeAccountColumn', $ac))) {
    if ($pair[1] -eq $null) {
      Write-Output ('    -> FAIL ' + $pair[0] + ' not found in dump')
      $missing++
    } else {
      Write-Output ('    #' + $pair[0] + ' (' + ($pair[1] -join ',') + ')')
    }
  }
  if ($missing -gt 0) { $script:colFail += $missing; return }
  if ($ac[2] -ne 300) {
    Write-Output ('    -> FAIL account column width=' + $ac[2] + ' logical px, expected exactly 300')
    $script:colFail++
  }
  if ($ac[1] -ne $gc[1]) {
    Write-Output ('    -> FAIL the two columns do not start at the same y: game y=' + $gc[1] + ' account y=' + $ac[1])
    $script:colFail++
  }
  $gap = $ac[0] - ($gc[0] + $gc[2])
  if ($gap -ne 16) {
    Write-Output ('    -> FAIL column gap=' + $gap + ' logical px, expected 16')
    $script:colFail++
  }
  $margin = $gc[0] - $hp[0]
  if ($margin -lt 22 -or $margin -gt 26) {
    Write-Output ('    -> FAIL left column does not start at the page margin: margin=' + $margin + ' (expected 24)')
    $script:colFail++
  }
  $rightSlack = ($hp[0] + $hp[2]) - ($ac[0] + $ac[2])
  if ($rightSlack -lt 22 -or $rightSlack -gt 26) {
    Write-Output ('    -> FAIL the column pair does not fill the page width: right slack=' + $rightSlack + ' (expected 24)')
    $script:colFail++
  }
  if ($gc[2] -lt 200) {
    Write-Output ('    -> FAIL the left (game) column is squeezed to ' + $gc[2] + ' logical px')
    $script:colFail++
  }
  Write-Output ('    columns OK: game=' + $gc[2] + ' wide at x=' + $gc[0] + ', account=300 wide at x=' + $ac[0] +
                ', gap=' + $gap + ', margins L=' + $margin + ' R=' + $rightSlack)
}

# ---- run A: default state (the user's call: offline is the default) ----
Write-Output 'run A: default (no click) at 1100x750'
$runA = Invoke-HomeRun 'default' '1100x750' '' 5000
Test-Columns 'default' $runA '1100x750'

$rfA = 0
$launchA = Get-Rect $runA.txt 'homeLaunchButton'
$nameA = Get-Rect $runA.txt 'homeVersionName'
$offEditA = Get-Rect $runA.txt 'homeOfflineNameEdit'
Write-Output ('  function check: launch=' + $(if ($launchA) { $launchA -join ',' } else { 'MISSING' }) +
              ' versionName=' + $(if ($nameA) { $nameA -join ',' } else { 'MISSING' }) +
              ' offlineId=' + $(if ($offEditA) { $offEditA -join ',' } else { 'MISSING' }))
if ($launchA -eq $null -or $nameA -eq $null -or $offEditA -eq $null) {
  Write-Output '    -> FAIL the rewrite lost one of: hero launch button / version name / offline ID field'
  $rfA++
}
Write-Output ('  dump: ' + (Get-DumpLine $runA.txt 'homeEditionPremiumButton'))
Write-Output ('        ' + (Get-DumpLine $runA.txt 'homeEditionOfflineButton'))
$premA = Get-Rect $runA.txt 'homeEditionPremiumButton'
$offlA = Get-Rect $runA.txt 'homeEditionOfflineButton'
$bandPremA = @(0, 0); $bandOfflA = @(0, 0); $fourA = @(0, 0, 0, 0)
if ($premA -eq $null -or $offlA -eq $null) {
  Write-Output '    -> FAIL one of the two edition logos is missing from the dump'
  $rfA++
} else {
  $bandPremA = Get-BandAccent $runA.png $premA $runA.dpr
  $bandOfflA = Get-BandAccent $runA.png $offlA $runA.dpr
  $fourA = Get-FourColors $runA.png $premA $runA.dpr
  Write-Output ('    logo rects: microsoft=' + ($premA -join ',') + ' disconnected=' + ($offlA -join ',') +
                ' (icon height 20 logical)')
  Write-Output ('    accent bar under icon: premium=' + $bandPremA[0] + '/' + $bandPremA[1] +
                ' offline=' + $bandOfflA[0] + '/' + $bandOfflA[1] + '  (default must be OFFLINE)')
  Write-Output ('    microsoft four colours: #F25022=' + $fourA[0] + ' #7FBA00=' + $fourA[1] +
                ' #00A4EF=' + $fourA[2] + ' #FFB900=' + $fourA[3])
  if ($bandOfflA[0] -lt 20) {
    Write-Output ('    -> FAIL the default state does not highlight offline (accent px=' + $bandOfflA[0] + ', threshold 20)')
    $rfA++
  }
  if ($bandPremA[0] -ne 0) {
    Write-Output ('    -> FAIL premium is also highlighted in the default state (accent px=' + $bandPremA[0] + ')')
    $rfA++
  }
  foreach ($i in 0..3) {
    if ($fourA[$i] -lt 6) {
      Write-Output ('    -> FAIL microsoft colour #' + $i + ' has only ' + $fourA[$i] +
                    ' px -- the logo is not drawn in its official four colours')
      $rfA++
    }
  }
}

# ---- run B: click the premium (Microsoft) logo ----
Write-Output 'run B: click the microsoft logo (SXCL_UI_HOME_EDITION=premium)'
$runB = Invoke-HomeRun 'premium' '1100x750' 'premium' 5000
Test-Columns 'premium' $runB '1100x750'
$rfB = 0
$premB = Get-Rect $runB.txt 'homeEditionPremiumButton'
$offlB = Get-Rect $runB.txt 'homeEditionOfflineButton'
$bandPremB = @(0, 0); $bandOfflB = @(0, 0); $fourB = @(0, 0, 0, 0)
$clickB = $runB.txt | Select-String -Pattern 'SXCL_UI_HOME_EDITION=premium' | Select-Object -First 1
if (-not $clickB) {
  Write-Output '    -> FAIL the click log line is missing (the logo was not reached via SXCL_UI_HOME_EDITION)'
  $rfB++
} else {
  Write-Output ('    ' + (($clickB.Line -replace '\s+', ' ').Trim()))
  if ((($clickB.Line -replace '\s+', ' ')) -notmatch 'premium=1 offline=0') {
    Write-Output '    -> FAIL after clicking the microsoft logo the pair is not "premium=1 offline=0"'
    $rfB++
  }
}
if ($premB -eq $null -or $offlB -eq $null) {
  Write-Output '    -> FAIL one of the two edition logos is missing from the dump'
  $rfB++
} else {
  $bandPremB = Get-BandAccent $runB.png $premB $runB.dpr
  $bandOfflB = Get-BandAccent $runB.png $offlB $runB.dpr
  $fourB = Get-FourColors $runB.png $premB $runB.dpr
  Write-Output ('    accent bar under icon: premium=' + $bandPremB[0] + '/' + $bandPremB[1] +
                ' offline=' + $bandOfflB[0] + '/' + $bandOfflB[1])
  Write-Output ('    microsoft four colours: #F25022=' + $fourB[0] + ' #7FBA00=' + $fourB[1] +
                ' #00A4EF=' + $fourB[2] + ' #FFB900=' + $fourB[3])
  if ($bandPremB[0] -lt 20) {
    Write-Output ('    -> FAIL the clicked logo is not highlighted (accent px=' + $bandPremB[0] + ', threshold 20)')
    $rfB++
  }
  if ($bandOfflB[0] -ne 0) {
    Write-Output ('    -> FAIL offline is still highlighted after clicking premium (accent px=' + $bandOfflB[0] + ')')
    $rfB++
  }
  foreach ($i in 0..3) {
    if ($fourB[$i] -lt 6) {
      Write-Output ('    -> FAIL microsoft colour #' + $i + ' has only ' + $fourB[$i] + ' px in the selected state')
      $rfB++
    }
  }
}
if ($premA -ne $null -and $offlB -ne $null) {
  Write-Output ('  cross-run highlight: premium band A=' + $bandPremA[0] + ' B=' + $bandPremB[0] +
                ' | offline band A=' + $bandOfflA[0] + ' B=' + $bandOfflB[0])
  if ($bandPremA[0] -eq $bandPremB[0]) { Write-Output '  -> FAIL premium highlight did not change on click'; $rfB++ }
}

# ---- runs C/D: the layout must hold at the other two window sizes ----
Write-Output 'run C: 900x600 (no click)'
$runC = Invoke-HomeRun 'small' '900x600' '' 3500
Test-Columns 'small' $runC '900x600'
Write-Output 'run D: 1280x800 (no click)'
$runD = Invoke-HomeRun 'large' '1280x800' '' 3500
Test-Columns 'large' $runD '1280x800'

# ---- run E: the launch chain hook must still reach the page ----
Write-Output 'run E: SXCL_UI_LAUNCH still reaches the home page'
$env:SXCL_UI_ROUTE = 'home'
$env:SXCL_UI_SETTINGS = $ini
$env:SXCL_UI_WINDOW = '1100x750'
$env:SXCL_UI_DUMP = '1'
$env:SXCL_UI_DUMP_DEPTH = '10'
$env:SXCL_UI_LAUNCH = '1'
$env:SXCL_UI_LAUNCH_DELAY = '2500'
$env:SXCL_UI_SHOT = (Join-Path $work 'launch.png')
$env:SXCL_UI_SHOT_DELAY = '5000'
Remove-Item Env:SXCL_UI_HOME_EDITION -ErrorAction SilentlyContinue
$outE = Join-Path $work 'dump_launch.txt'
Start-Process -FilePath $exe -RedirectStandardOutput $outE -RedirectStandardError ($outE + '.err') -Wait | Out-Null
$txtE = @(Get-Content $outE -Encoding utf8 -ErrorAction SilentlyContinue) +
        @(Get-Content ($outE + '.err') -Encoding utf8 -ErrorAction SilentlyContinue)
$edge = $txtE | Select-String -Pattern 'home-edition switched to offline|home-edition offline logo not found' | Select-Object -First 1
$hitE = $txtE | Select-String -Pattern 'home-launch: clicked|home-launch: button not found' | Select-Object -First 1
if ($edge) { Write-Output ('  ' + (($edge.Line -replace '\s+', ' ').Trim())) } else { Write-Output '  (no home-edition line)' }
if ($hitE) { Write-Output ('  ' + (($hitE.Line -replace '\s+', ' ').Trim())) } else { Write-Output '  (no home-launch line)' }
if (-not $edge -or -not $hitE) {
  Write-Output '  -> FAIL the SXCL_UI_LAUNCH path no longer reaches the home page buttons'
  $fail++
} else {
  if ((($edge.Line -replace '\s+', ' ')) -notmatch 'switched to offline') {
    Write-Output '  -> FAIL SXCL_UI_LAUNCH did not switch the page to the offline logo first'
    $fail++
  }
  if ((($hitE.Line -replace '\s+', ' ')) -notmatch 'home-launch: clicked') {
    Write-Output '  -> FAIL SXCL_UI_LAUNCH did not reach the launch button'
    $fail++
  }
}
Remove-Item Env:SXCL_UI_LAUNCH -ErrorAction SilentlyContinue

$fail += $rfA + $rfB + $script:colFail

Write-Output 'home layout = two columns (game / account 300); logos from assets/icons/ui (see NOTICE.md)'
if ($fail -eq 0) {
  Write-Output 'HOME-LAYOUT: ALL OK (4/4)'
} else {
  Write-Output ('HOME-LAYOUT: ' + $fail + ' FAILED')
  exit 1
}
