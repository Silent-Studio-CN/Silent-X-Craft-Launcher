<#
  window_behavior_check.ps1 —— 窗口三键语义的**客观读数**验收(见 docs/13-窗口行为.md §6)

  为什么要有它:窗口行为光"看着像"不算数。这里所有判据都来自 Win32 本身 ——
    GetWindowLong(GWL_EXSTYLE) & WS_EX_TOPMOST   -> 真的置顶了吗
    IsWindowVisible(hwnd)                        -> 窗口真的从桌面上消失了吗
    GetWindowRect(hwnd)                          -> 小窗口/恢复的矩形前后数值
    GetProcessById(pid) 还在吗                    -> 关闭 = 进程真的没了
  这些读数是**独立于程序自述**取的:程序自己也会往 stderr 打一行同样口径的读数,
  两边对得上才算数(程序那行在日志里,前缀 [wincheck] / [sxcl-ui] win |)。

  用法:
    pwsh -File tools/window_behavior_check.ps1 -Exe <sxcl-ui.exe 路径> [-VersionId 1.21.11]

  每次只起**一个**窗口,一个场景跑完就退;三个场景串行。
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$VersionId = "1.21.11",
    [string]$Work = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) { throw "找不到 exe: $Exe" }
if ([string]::IsNullOrWhiteSpace($Work)) {
    $Work = Join-Path $env:TEMP ("sxcl_wincheck_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
}
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$SettingsFile = Join-Path $Work "settings.conf"
$GameDir      = Join-Path $Work "game"
New-Item -ItemType Directory -Force -Path $GameDir | Out-Null

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class W32 {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrW(IntPtr hWnd, int index);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public const int GWL_EXSTYLE = -20;
    public const long WS_EX_TOPMOST = 0x00000008L;
    public static IntPtr FindByPid(uint wanted) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != wanted) return true;
            var sb = new StringBuilder(512); GetWindowTextW(h, sb, 512);
            if (sb.ToString() == "Silent X Craft Launcher") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static string Describe(uint pid) {
        IntPtr h = FindByPid(pid);
        if (h == IntPtr.Zero) return "hwnd=(none)";
        RECT r; GetWindowRect(h, out r);
        long ex = GetWindowLongPtrW(h, GWL_EXSTYLE).ToInt64();
        return String.Format("hwnd=0x{0:X} isWindowVisible={1} WS_EX_TOPMOST={2} exstyle=0x{3:X} rect=({4},{5} {6}x{7})",
            h.ToInt64(), IsWindowVisible(h) ? 1 : 0,
            ((ex & WS_EX_TOPMOST) != 0) ? 1 : 0, ex,
            r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top);
    }
}
"@

function Invoke-Scenario {
    param([string]$Name, [string]$Script, [int]$TimeoutSec = 120)

    Write-Output ""
    Write-Output ("=" * 78)
    Write-Output ("场景 {0}: WINCHECK={1}" -f $Name, $Script)
    Write-Output ("=" * 78)

    $log = Join-Path $Work ("{0}.log" -f $Name)
    $env:SXCL_UI_WINCHECK   = $Script
    $env:SXCL_UI_SETTINGS   = $SettingsFile
    $env:SXCL_UI_GAME_DIR   = $GameDir
    $env:SXCL_UI_TRACE      = "1"
    $env:SXCL_UI_THEME      = "dark"

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardError = $true
    $psi.RedirectStandardOutput = $true
    $psi.WorkingDirectory = $Work
    $proc = [System.Diagnostics.Process]::Start($psi)
    $errTask = $proc.StandardError.ReadToEndAsync()
    $outTask = $proc.StandardOutput.ReadToEndAsync()

    # 轮询:300ms 一次,记录**独立于程序自述**的 Win32 读数;值变了才打一行
    $last = ""
    $samples = @()
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
        $d = [W32]::Describe([uint32]$proc.Id)
        if ($d -ne $last) {
            $samples += ("t={0,6}ms  {1}" -f [int]((Get-Date) - $proc.StartTime).TotalMilliseconds, $d)
            $last = $d
        }
        Start-Sleep -Milliseconds 300
    }
    $exited = $proc.HasExited
    if (-not $exited) { $proc.Kill() }
    try { $proc.WaitForExit(10000) | Out-Null } catch { }

    Write-Output "--- 独立读数(Win32,按变化打点)---"
    $samples | ForEach-Object { Write-Output ("  " + $_) }

    $alive = $null -ne (Get-Process -Id $proc.Id -ErrorAction SilentlyContinue)
    Write-Output ("--- 进程: exited={0} pidStillAlive={1} exitCode={2} ---" -f $exited, $alive, $(if ($exited) { $proc.ExitCode } else { "n/a" }))

    $err = ""
    try { $err = $errTask.GetAwaiter().GetResult() } catch { }
    try { $outTask.GetAwaiter().GetResult() | Out-Null } catch { }
    [System.IO.File]::WriteAllText($log, $err, (New-Object System.Text.UTF8Encoding($false)))

    Write-Output "--- 程序自述([wincheck] / win | 行)---"
    ($err -split "?
") | Where-Object { $_ -match "[wincheck]|win |" } | ForEach-Object { Write-Output ("  " + $_.TrimEnd()) }
    Write-Output ("--- 完整 stderr 日志: {0} ---" -f $log)
    return $err
}

# ── 场景 1:最大化 = 最大化 + 置顶;还原 = 取消置顶;关闭 = 进程消失 ──
$null = Invoke-Scenario -Name "01-max-topmost" -Script "probe;max;wait:1600;probe;normal;wait:1600;probe;quit"

# ── 场景 2:缩成小窗口 -> 恢复原尺寸与位置 ──
$null = Invoke-Scenario -Name "02-mini" -Script "probe;mini;wait:1400;probe;unmini;wait:1400;probe;quit"

# ── 场景 3:最小化 = 隐藏(进程活、后台任务继续跑)──
$null = Invoke-Scenario -Name "03-hide-worker" -TimeoutSec 180 -Script ("download:{0};wait:9000;probe;min;wait:5000;probe;wait:5000;probe;show;wait:1500;probe;quit" -f $VersionId)

Write-Output ""
Write-Output ("工作目录(设置文件 / 游戏目录 / 日志): {0}" -f $Work)
$pending = Join-Path (Split-Path $SettingsFile) "pending_tasks.json"
Write-Output ("任务状态文件: {0} exists={1}" -f $pending, (Test-Path $pending))
if (Test-Path $pending) { Get-Content -Raw $pending | Write-Output }