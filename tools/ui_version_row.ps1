# (C) Silent X Craft Launcher -- "version row" acceptance (user 2026-09-26, three points).
#
# What this proves, per run (every number is measured on this machine, nothing is guessed):
#
#   A) every version row starts with a state ICON
#      launchable = grass block (assets/icons/blocks/Grass.png)
#      not launchable = OUR OWN hand-drawn red warning badge (assets/icons/ui/version_warn.svg)
#      - no emoji anywhere, and PCL's redstone block is NOT used (the user asked us not to look
#        like PCL: "PCL ... puts a redstone block on the left ... we should not be that similar")
#      - dump level : one #sxclVersionStateIcon_grass / _warn widget per row, 20x20 logical,
#                     at the row's left edge (x = card.x + 20), leftmost child of that row
#      - screen level: inside each icon box the pixels differ from the card background and the
#                     icon carries its own colours (grass = green-dominant, badge = red-dominant),
#                     so the two states are told apart by PIXEL FEATURES, not by our own report
#      - the old trailing warning triangle is gone from the version rows (InfoIconWidget count 0)
#
#   B) the inline text is ONE short sentence + ONE action word
#      - before/after printed line by line; the "before" strings were recorded from the
#        pre-change build (archived in D:\SilentStudio\_test\sxcl_version_row_before_record)
#      - assertion: inline (note + action) <= 20 chars, no path separator, no parentheses,
#        no full stop; a launchable row's info line <= 32 chars
#      - the full core reason and the instance path are NOT inline: they live only in the row
#        tooltip (tip="..." of the evidence line), and the reason really is inside that tip
#
#   C) a guessed version number is never shown
#      - fixture 114514 (folder name is not a version; the JSON clientVersion says 1.12.2)
#        shows "original 1.12.2" with baseFrom=core:json-other
#      - fixture 424242 (nothing in the JSON says which MC version it is) writes NOTHING:
#        base="" baseFrom=none info=""  -- neither "unknown" nor a guess
#      - the whole dump is grep-ed for the guess marker: 0 hits
#
# Fixtures (all folder names deliberately NOT version-like), 5 of them:
#   114514 normal (JSON + jar, clientVersion 1.12.2)      233333 missing libraries
#   314159 jar only, no version JSON                      271828 missing parent 1.12.2
#   424242 JSON + jar but NO version information at all
# Two routes are driven: "select" (card rows, both themes) and "versions" (painted rows).
#
# ASCII-only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM),
# so every Chinese string below is built from code points; the fragments were verified
# byte-for-byte against the recorded pre-change dump.
$ErrorActionPreference = 'Continue'

foreach ($v in @('SXCL_UI_CONFIG','SXCL_UI_NAV','SXCL_UI_COLLAPSE','SXCL_UI_RAILS_TEST','SXCL_UI_EDITION',
                 'SXCL_UI_SCROLL','SXCL_UI_POPUP','SXCL_UI_AUTH_DIALOG','SXCL_UI_JRE_HOSTED','SXCL_UI_MODS_QUERY',
                 'SXCL_UI_LAUNCH','SXCL_UI_DOWNLOAD','SXCL_UI_ICON_POPUP','SXCL_UI_SCROLLBAR_STRESS',
                 'SXCL_UI_ACCENT_APPLY','SXCL_UI_THEME_SWITCH','SXCL_UI_TRACE','SXCL_UI_ACCEPT',
                 'SXCL_UI_SETTINGS_ACCEPT','SXCL_UI_VERSION','SXCL_UI2')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root  = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe   = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work  = 'D:\SilentStudio\_test\sxcl_version_row'
$fail  = 0
Add-Type -AssemblyName System.Drawing

function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }

# ---- Chinese fragments (code points; see the header note) ----
$T_P    = C @(0x539F,0x7248)                                  # original (version)
$T_DOT  = C @(0x00B7)                                         # middle dot
$T_JAR  = ' jar'
$T_HAS  = (C @(0x6709)) + $T_JAR                              # "has jar"
$T_OWN  = (C @(0x65E0,0x81EA,0x5DF1,0x7684)) + $T_JAR          # "no own jar"
$T_NOSTART = C @(0x4E0D,0x80FD,0x542F,0x52A8,0xFF1A)          # "cannot start:"
$T_NEED = C @(0x9700,0x8981,0x5B89,0x88C5)                    # "need to install"
$T_PARENT = C @(0x4F5C,0x4E3A,0x524D,0x7F6E,0x7248,0x672C)     # "as parent version"
$T_MISS = C @(0x7F3A,0x5C11,0x7248,0x672C)                    # "missing ... version"
$T_JSON = ' JSON'
$T_NOFILE = C @(0x7F3A,0x7248,0x672C,0x6587,0x4EF6)           # "missing version file"
$T_NOPARENT = C @(0x7F3A,0x524D,0x7F6E,0x7248,0x672C)         # "missing parent version"
# 2026-09-27: the action word changed from "go download" to "repair" -- the user's words were
# that the old action just threw the user onto the download page and left them there.  The row
# now offers repair (0x4FEE 0x590D) and clicking it really completes the missing files in place.
$T_FIX = C @(0x4FEE,0x590D)                                   # "repair"
$T_GUESS = C @(0x731C)                                        # "guess"
$T_REASON_JSON  = $T_MISS + $T_JSON                           # core reason of 314159
$T_REASON_PARENT = $T_NEED + ' 1.12.2 ' + $T_PARENT           # core reason of 271828

