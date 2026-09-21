#pragma once
// ui_error —— 界面层的**统一错误出口**(一处实现,所有页面共用)
//
// 用户要求(原文):"所有报错自动复制到剪贴板"。做法不是每个页面各写一遍,而是:
//
//   pushUiError(...)  或  reportUiError(...)
//        └ 组一份**完整上下文** -> 写 stderr 追踪 -> 复制到系统剪贴板(同一个错误只复制一次)
//             └ 返回给调用方,页面自己决定怎么显示(通常接 InfoBar 的 content)
//
// 复制内容至少包含(与用户点名的四项一一对应):
//   * 时间戳、页面/路由、操作(例如"获取版本清单")
//   * **原始原因文本**(核心库给的人话 + 原始错误码/HTTP 状态码/URL —— 调用方原样传进来,本层不改写)
//   * 版本/构建信息(启动器版本、核心库版本、Qt 版本、平台)
//
// 硬性约束(都按"降级"处理,绝不因为复制失败把主流程带崩):
//   * **同一个错误只复制一次** —— 用 (页面|操作|原因) 做键去重,重复的只留在界面,不再覆盖剪贴板
//     (用户还在看/粘贴上一个错误时,被后来的刷新刷掉是最烦的);
//   * **剪贴板不可用就静默降级** —— QGuiApplication::clipboard() 为空、平台插件不支持、
//     复制抛异常,一律只写日志、不弹错;
//   * 桌面与 Android 同一份代码:走 Qt 的剪贴板抽象,Android 平台插件会转到系统剪贴板,
//     本层不写任何平台分支。
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
