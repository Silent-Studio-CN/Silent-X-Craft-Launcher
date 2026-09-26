# (C) Silent X Craft Launcher -- "edition icons" acceptance (docs/27 S11/S12).
# Download page side-2 footer = [Java cup] | [Bedrock LOGO]: two clickable icons with a
# vertical separator between them (#sxclEditionJavaButton / #sxclEditionBedrockButton,
# both sxcl::ui::IconSelectButton, container #sxclEditionPickRow).
# Icons (original assets, provenance in assets/icons/edition/NOTICE.md):
#   java_logo.svg    = official Java cup (devicon java-original, #0074BD), 20 logical px box
#   bedrock_logo.png = official MINECRAFT title logo from the user's Bedrock APK (1937x333 RGBA),
#                      20 logical px box -> width comes from the asset aspect (5.82 -> 116 px)
# What it proves, per run (all numbers measured on this machine, nothing guessed):
#   1) both buttons show up in SXCL_UI_DUMP with sane geometry and are not hidden, and the
#      LOGO button really is the WIDE box the asset aspect asks for (not a square 24x24)
#   2) the assets are NOT blank:
#      - asset level: the RGBA LOGO is scanned for non-transparent pixels (count / ratio)
#      - screen level: inside each icon box the pixels differ from the card background, and
#        each icon carries its own colours (Java cup = #0074BD family; LOGO = light letters
#        on its dark plate)
#      - the cup footprint and the LOGO footprint come out the same visual height (physical px)
#   3) the selected state is the indicator LINE UNDER THE ICON (user's final call: no box):
#      in the band "icon bottom +2..+6 logical px" the selected button has accent pixels and
#      the other one has exactly 0
#   3b) the [cup] | [LOGO] group is horizontally centred in the side-2 footer: group centre x
#      vs container centre x <= 1 logical pixel (container #sxclEditionPickRow, from the dump)
#   4) the vertical separator between the two icons is exactly ONE device pixel wide, runs
#      vertically over the whole button height, and sits within +/-1 px of the geometric
#      midpoint of the two buttons (container paintEvent + QPen(token, 0))
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
$assets = Join-Path $PSScriptRoot '..\assets\icons\edition'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$fail = 0
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path $exe)) {
  Write-Output ('EDITION-ICONS: FAIL (exe not found: ' + $exe + ')')
  exit 1
}

# This script drives the real exe three times. A leftover instance from an aborted run keeps
# running and can write game.edition into the same settings file while we measure (that is
# exactly how a restart run once reported the wrong side), so refuse to start while another
# sxcl-ui.exe is alive instead of producing a bogus verdict.
$busy = @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
  Write-Output ('EDITION-ICONS: FAIL (another sxcl-ui.exe is running, pid ' + ($busy.Id -join ',') +
                ' -- close it or wait, then re-run)')
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

# ---- asset level: the RGBA title LOGO must be a real image, not an empty canvas ----
function Get-LogoAssetStats([string]$png) {
  $bmp = New-Object System.Drawing.Bitmap($png)
  $total = $bmp.Width * $bmp.Height
  $opaque = 0; $light = 0
  $mnX = 999999; $mnY = 999999; $mxX = -1; $mxY = -1
  for ($y = 0; $y -lt $bmp.Height; $y++) {
    for ($x = 0; $x -lt $bmp.Width; $x++) {
      $c = $bmp.GetPixel($x, $y)
      if ($c.A -le 0) { continue }
      $opaque++
      $mean = ($c.R + $c.G + $c.B) / 3.0
      if ($mean -gt 100 -and $c.A -gt 128) {
        $light++
        if ($x -lt $mnX) { $mnX = $x }
        if ($x -gt $mxX) { $mxX = $x }
        if ($y -lt $mnY) { $mnY = $y }
        if ($y -gt $mxY) { $mxY = $y }
      }
    }
  }
  $w = $bmp.Width; $h = $bmp.Height
  $bmp.Dispose()
  return @{ w = $w; h = $h; total = $total; opaque = $opaque; light = $light;
            letterW = $mxX - $mnX + 1; letterH = $mxY - $mnY + 1;
            aspect = $w / [double]$h; letterAspect = ($mxX - $mnX + 1) / [double]($mxY - $mnY + 1) }
}

