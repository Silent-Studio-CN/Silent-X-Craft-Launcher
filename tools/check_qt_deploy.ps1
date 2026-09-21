<#
(C) Silent X Craft Launcher
Copyright by SilentStudio.
All rights reserved.
#>
# check_qt_deploy.ps1 —— Qt 运行时"摆放"自检(每次构建后跑,默认必须跑)
#
# 用户原话:"新增没问题,问题是找不到要注意目录摆放" —— 加新模块从来不是问题,
# 问题是 exe 旁边**该在的文件不在**:要么压根没拷,要么拷到了别的目录。
# 两类最难查的:
#   1) DLL 在、插件不在       -> qt.network.ssl: No functional TLS backend was found(HTTPS 全废)
#   2) windeployqt 不铺离屏    -> -platform offscreen 直接 "Could not find the Qt platform plugin"
#
# 做法:对给定的 exe(或 exe 所在目录里的每个 exe)断言"运行时真正需要的文件就在
# exe 同目录/子目录"。缺任何一个 -> 非零退出,并逐条打印"缺哪个文件、应该在哪个目录"。
#
# 判定依据(三路互补):
#   A. windeployqt --list relative <exe>  —— 权威清单(与部署时用同一套参数)
#   B. exe/同目录 Qt6*.dll 的 PE 导入表    —— 直接依赖的每个 Qt6*.dll(不依赖任何外部工具)
#   C. 插件目录规则                        —— Gui 应用必须有 platforms/qwindows.dll +
#                                             platforms/qoffscreen.dll + styles/ + imageformats/;
#                                             Network 应用必须有 tls/ 下的后端插件
# B/C 保证 windeployqt 缺失或不可用时自检依然**有效**,不会退化成"什么都不查"。
#
# 用法:
#   pwsh -NoProfile -File tools/check_qt_deploy.ps1 <exe 目录 或 exe 路径>
#   pwsh -NoProfile -File tools/check_qt_deploy.ps1 D:\repo\build-ui\src\ui\Release
#   pwsh -NoProfile -File tools/check_qt_deploy.ps1 D:\repo\build\Release\sxcl-dl.exe
#   pwsh -NoProfile -File tools/check_qt_deploy.ps1 <目标> -Windeployqt <windeployqt.exe 路径>
#   pwsh -NoProfile -File tools/check_qt_deploy.ps1 <目标> -ShowImports    # 只打印导入表(取证用)
#
# 退出码:0 = 全部就位;1 = 有缺失;2 = 用法/路径错误
# 跳过:$env:SXCL_SKIP_DEPLOY_CHECK=1(默认必须跑;跳过只留给极特殊场景)

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Exe,

    [string]$Windeployqt = '',

    # 必须与部署时的 windeployqt 参数一致,否则会要求"本来就不打算铺"的文件
    [string]$WindeployqtArgs = '--no-translations --no-opengl-sw --no-system-d3d-compiler --no-compiler-runtime',

    [switch]$ShowImports
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$tag = '[deploy-check]'

if ($env:SXCL_SKIP_DEPLOY_CHECK -eq '1') {
    Write-Host ($tag + ' SXCL_SKIP_DEPLOY_CHECK=1 -> 跳过 Qt 部署摆放自检(默认必须跑)')
    exit 0
}

