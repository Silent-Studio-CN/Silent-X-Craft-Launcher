# (C) Silent X Craft Launcher -- network layer benchmark (before/after table).
#
# What it measures (every number comes off this machine, nothing is guessed):
#   * 5 real queries  : 2x Modrinth search API + 2x CurseForge mirror search + 1 version manifest
#   * 20 real icons   : icon URLs taken from the live responses (10 Modrinth + 10 CurseForge mirror)
#   * 2 passes        : pass 1 = cold, pass 2 = second visit ("0 requests from cache" evidence)
#   * one case == the product path: its own transport + its own thread per request
#     (this is what mods_worker.cpp does: a new QThread and a new transport per request)
#
# The harness is src/net/net_bench_main.cpp, built into a scratch project under
# build-ui/net_bench (gitignored, never touches the repo CMakeLists).
#
#   -CaptureBaseline : run with SXCL_NET_LEGACY=1 (the pre-2026-09-27 behaviour) and store it
#                      as the "before" column. Later runs print before/after side by side.
#
# ASCII-only + UTF-8 BOM on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI otherwise).
param(
  [string]$Label = 'after',
  [int]$Passes = 2,
  [int]$TimeoutMs = 15000,
  [switch]$CaptureBaseline,
  [switch]$SkipBuild,
  [switch]$Prewarm,
  [string]$Plan = ''
)
$ErrorActionPreference = 'Stop'
$T = [char]9
$NL = [char]10
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$work = 'D:\SilentStudio\_test\net_bench'
$scratch = Join-Path $root 'build-ui\net_bench'
$outDir = Join-Path $scratch 'out'
$exe = Join-Path $outDir 'Release\net_bench.exe'
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Say([string]$text) { Write-Output $text }

