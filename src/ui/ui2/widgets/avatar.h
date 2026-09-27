/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 头像(圆形,docs/27 §11.2):正版用 ID 首字母,离线用玩家名首字母。
//
// 为什么用字母而不是皮肤渲染:用户口径就是"ID 首字母"(M2 这一版),而且这样**不需要**
// 任何图片素材、任何网络;皮肤贴图留给后面接账户那一轮。
#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

namespace sxcl::ui2 {

class AvatarWidget : public QWidget {
    Q_OBJECT
public:
    explicit AvatarWidget(QWidget *parent = nullptr);

    /** 名字:取第一个字符当字母(空 = 画一个"?"占位圆)。 */
    void setName(const QString &name);
    QString name() const { return m_name; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_name;
};

} // namespace sxcl::ui2
