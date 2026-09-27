# (C) Silent X Craft Launcher -- "mods page context version" acceptance (2026-09-27).
#
# The mods page used to write a version number it had GUESSED FROM THE INSTANCE NAME:
#   m_mc = m_instance; if it contains '-' take the head, and if that starts with 1./2. use it;
#   then the context line prefixed that guess with the Chinese word for "vanilla".  On a
#   folder called 114514 it printed "vanilla 114514" (a fact nobody stated) -- exactly the
#   called out.  This script proves the new behaviour, per run:
#
#   1) the page fetches the base version on a WORKER THREAD
#      [sxcl-ui] bg-task[mods-instance-version]: <worker thread>=0x.. <ui thread>=0x.. <verdict>=no
#      -- the two thread ids must differ and the verdict line must say "not the same thread".
#   2) the version comes from the CORE (version file only) and carries its source:
#      [sxcl-ui] mods-context: instance=233333 scanned=1 found=1 base="1.12.2"
#                              baseFrom=core:json-other coreReliable=1 loader=
#                              text="<current instance> 233333 <vanilla> 1.12.2 <isolation> on" err=""
#   3) when nothing in the JSON says which MC version it is, the line writes NOTHING:
#      base="" baseFrom=none, text has no version number at all -- and the old style
#      "vanilla <folder name>" must not appear anywhere in the dump.
#
# Fixtures (all under D:\SilentStudio\_test):
#   114514  folder name is not version-like, JSON has NO version information
#   233333  folder name is not version-like, JSON says clientVersion 1.12.2
#   1.21.4-fabric-0.15.11  folder name LOOKS like a version, JSON says nothing (the old code
#                          would have printed "vanilla 1.21.4" out of thin air)
# ASCII-only on purpose (Windows PowerShell 5.1 parses .ps1 as ANSI unless it has a BOM);
# Chinese is only ever printed through code points.
$ErrorActionPreference = 'Continue'

foreach ($v in @('SXCL_UI_CONFIG','SXCL_UI_NAV','SXCL_UI_COLLAPSE','SXCL_UI_RAILS_TEST','SXCL_UI_EDITION',
                 'SXCL_UI_SCROLL','SXCL_UI_POPUP','SXCL_UI_AUTH_DIALOG','SXCL_UI_JRE_HOSTED','SXCL_UI_MODS_QUERY',
                 'SXCL_UI_LAUNCH','SXCL_UI_DOWNLOAD','SXCL_UI_ICON_POPUP','SXCL_UI_LIST',
                 'SXCL_UI_ACCENT_APPLY','SXCL_UI_THEME_SWITCH','SXCL_UI_TRACE','SXCL_UI_ACCEPT',
                 'SXCL_UI_SETTINGS_ACCEPT','SXCL_UI_VERSION','SXCL_UI_MANIFEST')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe  = Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe'
$work = 'D:\SilentStudio\_test\sxcl_mods_version'
$fail = 0

function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }
$T_ORIGINAL = C @(0x539F,0x7248)   # "original (version)" -- the word that used to be followed by a guess

if (-not (Test-Path $exe)) { Write-Output ('MODS-VERSION: FAIL (exe not found: ' + $exe + ')'); exit 1 }
# Another sxcl-ui.exe (a colleague's acceptance run) does NOT have to stop this script: every
# number below comes from THIS run's own window (a widget grab, not a screen capture), its own
# trace lines and its own settings file + game dir -- nothing is shared with another instance.
# Still, wait a little for a quiet machine (cleaner numbers) and say so when we do not get one.
$busy = @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue)
if ($busy.Count -gt 0) {
  Write-Output ('NOTE another sxcl-ui.exe is running (pid ' + ($busy.Id -join ',') + '); waiting up to 60s for a quiet machine')
  for ($i = 0; $i -lt 6 -and @(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue).Count -gt 0; $i++) {
    Start-Sleep -Seconds 10
  }
  if (@(Get-Process -Name 'sxcl-ui' -ErrorAction SilentlyContinue).Count -gt 0) {
    Write-Output 'NOTE still busy -- continuing anyway (this script shares nothing with that instance)'
  } else {
    Write-Output 'NOTE the machine is quiet now'
  }
}

Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null
$game = Join-Path $work 'mc'
$ini = Join-Path $work 'mods.ini'

