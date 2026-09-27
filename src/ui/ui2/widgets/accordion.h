/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 就地手风琴(docs/27 §11.3:点「切换版本」**不是跳页**,是当前版本卡下方拉出一列版本)。
//
// 这一版的时间线(基座说明):展开/收起 = **220ms OutCubic** 的高度动画(QPropertyAnimation
// 驱动 maximumHeight,不 setGeometry);列表按 **25ms/行** 错峰冒出来。
// 为什么不用 Anim::stagger:src/ui/ui2/anim.* 此刻正在由另一个代理落地(尚未提交),
// 这一轮不把它的 API 编进来 —— 换成它的那一行在 staggerRows() 里留了注释,等它落地即可替换。
//
// 判据(验收脚本按它断言):body 高度 0 -> >0 -> 0;行可见性;错峰延迟表逐行 25ms。
#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QPropertyAnimation;
class QVBoxLayout;

namespace sxcl::ui2 {

struct VersionRow {
    QString name; // 版本名
    QString meta; // 元信息一行(加载器 / 原版)
};

/** "查看全部版本 ->" 那一行:文字 + 一枚箭头图标(箭头按令牌现染),整行可点。
 *  键盘可达(§4):Tab 能到、空格/回车能按。 */
class LinkRow : public QWidget {
    Q_OBJECT
public:
    explicit LinkRow(const QString &text, QWidget *parent = nullptr);

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
};

class AccordionSection : public QWidget {
    Q_OBJECT
public:
    explicit AccordionSection(QWidget *parent = nullptr);

    void setRows(const QVector<VersionRow> &rows);
    int rowCount() const { return m_rows.size(); }

    bool expanded() const { return m_expanded; }
    /** 展开/收起(220ms OutCubic;重复调用同一个状态是幂等的)。 */
    void setExpanded(bool on);
    void toggle() { setExpanded(!m_expanded); }

    QWidget *body() const { return m_body; }
    QVector<QWidget *> rows() const { return m_rows; }
    LinkRow *allButton() const { return m_all; }

signals:
    void expandedChanged(bool expanded);
    void allRequested();

private:
    void staggerRows();
    void hideRows();
    int bodyTargetHeight() const;

    QLabel *m_title = nullptr;
    QWidget *m_body = nullptr;
    QVBoxLayout *m_bodyLay = nullptr;
    LinkRow *m_all = nullptr;
    QVector<QWidget *> m_rows;
    QPropertyAnimation *m_anim = nullptr;
    bool m_expanded = false;
};

} // namespace sxcl::ui2
