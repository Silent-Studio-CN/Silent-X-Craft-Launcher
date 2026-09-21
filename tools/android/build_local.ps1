# SXCL C - Android APK build, LOCAL fallback (this machine).
# No VFS overlay here: the 'loader*' deletion quirk is a WS2025 host behaviour only.
# Everything lands on D: (C: has ~2GB free). ASCII only.
param([string]$Stage = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C\build\_android\stage',
      [string]$Local = 'D:\sxcl_local')
$ErrorActionPreference = 'Continue'
function L($s) { Write-Output ('[local] ' + $s) }
New-Item -ItemType Directory -Force -Path $Local | Out-Null

$NDK        = 'D:/AndroidSdk/ndk/28.2.13676358'
$QT_ANDROID = 'D:\Qt\6.11.2\android_arm64_v8a'
$QT_HOST    = 'D:\Qt\6.11.2\mingw_64'
$CMAKE      = 'D:\AndroidSdk\cmake\3.31.6\bin\cmake.exe'
$NINJA      = 'D:\AndroidSdk\cmake\3.31.6\bin\ninja.exe'
$BUILD      = Join-Path $Local 'work'
$OUT        = Join-Path $Local 'out'

$env:JAVA_HOME        = 'D:\jdk17'
$env:ANDROID_HOME     = 'D:\AndroidSdk'
$env:ANDROID_SDK_ROOT = 'D:\AndroidSdk'
$env:GRADLE_USER_HOME = 'D:\gradle-home'
$env:TEMP = 'D:\aqt-temp'; $env:TMP = 'D:\aqt-temp'
$env:Path = 'D:\jdk17\bin;' + (Split-Path $NINJA) + ';' + $env:Path

L ('qt android kit : ' + (Test-Path (Join-Path $QT_ANDROID 'lib\cmake\Qt6\qt.toolchain.cmake')))
L ('qt host kit    : ' + (Test-Path (Join-Path $QT_HOST 'bin\androiddeployqt.exe')))
L ('ndk             : ' + (Test-Path ($NDK + '/build/cmake/android.toolchain.cmake')))
L ('jdk             : ' + (Test-Path 'D:\jdk17\bin\javac.exe'))

# ---- refresh EVERYTHING the build consumes into the stage tree ----
# (bug found on 2026-09-18: the stage tree kept a stale copy of src/**, so the APK
#  silently missed the paths.c / versions_page / home_page fixes - always re-sync)
$Repo = 'D:\SilentStudio\prog\Silent-X-Craft-Launcher - C'
$PyQf = 'D:\SilentStudio\PyQf to C'
robocopy (Join-Path $Repo 'src')     (Join-Path $Stage 'app\sxcl\src')     /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
robocopy (Join-Path $Repo 'include') (Join-Path $Stage 'app\sxcl\include') /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
Copy-Item (Join-Path $Repo 'CMakeLists.txt') (Join-Path $Stage 'app\sxcl\CMakeLists.txt') -Force
robocopy (Join-Path $PyQf 'src')       (Join-Path $Stage 'app\pytoc\src')       /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
robocopy (Join-Path $PyQf 'resources') (Join-Path $Stage 'app\pytoc\resources') /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
Copy-Item (Join-Path $PSScriptRoot '..\app\CMakeLists.txt') (Join-Path $Stage 'app\CMakeLists.txt') -Force
Copy-Item (Join-Path $PSScriptRoot '..\app\sxcl_android_main.cpp') (Join-Path $Stage 'app\sxcl_android_main.cpp') -Force
Copy-Item (Join-Path $PSScriptRoot '..\pkg\AndroidManifest.xml') (Join-Path $Stage 'pkg\AndroidManifest.xml') -Force
robocopy (Join-Path $PSScriptRoot '..\pkg\java') (Join-Path $Stage 'pkg\java') /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
robocopy (Join-Path $PSScriptRoot '..\pkg\res')  (Join-Path $Stage 'pkg\res')  /E /NFL /NDL /NJH /NJS /NP | Out-Null

