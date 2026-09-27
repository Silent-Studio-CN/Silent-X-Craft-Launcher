# (C) Silent X Craft Launcher -- 设置页"重排 + 谁都不许被挤压"验收(用户 2026-09-27)。
#
# 用户原话:「压缩问题现在非常严重:设置里"版本列表刷新频率 两分钟"的"中"字没了,"主题模式 深色"的
# "色"没了,弹窗消息"顶部居中"的"中"字没了 —— 这种字全都给我独立元素,谁都不能挤压它。」
# 「设置页从版本隔离往下的 Java 运行时、内置 JRE 全都错乱了,重新排版;记住了,就两行……
# 最左边是 logo 不是小图标;右侧下拉/按钮排好。」「内置 JRE 什么"已装组件：无"这个用你说吗?影响布局就移除。」
#
# 本脚本只**读**运行结果(截图 + 控件树 dump + SXCL_UI_TRACE 读数),四组断言:
#   1) 三档窗口(900x600 / 1100x750 / 1280x800)的 dump 里 **CUT-W / CUT-H = 0**
#      (含隐藏控件:隐藏项的几何也要干净,不许出现"假截断");
#   2) 三个点名的值**完整可见**:下拉框的文字区 >= 文本宽(trace 的 ok=1)
#      + 截图里那段文字的**墨迹宽度** >= 文案自然宽的 90%(字形真的画全了,不是被裁掉半个);
#   3) "版本隔离以下那一段"的每张两行卡:logo=28、可见文字行**恰好 2 行**、右侧控件右对齐,
#      卡里空着的地方**画了分隔线**(trace 的 分隔线>=1),控件行数宽窗口=1、最窄窗口<=2;
#   4) 全页没有"已装组件：无"这类"只为显示无"的信息行。
#
# 用法:pwsh -File tools/ui_settings_rows.ps1 [-Exe <sxcl-ui.exe>]
param([string]$Exe = '')