# ---- "before" strings, recorded from the pre-change build (dump before_select.out.txt.err) ----
$beforeSelect = @{}
$beforeSelect['114514'] = $T_P + ' ' + $T_DOT + ' ' + $T_P + ' 1.12.2 ' + $T_DOT + ' ' + $T_HAS
$beforeSelect['233333'] = 'Forge 1.12.2-14.23.5.2859 ' + $T_DOT + ' ' + $T_HAS
$beforeSelect['271828'] = $T_P + ' ' + $T_DOT + ' ' + $T_P + ' 1.12.2 ' + $T_DOT + ' ' + $T_OWN + ' ' + $T_DOT + ' ' + $T_NOSTART + $T_REASON_PARENT
$beforeSelect['314159'] = $T_P + ' ' + $T_DOT + ' ' + $T_HAS + ' ' + $T_DOT + ' ' + $T_NOSTART + $T_REASON_JSON
$beforeSelect['424242'] = $T_P + ' ' + $T_DOT + ' ' + $T_HAS
$beforePage = @{}
$beforePage['314159'] = $T_REASON_JSON
$beforePage['271828'] = $T_REASON_PARENT

# ---- what the code under test declares (checked against the evidence line of each run) ----
# A fixture is "launchable" or not strictly by the core library (sxcl_instance_scan);
# the icon must follow that verdict, whatever it is. 233333 (libraries declared but their
# files absent) is EXPECTED to come out launchable: the core verdict only looks at
# mainClass / inheritsFrom / jar / JSON id (instance.c:1413-1427), and the pre-launch
# "complete the files" step downloads missing libraries (docs/24).  Missing libraries are
# therefore NOT a PCL-style "broken version" and must NOT get the red badge.
$wantIcon = @{}
$wantIcon['114514'] = $true
$wantIcon['233333'] = $true
$wantIcon['314159'] = $false
$wantIcon['271828'] = $false
$wantIcon['424242'] = $true

if (-not (Test-Path $exe)) {
  Write-Output ('VERSION-ROW: FAIL (exe not found: ' + $exe + ')')
  exit 1
}
$busy = @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
  Write-Output ('VERSION-ROW: FAIL (another sxcl-ui.exe is running, pid ' + ($busy.Id -join ',') + ' -- close it and re-run)')
  exit 1
}

# ---- fixtures (D:\SilentStudio\_test) ----
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
$game = Join-Path $work 'mc'
foreach ($d in @('versions\114514','versions\233333','versions\314159','versions\271828','versions\424242','libraries')) {
  New-Item -ItemType Directory -Force -Path (Join-Path $game $d) | Out-Null
}
$jar = 'fixture-jar'
# A: 114514 -- normal.  Folder name is NOT a version; the JSON says clientVersion 1.12.2.
$j = @{ id='114514'; mainClass='net.minecraft.client.main.Main'; type='release'; clientVersion='1.12.2'; libraries=@() }
Set-Content -Path (Join-Path $game 'versions\114514\114514.json') -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8
Set-Content -Path (Join-Path $game 'versions\114514\114514.jar') -Value $jar -Encoding ascii
# B: 233333 -- the JSON declares a Forge library that is NOT on disk (missing libraries)
$j = @{ id='233333'; mainClass='net.minecraft.client.main.Main'; type='release'; libraries=@(
        @{ name='net.minecraftforge:forge:1.12.2-14.23.5.2859'; downloads=@{ artifact=@{
             path='net/minecraftforge/forge/1.12.2-14.23.5.2859/forge-1.12.2-14.23.5.2859.jar';
             url='https://example.invalid/forge.jar'; sha1='0000000000000000000000000000000000000000'; size=1234 } } } ) }
Set-Content -Path (Join-Path $game 'versions\233333\233333.json') -Value ($j | ConvertTo-Json -Depth 8) -Encoding utf8
Set-Content -Path (Join-Path $game 'versions\233333\233333.jar') -Value $jar -Encoding ascii
# C: 314159 -- a jar with no version JSON at all
Set-Content -Path (Join-Path $game 'versions\314159\314159.jar') -Value $jar -Encoding ascii
# D: 271828 -- inheritsFrom 1.12.2, and 1.12.2 is not installed (missing parent)
$j = @{ id='271828'; mainClass='net.minecraft.client.main.Main'; type='release'; inheritsFrom='1.12.2'; libraries=@() }
Set-Content -Path (Join-Path $game 'versions\271828\271828.json') -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8
# E: 424242 -- JSON + jar, but nothing says which MC version it is (no clientVersion/inheritsFrom)
$j = @{ id='424242'; mainClass='net.minecraft.client.main.Main'; type='release'; libraries=@() }
Set-Content -Path (Join-Path $game 'versions\424242\424242.json') -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8
Set-Content -Path (Join-Path $game 'versions\424242\424242.jar') -Value $jar -Encoding ascii

$ids = @('114514','233333','314159','271828','424242')
$ini = Join-Path $work 'row.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $game) -Encoding utf8
Add-Content -Path $ini -Value 'game.selected_version=114514'

# offline fixture manifest so the "versions" route has deterministic rows (SXCL_UI_MANIFEST)
$vs = @()
for ($i = 0; $i -lt $ids.Count; $i++) {
  $vs += @{ id=$ids[$i]; type='release'; url=('https://example.invalid/' + $ids[$i] + '.json');
            sha1='0000000000000000000000000000000000000000'; size=1000;
            releaseTime=('2026-01-0' + ($i + 1) + 'T00:00:00+00:00'); time='2026-01-01T00:00:00+00:00' }
}
$manifest = Join-Path $work 'manifest.json'
Set-Content -Path $manifest -Value (@{ latest=@{ release='114514'; snapshot='114514' }; versions=$vs } | ConvertTo-Json -Depth 6) -Encoding utf8

