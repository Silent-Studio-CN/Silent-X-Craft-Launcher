# sync_to_build.ps1 - the Android packaging layer has ONE source of truth: the
# repository directory android/ (tracked by git). This script mirrors it into the
# gitignored build/_android/{pkg,app} trees the build scripts consume, and
# materialises the files that must not carry machine paths in git
# (pkg/deployment-settings.json and pkg/gradle/gradle.properties).
#
# Why: build/ is in .gitignore, so an open-source clone could never rebuild the
# APK from a manifest/java/res that only lived there. See docs/08-Android打包.md
# section 3.1. ASCII only.
param(
  [string]$Repo      = '',
  [string]$QtAndroid = 'D:\Qt\6.11.2\android_arm64_v8a',
  [string]$QtHost    = 'D:\Qt\6.11.2\mingw_64',
  [string]$Ndk       = 'D:/AndroidSdk/ndk/28.2.13676358',
  [string]$Sdk       = 'D:/AndroidSdk',
  [string]$BuildTools    = '34.0.0',
  [string]$CompileSdk    = '36',
  [string]$MinSdk        = '28',
  [string]$TargetSdk     = '34',
  [string]$PackageName   = 'com.silentstudio.sxcl',
  [string]$VersionCode   = '1',
  [string]$VersionName   = '0.2.0-android',
  [string]$AbiList       = 'arm64-v8a'
)
$ErrorActionPreference = 'Continue'
function L($s) { Write-Output ('[android-sync] ' + $s) }

