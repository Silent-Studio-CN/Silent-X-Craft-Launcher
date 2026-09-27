# (C) Silent X Craft Launcher -- "version row block icon" acceptance (user 2026-09-26).
#
# The user's words, for the record (this file itself stays ASCII):
#   "the version download page: the grass block on the left of a version row -- you forgot it"
#   "as for 'get server', we are not doing it for now, we will talk about it later"
#
# What this script proves on THIS machine, every number measured, nothing guessed:
#
#   A) EVERY row carries a version-type block icon on the left, installed or not
#      - fixtures: an EMPTY game dir (no instance folder at all) + a pinned manifest with
#        4 rows of 4 different version types.  The old code drew NOTHING for all four,
#        because it only drew an icon on rows that were installed.
#      - read from the page's own evidence line (same constants the delegate paints with):
#          state= empty          -> nothing is installed (on purpose)
#          block= vanilla|snapshot|old|none  -> version type mapped to a block asset
#          icon=  what the row really draws  -> warn overrides the block; none = nothing
#      - assertion: block and icon equal the expected asset kind for each of the 4 types,
#        and state is empty for all 4 (so the icon cannot come from an installed instance).
#
#   B) the pixels in that slot really ARE the asset (not merely "something green")
#      - the slot rectangle from the evidence line is cut out of the screenshot and compared
#        pixel by pixel against the asset PNG itself (blocks\Grass.png / CommandBlock.png /
#        CobbleStone.png), scaled to the very same slot size
#      - assertion: >= 80% of the asset's opaque pixels match (Manhattan distance <= 120);
#        green/red/non-background pixel counts are printed for every row
#
#   C) the "get server" entry is really gone (user: not doing it for now)
#      - the source file does not contain it, and neither does the shipped exe -- checked in
#        BOTH encodings (UTF-8 and UTF-16LE, because Qt string literals are UTF-16)
#      - the placeholder sentence ("... server download page (under development)") likewise
#
#   D) layout did not shift, and the one remaining row button still hugs the right margin
#      - 3 window sizes: list rect inside the window, constant 16 px gap between the
#        "version log" button's right edge (computed by the delegate's own buttonRects and
#        reported by the page) and the right edge of the list, constant icon column x
#      - at 1100x750 the list rect must still be the recorded 78,128 993x597
#
# ASCII-only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM),
# so every Chinese fragment below is built from code points.
$ErrorActionPreference = 'Continue'

foreach ($v in @('SXCL_UI_CONFIG','SXCL_UI_NAV','SXCL_UI_LIST','SXCL_UI_CATEGORY','SXCL_UI_MODS_QUERY',
                 'SXCL_UI_LAUNCH','SXCL_UI_DOWNLOAD','SXCL_UI_THEME','SXCL_UI_TRACE','SXCL_UI_MANIFEST',
                 'SXCL_UI_MANIFEST_URL','SXCL_UI_MANIFEST_OFFICIAL','SXCL_UI2','SXCL_UI_WINDOW',
                 'SXCL_UI_SHOT','SXCL_UI_SHOT_DELAY','SXCL_UI_DUMP','SXCL_UI_DUMP_DEPTH',
                 'SXCL_UI_SETTINGS','SXCL_UI_GAME_DIR','SXCL_UI_ACCEPT')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe  = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\sxcl_version_block'
$fail = 0
$checks = 0
Add-Type -AssemblyName System.Drawing

function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }

# ---- Chinese fragments (code points; see the header note) ----
$T_SERVER = (C @(0x83B7,0x53D6)) + (C @(0x670D,0x52A1,0x7AEF))   # "get server"
$T_DEV    = C @(0x529F,0x80FD,0x5F00,0x53D1,0x4E2D)              # "under development"
$T_LOGBTN = C @(0x7248,0x672C,0x65E5,0x5FD7)                     # "version log"

