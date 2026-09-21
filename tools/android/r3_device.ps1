# Device side: install the APK, take a visible-run screenshot, then render the
# nine acceptance routes at 1100x750 @1.5 (offscreen QPA) and pull them back.
# Runs on THIS machine (the devices are on the local LAN). ASCII only.
param([string]$Apk = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\_android\out\sxclui-debug.apk')
$ErrorActionPreference = 'Continue'
function L($s) { Write-Output ('[dev] ' + $s) }

$adb = 'D:\AndroidSdk\platform-tools\adb.exe'
$dev = '192.168.200.183:5555'
$pkg = 'com.silentstudio.sxcl'
$act = 'com.silentstudio.sxcl/com.silentstudio.sxcl.SxclActivity'
$outDir = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\_android\out\device'
$tmp = 'C:\sxcl_local'
New-Item -ItemType Directory -Force -Path $outDir, $tmp | Out-Null

$model = (& $adb -s $dev shell getprop ro.product.model) -join ''
$sdk   = (& $adb -s $dev shell getprop ro.build.version.sdk) -join ''
$abi   = (& $adb -s $dev shell getprop ro.product.cpu.abi) -join ''
L ('device model=' + $model + ' sdk=' + $sdk + ' abi=' + $abi)
L ('apk bytes=' + (Get-Item $Apk -ErrorAction SilentlyContinue).Length)

L '== install =='
& $adb -s $dev install -r $Apk
L ('install rc=' + $LASTEXITCODE)

L '== visible run =='
& $adb -s $dev shell am force-stop $pkg
& $adb -s $dev shell am start -n $act
Start-Sleep -Seconds 12
& $adb -s $dev shell 'dumpsys window | grep -i mCurrentFocus' | ForEach-Object { L ('  focus ' + $_) }
cmd /c "$adb -s $dev exec-out screencap -p > C:\sxcl_local\visible_home.png"
Move-Item 'C:\sxcl_local\visible_home.png' "$outDir\visible_home.png" -Force
L ('visible shot bytes=' + (Get-Item "$outDir\visible_home.png" -ErrorAction SilentlyContinue).Length)
& $adb -s $dev logcat -d -s sxcl:* > "$outDir\logcat_visible.txt"
& $adb -s $dev logcat -d -s AndroidRuntime:E > "$outDir\logcat_crash.txt"

$routes = @('home','versions','tasks','keymap','multiplayer','settings','download_config','download_progress','launch')
foreach ($r in $routes) {
  L ('== route ' + $r + ' ==')
  & $adb -s $dev shell am force-stop $pkg
  Start-Sleep -Milliseconds 800
  $remote = '/sdcard/Android/data/' + $pkg + '/files/shots/' + $r + '.png'
  & $adb -s $dev shell ('rm -f ' + $remote)
  & $adb -s $dev shell am start -n $act --es route $r --ez offscreen true --ez shot true --es scale 1.5 --es accent '#c044a3' | Out-Null
  $ok = $false
  for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 1500
    $sz = (& $adb -s $dev shell ('stat -c %s ' + $remote + ' 2>/dev/null')) -join ''
    if ($sz -match '^\d+$' -and [int]$sz -gt 1000) { $ok = $true; break }
    $alive = (& $adb -s $dev shell ('pidof ' + $pkg)) -join ''
    if ($alive -eq '' -and $i -gt 3) { break }
  }
  if ($ok) {
    & $adb -s $dev pull $remote "$outDir\$r.png"
    L ('  pulled ' + $r + '.png bytes=' + (Get-Item "$outDir\$r.png").Length)
  } else {
    L ('  NO SHOT for ' + $r)
    & $adb -s $dev logcat -d -s sxcl:* | Select-Object -Last 25 | ForEach-Object { L ('  log ' + $_) }
  }
}
L '== device run finished =='