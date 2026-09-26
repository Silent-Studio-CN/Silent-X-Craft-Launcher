# (C) Silent X Craft Launcher -- "no fluff" text acceptance for the download page + config page.
# User 2026-09-26 (translated): "official/mirror dual source -- do I need you to tell me that?
# 'which version to install' -- do I need you to tell me that? 'N versions in total' -- do I need
# you to tell me that? I have said this many times: drop the filler, delete every filler sentence."
# The script dumps both pages (SXCL_UI_DUMP=1) and asserts:
#   * every banned phrase is GONE from the visible text of both pages
#   * the texts that must stay (field labels / actions / status) are still there
# It also prints every visible string of both pages, so the before/after list can be read off.
# (Chinese literals are built from code points on purpose: this file stays pure ASCII.)
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
$work = 'D:\SilentStudio\_test\sxcl_tasks\no_fluff'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$ini  = Join-Path $work 'cfg.ini'
Set-Content -Path $ini -Value ('game.default_dir=' + $work + '\.minecraft') -Encoding utf8

function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }

# ---- banned phrases (the fluff the user named) ----------------------------------------------
$banned = [ordered]@{
  'download subtitle: dual source / silent install' = (C @(0x5B98,0x65B9,0x20,0x2F,0x20,0x955C,0x50CF,0x53CC,0x8DEF))
  'silent install'                                 = (C @(0x9759,0x9ED8,0x5B89,0x88C5))
  'which version to install (hint)'                = (C @(0x8981,0x88C5,0x54EA,0x4E2A,0x7248,0x672C))
  'inner page title Minecraft version'             = ('Minecraft ' + (C @(0x7248,0x672C)))
  'click a row to select (hint)'                   = (C @(0x70B9,0x4E00,0x884C,0x9009,0x4E2D))
  'no key needed (explanation)'                    = ((C @(0x514D)) + ' key')
  'official API requires x-api-key'                = ((C @(0x5B98,0x65B9)) + ' API ' + (C @(0x8981,0x6C42,0x5E26)))
}
$bannedRegex = @( ((C @(0x5171)) + '\s*\d+\s*' + (C @(0x4E2A,0x7248,0x672C))) )   # "N versions in total"
# ---- texts that must survive (per page: each page only carries its own) -----------------------
$keptByPage = @{
  'download-page' = [ordered]@{
    'download page title'   = (C @(0x4E0B,0x8F7D))
    'refresh action'        = (C @(0x5237,0x65B0))
    'retry action'          = (C @(0x91CD,0x8BD5))
    'release channel combo' = (C @(0x6B63,0x5F0F,0x7248))
  }
  'config-page' = [ordered]@{
    'page title (install X)'   = (C @(0x5B89,0x88C5))
    'back action'              = (C @(0x8FD4,0x56DE) + (C @(0x7248,0x672C,0x5217,0x8868)))
    'field label: version name'= (C @(0x7248,0x672C,0x540D,0x79F0))
    'section: mod loader'      = (C @(0x6A21,0x7EC4,0x52A0,0x8F7D,0x5668))
    'loader status: not chosen'= (C @(0x672A,0x9009,0x62E9))
    'primary action'           = (C @(0x5F00,0x59CB,0x4E0B,0x8F7D))
  }
}

function Get-VisibleTexts([string[]]$txt) {
  $rows = @()
  foreach ($line in $txt) {
    if ($line -notmatch '^\s+\S+ \(\d+,\d+ \d+x\d+\)') { continue }
    if ($line -notmatch '"([^"]*)"') { continue }
    $rows += $Matches[1]
  }
  return $rows
}

$bad = @()
$removed = 0
$keptHits = 0
$allTexts = @()
foreach ($case in @(@{tag='download-page'; config=$null}, @{tag='config-page'; config='1.20.1'})) {
  $env:SXCL_UI_WINDOW = '1100x750'
  $env:SXCL_UI_ROUTE = 'download'
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '12'
  $env:SXCL_UI_SHOT = (Join-Path $work ('shot_' + $case.tag + '.png'))
  $env:SXCL_UI_SHOT_DELAY = '5000'
  if ($case.config) { $env:SXCL_UI_CONFIG = $case.config } else { Remove-Item Env:SXCL_UI_CONFIG -ErrorAction SilentlyContinue }
  $out = Join-Path $work ($case.tag + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $txt = @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  $texts = Get-VisibleTexts $txt
  $allTexts += @($texts | ForEach-Object { '[' + $case.tag + '] ' + $_ })

  foreach ($name in $banned.Keys) {
    $hit = @($texts | Where-Object { $_ -like ('*' + $banned[$name] + '*') })
    if ($hit.Count -eq 0) { $removed++ } else { $bad += ('[' + $case.tag + '] banned phrase still visible (' + $name + '): ' + $hit[0]) }
  }
  foreach ($re in $bannedRegex) {
    $hit = @($texts | Where-Object { $_ -match $re })
    if ($hit.Count -eq 0) { $removed++ } else { $bad += ('[' + $case.tag + '] banned count line still visible: ' + $hit[0]) }
  }
  $kept = $keptByPage[$case.tag]
  foreach ($name in $kept.Keys) {
    if (@($texts | Where-Object { $_ -like ('*' + $kept[$name] + '*') }).Count -gt 0) { $keptHits++ }
    else { $bad += ('[' + $case.tag + '] required text missing (' + $name + '): ' + $kept[$name]) }
  }
  Write-Output ('  [' + $case.tag + '] visible strings = ' + $texts.Count + ', banned patterns clean = ' + ($banned.Count + $bannedRegex.Count) + ', required texts present = ' + $keptHits)
}

Write-Output '  --- visible texts now on both pages ---'
foreach ($t in $allTexts) { Write-Output ('    ' + $t) }

if ($bad.Count -eq 0) {
  Write-Output ('NO-FLUFF: ALL OK (removed=' + $removed + ' banned-phrase checks all clean over 2 pages, kept=' + $keptHits + ' required texts still present)')
  exit 0
}
foreach ($b in $bad) { Write-Output ('  FAIL ' + $b) }
Write-Output ('NO-FLUFF: FAIL (' + $bad.Count + ' assertion(s))')
exit 1
