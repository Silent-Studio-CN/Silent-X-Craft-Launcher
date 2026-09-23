/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 启动器自己的崩溃取证（声明与理由见 crash_handler.h）。

#define _CRT_SECURE_NO_WARNINGS 1

#include "crash_handler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <thread>

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QTimer>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include "sxcl/log.h"

#if defined(_WIN32)

#include <windows.h>

#include <dbghelp.h>

#pragma comment(lib, "Dbghelp.lib")

namespace {

/** 崩溃目录：与运行日志同一个 logs/ 下面（它本来就存在，别再造第二棵树）。 */
QString crashDir() {
    const QString logPath = QString::fromUtf8(sxcl_log_file_path());
    if (logPath.isEmpty())
        return QString();
    QDir dir = QFileInfo(logPath).absoluteDir();
    if (!dir.exists(QStringLiteral("crashes")) &&
        !dir.mkpath(QStringLiteral("crashes")))
        return QString();
    return dir.filePath(QStringLiteral("crashes"));
}

QString stamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

/** 异常码 -> 人话（只列我们真会撞上的那几个，其余打十六进制）。 */
const char *codeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "访问违例(ACCESS_VIOLATION)";
    case EXCEPTION_STACK_OVERFLOW:        return "栈溢出(STACK_OVERFLOW)";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "整数除零";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "非法指令";
    case EXCEPTION_IN_PAGE_ERROR:         return "内存页错误";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "数组越界";
    case EXCEPTION_PRIV_INSTRUCTION:      return "特权指令";
    case 0xE06D7363:                      return "未捕获的 C++ 异常(MSVC EH)";
    case 0xC0000409:                      return "栈缓冲区检查失败(/GS,常见于栈被写坏)";
    case 0xC0000374:                      return "堆损坏(HEAP_CORRUPTION)";
    case 0xC000041D:                      return "回调里未处理异常";
    default:                              return "未知异常码";
    }
}

/** 出错的那条指令落在哪个模块、偏移多少 —— 没符号也比"就崩了"强。 */
QString faultingModule(void *address) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &mod) == 0 ||
        mod == nullptr) {
        return QStringLiteral("(定位不到模块)");
    }
    wchar_t path[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
    const QString name = (n > 0) ? QFileInfo(QString::fromWCharArray(path)).fileName()
                                 : QStringLiteral("(模块名取不到)");
    const quintptr base = reinterpret_cast<quintptr>(mod);
    const quintptr addr = reinterpret_cast<quintptr>(address);
    return QStringLiteral("%1 + 0x%2").arg(name).arg(addr - base, 0, 16);
}

/** exe 所在目录（DBGHELP 的符号搜索路径：本工程的 .pdb 就摆在 exe 旁边）。 */
QString exeDir() {
    wchar_t path[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
        return QString();
    return QFileInfo(QString::fromWCharArray(path)).absolutePath();
}

/** 把调用栈符号化写成多行文本。
 *
 *  用 StackWalk64 + **异常现场的 CONTEXT**（不是 CaptureStackBackTrace）：
 *  后者从"当前栈"往回抓，而我们现在是在**异常处理里**，栈上先是我们自己的帧、
 *  再是系统分发帧，抓到"出错那一帧的调用者"就断了 —— 真机第一版报告就是这样，
 *  只看到 Qt 内部的 stop()，看不到是我们哪一行调进去的（这条报告本来就是为了定位，
 *  缺了调用者等于白留）。StackWalk64 从 CONTEXT 起走，能一路回到 main。
 *  符号路径带上 exe 目录：本工程 Release 也出 .pdb（CMake 里开了 ProgramDatabase）。 */
QString stackTrace(EXCEPTION_POINTERS *info) {
    const HANDLE proc = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    const QString dir = exeDir();
    (void)SymInitializeW(proc, dir.isEmpty() ? nullptr : reinterpret_cast<const wchar_t *>(dir.utf16()),
                         TRUE);

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (info != nullptr && info->ContextRecord != nullptr) {
        ctx = *info->ContextRecord;
    } else {
        RtlCaptureContext(&ctx);
    }

    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    QStringList lines;
    for (int i = 0; i < 48; ++i) {
        if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr,
                        SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == FALSE) {
            break;
        }
        if (frame.AddrPC.Offset == 0)
            break;

        const DWORD64 addr = frame.AddrPC.Offset;
        QString symbol;
        char buf[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)] = {0};
        auto *sym = reinterpret_cast<SYMBOL_INFOW *>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddrW(proc, addr, &disp, sym) != 0) {
            symbol = QStringLiteral("%1+0x%2")
                         .arg(QString::fromWCharArray(sym->Name, (int)sym->NameLen))
                         .arg(disp, 0, 16);
        } else {
            symbol = QStringLiteral("(没符号)");
        }
        IMAGEHLP_LINEW64 line;
        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        QString where;
        if (SymGetLineFromAddrW64(proc, addr, &lineDisp, &line) != 0) {
            where = QStringLiteral(" %1:%2")
                        .arg(QString::fromWCharArray(line.FileName))
                        .arg(line.LineNumber);
        }
        lines << QStringLiteral("  #%1 0x%2 %3%4  [%5]")
                     .arg(i, 2)
                     .arg(addr, 16, 16, QLatin1Char('0'))
                     .arg(symbol, where, faultingModule(reinterpret_cast<void *>(addr)));
    }
    (void)SymCleanup(proc);
    return lines.join(QLatin1Char('\n'));
}

