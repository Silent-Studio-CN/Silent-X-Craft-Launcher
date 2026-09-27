# (C) Silent X Craft Launcher -- global "nothing gets squeezed" acceptance.
# User rule (2026-09-27): every control must be able to show itself completely;
# a layout may wrap, but must NEVER cut a control's text -- unless the window is
# smaller than the control itself.  This script walks every route at the window
# minimum size and fails if any *visible* widget reports CUT-W / CUT-H.
$root = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C'
$exe  = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\no_cut'
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null
if (-not (Test-Path $exe)) { Write-Output "NO-CUT: FAIL (exe not found)"; exit 1 }
$ini = Join-Path $work 'settings.ini'
Set-Content -Path $ini -Value 'game.default_dir=' -Encoding utf8
$routes = @('home','download','team','multiplayer','more','settings','select','versions')
$sizes  = @('900x600','1100x750')
$bad = 0
$total = 0
foreach ($size in $sizes) {
  foreach ($route in $routes) {
    $env:SXCL_UI_SETTINGS = $ini
    $env:SXCL_UI_WINDOW = $size
    $env:SXCL_UI_ROUTE = $route
    $env:SXCL_UI_THEME = 'dark'
    $env:SXCL_UI_DUMP = '1'
    $env:SXCL_UI_DUMP_DEPTH = '12'
    $env:SXCL_UI_SHOT = (Join-Path $work ("shot_" + $route + "_" + $size + ".png"))
    $out = Join-Path $work ("dump_" + $route + "_" + $size + ".txt")
    $err = $out + '.err'
    Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
    $hits = @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue | Select-String -Pattern 'CUT-' | ForEach-Object { $_.Line.Trim() } | Where-Object { $_ -notmatch ' hidden ' })
    $total++
    if ($hits.Count -gt 0) {
      $bad++
      Write-Output ("== CUT " + $route + " " + $size + " (" + $hits.Count + ") ==")
      $hits | Select-Object -First 6 | ForEach-Object { Write-Output ("   " + $_) }
    } else {
      Write-Output ("   ok " + $route + " " + $size)
    }
  }
}
Write-Output ""
Write-Output ("NO-CUT: " + $(if ($bad -eq 0) { "ALL OK" } else { "FAIL" }) + " (" + ($total - $bad) + "/" + $total + " route-size combinations with zero visible cut widgets)")
if ($bad -ne 0) { exit 1 }