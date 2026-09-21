#include "ui_error.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QSet>
#include <QStringList>
#include <QSysInfo>

// 版本契约:与 main_window.cpp:40 同一条相对路径(不依赖链接 sxcl 也能拿到宏)
#include "../../include/sxcl/version.h"
#include "ui_paths.h"

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 的头在 /W4 下不是零警告,整体静音(见 libqf.h 的说明)
#endif
#include "fluent/fluent_controls.h" // InfoBar
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QCoreApplication>
#include <QtGlobal>

namespace sxcl::ui {
namespace {

// 会话内已经复制过的错误键(去重)。只在界面线程访问 —— 本层的调用点都是界面线程。
QSet<QString> &copiedKeys() {
    static QSet<QString> keys;
    return keys;
}

int &copiedCount() {
    static int count = 0;
    return count;
}

QString &lastReport() {
    static QString text;
    return text;
}

QString launcherVersionText() {
    return QStringLiteral("%1.%2.%3")
        .arg(SXCL_VERSION_MAJOR)
        .arg(SXCL_VERSION_MINOR)
        .arg(SXCL_VERSION_PATCH);
}

} // namespace

QString reportUiError(const UiErrorContext &context) {
    // ── 1. 组一份完整上下文的报告(纯文本:用户要粘到聊天/工单里,不能是富文本)──
    QStringList lines;
    lines << QStringLiteral("=== Silent X Craft Launcher 错误报告 ===");
    lines << QStringLiteral("时间: %1")
                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    lines << QStringLiteral("页面: %1").arg(context.page.isEmpty() ? QStringLiteral("-")
                                                                   : context.page);
    lines << QStringLiteral("操作: %1").arg(context.action.isEmpty() ? QStringLiteral("-")
                                                                     : context.action);
    // **原始原因**:调用方传什么就是什么,这里一个字都不改
    lines << QStringLiteral("原因: %1")
                 .arg(context.reason.isEmpty() ? QStringLiteral("(调用方没有给原因)")
                                               : context.reason);
    if (!context.detail.isEmpty())
        lines << QStringLiteral("详情: %1").arg(context.detail);
    lines << QStringLiteral("启动器: Silent X Craft Launcher %1").arg(launcherVersionText());
    lines << QStringLiteral("Qt: %1   平台: %2 / %3")
                 .arg(QString::fromLatin1(qVersion()),
                      QSysInfo::kernelType(),
                      QSysInfo::currentCpuArchitecture());
    lines << QStringLiteral("可执行: %1").arg(QCoreApplication::applicationFilePath());
    const QString report = lines.join(QLatin1Char('\n'));

    lastReport() = report;

    // ── 2. 去重键:同一个错误只复制一次(界面照旧显示,剪贴板不动)──
    const QString key = context.page + QLatin1Char('|') + context.action + QLatin1Char('|') +
                        context.reason;
    const bool firstTime = !copiedKeys().contains(key);
    if (firstTime)
        copiedKeys().insert(key);

    // ── 3. 复制(失败一律静默降级:剪贴板为空/平台不支持/抛异常都不能影响主流程)──
    bool copied = false;
    if (firstTime) {
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(report, QClipboard::Clipboard);
            // 平台插件可能"接受了调用但没真的落地"(无剪贴板的后端)。
            // 用**读回来比对**判定,而不是假定 setText 一定成功。
            copied = (clipboard->text(QClipboard::Clipboard) == report);
        }
    }

    if (copied)
        copiedCount() += 1;

    // 剪贴板回读长度:证明"setText 之后真的读得回来同一份文本"。
    // (注意:-platform offscreen 时 QPA 的剪贴板是**进程内**的,读回来能一致,
    //  但系统的 Win32 剪贴板看不到它 —— 这是 offscreen 取证的固有边界,不是本层的缺陷;
    //  本层没有任何平台分支,桌面/Android 走的是同一条 Qt 剪贴板抽象。)
    int readbackBytes = -1;
    if (firstTime) {
        if (QClipboard *clipboard = QGuiApplication::clipboard())
            readbackBytes = clipboard->text(QClipboard::Clipboard).size();
    }

    // ── 4. 追踪(SXCL_UI_TRACE=1):报告全文进 stderr,验收可以直接核对 ──
    uiTrace(QStringLiteral("error | page=%1 action=%2 copied=%3 repeat=%4 "
                           "report-bytes=%5 clipboard-readback-bytes=%6")
                .arg(context.page, context.action,
                     copied ? QStringLiteral("yes") : QStringLiteral("no"),
                     firstTime ? QStringLiteral("no") : QStringLiteral("yes"))
                .arg(report.size())
                .arg(readbackBytes));
    uiTrace(QStringLiteral("error-report-begin\n%1\nerror-report-end").arg(report));
    return report;
}

void pushUiError(QWidget *host, const UiErrorContext &context, int autoCloseMs) {
    const QString report = reportUiError(context);
    Q_UNUSED(report)
    if (host == nullptr)
        return;
    // 显示上只给"原因 + 一句提示",完整报告在剪贴板里(用户要的是能粘出去,不是糊满屏幕)
    QString content = context.reason.isEmpty() ? QStringLiteral("发生了未知错误") : context.reason;
    if (content.size() > 300)
        content = content.left(300) + QStringLiteral("…");
    content += QStringLiteral("\n完整错误已复制到剪贴板（可直接粘贴反馈）");
    InfoBar::push(context.warning ? InfoBar::Type::Warning : InfoBar::Type::Error,
                  context.title.isEmpty() ? QStringLiteral("出错了") : context.title, content,
                  host, autoCloseMs);
}

int uiErrorCopiedCount() { return copiedCount(); }

QString uiErrorLastReport() { return lastReport(); }

} // namespace sxcl::ui
