// settings_page.cpp —— 未移植占位(实现在后续批次)
//
// 参照:Python 版 src/app/pages/settings_page.py 与 docs/05-UI-1to1规格.md。
// 返回 nullptr 时主窗口用占位页,构建与导航不受影响。
#include "page_factory.h"

#include <QWidget>

namespace sxcl::ui {

QWidget *createSettingsPage(QWidget *parent) {
    Q_UNUSED(parent);
    return nullptr;
}

} // namespace sxcl::ui
