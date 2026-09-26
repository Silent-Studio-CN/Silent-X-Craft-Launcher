# (C) Silent X Craft Launcher -- settings page "fit to window" acceptance (user 2026-09-26).
# ASCII-only output lines; run with pwsh.
#   1) no HORIZONTAL scrollbar / drag range on the settings page at 900x600 / 1100x750 / 1280x800
#   2) every setting card's right edge <= viewport right edge
#   3) six sampled labels keep EXACTLY one line height (no squeeze into two stacked lines)
$ErrorActionPreference = 'Continue'

# hermetic: clear every SXCL_UI_* hook this script does not set itself.  A leftover from another
# acceptance run in the same shell (e.g. SXCL_UI_CONFIG opens the config page and the rail state
# machine then leaves the second-level rail collapsed) would change the measured geometry.
foreach ($v in @('SXCL_UI_CONFIG', 'SXCL_UI_NAV', 'SXCL_UI_COLLAPSE', 'SXCL_UI_RAILS_TEST',
                 'SXCL_UI_EDITION', 'SXCL_UI_SCROLL', 'SXCL_UI_POPUP', 'SXCL_UI_AUTH_DIALOG',
                 'SXCL_UI_JRE_HOSTED', 'SXCL_UI_MODS_QUERY', 'SXCL_UI_LAUNCH', 'SXCL_UI_DOWNLOAD',
                 'SXCL_UI_ICON_POPUP', 'SXCL_UI_SCROLLBAR_STRESS', 'SXCL_UI_ACCENT_APPLY',
                 'SXCL_UI_THEME_SWITCH', 'SXCL_UI_TRACE', 'SXCL_UI_ACCEPT', 'SXCL_UI_SETTINGS_ACCEPT')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe  = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\sxcl_tasks\settings_fit'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$ini  = Join-Path $work 'fit.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $work + '\.minecraft') -Encoding utf8

# Chinese sample prefixes built from code points (keeps this file encoding-proof)
function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }
$samples = @(
  (C @(0x542F,0x52A8,0x65F6,0x81EA,0x52A8,0x68C0,0x67E5,0x66F4,0x65B0)),   # startup update check (title)
  (C @(0x6D45,0x8272)),                                                     # light / dark / auto (content)
  (C @(0x6BCF,0x4E2A,0x7248,0x672C,0x5404,0x7528,0x4E00,0x5957)),           # version isolation (long content)
  (C @(0x542F,0x52A8,0x524D,0x8865,0x5168,0x65F6)),                         # file check off (longest content)
  (C @(0x8BBE,0x5907,0x7801,0x767B,0x5F55)),                                 # device code login (content)
  (C @(0x57FA,0x4E8E,0x20,0x51,0x74,0x36))                                  # about: "Qt6" (content)
)