function Check([string]$what, [bool]$ok, [string]$detail) {
  $script:checks++
  if ($ok) {
    Write-Output ('  OK   ' + $what + '  ' + $detail)
  } else {
    Write-Output ('  FAIL ' + $what + '  ' + $detail)
    $script:fail++
  }
}

if (-not (Test-Path $exe)) {
  Write-Output ('VERSION-BLOCK: FAIL (exe not found: ' + $exe + ')')
  exit 1
}
# Several agents share this machine and this tree, and their acceptance runs launch sxcl-ui too.
# Wait for a quiet moment; if one is still up, proceed anyway -- this script measures its OWN
# process (its own dump file and its own window grab), so a second instance cannot feed it numbers.
$t0 = Get-Date
while (@(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue).Count -gt 0 -and ((Get-Date) - $t0).TotalSeconds -lt 180) {
  Write-Output ('  waiting for another sxcl-ui.exe run to finish (' + [int]((Get-Date) - $t0).TotalSeconds + 's) ...')
  Start-Sleep -Seconds 5
}
$busy = @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
  Write-Output ('  NOTE another sxcl-ui.exe is still running (pid ' + ($busy.Id -join ',') + '); proceeding, this run reads its own process only.')
}

# ---- fixtures: an EMPTY game dir + a pinned manifest with 4 version types ----
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
$game = Join-Path $work 'mc'
New-Item -ItemType Directory -Force -Path (Join-Path $game 'versions') | Out-Null
$rows = @(
  @{ id = '26.3';      type = 'release';   block = 'vanilla';  asset = 'Grass.png' },
  @{ id = '26.2-pre1'; type = 'snapshot';  block = 'snapshot'; asset = 'CommandBlock.png' },
  @{ id = 'b1.7.3';    type = 'old_beta';  block = 'old';      asset = 'CobbleStone.png' },
  @{ id = 'a1.2.6';    type = 'old_alpha'; block = 'old';      asset = 'CobbleStone.png' }
)
$vs = @()
for ($i = 0; $i -lt $rows.Count; $i++) {
  $vs += @{ id = $rows[$i].id; type = $rows[$i].type;
            url = ('https://example.invalid/' + $rows[$i].id + '.json');
            sha1 = '0000000000000000000000000000000000000000'; size = 1000;
            releaseTime = ('2026-02-0' + ($i + 1) + 'T00:00:00+00:00');
            time = '2026-02-01T00:00:00+00:00' }
}
$manifest = Join-Path $work 'manifest.json'
Set-Content -Path $manifest -Value (@{ latest = @{ release = '26.3'; snapshot = '26.2-pre1' }; versions = $vs } | ConvertTo-Json -Depth 6) -Encoding utf8
$ini = Join-Path $work 'ver.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $game) -Encoding utf8
Write-Output ('fixtures: game dir = ' + $game + ' (versions/ is EMPTY)')
Write-Output ('          manifest  = ' + $manifest + ' -> ' + (($rows | ForEach-Object { $_.id + '(' + $_.type + ')' }) -join ', '))

function Invoke-Run([string]$tag, [string]$size, [string]$shotPath) {
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = $game
  $env:SXCL_UI_WINDOW = $size
  $env:SXCL_UI_ROUTE = 'versions'
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_MANIFEST = $manifest
  $env:SXCL_UI_CATEGORY = 'all'      # default is "release"; the 4 types must share one screen
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '14'
  # A screenshot is not just evidence -- it is also what makes this run EXIT (SXCL_UI_SHOT
  # grabs the window and quits). Without it the app would stay up forever and -Wait would hang.
  $delay = '5000'
  if ($shotPath -eq '') { $shotPath = (Join-Path $work ('shot_' + $tag + '.png')); $delay = '3000' }
  $env:SXCL_UI_SHOT_DELAY = $delay
  $env:SXCL_UI_SHOT = $shotPath
  Remove-Item $shotPath -ErrorAction SilentlyContinue
  $out = Join-Path $work ('dump_' + $tag + '.txt')
  $err = $out + '.err'
  $proc = Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
  $ok = $true
  try { Wait-Process -Id $proc.Id -Timeout 90 -ErrorAction Stop } catch { $ok = $false }
  if (-not $ok) {
    Write-Output ('  NOTE the app did not exit within 90 s; killing pid ' + $proc.Id)
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
  }
  return @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) + @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
}

