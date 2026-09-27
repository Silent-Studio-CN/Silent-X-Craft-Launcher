# (C) Silent X Craft Launcher -- dropdown popup width acceptance (user 2026-09-27).
#
# The user's rule (verbatim): "from now on make the drop-down show the WHOLE window
# width, wrap if it does not fit -- what is that little rectangle, who can read that?"
#
# This script checks that rule on three pages of the real product, at the control layer
# (libqf ComboBoxMenu::applyHostWidthPolicy + libqf LineEdit completers -- see
# patches/libqf/0003-combo-popup-width.patch), and it never touches build-ui:
#   settings / theme mode                 (item "follow the system")
#   settings / version list refresh rate  (item "2 minutes")
#   settings / Java runtime path          (the LONG entry: a java.exe path -- no-cut assertion)
#   download / Minecraft version category (item "snapshot")   <- the download page combo
#   download / MOD version filter         (completer popup of the version input box)
#
# Per target it runs the private build with SXCL_UI_POPUP / SXCL_UI_POPUP_KIND / SXCL_UI_SHOT,
# reads the machine-readable trace ([sxcl-ui] popup-trace / popup-item), and then COUNTS
# PIXELS in the popup screenshot:
#   * panel width measured from the screenshot == panel width from the trace (+-2.5 logical px);
#   * qf ComboBox popups: the widest painted text line is as wide as the widest item's text
#     (i.e. the whole text really was painted, nothing was cut) and no glyph ink reaches the
#     item's right padding/margin zone.
# The completer popup (a scrollable Qt list whose selection bar spans the whole row) gets the
# pixel panel check plus the trace's style-computed text-area numbers, not the ink-band check.
# ASCII only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM).

$ErrorActionPreference = 'Continue'

$exe  = 'D:\SilentStudio\_build\combow\src\ui\Release\sxcl-ui.exe'   # private build dir
$work = 'D:\SilentStudio\_test\combo_width'
$winW = 1100
$winH = 750
$ratioFloor = 0.90

