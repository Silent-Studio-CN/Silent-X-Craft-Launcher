# (C) Silent X Craft Launcher -- minimum window size acceptance (2026-09-27).
#
# User rule: the launcher must refuse to shrink past a floor ("给它设个最小比例,
# 小到什么程度就不能再缩小").  SXCL_UI_RESIZE=<w>x<h> resizes through the PRODUCT
# path (it does NOT clear setMinimumSize, unlike SXCL_UI_WINDOW which exists to
# emulate a phone screen), so the reported "actual" size is what a user gets.
$root = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C'
$exe  = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\min_size'
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null
if (-not (Test-Path $exe)) { Write-Output "MIN-SIZE: FAIL (exe not found)"; exit 1 }
$ini = Join-Path $work 'settings.ini'
Set-Content -Path $ini -Value 'game.default_dir=' -Encoding utf8

$cases = @(
  @{ ask = '700x450';  want = '900x600';  why = 'smaller than the floor -> must stay at the floor' },
  @{ ask = '500x300';  want = '900x600';  why = 'far smaller than the floor -> must stay at the floor' },
  @{ ask = '900x600';  want = '900x600';  why = 'exactly the floor -> allowed' },
  @{ ask = '1200x800'; want = '1200x800'; why = 'larger than the floor -> the user keeps it' }
)
$pass = 0; $fail = 0
foreach ($c in $cases) {
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_RESIZE = $c.ask
  $env:SXCL_UI_ROUTE = 'home'
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_SHOT = (Join-Path $work ('shot_' + ($c.ask -replace '[^0-9A-Za-z]','_') + '.png'))
  $out = Join-Path $work ('dump_' + ($c.ask -replace '[^0-9A-Za-z]','_') + '.txt')
  $err = $out + '.err'
  $ran = $false
  for ($try = 0; $try -lt 8 -and -not $ran; $try++) {
    try {
      Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
      $ran = $true
    } catch {
      Start-Sleep -Seconds 5   # another agent is relinking the exe; wait for a gap
    }
  }
  if (-not $ran) { Write-Output ("  FAIL " + $c.ask + ": exe not runnable (a build kept relinking it)"); $fail++; continue }
  $line = (Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue | Select-String -Pattern 'resize ' | Select-Object -Last 1)
  if ($line -eq $null) { Write-Output ("  FAIL " + $c.ask + ": no resize evidence line"); $fail++; continue }
  $text = $line.Line.Trim()
  $m = [regex]::Match($text, 'resize \S+=(\d+)x(\d+) \S+=(\d+)x(\d+) \S+=(\d+)x(\d+)')
  if (-not $m.Success) { Write-Output ("  FAIL " + $c.ask + ": cannot parse '" + $text + "'"); $fail++; continue }
  $actual = $m.Groups[3].Value + 'x' + $m.Groups[4].Value
  $floor  = $m.Groups[5].Value + 'x' + $m.Groups[6].Value
  if ($actual -eq $c.want) {
    Write-Output ("  ok   ask " + $c.ask + " -> actual " + $actual + " (floor " + $floor + "): " + $c.why)
    $pass++
  } else {
    Write-Output ("  FAIL ask " + $c.ask + " -> actual " + $actual + " (floor " + $floor + "), want " + $c.want + ": " + $c.why)
    $fail++
  }
}
Write-Output ""
$verdict = if ($fail -eq 0) { 'ALL OK' } else { 'FAIL' }
Write-Output ("MIN-SIZE: " + $verdict + " (" + $pass + "/" + ($pass + $fail) + " cases; floor holds, larger sizes are kept)")
if ($fail -ne 0) { exit 1 }