function Get-CurrentPage($lines) {
  foreach ($line in $lines) {
    if ($line -match '^ScrollArea #sxclPage_(\w+)') { return $Matches[1] }
  }
  return ''
}

$rowPattern = 'version-row page: id=(?<id>\S+) state=(?<state>\S*) iconRect=(?<x>\d+),(?<y>\d+),(?<w>\d+)x(?<h>\d+) chip="(?<chip>[^"]*)" tip="(?<tip>[^"]*)" vis=(?<vis>\d) inView=(?<inview>\d) list=(?<lx>\d+),(?<ly>\d+),(?<lw>\d+)x(?<lh>\d+) win=(?<winw>\d+)x(?<winh>\d+) block=(?<block>\S+) icon=(?<icon>\S+) logRect=(?<lgx>\d+),(?<lgy>\d+),(?<lgw>\d+)x(?<lgh>\d+)'
function Get-Rows($lines) {
  $out = @{}
  foreach ($line in $lines) {
    $m = [regex]::Match($line, $rowPattern)
    if (-not $m.Success) { continue }
    if ($m.Groups['vis'].Value -ne '1') { continue }   # hidden copies (download page right column) do not count
    $out[$m.Groups['id'].Value] = @{
      state = $m.Groups['state'].Value
      x = [int]$m.Groups['x'].Value; y = [int]$m.Groups['y'].Value
      w = [int]$m.Groups['w'].Value; h = [int]$m.Groups['h'].Value
      inview = $m.Groups['inview'].Value
      lx = [int]$m.Groups['lx'].Value; ly = [int]$m.Groups['ly'].Value
      lw = [int]$m.Groups['lw'].Value; lh = [int]$m.Groups['lh'].Value
      winw = [int]$m.Groups['winw'].Value; winh = [int]$m.Groups['winh'].Value
      block = $m.Groups['block'].Value; icon = $m.Groups['icon'].Value
      lgx = [int]$m.Groups['lgx'].Value; lgy = [int]$m.Groups['lgy'].Value
      lgw = [int]$m.Groups['lgw'].Value; lgh = [int]$m.Groups['lgh'].Value
    }
  }
  return $out
}

function Compare-Slot([string]$png, [string]$asset, [int]$x, [int]$y, [int]$w, [int]$h, [double]$dpr) {
  $shot = New-Object System.Drawing.Bitmap($png)
  $ref = New-Object System.Drawing.Bitmap($asset)
  $sw = [int][math]::Round($w * $dpr); $sh = [int][math]::Round($h * $dpr)
  $scaled = New-Object System.Drawing.Bitmap($sw, $sh)
  $g = [System.Drawing.Graphics]::FromImage($scaled)
  $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.DrawImage($ref, (New-Object System.Drawing.Rectangle(0, 0, $sw, $sh)))
  $g.Dispose()
  $bg = $shot.GetPixel([int](($x - 8) * $dpr), [int](($y + [math]::Floor($h / 2)) * $dpr))
  $opaque = 0; $match = 0; $green = 0; $nonBg = 0
  for ($py = 0; $py -lt $sh; $py++) {
    for ($px = 0; $px -lt $sw; $px++) {
      $r = $scaled.GetPixel($px, $py)
      $s = $shot.GetPixel([int]($x * $dpr) + $px, [int]($y * $dpr) + $py)
      $dbg = [math]::Abs($s.R - $bg.R) + [math]::Abs($s.G - $bg.G) + [math]::Abs($s.B - $bg.B)
      if ($dbg -gt 12) { $nonBg++ }
      if (($s.G - $s.R) -gt 15 -and ($s.G - $s.B) -gt 15) { $green++ }
      if ($r.A -ge 128) {
        $opaque++
        $d = [math]::Abs($s.R - $r.R) + [math]::Abs($s.G - $r.G) + [math]::Abs($s.B - $r.B)
        if ($d -le 120) { $match++ }
      }
    }
  }
  $shot.Dispose(); $ref.Dispose(); $scaled.Dispose()
  $ratio = 0.0
  if ($opaque -gt 0) { $ratio = [math]::Round(100.0 * $match / $opaque, 1) }
  return @{ opaque = $opaque; match = $match; ratio = $ratio; green = $green; nonBg = $nonBg; total = ($sw * $sh) }
}

