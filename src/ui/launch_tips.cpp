/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 启动页小贴士表(设计理由见 launch_tips.h)。

#include "launch_tips.h"

namespace sxcl::ui {

QStringList launchTips() {
    return QStringList{
        QStringLiteral("按住 Shift 再点物品栏，可以一次挪动一整组物品。"),
        QStringLiteral("饥饿值满了生命值才会慢慢回满，饿着肚子打架很吃亏。"),
        QStringLiteral("床在下界会爆炸，想在下界过夜只能想别的办法。"),
        QStringLiteral("给镐子附上「时运」再去挖矿，同样的矿能多出好几块。"),
        QStringLiteral("身上备一桶水，掉进岩浆前放出来就能立刻灭掉身上的火。"),
        QStringLiteral("甘蔗必须种在水边，一格水最多能照顾四格甘蔗。"),
        QStringLiteral("村民卖什么由他旁边的职业方块决定，拆掉他会重新挑一次。"),
        QStringLiteral("按住潜行再往前走，就不会从方块的边缘掉下去。"),
        QStringLiteral("末影珍珠会摔伤，落地前垫一格水或者放一块石头就好。"),
        QStringLiteral("把地图放进展示框，可以拼出一整面墙的大地图。"),
        QStringLiteral("信标要放在基座顶上才生效，底下垫的矿物块越多范围越大。"),
        QStringLiteral("在铁砧上给装备改名会消耗经验，改得越多越贵。"),
        QStringLiteral("用剪刀剪羊毛比打死羊划算，羊过一阵还会重新长毛。"),
        QStringLiteral("火把插在墙上一样能照亮脚下，进洞窟别舍不得用。"),
        QStringLiteral("雨天钓鱼比晴天更容易上钩。"),
        QStringLiteral("把南瓜戴在头上，末影人就不会主动打你。"),
        QStringLiteral("给弓附上「无限」以后，背包里留一支箭就能一直射。"),
        QStringLiteral("在村庄里放一张床再睡一觉，重生点就改在那儿了。"),
    };
}

QString launchPhaseText(int index) {
    switch (index) {
    case 0:
    case 1:
        return QStringLiteral("正在准备游戏…");
    case 2:
        return QStringLiteral("正在启动游戏…");
    case 3:
        return QStringLiteral("等待游戏…"); // 用户点名保留的那一句
    default:
        return QStringLiteral("游戏已退出");
    }
}

} // namespace sxcl::ui
