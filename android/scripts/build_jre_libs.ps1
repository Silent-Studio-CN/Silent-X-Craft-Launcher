# (C) Silent X Craft Launcher
# Copyright by SilentStudio.
# All rights reserved.
#
# build_jre_libs.ps1 - cross-compile the two native libraries our launcher must
# inject into an installed JRE before a JVM can start. This is FCL's
# RuntimeUtils.patchJava() semantics (FCL/src/main/java/com/tungsten/fcl/util/RuntimeUtils.java:239-249):
#   <jre>/lib/libawt_xawt.so   <- from nativeLibraryDir  (HARD: source must exist)
#   <jre>/lib/libjsound.so     <- from nativeLibraryDir  (only if the source exists)
# jre8 keeps its layout under <jre>/jre/lib instead of <jre>/lib - the runtime side
# handles that, this script only produces the two .so files.
#
# Why these two are missing from the JRE assets at all: the Android arm64 JRE bundles
# (jre8/17/21/25) ship without libawt_xawt.so, and jre17/21/25 also without
# libjsound.so. The JVM dlopen()s both BY NAME out of <jre>/lib, so a missing file is
# a hard start-up failure - it cannot be downloaded at runtime.
#
# Sources live in android/jni/jre-libs/ and are vendored from Fold Craft Launcher;
# see android/jni/jre-libs/README.md for provenance and license (GPL-2.0 with the
# Classpath exception, compatible with this project's AGPL-3.0). Copyright headers of
# those files are kept verbatim - do not strip them.
#
# Output (gitignored build tree, consumed by sync_to_build.ps1 -> android-extra-libs):
#   <Repo>/build/_android/jre-libs/<Abi>/libawt_xawt.so
#   <Repo>/build/_android/jre-libs/<Abi>/libjsound.so
#
# ASCII only.
param(
  [string]$Repo = '',
  [string]$Ndk  = 'D:/AndroidSdk/ndk/28.2.13676358',
  [string]$Out  = '',
  [string]$Abi  = 'arm64-v8a',
  [string]$Api  = '28'
)
$ErrorActionPreference = 'Continue'
function L($s) { Write-Output ('[jrelibs] ' + $s) }

if ([string]::IsNullOrWhiteSpace($Repo)) {
  $Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
$Src = Join-Path $Repo 'android\jni\jre-libs'
if ([string]::IsNullOrWhiteSpace($Out)) { $Out = Join-Path $Repo ('build\_android\jre-libs\' + $Abi) }

$toolchain = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64'
$clang     = Join-Path $toolchain 'bin\clang.exe'
$sysInc    = Join-Path $toolchain 'sysroot\usr\include'
foreach ($p in @($clang, $sysInc)) {
  if (-not (Test-Path $p)) { L ('MISSING toolchain piece: ' + $p); exit 1 }
}
# minSdk of the APK is 28 (android/scripts/sync_to_build.ps1 -MinSdk); the .so must not
# require a newer libc than the app itself declares.
$target = 'aarch64-linux-android' + $Api

New-Item -ItemType Directory -Force -Path $Out | Out-Null

function Compile-So([string]$name, [string[]]$srcs, [string[]]$extra) {
  $out = Join-Path $Out ($name + '.so')
  if (Test-Path $out) { Remove-Item $out -Force }
  # NOTE: the comma operator binds TIGHTER than '+' in PowerShell, so the target has
  # to be concatenated in its own variable - otherwise the whole option list becomes
  # one giant --target value (clang: "version ... in target triple ... is invalid").
  $targetArg = '--target=' + $target
  $argv = @($targetArg, '-shared', '-fPIC', '-O2', '-Wall',
            '-o', $out, '-I', $sysInc) + $extra + $srcs
  & $clang @argv
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $out)) {
    L ('COMPILE FAILED: ' + $name)
    return $false
  }
  L ('built ' + $name + '.so  ' + (Get-Item $out).Length + ' bytes  -> ' + $out)
  return $true
}

# libawt_xawt.so: the AWT/X11 stub jre8..25 need (a headless-safe no-op implementation).
$ok1 = Compile-So 'libawt_xawt' @((Join-Path $Src 'awt_xawt\xawt_fake.c')) @()

# libjsound.so: OpenJDK javax.sound native core + the OpenAL platform backend.
# Compile definitions are the ones FCL uses (FCL/src/main/jni/CMakeLists.txt:236-245):
# PORT/MIDI subsystems are compiled out (stub returns 0 devices), audio goes through
# jsound_openal.c, and jdk8_compat.c supplies the extra JDK8 Platform natives.
$jsound = Join-Path $Src 'jsound'
$jsoundSrcs = @(
  'Utilities.c', 'Platform.c', 'DirectAudioDevice.c', 'DirectAudioDeviceProvider.c',
  'PortMixer.c', 'PortMixerProvider.c', 'MidiOutDevice.c', 'MidiOutDeviceProvider.c',
  'MidiInDevice.c', 'MidiInDeviceProvider.c', 'PlatformMidi.c', 'jsound_openal.c',
  'jdk8_compat.c') | ForEach-Object { Join-Path $jsound $_ }
$defs = @('-I', $jsound,
          '-DX_PLATFORM=X_LINUX', '-D_LITTLE_ENDIAN',
          '-DUSE_DAUDIO=TRUE', '-DUSE_PORTS=FALSE',
          '-DUSE_PLATFORM_MIDI_OUT=FALSE', '-DUSE_PLATFORM_MIDI_IN=FALSE',
          '-llog')
$ok2 = Compile-So 'libjsound' $jsoundSrcs $defs

if (-not ($ok1 -and $ok2)) { L 'JRE LIBS BUILD FAILED'; exit 2 }
L ('JRE LIBS OK -> ' + $Out)
exit 0
