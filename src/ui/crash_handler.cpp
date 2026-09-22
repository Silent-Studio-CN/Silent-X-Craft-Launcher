/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 启动器自己的崩溃取证（声明与理由见 crash_handler.h）。

#define _CRT_SECURE_NO_WARNINGS 1

#include "crash_handler.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>

#include <QDateTime>
#include <QDir>
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

/** 把调用栈符号化写成多行文本（DBGHELP 的符号服务；拿不到符号就打模块+偏移）。 */
QString stackTrace() {
    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    (void)SymInitializeW(proc, nullptr, TRUE);

    void *frames[40] = {0};
    const USHORT got = CaptureStackBackTrace(0, 40, frames, nullptr);

    QStringList lines;
    for (USHORT i = 0; i < got; ++i) {
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        QString symbol;
        char buf[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)] = {0};
        auto *info = reinterpret_cast<SYMBOL_INFOW *>(buf);
        info->SizeOfStruct = sizeof(SYMBOL_INFOW);
        info->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddrW(proc, addr, &disp, info) != 0) {
            symbol = QStringLiteral("%1+0x%2")
                         .arg(QString::fromWCharArray(info->Name, (int)info->NameLen))
                         .arg(disp, 0, 16);
        } else {
            symbol = QStringLiteral("(没符号)");
        }
        lines << QStringLiteral("  #%1 0x%2 %3  [%4]")
                     .arg(i, 2)
                     .arg(addr, 16, 16, QLatin1Char('0'))
                     .arg(symbol, faultingModule(frames[i]));
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
        const QString stack = stackTrace();
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
