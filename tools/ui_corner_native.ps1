# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.
#
# Acceptance: the top-left corner of the old shell is drawn by **libqf itself**
# (StackedWidget + FluentStyleSheet::FLUENT_WINDOW), not by us.
#
# Evidence chain (read from the library, not guessed):
#   reference/qfluentwidgets/window/fluent_window.py:122,129
#       self.stackedWidget = StackedWidget(self)
#       FluentStyleSheet.FLUENT_WINDOW.apply(self.stackedWidget)
#   libqf resources/qf/qfluentwidgets/qss/dark/fluent_window.qss:63-69 (light: 1-7)
#       StackedWidget { border: 1px solid rgba(0,0,0,0.18); border-right: none;
#                       border-bottom: none; border-top-left-radius: 10px;
#                       background-color: rgba(255,255,255,0.0314); }
#   src/ui/main_window.cpp -> m_stack = new StackedWidget(body) +
#       FluentStyleSheet::apply(m_stack, FluentStyleSheet::FLUENT_WINDOW)
#       (no QPainterPath elbow, no hand drawn line, no helper inset any more)
#
# What this script checks, on real screenshots (dpr 1.5, 1100x750, route=home):
#   a) dark + light, main navigation collapsed (48)
#   b) the same with the main navigation expanded (322) and collapsed again
#   c) maximized, and restored again
#   d) hidden to the tray and shown again
# For every step: a screenshot is scanned for the three face colours, the junction
# band (how many runs, how many device pixels), the drawn corner radius and whether
# the two borders are connected through the corner.
#
# ASCII-only on purpose: Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM.
# Acceptance/temp files live under D:\SilentStudio\_test (never %TEMP% / C:).
#
# Run:  pwsh -File tools\ui_corner_native.ps1
# Exit: 0 = every scenario OK, 1 = at least one assertion failed.
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
if (-not (Test-Path $exe)) { $exe = Join-Path $root 'build\src\ui\Release\sxcl-ui.exe' }
if (-not (Test-Path $exe)) { Write-Output ('CORNER-NATIVE: FAIL (sxcl-ui.exe not found under ' + $root + ')'); exit 1 }

$work = 'D:\SilentStudio\_test\ui_corner_native'
New-Item -ItemType Directory -Force -Path $work | Out-Null

# ---------------------------------------------------------------- the pixel scanner
$scan = Join-Path $work 'corner_scan.py'
$scanner = @'
# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.
#
# Corner scanner for tools/ui_corner_native.ps1 (written out by that script). ASCII only.
#
# What it measures on a real screenshot (never on synthetic data):
#   * rail / title-band / content colours -- the three faces that meet at the corner
#   * the junction band: how many RUNS of non-background pixels cross it, how many
#     DEVICE pixels each run is, and where the band starts (that column / dpr is the
#     navigation width, measured rather than assumed)
#   * the corner cut "m" = how far from the corner point the OUTER edge of the drawn
#     arc first reaches the straight border row/column. For a 1-device-px stroke of
#     radius R the outer edge first enters the border row at m = R - sqrt(2R-1);
#     inverting: R = (m+1) + sqrt(2m). Printed in device px and logical px.
#     (The library QSS value is border-top-left-radius: 10px -> 15 device px @ dpr 1.5.)
#   * whether the two straight borders are CONNECTED through the corner. The user's
#     2026-09-26 complaint was exactly a corner that is NOT connected: the hand drawn
#     arc was covered by the opaque content box ("a line going up + a bend on the right").
#
# usage: corner_scan.py <png> <tag> <dpr>
import math
import sys
from PIL import Image


def hexs(c):
    return '#%02x%02x%02x' % (c[0], c[1], c[2])


def dist(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))