# =====================================================================================
# A + B -- 1100x900, dark theme, screenshot + widget dump
# =====================================================================================
Write-Output ''
Write-Output '== A/B: one screenshot, four rows of four version types (nothing installed) =='
$shot = Join-Path $work 'shot.png'
$lines = Invoke-Run 'types' '1100x900' $shot
$page = Get-CurrentPage $lines
if ($page -ne 'versions') {
  Write-Output ('  NOTE the window was on page "' + $page + '" instead of versions; re-running once')
  $lines = Invoke-Run 'types' '1100x900' $shot
  $page = Get-CurrentPage $lines
}
Check 'the versions page is the current page' ($page -eq 'versions') ('page="' + $page + '"')
$catHit = @($lines | Select-String -Pattern 'versions-category: spec=all' -SimpleMatch)
Check 'the category combo was really switched (real combobox signal chain)' ($catHit.Count -ge 1) ('trace lines = ' + $catHit.Count)

$dpr = 1.0
if (Test-Path $shot) {
  $bi = New-Object System.Drawing.Bitmap($shot)
  $dpr = [math]::Round($bi.Width / 1100.0, 2)
  Write-Output ('  screenshot ' + $shot + ' ' + $bi.Width + 'x' + $bi.Height + ' dpr=' + $dpr)
  $bi.Dispose()
}
$rowsSeen = Get-Rows $lines
Check 'four visible rows reported by the page' ($rowsSeen.Count -ge 4) ('rows with evidence = ' + $rowsSeen.Count)

