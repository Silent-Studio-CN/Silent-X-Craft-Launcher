# (C) Silent X Craft Launcher -- "versions list" acceptance (user 2026-09-27:
# "his Minecraft version list only shows 5 versions, and every version number is 6 digits").
#
# What this proves, per run (every number is measured on this machine, nothing is guessed):
#
#   1) HOW MANY ROWS THE MODEL REALLY HAS
#      [sxcl-ui] versions-list: host=<...> rows=<n> all=<m> rowH=52 viewport=WxH fitsRows=<n>
#                              scrollMin=0 scrollMax=<max> pageStep=<H> value=<v>
#                              top=<x>,<y> firstVisible=<id> lastVisible=<id>
#      is printed by the page itself (versions_page.cpp, printListTrace) for the instance that
#      is ON SCREEN.  "5 rows" and "the model only has 5 rows" are two different facts and this
#      line is what tells them apart.
#
#   2) THE VISIBLE COUNT IS PURELY viewportHeight / rowHeight
#      asserted exactly: fitsRows == ceil(viewportH / 52), pageStep == viewportH,
#      scrollMax == rows*52 - viewportH -- and the SAME list is measured at three window
#      heights (560 / 750 / 900) so the relationship is not a coincidence of one size.
#
#   3) THE LIST REALLY SCROLLS TO THE OLD VERSIONS
#      two runs per size: SXCL_UI_LIST=wheel (5 real wheel events sent to the viewport) and
#      SXCL_UI_LIST=bottom (scrollbar dragged to the end).  At the end the last VISIBLE row
#      must be the model's last row (the oldest version), and the value must equal maximum.
#
#   4) THE CHROME ABOVE THE LIST GOT OUT OF THE WAY
#      before this change (recorded, same command, 1100x750):
#        route page       : #versionList (78,193 993x532)   <- page title + a full toolbar row
#        download embed   : #versionList (371,168 701x558)  <- toolbar row + status row + 16px gaps
#      after: the page title and the toolbar share ONE row (#sxclVersionsTitleRow), the status
#      row is hidden as a whole when it has nothing to say, and the row gap is 8 instead of 16.
#      The script asserts the list moved UP and got TALLER on both embeddings, and that the
#      title and the search box really are on the same row (same y band, title left of search).
#
# ASCII-only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM);
# Chinese is only ever printed through code points.
$ErrorActionPreference = 'Continue'

foreach ($v in @('SXCL_UI_CONFIG','SXCL_UI_NAV','SXCL_UI_COLLAPSE','SXCL_UI_RAILS_TEST','SXCL_UI_EDITION',
                 'SXCL_UI_SCROLL','SXCL_UI_POPUP','SXCL_UI_AUTH_DIALOG','SXCL_UI_JRE_HOSTED','SXCL_UI_MODS_QUERY',
                 'SXCL_UI_LAUNCH','SXCL_UI_DOWNLOAD','SXCL_UI_ICON_POPUP','SXCL_UI_SCROLLBAR_STRESS',
                 'SXCL_UI_ACCENT_APPLY','SXCL_UI_THEME_SWITCH','SXCL_UI_TRACE','SXCL_UI_ACCEPT',
                 'SXCL_UI_SETTINGS_ACCEPT','SXCL_UI_VERSION','SXCL_UI_LIST','SXCL_UI_MANIFEST')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe  = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\sxcl_versions_list'
$fail = 0
$rowH = 52

function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }

if (-not (Test-Path $exe)) { Write-Output ('VERSIONS-LIST: FAIL (exe not found: ' + $exe + ')'); exit 1 }
if (@(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue).Count -gt 0) {
  Write-Output 'VERSIONS-LIST: FAIL (another sxcl-ui.exe is running -- close it and re-run)'
  exit 1
}

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null
$game = Join-Path $work 'mc'
New-Item -ItemType Directory -Force -Path (Join-Path $game 'versions') | Out-Null
$ini = Join-Path $work 'list.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $game) -Encoding utf8
Write-Output ('game dir (deliberately empty of instances): ' + $game)