$cases = 0
$bad = @()
foreach ($theme in @('dark', 'light')) {
  foreach ($size in @('900x600', '1100x750', '1280x800')) {
    $cases++
    $env:SXCL_UI_WINDOW = $size
    $env:SXCL_UI_ROUTE = 'settings'
    $env:SXCL_UI_THEME = $theme
    $env:SXCL_UI_SETTINGS = $ini
    $env:SXCL_UI_DUMP = '1'
    $env:SXCL_UI_SHOT = (Join-Path $work ('shot_' + $theme + '_' + $size + '.png'))
    $env:SXCL_UI_SHOT_DELAY = '3500'
    $out = Join-Path $work ('dump_' + $theme + '_' + $size + '.txt')
    $err = $out + '.err'
    Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
    $txt = @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)

    $page = $txt | Select-String -Pattern 'ScrollArea #SettingsPage \((\d+),(\d+) (\d+)x(\d+)\)' | Select-Object -First 1
    $vp   = $txt | Select-String -Pattern 'QWidget #qt_scrollarea_viewport \((\d+),(\d+) (\d+)x(\d+)\)' | Select-Object -First 1
    if ((-not $page) -or (-not $vp)) { $bad += ('[' + $theme + ' ' + $size + '] page/viewport line missing'); continue }
    $null = ($vp.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
    $vx = [int]$Matches[1]; $vw = [int]$Matches[3]
    $vRight = $vx + $vw

    # (a) horizontal scrollbar: libqf shows it iff maximum() > 0 (fluent_scroll.cpp:264/312)
    $hbars = @($txt | Select-String -Pattern 'SmoothScrollBar \((\d+),(\d+) (\d+)x(\d+)\)')
    $hVisible = 0
    foreach ($b in $hbars) {
      $null = ($b.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
      if ([int]$Matches[3] -gt [int]$Matches[4]) {   # wider than tall = the horizontal bar
        if ($b.Line -notmatch 'hidden') { $hVisible++ }
      }
    }
    # content widget = first plain QWidget child of the viewport (indent 4)
    $content = $txt | Select-String -Pattern '^ {4}QWidget \((\d+),(\d+) (\d+)x(\d+)\)' | Select-Object -First 1
    $contentW = -1
    if ($content) { $null = ($content.Line -match 'QWidget \((\d+),(\d+) (\d+)x(\d+)\)'); $contentW = [int]$Matches[3] }
    if ($hVisible -ne 0 -or $contentW -gt $vw -or $contentW -lt 0) {
      $bad += ('[' + $theme + ' ' + $size + '] (a) horizontal bar visible=' + $hVisible + ' contentW=' + $contentW + ' viewportW=' + $vw)
    }

    # (b) every card's right edge within the viewport
    $cards = 0; $worst = 0; $worstName = ''
    foreach ($line in $txt) {
      if ($line -notmatch '^\s+\S*SettingCard\S* \((\d+),(\d+) (\d+)x(\d+)\)') { continue }
      $cards++
      $right = [int]$Matches[1] + [int]$Matches[3]
      if ($right -gt $worst) { $worst = $right; $worstName = ($line.Trim() -split ' ')[0] }
    }
    if ($cards -lt 30) { $bad += ('[' + $theme + ' ' + $size + '] (b) only ' + $cards + ' cards found') }
    if ($worst -gt $vRight) { $bad += ('[' + $theme + ' ' + $size + '] (b) card right ' + $worst + ' > viewport right ' + $vRight + ' (' + $worstName + ')') }

    # (c) sampled labels: got height == single line height (text= height of a one-line label)
    $lineOk = 0
    foreach ($s in $samples) {
      $hit = $txt | Select-String -Pattern ('QLabel[^"]*"' + [regex]::Escape($s)) | Select-Object -First 1
      if (-not $hit) { $bad += ('[' + $theme + ' ' + $size + '] (c) sample not found: ' + $s); continue }
      if ($hit.Line -notmatch '\[text=(\d+)x(\d+) need=\d+x\d+ got=(\d+)x(\d+)\]') {
        $bad += ('[' + $theme + ' ' + $size + '] (c) no metrics for: ' + $s); continue
      }
      $textH = [int]$Matches[2]; $gotH = [int]$Matches[4]
      if ($hit.Line -match 'CUT-H') { $bad += ('[' + $theme + ' ' + $size + '] (c) CUT-H on: ' + $s); continue }
      if ($textH -ne $gotH) { $bad += ('[' + $theme + ' ' + $size + '] (c) height ' + $gotH + ' != one-line ' + $textH + ' on: ' + $s); continue }
      $lineOk++
    }
    Write-Output ('  [' + $theme + ' ' + $size + '] viewportW=' + $vw + ' contentW=' + $contentW + ' hbarVisible=' + $hVisible + ' cards=' + $cards + ' worstCardRight=' + $worst + '/' + $vRight + ' labelsOneLine=' + $lineOk + '/' + $samples.Count)
  }
}

if ($bad.Count -eq 0) {
  Write-Output ('SETTINGS-FIT: ALL OK (' + $cases + '/6 cases: 3 sizes x 2 themes; horizontal bar hidden, content width == viewport width; every card right edge <= viewport right; 6 sampled labels x ' + $cases + ' runs single-line)')
  exit 0
}
foreach ($b in $bad) { Write-Output ('  FAIL ' + $b) }
Write-Output ('SETTINGS-FIT: FAIL (' + $bad.Count + ' assertion(s) over ' + $cases + ' cases)')
exit 1
