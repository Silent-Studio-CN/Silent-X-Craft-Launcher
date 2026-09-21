# run_android_args_fixture.ps1 - 离线验"安卓起游戏主类的参数装配"(不需要设备/真 JRE/不起 JVM)
#
# 它建一个独立小工程(android/app/tests),把核心库 sxcl 与我们的安卓装配层
# (android/app/sxcl_android_game_args.c)连起来,跑夹具断言并打印两份 argv 的原文。
# 用法: pwsh -File android/scripts/run_android_args_fixture.ps1
param(
  [string]$Repo = '',
  [string]$Build = 'D:\sxcl_local\args_fixture',
  [string]$Cmake = 'cmake'
)
$ErrorActionPreference = 'Continue'
if ([string]::IsNullOrWhiteSpace($Repo)) {
  $Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
Write-Output ('repo  = ' + $Repo)
Write-Output ('build = ' + $Build)
if (Test-Path $Build) { Remove-Item $Build -Recurse -Force }
& $Cmake -S (Join-Path $Repo 'android\app\tests') -B $Build -A x64 ('-DSXCL_REPO_ROOT=' + $Repo) -DCMAKE_PREFIX_PATH='D:/Qt/6.11.2/msvc2022_64' 2>&1 | Select-Object -Last 6
Write-Output ('configure rc=' + $LASTEXITCODE)
& $Cmake --build $Build --config Release -j 8 2>&1 | Select-Object -Last 8
Write-Output ('build rc=' + $LASTEXITCODE)
& ctest --test-dir $Build -C Release --output-on-failure 2>&1 | Select-Object -Last 60
Write-Output ('ctest rc=' + $LASTEXITCODE)
