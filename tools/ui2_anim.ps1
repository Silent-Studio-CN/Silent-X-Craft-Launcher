# (C) Silent X Craft Launcher -- animation base acceptance on the REAL shell (docs/27 S13.1).
#
# What this asserts (no eyeballing: every number below is measured):
#   * the real shell runs with SXCL_UI2=1 and, with the verification probe armed
#     (SXCL_UI2_ANIM_DEMO=1), four rows of its LEFT SUB RAIL fade in with Anim::stagger(rows,1200,400);
#   * the shell itself saves two frames -- 30% and 100% of the timeline -- into D:\SilentStudio\_test\ui2;
#   * a pixel scan of both PNGs counts the ink of every row (sum of luminance above the rail
#     background, which is proportional to the row's reveal/alpha), and the conclusion
#     "row N is visible / not visible yet" is derived from those counts, not from looking at the image;
#   * the ink ratios are cross-checked against the reveal values the app printed at the same instant
#     (pixels and state must agree), and the global heartbeat count must be exactly 1.
#
# Output files: D:\SilentStudio\_test\ui2 (shots, shell log, report.txt).
# Last line is either "ANIM-BASE: ALL OK" or "ANIM-BASE: FAIL (n)".
#
# ASCII-only on purpose: Windows PowerShell 5.1 parses .ps1 as ANSI unless the file has a BOM,
# so this file is ASCII + UTF-8 BOM and never contains Chinese.
$ErrorActionPreference = 'Continue'

# hermetic: drop every SXCL_UI_* / SXCL_UI2_* hook this script does not set itself
foreach ($v in @('SXCL_UI_DUMP', 'SXCL_UI_SHOT', 'SXCL_UI_SHOT_DELAY', 'SXCL_UI_ROUTE', 'SXCL_UI_NAV',
                 'SXCL_UI_COLLAPSE', 'SXCL_UI_THEME', 'SXCL_UI_TRACE', 'SXCL_UI_ACCEPT',
                 'SXCL_UI_MOTION', 'SXCL_UI_MOTION_SYSTEM', 'SXCL_UI2_ANIM_TRACE')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\ui2'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$fail = 0
$notes = New-Object System.Collections.Generic.List[string]
function Note([string]$s) { Write-Output $s; $notes.Add($s) }

Note '== SXCL ui2 animation base: real-shell stagger acceptance =='
if (-not (Test-Path $exe)) {
  Note ('  [FAIL] shell not built: ' + $exe)
  Note 'ANIM-BASE: FAIL (1)'
  exit 1
}
Get-ChildItem -Path $work -Filter 'shot_*.png' -ErrorAction SilentlyContinue | Remove-Item -Force
$ini = Join-Path $work 'anim.ini'
Set-Content -Path $ini -Value 'ui.motion=standard' -Encoding utf8

# --- 1) run the real shell (SXCL_UI2=1) with the probe armed -------------------
$env:SXCL_UI2 = '1'
$env:SXCL_UI2_ANIM_DEMO = '1'
$env:SXCL_UI2_ANIM_MS = '1200'
$env:SXCL_UI2_ANIM_STEP = '400'
$env:SXCL_UI2_ANIM_SHOT_DIR = $work
$env:SXCL_UI_MOTION = 'standard'
$env:SXCL_UI_MOTION_SYSTEM = 'standard'
$env:SXCL_UI_WINDOW = '1100x750'
$env:SXCL_UI_SETTINGS = $ini
$out = Join-Path $work 'shell.out'
$err = Join-Path $work 'shell.err'
Remove-Item $out, $err -ErrorAction SilentlyContinue
$proc = Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
if (-not $proc.WaitForExit(60000)) {
  try { $proc.Kill() } catch { }
  Note '  [FAIL] shell did not exit within 60s'
  $fail++
}
Start-Sleep -Milliseconds 200
$log = @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue) +
       @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue)
$log | Set-Content -Path (Join-Path $work 'shell.log') -Encoding utf8
Note ('  [..] shell exit=' + $proc.ExitCode + ' log lines=' + $log.Count)

