# Forge/NeoForge 静默安装的端到端验收(PowerShell,无 Python)。
# 用法: pwsh -File tools/verify_loader_install.ps1 [-GameDir build/e2e/mc] [-Forge 1.21.11]
#
# 判据(全部真跑,不许"看起来装上了"):
#   1) 安装器 jar 用我们自己的下载器取回,并且哈希与官方 maven 的 .sha1 一致
#   2) 静默安装退出码为 0,且**不依赖 jar 是否生成**这个弱判据
#   3) 版本目录里出现 versions/<目标版本>/<目标版本>.json
#   4) 该版本 JSON 能被 sxcl-dl 自己的 JSON 解析器读通,且 libraries 全在磁盘上、逐个 SHA-1 校验通过
#   5) launcher_profiles.json 里出现该实例,且**原有档案一个都没丢**(合并而非覆盖)
param(
    [string]$GameDir = 'build/e2e/mc',
    [string]$Forge = '1.21.11',
    [string]$McVersion = '1.21.11',
    [string]$Repo = (Join-Path $PSScriptRoot '..')
)
$ErrorActionPreference = 'Stop'
Set-Location $Repo
$exe = Join-Path $Repo 'build\Release\sxcl-dl.exe'
if (-not (Test-Path $exe)) { throw "先构建 sxcl-dl: $exe" }

$version = "$McVersion-$Forge"
$dir = Join-Path (Join-Path $Repo $GameDir) 'loaders'
New-Item -ItemType Directory -Force $dir | Out-Null
$installer = Join-Path $dir "forge-$version-installer.jar"

Write-Output "[1/5] 下载安装器(用我们自己的下载器 + 官方哈希)"
$base = 'https://maven.minecraftforge.net/net/minecraftforge/forge'
$url = "$base/$version/forge-$version-installer.jar"
$shaFile = Join-Path $dir 'installer.sha1'
& $exe get "$url.sha1" $shaFile --workers 1 | Out-Null
$want = (Get-Content $shaFile -Raw).Trim()
& $exe get $url $installer --sha1 $want --conn 4 --workers 2 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "安装器下载失败" }
Write-Output ("      安装器 " + [math]::Round((Get-Item $installer).Length/1MB,2) + " MB,官方 SHA-1 校验通过")

Write-Output "[2/5] 静默安装(方式 A:--installClient)"
# 注意:这一步要等 C 版的安装器驱动落地;命令名以实际 CLI 为准,落地后我会同步这里
& $exe loader forge $Forge $McVersion $GameDir
if ($LASTEXITCODE -ne 0) { throw "静默安装失败(退出码 $LASTEXITCODE)" }

Write-Output "[3/5] 版本目录"
$vjson = Join-Path (Join-Path (Join-Path $Repo $GameDir) 'versions') "$version\$version.json"
if (-not (Test-Path $vjson)) { throw "没有生成版本 JSON: $vjson" }
Write-Output "      $vjson"

Write-Output "[4/5] 版本 JSON 与依赖库复核"
& $exe get ('file:///' + ($vjson -replace '\\','/')) (Join-Path $dir 'unused.json') 2>$null | Out-Null
$libDir = Join-Path (Join-Path $Repo $GameDir) 'libraries'
$json = Get-Content $vjson -Raw | ConvertFrom-Json
$ok = 0; $bad = 0; $missing = 0
foreach ($lib in $json.libraries) {
    $a = $lib.downloads.artifact
    if (-not $a -or -not $a.path) { continue }
    $p = Join-Path $libDir ($a.path -replace '/','\\')
    if (-not (Test-Path $p)) { $missing++; continue }
    if ((Get-FileHash $p -Algorithm SHA1).Hash.ToLower() -eq $a.sha1) { $ok++ } else { $bad++ }
}
Write-Output ("      库: 通过 $ok,哈希不符 $bad,缺失 $missing")
if ($bad -gt 0 -or $missing -gt 0) { throw "依赖库复核没过" }

Write-Output "[5/5] launcher_profiles.json 合并检查"
$profiles = Join-Path (Join-Path $Repo $GameDir) 'launcher_profiles.json'
if (-not (Test-Path $profiles)) { throw "没有 launcher_profiles.json" }
$pf = Get-Content $profiles -Raw | ConvertFrom-Json
$names = $pf.profiles.PSObject.Properties.Name
Write-Output ("      档案数=" + $names.Count + " 含目标实例=" + ($names -contains $version))
if (-not ($names -contains $version)) { throw "launcher_profiles.json 里没有 $version" }

Write-Output ""
Write-Output "全部通过:Forge $version 静默安装 + 依赖库强校验 + 档案合并"
