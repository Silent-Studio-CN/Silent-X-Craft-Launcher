# build_apk.ps1 - SXCL C Android APK build, LOCAL fallback (this machine).
#
# THIS FILE IS TRACKED BY GIT (android/ is in the repository; build/ is not).
# It is the from-a-clone entry point: every packaging input it consumes comes
# from the repository (android/**) or from the SXCL source tree, never from a
# hand-maintained copy under build/. See docs/08-Android打包.md section 3.1.
#
# build/_android/scripts/build_local.ps1 is a thin forwarder to this script, so
# the older entry point keeps working.
#
# No VFS overlay here: the 'loader*' deletion quirk is a WS2025 host behaviour only.
# Everything lands on D: (C: has ~2GB free). ASCII only.
param(
  [string]$Stage = '',
  [string]$Local = 'D:\sxcl_local',
  [string]$QtAndroid = 'D:\Qt\6.11.2\android_arm64_v8a',
  [string]$QtHost    = 'D:\Qt\6.11.2\mingw_64',
  [string]$Ndk       = 'D:/AndroidSdk/ndk/28.2.13676358',
  [string]$Sdk       = 'D:/AndroidSdk',
  [string]$PyQf      = 'D:\SilentStudio\PyQf to C',
  [string]$Cmake     = 'D:\AndroidSdk\cmake\3.31.6\bin\cmake.exe',
  [string]$Ninja     = 'D:\AndroidSdk\cmake\3.31.6\bin\ninja.exe',
  [string]$JdkHome   = 'D:\jdk17',
  [string]$GradleHome = 'D:\gradle-home',
  [switch]$NoGradleOverlay
)
$ErrorActionPreference = 'Continue'
function L($s) { Write-Output ('[apk] ' + $s) }

$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ([string]::IsNullOrWhiteSpace($Stage)) { $Stage = Join-Path $Repo 'build\_android\stage' }
New-Item -ItemType Directory -Force -Path $Local | Out-Null

$BUILD = Join-Path $Local 'work'
$OUT   = Join-Path $Local 'out'

$env:JAVA_HOME        = $JdkHome
$env:ANDROID_HOME     = $Sdk
$env:ANDROID_SDK_ROOT = $Sdk
$env:GRADLE_USER_HOME = $GradleHome
$env:TEMP = 'D:\aqt-temp'; $env:TMP = 'D:\aqt-temp'
$env:Path = (Join-Path $JdkHome 'bin') + ';' + (Split-Path $Ninja) + ';' + $env:Path

L ('repo            : ' + $Repo)
L ('qt android kit  : ' + (Test-Path (Join-Path $QtAndroid 'lib\cmake\Qt6\qt.toolchain.cmake')))
L ('qt host kit     : ' + (Test-Path (Join-Path $QtHost 'bin\androiddeployqt.exe')))
L ('ndk             : ' + (Test-Path ($Ndk + '/build/cmake/android.toolchain.cmake')))
L ('jdk             : ' + (Test-Path (Join-Path $JdkHome 'bin\javac.exe')))
L ('libqf source    : ' + (Test-Path (Join-Path $PyQf 'CMakeLists.txt')))

# ---- 0. JRE-side libs (must exist BEFORE the sync step, which lists them in
#         deployment-settings.json android-extra-libs): libawt_xawt.so / libjsound.so
#         cross-compiled from the vendored sources under android/jni/jre-libs/. ---
& (Join-Path $PSScriptRoot 'build_jre_libs.ps1') -Repo $Repo -Ndk $Ndk
if ($LASTEXITCODE -ne 0) { L 'build_jre_libs FAILED'; exit 1 }

# ---- 1. packaging layer: repository android/ -> build/_android (gitignored) --
& (Join-Path $PSScriptRoot 'sync_to_build.ps1') -Repo $Repo -QtAndroid $QtAndroid -QtHost $QtHost -Ndk $Ndk -Sdk $Sdk
if ($LASTEXITCODE -ne 0) { L 'android sync FAILED'; exit 1 }