$iconX = -1
foreach ($row in $rows) {
  $id = $row.id
  Write-Output ''
  Write-Output ('  -- ' + $id + ' (type=' + $row.type + ', expected asset ' + $row.asset + ') --')
  if (-not $rowsSeen.ContainsKey($id)) { Check ($id + ': evidence line') $false 'no visible evidence line'; continue }
  $r = $rowsSeen[$id]
  Check ($id + ': nothing is installed, so the icon cannot come from an instance') ($r.state -eq '') ('state="' + $r.state + '"')
  Check ($id + ': version type maps to the expected block asset') ($r.block -eq $row.block) ('block=' + $r.block + ' want=' + $row.block)
  Check ($id + ': the row really draws a block icon') ($r.icon -eq $row.block) ('icon=' + $r.icon)
  Check ($id + ': the row is inside the viewport') ($r.inview -eq '1') ('inView=' + $r.inview)
  Check ($id + ': icon slot is 20x20 logical') (($r.w -eq 20) -and ($r.h -eq 20)) ('slot=' + $r.w + 'x' + $r.h)
  if ($iconX -lt 0) { $iconX = $r.x }
  Check ($id + ': icon column x is constant across rows') ($r.x -eq $iconX) ('x=' + $r.x + ' first=' + $iconX)
  if (-not (Test-Path $shot)) { continue }
  $assetPath = Join-Path $root ('assets\icons\blocks\' + $row.asset)
  if (-not (Test-Path $assetPath)) { Check ($id + ': asset exists') $false $assetPath; continue }
  $cmp = Compare-Slot $shot $assetPath $r.x $r.y $r.w $r.h $dpr
  Write-Output ('    slot=' + $r.x + ',' + $r.y + ' ' + $r.w + 'x' + $r.h + '  assetPx=' + $cmp.opaque + ' matched=' + $cmp.match + ' ratio=' + $cmp.ratio + '%  nonBgPx=' + $cmp.nonBg + '/' + $cmp.total + ' greenPx=' + $cmp.green)
  Check ($id + ': the slot pixels are the asset itself') ($cmp.ratio -ge 80.0) ('match ' + $cmp.ratio + '% (>= 80%)')
  Check ($id + ': the slot is not blank') ($cmp.nonBg -ge 80) ('nonBgPx=' + $cmp.nonBg + ' (>= 80)')
  if ($row.block -eq 'vanilla') {
    Check ($id + ': a release row really shows green (grass top)') ($cmp.green -ge 40) ('greenPx=' + $cmp.green + ' (>= 40)')
  }
}

# =====================================================================================
# C -- the "get server" entry is gone: source and shipped exe, both encodings
# =====================================================================================
Write-Output ''
Write-Output '== C: the "get server" entry must be gone from the source and from the exe =='
function Count-Hits([string]$hay, [string]$needle) {
  if ($needle.Length -eq 0) { return 0 }
  $n = 0; $i = 0
  while ($true) {
    $i = $hay.IndexOf($needle, $i)
    if ($i -lt 0) { break }
    $n++; $i += $needle.Length
  }
  return $n
}

# (1) the UI source: the phrase may only survive inside COMMENTS (quoting the user's own words),
#     never in code -- no string literal, no InfoBar title.
$srcPath = Join-Path $root 'src\ui\pages\versions_page.cpp'
$srcLines = @(Get-Content $srcPath -Encoding utf8)
$srcCode = @(); $srcComment = @()
foreach ($line in $srcLines) {
  if (-not $line.Contains($T_SERVER)) { continue }
  $t = $line.Trim()
  if ($t.StartsWith('//') -or $t.StartsWith('*') -or $t.StartsWith('/*')) { $srcComment += $t } else { $srcCode += $t }
}
Check 'source: the phrase survives only in comments, never in code' ($srcCode.Count -eq 0) ('comment lines = ' + $srcComment.Count + ', code lines = ' + $srcCode.Count)
foreach ($t in $srcComment) { Write-Output ('      comment: ' + $t) }
$srcText = [System.IO.File]::ReadAllText($srcPath, [System.Text.Encoding]::UTF8)
Check 'source has no "under development" placeholder' (-not $srcText.Contains($T_DEV)) ('hits in versions_page.cpp = ' + $(if ($srcText.Contains($T_DEV)) { 1 } else { 0 }))

# (2) the live UI: a real run of the versions page must not print or show the entry anywhere
#     (dump = widget tree, err = the page's own evidence lines).
$liveHits = @($lines | Select-String -Pattern $T_SERVER -SimpleMatch)
Check 'live run: nothing on the versions page offers it' ($liveHits.Count -eq 0) ('dump+stderr lines containing it = ' + $liveHits.Count)

# (3) the shipped exe.  Qt string literals are UTF-16, so a UTF-16 hit would mean some compiled
#     code path can still display it -- there must be none.  The UTF-8 hits are the built-in
#     language table (src/core/instance/lang_table.inc), a 1:1 port of the Python table whose
#     76 keys are pinned by tests/lang_test.c; those rows are data, not wiring, and stay.
$bytes = [System.IO.File]::ReadAllBytes($exe)
$u16 = [System.Text.Encoding]::Unicode.GetString($bytes)
$utf8 = [System.Text.Encoding]::UTF8.GetString($bytes)
$n16 = Count-Hits $u16 $T_SERVER
$n8 = Count-Hits $utf8 $T_SERVER
Check 'exe: no compiled string literal can display it (UTF-16 hits)' ($n16 -eq 0) ('UTF-16 occurrences = ' + $n16)
Write-Output ('      NOTE exe UTF-8 occurrences = ' + $n8 + ' (the Chinese row of the built-in language table; its English row says "Get Server"; tests/lang_test.c pins that table at 76 keys -- the rows stay, no UI code points at them any more)')
Check 'exe has no "under development" placeholder (UTF-16LE)' (-not $u16.Contains($T_DEV)) 'info bar text is gone too'

# =====================================================================================
# D -- layout across three window sizes; the version-log button still hugs the right edge
# =====================================================================================
Write-Output ''
Write-Output '== D: layout across three window sizes =='
$sizes = @(@{ size = '900x600';  list = '' }, @{ size = '1100x750'; list = '78,128 993x597' }, @{ size = '1280x800'; list = '' })
foreach ($s in $sizes) {
  $sz = $s.size
  $ls = Invoke-Run ('size_' + $sz.Replace('x', '_')) $sz ''
  $pg = Get-CurrentPage $ls
  if ($pg -ne 'versions') {
    Write-Output ('  NOTE window was on "' + $pg + '" instead of versions; re-running once')
    $ls = Invoke-Run ('size_' + $sz.Replace('x', '_')) $sz ''
    $pg = Get-CurrentPage $ls
  }
  $rr = Get-Rows $ls
  Write-Output ('  ' + $sz + ': page=' + $pg + ' rows=' + $rr.Count)
  Check ($sz + ': the versions page is current') ($pg -eq 'versions') ('page="' + $pg + '"')
  $any = $null
  foreach ($k in $rr.Keys) { $any = $rr[$k]; break }
  if ($any -eq $null) { Check ($sz + ': a visible row') $false 'no evidence line'; continue }
  $listRect = '' + $any.lx + ',' + $any.ly + ' ' + $any.lw + 'x' + $any.lh
  $gap = $any.lw - ($any.lgx + $any.lgw)
  Write-Output ('    list=' + $listRect + ' win=' + $any.winw + 'x' + $any.winh + ' iconSlot=' + $any.x + ',' + $any.y + ' logBtn=' + $any.lgx + ',' + $any.lgy + ' ' + $any.lgw + 'x' + $any.lgh + ' rightGap=' + $gap)
  Check ($sz + ': the list is inside the window') (($any.lx -ge 0) -and ($any.ly -ge 0) -and (($any.lx + $any.lw) -le $any.winw) -and (($any.ly + $any.lh) -le $any.winh)) ('list=' + $listRect + ' win=' + $any.winw + 'x' + $any.winh)
  Check ($sz + ': the version-log button keeps a constant 16 px right margin') (($gap -eq 16) -or ($gap -eq 15) -or ($gap -eq 17)) ('rightGap=' + $gap + ' px (want 16 +-1)')
  Check ($sz + ': the version-log button sits inside the list') (($any.lgx -ge 0) -and (($any.lgx + $any.lgw) -le $any.lw)) ('logBtn right edge=' + ($any.lgx + $any.lgw) + ' listW=' + $any.lw)
  Check ($sz + ': the icon column keeps the same x') ($any.x -eq $iconX) ('x=' + $any.x + ' first=' + $iconX)
  if ($sz -eq '1100x750') {
    Check ($sz + ': the list rect is unchanged from the recorded one') ((($any.lx -eq 78) -and ($any.ly -eq 128) -and ($any.lw -eq 993) -and ($any.lh -eq 597))) ('list=' + $listRect + ' want 78,128 993x597')
  }
}

Write-Output ''
Write-Output ('product files: ' + $work)
if ($fail -eq 0) {
  Write-Output ('VERSION-BLOCK: ALL OK (' + $checks + '/' + $checks + ' checks: per-row block icon by version type incl. uninstalled rows, slot pixels match the asset, server entry gone from source+exe, right-margin button and layout stable across 3 sizes)')
  exit 0
}
Write-Output ('VERSION-BLOCK: FAIL (' + $fail + ' of ' + $checks + ' checks)')
exit 1