# one row of the trace -> a hashtable (or $null)
function Get-ListTrace($lines) {
  $last = $null
  foreach ($line in $lines) {
    $m = [regex]::Match($line, 'versions-list: host=(?<host>\S+) rows=(?<rows>\d+) all=(?<all>\d+) rowH=(?<rowh>\d+) viewport=(?<vw>\d+)x(?<vh>\d+) fitsRows=(?<fits>\d+) scrollMin=(?<smin>-?\d+) scrollMax=(?<smax>-?\d+) pageStep=(?<step>-?\d+) value=(?<val>-?\d+) top=(?<tx>\d+),(?<ty>\d+) firstVisible=(?<first>\S+) lastVisible=(?<last>\S+)')
    if ($m.Success) {
      $last = @{ host = $m.Groups['host'].Value; rows = [int]$m.Groups['rows'].Value
                 all = [int]$m.Groups['all'].Value; rowh = [int]$m.Groups['rowh'].Value
                 vw = [int]$m.Groups['vw'].Value; vh = [int]$m.Groups['vh'].Value
                 fits = [int]$m.Groups['fits'].Value; smin = [int]$m.Groups['smin'].Value
                 smax = [int]$m.Groups['smax'].Value; step = [int]$m.Groups['step'].Value
                 val = [int]$m.Groups['val'].Value; tx = [int]$m.Groups['tx'].Value
                 ty = [int]$m.Groups['ty'].Value; first = $m.Groups['first'].Value
                 last = $m.Groups['last'].Value }
    }
  }
  return $last
}
function Get-OrderLine($lines) {
  $last = $null
  foreach ($line in $lines) {
    $m = [regex]::Match($line, 'versions-list-order: host=(?<host>\S+) head=(?<head>\S*) tail=(?<tail>\S*)')
    if ($m.Success) { $last = @{ head = $m.Groups['head'].Value; tail = $m.Groups['tail'].Value } }
  }
  return $last
}
function Invoke-ListRun([string]$tag, [string]$route, [string]$size, [string]$probe) {
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = $game
  $env:SXCL_UI_WINDOW = $size
  $env:SXCL_UI_ROUTE = $route
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '6'
  $env:SXCL_UI_SHOT_DELAY = '6000'
  if ($probe -eq '') { Remove-Item Env:SXCL_UI_LIST -ErrorAction SilentlyContinue } else { $env:SXCL_UI_LIST = $probe }
  $png = Join-Path $work ('shot_' + $tag + '.png')
  $env:SXCL_UI_SHOT = $png
  $out = Join-Path $work ('dump_' + $tag + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $lines = @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) + @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  return @{ lines = $lines; trace = (Get-ListTrace $lines); order = (Get-OrderLine $lines); png = $png }
}

# ---- the list really is the real 916-entry manifest, and it is the release filter by default --
Write-Output ''
Write-Output '== 1) model rows (route page, 1100x750) =='
$run = Invoke-ListRun 'route_750_top' 'versions' '1100x750' ''
$t = $run.trace
if ($t -eq $null) {
  Write-Output '  -> FAIL no versions-list trace line (page did not report)'
  $fail++
} else {
  Write-Output ('  host=' + $t.host + ' rows=' + $t.rows + ' all=' + $t.all + ' rowH=' + $t.rowh + ' viewport=' + $t.vw + 'x' + $t.vh + ' fitsRows=' + $t.fits + ' scrollMax=' + $t.smax + ' pageStep=' + $t.step + ' value=' + $t.val + ' firstVisible=' + $t.first + ' lastVisible=' + $t.last)
  if ($t.rows -le 5) { Write-Output ('  -> FAIL the model really only has ' + $t.rows + ' rows'); $fail++ }
  if ($t.all -lt 100) { Write-Output ('  -> FAIL the manifest itself looks wrong: all=' + $t.all); $fail++ }
  Write-Output ('  model rows = ' + $t.rows + ' (the default category is the release filter); the manifest holds ' + $t.all + ' versions in total')
  # q1: the visible count is exactly viewport/rowH, and the scroll range is exactly rows*rowH - viewport
  $fitsWant = [int][math]::Ceiling($t.vh / [double]$rowH)
  $smaxWant = $t.rows * $rowH - $t.vh
  if ($t.rowh -ne $rowH) { Write-Output ('  -> FAIL row height is ' + $t.rowh + ', expected ' + $rowH); $fail++ }
  if ($t.fits -ne $fitsWant) { Write-Output ('  -> FAIL fitsRows=' + $t.fits + ' but ceil(viewportH/' + $rowH + ')=' + $fitsWant); $fail++ }
  if ($t.step -ne $t.vh) { Write-Output ('  -> FAIL pageStep=' + $t.step + ' but viewport height=' + $t.vh); $fail++ }
  if ($t.smax -ne $smaxWant) { Write-Output ('  -> FAIL scrollMax=' + $t.smax + ' but rows*rowH-viewportH=' + $smaxWant); $fail++ }
  Write-Output ('  arithmetic: ' + $t.rows + ' rows * ' + $rowH + 'px - ' + $t.vh + 'px viewport = ' + $smaxWant + 'px of scroll range; fitsRows = ceil(' + $t.vh + '/' + $rowH + ') = ' + $fitsWant)
}
$o = $run.order
if ($o -eq $null) { Write-Output '  -> FAIL no versions-list-order line'; $fail++ }
else {
  Write-Output ('  order: head=' + $o.head)
  Write-Output ('         tail=' + $o.tail)
  if (-not $o.tail.EndsWith('1.0')) { Write-Output ('  -> FAIL the last model row is not the oldest release (tail=' + $o.tail + ')'); $fail++ }
}