def main():
    png, tag = sys.argv[1], sys.argv[2]
    dpr = float(sys.argv[3])
    im = Image.open(png).convert('RGB')
    px = im.load()
    W, H = im.size
    tol = 2

    # ---- the three faces -------------------------------------------------
    y0 = int(round(0.53 * H))                                  # a row with no nav widgets
    rail = px[max(1, int(round(6 * dpr))), y0]                 # left navigation column
    title = px[int(round(W * 0.42)), max(1, int(round(6 * dpr)))]   # title band

    # Walk right until the colour stops being the rail colour: the first differing
    # column is the left edge of the content card (drawn by the library).
    x = int(round(6 * dpr))
    while x < W - 1 and dist(px[x, y0], rail) <= tol:
        x += 1
    corner_x = x
    while x < W - 1 and dist(px[x, y0], rail) > tol:
        x += 1
    content = px[min(W - 1, x + int(round(20 * dpr))), y0]

    # The card's top border row, measured in a column that is inside the content card
    # but left of any page content (every page keeps its own left margin).
    xprobe = min(W - 1, corner_x + int(round(20 * dpr)))
    y = 0
    while y < H - 1 and dist(px[xprobe, y], title) <= tol:
        y += 1
    corner_y = y
    while y < H - 1 and dist(px[xprobe, y], title) > tol:
        y += 1

    # ---- runs at the junction -------------------------------------------
    def runs(values, present):
        out = []
        start = None
        for v in values:
            if present(v):
                if start is None:
                    start = v
            elif start is not None:
                out.append((start, v - 1))
                start = None
        if start is not None:
            out.append((start, values[-1]))
        return out

    v_runs = runs(list(range(corner_x - 4, corner_x + int(round(12 * dpr)))),
                  lambda xx: dist(px[xx, y0], rail) > tol)
    h_runs = runs(list(range(corner_y - 4, corner_y + int(round(12 * dpr)))),
                  lambda yy: dist(px[xprobe, yy], title) > tol)
    v_band = (v_runs[-1][1] - corner_x + 1) if v_runs else 0
    h_band = (h_runs[-1][1] - corner_y + 1) if h_runs else 0

    # ---- arc profile around the corner ----------------------------------
    probe = int(round(20 * dpr))
    topmost = {}
    leftmost = {}
    for xx in range(max(0, corner_x - 2), min(W, corner_x + probe)):
        for yy in range(max(0, corner_y - 2), min(H, corner_y + probe)):
            if dist(px[xx, yy], rail) > tol:
                if xx not in topmost:
                    topmost[xx] = yy
                if yy not in leftmost or xx < leftmost[yy]:
                    leftmost[yy] = xx
    m_x = -1
    for xx in range(corner_x, min(W, corner_x + probe)):
        if topmost.get(xx, 10 ** 6) <= corner_y:
            m_x = xx - corner_x
            break
    m_y = -1
    for yy in range(corner_y, min(H, corner_y + probe)):
        if leftmost.get(yy, 10 ** 6) <= corner_x:
            m_y = yy - corner_y
            break
    m = max(m_x, m_y)
    radius = (m + 1) + math.sqrt(2.0 * m) if m > 0 else -1.0

    # ---- connectivity of the two straight borders ------------------------
    box = int(round(20 * dpr))
    mask = set()
    for xx in range(max(0, corner_x - 2), min(W, corner_x + box)):
        for yy in range(max(0, corner_y - 2), min(H, corner_y + box)):
            if dist(px[xx, yy], rail) > tol:
                mask.add((xx, yy))
    reached = False
    seen = set()
    seed = (corner_x + box - 3, corner_y)
    if seed in mask:
        stack = [seed]
        seen.add(seed)
        while stack:
            cur = stack.pop()
            if cur[1] >= corner_y + box - 5 and cur[0] <= corner_x + 1:
                reached = True
                break
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    n = (cur[0] + dx, cur[1] + dy)
                    if n in mask and n not in seen:
                        seen.add(n)
                        stack.append(n)

    same_bg = (dist(rail, title) <= tol) and (dist(rail, content) <= tol)
    print('  [%s] dpr=%.2f nav=%.2f logical (measured: corner column / dpr) rail=%s title=%s '
          'content=%s same_bg=%s'
          % (tag, dpr, corner_x / dpr, hexs(rail), hexs(title), hexs(content),
             'yes' if same_bg else 'NO'))
    print('  [%s] junction: vertical band starts col %d, spans %d device px, %d run(s) %s'
          % (tag, corner_x, v_band, len(v_runs), ['%d..%d' % (a, b) for (a, b) in v_runs]))
    print('  [%s] junction: horizontal band starts row %d, spans %d device px, %d run(s) %s'
          % (tag, corner_y, h_band, len(h_runs), ['%d..%d' % (a, b) for (a, b) in h_runs]))
    print('  [%s] corner point=(%d,%d) device; corner cut m=%d device px -> radius %.2f device '
          '= %.2f logical px; borders connected=%s'
          % (tag, corner_x, corner_y, m, radius, radius / dpr if radius > 0 else -1,
             'yes' if reached else 'NO'))

    ok = (same_bg and 1 <= len(v_runs) <= 2 and 1 <= len(h_runs) <= 2
          and v_band <= 4 and h_band <= 4 and reached and radius > 0
          and abs(radius / dpr - 10.0) <= 2.5)
    reasons = []
    if not same_bg:
        reasons.append('the three faces are not one background colour')
    if not (1 <= len(v_runs) <= 2) or not (1 <= len(h_runs) <= 2):
        reasons.append('junction has %d vertical / %d horizontal line runs (expected 1..2)'
                       % (len(v_runs), len(h_runs)))
    if v_band > 4 or h_band > 4:
        reasons.append('junction band is %d device px wide (expected <= 4)' % max(v_band, h_band))
    if not reached:
        reasons.append('the two borders are NOT connected through the corner')
    if radius <= 0:
        reasons.append('no corner cut found (nothing rounded)')
    elif abs(radius / dpr - 10.0) > 2.5:
        reasons.append('fitted radius %.2f logical px (library QSS says 10)' % (radius / dpr))
    print('RESULT %s radius_px=%d lines=%d band_px=%d bg=%s ok=%d'
          % (tag, int(round(radius)), len(v_runs), max(v_band, h_band), hexs(content),
             1 if ok else 0))
    for why in reasons:
        print('  [%s] FAIL: %s' % (tag, why))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