$rowRe = '^\[ui2-anim-demo\] ROW i=(\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) text=(.*)$'
$shotRe = '^\[ui2-anim-demo\] SHOT tag=(\S+) ok=(\d+) p=([\d.]+) reveal=([\d.,]+) bg=(\d+),(\d+),(\d+) ink=(\d+),(\d+),(\d+) dpr=([\d.]+) file=(.*)$'
$rows = @()
$shots = @{}
foreach ($line in $log) {
  if ($line -match $rowRe) {
    $rows += [pscustomobject]@{ i = [int]$Matches[1]; x = [int]$Matches[2]; y = [int]$Matches[3];
                                w = [int]$Matches[4]; h = [int]$Matches[5]; text = $Matches[6] }
    continue
  }
  if ($line -match $shotRe) {
    $shots[$Matches[1]] = [pscustomobject]@{
      tag = $Matches[1]; ok = [int]$Matches[2]; p = [double]$Matches[3]; reveal = $Matches[4]
      bg = @([int]$Matches[5], [int]$Matches[6], [int]$Matches[7])
      ink = @([int]$Matches[8], [int]$Matches[9], [int]$Matches[10])
      dpr = [double]$Matches[11]; file = $Matches[12] }
  }
}
$startLine = ($log | Select-String -Pattern 'start rows=(\d+) injected=(\d+) ms=(\d+) step=(\d+) total=(\d+) level=(\S+) heartbeats=(\d+) timelines=(\d+)' | Select-Object -First 1)
if (-not $startLine) {
  Note '  [FAIL] probe never started inside the shell (see shell.log)'
  $fail++
} else {
  $startRows = [int]$startLine.Matches[0].Groups[1].Value
  $heartbeats = [int]$startLine.Matches[0].Groups[7].Value
  $timelines = [int]$startLine.Matches[0].Groups[8].Value
  Note ('  [..] timeline: ' + $startLine.Line.Trim())
  if ($heartbeats -eq 1) { Note '  [ok] global heartbeat timers = 1 (all animations share one)' }
  else { Note ('  [FAIL] global heartbeat timers = ' + $heartbeats + ' (must be 1)'); $fail++ }
  if ($startRows -eq 4) { Note '  [ok] 4 visible rows -> 4 tracks' }
  else { Note ('  [FAIL] visible rows = ' + $startRows + ' (want 4)'); $fail++ }
  if ($timelines -eq 1) { Note '  [ok] one single timeline drives all 4 rows' }
  else { Note ('  [FAIL] timelines = ' + $timelines + ' (must be 1)'); $fail++ }
}
if ($rows.Count -ne 4) { Note ('  [FAIL] sub-rail rows found = ' + $rows.Count + ' (want 4)'); $fail++ }
else { Note '  [ok] 4 rows injected into the left sub rail' }
if (-not $shots.ContainsKey('p30') -or -not $shots.ContainsKey('p100')) {
  Note '  [FAIL] both shots were not produced'
  $fail++
  Note 'ANIM-BASE: FAIL'
  $notes | Set-Content -Path (Join-Path $work 'report.txt') -Encoding utf8
  exit 1
}

# --- 2) pixel scan ------------------------------------------------------------
Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue
# Ink of one rectangle = sum of |luminance - background luminance| over every pixel.
# Linear in the row's reveal/alpha, and works for a dark AND a light theme (absolute difference).
function Measure-Ink([string]$path, [int]$x, [int]$y, [int]$w, [int]$h, [double]$bgLum) {
  $bmp = [System.Drawing.Bitmap]::FromFile($path)
  try {
    $x0 = [Math]::Max(0, $x); $y0 = [Math]::Max(0, $y)
    $x1 = [Math]::Min($bmp.Width, $x + $w); $y1 = [Math]::Min($bmp.Height, $y + $h)
    $sum = 0.0; $count = 0
    for ($py = $y0; $py -lt $y1; $py++) {
      for ($px = $x0; $px -lt $x1; $px++) {
        $c = $bmp.GetPixel($px, $py)
        $lum = ($c.R + $c.G + $c.B) / 3.0
        $d = [Math]::Abs($lum - $bgLum)
        if ($d -gt 6.0) { $sum += $d }        # 6 = noise floor (PNG is lossless; only AA edges)
        if ($d -gt 40.0) { $count++ }
      }
    }
    return @{ Sum = $sum; Ink = $count; W = ($x1 - $x0); H = ($y1 - $y0) }
  } finally { $bmp.Dispose() }
}