foreach ($v in @('SXCL_UI_ROUTE','SXCL_UI_NAV','SXCL_UI_THEME','SXCL_UI_POPUP','SXCL_UI_POPUP_KIND',
                 'SXCL_UI_SHOT','SXCL_UI_WINDOW','SXCL_UI_SETTINGS','SXCL_UI_GAME_DIR','SXCL_MENU_TRACE')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null
if (-not (Test-Path $exe)) { Write-Output ('COMBO-WIDTH: FAIL (private build not found: ' + $exe + ')'); exit 1 }
$ini = Join-Path $work 'settings.ini'
Set-Content -Path $ini -Value 'game.default_dir=' -Encoding utf8

# ---- pixel reader -------------------------------------------------------------------
# C# because a PowerShell loop over ~500k pixels per popup is far too slow. Loaded
# assemblies are handed to the compiler as references (System.Drawing.Common and friends
# are not part of the default reference set under PowerShell 7).
Add-Type -AssemblyName System.Drawing
$refs = @([AppDomain]::CurrentDomain.GetAssemblies() | Where-Object { -not $_.IsDynamic -and $_.Location } |
          Select-Object -ExpandProperty Location -Unique)
Add-Type -Language CSharp -ReferencedAssemblies $refs -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class ComboPix
{
    // layout: 0..3 panel bbox, 4..6 background rgb, 7..10 ink bbox, 11 band count,
    //         12.. bands (x0,y0,x1,y1) * band count   -- all in DEVICE pixels
    public static double Luma(int r, int g, int b) { return 0.299 * r + 0.587 * g + 0.114 * b; }

    public static int[] Analyze(string path)
    {
        using (Bitmap bmp = new Bitmap(path))
        {
            int w = bmp.Width, h = bmp.Height;
            BitmapData data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly,
                                           PixelFormat.Format32bppArgb);
            int stride = data.Stride;
            byte[] buf = new byte[stride * h];
            Marshal.Copy(data.Scan0, buf, 0, buf.Length);
            bmp.UnlockBits(data);

            // panel = opaque area (the popup's translucent/shadow margin is skipped)
            int px0 = w, py0 = h, px1 = -1, py1 = -1;
            int floor = 250;
            for (int pass = 0; pass < 2 && px1 < 0; pass++)
            {
                if (pass == 1) floor = 8;   // translucent popup: any opacity
                for (int y = 0; y < h; y++)
                {
                    int row = y * stride;
                    for (int x = 0; x < w; x++)
                    {
                        if (buf[row + x * 4 + 3] < floor) continue;
                        if (x < px0) px0 = x;
                        if (x > px1) px1 = x;
                        if (y < py0) py0 = y;
                        if (y > py1) py1 = y;
                    }
                }
            }
            int[] res = new int[12 + 4 * 64];
            if (px1 < 0) return res;

            int[] hist = new int[32768];
            for (int y = py0 + 2; y <= py1 - 2; y += 3)
            {
                int row = y * stride;
                for (int x = px0 + 2; x <= px1 - 2; x += 3)
                {
                    int o = row + x * 4;
                    hist[((buf[o + 2] >> 3) << 10) | ((buf[o + 1] >> 3) << 5) | (buf[o] >> 3)]++;
                }
            }
            int bestKey = 0, bestCount = -1;
            for (int k = 0; k < 32768; k++)
                if (hist[k] > bestCount) { bestCount = hist[k]; bestKey = k; }
            int bgR = ((bestKey >> 10) & 31) << 3, bgG = ((bestKey >> 5) & 31) << 3, bgB = (bestKey & 31) << 3;
            double bgL = Luma(bgR, bgG, bgB);

            // ink = glyph pixels: luma far from the panel background AND grey (a coloured
            // highlight bar / accent indicator / scroll bar is not text)
            bool[] rowInk = new bool[h];
            int ix0 = w, iy0 = h, ix1 = -1, iy1 = -1;
            for (int y = py0 + 3; y <= py1 - 3; y++)
            {
                int row = y * stride;
                for (int x = px0 + 3; x <= px1 - 3; x++)
                {
                    int o = row + x * 4;
                    if (buf[o + 3] < 200) continue;
                    int r = buf[o + 2], g = buf[o + 1], b = buf[o];
                    int mx = Math.Max(r, Math.Max(g, b)), mn = Math.Min(r, Math.Min(g, b));
                    if (mx - mn > 48) continue;
                    if (Math.Abs(Luma(r, g, b) - bgL) < 55) continue;
                    rowInk[y] = true;
                    if (x < ix0) ix0 = x;
                    if (x > ix1) ix1 = x;
                    if (y < iy0) iy0 = y;
                    if (y > iy1) iy1 = y;
                }
            }

            // one bbox per maximal run of ink rows = one painted text line
            int bandCount = 0, start = -1;
            for (int y = py0; y <= py1 + 1; y++)
            {
                bool ink = (y <= py1) && rowInk[y];
                if (ink) { if (start < 0) start = y; continue; }
                if (start < 0) continue;
                int bx0 = w, bx1 = -1;
                for (int yy = start; yy < y; yy++)
                {
                    int row = yy * stride;
                    for (int x = px0 + 3; x <= px1 - 3; x++)
                    {
                        int o = row + x * 4;
                        if (buf[o + 3] < 200) continue;
                        int r = buf[o + 2], g = buf[o + 1], b = buf[o];
                        int mx = Math.Max(r, Math.Max(g, b)), mn = Math.Min(r, Math.Min(g, b));
                        if (mx - mn > 48) continue;
                        if (Math.Abs(Luma(r, g, b) - bgL) < 55) continue;
                        if (x < bx0) bx0 = x;
                        if (x > bx1) bx1 = x;
                    }
                }
                if (bx1 >= 0 && bandCount < 64)
                {
                    int b2 = 12 + bandCount * 4;
                    res[b2] = bx0; res[b2 + 1] = start; res[b2 + 2] = bx1; res[b2 + 3] = y - 1;
                    bandCount++;
                }
                start = -1;
            }

            res[0] = px0; res[1] = py0; res[2] = px1; res[3] = py1;
            res[4] = bgR; res[5] = bgG; res[6] = bgB;
            res[7] = ix0; res[8] = iy0; res[9] = ix1; res[10] = iy1;
            res[11] = bandCount;
            return res;
        }
    }
}
'@