'@
Set-Content -Path $scan -Value $scanner -Encoding utf8

# ------------------------------------------------------------------- scenarios
# tag / theme / route / SXCL_UI_SHOT_DELAY / SXCL_UI_WINCHECK
$scenarios = @(
  @{ tag = 'home-dark-48';      theme = 'dark';  route = 'home'; delay = '3000'; wincheck = '' },
  @{ tag = 'home-light-48';     theme = 'light'; route = 'home'; delay = '3000'; wincheck = '' },
  @{ tag = 'home-dark-nav322';  theme = 'dark';  route = 'home'; delay = '2600'; wincheck = 'nav322;wait:1800' },
  @{ tag = 'home-light-nav322'; theme = 'light'; route = 'home'; delay = '2600'; wincheck = 'nav322;wait:1800' },
  @{ tag = 'home-dark-nav48';   theme = 'dark';  route = 'home'; delay = '4200'; wincheck = 'nav322;wait:1200;nav48;wait:1500' },
  @{ tag = 'home-dark-max';     theme = 'dark';  route = 'home'; delay = '3000'; wincheck = 'max;wait:2400' },
  @{ tag = 'home-dark-restore'; theme = 'dark';  route = 'home'; delay = '4300'; wincheck = 'max;wait:1200;normal;wait:1800' },
  @{ tag = 'home-dark-tray';    theme = 'dark';  route = 'home'; delay = '4300'; wincheck = 'min;wait:900;show;wait:1500' }
)

$dpr = '1.5'
$fail = 0
$results = @{}

foreach ($s in $scenarios) {
  $tag = $s.tag
  $png = Join-Path $work ($tag + '.png')
  $err = Join-Path $work ($tag + '.err.txt')
  $out = Join-Path $work ($tag + '.out.txt')
  if (Test-Path $png) { Remove-Item $png -Force }
  $env:SXCL_UI_WINDOW = '1100x750'
  $env:SXCL_UI_ROUTE = $s.route
  $env:SXCL_UI_THEME = $s.theme
  $env:SXCL_UI_SHOT_DELAY = $s.delay
  $env:SXCL_UI_SHOT = $png
  Remove-Item Env:SXCL_UI_SETTINGS -ErrorAction SilentlyContinue
  Remove-Item Env:SXCL_UI_RAILS_TEST -ErrorAction SilentlyContinue
  if ($s.wincheck -eq '') { Remove-Item Env:SXCL_UI_WINCHECK -ErrorAction SilentlyContinue }
  else { $env:SXCL_UI_WINCHECK = $s.wincheck }
  Write-Output ('--- scenario ' + $tag + ' (theme=' + $s.theme + ', route=' + $s.route + ')')
  if ($s.wincheck -ne '') { Write-Output ('    SXCL_UI_WINCHECK=' + $s.wincheck) }
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  if (Test-Path $err) {
    # per-step readouts (window state / geometry / nav step)
    $readout = Select-String -Path $err -Pattern '^\[wincheck\]|^\[rails\]' | Select-Object -First 24
    foreach ($line in $readout) { Write-Output ('    ' + $line.Line.Trim()) }
  }
  if (-not (Test-Path $png)) {
    Write-Output ('    [FAIL] no screenshot: ' + $png)
    $fail++
    continue
  }
  $text = & python $scan $png $tag $dpr
  foreach ($line in $text) { Write-Output $line }
  if ($LASTEXITCODE -ne 0) { $fail++ }
  $res = ($text | Select-String -Pattern '^RESULT ' | Select-Object -First 1)
  if ($res) { $results[$tag] = $res.Line }
}

# ------------------------------------------------------------------- verdict
# The required summary line is the dark/collapsed reading (theme dark, nav 48);
# the light reading follows on its own line.
$dark = $results['home-dark-48']
$light = $results['home-light-48']
$ok = ($fail -eq 0 -and $dark -and $light)
if ($ok) {
  $d = ($dark -split '\s+')
  $l = ($light -split '\s+')
  Write-Output ('CORNER-NATIVE: ALL OK (' + $d[2] + ' ' + $d[3] + ' ' + $d[5] + ')')
  Write-Output ('CORNER-NATIVE: light ' + 'ALL OK (' + $l[2] + ' ' + $l[3] + ' ' + $l[5] + ')')
  Write-Output ('  scenarios: ' + $scenarios.Count + '; results:')
  foreach ($s in $scenarios) { Write-Output ('    ' + $results[$s.tag]) }
  exit 0
}
Write-Output ('CORNER-NATIVE: FAIL (' + $fail + ' scenario(s) failed)')
foreach ($s in $scenarios) { Write-Output ('    ' + $results[$s.tag]) }
exit 1