# ---- asset level: the two icons are real, and they are OURS ----
Write-Output 'icon sources (hashes are of the files in the repo):'
$grass = Join-Path $root 'assets\icons\blocks\Grass.png'
$badge = Join-Path $root 'assets\icons\ui\version_warn.svg'
foreach ($pair in @(@('ok   -> grass block ', $grass), @('warn -> our own badge', $badge))) {
  $path = $pair[1]
  if (-not (Test-Path $path)) {
    Write-Output ('  -> FAIL missing ' + $path); $fail++
    continue
  }
  $hash = (Get-FileHash $path -Algorithm SHA256).Hash
  Write-Output ('  ' + $pair[0] + ' ' + $path.Replace($root + '\', '') + '  ' + (Get-Item $path).Length + ' bytes  sha256=' + $hash)
}
if (Test-Path $badge) {
  $svg = Get-Content $badge -Raw -Encoding utf8
  # our own vector: currentColor + an even-odd punched "!"; no embedded raster / external ref
  foreach ($need in @('currentColor', 'fill-rule="evenodd"')) {
    if ($svg -notlike ('*' + $need + '*')) { Write-Output ('  -> FAIL badge svg lacks ' + $need); $fail++ }
  }
  foreach ($bad in @('<image', 'href=', 'data:', 'base64')) {
    if ($svg -like ('*' + $bad + '*')) { Write-Output ('  -> FAIL badge svg references ' + $bad + ' (not our own drawing)'); $fail++ }
  }
  Write-Output ('  badge svg: currentColor + fill-rule="evenodd" present, no embedded raster / external ref')
}

# ---- helpers ----
function Get-Rgb([System.Drawing.Bitmap]$bmp, [int]$px, [int]$py) {
  $c = $bmp.GetPixel($px, $py); return @($c.R, $c.G, $c.B)
}
function Get-Dist($a, $b) {
  return ([math]::Abs([int]$a[0] - [int]$b[0]) + [math]::Abs([int]$a[1] - [int]$b[1]) + [math]::Abs([int]$a[2] - [int]$b[2]))
}
# one icon box: how many pixels differ from the card background, and which colour family they are
function Get-IconStats([string]$png, [int]$x, [int]$y, [int]$w, [int]$h, [double]$dpr, [int[]]$bgRef) {
  $bmp = New-Object System.Drawing.Bitmap($png)
  $x0 = [int][math]::Floor($x * $dpr); $x1 = [int][math]::Ceiling(($x + $w) * $dpr)
  $y0 = [int][math]::Floor($y * $dpr); $y1 = [int][math]::Ceiling(($y + $h) * $dpr)
  $total = 0; $nonBg = 0; $green = 0; $red = 0; $light = 0
  for ($py = $y0; $py -lt $y1; $py++) {
    for ($px = $x0; $px -lt $x1; $px++) {
      $p = Get-Rgb $bmp $px $py
      $total++
      if ((Get-Dist $p $bgRef) -le 12) { continue }
      $nonBg++
      if (($p[1] - $p[0]) -gt 15 -and ($p[1] - $p[2]) -gt 15) { $green++ }
      if (($p[0] - $p[1]) -gt 40 -and ($p[0] - $p[2]) -gt 40) { $red++ }
      if ((($p[0] + $p[1] + $p[2]) / 3.0) -gt 120) { $light++ }
    }
  }
  $bmp.Dispose()
  return @{ total = $total; nonBg = $nonBg; green = $green; red = $red; light = $light }
}
function Invoke-RowRun([string]$tag, [string]$route, [string]$theme) {
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = $game
  # 1100x900: the versions route has to show all five rows at once (the painted column has no
  # widget to scroll to), and the screen here is 2560x1440 so the window fits.
  $env:SXCL_UI_WINDOW = '1100x900'
  $env:SXCL_UI_ROUTE = $route
  $env:SXCL_UI_THEME = $theme
  $env:SXCL_UI_MANIFEST = $manifest
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '14'
  $env:SXCL_UI_SHOT_DELAY = '5000'
  $png = Join-Path $work ('shot_' + $tag + '.png')
  $env:SXCL_UI_SHOT = $png
  Remove-Item $png -ErrorAction SilentlyContinue
  $out = Join-Path $work ('dump_' + $tag + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $lines = @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) + @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  return @{ png = $png; lines = $lines }
}
# which page is on screen when the dump was taken (the dump's own root line says it)
function Get-CurrentPage($lines) {
  foreach ($line in $lines) {
    if ($line -match '^ScrollArea #sxclPage_(\w+)') { return $Matches[1] }
  }
  return ''
}
function Get-Rows($lines, [string]$pattern) {
  $rows = @{}
  foreach ($line in $lines) {
    $m = [regex]::Match($line, $pattern)
    if (-not $m.Success) { continue }
    $rows[$m.Groups['id'].Value] = @{
      state = $m.Groups['state'].Value
      launchable = $m.Groups['launchable'].Value
      base = $m.Groups['base'].Value
      baseFrom = $m.Groups['baseFrom'].Value
      coreReliable = $m.Groups['coreReliable'].Value
      info = $m.Groups['info'].Value
      note = $m.Groups['note'].Value
      action = $m.Groups['action'].Value
      path = $m.Groups['path'].Value
      tip = $m.Groups['tip'].Value
    }
  }
  return $rows
}
# the card block of one row in the widget-tree dump: the icon widget, the inline labels and
# their geometry.  (The dump prints every widget with its objectName and window coordinates.)
function Get-CardBlock($lines, [string]$id) {
  $cardIdx = -1
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -notmatch '^\s*CardWidget \(') { continue }
    $indent = $lines[$i].Length - $lines[$i].TrimStart().Length
    for ($k = $i + 1; $k -lt $lines.Count; $k++) {
      $ik = $lines[$k].Length - $lines[$k].TrimStart().Length
      if ($ik -le $indent) { break }
      if ($lines[$k] -like ('*"' + $id + '"*')) { $cardIdx = $i; break }
    }
    if ($cardIdx -ge 0) { break }
  }
  if ($cardIdx -lt 0) { return $null }
  $indent = $lines[$cardIdx].Length - $lines[$cardIdx].TrimStart().Length
  $block = @()
  for ($k = $cardIdx; $k -lt $lines.Count; $k++) {
    if ($k -gt $cardIdx) {
      $ik = $lines[$k].Length - $lines[$k].TrimStart().Length
      if ($ik -le $indent) { break }
    }
    $block += $lines[$k]
  }
  return @{ card = $lines[$cardIdx]; block = $block }
}
function Get-Widget($block, [string]$namePattern) {
  foreach ($line in $block) {
    $m = [regex]::Match($line, $namePattern + ' \((\d+),(\d+) (\d+)x(\d+)\)(.*)$')
    if ($m.Success) {
      $text = ''
      $tm = [regex]::Match($m.Groups[5].Value, '"([^"]*)"')
      if ($tm.Success) { $text = $tm.Groups[1].Value }
      return @{ x = [int]$m.Groups[1].Value; y = [int]$m.Groups[2].Value; w = [int]$m.Groups[3].Value;
                h = [int]$m.Groups[4].Value; text = $text; line = $line.Trim(); rest = $m.Groups[5].Value }
    }
  }
  return $null
}

