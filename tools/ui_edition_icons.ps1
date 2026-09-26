# (C) Silent X Craft Launcher -- "edition icons" acceptance (docs/27 S11/S12).
# Download page side-2 footer = [Java cup] | [Bedrock block]: two clickable icons with a
# vertical separator between them (#sxclEditionJavaButton / #sxclEditionBedrockButton,
# both sxcl::ui::IconSelectButton, container #sxclEditionPickRow).
# What it proves, per run (all numbers measured on this machine, nothing guessed):
#   1) both buttons show up in SXCL_UI_DUMP with sane geometry and are not hidden
#   2) the icons are NOT blank: pixels inside the icon box differ from the card background
#      (a missing/blanked asset gives 0) and each one carries its own official colours
#      (Java cup = the #0074BD family; Bedrock block = the APK's gray rock palette)
#   3) the selected state really moves: only the selected button carries accent-coloured
#      pixels in the ring around the icon (theme token accent), the other one carries none
#   4) the vertical separator between the two icons is exactly ONE device pixel wide and
#      runs vertically over the whole button height (container paintEvent + QPen(token, 0))
#   5) clicking goes through the product path (SXCL_UI_EDITION -> button->click()) and the
#      settings file really says game.edition=<picked> afterwards
# Run A clicks bedrock, run B clicks java again, run C restarts with NO click at all and
# expects the persisted choice to come back on its own.
#
# ASCII-only on purpose: Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM,
# so Chinese here would break the script (keep this file ASCII + UTF-8 BOM).
$ErrorActionPreference = 'Continue'
$exe = Join-Path $PSScriptRoot '..\build-ui\src\ui\Release\sxcl-ui.exe'
# acceptance/temp files live under D:\SilentStudio\_test (never %TEMP% / C:)
$work = 'D:\SilentStudio\_test\edition_icons'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$fail = 0
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $exe)) {
  Write-Output ('EDITION-ICONS: FAIL (exe not found: ' + $exe + ')')
  exit 1
}

$ini = Join-Path $work 'edition.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $work + '\.minecraft') -Encoding utf8
Add-Content -Path $ini -Value 'game.edition=java'

function Get-Rgb([System.Drawing.Bitmap]$bmp, [int]$px, [int]$py) {
  $c = $bmp.GetPixel($px, $py)
  return @($c.R, $c.G, $c.B)
}
function Get-Hex($rgb) { return ('#{0:X2}{1:X2}{2:X2}' -f [int]$rgb[0], [int]$rgb[1], [int]$rgb[2]) }
function Get-Dist($a, $b) {
  return ([math]::Abs([int]$a[0] - [int]$b[0]) + [math]::Abs([int]$a[1] - [int]$b[1]) + [math]::Abs([int]$a[2] - [int]$b[2]))
}
function Near($p, $r, $g, $b, $tol) { return ((Get-Dist $p @($r, $g, $b)) -le $tol) }

