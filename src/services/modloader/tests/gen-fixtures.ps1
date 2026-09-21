# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.

$ErrorActionPreference = 'Stop'
$T = $PSScriptRoot
$ROOT = Split-Path (Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent) -Parent
$F = Join-Path $ROOT 'build-b2/fixtures'
$A = Join-Path (Split-Path $ROOT -Parent) 'Silent-X-Craft-Launcher/android/app/src/main/assets/keymaps'
New-Item -ItemType Directory -Force -Path $T | Out-Null
$LF = [string][char]10
$TAB = [string][char]9
$CR = [string][char]13

function Get-Raw([string]$name) { return (Get-Content (Join-Path $F $name) -Raw) }
function Esc-C([string]$s) {
  return $s.Replace('\','\\').Replace('"','\"').Replace($TAB,'\t').Replace($CR,'').Replace($LF,'\n')
}
$script:sb = $null
function Start-File([string]$header) {
  $script:sb = New-Object System.Text.StringBuilder
  [void]$script:sb.AppendLine($header)
}
function Emit-C([string]$name, [string]$text, [string]$comment) {
  if ($comment) { [void]$script:sb.AppendLine('/* ' + $comment + ' */') }
  [void]$script:sb.AppendLine('static const char ' + $name + '[] =')
  $lines = $text -split $LF
  $emitted = 0
  for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    $isLast = ($i -eq $lines.Count - 1)
    if ($isLast -and $line -eq '') { break }   # 文件本来就没有末尾换行,别凭空补一个
    $esc = Esc-C $line
    if (-not $isLast) { $esc = $esc + '\n' }
    if ($esc.Length -le 900) {
      [void]$script:sb.AppendLine('    "' + $esc + '"')
    } else {
      # 长行(比如 BMCLAPI 的单行 JSON)要切开:切点绝不能落在反斜杠后面,
      # 否则那个反斜杠会把字符串的收尾引号转义掉(踩过:error C2001 字符串字面量中的换行符)。
      $k = 0
      while ($k -lt $esc.Length) {
        $end = [Math]::Min($k + 400, $esc.Length)
        while ($end -gt ($k + 1) -and $esc[$end - 1] -eq [char]92) { $end-- }
        [void]$script:sb.AppendLine('    "' + $esc.Substring($k, $end - $k) + '"')
        $k = $end
      }
    }
    $emitted++
  }
  if ($emitted -eq 0) { [void]$script:sb.AppendLine('    ""') }
  [void]$script:sb.AppendLine('    ;')
  [void]$script:sb.AppendLine('')
}
function Save-File([string]$path) {
  $utf8 = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText($path, $script:sb.ToString(), $utf8)
}
function Split-Lines([string]$raw) { return ($raw -split $LF) }
function Find-Line([string[]]$lines, [string]$pattern, [int]$start = 0) {
  for ($i = $start; $i -lt $lines.Count; $i++) { if ($lines[$i].Contains($pattern)) { return $i } }
  return -1
}
function Line-Slice([string[]]$lines, [int]$from, [int]$count) {
  $to = [Math]::Min($lines.Count - 1, $from + $count - 1)
  return (($lines[$from..$to]) -join $LF)
}
function Top-Level-Objects([string]$raw) {
  $ranges = New-Object System.Collections.Generic.List[int[]]
  $depth = 0; $inStr = $false; $esc = $false; $start = -1; $seenArray = $false
  for ($i = 0; $i -lt $raw.Length; $i++) {
    $c = $raw[$i]
    if ($inStr) {
      if ($esc) { $esc = $false }
      elseif ($c -eq [char]92) { $esc = $true }
      elseif ($c -eq [char]34) { $inStr = $false }
      continue
    }
    if ($c -eq [char]34) { $inStr = $true; continue }
    if ($c -eq '[' -and -not $seenArray) { $seenArray = $true; continue }
    if ($c -eq '{') { if ($depth -eq 0) { $start = $i }; $depth++ }
    elseif ($c -eq '}') { $depth--; if ($depth -eq 0 -and $start -ge 0) { $ranges.Add(@($start, $i)); $start = -1 } }
  }
  return $ranges
}
function Json-Items([string]$raw, [int]$first, [int]$count) {
  $r = Top-Level-Objects $raw
  $parts = @()
  for ($k = $first; $k -lt [Math]::Min($first + $count, $r.Count); $k++) {
    $parts += $raw.Substring($r[$k][0], $r[$k][1] - $r[$k][0] + 1)
  }
  return ('[' + ($parts -join ',') + ']')
}