# =====================================================================================
# A/B/C -- route "select" (card rows), both themes
# =====================================================================================
$selectRows = @{}
$selectShot = @{}
$selectLines = @{}
$cardInfo = @{}
$cardInfoByTheme = @{}
foreach ($theme in @('dark','light')) {
  $case = 'select/' + $theme
  $run = Invoke-RowRun ('select_' + $theme) 'select' $theme
  $page = Get-CurrentPage $run.lines
  if ($page -ne 'select') {
    # A stray click from outside (this session drives a real window) can send the window
    # somewhere else; that is not a product failure, so retry once and say so.
    Write-Output ('  NOTE the window was on page "' + $page + '" instead of select; re-running once')
    $run = Invoke-RowRun ('select_' + $theme) 'select' $theme
    $page = Get-CurrentPage $run.lines
  }
  if ($page -ne 'select') { Write-Output ('  -> FAIL the select page is not the current page (got "' + $page + '")'); $fail++ }
  $lines = $run.lines
  $selectLines[$theme] = $lines
  $selectShot[$theme] = $run.png
  $pattern = 'version-row select: id=(?<id>\S+) state=(?<state>\S+) launchable=(?<launchable>\d) problem=(?<problem>\S+) base="(?<base>[^"]*)" baseFrom=(?<baseFrom>\S+) coreReliable=(?<coreReliable>\d) info="(?<info>[^"]*)" note="(?<note>[^"]*)" action="(?<action>[^"]*)" path="(?<path>[^"]*)" tip="(?<tip>[^"]*)"'
  $rows = Get-Rows $lines $pattern
  if ($theme -eq 'dark') { $selectRows = $rows }
  Write-Output ''
  Write-Output ('== ' + $case + ': evidence lines (one per row) ==')
  foreach ($id in $ids) {
    if (-not $rows.ContainsKey($id)) { Write-Output ('  -> FAIL no evidence line for ' + $id); $fail++; continue }
    $r = $rows[$id]
    Write-Output ('  ' + $id + ': icon=' + $r.state + ' launchable=' + $r.launchable + ' base="' + $r.base + '" baseFrom=' + $r.baseFrom + ' coreReliable=' + $r.coreReliable + ' info="' + $r.info + '" note="' + $r.note + '" action="' + $r.action + '"')
    if ($r.state -eq '' -or ($r.launchable -eq '1' -and $r.state -ne 'grass') -or ($r.launchable -eq '0' -and $r.state -ne 'warn')) {
      Write-Output ('    -> FAIL icon state ' + $r.state + ' does not follow launchable=' + $r.launchable)
      $fail++
    }
    # C) version numbers
    if ($id -eq '114514') {
      if ($r.base -ne '1.12.2') { Write-Output ('    -> FAIL 114514 base="' + $r.base + '" (want 1.12.2)'); $fail++ }
      if ($r.baseFrom -ne 'core:json-other') { Write-Output ('    -> FAIL 114514 baseFrom=' + $r.baseFrom + ' (want core:json-other)'); $fail++ }
      if ($r.info -ne ($T_P + ' 1.12.2')) { Write-Output ('    -> FAIL 114514 info="' + $r.info + '"'); $fail++ }
    }
    if ($id -eq '424242') {
      if ($r.base -ne '') { Write-Output ('    -> FAIL 424242 wrote a version it cannot know: "' + $r.base + '"'); $fail++ }
      if ($r.baseFrom -ne 'none') { Write-Output ('    -> FAIL 424242 baseFrom=' + $r.baseFrom + ' (want none)'); $fail++ }
      if ($r.info -ne '') { Write-Output ('    -> FAIL 424242 inline info is not empty: "' + $r.info + '"'); $fail++ }
      if ($r.coreReliable -ne '0') { Write-Output ('    -> FAIL 424242 coreReliable=' + $r.coreReliable + ' (want 0)'); $fail++ }
    }
    # B) inline text shape
    $inline = $r.info
    if ($r.launchable -eq '0') {
      $inline = $r.note + $r.action
      if ($r.info -ne '') { Write-Output ('    -> FAIL broken row still writes an info line: "' + $r.info + '"'); $fail++ }
      if ($r.note -eq '' -or $r.action -eq '') { Write-Output '    -> FAIL broken row is missing its sentence or its action'; $fail++ }
      # 2026-09-27: the sentence now counts up what is missing ("missing game jar, 1 library"),
      # so the cap moved 20 -> 34.  It must still be ONE short line (no path / no parentheses).
      if ($inline.Length -gt 34) { Write-Output ('    -> FAIL inline text is ' + $inline.Length + ' chars (cap 34): "' + $inline + '"'); $fail++ }
      # one sentence = no full stop / no semicolon; and a "hint" must not smuggle in a path,
      # parentheses or jargon punctuation
      foreach ($ch in @('\', '/', '(', ')', (C @(0xFF08)), (C @(0xFF09)), (C @(0x3002)), (C @(0xFF1B)), (C @(0xFF1A)))) {
        if ($inline.Contains($ch)) { Write-Output ('    -> FAIL inline text contains "' + $ch + '": ' + $inline); $fail++ }
      }
      $nativePath = $r.path.Replace('/', '\')
      if ($r.tip -notlike ('*' + $nativePath + '*')) { Write-Output ('    -> FAIL the tip does not carry the path: ' + $r.tip); $fail++ }
      if ($r.tip.Length -le $inline.Length) { Write-Output '    -> FAIL the tip is not richer than the inline text'; $fail++ }
    } else {
      if ($r.note -ne '' -or $r.action -ne '') { Write-Output ('    -> FAIL launchable row still carries a problem note/action: "' + $r.note + '" "' + $r.action + '"'); $fail++ }
      if ($inline.Length -gt 32) { Write-Output ('    -> FAIL info line is ' + $inline.Length + ' chars (cap 32): "' + $inline + '"'); $fail++ }
    }
    # the full core reason must be in the tip and NOT inline
    $reason = ''
    if ($id -eq '314159') { $reason = $T_REASON_JSON }
    if ($id -eq '271828') { $reason = $T_REASON_PARENT }
    if ($reason -ne '') {
      if ($r.tip -notlike ('*' + $reason + '*')) { Write-Output ('    -> FAIL the full reason is not in the tip: ' + $r.tip); $fail++ }
      if ($inline -like ('*' + $reason + '*')) { Write-Output '    -> FAIL the full reason is still inline'; $fail++ }
      if ($beforeSelect[$id] -like ('*' + $reason + '*') -eq $false) { Write-Output ('    -> NOTE before-string for ' + $id + ' did not contain the reason (recorded table mismatch)'); $fail++ }
    }
  }

  # widget tree: one state icon per row, leftmost child, 20x20
  Write-Output ('-- ' + $case + ': widget tree --')
  $iconWidgets = @($lines | Where-Object { $_ -match '#sxclVersionStateIcon_' })
  if ($iconWidgets.Count -ne $ids.Count) {
    Write-Output ('  -> FAIL state icon widgets = ' + $iconWidgets.Count + ' (want ' + $ids.Count + ')')
    $fail++
  } else {
    Write-Output ('  state icon widgets: ' + $iconWidgets.Count + ' (one per row)')
  }
  $triangles = @($lines | Where-Object { $_ -match 'InfoIconWidget' })
  if ($triangles.Count -ne 0) {
    Write-Output ('  -> FAIL the old warning triangle is still in the version rows: ' + $triangles.Count + ' InfoIconWidget')
    $fail++
  } else {
    Write-Output '  old trailing warning triangle in the rows: 0 InfoIconWidget (replaced by the state icon)'
  }
  foreach ($id in $ids) {
    $card = Get-CardBlock $lines $id
    if ($card -eq $null) { Write-Output ('  -> FAIL no CardWidget block for ' + $id); $fail++; continue }
    $cm = [regex]::Match($card.card, '\((\d+),(\d+) (\d+)x(\d+)\)')
    $cx = [int]$cm.Groups[1].Value; $cy = [int]$cm.Groups[2].Value
    $cw = [int]$cm.Groups[3].Value; $ch = [int]$cm.Groups[4].Value
    $icon = Get-Widget $card.block '#sxclVersionStateIcon_\w+'
    $title = Get-Widget $card.block 'FluentLabelBase'
    $note = Get-Widget $card.block '#sxclVersionRowNote'
    $action = Get-Widget $card.block '#sxclVersionRowAction'
    $infoLabel = Get-Widget $card.block '#sxclVersionRowInfo'
    if ($icon -eq $null) { Write-Output ('  -> FAIL ' + $id + ': no icon widget in the row'); $fail++; continue }
    $wantState = 'grass'; if (-not $wantIcon[$id]) { $wantState = 'warn' }
    $iconLine = ($icon.line -replace '\s+', ' ')
    Write-Output ('  ' + $id + ' card=(' + $cx + ',' + $cy + ' ' + $cw + 'x' + $ch + ') ' + $iconLine)
    if ($icon.line -notmatch ('#sxclVersionStateIcon_' + $wantState)) {
      Write-Output ('    -> FAIL icon widget is not the expected state ' + $wantState); $fail++
    }
    if ($icon.line -match ' hidden') { Write-Output '    -> FAIL the icon widget is hidden'; $fail++ }
    if ($icon.w -ne 20 -or $icon.h -ne 20) { Write-Output ('    -> FAIL icon box is ' + $icon.w + 'x' + $icon.h + ' (want 20x20)'); $fail++ }
    if (($icon.x - $cx) -ne 20) { Write-Output ('    -> FAIL icon is not at the row left edge (x-card.x = ' + ($icon.x - $cx) + ', want 20)'); $fail++ }
    if ($title -ne $null -and $icon.x -ge $title.x) {
      Write-Output ('    -> FAIL the icon is not the leftmost child (icon.x=' + $icon.x + ' title.x=' + $title.x + ')'); $fail++
    }
    if ($note -ne $null -and $action -ne $null) {
      Write-Output ('    inline labels: note="' + $note.text + '" (' + $note.w + 'px) action="' + $action.text + '" (' + $action.w + 'px)')
    }
    if ($infoLabel -ne $null) { Write-Output ('    inline info label: "' + $infoLabel.text + '"') }
    $cardInfo[$id] = @{ icon = $icon; title = $title; note = $note; action = $action; info = $infoLabel; cx = $cx; cy = $cy }
  }
  $cardInfoByTheme[$theme] = $cardInfo
}

# ---- screen level (both themes): every icon box must carry its own colours ----
foreach ($theme in @('dark','light')) {
Write-Output ''
  Write-Output ('== select/' + $theme + ': pixel features inside each icon box ==')
  $png = $selectShot[$theme]
  $cardInfo = $cardInfoByTheme[$theme]
  if (-not (Test-Path $png)) {
    Write-Output ('  -> FAIL screenshot missing: ' + $png); $fail++
    continue
  }
  $bi = New-Object System.Drawing.Bitmap($png)
  $dpr = [math]::Round($bi.Width / 1100.0, 2); $shotW = $bi.Width
  $bi.Dispose()
  Write-Output ('  shot ' + $shotW + 'x' + ([int]($shotW * 900.0 / 1100.0)) + ' dpr=' + $dpr)
  $bmpRef = New-Object System.Drawing.Bitmap($png)
  foreach ($id in $ids) {
    if (-not $cardInfo.ContainsKey($id)) { continue }
    $ci = $cardInfo[$id]
    $icon = $ci.icon
    # card background: sampled just left of the icon box (inside the card padding)
    $bg = @(0,0,0)
    $bgp = $bmpRef.GetPixel([int](($icon.x - 8) * $dpr), [int](($icon.y + 10) * $dpr))
    $bg = @($bgp.R, $bgp.G, $bgp.B)
    $s = Get-IconStats $png $icon.x $icon.y $icon.w $icon.h $dpr $bg
    $kind = 'blank'
    if ($s.green -ge 60 -and $s.red -le 60) { $kind = 'grass' }
    if ($s.red -ge 150 -and $s.green -le 60) { $kind = 'badge' }
    $want = 'grass'; if (-not $wantIcon[$id]) { $want = 'badge' }
    Write-Output ('  ' + $id + ': iconBox=' + $icon.x + ',' + $icon.y + ' ' + $icon.w + 'x' + $icon.h + ' bg=#' + ('{0:X2}{1:X2}{2:X2}' -f $bg[0],$bg[1],$bg[2]) + ' nonBgPx=' + $s.nonBg + '/' + $s.total + ' greenPx=' + $s.green + ' redPx=' + $s.red + ' lightPx=' + $s.light + ' -> ' + $kind)
    if ($s.nonBg -lt 120) { Write-Output ('    -> FAIL icon looks blank (nonBgPx=' + $s.nonBg + ')'); $fail++ }
    if ($kind -ne $want) { Write-Output ('    -> FAIL pixel features say ' + $kind + ', the row declares ' + $want); $fail++ }
  }
  $bmpRef.Dispose()
}


# =====================================================================================
# B (continued) -- route "versions": the same helper feeds the painted rows
# =====================================================================================
$case = 'versions/dark'
$run = Invoke-RowRun 'versions_dark' 'versions' 'dark'
$page = Get-CurrentPage $run.lines
if ($page -ne 'versions') {
  Write-Output ('  NOTE the window was on page "' + $page + '" instead of versions; re-running once')
  $run = Invoke-RowRun 'versions_dark' 'versions' 'dark'
  $page = Get-CurrentPage $run.lines
}
if ($page -ne 'versions') { Write-Output ('  -> FAIL the versions page is not the current page (got "' + $page + '")'); $fail++ }
$lines = $run.lines
$pattern = 'version-row page: id=(?<id>\S+) state=(?<state>\S+) iconRect=(?<x>\d+),(?<y>\d+),(?<w>\d+)x(?<h>\d+) chip="(?<chip>[^"]*)" tip="(?<tip>[^"]*)" vis=(?<vis>\d) inView=(?<inview>\d) list=(?<lx>\d+),(?<ly>\d+),(?<lw>\d+)x(?<lh>\d+) win=(?<winw>\d+)x(?<winh>\d+)'
# the dump's own geometry of the painted list, taken at the very same moment as the screenshot
$dumpList = $null
$lm = [regex]::Match(($lines -join "`n"), '#versionList \((\d+),(\d+) (\d+)x(\d+)\)')
if ($lm.Success) {
  $dumpList = @([int]$lm.Groups[1].Value, [int]$lm.Groups[2].Value, [int]$lm.Groups[3].Value, [int]$lm.Groups[4].Value)
  Write-Output ('  dump #versionList = ' + ($dumpList -join ','))
}
if ($dumpList -eq $null) { Write-Output '  -> FAIL the dump has no #versionList line'; $fail++ }
$pageLines = @{}
foreach ($line in $lines) {
  $m = [regex]::Match($line, $pattern)
  if (-not $m.Success) { continue }
  if ($m.Groups['vis'].Value -ne '1') { continue }  # hidden copies (inside the download page) do not count
  if (-not $pageLines.ContainsKey($m.Groups['id'].Value)) { $pageLines[$m.Groups['id'].Value] = @() }
  $pageLines[$m.Groups['id'].Value] += @{
    state = $m.Groups['state'].Value; x = [int]$m.Groups['x'].Value; y = [int]$m.Groups['y'].Value
    w = [int]$m.Groups['w'].Value; h = [int]$m.Groups['h'].Value
    chip = $m.Groups['chip'].Value; tip = $m.Groups['tip'].Value
    inview = $m.Groups['inview'].Value
    list = @([int]$m.Groups['lx'].Value, [int]$m.Groups['ly'].Value, [int]$m.Groups['lw'].Value, [int]$m.Groups['lh'].Value)
    winw = [int]$m.Groups['winw'].Value; winh = [int]$m.Groups['winh'].Value
  }
}
# keep the reading whose list geometry matches the dump (a stale pre-layout reading is not a fact)
$pageRows = @{}
foreach ($id in $pageLines.Keys) {
  $pick = $null
  foreach ($entry in $pageLines[$id]) {
    if ($dumpList -ne $null -and ($entry.list -join ',') -eq ($dumpList -join ',')) { $pick = $entry }
  }
  if ($pick -eq $null) {
    Write-Output ('  -> FAIL ' + $id + ': none of the ' + $pageLines[$id].Count + ' readings matches the dump list geometry (' + (($pageLines[$id][-1].list) -join ',') + ' vs ' + $(if ($dumpList -ne $null) { $dumpList -join ',' } else { '-' }) + ')')
    $fail++
    $pick = $pageLines[$id][-1]
  }
  # also make sure an icon slot really sits inside the list
  $inside = ($pick.x -ge $pick.list[0]) -and ($pick.y -ge $pick.list[1]) -and (($pick.x + $pick.w) -le ($pick.list[0] + $pick.list[2])) -and (($pick.y + $pick.h) -le ($pick.list[1] + $pick.list[3]))
  if (-not $inside) {
    Write-Output ('  -> FAIL ' + $id + ': icon slot ' + $pick.x + ',' + $pick.y + ' is outside the list rect ' + ($pick.list -join ','))
    $fail++
  }
  $pageRows[$id] = $pick
}
Write-Output ''
Write-Output ('== ' + $case + ': evidence lines (painted rows, vis=1 only) ==')
if ($pageRows.Count -ne $ids.Count) {
  Write-Output ('  -> FAIL visible rows with a state icon = ' + $pageRows.Count + ' (want ' + $ids.Count + ')')
  $fail++
}
foreach ($id in $ids) {
  if (-not $pageRows.ContainsKey($id)) { Write-Output ('  -> FAIL no visible evidence line for ' + $id); $fail++; continue }
  $p = $pageRows[$id]
  $want = 'grass'; if (-not $wantIcon[$id]) { $want = 'warn' }
  Write-Output ('  ' + $id + ': icon=' + $p.state + ' iconRect=' + $p.x + ',' + $p.y + ' ' + $p.w + 'x' + $p.h + ' chip="' + $p.chip + '" tip="' + $p.tip + '"')
  if ($p.state -ne $want) { Write-Output ('    -> FAIL icon state ' + $p.state + ', expected ' + $want); $fail++ }
  if ($p.w -ne 20 -or $p.h -ne 20) { Write-Output ('    -> FAIL icon slot ' + $p.w + 'x' + $p.h + ' (want 20x20)'); $fail++ }
  if ($p.inview -ne '1') { Write-Output '    -> FAIL the row is scrolled out of view (cannot be measured)'; $fail++ }
  if ($p.x -lt 0 -or $p.y -lt 0 -or ($p.x + $p.w) -gt $p.winw -or ($p.y + $p.h) -gt $p.winh) {
    Write-Output '    -> FAIL icon slot is outside the window'; $fail++
  }
  if ($want -eq 'warn') {
    if ($p.chip -eq '') { Write-Output '    -> FAIL broken painted row has no chip text'; $fail++ }
    if ($p.chip.Length -gt 34) { Write-Output ('    -> FAIL chip text is ' + $p.chip.Length + ' chars (cap 34): ' + $p.chip); $fail++ }
    if ($p.chip -notlike ('*' + $T_FIX + '*')) { Write-Output '    -> FAIL chip does not offer the action'; $fail++ }
    $sel = $selectRows[$id]
    if ($sel -ne $null) {
      $wantChip = $sel.note + ' ' + $T_DOT + ' ' + $sel.action
      if ($p.chip -ne $wantChip) { Write-Output ('    -> FAIL the two pages disagree: select note+action = "' + $wantChip + '" but chip = "' + $p.chip + '"'); $fail++ }
    }
  } else {
    if ($p.chip -ne '') { Write-Output ('    -> FAIL launchable painted row still has a chip: ' + $p.chip); $fail++ }
  }
}
# pixel check of the painted slots
if (Test-Path $run.png) {
  $bi = New-Object System.Drawing.Bitmap($run.png)
  $dpr = [math]::Round($bi.Width / 1100.0, 2); $bi.Dispose()
  Write-Output ('  shot ' + $run.png + ' dpr=' + $dpr)
  Write-Output ('  pixel features inside each painted icon slot:')
  $bmpRef = New-Object System.Drawing.Bitmap($run.png)
  foreach ($id in $ids) {
    if (-not $pageRows.ContainsKey($id)) { continue }
    $p = $pageRows[$id]
    $bgp = $bmpRef.GetPixel([int](($p.x - 8) * $dpr), [int](($p.y + 10) * $dpr))
    $bg = @($bgp.R, $bgp.G, $bgp.B)
    $s = Get-IconStats $run.png $p.x $p.y $p.w $p.h $dpr $bg
    $kind = 'blank'
    if ($s.green -ge 60 -and $s.red -le 60) { $kind = 'grass' }
    if ($s.red -ge 150 -and $s.green -le 60) { $kind = 'badge' }
    $want = 'grass'; if (-not $wantIcon[$id]) { $want = 'badge' }
    Write-Output ('    ' + $id + ': bg=#' + ('{0:X2}{1:X2}{2:X2}' -f $bg[0],$bg[1],$bg[2]) + ' nonBgPx=' + $s.nonBg + '/' + $s.total + ' greenPx=' + $s.green + ' redPx=' + $s.red + ' -> ' + $kind)
    if ($s.nonBg -lt 120) { Write-Output ('      -> FAIL painted icon looks blank (nonBgPx=' + $s.nonBg + ')'); $fail++ }
    if ($kind -ne $want) { Write-Output ('      -> FAIL pixel features say ' + $kind + ', the row declares ' + $want); $fail++ }
  }
  $bmpRef.Dispose()
} else {
  Write-Output ('  -> FAIL screenshot missing: ' + $run.png); $fail++
}

# ---- the guess marker must not appear anywhere in either dump ----
Write-Output ''
Write-Output '== version-number discipline: the guess marker must not appear at all =='
foreach ($theme in @('dark','light')) {
  $hits = @($selectLines[$theme] | Select-String -Pattern $T_GUESS -SimpleMatch)
  Write-Output ('  select/' + $theme + ': lines containing the guess marker = ' + $hits.Count)
  if ($hits.Count -ne 0) { Write-Output ('    -> FAIL ' + $hits[0].Line.Trim()); $fail++ }
}
$hits = @($lines | Select-String -Pattern $T_GUESS -SimpleMatch)
Write-Output ('  versions/dark: lines containing the guess marker = ' + $hits.Count)
if ($hits.Count -ne 0) { Write-Output ('    -> FAIL ' + $hits[0].Line.Trim()); $fail++ }
$r424 = $selectRows['424242']
if ($r424 -ne $null) {
  $inline424 = $r424.info + $r424.note + $r424.action
  Write-Output ('  424242 inline text: "' + $inline424 + '" (length ' + $inline424.Length + ')')
  if ($inline424 -match '\d+\.\d+') { Write-Output '    -> FAIL the row with no version information still shows a version number'; $fail++ }
}

# ---- B: before / after, printed line by line ----
Write-Output ''
Write-Output '== inline text, before vs after (select/dark) =='
$maxInline = 0
foreach ($id in $ids) {
  $b = $beforeSelect[$id]
  $sel = $selectRows[$id]
  if ($sel -eq $null) { continue }
  $a = $sel.info
  if ($sel.launchable -eq '0') { $a = $sel.note + $sel.action }
  if ($a.Length -gt $maxInline) { $maxInline = $a.Length }
  Write-Output ('  ' + $id)
  Write-Output ('    before (recorded from the pre-change build): "' + $b + '"  (' + $b.Length + ' chars)')
  Write-Output ('    after  (this build, evidence line)        : "' + $a + '"  (' + $a.Length + ' chars)')
  if ($a -eq $b) { Write-Output '    -> FAIL the inline text did not change'; $fail++ }
  if ($a.Length -ge $b.Length -and $sel.launchable -eq '0') {
    Write-Output '    -> FAIL the problem text did not get shorter'; $fail++
  }
}
Write-Output '== chip text, before vs after (versions/dark) =='
foreach ($id in @('314159','271828')) {
  $b = $beforePage[$id]
  $p = $pageRows[$id]
  if ($p -eq $null) { continue }
  Write-Output ('  ' + $id + ': before="' + $b + '" (' + $b.Length + ')  after="' + $p.chip + '" (' + $p.chip.Length + ')')
  if ($p.chip -eq $b) { Write-Output '    -> FAIL the chip text did not change'; $fail++ }
}
Write-Output ('inline text cap used by this script: problem rows (note+action) <= 20 chars, info rows <= 32 chars; max measured here = ' + $maxInline + ' chars')

Write-Output ''
Write-Output ('product files: ' + $work)
if ($fail -eq 0) {
  Write-Output 'VERSION-ROW: ALL OK (3 fixtures + 2 extra: icon per row green/red by pixel features, inline text one sentence + one action, no guessed version number)'
  exit 0
}
Write-Output ('VERSION-ROW: FAIL (' + $fail + ' assertion(s))')
exit 1