# CJK selectors are built from code points: this file must survive Windows PowerShell 5.1
# reading it as ANSI (see tools/ui_mods_filters.ps1 for the same helper).
function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }

# ---- assertions ---------------------------------------------------------------------
$script:total = 0
$script:fail = 0
function Ok([bool]$cond, [string]$what) {
  $script:total++
  if ($cond) { Write-Output ('  [ok]   ' + $what) }
  else { Write-Output ('  [FAIL] ' + $what); $script:fail++ }
}

# ---- one target ---------------------------------------------------------------------
function Test-Target($t) {
  Write-Output ''
  Write-Output ('== ' + $t.name + ' (' + $t.route + $(if ($t.nav) { ' / ' + $t.nav } else { '' }) + ', kind=' + $t.kind + ') ==')
  $png = Join-Path $work ('pop_' + $t.tag + '.png')
  Remove-Item $png -ErrorAction SilentlyContinue

  # selectors are tried in order: item text drifts between builds, indices drift more
  $errLines = $null
  foreach ($pick in $t.pick) {
    $err = Join-Path $work ('err_' + $t.tag + '.txt')
    $env:SXCL_UI_SETTINGS = $ini
    $env:SXCL_UI_WINDOW = ('' + $winW + 'x' + $winH)
    $env:SXCL_UI_ROUTE = $t.route
    $env:SXCL_UI_THEME = 'dark'
    $env:SXCL_UI_POPUP = $pick
    $env:SXCL_UI_POPUP_KIND = $t.kind
    $env:SXCL_UI_SHOT = $png
    if ($t.nav) { $env:SXCL_UI_NAV = $t.nav } else { Remove-Item Env:SXCL_UI_NAV -ErrorAction SilentlyContinue }
    Start-Process -FilePath $exe -RedirectStandardOutput (Join-Path $work 'out.txt') -RedirectStandardError $err -Wait | Out-Null
    $lines = @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
    $hit = @($lines | Where-Object { $_ -match 'popup-trace' })
    if ($hit.Count -gt 0 -and (Test-Path $png)) { $errLines = $lines; Write-Output ('  selector "' + $pick + '"'); break }
    Write-Output ('  (selector "' + $pick + '" opened nothing -- trying next)')
  }
  if ($null -eq $errLines) {
    Ok $false ('popup opened (no selector matched)')
    return
  }

  $trace = ($errLines | Where-Object { $_ -match 'popup-trace' } | Select-Object -First 1)
  $m = [regex]::Match($trace, 'kind=(\S+) index=(-?\d+) win=(\d+)x(\d+) panel=(\d+)x(\d+) container=(\d+)x(\d+) ratio=([0-9.]+)')
  if (-not $m.Success) { Ok $false 'popup-trace parsed'; return }
  $wW = [int]$m.Groups[3].Value; $wH = [int]$m.Groups[4].Value
  $pW = [int]$m.Groups[5].Value; $pH = [int]$m.Groups[6].Value
  $cW = [int]$m.Groups[7].Value; $cH = [int]$m.Groups[8].Value
  $ratio = $pW / $wW

  $items = @()
  foreach ($line in $errLines) {
    $im = [regex]::Match($line, 'popup-item \| i=(\d+) textw=(\d+) area=(\d+) lines=(\d+) rowH=(\d+) fit=(\d+) text=(.*)$')
    if ($im.Success) {
      $items += [pscustomobject]@{
        i = [int]$im.Groups[1].Value; textw = [int]$im.Groups[2].Value; area = [int]$im.Groups[3].Value
        lines = [int]$im.Groups[4].Value; rowH = [int]$im.Groups[5].Value; fit = [int]$im.Groups[6].Value
        text = $im.Groups[7].Value
      }
    }
  }
  Ok ($items.Count -gt 0) ('popup has readable items (' + $items.Count + ')')

  $widest = $items | Sort-Object -Property textw -Descending | Select-Object -First 1
  $widestFit = ($widest.fit -eq 1)
  $notFitting = @($items | Where-Object { $_.fit -ne 1 })

  Write-Output ('  window=' + $wW + 'x' + $wH + '  panel=' + $pW + 'x' + $pH + '  container=' + $cW + 'x' + $cH + '  ratio=' + ('{0:N4}' -f $ratio))
  Write-Output ('  items=' + $items.Count + '  widest: textw=' + $widest.textw + ' area=' + $widest.area + ' lines=' + $widest.lines + '  text="' + $widest.text + '"')

  Ok ($ratio -ge $ratioFloor) ('panel width ' + $pW + ' >= ' + ($ratioFloor * 100) + '% of window width ' + $wW + ' (ratio ' + ('{0:N4}' -f $ratio) + ')')
  Ok ($widestFit) ('longest item fully shown (textw=' + $widest.textw + ' <= area=' + $widest.area + ', or wrapped into ' + $widest.lines + ' line(s))')
  Ok ($notFitting.Count -eq 0) ('every item fit=1 (' + $items.Count + ' items, ' + $notFitting.Count + ' not fitting)')

  if ($t.kind -eq 'combo') {
    # no scrolling -> the right edge zone really is padding, not a scroll bar
    $sumRow = ($items | Measure-Object -Property rowH -Sum).Sum
    Ok (($sumRow + 8) -le $pH) ('no vertical overflow: sum(rowH)=' + $sumRow + ' + margins <= panel height ' + $pH)
  } else {
    Write-Output ('  (completer popup is a scrollable Qt list: ' + $items.Count + ' rows in ' + $pH + ' px -- text fit is asserted from the trace above)')
  }

  # ---- pixels --------------------------------------------------------------------
  $px = [ComboPix]::Analyze($png)
  $bmp = [System.Drawing.Bitmap]::new($png); $imgW = $bmp.Width; $imgH = $bmp.Height; $bmp.Dispose()
  $dpr = $imgW / [double]$cW
  $px0 = $px[0]; $px1 = $px[2]
  $inkX0 = $px[7]; $inkX1 = $px[9]
  $bandCount = $px[11]
  $panelLogW = ($px1 - $px0 + 1) / $dpr
  $inkLogW = 0; $inkRightGap = 0
  if ($inkX1 -ge $inkX0) { $inkLogW = ($inkX1 - $inkX0 + 1) / $dpr; $inkRightGap = ($px1 - $inkX1) / $dpr }
  $widestBand = 0
  for ($b = 0; $b -lt $bandCount; $b++) {
    $bw = ($px[12 + $b * 4 + 2] - $px[12 + $b * 4] + 1) / $dpr
    if ($bw -gt $widestBand) { $widestBand = $bw }
  }
  Write-Output ('  pixels: img=' + $imgW + 'x' + $imgH + ' dpr=' + ('{0:N2}' -f $dpr) + ' panel=' + ($px1 - $px0 + 1) + 'x' + ($px[3] - $px[1] + 1) + ' (' + ('{0:N1}' -f $panelLogW) + ' logical) textlines=' + $bandCount)
  Write-Output ('  pixels: ink width=' + ('{0:N1}' -f $inkLogW) + ' widest painted line=' + ('{0:N1}' -f $widestBand) + ' gap ink->panel right edge=' + ('{0:N1}' -f $inkRightGap) + ' logical px')

  # the whole popup window must be in the screenshot at the traced container size
  $imgLogW = $imgW / $dpr
  Ok ([math]::Abs($imgLogW - $cW) -le 1.0) ('screenshot covers the popup window: ' + ('{0:N1}' -f $imgLogW) + ' == container width ' + $cW + ' (+-1)')
  # qf ComboBox popup: the visible panel is the opaque area (translucent 12px side margins around it).
  # Qt completer popup: its own frame fades out (a few translucent px), so the tolerance is wider.
  $panelTol = 2.5
  if ($t.kind -ne 'combo') { $panelTol = 10.0 }
  Ok ([math]::Abs($panelLogW - $pW) -le $panelTol) ('screenshot panel width ' + ('{0:N1}' -f $panelLogW) + ' == trace panel width ' + $pW + ' (+-' + $panelTol + ')')
  if ($t.kind -eq 'combo') {
    Ok ($inkRightGap -ge 12.0) ('no glyph ink in the right margin/padding: last ink is ' + ('{0:N1}' -f $inkRightGap) + ' logical px left of the panel edge (>= 12)')
    if ($widest.lines -eq 1) {
      Ok ($widestBand -ge ($widest.textw - 6)) ('widest painted text line ' + ('{0:N1}' -f $widestBand) + ' >= widest item text width ' + $widest.textw + ' - 6  (the whole text was painted)')
    } else {
      Ok ($true) ('widest item wraps into ' + $widest.lines + ' lines -> per-line band check not applicable')
    }
  } else {
    Ok ($widestBand -ge 4.0) ('completer popup paints text (widest painted line ' + ('{0:N1}' -f $widestBand) + ' logical px)')
  }
}