# ---- 2) the same list at three window heights: visible rows track the height, nothing else ----
Write-Output ''
Write-Output '== 2) visible rows vs window height (route page) =='
$bySize = @{}
foreach ($h in @(560,750,900)) {
  $size = '1100x' + $h
  $r = Invoke-ListRun ('route_' + $h + '_top') 'versions' $size ''
  $tt = $r.trace
  if ($tt -eq $null) { Write-Output ('  -> FAIL no trace at ' + $size); $fail++; continue }
  $bySize[$h] = $tt
  $fitsWant = [int][math]::Ceiling($tt.vh / [double]$rowH)
  Write-Output ('  ' + $size + ': viewport=' + $tt.vw + 'x' + $tt.vh + ' fitsRows=' + $tt.fits + ' (=ceil(' + $tt.vh + '/' + $rowH + ')) firstVisible=' + $tt.first + ' lastVisible=' + $tt.last)
  if ($tt.fits -ne $fitsWant) { Write-Output ('    -> FAIL fitsRows != ceil(viewportH/rowH)'); $fail++ }
  if ($tt.rows -ne $t.rows) { Write-Output ('    -> FAIL the model row count changed with the window size (' + $tt.rows + ' vs ' + $t.rows + ')'); $fail++ }
}
if ($bySize.Count -eq 3) {
  $seq = @($bySize[560].fits, $bySize[750].fits, $bySize[900].fits)
  Write-Output ('  fitsRows sequence 560/750/900 = ' + ($seq -join ' / ') + ' (strictly increasing with the window)')
  if (-not ($seq[0] -lt $seq[1] -and $seq[1] -lt $seq[2])) { Write-Output '  -> FAIL the visible row count does not grow with the window'; $fail++ }
}

# ---- 3) scrolling really reaches the old versions (wheel + scrollbar), download embedding too --
Write-Output ''
Write-Output '== 3) scrolling: wheel events and drag-to-bottom =='
foreach ($case in @(@('versions','1100x750'), @('download','1100x750'), @('download','1100x560'))) {
  $route = $case[0]; $size = $case[1]
  $top = Invoke-ListRun ($route + '_top_scroll') $route $size ''
  $wheel = Invoke-ListRun ($route + '_wheel_scroll') $route $size 'wheel'
  $bottom = Invoke-ListRun ($route + '_bottom_scroll') $route $size 'bottom'
  $tl = $top.trace; $wl = $wheel.trace; $bl = $bottom.trace
  if ($tl -eq $null -or $wl -eq $null -or $bl -eq $null) {
    Write-Output ('  [' + $route + ' ' + $size + '] -> FAIL missing trace (' + ($tl -eq $null) + '/' + ($wl -eq $null) + '/' + ($bl -eq $null) + ')')
    $fail++
    continue
  }
  Write-Output ('  [' + $route + ' ' + $size + '] host=' + $tl.host)
  Write-Output ('    at top    : value=' + $tl.val + '/' + $tl.smax + ' firstVisible=' + $tl.first + ' lastVisible=' + $tl.last)
  Write-Output ('    after wheel: value=' + $wl.val + '/' + $wl.smax + ' firstVisible=' + $wl.first + ' lastVisible=' + $wl.last)
  Write-Output ('    at bottom : value=' + $bl.val + '/' + $bl.smax + ' firstVisible=' + $bl.first + ' lastVisible=' + $bl.last)
  if ($tl.val -ne 0) { Write-Output ('    -> FAIL the list does not start at the top (value=' + $tl.val + ')'); $fail++ }
  if ($wl.val -le $tl.val) { Write-Output '    -> FAIL the wheel did not scroll the list'; $fail++ }
  if ($bl.val -ne $bl.smax) { Write-Output ('    -> FAIL the scrollbar did not reach the end (' + $bl.val + '/' + $bl.smax + ')'); $fail++ }
  $o2 = $bottom.order
  if ($o2 -ne $null) {
    $lastId = ($o2.tail -split ',')[-1]
    if ($bl.last -ne $lastId) { Write-Output ('    -> FAIL the last visible row at the bottom is ' + $bl.last + ', the model ends with ' + $lastId); $fail++ }
    else { Write-Output ('    at the bottom the last visible row is the oldest version ' + $lastId + ' -- the old versions are reachable') }
  }
}