# ────────────────────────── PE 导入表 ──────────────────────────
# 纯 PowerShell 解析(不用 dumpbin / 任何外部依赖):MZ -> PE 头 -> 可选头数据目录[1] -> 导入描述符
function Get-PeImportedDllNames {
    param([Parameter(Mandatory = $true)][string]$Path)

    # 读文件要重试:链接刚结束、杀软扫描、另一个构建/测试在同目录跑,都会短暂独占 exe
    $bytes = $null
    for ($attempt = 1; $attempt -le 6; $attempt++) {
        try { $bytes = [System.IO.File]::ReadAllBytes($Path); break }
        catch {
            if ($attempt -eq 6) {
                throw ('读不了这个 exe(被别的进程占用?链接/扫描/并发构建都可能):' + $Path + ' -> ' + $_.Exception.Message)
            }
            Start-Sleep -Milliseconds 300
        }
    }
    if ($bytes.Length -lt 0x40) { throw ('不是有效的 PE 文件(太小):' + $Path) }
    if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw ('不是有效的 PE 文件(没有 MZ):' + $Path) }

    $peOff = [System.BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOff -le 0 -or ($peOff + 24) -ge $bytes.Length) { throw ('PE 头偏移越界:' + $Path) }
    if ([System.BitConverter]::ToUInt32($bytes, $peOff) -ne [uint32]0x00004550) {
        throw ('不是有效的 PE 文件(没有 PE 签名):' + $Path)
    }

    $coff = $peOff + 4
    $numSections = [System.BitConverter]::ToUInt16($bytes, $coff + 2)
    $optSize = [System.BitConverter]::ToUInt16($bytes, $coff + 16)
    $opt = $coff + 20
    $magic = [System.BitConverter]::ToUInt16($bytes, $opt)
    if ($magic -eq 0x20B) { $ddOff = $opt + 112 }        # PE32+
    elseif ($magic -eq 0x10B) { $ddOff = $opt + 96 }     # PE32
    else { throw ('PE 可选头 magic 不认识:' + ('0x{0:X}' -f $magic)) }

    $importRva = [System.BitConverter]::ToUInt32($bytes, $ddOff + 8)
    if ($importRva -eq 0) { return @() }

    $secOff = $opt + $optSize
    $sections = @()
    for ($i = 0; $i -lt $numSections; $i++) {
        $s = $secOff + 40 * $i
        if (($s + 40) -gt $bytes.Length) { break }
        $sections += [pscustomobject]@{
            VA     = [System.BitConverter]::ToUInt32($bytes, $s + 12)
            VSize  = [System.BitConverter]::ToUInt32($bytes, $s + 8)
            RawOff = [System.BitConverter]::ToUInt32($bytes, $s + 20)
        }
    }

    function Convert-RvaToOffset {
        param([uint32]$Rva)
        foreach ($sec in $sections) {
            $span = [Math]::Max([int]$sec.VSize, 4096)
            if ($Rva -ge $sec.VA -and $Rva -lt ($sec.VA + $span)) {
                return [int]($sec.RawOff + ($Rva - $sec.VA))
            }
        }
        return -1
    }

    function Read-AsciiZ {
        param([int]$Offset)
        if ($Offset -lt 0 -or $Offset -ge $bytes.Length) { return '' }
        $sb = New-Object System.Text.StringBuilder
        $i = $Offset
        while ($i -lt $bytes.Length -and $bytes[$i] -ne 0) {
            [void]$sb.Append([char]$bytes[$i])
            $i++
        }
        return $sb.ToString()
    }

    $names = @()
    $desc = Convert-RvaToOffset -Rva $importRva
    while ($desc -gt 0 -and ($desc + 20) -le $bytes.Length) {
        $nameRva = [System.BitConverter]::ToUInt32($bytes, $desc + 12)
        $firstThunk = [System.BitConverter]::ToUInt32($bytes, $desc)
        if ($nameRva -eq 0 -and $firstThunk -eq 0) { break }
        $n = Read-AsciiZ -Offset (Convert-RvaToOffset -Rva $nameRva)
        if ($n) { $names += $n }
        $desc += 20
    }
    return $names
}