Start-File @'
/* 本文件由 tests/gen-fixtures.ps1 从**真实网络响应**切片生成,请勿手改。
 * 每个片段都注明来源 URL 与抓取日期;测试不联网,全部用这些字符串喂进去。
 * 生成的 .inc 是纯文本(UTF-8),随测试一起进仓库;原始响应留在 build-b2/fixtures/(gitignored)。
 */
'@

$forge = Get-Raw 'forge-maven-metadata.xml'
$fl = Split-Lines $forge
Emit-C 'k_forge_xml_head' (Line-Slice $fl 0 13) 'https://maven.minecraftforge.net/net/minecraftforge/forge/maven-metadata.xml 前 13 行,逐字节原样(2026-09-17 抓取)'
$i1201 = Find-Line $fl '<version>1.20.1-47.4.5</version>'
Emit-C 'k_forge_xml_1201' (Line-Slice $fl ($i1201 - 1) 9) ('同上第 ' + ($i1201) + ' 行起连续 9 行(1.20.1 段),原样')
$iold = Find-Line $fl '<version>1.12.2-14.23.5.2860</version>'
Emit-C 'k_forge_xml_old' (Line-Slice $fl ($iold - 1) 4) ('同上第 ' + ($iold) + ' 行起连续 4 行(1.12.2 老版本段),原样')
$ilast = Find-Line $fl '<lastUpdated>'
Emit-C 'k_forge_xml_tail' (Line-Slice $fl ($ilast - 3) 5) '同上末尾 5 行(含 <lastUpdated>),原样'

$neo = Get-Raw 'neoforge-mirror.xml'
$nl = Split-Lines $neo
Emit-C 'k_neo_xml_head' (Line-Slice $nl 0 13) 'BMCLAPI 代理的 maven.neoforged.net/.../neoforge/maven-metadata.xml 前 13 行(2026-09-17;官方域名 TLS 握手在本机失败,故取镜像,文档内容一致)'
$i211 = Find-Line $nl '<version>21.1.72</version>'
Emit-C 'k_neo_xml_211' (Line-Slice $nl ($i211 - 2) 7) ('同上第 ' + ($i211) + ' 行附近连续 7 行(21.1.x 与 21.2.x/21.3.x-beta),原样')

$fabric_mc = Get-Raw 'fabric-loader-1.20.1.json'
Emit-C 'k_fabric_json_mc' (Json-Items $fabric_mc 0 1) 'https://meta.fabricmc.net/v2/versions/loader/1.20.1 的第 1 项(含 launcherMeta,项内逐字节原样;外层 [] 是切片补的,原响应本就是数组)(2026-09-17)'
Emit-C 'k_fabric_json_mc_old' (Json-Items $fabric_mc 2 1) '同上前 4 项里的第 3 项(loader 0.19.3, stable=false),项内原样'

$quilt = Get-Raw 'quilt-loader.json'
Emit-C 'k_quilt_json_mc' (Json-Items $quilt 0 1) 'https://meta.quiltmc.org/v3/versions/loader/1.20.1 的第 1 项(0.20.0-beta.9,项内原样)(2026-09-17)'
Emit-C 'k_quilt_json_mc2' (Json-Items $quilt 1 2) '同上第 2-3 项,项内原样'