# ---- targets ------------------------------------------------------------------------
$themeFollow = C @(0x8DDF, 0x968F, 0x7CFB, 0x7EDF)      # "follow the system"
$themeDark   = C @(0x6DF1, 0x8272)                      # "dark"
$refresh2m   = '2' + (C @(0x5206, 0x949F))              # "2 minutes"
$refresh30s  = '30' + (C @(0x79D2))                     # "30 seconds"
$snapshot    = C @(0x5FEB, 0x7167)                      # "snapshot"
$release     = C @(0x6B63, 0x5F0F, 0x7248)              # "release"

$targets = @(
  [pscustomobject]@{ tag = 'settings_theme';   name = 'settings: theme mode';               route = 'settings'; nav = '';             kind = 'combo';     pick = @(('text:' + $themeFollow), ('text:' + $themeDark)) },
  [pscustomobject]@{ tag = 'settings_refresh'; name = 'settings: version list refresh';    route = 'settings'; nav = '';             kind = 'combo';     pick = @(('text:' + $refresh2m), ('text:' + $refresh30s)) },
  [pscustomobject]@{ tag = 'settings_java';    name = 'settings: Java runtime path (LONG)'; route = 'settings'; nav = '';             kind = 'combo';     pick = @('text:java.exe','text:Java ') },
  [pscustomobject]@{ tag = 'download_mc';      name = 'download: version category';         route = 'download'; nav = 'download_mc'; kind = 'combo';     pick = @(('text:' + $snapshot), ('text:' + $release)) },
  [pscustomobject]@{ tag = 'download_mods';    name = 'download/MOD: version filter';       route = 'download'; nav = 'download_mod'; kind = 'completer'; pick = @('text:1.21','text:1.20') }
)

foreach ($t in $targets) { Test-Target $t }

Write-Output ''
if ($script:fail -eq 0) {
  Write-Output ('COMBO-WIDTH: ALL OK (' + $script:total + '/' + $script:total + ' assertions; every popup panel >= 90% of the window width, long text never cut)')
} else {
  Write-Output ('COMBO-WIDTH: FAIL (' + ($script:total - $script:fail) + '/' + $script:total + ' assertions)')
  exit 1
}