# ---- one button rect: icon footprint (vs the card background), accent ring, icon colours ----
# dump coordinates are LOGICAL, the screenshot is PHYSICAL (dpr), so everything scales.
# The icon box is the button minus the 2 px padding on every side; the ring (that padding)
# is where the accent frame/backdrop lives, so the icon's own colours never pollute the
# selection test, and the footprint bbox taken in the UNSELECTED state is the icon itself.
function Get-ButtonStats([string]$png, [int]$x, [int]$y, [int]$w, [int]$h, [double]$dpr,
                         [int]$padX, [int]$padTop, [int]$padBottom, [int[]]$cardRef) {
  $bmp = New-Object System.Drawing.Bitmap($png)
  $x0 = [int][math]::Floor($x * $dpr); $y0 = [int][math]::Floor($y * $dpr)
  $x1 = [int][math]::Ceiling(($x + $w) * $dpr); $y1 = [int][math]::Ceiling(($y + $h) * $dpr)
  # icon box: padX on the sides, padTop above, and below it the 4 px gap + 2 px indicator line
  # + 2 px bottom pad (IconSelectButton::sizeHint) -- so height = h - padTop - padBottom
  $ix0 = [int][math]::Floor(($x + $padX) * $dpr); $ix1 = [int][math]::Ceiling(($x + $w - $padX) * $dpr)
  $iy0 = [int][math]::Floor(($y + $padTop) * $dpr); $iy1 = [int][math]::Ceiling(($y + $h - $padBottom) * $dpr)
  # the indicator band: "icon bottom +2..+6 logical px"
  $bx0 = $ix0; $bx1 = $ix1
  $by0 = [int][math]::Floor($iy1 + 2 * $dpr); $by1 = [int][math]::Ceiling($iy1 + 6 * $dpr)
  $colors = @{}; $ink = 0; $inner = 0; $innerVsCard = 0; $bandAccent = 0; $bandTotal = 0
  $bluePx = 0; $lightPx = 0; $platePx = 0; $total = 0
  for ($py = $by0; $py -lt $by1; $py++) {
    for ($px = $bx0; $px -lt $bx1; $px++) {
      $pb = Get-Rgb $bmp $px $py
      $bandTotal++
      # accent (theme token) is blue-dominant: #4CC2FF in dark mode, #0067C0 in light mode
      if (($pb[2] - $pb[0]) -gt 40 -and $pb[2] -gt 110) { $bandAccent++ }
    }
  }
  $mnX = 999999; $mnY = 999999; $mxX = -1; $mxY = -1
  for ($py = $y0; $py -lt $y1; $py++) {
    for ($px = $x0; $px -lt $x1; $px++) {
      $p = Get-Rgb $bmp $px $py
      $k = ($p -join ',')
      if ($colors.ContainsKey($k)) { $colors[$k]++ } else { $colors[$k] = 1 }
      $total++
      $isInner = ($px -ge $ix0 -and $px -lt $ix1 -and $py -ge $iy0 -and $py -lt $iy1)
      if ($isInner) {
        $inner++
        if ((Get-Dist $p $cardRef) -gt 12) {
          $innerVsCard++
          # footprint of the icon itself (only meaningful while the button is unselected:
          # once it is selected the accent backdrop covers the whole box)
          if ($px -lt $mnX) { $mnX = $px }
          if ($px -gt $mxX) { $mxX = $px }
          if ($py -lt $mnY) { $mnY = $py }
          if ($py -gt $mxY) { $mxY = $py }
        }
        # the official Java cup is the #0074BD family (blue-dominant)
        if (($p[2] - $p[0]) -gt 40 -and $p[2] -gt 110) { $bluePx++ }
        $mean = ($p[0] + $p[1] + $p[2]) / 3.0
        if ($mean -gt 100) { $lightPx++ }   # LOGO letters / cup lines
        if ($mean -lt 40) { $platePx++ }    # LOGO dark plate
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
            bandAccent = $bandAccent; bandTotal = $bandTotal;
            bluePx = $bluePx; lightPx = $lightPx; platePx = $platePx;
            footW = $(if ($mxX -ge 0) { $mxX - $mnX + 1 } else { 0 });
            footH = $(if ($mxY -ge 0) { $mxY - $mnY + 1 } else { 0 });
            modal = $modal }
}

# ---- the vertical separator: exactly ONE device pixel, vertical, on the geometric midpoint ----
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
  # geometric midpoint of the two buttons, in the same device pixel space the painter uses
  $midLogical = [int](($leftRight + $rightLeft) / 2)
  $midDevice = $midLogical * $dpr
  $bmp.Dispose()
  return @{ width = $lineCols.Count; col = $(if ($lineCols.Count -ge 1) { $lineCols[0] } else { -1 });
            span = @($x0, $x1); rows = $rows; bg = (Get-Hex $bgTriple); vertical = $vertical;
            midLogical = $midLogical; midDevice = $midDevice;
            offBy = $(if ($lineCols.Count -ge 1) { [math]::Abs($lineCols[0] - $midDevice) } else { -1 });
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

  # 4) pixel scan: icons not blank, official colours, accent only on the selected one
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
    # icon box = button minus 2 px on the sides/top and 8 px at the bottom (4 gap + 2 line + 2 pad);
    # the LOGO box is wide because the asset is wide
    $boxLogical = @(($r[2] - 4), ($r[3] - 10))
    $boxPhys = @([math]::Round($boxLogical[0] * $dpr, 1), [math]::Round($boxLogical[1] * $dpr, 1))
    $s = Get-ButtonStats $png $r[0] $r[1] $r[2] $r[3] $dpr 2 2 8 $cardRef
    $stats[$name] = $s
    Write-Output ('    ' + $name + ': rect=' + ($r -join ',') + ' iconBox=' + ($boxLogical -join 'x') +
                  ' logical (' + ($boxPhys -join 'x') + ' phys) ink=' + $s.ink + '/' + $s.total +
                  ' innerVsCard=' + $s.innerVsCard + '/' + $s.inner +
                  ' indicatorBandAccentPx=' + $s.bandAccent + '/' + $s.bandTotal +
                  ' javaBluePx=' + $s.bluePx + ' lightPx=' + $s.lightPx + ' platePx=' + $s.platePx +
                  ' footprint=' + $s.footW + 'x' + $s.footH + ' modal=' + (Get-Hex $s.modal))
    if ($s.innerVsCard -lt 60) {
      Write-Output ('    -> FAIL ' + $name + ' icon looks BLANK (innerVsCard=' + $s.innerVsCard + ')')
      $script:fail++
    }
  }

  # the container that holds the group (used for the "group is centred" assertion)
  $hostRect = $null
  $hostHit = $txt | Select-String -Pattern '#sxclEditionPickRow \((\d+),(\d+) (\d+)x(\d+)\)' | Select-Object -First 1
  if ($hostHit) {
    $null = ($hostHit.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
    $hostRect = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3], [int]$Matches[4])
    Write-Output ('    container #sxclEditionPickRow (' + ($hostRect -join ',') + ')')
  } else {
    Write-Output '    -> FAIL container #sxclEditionPickRow not found in dump'
    $script:fail++
  }

  # 4a) each side must carry its own official colours
  if ($stats['sxclEditionJavaButton'].bluePx -lt 40) {
    Write-Output ('    -> FAIL java button does not show the official #0074BD cup (javaBluePx=' +
                  $stats['sxclEditionJavaButton'].bluePx + ', threshold 40)')
    $script:fail++
  }
  if ($stats['sxclEditionBedrockButton'].lightPx -lt 200) {
    Write-Output ('    -> FAIL bedrock button does not show the LOGO letters (lightPx=' +
                  $stats['sxclEditionBedrockButton'].lightPx + ', threshold 200)')
    $script:fail++
  }
  if ($stats['sxclEditionBedrockButton'].platePx -lt 200) {
    Write-Output ('    -> FAIL bedrock button does not show the LOGO plate (platePx=' +
                  $stats['sxclEditionBedrockButton'].platePx + ', threshold 200)')
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
  Write-Output ('    separator midpoint check: geometric mid logical=' + $sep.midLogical +
                ' -> device ' + $sep.midDevice + ' ; line is off by ' + $sep.offBy + ' device px')
  if ($sep.width -ne 1) {
    Write-Output ('    -> FAIL separator is not exactly 1 device pixel wide (cols differing from bg = ' + $sep.width + ')')
    $script:fail++
  }
  if ($sep.vertical -ne 3) {
    Write-Output ('    -> FAIL separator does not run vertically over the button height (rows hit ' + $sep.vertical + '/3)')
    $script:fail++
  }
  if ($sep.offBy -lt 0 -or $sep.offBy -gt 1) {
    Write-Output ('    -> FAIL separator is not on the midpoint of the two buttons (off by ' + $sep.offBy + ' device px)')
    $script:fail++
  }

  # 6) selection state = the indicator LINE UNDER THE ICON (user's final call: no box)
  if ($selectName -ne '') {
    $selName = $selectName
  } elseif ($pick -eq 'bedrock') {
    $selName = 'sxclEditionBedrockButton'
  } else {
    $selName = 'sxclEditionJavaButton'
  }
  $otherName = if ($selName -eq 'sxclEditionBedrockButton') { 'sxclEditionJavaButton' } else { 'sxclEditionBedrockButton' }
  $sel = $stats[$selName]; $other = $stats[$otherName]
  Write-Output ('    indicator band (icon bottom +2..+6 logical px): selected=' + $selName +
                ' accentPx=' + $sel.bandAccent + '/' + $sel.bandTotal + ' | unselected=' +
                $otherName + ' accentPx=' + $other.bandAccent + '/' + $other.bandTotal)
  if ($sel.bandAccent -lt 30) {
    Write-Output ('    -> FAIL the selected button has no accent line under its icon (band accentPx=' +
                  $sel.bandAccent + ', threshold 30)')
    $script:fail++
  }
  if ($other.bandAccent -ne 0) {
    Write-Output ('    -> FAIL the unselected button also draws an accent line under its icon (band accentPx=' +
                  $other.bandAccent + ')')
    $script:fail++
  }

  # 7) the whole [cup] | [LOGO] group must be centred in the footer container
  if ($hostRect -ne $null) {
    $groupLeft = [math]::Min($jr[0], $br[0])
    $groupRight = [math]::Max($jr[0] + $jr[2], $br[0] + $br[2])
    $groupCentre = ($groupLeft + $groupRight) / 2.0
    $hostCentre = $hostRect[0] + $hostRect[2] / 2.0
    $delta = [math]::Abs($groupCentre - $hostCentre)
    Write-Output ('    group centre x=' + $groupCentre + ' (group ' + $groupLeft + '..' + $groupRight +
                  ') vs container centre x=' + $hostCentre + ' (container ' + $hostRect[0] + '..' +
                  ($hostRect[0] + $hostRect[2]) + ') -> delta=' + [math]::Round($delta, 2) + ' logical px')
    if ($delta -gt 1.0) {
      Write-Output ('    -> FAIL the icon group is not centred in the container (delta=' +
                    [math]::Round($delta, 2) + ' logical px, threshold 1)')
      $script:fail++
    }
  }
  # the unselected one is where the icon's own footprint can be measured cleanly
  Write-Output ('    icon footprint of the UNSELECTED side (' + $otherName + ') = ' + $other.footW + 'x' +
                $other.footH + ' phys = ' + [math]::Round($other.footW / $dpr, 2) + 'x' +
                [math]::Round($other.footH / $dpr, 2) + ' logical')
  $script:runStats = $stats
  $script:unselName = $otherName
  $script:unselFoot = @($other.footW, $other.footH)
  $script:selBand = $sel.bandAccent
  $script:unselBand = $other.bandAccent
  $script:runDpr = $dpr
}

# ---- asset-level readings (printed once, they do not depend on a run) ----
$logoPng = Join-Path $assets 'bedrock_logo.png'
Write-Output 'asset check (original files in the repo):'
if (-not (Test-Path $logoPng)) {
  Write-Output ('  -> FAIL missing ' + $logoPng)
  $fail++
} else {
  $logo = Get-LogoAssetStats $logoPng
  $ratio = [math]::Round(100.0 * $logo.opaque / $logo.total, 2)
  Write-Output ('  bedrock_logo.png ' + $logo.w + 'x' + $logo.h + ' RGBA: non-transparent pixels = ' +
                $logo.opaque + ' / ' + $logo.total + ' = ' + $ratio + '%')
  Write-Output ('  letters (opaque, mean>100) = ' + $logo.light + ' px, bbox ' + $logo.letterW + 'x' +
                $logo.letterH + ' -> letter ink height is ' +
                [math]::Round(100.0 * $logo.letterH / $logo.h, 1) + '% of the canvas, aspect ' +
                [math]::Round($logo.letterAspect, 2) + ', canvas aspect ' + [math]::Round($logo.aspect, 3))
  if ($logo.opaque -lt 100000 -or $ratio -lt 50 -or $ratio -gt 99) {
    Write-Output ('  -> FAIL the LOGO asset does not look like the official title art (opaque=' +
                  $logo.opaque + ' ratio=' + $ratio + '%)')
    $fail++
  }
  if ($logo.letterW -lt 1000) { Write-Output '  -> FAIL the LOGO asset has no letter ink'; $fail++ }
  # the canvas must be cropped to the letter ink (that is why the two icons can be the same ink height):
  # any black margin left on top/bottom would make the letters smaller than the cup at the same box height
  if ($logo.letterH -lt 0.95 * $logo.h -or $logo.letterW -lt 0.95 * $logo.w) {
    Write-Output ('  -> FAIL the LOGO canvas is not cropped to the letter ink (letters ' + $logo.letterW +
                  'x' + $logo.letterH + ' vs canvas ' + $logo.w + 'x' + $logo.h + ')')
    $fail++
  } else {
    Write-Output ('  canvas is cropped to the letter ink: letters ' + $logo.letterW + 'x' + $logo.letterH +
                  ' vs canvas ' + $logo.w + 'x' + $logo.h + ' (>= 95% both ways)')
  }
}

Write-Output 'run A: click bedrock'
$before = (Select-String -Path $ini -Pattern 'game\.edition=(\w+)' | Select-Object -Last 1).Line.Trim()
Write-Output ('  start state: ' + $before)
Invoke-EditionRun 'bedrock' 'bedrock' 'bedrock'
$statsA = $script:runStats
$cupFoot = $script:unselFoot      # run A leaves JAVA unselected -> the cup footprint
$cupFootName = $script:unselName
$dpr = $script:runDpr

Write-Output 'run B: click java again'
Invoke-EditionRun 'java' 'java' 'java'
$statsB = $script:runStats
$logoFoot = $script:unselFoot     # run B leaves BEDROCK unselected -> the LOGO footprint
$logoFootName = $script:unselName

Write-Output 'run C: restart with NO click (the choice must come back from the settings file)'
Set-Content -Path $ini -Value ('game.default_dir=' + $work + '\.minecraft') -Encoding utf8
Add-Content -Path $ini -Value 'game.edition=bedrock'
$restoreLine = (Select-String -Path $ini -Pattern 'game.edition=(\w+)' | Select-Object -Last 1).Line.Trim()
Write-Output ('  settings before run C: ' + $restoreLine)
Invoke-EditionRun 'restore' '' '' 'sxclEditionBedrockButton'
$statsC = $script:runStats
if ($statsC.Count -eq 2) {
  Write-Output ('  restart: bedrock indicator band accentPx=' + $statsC['sxclEditionBedrockButton'].bandAccent +
                ', java indicator band accentPx=' + $statsC['sxclEditionJavaButton'].bandAccent)
  if ($statsC['sxclEditionBedrockButton'].bandAccent -lt 30) {
    Write-Output '  -> FAIL after restart the bedrock button is not the selected one (no accent line)'
    $fail++
  }
  if ($statsC['sxclEditionJavaButton'].bandAccent -ne 0) {
    Write-Output '  -> FAIL after restart the java button wrongly draws an accent line'
    $fail++
  }
}

# cross-run: the same button must draw the accent line only when it is the selected one
if ($statsA.Count -eq 2 -and $statsB.Count -eq 2) {
  foreach ($name in @('sxclEditionJavaButton', 'sxclEditionBedrockButton')) {
    $a = $statsA[$name].bandAccent; $b = $statsB[$name].bandAccent
    Write-Output ('  cross-run ' + $name + ': indicator band accentPx A=' + $a + ' B=' + $b)
    if ($a -eq $b) { Write-Output ('  -> FAIL ' + $name + ' indicator looks identical in both runs'); $fail++ }
  }
}

# ---- the sizing claim: both icons must come out the same visual height ----
Write-Output 'sizing cross-check (each side measured while UNSELECTED):'
Write-Output ('  cup  (' + $cupFootName + ', run A) footprint = ' + $cupFoot[0] + 'x' + $cupFoot[1] +
              ' phys = ' + [math]::Round($cupFoot[0] / $dpr, 2) + 'x' + [math]::Round($cupFoot[1] / $dpr, 2) +
              ' logical')
Write-Output ('  LOGO (' + $logoFootName + ', run B) footprint = ' + $logoFoot[0] + 'x' + $logoFoot[1] +
              ' phys = ' + [math]::Round($logoFoot[0] / $dpr, 2) + 'x' + [math]::Round($logoFoot[1] / $dpr, 2) +
              ' logical ; asset canvas aspect ' + [math]::Round($logo.aspect, 3) + ' -> screen aspect ' +
              [math]::Round($logoFoot[0] / [double]$logoFoot[1], 3))
# the ink height is what must match between the two icons (1 logical pixel tolerance)
$cupInkLogical = $cupFoot[1] / $dpr
$logoInkLogical = $logoFoot[1] / $dpr
Write-Output ('  ink height: cup = ' + [math]::Round($cupInkLogical, 2) + ' logical (' + $cupFoot[1] +
              ' phys), LOGO = ' + [math]::Round($logoInkLogical, 2) + ' logical (' + $logoFoot[1] +
              ' phys) -> difference ' + [math]::Round([math]::Abs($cupInkLogical - $logoInkLogical), 2) +
              ' logical px (allowed <= 1)')
if ([math]::Abs($cupInkLogical - $logoInkLogical) -gt 1.0) {
  Write-Output ('  -> FAIL the two icons do not have the same ink height (cup ' + [math]::Round($cupInkLogical, 2) +
                ' vs LOGO ' + [math]::Round($logoInkLogical, 2) + ' logical px)')
  $fail++
}
if ($logoFoot[1] -lt 28 -or $logoFoot[1] -gt 32) {
  Write-Output ('  -> FAIL LOGO ink height ' + $logoFoot[1] + ' phys is not the expected 30 (20 logical)')
  $fail++
}
if ($logoFoot[0] -lt 204 -or $logoFoot[0] -gt 212) {
  Write-Output ('  -> FAIL LOGO ink width ' + $logoFoot[0] + ' phys is not the expected 208.5 (139 logical)')
  $fail++
}
$screenAspect = $logoFoot[0] / [double]$logoFoot[1]
if ([math]::Abs($screenAspect - $logo.aspect) / $logo.aspect -gt 0.10) {
  Write-Output ('  -> FAIL the LOGO is not rendered at its own aspect (' + [math]::Round($screenAspect, 3) +
                ' vs ' + [math]::Round($logo.aspect, 3) + ') -- it would be distorted or not the official art')
  $fail++
}
if ($cupFoot[0] -lt 15 -or $cupFoot[1] -lt 25) {
  Write-Output ('  -> FAIL cup footprint ' + $cupFoot[0] + 'x' + $cupFoot[1] + ' phys looks blank/too small')
  $fail++
}

Write-Output 'both icons clickable: run A -> bedrock button, run B -> java button (product path button->click())'
Write-Output 'icon sources (kept in sync with assets/icons/edition/NOTICE.md):'
Write-Output '  java_logo.svg    <- https://raw.githubusercontent.com/devicons/devicon/master/icons/java/java-original.svg'
Write-Output '                      (same bytes also at https://unpkg.com/devicon@latest/icons/java/java-original.svg ; sha256 7582E518A9C02425F97155E5A3BD39D1A3A7D421B78CAF9C8DF7443DAD3EDC5D)'
Write-Output '  bedrock_logo.png <- D:\SilentStudio\AdbGUI\APK\Minecraft_1.26.40.5.apk : assets/assets/resource_packs/vanilla/textures/ui/title.png'
Write-Output '                      original 1937x333 RGBA 86796 bytes sha256 1AB368C3719A0FA0C273A0040BD5D3E8C47A9678B8DFF22A09AA1BF570781662'
Write-Output '                      in-repo file = SAME PIXELS, canvas only cropped to the letter ink (19,17)..(1916,289) -> 1898x273 151064 bytes'
Write-Output '                      sha256 C9102616002FEEA5890406F9BB2054D3E2DCED3CB5C8104AA954D28A1B7D3FC7 (no keying, no recolouring, no scaling)'
Write-Output '  bedrock_block.png (spare, not used by the UI) <- same APK: assets/assets/resource_packs/vanilla/textures/blocks/bedrock.png'

# the required banner says "luo pan yi zhi" (settings files agree) in Chinese; built from code
# points so THIS FILE STAYS PURE ASCII while the printed line is the exact required wording.
$okSuffix = 'EDITION-ICONS: ALL OK (java/bedrock ' + [char]0x843D + [char]0x76D8 + [char]0x4E00 + [char]0x81F4 + ')'
if ($fail -eq 0) {
  Write-Output $okSuffix
} else {
  Write-Output ('EDITION-ICONS: ' + $fail + ' FAILED')
  exit 1
}