# ---- stage the two derived trees the CMake project expects ----
robocopy (Join-Path $Stage 'app\sxcl\include') (Join-Path $Stage 'app\include') /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $Stage 'app\sxcl\assets')  (Join-Path $Stage 'pkg\assets')  /E /NFL /NDL /NJH /NJS /NP | Out-Null
L ('pkg assets      : ' + (Get-ChildItem (Join-Path $Stage 'pkg\assets') -Recurse -File).Count + ' files')

# ---- native ----
if (Test-Path $BUILD) { Remove-Item $BUILD -Recurse -Force }
$cfg = @(
  '-S', (Join-Path $Stage 'app'),
  '-B', $BUILD,
  '-G', 'Ninja',
  ('-DCMAKE_TOOLCHAIN_FILE=' + ($QT_ANDROID -replace '\\','/') + '/lib/cmake/Qt6/qt.toolchain.cmake'),
  ('-DQT_CHAINLOAD_TOOLCHAIN_FILE=' + $NDK + '/build/cmake/android.toolchain.cmake'),
  ('-DANDROID_NDK_ROOT=' + $NDK),
  '-DANDROID_SDK_ROOT=D:/AndroidSdk',
  ('-DQT_HOST_PATH=' + ($QT_HOST -replace '\\','/')),
  '-DANDROID_ABI=arm64-v8a',
  '-DANDROID_PLATFORM=android-28',
  '-DCMAKE_BUILD_TYPE=Release',
  # cache-level: the desktop build gets AUTOMOC from the root CMakeLists, and the
  # src/ui target is added from OUR scope, so a plain set() is not enough here
  '-DCMAKE_AUTOMOC=ON',
  ('-DCMAKE_MAKE_PROGRAM=' + $NINJA)
)
& $CMAKE @cfg
if ($LASTEXITCODE -ne 0) { L 'cmake configure FAILED'; exit 1 }
& $NINJA -C $BUILD -j 10 sxclui
if ($LASTEXITCODE -ne 0) { L 'ninja FAILED'; exit 2 }
$so = (Get-ChildItem $BUILD -Filter 'libsxclui*.so' | Select-Object -First 1).FullName
L ('app lib         : ' + $so + ' ' + (Get-Item $so).Length)

# ---- package ----
if (Test-Path $OUT) { Remove-Item $OUT -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $OUT 'libs\arm64-v8a') | Out-Null
Copy-Item $so (Join-Path $OUT 'libs\arm64-v8a\libsxclui_arm64-v8a.so') -Force
$ds = (Get-Content (Join-Path $Stage 'pkg\deployment-settings.json') -Raw)
L ('deployment settings present: ' + ($ds.Length -gt 0))
& (Join-Path $QT_HOST 'bin\androiddeployqt.exe') --input (Join-Path $Stage 'pkg\deployment-settings.json') --output $OUT
L ('androiddeployqt rc=' + $LASTEXITCODE)
if (-not (Test-Path (Join-Path $OUT 'settings.gradle'))) {
  $sg = "pluginManagement {" + [Environment]::NewLine + "    repositories { google(); mavenCentral(); gradlePluginPortal() }" + [Environment]::NewLine + "}" + [Environment]::NewLine + "rootProject.name = 'sxcl'" + [Environment]::NewLine
  Set-Content -Path (Join-Path $OUT 'settings.gradle') -Value $sg -Encoding ASCII
}
robocopy (Join-Path $Stage 'pkg\assets') (Join-Path $OUT 'assets') /E /NFL /NDL /NJH /NJS /NP | Out-Null
L ('assets in gradle project: ' + (Get-ChildItem (Join-Path $OUT 'assets') -Recurse -File).Count)

$gradle = (Get-ChildItem 'D:\gradle-home\wrapper\dists' -Recurse -Filter 'gradle.bat' -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
L ('gradle          : ' + $gradle)
& $gradle --project-dir $OUT assembleDebug
L ('gradle rc=' + $LASTEXITCODE)
Get-ChildItem (Join-Path $OUT 'build\outputs\apk') -Recurse -Filter *.apk -ErrorAction SilentlyContinue | ForEach-Object { L ('APK ' + $_.FullName + ' ' + $_.Length) }
L 'DONE'