function New-Fixture([string]$id, [string]$clientVersion) {
  $dir = Join-Path $game ('versions\' + $id)
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $j = @{ id = $id; mainClass = 'net.minecraft.client.main.Main'; type = 'release'; libraries = @() }
  if ($clientVersion -ne '') { $j['clientVersion'] = $clientVersion }
  Set-Content -Path (Join-Path $dir ($id + '.json')) -Value ($j | ConvertTo-Json -Depth 5) -Encoding utf8
  Set-Content -Path (Join-Path $dir ($id + '.jar')) -Value 'fixture-jar' -Encoding ascii
}
New-Fixture '114514' ''            # no version information anywhere in the JSON
New-Fixture '233333' '1.12.2'      # clientVersion is a fact from the version file
New-Fixture '1.21.4-fabric-0.15.11' '' # name looks like a version, JSON says nothing

function Invoke-ModsRun([string]$id, [string]$tag) {
  Set-Content -Path $ini -Value ('game.default_dir=' + $game) -Encoding utf8
  Add-Content -Path $ini -Value ('game.selected_version=' + $id)
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_GAME_DIR = $game
  $env:SXCL_UI_WINDOW = '1100x750'
  $env:SXCL_UI_ROUTE = 'download'
  $env:SXCL_UI_NAV = 'download_mod'   # the mods column of the download page
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '6'
  $env:SXCL_UI_SHOT_DELAY = '5000'
  $env:SXCL_UI_SHOT = (Join-Path $work ('shot_' + $tag + '.png'))
  $out = Join-Path $work ('dump_' + $tag + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  return @(Get-Content $out -Encoding utf8 -ErrorAction SilentlyContinue) +
         @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
}
# the last context line of one run (the page prints one per pane; both panes end up identical)
function Get-Context($lines, [string]$id) {
  $last = $null
  foreach ($line in $lines) {
    $m = [regex]::Match($line, 'mods-context: instance=(?<id>\S+) scanned=(?<scanned>\d+) found=(?<found>\d+) base="(?<base>[^"]*)" baseFrom=(?<from>\S+) coreReliable=(?<rel>\d+) loader=(?<loader>\S*) text="(?<text>[^"]*)" err="(?<err>[^"]*)"')
    if ($m.Success -and $m.Groups['id'].Value -eq $id) { $last = $m }
  }
  return $last
}
function Get-BgThread($lines) {
  $pat = (C @(0x5DE5,0x4F5C,0x7EBF,0x7A0B)) + '=(?<w>\S+) ' + (C @(0x754C,0x9762,0x7EBF,0x7A0B)) + '=(?<u>\S+) ' + (C @(0x540C,0x4E00,0x6761,0x7EBF,0x7A0B)) + '=(?<same>\S+)'
  foreach ($line in $lines) {
    $m = [regex]::Match($line, 'bg-task\[mods-instance-version\]: ' + $pat)
    if ($m.Success) { return $m }
  }
  return $null
}

$cases = 0
foreach ($case in @(@('114514','',$false), @('233333','1.12.2',$false), @('1.21.4-fabric-0.15.11','',$true))) {
  $id = $case[0]; $wantBase = $case[1]; $nameLooksLikeVersion = $case[2]
  $tag = 'mods_' + ($id -replace '[^0-9A-Za-z]', '_')
  $lines = Invoke-ModsRun $id $tag
  $cases++
  Write-Output ''
  Write-Output ('== fixture "' + $id + '" (JSON clientVersion = ' + $(if ($wantBase -eq '') { '(none)' } else { $wantBase }) + '; folder name looks like a version = ' + $nameLooksLikeVersion + ') ==')

  # the dump's own copy of the line (the widget named modsContextLine) -- independent of our trace
  $dumpText = $null
  foreach ($line in $lines) {
    $dm = [regex]::Match($line, '#modsContextLine \((\d+),(\d+) (\d+)x(\d+)\) "([^"]*)"')
    if ($dm.Success) { $dumpText = $dm.Groups[5].Value }
  }
  if ($dumpText -eq $null) { Write-Output '  -> FAIL #modsContextLine not found in the widget dump'; $fail++ }
  else { Write-Output ('  widget dump line: "' + $dumpText + '"') }

  $ctx = Get-Context $lines $id
  if ($ctx -eq $null) { Write-Output '  -> FAIL no mods-context line for this instance'; $fail++; continue }
  $base = $ctx.Groups['base'].Value
  $from = $ctx.Groups['from'].Value
  $text = $ctx.Groups['text'].Value
  Write-Output ('  trace: scanned=' + $ctx.Groups['scanned'].Value + ' found=' + $ctx.Groups['found'].Value + ' base="' + $base + '" baseFrom=' + $from + ' coreReliable=' + $ctx.Groups['rel'].Value)
  Write-Output ('  line : "' + $text + '"')
  if ($ctx.Groups['scanned'].Value -ne '1') { Write-Output '  -> FAIL the instance was never scanned'; $fail++ }
  if ($ctx.Groups['found'].Value -ne '1') { Write-Output '  -> FAIL the instance directory was not found by the core scan'; $fail++ }
  if ($from -eq 'pending') { Write-Output '  -> FAIL the trace still says pending (the async result never came back)'; $fail++ }

  if ($wantBase -eq '') {
    # (a) nothing in the version file says which MC version this is -> the line must write NOTHING
    if ($base -ne '') { Write-Output ('  -> FAIL it wrote a version it cannot know: "' + $base + '"'); $fail++ }
    if ($from -ne 'none') { Write-Output ('  -> FAIL baseFrom=' + $from + ' (want none)'); $fail++ }
    # no version CLAIM at all: the word that would introduce one must not be there
    if ($text -like ('*' + $T_ORIGINAL + '*')) { Write-Output ('  -> FAIL the line still claims a vanilla version: ' + $text); $fail++ }
    if ($dumpText -ne $null -and $dumpText -like ('*' + $T_ORIGINAL + '*')) { Write-Output ('  -> FAIL the widget dump line still claims a vanilla version: ' + $dumpText); $fail++ }
    if ($nameLooksLikeVersion) {
      # this fixture's folder NAME contains digits and dots (the user picked that name); that is not
      # a claim about the MC version -- what must be absent is the vanilla-version marker above
      Write-Output '  the folder name itself looks like a version: digits in the line come from the NAME, not a claim'
    } else {
      if ($text -match '\d+\.\d+') { Write-Output '  -> FAIL the line contains a version-number-like token'; $fail++ }
      if ($dumpText -ne $null -and $dumpText -match '\d+\.\d+') { Write-Output ('  -> FAIL the widget dump line contains a version-number-like token: ' + $dumpText); $fail++ }
    }
    $oldStyle = $T_ORIGINAL + ' ' + $id
    $hits = @($lines | Select-String -Pattern $oldStyle -SimpleMatch)
    Write-Output ('  old style line "' + $oldStyle + '" (what the removed code path wrote) in the dump: ' + $hits.Count + ' hit(s)')
    if ($hits.Count -ne 0) { Write-Output ('  -> FAIL ' + $hits[0].Line.Trim()); $fail++ }
  } else {
    # (b) the version file says it -> show it, and say which field it came from
    if ($base -ne $wantBase) { Write-Output ('  -> FAIL base="' + $base + '" (want ' + $wantBase + ')'); $fail++ }
    if ($from -notlike 'core:*') { Write-Output ('  -> FAIL baseFrom=' + $from + ' (want a core:* source)'); $fail++ }
    if ($ctx.Groups['rel'].Value -ne '1') { Write-Output '  -> FAIL coreReliable is not 1'; $fail++ }
    $wantText = $T_ORIGINAL + ' ' + $wantBase
    if ($text -notlike ('*' + $wantText + '*')) { Write-Output ('  -> FAIL the line does not show it: ' + $text); $fail++ }
    if ($dumpText -ne $null -and $dumpText -notlike ('*' + $wantText + '*')) { Write-Output ('  -> FAIL the widget dump line does not show it: ' + $dumpText); $fail++ }
    Write-Output ('  the line shows it and the trace names the source field: baseFrom=' + $from)
  }

  # (c) the fetch really happens on a worker thread
  $bg = Get-BgThread $lines
  if ($bg -eq $null) {
    Write-Output '  -> FAIL no bg-task[mods-instance-version] thread evidence line'
    $fail++
  } else {
    $worker = $bg.Groups['w'].Value; $uiThread = $bg.Groups['u'].Value; $same = $bg.Groups['same'].Value
    Write-Output ('  thread evidence: worker=' + $worker + ' ui=' + $uiThread + ' same=' + $same)
    if ($worker -eq $uiThread) { Write-Output '  -> FAIL the scan ran on the UI thread'; $fail++ }
    if ($same -ne (C @(0x5426))) { Write-Output ('  -> FAIL the verdict line does not say "not the same thread" (got ' + $same + ')'); $fail++ }
  }
}

Write-Output ''
Write-Output ('runs: ' + $cases + ' fixture(s); product files: ' + $work)
if ($fail -eq 0) {
  Write-Output 'MODS-VERSION: ALL OK (worker-thread scan, version file is the only source, nothing written when it cannot be known)'
  exit 0
}
Write-Output ('MODS-VERSION: FAIL (' + $fail + ' assertion(s))')
exit 1