# ---- 2. refresh EVERYTHING the build consumes into the stage tree -----------
# (bug found on 2026-09-18: the stage tree kept a stale copy of src/**, so the APK
#  silently missed the paths.c / versions_page / home_page fixes - always re-sync)
robocopy (Join-Path $Repo 'src')     (Join-Path $Stage 'app\sxcl\src')     /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
robocopy (Join-Path $Repo 'include') (Join-Path $Stage 'app\sxcl\include') /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
Copy-Item (Join-Path $Repo 'CMakeLists.txt') (Join-Path $Stage 'app\sxcl\CMakeLists.txt') -Force
robocopy (Join-Path $PyQf 'src')       (Join-Path $Stage 'app\pytoc\src')       /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
robocopy (Join-Path $PyQf 'resources') (Join-Path $Stage 'app\pytoc\resources') /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
# the packaging layer itself (mirrored in step 1) - never re-sourced from build/
$Pkg = Join-Path $Repo 'build\_android\pkg'
Copy-Item (Join-Path $Pkg 'AndroidManifest.xml')       (Join-Path $Stage 'pkg\AndroidManifest.xml') -Force
Copy-Item (Join-Path $Pkg 'deployment-settings.json')  (Join-Path $Stage 'pkg\deployment-settings.json') -Force
robocopy (Join-Path $Pkg 'java') (Join-Path $Stage 'pkg\java') /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
robocopy (Join-Path $Pkg 'res')  (Join-Path $Stage 'pkg\res')  /E /NFL /NDL /NJH /NJS /NP | Out-Null
# EVERY file of the app packaging layer, not just the ones that existed first:
# sxcl_jre_probe.c (in-process JVM bootstrap probe) was added later and the old
# explicit Copy-Item pair silently left it behind -> "Cannot find source file" at
# cmake configure time. robocopy /E (no /PURGE: sxcl/ and pytoc/ live here too).
robocopy (Join-Path $Repo 'build\_android\app') (Join-Path $Stage 'app') /E /NFL /NDL /NJH /NJS /NP | Out-Null

# ---- stage the two derived trees the CMake project expects ----
robocopy (Join-Path $Stage 'app\sxcl\include') (Join-Path $Stage 'app\include') /E /NFL /NDL /NJH /NJS /NP | Out-Null
robocopy (Join-Path $Stage 'app\sxcl\assets')  (Join-Path $Stage 'pkg\assets')  /E /NFL /NDL /NJH /NJS /NP | Out-Null
L ('pkg assets      : ' + (Get-ChildItem (Join-Path $Stage 'pkg\assets') -Recurse -File).Count + ' files')

# ---- 3. native ----
if (Test-Path $BUILD) { Remove-Item $BUILD -Recurse -Force }
$cfg = @(
  '-S', (Join-Path $Stage 'app'),
  '-B', $BUILD,
  '-G', 'Ninja',
  ('-DCMAKE_TOOLCHAIN_FILE=' + ($QtAndroid -replace '\\','/') + '/lib/cmake/Qt6/qt.toolchain.cmake'),
  ('-DQT_CHAINLOAD_TOOLCHAIN_FILE=' + $Ndk + '/build/cmake/android.toolchain.cmake'),
  ('-DANDROID_NDK_ROOT=' + $Ndk),
  ('-DANDROID_SDK_ROOT=' + ($Sdk -replace '\\','/')),
  ('-DQT_HOST_PATH=' + ($QtHost -replace '\\','/')),
  '-DANDROID_ABI=arm64-v8a',
  '-DANDROID_PLATFORM=android-28',
  '-DCMAKE_BUILD_TYPE=Release',
  # cache-level: the desktop build gets AUTOMOC from the root CMakeLists, and the
  # src/ui target is added from OUR scope, so a plain set() is not enough here
  '-DCMAKE_AUTOMOC=ON',
  ('-DCMAKE_MAKE_PROGRAM=' + $Ninja)
)
& $Cmake @cfg
if ($LASTEXITCODE -ne 0) { L 'cmake configure FAILED'; exit 1 }
# sxclgame = :game 游戏进程的原生层(自己的 JVM 自举 + 与主进程的本地 socket 通道),
# 和 sxclui 一起编:两个 .so 缺一个游戏都起不来,不能只编一个就往下走。
& $Ninja -C $BUILD -j 10 sxclui sxclgame
if ($LASTEXITCODE -ne 0) { L 'ninja FAILED'; exit 2 }
$so = (Get-ChildItem $BUILD -Filter 'libsxclui*.so' | Select-Object -First 1).FullName
L ('app lib         : ' + $so + ' ' + (Get-Item $so).Length)
$gameSo = (Get-ChildItem $BUILD -Filter 'libsxclgame.so' | Select-Object -First 1).FullName
if (-not $gameSo) { L 'game lib MISSING: libsxclgame.so 没编出来(AndroidManifest 的 GameActivity 需要它)'; exit 5 }
L ('game lib        : ' + $gameSo + ' ' + (Get-Item $gameSo).Length)

# ---- 4. package ----
if (Test-Path $OUT) { Remove-Item $OUT -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $OUT 'libs\arm64-v8a') | Out-Null
Copy-Item $so (Join-Path $OUT 'libs\arm64-v8a\libsxclui_arm64-v8a.so') -Force
& (Join-Path $QtHost 'bin\androiddeployqt.exe') --input (Join-Path $Stage 'pkg\deployment-settings.json') --output $OUT
L ('androiddeployqt rc=' + $LASTEXITCODE)