# ────────────────────────── windeployqt 定位 ──────────────────────────
function Find-Windeployqt {
    param([string]$Explicit, [string]$ExeDir)

    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit -PathType Leaf) { return (Resolve-Path -LiteralPath $Explicit).Path }
        Write-Host ($tag + ' 警告:指定的 windeployqt 不存在:' + $Explicit)
    }
    if ($env:SXCL_WINDEPLOYQT -and (Test-Path -LiteralPath $env:SXCL_WINDEPLOYQT -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $env:SXCL_WINDEPLOYQT).Path
    }
    $cmd = Get-Command windeployqt -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    # 单跑时最常见的来源:从 exe 目录往上找 CMakeCache.txt 的 Qt6_DIR -> <qt>/bin/windeployqt.exe
    $dir = $ExeDir
    while ($dir) {
        $cache = Join-Path $dir 'CMakeCache.txt'
        if (Test-Path -LiteralPath $cache -PathType Leaf) {
            $line = Select-String -LiteralPath $cache -Pattern '^Qt6_DIR:PATH=' | Select-Object -First 1
            if ($line) {
                $qtDir = $line.Line.Substring('Qt6_DIR:PATH='.Length).Trim()
                $qtRoot = [System.IO.Path]::GetFullPath((Join-Path $qtDir '..\..\..'))
                $cand = Join-Path $qtRoot 'bin\windeployqt.exe'
                if (Test-Path -LiteralPath $cand -PathType Leaf) { return $cand }
            }
        }
        $parent = Split-Path -Path $dir -Parent
        if (-not $parent -or $parent -eq $dir) { break }
        $dir = $parent
    }
    return ''
}

# ────────────────────────── 目标解析 ──────────────────────────
$targetFull = $Exe
if (-not (Test-Path -LiteralPath $targetFull)) { $targetFull = Join-Path (Get-Location).Path $Exe }
if (-not (Test-Path -LiteralPath $targetFull)) {
    Write-Host ($tag + ' 用法错误:找不到目标 -> ' + $Exe)
    exit 2
}
$targetItem = Get-Item -LiteralPath $targetFull
if ($targetItem.PSIsContainer) {
    $exeFiles = @(Get-ChildItem -LiteralPath $targetItem.FullName -Filter '*.exe' -File | Sort-Object Name)
    if ($exeFiles.Count -eq 0) {
        Write-Host ($tag + ' 用法错误:目录里没有 exe -> ' + $targetItem.FullName)
        exit 2
    }
}
else {
    if ([System.IO.Path]::GetExtension($targetItem.Name) -ne '.exe') {
        Write-Host ($tag + ' 用法错误:目标既不是 exe 也不是目录 -> ' + $targetItem.FullName)
        exit 2
    }
    $exeFiles = @($targetItem)
}

$exeDirs = @($exeFiles | ForEach-Object { $_.DirectoryName } | Sort-Object -Unique)
$wdqPath = Find-Windeployqt -Explicit $Windeployqt -ExeDir $exeDirs[0]
$wdqArgs = @()
foreach ($a in ($WindeployqtArgs -split '[\s;]+')) { if ($a) { $wdqArgs += $a } }

Write-Host ($tag + ' 目标目录:' + ($exeDirs -join ' ; '))
Write-Host ($tag + ' exe:' + $exeFiles.Count + ' 个 -> ' + (($exeFiles | ForEach-Object { $_.Name }) -join ', '))
if ($wdqPath) {
    $wdqVersion = ''
    try { $wdqVersion = (& $wdqPath -version 2>$null | Select-Object -First 1) } catch { $wdqVersion = '' }
    Write-Host ($tag + ' 权威清单:windeployqt --list relative (' + $wdqPath + ' ' + $wdqVersion + ')')
}
else {
    Write-Host ($tag + ' 权威清单:windeployqt 不可用 -> 只用 PE 导入表 + 插件目录规则判定(自检依然有效)')
}

$failures = New-Object System.Collections.ArrayList
$reqCount = 0
$okCount = 0
$warnCount = 0
$unreadable = 0
$wdqScratch = ''          # windeployqt 没有 --dry-run 时的临时落点(用完删)
# 两阶段:先把"该有什么"全部收集完(这一阶段会调用 windeployqt),
# 再统一断言存在性 —— 任何工具副作用都不可能发生在断言之前。
$allChecks = New-Object System.Collections.ArrayList
$allRules = New-Object System.Collections.ArrayList

