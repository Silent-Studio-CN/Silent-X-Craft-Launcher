#pragma once
// libqf.h —— libqf(qfluentwidgets 的 C/C++ 重写)的统一入口
//
// 两个必须记住的事实:
//   1) 库里的窗口/导航/图标类在**全局命名空间**(FluentWindowBase / FluentTitleBar /
//      NavigationPushButton ...),只有主题与样式在 namespace fluent 里
//      (fluent::Theme / fluent::FluentStyle / fluent::setTheme / fluent::icon)。
//   2) 它的头文件在 /W4 下不是零警告(例如 fluent_style.h 的 C4100 未引用参数),
//      而本工程 /WX 把警告当错误。libqf 对我们是"外部依赖",所以这里用 MSVC 的
//      warning push/pop 把它整段静音 —— 注意 /W4 /WX 仍然约束我们自己写的代码。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include "fluent/fluent_icon.h"
#include "fluent/fluent_navigation.h"
#include "fluent/fluent_style.h"
#include "fluent/fluent_window.h"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