# gradle project config: the repository's android/gradle/** wins over whatever
# androiddeployqt generated (it is the copy under version control).
if (-not $NoGradleOverlay) {
  $g = Join-Path $Repo 'build\_android\pkg\gradle'
  Copy-Item (Join-Path $g 'settings.gradle')           (Join-Path $OUT 'settings.gradle') -Force
  Copy-Item (Join-Path $g 'build.gradle')              (Join-Path $OUT 'build.gradle') -Force
  Copy-Item (Join-Path $g 'gradle.properties')         (Join-Path $OUT 'gradle.properties') -Force
  Copy-Item (Join-Path $g 'local.properties')          (Join-Path $OUT 'local.properties') -Force
  New-Item -ItemType Directory -Force -Path (Join-Path $OUT 'gradle\wrapper') | Out-Null
  Copy-Item (Join-Path $g 'gradle-wrapper.properties') (Join-Path $OUT 'gradle\wrapper\gradle-wrapper.properties') -Force
  L 'gradle config overlaid from android/gradle (tracked)'
}
robocopy (Join-Path $Stage 'pkg\assets') (Join-Path $OUT 'assets') /E /NFL /NDL /NJH /NJS /NP | Out-Null
L ('assets in gradle project: ' + (Get-ChildItem (Join-Path $OUT 'assets') -Recurse -File).Count)

# ---- TLS self-check -------------------------------------------------------
# Measured defect (2026-09-21): the APK contained libQt6Network but NO tls
# backend and no OpenSSL, so EVERY https request failed on the device
# ("network request failed" from both the official source and BMCLAPI).
# Three files have to end up in libs/arm64-v8a/ (= APK lib/arm64-v8a/).
# android-extra-libs in deployment-settings.json normally does the last two;
# this loop verifies it and copies them itself if it did not, so the APK can
# never silently ship without TLS again.
$tlsNeeded = @(
  'libplugins_tls_qopensslbackend_arm64-v8a.so',
  'libssl_3.so',
  'libcrypto_3.so')
$libDir = Join-Path $OUT 'libs\arm64-v8a'
New-Item -ItemType Directory -Force -Path $libDir | Out-Null
foreach ($n in $tlsNeeded) {
  $dst = Join-Path $libDir $n
  if (Test-Path $dst) {
    L ('tls ok        : ' + $n + '  ' + (Get-Item $dst).Length)
    continue
  }
  $srcQt  = Join-Path $QtAndroid ('plugins\tls\' + $n)
  $srcPre = Join-Path $Repo ('android\prebuilt\arm64-v8a\' + $n)
  if (Test-Path $srcQt) {
    Copy-Item $srcQt $dst -Force
    L ('tls FALLBACK  : copied from the Qt kit -> ' + $n)
  } elseif (Test-Path $srcPre) {
    Copy-Item $srcPre $dst -Force
    L ('tls FALLBACK  : copied from android/prebuilt -> ' + $n)
  } else {
    L ('tls MISSING   : ' + $n + ' (no source found) -- aborting, https would break silently')
    exit 3
  }
}

# JRE-side libs: same "must never silently ship without" rule as TLS above. The JVM
# dlopen()s both BY NAME from the installed JRE's lib dir, and the launcher copies them
# out of nativeLibraryDir, so a missing file means no JVM at all.
# :game 进程的原生层:和 TLS / JRE 侧 .so 同一条纪律 —— 缺了就 abort,绝不装作能起游戏。
# Java 侧是 System.loadLibrary("sxclgame") -> lib/<abi>/libsxclgame.so,少了它 GameActivity
# 只能报 "native-lib-missing" 然后收尾(正是本轮要避免的静默降级)。
$gameDst = Join-Path $libDir 'libsxclgame.so'
if (Test-Path $gameDst) { Remove-Item $gameDst -Force }
Copy-Item $gameSo $gameDst -Force
if (Test-Path $gameDst) {
  L ('game lib ok   : libsxclgame.so  ' + (Get-Item $gameDst).Length + '  (-> APK lib/arm64-v8a)')
} else {
  L 'game lib MISSING in the gradle project (libsxclgame.so) -- aborting'
  exit 5
}

foreach ($n in @('libawt_xawt.so', 'libjsound.so')) {
  $dst = Join-Path $libDir $n
  if (Test-Path $dst) {
    L ('jre lib ok    : ' + $n + '  ' + (Get-Item $dst).Length)
    continue
  }
  $srcJre = Join-Path $Repo ('build\_android\jre-libs\arm64-v8a\' + $n)
  if (Test-Path $srcJre) {
    Copy-Item $srcJre $dst -Force
    L ('jre lib FALLBACK: copied from build/_android/jre-libs -> ' + $n)
  } else {
    L ('jre lib MISSING   : ' + $n + ' (no source found) -- aborting, the JVM could not dlopen it')
    exit 4
  }
}

$gradle = (Get-ChildItem (Join-Path $GradleHome 'wrapper\dists') -Recurse -Filter 'gradle.bat' -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
L ('gradle          : ' + $gradle)
& $gradle --project-dir $OUT assembleDebug
L ('gradle rc=' + $LASTEXITCODE)
Get-ChildItem (Join-Path $OUT 'build\outputs\apk') -Recurse -Filter *.apk -ErrorAction SilentlyContinue | ForEach-Object { L ('APK ' + $_.FullName + ' ' + $_.Length) }
L 'DONE'