$bg30 = $shots['p30'].bg
$bgLum = ($bg30[0] + $bg30[1] + $bg30[2]) / 3.0
Note ('  [..] rail background rgb=' + ($bg30 -join ',') + ' (lum ' + [Math]::Round($bgLum, 1) + '), dpr=' + $shots['p30'].dpr)
Note ('  [..] shot p30 progress=' + $shots['p30'].p + ' reveal=' + $shots['p30'].reveal)
Note ('  [..] shot p100 progress=' + $shots['p100'].p + ' reveal=' + $shots['p100'].reveal)
$rv30 = $shots['p30'].reveal.Split(',') | ForEach-Object { [double]$_ }
$rv100 = $shots['p100'].reveal.Split(',') | ForEach-Object { [double]$_ }
$dpr = $shots['p30'].dpr

$inkA = @(); $inkB = @(); $ratio = @()
for ($i = 0; $i -lt $rows.Count; $i++) {
  $r = $rows[$i]
  $x = [int][Math]::Round($r.x * $dpr); $y = [int][Math]::Round($r.y * $dpr)
  $w = [int][Math]::Round($r.w * $dpr); $h = [int][Math]::Round($r.h * $dpr)
  $a = Measure-Ink $shots['p30'].file $x $y $w $h $bgLum
  $b = Measure-Ink $shots['p100'].file $x $y $w $h $bgLum
  $inkA += $a; $inkB += $b
  $rt = 0.0
  if ($b.Sum -gt 0) { $rt = $a.Sum / $b.Sum }
  $ratio += $rt
  $verdict = 'not visible yet'
  if ($rt -ge 0.70) { $verdict = 'VISIBLE' } elseif ($rt -ge 0.15) { $verdict = 'fading in' }
  Note ('  row ' + $i + ' "' + $r.text + '" rect=' + $x + ',' + $y + ' ' + $w + 'x' + $h + 'px' +
        ' | ink 30%=' + [Math]::Round($a.Sum) + ' ink 100%=' + [Math]::Round($b.Sum) +
        ' ratio=' + [Math]::Round($rt, 3) + ' reveal(30%)=' + $rv30[$i] + ' -> ' + $verdict)
}

# --- 3) assertions ------------------------------------------------------------
function Check([bool]$ok, [string]$what) {
  if ($ok) { Note ('  [ok] ' + $what) } else { Note ('  [FAIL] ' + $what); $script:fail++ }
}
Check ([Math]::Abs($shots['p30'].p - 0.30) -le 0.06) ('shot p30 progress = ' + $shots['p30'].p + ' (want 0.30 +-0.06)')
Check ([Math]::Abs($shots['p100'].p - 1.00) -le 0.02) ('shot p100 progress = ' + $shots['p100'].p + ' (want 1.00 +-0.02)')
Check ($shots['p30'].ok -eq 1 -and $shots['p100'].ok -eq 1) 'both PNGs were saved by the shell itself'
$allVisible = $true
foreach ($b in $inkB) { if ($b.Sum -le 0) { $allVisible = $false } }
Check $allVisible 'at 100% every one of the 4 rows has ink (all fully visible)'
Check ($ratio[0] -gt $ratio[1] -and $ratio[1] -gt $ratio[2] -and $ratio[2] -ge 0 -and $ratio[3] -le [Math]::Max(0.15, $ratio[2] + 0.05)) 'stagger order: ink(1) > ink(2) > ink(3) >= ink(4) at 30%'
Check ($ratio[0] -ge 0.70) ('row 0 is clearly visible at 30% (ratio ' + [Math]::Round($ratio[0], 3) + ' >= 0.70)')
Check ($ratio[1] -ge 0.15) ('row 1 has started to appear at 30% (ratio ' + [Math]::Round($ratio[1], 3) + ' >= 0.15)')
Check ($ratio[2] -le 0.15 -and $ratio[3] -le 0.15) ('rows 2 and 3 are NOT visible yet at 30% (ratios ' + [Math]::Round($ratio[2], 3) + ' / ' + [Math]::Round($ratio[3], 3) + ')')
$pixelMatch = $true
for ($i = 0; $i -lt 4; $i++) {
  if ([Math]::Abs($ratio[$i] - $rv30[$i]) -gt 0.12) { $pixelMatch = $false }
}
Check $pixelMatch 'pixel ratios agree with the reveal values printed at the same instant (|diff| <= 0.12)'

if ($fail -eq 0) { Note 'ANIM-BASE: ALL OK' } else { Note ('ANIM-BASE: FAIL (' + $fail + ')') }
$notes | Set-Content -Path (Join-Path $work 'report.txt') -Encoding utf8
if ($fail -eq 0) { exit 0 } else { exit 1 }
