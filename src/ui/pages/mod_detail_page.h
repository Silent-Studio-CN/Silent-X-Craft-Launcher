/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 模组 / 光影的**详情页**（用户 2026-09-27：
//   「关于模组介绍信息，强制只有一行，后面的用 3 个点代替，单击进去可以看模组详细信息」）。
//
// 口径（与列表行那一边配套）：
//   * 结果行点一下**进这里**（不再直接装）；行本身仍然零按钮、四行结构不变；
//   * 这一页给全：图标 / 名字 / 来源 / 开发商 / 版本范围 / 下载量 / 更新时间 / **完整介绍**
//     （一个字都不截、换行显示 —— 列表里那一行才是省略号）；
//   * **一个**明显的「安装」动作：点它 = 按当前实例的版本 + 加载器挑最合适的那个文件装下去
//     （"点击代表我要它"落在这一步），不是一排按钮；
//   * 「好几个版本都说得过去」时把可选项摆在这一页里（点一项 = 我要它），不排按钮；
//   * 装的过程里按钮自己锁住（"正在装…"），成功/失败照旧由窗口的提示条如实报出来。
#pragma once

#include "page_shell.h"           // 与下载/模组页同一个外壳(标题 + 副标题 + 可滚动内容区)
#include "../workers/mods_merge.h"  // ModsHitRow / 下载量与相对时间那份展示口径

#include <QString>

#include <functional>
#include <vector>

class QAbstractButton;
class QLabel;
class QVBoxLayout;

struct sxcl_mod_file;  // 核心库 include/sxcl/mods.h

namespace sxcl::ui {

/** 详情页 logo 那个**固定方块**的边长（模组页下图标时按它缩 —— 与列表里那枚 48 同一份缓存）。 */
constexpr int kModDetailIconSide = 96;

class ModDetailPage : public PageShell {
public:
    /** 装哪一个：由模组页那一栏给（只有它知道眼前是哪个实例、装进 mods 还是 shaderpacks）。 */
    using InstallHandler = std::function<void()>;
    using FilePickHandler = std::function<void(const sxcl_mod_file &)>;
    using BackHandler = std::function<void()>;

    explicit ModDetailPage(QWidget *parent);

    /** 换成点进来的那一条（同一个页面复用：不重建，状态也就不会越攒越多）。 */
    void setItem(const ModsHitRow &row);

    void setInstallHandler(InstallHandler handler) { m_install = std::move(handler); }
    void setFilePickHandler(FilePickHandler handler) { m_pickFile = std::move(handler); }
    void setBackHandler(BackHandler handler) { m_back = std::move(handler); }

    /** 装的过程中把「安装」锁住（点一次就是一次）。 */
    void setInstalling(bool installing);

    /** 这个包"好几个版本都说得过去"：把可选项摆出来（**点一项 = 我要它**）。 */
    void setFileChoices(const std::vector<sxcl_mod_file> &files, size_t count);
    void clearFileChoices();

    const QString &rowId() const { return m_id; }
    /** 图标那格：模组页那一栏拿它去下图标（走它自己那份缓存/工作线程）。 */
    QLabel *iconLabel() const { return m_icon; }

private:
    QString m_id;
    QLabel *m_icon = nullptr; // 只是一格方框：填图由模组页那一栏做（startIcon）
    BodyLabel *m_source = nullptr;
    BodyLabel *m_author = nullptr;
    BodyLabel *m_versions = nullptr;
    BodyLabel *m_downloads = nullptr;
    BodyLabel *m_updated = nullptr;
    BodyLabel *m_description = nullptr; // **完整**介绍（列表那一行才是省略号）
    BodyLabel *m_backLink = nullptr;    // 「返回」是一个可点的字，不是按钮
    QAbstractButton *m_installButton = nullptr;
    QVBoxLayout *m_choiceBox = nullptr;
    InstallHandler m_install;
    FilePickHandler m_pickFile;
    BackHandler m_back;
};

} // namespace sxcl::ui
