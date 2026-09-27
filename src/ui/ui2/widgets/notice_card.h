/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 公告卡(docs/27 §11.4:换掉"最近启动")。
//
// 形态:一张**安静的卡** —— 没有按钮,标题 + 两三行 + 右上角一枚「查看全部」箭头(纯图标,不可点)。
// **没有内容时整块不出现**:所以这张卡由页面按"有没有内容"决定要不要建
// (绝不显示"暂无公告"四个字,也不留一块空白)。
#pragma once

#include <QStringList>
#include <QWidget>

namespace sxcl::ui2 {

class NoticeCard : public QWidget {
    Q_OBJECT
public:
    explicit NoticeCard(const QStringList &lines, QWidget *parent = nullptr);
};

} // namespace sxcl::ui2