if ([string]::IsNullOrWhiteSpace($Repo)) {
  $Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$And = Join-Path $Repo 'android'
$Pkg = Join-Path $Repo 'build\_android\pkg'
$App = Join-Path $Repo 'build\_android\app'

if (-not (Test-Path (Join-Path $And 'AndroidManifest.xml'))) {
  L ('MISSING repository packaging layer: ' + $And)
  exit 1
}

# ---- 1. mirror the tracked tree (manifest / java / res / icon) --------------
New-Item -ItemType Directory -Force -Path $Pkg, $App | Out-Null
Copy-Item (Join-Path $And 'AndroidManifest.xml') (Join-Path $Pkg 'AndroidManifest.xml') -Force
if (Test-Path (Join-Path $And 'icon_512.png')) {
  Copy-Item (Join-Path $And 'icon_512.png') (Join-Path $Pkg 'icon_512.png') -Force
}
robocopy (Join-Path $And 'java') (Join-Path $Pkg 'java') /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
if ($LASTEXITCODE -ge 8) { L ('robocopy java FAILED rc=' + $LASTEXITCODE); exit 1 }
robocopy (Join-Path $And 'res')  (Join-Path $Pkg 'res')  /E /NFL /NDL /NJH /NJS /NP /PURGE | Out-Null
if ($LASTEXITCODE -ge 8) { L ('robocopy res FAILED rc=' + $LASTEXITCODE); exit 1 }
robocopy (Join-Path $And 'app')  $App                    /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { L ('robocopy app FAILED rc=' + $LASTEXITCODE); exit 1 }
L ('mirrored ' + $And + ' -> ' + $Pkg + ' , ' + $App)

# ---- 2. deployment-settings.json (androiddeployqt input) -------------------
# The template keeps placeholders so no build machine's absolute path is in git.
$dsTpl = Get-Content (Join-Path $And 'deployment-settings.template.json') -Raw
$qtAndroidFwd = $QtAndroid -replace '\\','/'
$qtHostFwd    = $QtHost    -replace '\\','/'
$pkgFwd       = $Pkg       -replace '\\','/'
# keep only dependency files that really exist in this Qt kit (a kit without a
# plugin must not fail the whole APK build) - same filter the WS2025 stage used
$cand = @(
  'jar/Qt6Android.jar',
  'lib/libQt6Core_arm64-v8a.so','lib/libQt6Gui_arm64-v8a.so','lib/libQt6Widgets_arm64-v8a.so',
  'lib/libQt6Svg_arm64-v8a.so','lib/libQt6Network_arm64-v8a.so','lib/libQt6OpenGL_arm64-v8a.so',
  'lib/libQt6OpenGLWidgets_arm64-v8a.so','lib/libQt6PrintSupport_arm64-v8a.so','lib/libQt6Sql_arm64-v8a.so',
  'lib/libQt6Concurrent_arm64-v8a.so',
  'plugins/platforms/libplugins_platforms_qtforandroid_arm64-v8a.so',
  'plugins/platforms/libplugins_platforms_qoffscreen_arm64-v8a.so',
  'plugins/styles/libplugins_styles_qandroidstyle_arm64-v8a.so',
  # TLS: WITHOUT this plugin Android has NO https backend at all (measured on the
  # device: the APK had libQt6Network but no tls plugin, so every https request
  # failed). The plugin loads libssl_3.so / libcrypto_3.so at runtime via dlopen,
  # and those two are NOT in the Android NDK, hence android-extra-libs below.
  'plugins/tls/libplugins_tls_qopensslbackend_arm64-v8a.so',
  'plugins/imageformats/libplugins_imageformats_qsvg_arm64-v8a.so',
  'plugins/imageformats/libplugins_imageformats_qjpeg_arm64-v8a.so',
  'plugins/imageformats/libplugins_imageformats_qgif_arm64-v8a.so',
  'plugins/imageformats/libplugins_imageformats_qico_arm64-v8a.so'
)
$keep = @()
foreach ($c in $cand) {
  $p = Join-Path $QtAndroid ($c -replace '/','\')
  if (Test-Path $p) { $keep += $c } else { L ('not in kit, dropped from dependencies: ' + $c) }
}
$ds = $dsTpl
$ds = $ds -replace '@COMPILE_SDK@', $CompileSdk
$ds = $ds -replace '@MIN_SDK@', $MinSdk
$ds = $ds -replace '@TARGET_SDK@', $TargetSdk
$ds = $ds -replace '@PACKAGE_NAME@', $PackageName
$ds = $ds -replace '@VERSION_CODE@', $VersionCode
$ds = $ds -replace '@VERSION_NAME@', $VersionName
$ds = $ds -replace '@BUILD_PKG@', $pkgFwd
$ds = $ds -replace '@NDK@', $Ndk
$ds = $ds -replace '@QT_HOST@', $qtHostFwd
$ds = $ds -replace '@QT_ANDROID@', $qtAndroidFwd
$ds = $ds -replace '@SDK@', $Sdk
$ds = $ds -replace '@BUILD_TOOLS@', $BuildTools
$ds = $ds -replace '@DEPLOYMENT_DEPENDENCIES@', ([string]::Join(',', $keep))
# OpenSSL 3 for Android (libssl_3.so / libcrypto_3.so) is NOT in the NDK and NOT in the
# Qt kit: it is the prebuilt pair committed under android/prebuilt/arm64-v8a/.
# Order matters: libcrypto BEFORE libssl (androiddeployqt's docs: a library listed
# before its dependencies fails to load on some devices).
$prebuilt = (Join-Path $And 'prebuilt\arm64-v8a') -replace '\\','/'
$extraLibs = @(($prebuilt + '/libcrypto_3.so'), ($prebuilt + '/libssl_3.so'))
# JRE-side libs (libawt_xawt.so / libjsound.so) are NOT in the JRE assets and NOT in
# the NDK: android/scripts/build_jre_libs.ps1 cross-compiles them from the vendored
# sources under android/jni/jre-libs/ into the gitignored build tree. They have to end
# up in the APK's lib/<abi>/ = nativeLibraryDir, because the launcher copies them from
# there into the installed JRE (FCL RuntimeUtils.patchJava semantics).
$jreLibDirFs = Join-Path $Repo 'build\_android\jre-libs\arm64-v8a'
$jreLibDir   = $jreLibDirFs -replace '\\','/'
foreach ($n in @('libawt_xawt.so', 'libjsound.so')) {
  if (Test-Path (Join-Path $jreLibDirFs $n)) {
    $extraLibs += ($jreLibDir + '/' + $n)
  } else {
    L ('JRE lib NOT BUILT, not packaged: ' + $n + '  (run android/scripts/build_jre_libs.ps1)')
  }
}
$ds = $ds -replace '@ANDROID_EXTRA_LIBS@', ([string]::Join(',', $extraLibs))
Set-Content -Path (Join-Path $Pkg 'deployment-settings.json') -Value $ds -Encoding ASCII -NoNewline
L ('deployment-settings.json written (deps=' + $keep.Count + ')')

# ---- 3. gradle config (androiddeployqt needs a project it never generates) --
$gradleDir = Join-Path $Pkg 'gradle'
New-Item -ItemType Directory -Force -Path $gradleDir | Out-Null
Copy-Item (Join-Path $And 'gradle\settings.gradle')           (Join-Path $gradleDir 'settings.gradle') -Force
Copy-Item (Join-Path $And 'gradle\build.gradle')              (Join-Path $gradleDir 'build.gradle') -Force
Copy-Item (Join-Path $And 'gradle\gradle-wrapper.properties') (Join-Path $gradleDir 'gradle-wrapper.properties') -Force
$gp = Get-Content (Join-Path $And 'gradle\gradle.properties.template') -Raw
$gp = $gp -replace '@EXTRA_PROPERTIES@', ''
$gp = $gp -replace '@BUILD_TOOLS@', $BuildTools
$gp = $gp -replace '@COMPILE_SDK@', $CompileSdk
$gp = $gp -replace '@NDK_VERSION@', (Split-Path $Ndk -Leaf)
$gp = $gp -replace '@PACKAGE_NAME@', $PackageName
$gp = $gp -replace '@QT_ANDROID@', $qtAndroidFwd
$gp = $gp -replace '@MIN_SDK@', $MinSdk
$gp = $gp -replace '@TARGET_SDK@', $TargetSdk
$gp = $gp -replace '@ABI_LIST@', $AbiList
Set-Content -Path (Join-Path $gradleDir 'gradle.properties') -Value $gp -Encoding ASCII -NoNewline
Set-Content -Path (Join-Path $gradleDir 'local.properties') -Value ('sdk.dir=' + $Sdk + [Environment]::NewLine) -Encoding ASCII -NoNewline
L ('gradle config written to ' + $gradleDir)

# ---- 4. self-report so a build log proves what was mirrored ----------------
foreach ($f in @('AndroidManifest.xml','deployment-settings.json')) {
  $p = Join-Path $Pkg $f
  if (Test-Path $p) { L ('  ' + $f + '  ' + (Get-FileHash $p -Algorithm SHA256).Hash.Substring(0,16)) }
}
$jx = Join-Path $Pkg 'java\com\silentstudio\sxcl\SxclActivity.java'
if (Test-Path $jx) { L ('  java\com\silentstudio\sxcl\SxclActivity.java  ' + (Get-FileHash $jx -Algorithm SHA256).Hash.Substring(0,16)) }
L 'ANDROID SYNC OK'
exit 0
