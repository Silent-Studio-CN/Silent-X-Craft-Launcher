$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$src = Join-Path $root 'src'
$dst = Join-Path $root 'fixture.zip'
if (Test-Path $dst) { Remove-Item $dst -Force }
Compress-Archive -Path (Join-Path $src '*') -DestinationPath $dst -Force
if (-not (Test-Path $dst)) { exit 3 }
exit 0