/** 同一个进程里只写一次（写报告本身也可能触发异常 —— 不加这个就是无限递归）。 */
volatile LONG g_in_handler = 0;

void writeReport(EXCEPTION_POINTERS *info, const char *how) {
    if (InterlockedCompareExchange(&g_in_handler, 1, 0) != 0) {
        return; // 已经在写了（或者写崩了）：直接让进程死，别套娃
    }
    const QString dir = crashDir();
    if (dir.isEmpty()) {
        return;
    }
    const QString tag = stamp();
    const QString txt = QDir(dir).filePath(QStringLiteral("sxcl-ui-crash-%1.txt").arg(tag));
    const QString dmp = QDir(dir).filePath(QStringLiteral("sxcl-ui-crash-%1.dmp").arg(tag));

    const DWORD code = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0;
    void *address = (info && info->ExceptionRecord)
                        ? info->ExceptionRecord->ExceptionAddress
                        : nullptr;

    FILE *f = fopen(txt.toUtf8().constData(), "wb");
    if (f != nullptr) {
        fprintf(f, "SXCL 启动器崩溃报告\n");
        fprintf(f, "时间: %s\n", QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8().constData());
        fprintf(f, "来源: %s\n", how != nullptr ? how : "(未说明)");
        fprintf(f, "异常码: 0x%08lX %s\n", (unsigned long)code, codeName(code));
        fprintf(f, "出错地址: %p  %s\n", address,
                faultingModule(address).toUtf8().constData());
        fprintf(f, "线程: %lu\n", (unsigned long)GetCurrentThreadId());
        fprintf(f, "运行日志: %s\n", sxcl_log_file_path());
        fprintf(f, "版本: %s\n", qVersion());
        const QString stack = stackTrace(info);
        fprintf(f, "\n调用栈(最内层在最上面):\n%s\n", stack.toUtf8().constData());
        fclose(f);
        SXCL_LOG_E("crash", "启动器崩溃: %s 报告=%s", how != nullptr ? how : "?",
                   txt.toUtf8().constData());
    }

    /* minidump:带内存与线程现场（要更细的定位就用调试器开它）。
     * 写不成不影响上面的文本报告 —— 两样各自独立。 */
    HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(dmp.utf16()), GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                                   MiniDumpWithThreadInfo);
        (void)MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                (info != nullptr) ? type : MiniDumpNormal,
                                (info != nullptr) ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(file);
    }
    sxcl_log_shutdown();
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS *info) {
    writeReport(info, "未处理异常(SetUnhandledExceptionFilter)");
    return EXCEPTION_EXECUTE_HANDLER; // 让进程按默认方式终止（我们只是把现场留下了）
}