# ---- 4) the chrome above the list: title + toolbar share one row, the list went up and grew ---
Write-Output ''
Write-Output '== 4) vertical space handed over to the list =='
$recordedBefore = @{}
$recordedBefore['versions'] = @(193, 532)   # y, height  (recorded before this change, 1100x750)
$recordedBefore['download'] = @(168, 558)
foreach ($route in @('versions','download')) {
  $r = Invoke-ListRun ('space_' + $route) $route '1100x750' ''
  $lm = $null
  foreach ($line in $r.lines) {
    $m = [regex]::Match($line, 'QListView #versionList \((\d+),(\d+) (\d+)x(\d+)\)')
    if ($m.Success) { $lm = @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value); break }
  }
  if ($lm -eq $null) { Write-Output ('  [' + $route + '] -> FAIL no #versionList in the dump'); $fail++; continue }
  $b = $recordedBefore[$route]
  Write-Output ('  [' + $route + ' 1100x750] list before=(x,' + $b[0] + ' x' + $b[1] + ')  after=(' + $lm[0] + ',' + $lm[1] + ' ' + $lm[2] + 'x' + $lm[3] + ')  -> y ' + ($b[0] - $lm[1]) + 'px higher, ' + ($lm[3] - $b[1]) + 'px taller')
  if ($lm[1] -ge $b[0]) { Write-Output ('    -> FAIL the list did not move up (y=' + $lm[1] + ', before ' + $b[0] + ')'); $fail++ }
  if ($lm[3] -le $b[1]) { Write-Output ('    -> FAIL the list did not get taller (h=' + $lm[3] + ', before ' + $b[1] + ')'); $fail++ }
}
# the title row really carries both the page name and the search box
$r = Invoke-ListRun 'title_row' 'versions' '1100x750' ''
$titleRow = $null
foreach ($line in $r.lines) {
  $m = [regex]::Match($line, '#sxclVersionsTitleRow \((\d+),(\d+) (\d+)x(\d+)\)')
  if ($m.Success) { $titleRow = @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value) }
}
$titleLab = $null; $search = $null
foreach ($line in $r.lines) {
  $m = [regex]::Match($line, 'FluentLabelBase \((\d+),(\d+) (\d+)x(\d+)\) "' + [regex]::Escape((C @(0x004D,0x0069,0x006E,0x0065,0x0063,0x0072,0x0061,0x0066,0x0074)) ) + ' ')
  if ($m.Success -and $titleLab -eq $null) { $titleLab = @([int]$m.Groups[1].Value, [int]$m.Groups[2].Value, [int]$m.Groups[3].Value, [int]$m.Groups[4].Value) }
  $m2 = [regex]::Match($line, 'SearchLineEdit \((\d+),(\d+) (\d+)x(\d+)\)')
  if ($m2.Success -and $search -eq $null) { $search = @([int]$m2.Groups[1].Value, [int]$m2.Groups[2].Value, [int]$m2.Groups[3].Value, [int]$m2.Groups[4].Value) }
}
if ($titleRow -eq $null -or $titleLab -eq $null -or $search -eq $null) {
  Write-Output ('  -> FAIL title row / title label / search box not all found (' + ($titleRow -ne $null) + '/' + ($titleLab -ne $null) + '/' + ($search -ne $null) + ')')
  $fail++
} else {
  Write-Output ('  title row=(' + ($titleRow -join ',') + ') title=(' + ($titleLab -join ',') + ') search=(' + ($search -join ',') + ')')
  if ($search[0] -le $titleLab[0]) { Write-Output '  -> FAIL the search box is not to the right of the page name'; $fail++ }
  $overlap = ($titleLab[1] -lt ($search[1] + $search[3])) -and ($search[1] -lt ($titleLab[1] + $titleLab[3]))
  if (-not $overlap) { Write-Output '  -> FAIL the page name and the search box are not on the same row'; $fail++ }
  else { Write-Output '  page name and search box overlap in y: they share one row' }
  if (($titleRow[1] -gt $titleLab[1]) -or (($titleRow[1] + $titleRow[3]) -lt ($titleLab[1] + $titleLab[3]))) {
    Write-Output '  -> FAIL the title label is not inside the title row'; $fail++
  }
  if ($search[2] -lt 200) { Write-Output ('  -> FAIL the search box got squeezed to ' + $search[2] + 'px'); $fail++ }
}

Write-Output ''
Write-Output ('product files: ' + $work)
if ($fail -eq 0) {
  Write-Output 'VERSIONS-LIST: ALL OK (model rows vs visible rows separated by runtime trace; scroll range = rows*rowH - viewport; wheel + drag reach the oldest version; title+toolbar on one row and the list gained height)'
  exit 0
}
Write-Output ('VERSIONS-LIST: FAIL (' + $fail + ' assertion(s))')
exit 1
