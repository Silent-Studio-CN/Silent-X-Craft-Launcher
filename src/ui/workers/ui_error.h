/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QString>

class QWidget;

namespace sxcl::ui {

// 一次错误的完整上下文。**reason 原样进剪贴板**,本层不做任何改写/截断(只在显示时截)。
struct UiErrorContext {
    QString page;    // 页面/路由,如 "下载进度页 / download_progress_1.20.1"
    QString action;  // 操作,如 "安装 1.20.1(加载器 none)"
    QString reason;  // **原始原因**:核心库人话 + 错误码/HTTP 状态码/URL
    QString detail;  // 可选补充(阶段、路径、退出码…)
    QString title;   // InfoBar 标题(只用于显示)
    bool warning = false; // true = 用 Warning 色的 InfoBar(语义仍是"用户要看的错误",一样复制)
};

// 组文案 + 去重 + 复制 + 追踪。返回**完整报告文本**(调用方可以只用来显示)。
// 已复制过同一个错误时,文本照旧返回,只是不再碰剪贴板。
QString reportUiError(const UiErrorContext &context);

// 便捷出口:InfoBar(error) + 复制。content 末尾会带上"完整错误已复制到剪贴板"。
// host 为空时不弹 InfoBar(仍然复制 + 追踪)—— 与 libqf InfoBar::push 的语义一致。
void pushUiError(QWidget *host, const UiErrorContext &context, int autoCloseMs = 10000);

// 自检/取证用:当前会话已经复制过几条(去重前计). 报告里给数字。
int uiErrorCopiedCount();
// 自检用:最近一次组出来的报告全文(剪贴板不可用时也能核对内容)
QString uiErrorLastReport();

} // namespace sxcl::ui