foreach ($exeFile in $exeFiles) {
    $exeDir = $exeFile.DirectoryName

    Write-Host ''
    Write-Host ($tag + ' === ' + $exeFile.Name + ' ===')

    $imports = @()
    $importErr = ''
    try { $imports = @(Get-PeImportedDllNames -Path $exeFile.FullName) }
    catch { $importErr = $_.Exception.Message }
    if ($importErr) {
        $unreadable++
        Write-Host ($tag + '   读取失败,跳过断言(这是环境问题,不是部署问题):' + $importErr)
        continue
    }

    $qtImports = @($imports | Where-Object { $_ -like 'Qt6*.dll' })
    $qtModules = @($qtImports | ForEach-Object { $_.Substring(3, $_.Length - 7) } | Sort-Object)
    Write-Host ($tag + '   直接 import 的 Qt 模块:' + (($qtModules | ForEach-Object { 'Qt6' + $_ }) -join ', '))

    if ($ShowImports) {
        Write-Host ($tag + '   全部导入 DLL(' + $imports.Count + '):' + ($imports -join ', '))
        continue
    }

    $reqs = New-Object System.Collections.ArrayList

    # B. exe 直接 import 的每个 Qt6*.dll 必须在 exe 同目录(Windows 先查 exe 目录,其余靠 PATH = 不稳)
    foreach ($dll in $qtImports) {
        [void]$reqs.Add([pscustomobject]@{
                Kind     = 'exe 直接依赖'
                Rel      = $dll
                FullPath = (Join-Path $exeDir $dll)
                Why      = 'exe 的 PE 导入表里有它,Windows 只在 exe 同目录找 -> 不靠 PATH'
            })
    }

    # B2. 同目录里已铺的 Qt6*.dll,它自己 import 的 Qt6*.dll 也必须在同目录(漏一个就是链断)
    $localQtDlls = @(Get-ChildItem -LiteralPath $exeDir -Filter 'Qt6*.dll' -File -ErrorAction SilentlyContinue)
    foreach ($dllFile in $localQtDlls) {
        $sub = @()
        try { $sub = @(Get-PeImportedDllNames -Path $dllFile.FullName | Where-Object { $_ -like 'Qt6*.dll' }) } catch { $sub = @() }
        foreach ($dep in $sub) {
            [void]$reqs.Add([pscustomobject]@{
                    Kind     = 'Qt DLL 之间的依赖'
                    Rel      = $dep
                    FullPath = (Join-Path $exeDir $dep)
                    Why      = ($dllFile.Name + ' 依赖它 -> 必须同目录,否则加载到一半失败')
                })
        }
    }

    # A. windeployqt 权威清单里的每一项(相对路径按 exe 目录解析)
    if ($wdqPath -and $qtImports.Count -eq 0) {
        Write-Host ($tag + '   非 Qt 程序(导入表里没有 Qt6*.dll)-> 不需要 Qt 运行时,跳过 windeployqt')
    }
    elseif ($wdqPath) {
        # **必须 --dry-run**:实测 windeployqt 的 --list 不是"只看不写" —— 它会真的把文件铺出去!
        # 不 dry-run 的话,自检会先把"缺的文件"悄悄补好,再断言"都在位" -> 永远测不出问题。
        # (反向验证时踩到:移走 tls/qschannelbackend.dll,自检却次次通过,文件还被铺了回来。)
        # 老版本 windeployqt 没有 --dry-run:退回把 --dir 指到临时目录,让副作用落在那里。
        $listOut = @()
        $listErr = ''
        $rc = 0
        $listErrFile = [System.IO.Path]::GetTempFileName()
        try {
            $listOut = @(& $wdqPath @wdqArgs --dry-run --list relative $exeFile.FullName 2>$listErrFile)
            $rc = $LASTEXITCODE
            $listErr = (Get-Content -LiteralPath $listErrFile -Raw -ErrorAction SilentlyContinue)
            $items = @($listOut | ForEach-Object { $_.Trim() } | Where-Object { $_ })
            if ($rc -ne 0 -or $items.Count -eq 0) {
                if (-not $wdqScratch) {
                    $wdqScratch = Join-Path ([System.IO.Path]::GetTempPath()) ('sxcl-deploy-check-' + $PID)
                    New-Item -ItemType Directory -Path $wdqScratch -Force | Out-Null
                }
                $listOut = @(& $wdqPath @wdqArgs --dir $wdqScratch --list relative $exeFile.FullName 2>$listErrFile)
                $rc = $LASTEXITCODE
                $listErr = (Get-Content -LiteralPath $listErrFile -Raw -ErrorAction SilentlyContinue)
            }
        }
        catch {
            $rc = 1
            $listErr = $_.Exception.Message
        }
        Remove-Item -LiteralPath $listErrFile -Force -ErrorAction SilentlyContinue
        $items = @($listOut | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        if ($rc -ne 0 -or $items.Count -eq 0) {
            $warnCount++
            Write-Host ($tag + '   警告:windeployqt --list 没给出清单(rc=' + $rc + ';非 Qt 程序可忽略)')
            if ($listErr) { Write-Host ($tag + '   原因:' + (($listErr.Trim() -split "\r?\n") | Select-Object -First 1)) }
        }
        else {
            Write-Host ($tag + '   windeployqt 权威清单:' + $items.Count + ' 项')
            foreach ($it in $items) {
                $p = $it -replace '/', '\'
                $full = $p
                if (-not [System.IO.Path]::IsPathRooted($p)) { $full = Join-Path $exeDir $p }
                [void]$reqs.Add([pscustomobject]@{
                        Kind     = 'windeployqt 清单'
                        Rel      = $p
                        FullPath = $full
                        Why      = 'windeployqt --list relative 说要铺它(与部署同一套参数)'
                    })
            }
        }
    }

    # C. 插件目录规则(不依赖 windeployqt;按这个 exe 真正 import 的 Qt 模块决定要求)
    if ($qtImports -contains 'Qt6Gui.dll') {
        $platformWhy = 'Windows 上起窗的最低要求:没有它连窗口都建不出来'
        $offscreenWhy = '我们额外铺的离屏插件:windeployqt 不铺,-platform offscreen(测试/截图)会报 Could not find the Qt platform plugin'
        foreach ($rel in @('platforms\qwindows.dll', 'platforms\qoffscreen.dll')) {
            $why = $platformWhy
            if ($rel -like '*qoffscreen*') { $why = $offscreenWhy }
            [void]$reqs.Add([pscustomobject]@{
                    Kind     = '插件规则(Gui)'
                    Rel      = $rel
                    FullPath = (Join-Path $exeDir $rel)
                    Why      = $why
                })
        }
        $guiRules = @(
            @{ Dir = 'styles'; Why = 'Qt Widgets 的样式插件(qmodernwindowsstyle):缺了界面退化成 Windows 经典外观' },
            @{ Dir = 'imageformats'; Why = '图片格式插件(qico/qjpeg/qsvg 等):缺了图标/图片直接不显示' }
        )
        foreach ($rule in $guiRules) {
            [void]$allRules.Add([pscustomobject]@{
                    Exe  = $exeFile.Name
                    Glob = (Join-Path $exeDir ($rule.Dir + '\*.dll'))
                    Was  = $exeDir + '\' + $rule.Dir + '\'
                    What = '目录 ' + $rule.Dir + '/ 下的任意一个 .dll'
                    Why  = $rule.Why
                    Kind = '插件规则(Gui)'
                })
        }
    }
    if ($qtImports -contains 'Qt6Multimedia.dll') {
        # 我们当前**不链** Qt6Multimedia(见 docs/07 §5:libqf 的 PYQFTO_MULTIMEDIA 被关掉)。
        # 这条规则是给"哪天开了媒体模块"准备的:fluent_demo 那类目标一旦链上多媒体,
        # Qt6Multimedia(.dll) 与 multimedia/ 后端插件(ffmpegmediaplugin 等,背后是 FFmpeg)
        # 必须同时与 exe 同目录 —— 正是最容易漏的"目录摆放"。
        [void]$allRules.Add([pscustomobject]@{
                Exe  = $exeFile.Name
                Glob = (Join-Path $exeDir 'multimedia\*.dll')
                Was  = $exeDir + '\multimedia\'
                What = '目录 multimedia/ 下的后端插件(如 ffmpegmediaplugin.dll / windowsmediaplugin.dll)'
                Why  = 'exe import 了 Qt6Multimedia:缺后端插件就是"能链不能播",而且多媒体插件还会拖进 FFmpeg 运行库'
                Kind = '插件规则(Multimedia)'
            })
    }
    if ($qtImports -contains 'Qt6Network.dll') {
        [void]$allRules.Add([pscustomobject]@{
                Exe  = $exeFile.Name
                Glob = (Join-Path $exeDir 'tls\*.dll')
                Was  = $exeDir + '\tls\'
                What = '目录 tls/ 下的 TLS 后端插件(如 qschannelbackend.dll / qopensslbackend.dll)'
                Why  = 'HTTPS 全靠它:缺了就报 No functional TLS backend was found -> 下载全废'
                Kind = '插件规则(Network)'
            })
    }

    foreach ($r in $reqs) {
        [void]$allChecks.Add([pscustomobject]@{ Exe = $exeFile.Name; Expect = $r.FullPath; Why = $r.Why; Kind = $r.Kind })
    }
}

# ── 第二阶段:断言(所有 windeployqt 调用都已结束) ──
foreach ($c in $allChecks) {
    $reqCount++
    if (Test-Path -LiteralPath $c.Expect -PathType Leaf) { $okCount++; continue }
    [void]$failures.Add($c)
}
foreach ($rule in $allRules) {
    $reqCount++
    $hit = @(Get-ChildItem -Path $rule.Glob -File -ErrorAction SilentlyContinue)
    if ($hit.Count -gt 0) { $okCount++; continue }
    [void]$failures.Add([pscustomobject]@{
            Exe = $rule.Exe; Expect = ('缺:' + $rule.Was + '(' + $rule.What + ')'); Why = $rule.Why; Kind = $rule.Kind
        })
}
if ($wdqScratch -and (Test-Path -LiteralPath $wdqScratch)) {
    Remove-Item -LiteralPath $wdqScratch -Recurse -Force -ErrorAction SilentlyContinue
}

if ($ShowImports) {
    Write-Host ''
    Write-Host ($tag + ' 只打印导入表(-ShowImports),未做摆放断言')
    exit 0
}

Write-Host ''
if ($failures.Count -eq 0) {
    $tail = ''
    if ($warnCount -gt 0) { $tail = '(' + $warnCount + ' 条 windeployqt 警告)' }
    Write-Host ($tag + ' 通过:' + $exeFiles.Count + ' 个 exe / ' + $reqCount + ' 项断言全部在位' + $tail)
    if ($unreadable -gt 0) {
        Write-Host ($tag + ' 但 ' + $unreadable + ' 个 exe 读不了(被占用),这次没能断言它们 —— 重跑一次再下结论')
        exit 2
    }
    exit 0
}

Write-Host ($tag + ' 失败:缺 ' + $failures.Count + ' 项(共断言 ' + $reqCount + ' 项)—— 这就是"找不到 DLL/插件"的直接原因')
Write-Host ''
foreach ($f in $failures) {
    Write-Host ($tag + '   缺:' + $f.Expect)
    Write-Host ($tag + '       哪个 exe:' + $f.Exe + '(' + $f.Kind + ')')
    Write-Host ($tag + '       为什么:' + $f.Why)
}
Write-Host ''
Write-Host ($tag + ' 结论:部署目录摆放不正确 -> 干净机器上必然起不来(或 HTTPS 全废)。')
Write-Host ($tag + ' 修法:重新构建让部署函数重新铺(sxcl_deploy_qt/sxcl_deploy_qt_full),不要手工拷。')
exit 1