# ---------------------------------------------------------------- Qt prefix + generator (from build-ui)
$cache = Join-Path $root 'build-ui\CMakeCache.txt'
if (-not (Test-Path $cache)) { throw 'build-ui\CMakeCache.txt not found -- configure build-ui first' }
$qtDir = (Select-String -Path $cache -Pattern '^Qt6_DIR:PATH=(.+)$').Matches[0].Groups[1].Value.Trim()
$qtPrefix = ($qtDir -replace '/lib/cmake/Qt6$', '')
$gen = (Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$').Matches[0].Groups[1].Value.Trim()
$plat = 'x64'
$platLine = Select-String -Path $cache -Pattern '^CMAKE_GENERATOR_PLATFORM:INTERNAL=(.+)$'
if ($platLine) { $plat = $platLine.Matches[0].Groups[1].Value.Trim() }
if (-not $plat) { $plat = 'x64' }
$qtBin = Join-Path $qtPrefix 'bin'
Say ('QTN=' + $qtPrefix)
Say ('GEN=' + $gen + ' PLAT=' + $plat)

# ---------------------------------------------------------------- scratch project (gitignored)
if (-not $SkipBuild) {
  New-Item -ItemType Directory -Force -Path $scratch | Out-Null
  $srcPath = $root -replace '\\', '/'
  $cmakeText = @"
cmake_minimum_required(VERSION 3.24)
project(sxcl_net_bench LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
find_package(Qt6 REQUIRED COMPONENTS Network)
add_executable(net_bench "$srcPath/src/net/net_bench_main.cpp" "$srcPath/src/net/transport_qt.cpp")
target_include_directories(net_bench PRIVATE "$srcPath/include")
if(MSVC)
  target_compile_options(net_bench PRIVATE /utf-8 /W4)
endif()
target_link_libraries(net_bench PRIVATE Qt6::Network)
"@
  Set-Content -Path (Join-Path $scratch 'CMakeLists.txt') -Value $cmakeText -Encoding utf8
  & cmake -S $scratch -B $outDir -G $gen -A $plat -DCMAKE_PREFIX_PATH="$qtPrefix" -DCMAKE_CONFIGURATION_TYPES=Release | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
  & cmake --build $outDir --config Release --target net_bench | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }
}
if (-not (Test-Path $exe)) { throw ('net_bench.exe not built: ' + $exe) }

# ---------------------------------------------------------------- case plan (fixed set)
$planFile = Join-Path $work 'plan.tsv'
$iconFile = Join-Path $work 'icons.txt'
if (-not (Test-Path $iconFile)) {
  $icons = New-Object System.Collections.Generic.List[string]
  try {
    $mr = Invoke-RestMethod -Uri 'https://api.modrinth.com/v2/search?query=optimization&limit=12' -TimeoutSec 20
    foreach ($hit in $mr.hits) {
      if ($hit.icon_url -and $icons.Count -lt 10) { $icons.Add(($hit.icon_url -replace ' ', '%20')) }
    }
  } catch { Say ('WARN modrinth icon harvest failed: ' + $_.Exception.Message) }
  try {
    $cf = Invoke-RestMethod -Uri 'https://mod.mcimirror.top/curseforge/v1/mods/search?gameId=432&index=0&pageSize=12&classId=6' -TimeoutSec 20
    foreach ($mod in $cf.data) {
      if ($icons.Count -ge 20) { break }
      $u = $null
      if ($mod.logo) { $u = $mod.logo.thumbnailUrl; if (-not $u) { $u = $mod.logo.url } }
      if ($u) { $icons.Add(($u -replace ' ', '%20')) }
    }
  } catch { Say ('WARN curseforge icon harvest failed: ' + $_.Exception.Message) }
  if ($icons.Count -lt 20) { throw ('only ' + $icons.Count + ' icon urls harvested, 20 needed') }
  Set-Content -Path $iconFile -Value ($icons -join $NL) -Encoding ascii
}
$icons = @(Get-Content $iconFile | Where-Object { $_.Trim() -ne '' })

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add(('# suite' + $T + 'name' + $T + 'url' + $T + 'headers'))
$lines.Add('search' + $T + 'modrinth-jei' + $T + 'https://api.modrinth.com/v2/search?query=jei&limit=20')
$lines.Add('search' + $T + 'modrinth-sodium' + $T + 'https://api.modrinth.com/v2/search?query=sodium&limit=20')
$lines.Add('search' + $T + 'cf-mirror-optimization' + $T + 'https://mod.mcimirror.top/curseforge/v1/mods/search?gameId=432&index=0&pageSize=20&classId=6')
$lines.Add('search' + $T + 'cf-mirror-jei' + $T + 'https://mod.mcimirror.top/curseforge/v1/mods/search?gameId=432&index=0&pageSize=20&classId=6&searchFilter=jei')
$lines.Add('manifest' + $T + 'version-manifest-v2' + $T + 'https://piston-meta.mojang.com/mc/game/version_manifest_v2.json')
$i = 0
foreach ($u in $icons) {
  $i++
  $lines.Add('icon' + $T + ('icon-' + $i.ToString('00')) + $T + $u)
}
Set-Content -Path $planFile -Value ($lines -join $NL) -Encoding ascii
$iconPlan = Join-Path $work 'plan_icons.tsv'
$iconLines = @($lines | Where-Object { $_ -like 'icon*' })
Set-Content -Path $iconPlan -Value ($iconLines -join $NL) -Encoding ascii
Say ('CASE plan=' + $planFile + ' cases=' + $lines.Count + ' icon-cases=' + $iconLines.Count)

# ---------------------------------------------------------------- run
if ($Plan -ne '') { $planFile = $Plan }
$tag = $Label
if ($CaptureBaseline) { $tag = 'baseline'; $env:SXCL_NET_LEGACY = '1' } else { Remove-Item Env:SXCL_NET_LEGACY -ErrorAction SilentlyContinue }
$env:SXCL_NET_TRACE = '1'
$env:SXCL_NET_CACHE_DIR = (Join-Path $work 'cache')
$prewarmFlag = 0
if ($Prewarm) { $prewarmFlag = 1 }
$raw = Join-Path $work ('raw_' + $tag + '.tsv')
$rawBatch = Join-Path $work ('raw_' + $tag + '_batch.tsv')
$errFile = Join-Path $work ('raw_' + $tag + '.err')
$env:PATH = $qtBin + ';' + $env:PATH
# run A: per-case table + second-visit cache evidence.
# Pass 1 must be COLD: wipe the cache dir first (a leftover disk entry would fake a hit).
Remove-Item -Recurse -Force $env:SXCL_NET_CACHE_DIR -ErrorAction SilentlyContinue
$sw = [System.Diagnostics.Stopwatch]::StartNew()
# prewarm-wait-ms = the gap between "the mods page opens" (prewarm starts) and "the user clicks
# search". The baseline has no prewarm at all, so this wait cannot flatter it or hurt it -- it only
# gives the optimized path the same head start the product gives it. Measured on this machine, the
# TLS handshake to api.modrinth.com alone needs 0.3 s on a good moment and ~4 s on a bad one, hence
# 2.5 s here (see the report: 1.5 s leaves the first Modrinth query still handshaking).
& $exe --plan $planFile --passes $Passes --batch 0 --label $tag --prewarm $prewarmFlag --prewarm-wait-ms 2500 --timeout-ms $TimeoutMs > $raw 2> $errFile
$swA = $sw.ElapsedMilliseconds
# run B: the icon batch (cold/warm) in its own process, own cache dir, so "cold" is really cold
$env:SXCL_NET_CACHE_DIR = (Join-Path $work 'cache_batch')
Remove-Item -Recurse -Force $env:SXCL_NET_CACHE_DIR -ErrorAction SilentlyContinue
& $exe --plan $iconPlan --passes 1 --batch 1 --label ($tag + '-batch') --prewarm 1 --prewarm-wait-ms 2500 --timeout-ms $TimeoutMs > $rawBatch 2> ($rawBatch + '.err')
$sw.Stop()
Remove-Item Env:SXCL_NET_LEGACY -ErrorAction SilentlyContinue
Say ('RUN label=' + $tag + ' table=' + $swA + 'ms total=' + $sw.ElapsedMilliseconds + 'ms raw=' + $raw)
if ($CaptureBaseline) {
  Copy-Item -Force $raw (Join-Path $work 'baseline.tsv')
  Copy-Item -Force $rawBatch (Join-Path $work 'baseline_batch.tsv')
  Copy-Item -Force $raw (Join-Path $scratch 'baseline.tsv')
  Copy-Item -Force $rawBatch (Join-Path $scratch 'baseline_batch.tsv')
}

# ---------------------------------------------------------------- parse
function Read-Rows([string]$path) {
  $rows = @{}
  $stats = @{}
  $batches = @{}
  foreach ($line in (Get-Content $path -ErrorAction SilentlyContinue)) {
    $f = $line -split $T
    if ($f.Count -ge 2 -and $f[0] -eq 'SAMPLE') {
      $key = $f[1] + '|' + $f[3]
      $rows[$key] = [pscustomobject]@{
        pass = [int]$f[1]; suite = $f[2]; name = $f[3]; ok = [int]$f[4]; status = [int]$f[5]
        dns = [int]$f[6]; conn = [int]$f[7]; ttfb = [int]$f[8]; total = [int]$f[9]
        reused = [int]$f[10]; src = [int]$f[11]; wire = [long]$f[12]; body = [long]$f[13]
        enc = $f[14]; http2 = [int]$f[15]; tries = [int]$f[16]; magic = $f[17]
      }
    } elseif ($f.Count -ge 4 -and $f[0] -eq 'BATCH') {
      $bkey = $f[1] + '|' + $f[2]
      $batches[$bkey] = [pscustomobject]@{
        pass = [int]$f[1]; suite = $f[2]; wall = [long]$f[3]; ok = [int]$f[4]; cases = [int]$f[5]
        requests = [long]$f[6]; mem = [long]$f[7]; disk = [long]$f[8]; body = [long]$f[9]
      }
    } elseif ($f.Count -ge 3 -and $f[0] -eq 'STATS') {
      $key = $f[1] + '|' + $f[2]
      $stats[$key] = [pscustomobject]@{
        requests = [long]$f[3]; mem = [long]$f[4]; disk = [long]$f[5]; stores = [long]$f[6]
        prewarm_ok = [long]$f[7]; prewarm_fail = [long]$f[8]; retries = [long]$f[9]
        throttled = [long]$f[10]; wire = [long]$f[11]; body = [long]$f[12]
      }
    }
  }
  return @{ rows = $rows; stats = $stats; batches = $batches }
}

function Median($values) {
  $v = @($values | Sort-Object)
  if ($v.Count -eq 0) { return 0 }
  $mid = [int][math]::Floor($v.Count / 2)
  if ($v.Count % 2 -eq 1) { return $v[$mid] }
  return [int](($v[$mid - 1] + $v[$mid]) / 2)
}

$now = Read-Rows $raw
$nowBatch = Read-Rows $rawBatch
foreach ($k in $nowBatch.batches.Keys) { $now.batches[$k] = $nowBatch.batches[$k] }
$base = $null
$basePath = Join-Path $work 'baseline.tsv'
if ((Test-Path $basePath) -and (-not $CaptureBaseline)) {
  $base = Read-Rows $basePath
  $baseBatchPath = Join-Path $work 'baseline_batch.tsv'
  if (Test-Path $baseBatchPath) {
    $bb = Read-Rows $baseBatchPath
    foreach ($k in $bb.batches.Keys) { $base.batches[$k] = $bb.batches[$k] }
  }
}

# ---------------------------------------------------------------- per-case table
Say ''
Say '== per-case (pass 1 = cold; ms = that request wall clock, product path) =='
$fmt = '{0,-9} {1,-22} {2,8} {3,8} {4,8} {5,8} {6,7} {7,6} {8,4}'
Say ($fmt -f 'suite', 'case', 'before', 'after', 'delta', 'speedup', 'status', 'cache', 'h2')
$names = @($now.rows.Values | Where-Object { $_.pass -eq 1 } | Sort-Object { $_.suite + $_.name })
foreach ($r in $names) {
  $key = '1|' + $r.name
  $b = 0
  if ($base -and $base.rows.ContainsKey($key)) { $b = $base.rows[$key].total }
  $d = $r.total - $b
  $sp = '-'
  if ($b -gt 0 -and $r.total -gt 0) { $sp = [string][math]::Round(($b / [double]$r.total), 2) + 'x' }
  Say ($fmt -f $r.suite, $r.name, $b, $r.total, $d, $sp, $r.status, $r.src, $r.http2)
}

# ---------------------------------------------------------------- suite summary
Say ''
Say '== suite summary (pass 1) =='
$fmt2 = '{0,-9} {1,6} {2,9} {3,9} {4,9} {5,9} {6,9} {7,9}'
Say ($fmt2 -f 'suite', 'cases', 'median-b', 'median-a', 'sum-b', 'sum-a', 'ok-rate', 'wire-a')
foreach ($suite in @('search', 'manifest', 'icon')) {
  $rows = @($names | Where-Object { $_.suite -eq $suite })
  if ($rows.Count -eq 0) { continue }
  $bList = @()
  foreach ($r in $rows) {
    $key = '1|' + $r.name
    if ($base -and $base.rows.ContainsKey($key)) { $bList += $base.rows[$key].total }
  }
  $okRate = [math]::Round(100.0 * (@($rows | Where-Object { $_.ok -eq 1 }).Count) / $rows.Count, 1)
  Say ($fmt2 -f $suite, $rows.Count, (Median $bList), (Median @($rows | ForEach-Object { $_.total })),
        (($bList | Measure-Object -Sum).Sum), (($rows | ForEach-Object { $_.total } | Measure-Object -Sum).Sum),
        ($okRate.ToString() + '%'), (($rows | ForEach-Object { $_.wire } | Measure-Object -Sum).Sum))
}

# ---------------------------------------------------------------- icon batch (parallel, like the page)
Say ''
Say '== icon batch: all 20 fetched at the same time (the page does exactly this) =='
$fmt3 = '{0,-14} {1,8} {2,6} {3,9} {4,9} {5,9} {6,9}'
Say ($fmt3 -f 'batch', 'wall-ms', 'ok', 'requests', 'mem-hit', 'disk-hit', 'bytes')
function Show-Batch([string]$label, $b) {
  if ($b -eq $null) { Say ($fmt3 -f $label, '-', '-', '-', '-', '-', '-'); return }
  Say ($fmt3 -f $label, $b.wall, ($b.ok.ToString() + '/' + $b.cases), $b.requests, $b.mem, $b.disk, $b.body)
}
Show-Batch 'cold-before' $(if ($base) { $base.batches['1|icon-cold'] } else { $null })
Show-Batch 'cold-after' $now.batches['1|icon-cold']
Show-Batch 'warm-before' $(if ($base) { $base.batches['2|icon-warm'] } else { $null })
Show-Batch 'warm-after' $now.batches['2|icon-warm']
$bc = $now.batches['1|icon-cold']
$bw = $now.batches['2|icon-warm']
if ($bc -and $bw) {
  if ($bw.requests -eq 0) { Say ('ICON-BATCH: PASS (cold ' + $bc.requests + ' requests, warm 0 requests)') }
  else { Say ('ICON-BATCH: FAIL (warm batch still issued ' + $bw.requests + ' network requests)') }
}

# ---------------------------------------------------------------- second visit evidence
Say ''
Say '== second visit: cache evidence =='
$s1 = $now.stats['1|icon']
$s2 = $now.stats['2|icon']
$q1 = $now.stats['1|search']
$q2 = $now.stats['2|search']
if ($s1 -and $s2) {
  Say ('ICON pass1 requests=' + $s1.requests + ' cache_mem=' + $s1.mem + ' cache_disk=' + $s1.disk + ' stored=' + $s1.stores)
  Say ('ICON pass2 requests=' + $s2.requests + ' cache_mem=' + $s2.mem + ' cache_disk=' + $s2.disk)
  if ($s2.requests -eq 0 -and (($s2.mem + $s2.disk) -ge 20)) { Say 'ICON-CACHE: PASS (second visit issued 0 network requests)' }
  else { Say 'ICON-CACHE: FAIL (second visit still used the network)' }
}
if ($q1 -and $q2) {
  Say ('QUERY pass1 requests=' + $q1.requests + ' cache_mem=' + $q1.mem)
  Say ('QUERY pass2 requests=' + $q2.requests + ' cache_mem=' + $q2.mem)
  if ($q2.requests -eq 0) { Say 'QUERY-CACHE: PASS (repeated query within 60 s issued 0 network requests)' }
  else { Say 'QUERY-CACHE: FAIL' }
}
$p1 = @($now.rows.Values | Where-Object { $_.pass -eq 1 })
$reuse = @($p1 | Where-Object { $_.reused -eq 1 }).Count
$h2 = @($p1 | Where-Object { $_.http2 -eq 1 }).Count
Say ('REUSE pass1: reused-connection cases=' + $reuse + '/' + $p1.Count + '  http2 cases=' + $h2)
Say ('COMPRESS pass1: gzip cases=' + @($p1 | Where-Object { $_.enc -ne '-' }).Count + ' wire=' + (($p1 | ForEach-Object { $_.wire } | Measure-Object -Sum).Sum) + ' body=' + (($p1 | ForEach-Object { $_.body } | Measure-Object -Sum).Sum))
Say ('NETBENCH-DONE label=' + $tag)
exit 0