$ErrorActionPreference = 'Continue'
# 与其它验收脚本同一口径:先把可能影响几何的 SXCL_UI_* 清干净
foreach ($v in @('SXCL_UI_CONFIG', 'SXCL_UI_NAV', 'SXCL_UI_COLLAPSE', 'SXCL_UI_RAILS_TEST',
                 'SXCL_UI_EDITION', 'SXCL_UI_SCROLL', 'SXCL_UI_POPUP', 'SXCL_UI_AUTH_DIALOG',
                 'SXCL_UI_JRE_HOSTED', 'SXCL_UI_MODS_QUERY', 'SXCL_UI_LAUNCH', 'SXCL_UI_DOWNLOAD',
                 'SXCL_UI_ICON_POPUP', 'SXCL_UI_SCROLLBAR_STRESS', 'SXCL_UI_ACCENT_APPLY',
                 'SXCL_UI_THEME_SWITCH', 'SXCL_UI_ACCEPT', 'SXCL_UI_SETTINGS_ACCEPT',
                 'SXCL_UI_DUMP_DEPTH', 'SXCL_UI_SHOT_DELAY')) {
  Remove-Item ('Env:' + $v) -ErrorAction SilentlyContinue
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = if ($Exe -ne '') { $Exe } else { Join-Path $root 'build-ui\src\ui\Release\sxcl-ui.exe' }
$work = 'D:\SilentStudio\_test\settings_rows'
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $work | Out-Null
if (-not (Test-Path $exe)) { Write-Output ('SETTINGS-ROWS: FAIL (exe not found: ' + $exe + ')'); exit 1 }
$ini = Join-Path $work 'rows.ini'
Set-Content -Path $ini -Value 'game.default_dir=' -Encoding utf8

# 中文一律用码点拼(这份文件对编码免疫,与 tools/ui_settings_fit.ps1 同一套路)
function C([int[]]$cp) { -join ($cp | ForEach-Object { [char]$_ }) }
$vTwoMin    = '2' + (C @(0x5206, 0x949F))                     # 2分钟(用户点名的第一个值)
$vDark      = (C @(0x6DF1, 0x8272))                           # 深色
$vTopCenter = (C @(0x9876, 0x90E8, 0x5C45, 0x4E2D))           # 顶部居中
$noInstalled = (C @(0x5DF2, 0x88C5, 0x7EC4, 0x4EF6, 0xFF1A, 0x65E0))  # 已装组件：无
$rowSubtitle = 'rowSubtitle'
$rowControls = 'rowControls'
$sepKey     = (C @(0x5206, 0x9694, 0x7EBF))                   # 分隔线
$rowKey     = (C @(0x6587, 0x5B57, 0x884C))                   # 文字行
$ctrlRowKey = (C @(0x63A7, 0x4EF6, 0x884C))                   # 控件行
$logoKey    = 'logo='

$sizes = @('900x600', '1100x750', '1280x800')
$bad = @()
$cases = 0
$cutTotal = 0
$comboTotal = 0
$cardTotal = 0

Add-Type -AssemblyName System.Drawing

foreach ($size in $sizes) {
  $cases++
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_WINDOW = $size
  $env:SXCL_UI_ROUTE = 'settings'
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_DUMP_DEPTH = '12'
  $env:SXCL_UI_TRACE = '1'
  $shot = Join-Path $work ('shot_' + $size + '.png')
  $env:SXCL_UI_SHOT = $shot
  $env:SXCL_UI_SHOT_DELAY = '2600'
  $out = Join-Path $work ('dump_' + $size + '.txt')
  $err = $out + '.err'
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $txt = @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  if ($txt.Count -eq 0) { $bad += ('[' + $size + '] dump 是空的:程序没起来?'); continue }

  # ── 1) 不许有 CUT-W / CUT-H(含隐藏控件) ──────────────────────────────
  $cuts = @($txt | Select-String -Pattern 'CUT-' | ForEach-Object { $_.Line.Trim() })
  $cutTotal += $cuts.Count
  if ($cuts.Count -gt 0) {
    $bad += ('[' + $size + '] 有 ' + $cuts.Count + ' 个被裁的控件')
    $cuts | Select-Object -First 8 | ForEach-Object { $bad += ('    ' + $_) }
  }

  # ── 2) 点名的三个值:文字区够宽 + 截图里墨迹画全 ──────────────────────
  $comboLines = @($txt | Select-String -Pattern '^\[sxcl-ui\] combo-fit \|' | ForEach-Object { $_.Line })
  if ($comboLines.Count -eq 0) { $bad += ('[' + $size + '] 没有 combo-fit 读数(要 SXCL_UI_TRACE=1)') }
  foreach ($line in $comboLines) {
    $comboTotal++
    if ($line -notmatch 'ok=1') { $bad += ('[' + $size + '] 下拉框文字区不够: ' + $line) }
  }
  $cmp = $null
  if (Test-Path $shot) { $cmp = [System.Drawing.Bitmap]::FromFile($shot) }
  if ($cmp -eq $null) { $bad += ('[' + $size + '] 没有截图: ' + $shot) }
  $winW = 0
  foreach ($line in $txt) { if ($line -match '强制窗口尺寸 (\d+)x(\d+)') { $winW = [int]$Matches[1] } }
  $scale = if ($cmp -ne $null -and $winW -gt 0) { $cmp.Width / $winW } else { 0 }
  foreach ($want in @($vTwoMin, $vDark, $vTopCenter)) {
    $hit = $txt | Select-String -Pattern ('ComboBox \((-?\d+),(-?\d+) (\d+)x(\d+)\) "' + [regex]::Escape($want) + '" \[text=(\d+)x(\d+)') | Select-Object -First 1
    if (-not $hit) { $bad += ('[' + $size + '] 没找到下拉框的值: ' + $want); continue }
    $null = ($hit.Line -match 'ComboBox \((-?\d+),(-?\d+) (\d+)x(\d+)\) "' + [regex]::Escape($want) + '" \[text=(\d+)x(\d+)')
    $cx = [int]$Matches[1]; $cy = [int]$Matches[2]; $cw = [int]$Matches[3]; $ch = [int]$Matches[4]
    $textW = [int]$Matches[5]
    if ($cmp -eq $null -or $scale -le 0) { continue }
    # 只量"文字区":左边内缩 4px,右边留出箭头那块(20px),上下各内缩 3px
    $x0 = [int](($cx + 4) * $scale); $x1 = [int](($cx + $cw - 20) * $scale)
    $y0 = [int](($cy + 3) * $scale); $y1 = [int](($cy + $ch - 3) * $scale)
    $inkCols = 0
    for ($px = $x0; $px -lt $x1; $px++) {
      $lit = $false
      for ($py = $y0; $py -lt $y1 -and -not $lit; $py++) {
        $c = $cmp.GetPixel($px, $py)
        if (($c.R + $c.G + $c.B) -gt 300) { $lit = $true }
      }
      if ($lit) { $inkCols++ }
    }
    $need = [int]($textW * $scale * 0.9)
    Write-Output ('  [' + $size + '] "' + $want + '" 文字自然宽=' + $textW + 'px 墨迹=' + $inkCols + 'px(需要>=' + $need + ',缩放=' + $scale + ')')
    if ($inkCols -lt $need) { $bad += ('[' + $size + '] "' + $want + '" 少了字:墨迹 ' + $inkCols + 'px < ' + $need + 'px') }
  }
  if ($cmp -ne $null) { $cmp.Dispose() }

  # ── 2b) 版面不许溢出:没有可见的横向滚动条,每张卡的右缘都在视口里 ──────────────
  # (两条都与 tools/ui_settings_fit.ps1 同口径;这里合进同一趟 dump,免得再起一次进程)
  $vp = $txt | Select-String -Pattern 'QWidget #qt_scrollarea_viewport \((\d+),(\d+) (\d+)x(\d+)\)' | Select-Object -First 1
  if ($vp) {
    $null = ($vp.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
    $vRight = [int]$Matches[1] + [int]$Matches[3]
    $hbar = 0
    foreach ($sb in ($txt | Select-String -Pattern 'SmoothScrollBar \((\d+),(\d+) (\d+)x(\d+)\)')) {
      $null = ($sb.Line -match '\((\d+),(\d+) (\d+)x(\d+)\)')
      if ([int]$Matches[3] -gt [int]$Matches[4] -and $sb.Line -notmatch 'hidden') { $hbar++ }
    }
    $worstRight = 0
    foreach ($line in $txt) {
      if ($line -match '^\s+\S*SettingCard\S* \((\d+),(\d+) (\d+)x(\d+)\)') {
        $right = [int]$Matches[1] + [int]$Matches[3]
        if ($right -gt $worstRight) { $worstRight = $right }
      }
    }
    Write-Output ('  [' + $size + '] 视口右缘=' + $vRight + ' 最右卡片缘=' + $worstRight + ' 可见横向条=' + $hbar)
    if ($hbar -ne 0) { $bad += ('[' + $size + '] 设置页出现了可见的横向滚动条 ' + $hbar + ' 条') }
    if ($worstRight -gt $vRight) { $bad += ('[' + $size + '] 卡片右缘 ' + $worstRight + ' 超出视口右缘 ' + $vRight) }
  } else {
    $bad += ('[' + $size + '] 找不到视口行,量不了溢出')
  }

  # ── 3) 两行卡:每张恰好两行 + logo 28 + 有分隔线 + 控件排好 ──────────────
  $cardLines = @($txt | Select-String -Pattern '^\[sxcl-ui\] row-card \|' | ForEach-Object { $_.Line })
  $cardTotal += $cardLines.Count
  if ($cardLines.Count -ne 2) { $bad += ('[' + $size + '] 两行卡应有 2 张,实际 ' + $cardLines.Count + ' 张') }
  foreach ($line in $cardLines) {
    if ($line -notmatch ($rowKey + '=2')) { $bad += ('[' + $size + '] 卡片不是两行: ' + $line) }
    if ($line -notmatch ($logoKey + '28')) { $bad += ('[' + $size + '] logo 不是 28: ' + $line) }
    if ($line -notmatch ($sepKey + '=[1-9]')) { $bad += ('[' + $size + '] 卡里没画分隔线: ' + $line) }
    if ($line -notmatch ($ctrlRowKey + '=(\d+)')) { $bad += ('[' + $size + '] 控件行读数缺失: ' + $line); continue }
    $rows = [int]$Matches[1]
    if ($rows -lt 1) { $bad += ('[' + $size + '] 右侧控件没排出来: ' + $line) }
    if ($size -eq '900x600') { if ($rows -gt 2) { $bad += ('[' + $size + '] 最窄窗口控件超过两行: ' + $line) } }
    else { if ($rows -ne 1) { $bad += ('[' + $size + '] 宽窗口右侧控件应在一行内: ' + $line) } }
  }

  # ── 3b) dump 结构:每张两行卡里**可见的文字行恰好 2 行**(不许三行四行) ──
  for ($i = 0; $i -lt $txt.Count; $i++) {
    $line = $txt[$i]
    if ($line -notmatch ('#' + $rowSubtitle)) { continue }
    $indent = ([regex]::Match($line, '^\s*')).Value.Length
    $cardIndent = $indent - 2
    # 往回找这张卡的起始行(严格比子行浅一层缩进)
    $start = $i
    for ($j = $i - 1; $j -ge 0; $j--) {
      $ind = ([regex]::Match($txt[$j], '^\s*')).Value.Length
      if ($ind -lt $indent) { $start = $j; break }
      if ($ind -lt $cardIndent) { break }
    }
    $cardName = ($txt[$start].Trim() -split ' ')[0]
    # 卡片块 = 从卡行到下一个缩进更浅的行
    $labelRows = 0
    for ($j = $start + 1; $j -lt $txt.Count; $j++) {
      $ind = ([regex]::Match($txt[$j], '^\s*')).Value.Length
      if ($ind -le $cardIndent) { break }
      if ($ind -ne $indent) { continue }
      if ($txt[$j] -match ' hidden') { continue }
      if ($txt[$j] -match '(QLabel|FluentLabelBase|CaptionLabel)') { $labelRows++ }
    }
    if ($labelRows -ne 2) {
      $bad += ('[' + $size + '] ' + $cardName + ' 有 ' + $labelRows + ' 行可见文字(要求恰好 2 行:主标题 + 副标题)')
    }
  }

  # ── 4) 不许有"已装组件：无"这种只为显示"无"的信息行 ────────────────────
  $noise = @($txt | Select-String -Pattern ([regex]::Escape($noInstalled)) | ForEach-Object { $_.Line.Trim() })
  if ($noise.Count -gt 0) {
    $bad += ('[' + $size + '] 还有"已装组件：无"这类信息行 ' + $noise.Count + ' 处')
  }
  Write-Output ('  [' + $size + '] CUT=' + $cuts.Count + ' 下拉读数=' + $comboLines.Count + ' 两行卡=' + $cardLines.Count + ' 噪音行=' + $noise.Count)
}

# ── 5) 退出路径:设置页 -> 直接退出(用户 2026-09-27:绝不许被后台活按住)────────────
# 场景:进设置页 ~1ms 后就退出 —— 这时 Java 探测(java-detect,每个候选起一次进程)还在跑。
# 判据:
#   a) 每一处收尾等待都 <= 300ms(读数由 src/ui/workers/bg_task.cpp 自己打);
#   b) 这一趟**不生成新的 hang 报告**(看门狗阈值 3000ms —— 以前退出被按住 3104ms 就落了一份);
#   c) 退出被后台活拖住的净时间 <= 1000ms:用"立即退出"与"等探测跑完再退出"两趟的总时长反推
#      (两趟的启动开销相同,差值就是收尾多等的那部分)。
$crashes = Join-Path $env:APPDATA 'SilentXCraftLauncher\logs\crashes'
$hangBefore = if (Test-Path $crashes) { @(Get-ChildItem $crashes -Filter 'sxcl-ui-hang-*.txt' -ErrorAction SilentlyContinue).Count } else { 0 }
$wallImmediate = 0.0
$wallSettled = 0.0
$maxJoinWait = -1
$joinCount = 0
foreach ($spec in @(@{ tag = 'exit-immediately'; delay = 1 }, @{ tag = 'exit-after-probe'; delay = 2600 })) {
  $env:SXCL_UI_SETTINGS = $ini
  $env:SXCL_UI_WINDOW = '900x600'
  $env:SXCL_UI_ROUTE = 'settings'
  $env:SXCL_UI_THEME = 'dark'
  $env:SXCL_UI_DUMP = '1'
  $env:SXCL_UI_TRACE = '1'
  $env:SXCL_UI_SHOT = (Join-Path $work ('exit_' + $spec.tag + '.png'))
  $env:SXCL_UI_SHOT_DELAY = [string]$spec.delay
  $out = Join-Path $work ('exit_' + $spec.tag + '.txt')
  $err = $out + '.err'
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  Start-Process -FilePath $exe -RedirectStandardOutput $out -RedirectStandardError $err -Wait | Out-Null
  $sw.Stop()
  $seconds = [Math]::Round($sw.Elapsed.TotalSeconds, 3)
  if ($spec.tag -eq 'exit-immediately') { $wallImmediate = $seconds } else { $wallSettled = $seconds }
  $txt = @(Get-Content $err -Encoding utf8 -ErrorAction SilentlyContinue)
  $joins = @($txt | Select-String -Pattern 'bg-task: ' | ForEach-Object { $_.Line.Trim() } | Where-Object { $_ -match '析构等待' })
  $joinCount += $joins.Count
  foreach ($j in $joins) {
    if ($j -match '析构等待=(\d+)ms 上限=(\d+)ms') {
      $w = [int]$Matches[1]; $cap = [int]$Matches[2]
      if ($w -gt $maxJoinWait) { $maxJoinWait = $w }
      if ($cap -ne 300) { $bad += ('[' + $spec.tag + '] 收尾等待上限不是 300ms: ' + $j) }
      if ($w -gt 300) { $bad += ('[' + $spec.tag + '] 收尾等待超过 300ms: ' + $j) }
    }
  }
  Write-Output ('  [' + $spec.tag + '] 进程总时长=' + $seconds + 's 收尾读数=' + $joins.Count + ' 条')
}
$hangAfter = if (Test-Path $crashes) { @(Get-ChildItem $crashes -Filter 'sxcl-ui-hang-*.txt' -ErrorAction SilentlyContinue).Count } else { 0 }
$extraExitMs = [int](($wallImmediate - $wallSettled + 2.6) * 1000)
Write-Output ('  [退出路径] 立即退出=' + $wallImmediate + 's;等探测跑完再退=' + $wallSettled + 's -> 被后台活多拖=' + $extraExitMs + 'ms(上限 1000) 最大收尾等待=' + $maxJoinWait + 'ms(上限 300) hang 报告 ' + $hangBefore + ' -> ' + $hangAfter)
if ($hangAfter -gt $hangBefore) { $bad += ('[退出路径] 新增 hang 报告 ' + ($hangAfter - $hangBefore) + ' 份') }
if ($extraExitMs -gt 1000) { $bad += ('[退出路径] 退出被后台活多拖 ' + $extraExitMs + 'ms > 1000ms') }
if ($joinCount -eq 0) { $bad += '[退出路径] 没有收尾读数:立即退出那一趟本该在探测还在跑时收尾(bg-task 析构读数缺失)' }

if ($bad.Count -eq 0) {
  Write-Output ''
  Write-Output ('SETTINGS-ROWS: ALL OK (' + $cases + ' 档窗口:CUT-W/CUT-H=' + $cutTotal + ';下拉框读数 ' + $comboTotal + ' 条全部 ok=1;两行卡 ' + $cardTotal + ' 张全部 两行+logo28+有分隔线;退出路径:最大收尾等待 ' + $maxJoinWait + 'ms、hang 报告 ' + $hangBefore + '->' + $hangAfter + ')')
  exit 0
}
Write-Output ''
foreach ($b in $bad) { Write-Output ('  FAIL ' + $b) }
Write-Output ('SETTINGS-ROWS: FAIL (' + $bad.Count + ' 处)')
exit 1
