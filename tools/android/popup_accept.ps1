# Popup acceptance on device: locate the popup, drag inside it, tap an item.
param([string]$Dev = '192.168.220.33:5555', [int]$PopupIdx = 2, [int]$Tall = 60)
$ErrorActionPreference = 'Continue'
$adb = 'D:\AndroidSdk\platform-tools\adb.exe'
$pkg = 'com.silentstudio.sxcl'
$act = $pkg + '/' + $pkg + '.SxclActivity'
$out = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\_android\out\device'
$tmp = 'C:\sxcl_local'
function Focus { ((& $adb -s $Dev shell dumpsys window 2>&1 | Select-String 'mCurrentFocus' | Select-Object -First 1).Line) }
function Shot($p) { cmd /c "$adb -s $Dev exec-out screencap -p > $tmp\p.png"; Move-Item "$tmp\p.png" $p -Force }
function Launch($open) {
  & $adb -s $Dev shell am force-stop $pkg | Out-Null; Start-Sleep -Milliseconds 700
  $a = @('shell','am','start','-n',$act,'--es','route','settings','--es','tallmenu',"$Tall")
  if ($open) { $a += @('--es','popup',"$PopupIdx") }
  & $adb -s $Dev @a | Out-Null
  Start-Sleep -Milliseconds 3200
}
Write-Output '=== reference frame: same page, popup CLOSED ==='
Launch $false
if ((Focus) -notlike ('*' + $pkg + '*')) { Write-Output '  focus lost'; exit }
Shot ($out + '\popup_tall_closed.png')
Write-Output '=== popup OPEN (tall menu) ==='
& $adb -s $Dev logcat -c | Out-Null
Launch $true
if ((Focus) -notlike ('*' + $pkg + '*')) { Write-Output '  focus lost'; exit }
Shot ($out + '\popup_tall_open.png')
& $adb -s $Dev logcat -d -s sxcl:* 2>&1 | Select-String -Pattern 'opening popup' | Select-Object -Last 2 | ForEach-Object { Write-Output ('  log ' + ($_ -replace '.*sxcl *: ','')) }
Write-Output 'POPUPPART1 DONE'