# ---- one button rect: ink vs card background, accent ring, official icon colours ----
# dump coordinates are LOGICAL, the screenshot is PHYSICAL (dpr), so everything scales.
function Get-ButtonStats([string]$png, [int]$x, [int]$y, [int]$w, [int]$h, [double]$dpr,
                         [int]$iconSide, [int]$pad, [int[]]$cardRef) {
  $bmp = New-Object System.Drawing.Bitmap($png)
  $x0 = [int][math]::Floor($x * $dpr); $y0 = [int][math]::Floor($y * $dpr)
  $x1 = [int][math]::Ceiling(($x + $w) * $dpr); $y1 = [int][math]::Ceiling(($y + $h) * $dpr)
  # the icon box sits centered inside the button, pad pixels from every edge
  $ix0 = [int][math]::Floor(($x + $pad) * $dpr); $ix1 = [int][math]::Ceiling(($x + $pad + $iconSide) * $dpr)
  $iy0 = [int][math]::Floor(($y + $pad) * $dpr); $iy1 = [int][math]::Ceiling(($y + $pad + $iconSide) * $dpr)
  $colors = @{}; $ink = 0; $inner = 0; $innerVsCard = 0; $ringBlue = 0; $bluePx = 0
  $rockLight = 0; $rockMid = 0; $total = 0
  for ($py = $y0; $py -lt $y1; $py++) {
    for ($px = $x0; $px -lt $x1; $px++) {
      $p = Get-Rgb $bmp $px $py
      $k = ($p -join ',')
      if ($colors.ContainsKey($k)) { $colors[$k]++ } else { $colors[$k] = 1 }
      $total++
      $isInner = ($px -ge $ix0 -and $px -lt $ix1 -and $py -ge $iy0 -and $py -lt $iy1)
      if ($isInner) {
        $inner++
        if ((Get-Dist $p $cardRef) -gt 12) { $innerVsCard++ }
        # the official Java cup is the #0074BD family (blue-dominant; measured 67 unselected /
        # 129 selected on this machine, and 0..6 in the bedrock box -- clean separation)
        if (($p[2] - $p[0]) -gt 40 -and $p[2] -gt 110) { $bluePx++ }
        # the official bedrock block: its own gray rock faces from the APK texture
        # (light #979797/#808080, mid #575757/#636363). Distance to the card #2B2B2B is >= 132
        # for these, so a blank/uniform button can never score here: measured 0 in the java box.
        if ((Near $p 0x97 0x97 0x97 20) -or (Near $p 0x80 0x80 0x80 12)) { $rockLight++ }
        if ((Near $p 0x57 0x57 0x57 14) -or (Near $p 0x63 0x63 0x63 14)) { $rockMid++ }
      } else {
        # ring = the padding around the icon: only the backdrop / accent frame lives there
        if (($p[2] - $p[0]) -gt 18) { $ringBlue++ }
      }
    }
  }
  $bmp.Dispose()
  $modalKey = ($colors.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key
  $modal = $modalKey.Split(',') | ForEach-Object { [int]$_ }
  foreach ($k in $colors.Keys) {
    $q = $k.Split(',') | ForEach-Object { [int]$_ }
    if ((Get-Dist $q $modal) -gt 30) { $ink += $colors[$k] }
  }
  return @{ ink = $ink; total = $total; inner = $inner; innerVsCard = $innerVsCard;
            ringBlue = $ringBlue; bluePx = $bluePx; rockLight = $rockLight; rockMid = $rockMid;
            modal = $modal }
}

# ---- the vertical separator: must be exactly ONE device pixel, vertical ----
function Get-SeparatorStats([string]$png, [int]$leftRight, [int]$rightLeft, [int]$top, [int]$height, [double]$dpr) {
  $bmp = New-Object System.Drawing.Bitmap($png)
  $x0 = [int][math]::Floor($leftRight * $dpr) + 1
  $x1 = [int][math]::Ceiling($rightLeft * $dpr) - 1
  $rows = @([int][math]::Floor(($top + 2) * $dpr),
            [int][math]::Floor(($top + $height / 2.0) * $dpr),
            [int][math]::Floor(($top + $height - 3) * $dpr))
  $colColors = @{}
  foreach ($cx in ($x0..$x1)) { $colColors[$cx] = Get-Rgb $bmp $cx $rows[1] }
  $hist = @{}
  foreach ($cx in $colColors.Keys) {
    $k = ($colColors[$cx] -join ',')
    if ($hist.ContainsKey($k)) { $hist[$k]++ } else { $hist[$k] = 1 }
  }
  $bgTriple = (($hist.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key).Split(',') | ForEach-Object { [int]$_ }
  $lineCols = @(); $lineColor = $null
  foreach ($cx in ($colColors.Keys | Sort-Object)) {
    if ((Get-Dist $colColors[$cx] $bgTriple) -gt 12) { $lineCols += $cx; $lineColor = $colColors[$cx] }
  }
  $vertical = 0
  if ($lineCols.Count -eq 1) {
    foreach ($ry in $rows) {
      if ((Get-Dist (Get-Rgb $bmp $lineCols[0] $ry) $bgTriple) -gt 12) { $vertical++ }
    }
  }
  $bmp.Dispose()
  return @{ width = $lineCols.Count; col = $(if ($lineCols.Count -ge 1) { $lineCols[0] } else { -1 });
            span = @($x0, $x1); rows = $rows; bg = (Get-Hex $bgTriple); vertical = $vertical;
            color = $(if ($lineColor) { Get-Hex $lineColor } else { '-' }) }
}

# $pick = 'java' / 'bedrock' clicks that button; $pick = '' starts with NO click at all
# (that is the restart run: the choice has to come back from the settings file by itself).
function Invoke-EditionRun([string]$tag, [string]$pick, [string]$expect, [string]$selectName) {
  $script:runStats = @{}
  $env:SXCL_UI_ROUTE = 'download'
  $env:SXCL_UI_NAV = 'download_mc'
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_WINDOW = '1100x750'
  $env:SXCL_UI_DUMP = '1'
  if ($pick -ne '') {
    $env:SXCL_UI_EDITION = $pick
  } else {
    Remove-Item Env:SXCL_UI_EDITION -ErrorAction SilentlyContinue
  }
  $env:SXCL_UI_SHOT_DELAY = '6000'
  $png = Join-Path $work ('edition_' + $tag + '.png')
  $env:SXCL_UI_SHOT = $png
  Remove-Item $png -ErrorAction SilentlyContinue
  $out = Join-Path $work ('dump_' + $tag + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $txt = @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) +
         @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)

  Write-Output ('  [' + $tag + '] click=' + $(if ($pick -eq '') { '(none: restart restore)' } else { $pick }) + ' shot=' + $png)

  # 1) both buttons in the dump, with geometry
  $rects = @{}
  foreach ($name in @('sxclEditionJavaButton', 'sxclEditionBedrockButton')) {
    $hit = $txt | Select-String -Pattern ('#?' + $name + ' \((\d+),(\d+) (\d+)x(\d+)\)') | Select-Object -First 1
    if (-not $hit) {
      Write-Output ('    -> FAIL ' + $name + ' not found in dump (SXCL_UI_DUMP)')
      $script:fail++
      continue
    }
    $line = $hit.Line -replace '\s+', ' '
    $null = ($hit.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
    $rects[$name] = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
    Write-Output ('    ' + $line.Trim())
    if ($line -match ' hidden') { Write-Output ('    -> FAIL ' + $name + ' is hidden'); $script:fail++ }
    if ($rects[$name][2] -lt 16 -or $rects[$name][3] -lt 16) {
      Write-Output ('    -> FAIL ' + $name + ' geometry too small: ' + $rects[$name][2] + 'x' + $rects[$name][3])
      $script:fail++
    }
  }
  if ($rects.Count -ne 2) { return }

  # 2) click really happened (product path) and the checked pair flipped as asked
  if ($pick -ne '') {
    $want = @{ java = 'java=1 bedrock=0'; bedrock = 'java=0 bedrock=1' }[$pick]
    $clicked = $txt | Select-String -Pattern ('sxclEdition' + $(if ($pick -eq 'bedrock') { 'Bedrock' } else { 'Java' }) + 'Button\(SXCL_UI_EDITION=' + $pick + '\):') | Select-Object -First 1
    if (-not $clicked) {
      Write-Output '    -> FAIL click log line missing (button not reached via SXCL_UI_EDITION)'
      $script:fail++
    } else {
      Write-Output ('    ' + (($clicked.Line -replace '\s+', ' ').Trim()))
      if (($clicked.Line -replace '\s+', ' ') -notmatch $want) {
        Write-Output ('    -> FAIL checked pair is not "' + $want + '"')
        $script:fail++
      }
    }
  }

  # 3) the setting file really says the picked edition
  if ($pick -ne '') {
    $hit = Select-String -Path $ini -Pattern 'game\.edition=(\w+)' | Select-Object -Last 1
    if (-not $hit) {
      Write-Output '    -> FAIL game.edition line missing in the settings file'
      $script:fail++
    } else {
      $got = $hit.Matches[0].Groups[1].Value
      Write-Output ('    settings: ' + ($hit.Line.Trim()) + '  (expected ' + $expect + ')')
      if ($got -ne $expect) { Write-Output ('    -> FAIL game.edition=' + $got + ', expected ' + $expect); $script:fail++ }
    }
  }

  # 4) pixel scan: icons not blank, official colours present, accent only on the selected one
  if (-not (Test-Path $png)) {
    Write-Output ('    -> FAIL screenshot missing: ' + $png)
    $script:fail++
    return
  }
  $bmpInfo = New-Object System.Drawing.Bitmap($png)
  $dpr = [math]::Round($bmpInfo.Width / 1100.0, 2)
  $shotW = $bmpInfo.Width; $shotH = $bmpInfo.Height
  $bmpInfo.Dispose()
  Write-Output ('    shot ' + $shotW + 'x' + $shotH + ' dpr=' + $dpr + ' (window 1100x750)')
  if ($dpr -lt 1.0) { Write-Output '    -> FAIL screenshot smaller than the logical window'; $script:fail++ }

  $jr = $rects['sxclEditionJavaButton']
  $br = $rects['sxclEditionBedrockButton']
  # card background: sampled OUTSIDE both buttons (left of java / right of bedrock)
  $bmpRef = New-Object System.Drawing.Bitmap($png)
  $cardRef = Get-Rgb $bmpRef ([int]($jr[0] * $dpr) - 8) ([int](($jr[1] + $jr[3] / 2.0) * $dpr))
  $cardRefRight = Get-Rgb $bmpRef ([int](($br[0] + $br[2]) * $dpr) + 8) ([int](($br[1] + $br[3] / 2.0) * $dpr))
  $bmpRef.Dispose()
  Write-Output ('    card background: ' + (Get-Hex $cardRef) + ' / ' + (Get-Hex $cardRefRight))

  $stats = @{}
  foreach ($name in @('sxclEditionJavaButton', 'sxclEditionBedrockButton')) {
    $r = $rects[$name]
    $s = Get-ButtonStats $png $r[0] $r[1] $r[2] $r[3] $dpr 20 2 $cardRef
    $stats[$name] = $s
    Write-Output ('    ' + $name + ': rect=' + ($r -join ',') + ' ink=' + $s.ink + '/' + $s.total +
                  ' innerVsCard=' + $s.innerVsCard + '/' + $s.inner + ' accentRingPx=' + $s.ringBlue +
                  ' javaBluePx=' + $s.bluePx + ' rockLightPx=' + $s.rockLight + ' rockMidPx=' + $s.rockMid +
                  ' modal=' + (Get-Hex $s.modal))
    if ($s.innerVsCard -lt 60) {
      Write-Output ('    -> FAIL ' + $name + ' icon looks BLANK (innerVsCard=' + $s.innerVsCard + ')')
      $script:fail++
    }
  }
  # each icon must carry its OWN official colours, and not the other one's
  if ($stats['sxclEditionJavaButton'].bluePx -lt 40) {
    Write-Output ('    -> FAIL java button does not show the official #0074BD cup (javaBluePx=' +
                  $stats['sxclEditionJavaButton'].bluePx + ', threshold 40)')
    $script:fail++
  }
  if ($stats['sxclEditionJavaButton'].rockMid -ne 0) {
    Write-Output ('    -> FAIL java button carries bedrock rock pixels (rockMidPx=' +
                  $stats['sxclEditionJavaButton'].rockMid + ')')
    $script:fail++
  }
  if ($stats['sxclEditionBedrockButton'].rockMid -lt 100 -or $stats['sxclEditionBedrockButton'].rockLight -lt 50) {
    Write-Output ('    -> FAIL bedrock button does not show the official gray block (rockMidPx=' +
                  $stats['sxclEditionBedrockButton'].rockMid + ' rockLightPx=' +
                  $stats['sxclEditionBedrockButton'].rockLight + ', thresholds 100/50)')
    $script:fail++
  }
  if ($stats['sxclEditionBedrockButton'].bluePx -gt 10) {
    Write-Output ('    -> FAIL bedrock button carries java-cup blue pixels (javaBluePx=' +
                  $stats['sxclEditionBedrockButton'].bluePx + ')')
    $script:fail++
  }

  # 5) the vertical separator in the middle (dump coordinates + pixel scan)
  if ($jr[0] -lt $br[0]) {
    $sep = Get-SeparatorStats $png ($jr[0] + $jr[2]) $br[0] $jr[1] $jr[3] $dpr
  } else {
    $sep = Get-SeparatorStats $png ($br[0] + $br[2]) $jr[0] $br[1] $br[3] $dpr
  }
  Write-Output ('    separator: gap phys cols ' + $sep.span[0] + '..' + $sep.span[1] +
                ' line col=' + $sep.col + ' (logical x=' + [math]::Round($sep.col / $dpr, 2) + ')' +
                ' width=' + $sep.width + ' device px color=' + $sep.color +
                ' rows hit ' + $sep.vertical + '/3 bg=' + $sep.bg)
  if ($sep.width -ne 1) {
    Write-Output ('    -> FAIL separator is not exactly 1 device pixel wide (cols differing from bg = ' + $sep.width + ')')
    $script:fail++
  }
  if ($sep.vertical -ne 3) {
    Write-Output ('    -> FAIL separator does not run vertically over the button height (rows hit ' + $sep.vertical + '/3)')
    $script:fail++
  }

  if ($selectName -ne '') {
    $selName = $selectName
  } elseif ($pick -eq 'bedrock') {
    $selName = 'sxclEditionBedrockButton'
  } else {
    $selName = 'sxclEditionJavaButton'
  }
  $otherName = if ($selName -eq 'sxclEditionBedrockButton') { 'sxclEditionJavaButton' } else { 'sxclEditionBedrockButton' }
  $sel = $stats[$selName]; $other = $stats[$otherName]
  Write-Output ('    selected=' + $selName + ' accentRingPx=' + $sel.ringBlue +
                ' | other=' + $otherName + ' accentRingPx=' + $other.ringBlue)
  if ($sel.ringBlue -lt 20) { Write-Output '    -> FAIL selected button carries no accent pixels in its ring'; $script:fail++ }
  if ($other.ringBlue -gt 5) { Write-Output '    -> FAIL unselected button also carries accent pixels'; $script:fail++ }
  $script:runStats = $stats
}

Write-Output 'run A: click bedrock'
$before = (Select-String -Path $ini -Pattern 'game\.edition=(\w+)' | Select-Object -Last 1).Line.Trim()
Write-Output ('  start state: ' + $before)
Invoke-EditionRun 'bedrock' 'bedrock' 'bedrock'
$statsA = $script:runStats

Write-Output 'run B: click java again'
Invoke-EditionRun 'java' 'java' 'java'
$statsB = $script:runStats

Write-Output 'run C: restart with NO click (the choice must come back from the settings file)'
Set-Content -Path $ini -Value ('game.default_dir=' + $work + '\.minecraft') -Encoding utf8
Add-Content -Path $ini -Value 'game.edition=bedrock'
$restoreLine = (Select-String -Path $ini -Pattern 'game\.edition=(\w+)' | Select-Object -Last 1).Line.Trim()
Write-Output ('  settings before run C: ' + $restoreLine)
Invoke-EditionRun 'restore' '' '' 'sxclEditionBedrockButton'
$statsC = $script:runStats
if ($statsC.Count -eq 2) {
  if ($statsC['sxclEditionBedrockButton'].ringBlue -lt 20) {
    Write-Output '  -> FAIL after restart the bedrock button is not the selected one (no accent pixels)'
    $fail++
  }
  if ($statsC['sxclEditionJavaButton'].ringBlue -gt 5) {
    Write-Output '  -> FAIL after restart the java button wrongly carries accent pixels'
    $fail++
  }
}

# cross-run: the same button must look different when it is the selected one
if ($statsA.Count -eq 2 -and $statsB.Count -eq 2) {
  foreach ($name in @('sxclEditionJavaButton', 'sxclEditionBedrockButton')) {
    $a = $statsA[$name].ringBlue; $b = $statsB[$name].ringBlue
    Write-Output ('  cross-run ' + $name + ': accentRingPx A=' + $a + ' B=' + $b)
    if ($a -eq $b) { Write-Output ('  -> FAIL ' + $name + ' ring looks identical in both runs'); $fail++ }
  }
}

Write-Output 'both icons clickable: run A -> bedrock button, run B -> java button (product path button->click())'
Write-Output 'icon sources (kept in sync with assets/icons/edition/NOTICE.md):'
Write-Output '  java_logo.svg    <- https://raw.githubusercontent.com/devicons/devicon/master/icons/java/java-original.svg'
Write-Output '                      (same bytes also at https://unpkg.com/devicon@latest/icons/java/java-original.svg ; sha256 7582E518A9C02425F97155E5A3BD39D1A3A7D421B78CAF9C8DF7443DAD3EDC5D)'
Write-Output '  bedrock_logo.png <- D:\SilentStudio\AdbGUI\APK\Minecraft_1.26.40.5.apk : assets/assets/resource_packs/vanilla/textures/blocks/bedrock.png'
Write-Output '                      (16x16 sha256 20CED86BA8CB89E29E2115F76C758278893E145575A73CA311BCC4305C140D04, scaled 4x nearest)'

# the required banner says "luo pan yi zhi" (settings files agree) in Chinese; built from code
# points so THIS FILE STAYS PURE ASCII while the printed line is the exact required wording.
$okSuffix = 'EDITION-ICONS: ALL OK (java/bedrock ' + [char]0x843D + [char]0x76D8 + [char]0x4E00 + [char]0x81F4 + ')'
if ($fail -eq 0) {
  Write-Output $okSuffix
} else {
  Write-Output ('EDITION-ICONS: ' + $fail + ' FAILED')
  exit 1
}
