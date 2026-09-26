# (C) Silent X Craft Launcher -- rail geometry assertions (docs/27 S12). ASCII-only on purpose:
# Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM, so Chinese here would break it.
$ErrorActionPreference = 'Continue'
$exe = Join-Path $PSScriptRoot '..\build-ui\src\ui\Release\sxcl-ui.exe'
$iniPath = Join-Path $env:TEMP 'sxcl_rail_assert.ini'
Set-Content -Path $iniPath -Value ('game.default_dir=' + $env:TEMP + '\.minecraft') -Encoding utf8
$fail = 0
foreach ($size in @('900x600','1100x750','1280x800')) {
  $env:SXCL_UI_WINDOW = $size
  $env:SXCL_UI_ROUTE = 'select'
  $env:SXCL_UI_SETTINGS = $iniPath
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_SHOT = (Join-Path $env:TEMP ('rail_' + $size + '.png'))
  $env:SXCL_UI_SHOT_DELAY = '5000'
  $out = Join-Path $env:TEMP ('rail_dump_' + $size + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $txt = @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) + @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  $page = ($txt | Select-String -Pattern 'sxclPage_select \((\d+),(\d+) (\d+)x(\d+)\)' | Select-Object -First 1)
  $nav = ($txt | Select-String -Pattern 'sxclVersionFolderNav \((\d+),(\d+) (\d+)x(\d+)\)' | Select-Object -First 1)
  if ((-not $page) -or (-not $nav)) { Write-Output ('  [' + $size + '] dump lines not found -> FAIL'); $fail++; continue }
  $pageLine = $page.Line -replace '\s+',' '
  $navLine = $nav.Line -replace '\s+',' '
  $null = ($page.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
  $px = [int]$Matches[1]; $py = [int]$Matches[2]
  $null = ($nav.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
  $nx = [int]$Matches[1]; $ny = [int]$Matches[2]
  $dx = $nx - $px; $dy = $ny - $py
  $ok = ($dx -le 1) -and ($dy -le 2)
  Write-Output ('  [' + $size + '] page=' + $pageLine + ' | sub=' + $navLine + ' | dx=' + $dx + ' dy=' + $dy + ' -> ' + $(if ($ok) { 'OK' } else { 'FAIL' }))
  if (-not $ok) { $fail++ }
}
if ($fail -eq 0) { Write-Output 'RAIL-GEOMETRY: ALL OK' } else { Write-Output ('RAIL-GEOMETRY: ' + $fail + ' FAILED'); exit 1 }