$ofhtml = Get-Raw 'optifine-downloads.xml'
$ol = Split-Lines $ofhtml
$ij9 = Find-Line $ol 'OptiFine HD U J9'
Emit-C 'k_optifine_html' (Line-Slice $ol ($ij9 - 3) 24) 'https://optifine.net/downloads 里 HD U J9 那行 tr.downloadLine 附近的连续 24 行(官方页面是 HTML,不是严格 XML;原样抓取)(2026-09-17)'
$inull = Find-Line $ol 'Forge N/A'
Emit-C 'k_optifine_html_preview' (Line-Slice $ol ($inull - 6) 9) '同上(2026-09-17):colForge=Forge N/A 的预览行附近连续 9 行,原样'
Emit-C 'k_optifine_json' (Get-Raw 'bmcl-optifine-1.20.1.json') 'https://bmclapi2.bangbang93.com/optifine/1.20.1 全量响应(BMCLAPI 是 OptiFine 唯一的接口;官方 optifine.net/downloads.xml 实测 404)(2026-09-17)'
Save-File (Join-Path $T 'catalog_fragments.inc')
Write-Output ('catalog_fragments.inc 大小 ' + (Get-Item (Join-Path $T 'catalog_fragments.inc')).Length)

Start-File @'
/* 本文件由 build-b2/gen-fixtures.ps1 从安卓端 assets/keymaps/*.json **逐字节**嵌入生成,请勿手改。
 * 来源:Silent-X-Craft-Launcher/android/app/src/main/assets/keymaps/(9 套布局,index.json 除外)
 * 目的:桌面 C 侧不联网、不依赖安卓目录也能验证 sxcl.keymap.v1 的跨端一致性。
 * 期望字段表 k_keymap_expect 是用 PowerShell 的 ConvertFrom-Json(独立解析器)从同一份文件里
 * 提取出来的,用来交叉验证 C 实现解析出来的关键字段。
 */
'@
$names = @()
foreach ($f in (Get-ChildItem $A -Filter '*.json' | Where-Object { $_.Name -ne 'index.json' } | Sort-Object Name)) {
  $sym = 'k_keymap_asset_' + ($f.BaseName -replace '[^A-Za-z0-9]','_')
  $names += @{ sym = $sym; file = $f.Name }
  Emit-C $sym (Get-Content $f.FullName -Raw) ('安卓端 assets/keymaps/' + $f.Name + ' 全文,逐字节')
}
[void]$script:sb.AppendLine('typedef struct keymap_asset_row { const char *file; const char *text; } keymap_asset_row;')
[void]$script:sb.AppendLine('static const keymap_asset_row k_keymap_assets[] = {')
foreach ($n in ($names | Sort-Object { $_.file })) {
  [void]$script:sb.AppendLine(('    {{ "{0}", {1} }},' -f $n.file, $n.sym))
}
[void]$script:sb.AppendLine('};')
[void]$script:sb.AppendLine('static const size_t k_keymap_asset_count = sizeof(k_keymap_assets) / sizeof(k_keymap_assets[0]);')
[void]$script:sb.AppendLine('')
[void]$script:sb.AppendLine('/* 独立解析(PowerShell ConvertFrom-Json)出来的期望值 */')
[void]$script:sb.AppendLine('typedef struct keymap_expect_row { const char *file; const char *name; const char *screen; const char *preset; const char *mc_version; int buttons; int directions; int builtin; const char *first_button; double first_x; double first_y; } keymap_expect_row;')
[void]$script:sb.AppendLine('static const keymap_expect_row k_keymap_expect[] = {')
foreach ($f in (Get-ChildItem $A -Filter '*.json' | Where-Object { $_.Name -ne 'index.json' } | Sort-Object Name)) {
  $d = Get-Content $f.FullName -Raw | ConvertFrom-Json
  $b0 = $d.buttons[0]
  $bi = 0; if ($d.meta.builtin) { $bi = 1 }
  $line = '    {{ "{0}", "{1}", "{2}", "{3}", "{4}", {5}, {6}, {7}, "{8}", {9}, {10} }},' -f $f.Name, $d.name, $d.screen, $d.meta.preset, $d.mc_version, $d.buttons.Count, $d.directions.Count, $bi, $b0.id, $b0.x, $b0.y
  [void]$script:sb.AppendLine($line)
}
[void]$script:sb.AppendLine('};')
Save-File (Join-Path $T 'keymap_assets.inc')
Write-Output ('keymap_assets.inc 大小 ' + (Get-Item (Join-Path $T 'keymap_assets.inc')).Length)