void onTerminate() {
    writeReport(nullptr, "C++ 异常逃逸(std::terminate)");
    abort();
}

} // namespace

namespace sxcl::ui {


/* ── 界面卡住(Windows 说"未响应")的取证 ──────────────────────────────
 * 为什么要有:用户 2026-09-23「启动游戏 SXCL 依旧未响应」—— 进程没崩(所以崩溃报告不写),
 * 日志也停在半路(因为卡住的是 GUI 线程,它写不出新行)。没有现场就只能猜。
 * 做法:一个看门狗线程每 500ms 看一次"GUI 线程还跳不跳"(GUI 侧一个 100ms 定时器在
 * 递增计数器);连续 thresholdMs 没跳 -> 落一份 hang 报告 + 全进程 minidump,
 * 里面**所有线程的调用栈**都在(dump 用调试器打开就能看到 GUI 线程卡在哪一行)。
 * 只报一次,不刷屏;写报告绝不动 GUI 线程(不去 SuspendThread,免得把现场弄得更糟)。 */
namespace {

std::atomic<uint64_t> g_guiTick{0};      // GUI 线程在跳(每 100ms +1)
std::atomic<int> g_hangReported{0};

void writeHangReport(int blockedMs) {
    const QString dir = crashDir();
    if (dir.isEmpty()) {
        return;
    }
    const QString tag = stamp();
    const QString txt = QDir(dir).filePath(QStringLiteral("sxcl-ui-hang-%1.txt").arg(tag));
    const QString dmp = QDir(dir).filePath(QStringLiteral("sxcl-ui-hang-%1.dmp").arg(tag));
    FILE *f = fopen(txt.toUtf8().constData(), "wb");
    if (f != nullptr) {
        fprintf(f, "SXCL 启动器**卡住**报告(GUI 线程连续 %d ms 没有响应消息)\n", blockedMs);
        fprintf(f, "时间: %s\n",
                QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8().constData());
        fprintf(f, "运行日志: %s\n", sxcl_log_file_path());
        fprintf(f, "版本: %s\n", qVersion());
        fprintf(f, "说明: 这不是崩溃 —— 进程还在,只是界面线程卡住了;对应的 .dmp 里能看到它卡在哪。\n");
        fclose(f);
    }
    HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(dmp.utf16()), GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                                   MiniDumpWithThreadInfo | MiniDumpWithFullMemory);
        (void)MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr,
                                nullptr, nullptr);
        CloseHandle(file);
    }
    SXCL_LOG_E("crash", "界面卡住 %d ms:报告=%s(全进程 dump 同名)", blockedMs,
               txt.toUtf8().constData());
}

} // namespace

void installHangWatchdog(int thresholdMs) {
    /* GUI 侧的跳表(这个定时器住在主线程上,所以它不跳 = 主线程卡住)。 */
    auto *tick = new QTimer(qApp);
    QObject::connect(tick, &QTimer::timeout, [] { g_guiTick.fetch_add(1); });
    tick->start(100);

    std::thread([thresholdMs]() {
        uint64_t last = g_guiTick.load();
        auto lastChange = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const uint64_t now = g_guiTick.load();
            if (now != last) {
                last = now;
                lastChange = std::chrono::steady_clock::now();
                continue;
            }
            const auto blocked = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - lastChange)
                                     .count();
            if (blocked >= thresholdMs && g_hangReported.exchange(1) == 0) {
                writeHangReport((int)blocked);
            }
        }
    }).detach();
    SXCL_LOG_I("crash", "界面卡住看门狗已就位(%d ms 阈值 -> logs/crashes/sxcl-ui-hang-*)",
               thresholdMs);
}

void installCrashHandler() {
    SetUnhandledExceptionFilter(onUnhandledException);
    std::set_terminate(onTerminate);
    SXCL_LOG_I("crash", "启动器崩溃取证已就位(未处理异常 -> logs/crashes/*.txt + *.dmp)");
}

} // namespace sxcl::ui

#else // 非 Windows：先占位（各平台该用各自的手段，不在这里假装）

namespace sxcl::ui {

void installCrashHandler() {}

} // namespace sxcl::ui

#endif
