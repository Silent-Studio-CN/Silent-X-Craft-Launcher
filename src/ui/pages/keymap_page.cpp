// keymap_page.cpp —— 按键映射页(在电脑上编排手机用的虚拟按键)
//
// 1:1 移植 Python 版 src/app/pages/keymap_page.py(+ 它依赖的 src/core/keymap 数据模型)。
//
// 页面结构(keymap_page.py:235-306 _build_content):
//   BasePage(title="按键映射", subtitle="给手机端用的虚拟按键布局；可在此编排、检查冲突、导出教学")
//     vBoxLayout: contentsMargins(28,24,28,24) / spacing 16 / AlignTop     base_page.py:51-53
//       toolbar  QWidget + QHBoxLayout margins(0,0,0,0) spacing 10        :236-239
//                BodyLabel("预设") / ComboBox(预设) / ComboBox(横竖屏,固定宽 90) / SearchLineEdit(stretch 1)
//       canvas_card  CardWidget + QVBoxLayout margins(8,8,8,8)            :308-314
//       body    QHBoxLayout spacing 16: canvas(stretch 3) + side(stretch 2) :278-281
//               side = QVBoxLayout spacing 8: StrongBodyLabel("冲突检查") / QListWidget(minH 110)
//                                            StrongBodyLabel("新手教学（可导出给手机端）") / QListWidget
//       buttons QHBoxLayout spacing 8: 六个按钮 + addStretch(1)            :283-300
//
// 画布(KeymapCanvas,:80-218)按 0~1 归一化坐标绘制"手机形状 + 虚拟按键",
// 颜色/线宽/圆角/字号全部照抄 Python,无一自创:
//   底色 bg / 手机 card+border_strong 2px 圆角 14 / 顶部一行 8pt tertiary 说明
//   普通键 accent alpha=255*min(0.75,opacity) 圆或圆角 8 矩形,描边 border_strong 1.4
//   摇杆画"中心圆(bg+border_strong 1.2)+ 摇杆帽(accent)",标签字号 max(7.5,min(11,h/3.2))pt
#include "page_factory.h"

#include "fluent_theme.h"
#include "libqf.h"
#include "theme_bridge.h"

// 键位数据的"唯一事实来源"是纯 C 核心库(keymap.c/keymap_store.c/keymap_fcl.c,
// 对应 Python 的 model.py/store.py/fcl.py):本页只负责画与编排,落盘/读盘一律走它。
#include "sxcl/keymap.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_input.h"
#include "fluent/fluent_labels.h"
#include "fluent/fluent_scroll.h"
#include "fluent/fluent_setting_cards.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QAbstractButton>
#include <QBrush>
#include <QComboBox>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>

namespace sxcl::ui {
namespace {

// ================================================================
// 一、数据模型 —— 对应 src/core/keymap/model.py + presets.py + guide.py
//     只搬这一页用得到的部分;presets/guide 的全部预设都照抄,便于下拉框切屏。
// ================================================================

struct KBinding {                // model.py:Binding
    QString action;
    QStringList keys;
    QString behavior = QStringLiteral("hold");
};

struct KControl {                // model.py:ControlButton + DirectionControl
    QString id;
    QString label;
    QString hint;
    QString icon;
    double x = 0.1;
    double y = 0.6;
    double w = 0.09;
    double h = 0.16;
    QString shape = QStringLiteral("round");   // round / square / pill
    double opacity = 0.55;
    QString group;
    QString aliasOf;
    // DirectionControl 专有
    bool isDirection = false;
    QString style = QStringLiteral("dpad_compact"); // dpad / rocker / dpad_compact
    double deadZone = 0.18;
    QMap<QString, QString> dirKeys;             // up/down/left/right

    // Python events 是 dict(插入序);用 QVector 保序,行为一致
    QVector<QPair<QString, KBinding>> events;

    void bind(const QString &kind, const KBinding &b) {
        static const char *kKinds[] = {"press", "long_press", "click", "double_click"};
        for (const char *k : kKinds) {
            if (kind == QLatin1String(k)) { events.append({kind, b}); return; }
        }
    }
    QStringList allKeys() const {                  // model.py:121-126 / 191-192
        QStringList out;
        if (isDirection) {
            for (const QString &v : dirKeys) if (!v.isEmpty()) out << v;
            return out;
        }
        for (const auto &ev : events) out << ev.second.keys;
        return out;
    }
};

struct KConflict {               // model.py:Conflict
    QString level;
    QString message;
    QStringList controls;
};

struct KGuideStep {              // guide.py:GuideStep
    int order = 0;
    QString title;
    QString instruction;
    QString why;
    QString controlId;
    QStringList keys;
    bool optional = false;
};

struct KLayout {                 // model.py:KeymapLayout
    QString name = QStringLiteral("默认");
    QString screen = QStringLiteral("landscape");
    QString description;
    // model.py 的 meta:页面自己不用它,但必须原样带住 —— FCL 导入会把原始数据塞在
    // meta.fcl_raw 里,编辑完再"保存为我的布局"时不能把它丢了(fcl.py/store.py 都不丢)。
    QJsonObject meta;
    QVector<KControl> buttons;
    QVector<KControl> directions;

    QVector<KControl> controls() const {           // model.py:252-253
        QVector<KControl> out = directions;
        out += buttons;
        return out;
    }

    QSet<QString> actionIndex() const {            // model.py:261-271
        QSet<QString> index;
        for (const KControl &c : buttons)
            for (const auto &ev : c.events)
                if (!ev.second.action.isEmpty()) index.insert(ev.second.action);
        for (const KControl &d : directions)
            for (auto it = d.dirKeys.constBegin(); it != d.dirKeys.constEnd(); ++it)
                index.insert(it.key());
        return index;
    }

    QStringList find(const QString &text) const {  // model.py:273-286
        const QString query = text.trimmed().toLower();
        QStringList hits;
        if (query.isEmpty()) return hits;
        for (const KControl &c : controls()) {
            QStringList hay{c.id, c.label, c.hint};
            hay += c.allKeys();
            if (!c.isDirection)
                for (const auto &ev : c.events) hay << ev.second.action;
            for (const QString &item : hay) {
                if (!item.isEmpty() && item.toLower().contains(query)) { hits << c.id; break; }
            }
        }
        return hits;
    }

    QStringList validate() const {                 // model.py:290-304
        QStringList errors;
        QSet<QString> seen;
        for (const KControl &c : controls()) {
            if (c.id.isEmpty()) { errors << QStringLiteral("存在没有 id 的控件"); continue; }
            if (seen.contains(c.id)) errors << QStringLiteral("控件 id 重复: ") + c.id;
            seen.insert(c.id);
            if (c.x + c.w > 1.001 || c.y + c.h > 1.001)
                errors << c.id + QStringLiteral(": 超出屏幕范围");
            if (!c.isDirection && c.events.isEmpty())
                errors << c.id + QStringLiteral(": 没有绑定任何事件");
        }
        return errors;
    }

    QVector<KConflict> conflicts() const {         // model.py:306-359
        QVector<KConflict> result;
        // 1) 同一个按键被多个控件抢
        QMap<QString, QVector<QPair<QString, QStringList>>> perKey;
        for (const KControl &c : controls()) {
            QMap<QString, QSet<QString>> actionsByKey;
            for (const auto &ev : c.events) {
                for (const QString &key : ev.second.keys) {
                    if (!c.aliasOf.isEmpty()) actionsByKey[key].insert(c.aliasOf);
                    else if (!ev.second.action.isEmpty()) actionsByKey[key].insert(ev.second.action);
                }
            }
            for (auto it = actionsByKey.constBegin(); it != actionsByKey.constEnd(); ++it) {
                QStringList acts = it.value().values();
                acts.sort();
                perKey[it.key()].append({c.id, acts});
            }
        }
        for (auto it = perKey.constBegin(); it != perKey.constEnd(); ++it) {
            const auto &owners = it.value();
            if (owners.size() < 2) continue;
            QSet<QString> allActions;
            bool allSingle = true;
            for (const auto &o : owners) {
                for (const QString &a : o.second) allActions.insert(a);
                if (o.second.size() > 1) allSingle = false;
            }
            if (allActions.size() <= 1 && allSingle) continue;
            QStringList names;
            for (const auto &o : owners) names << o.first;
            KConflict conf;
            conf.level = QStringLiteral("warning");
            conf.message = QStringLiteral("按键 %1 被 %2 个控件绑到不同动作（%3），会互相打架")
                               .arg(it.key()).arg(owners.size())
                               .arg(names.join(QString::fromUtf8("、")));
            conf.controls = names;
            result.append(conf);
        }
        // 2) 控件互相重叠(重叠面积 / 较小者面积 > 0.6)
        const QVector<KControl> all = controls();
        for (int i = 0; i < all.size(); ++i) {
            for (int j = i + 1; j < all.size(); ++j) {
                const double ratio = overlapRatio(all[i], all[j]);
                if (ratio > 0.6) {
                    KConflict conf;
                    conf.level = QStringLiteral("warning");
                    conf.message = QStringLiteral("%1 与 %2 位置重叠（%3%），触屏容易误触")
                                       .arg(all[i].id, all[j].id)
                                       .arg(ratio * 100.0, 0, 'f', 0);
                    conf.controls = {all[i].id, all[j].id};
                    result.append(conf);
                }
            }
        }
        // 3) 关键动作缺失
        const QSet<QString> actions = actionIndex();
        const QPair<QString, QString> required[] = {
            {QStringLiteral("jump"), QString::fromUtf8("跳跃")},
            {QStringLiteral("forward"), QString::fromUtf8("移动")},
            {QStringLiteral("inventory"), QString::fromUtf8("打开背包")},
        };
        for (const auto &req : required) {
            if (!actions.contains(req.first)
                && !(req.first == QLatin1String("forward") && !directions.isEmpty())) {
                KConflict conf;
                conf.level = QStringLiteral("error");
                conf.message = QStringLiteral("没有绑定「%1」，进游戏后会寸步难行").arg(req.second);
                result.append(conf);
            }
        }
        return result;
    }

private:
    static double overlapRatio(const KControl &a, const KControl &b) { // model.py:398-408
        const double left = std::max(a.x, b.x);
        const double top = std::max(a.y, b.y);
        const double right = std::min(a.x + a.w, b.x + b.w);
        const double bottom = std::min(a.y + a.h, b.y + b.h);
        if (right <= left || bottom <= top) return 0.0;
        const double overlap = (right - left) * (bottom - top);
        const double smaller = std::min(a.w * a.h, b.w * b.h);
        return smaller > 0 ? overlap / smaller : 0.0;
    }
};

// ---- presets.py 逐条照抄 ----

KControl makeButton(const QString &id, const QString &label, double x, double y, double w,
                    double h, const QString &action = QString(), const QStringList &keys = {},
                    const QString &hint = QString(), const QString &group = QStringLiteral("right"),
                    const QString &shape = QStringLiteral("round"),
                    const QString &longAction = QString(), const QStringList &longKeys = {},
                    const QString &clickAction = QString(), const QStringList &clickKeys = {},
                    const QString &doubleAction = QString(), const QStringList &doubleKeys = {},
                    double opacity = 0.55, const QString &aliasOf = QString()) {
    KControl b;                                    // presets.py:_btn
    b.id = id; b.label = label; b.hint = hint;
    b.x = x; b.y = y; b.w = w; b.h = h;
    b.shape = shape; b.group = group; b.opacity = opacity; b.aliasOf = aliasOf;
    if (!action.isEmpty() || !keys.isEmpty())
        b.bind(QStringLiteral("press"), {action, keys, QStringLiteral("hold")});
    if (!longAction.isEmpty() || !longKeys.isEmpty())
        b.bind(QStringLiteral("long_press"), {longAction, longKeys, QStringLiteral("hold")});
    if (!clickAction.isEmpty() || !clickKeys.isEmpty())
        b.bind(QStringLiteral("click"), {clickAction, clickKeys, QStringLiteral("tap")});
    if (!doubleAction.isEmpty() || !doubleKeys.isEmpty())
        b.bind(QStringLiteral("double_click"), {doubleAction, doubleKeys, QStringLiteral("toggle")});
    return b;
}

KControl makeMove(const QString &screen) {          // presets.py:_move
    KControl d;
    d.isDirection = true;
    d.id = QStringLiteral("move");
    d.label = QString::fromUtf8("移动");
    d.hint = QString::fromUtf8("推到底可以快走，配合潜行键=疾跑");
    if (screen == QLatin1String("portrait")) {
        d.x = 0.06; d.y = 0.62; d.w = 0.34; d.h = 0.22;
    } else {
        d.x = 0.03; d.y = 0.58; d.w = 0.20; d.h = 0.36;
    }
    d.style = QStringLiteral("rocker");
    d.group = QStringLiteral("left");
    d.opacity = 0.5;
    d.dirKeys = {{QStringLiteral("up"), QStringLiteral("KEY_W")},
                 {QStringLiteral("down"), QStringLiteral("KEY_S")},
                 {QStringLiteral("left"), QStringLiteral("KEY_A")},
                 {QStringLiteral("right"), QStringLiteral("KEY_D")}};
    return d;
}

QVector<KControl> coreButtons(const QString &screen) { // presets.py:_core_buttons
    const QString lmb = QStringLiteral("MOUSE_LEFT");
    const QString rmb = QStringLiteral("MOUSE_RIGHT");
    if (screen == QLatin1String("portrait")) {
        return {
            makeButton(QStringLiteral("jump"), QString::fromUtf8("跳"), 0.78, 0.66, 0.16, 0.09,
                       QStringLiteral("jump"), {QStringLiteral("KEY_SPACE")},
                       QString::fromUtf8("空格键：跳跃。按住可以连续跳（跑酷常用）")),
            makeButton(QStringLiteral("mine"), QString::fromUtf8("挖"), 0.60, 0.78, 0.16, 0.09,
                       QStringLiteral("attack"), {lmb},
                       QString::fromUtf8("左键：攻击 / 挖掘。按住就是一直挖，不用反复点")),
            makeButton(QStringLiteral("place"), QString::fromUtf8("放"), 0.78, 0.78, 0.16, 0.09,
                       QStringLiteral("use"), {rmb},
                       QString::fromUtf8("右键：放置方块 / 使用物品 / 开门 / 喂动物")),
            makeButton(QStringLiteral("sneak"), QString::fromUtf8("潜行"), 0.42, 0.78, 0.16, 0.09,
                       QStringLiteral("sneak"), {QStringLiteral("KEY_LEFT_SHIFT")},
                       QString::fromUtf8("Shift：潜行（不会掉下方块）。双击可以切换成常驻潜行"),
                       QStringLiteral("right"), QStringLiteral("pill"), QString(), {},
                       QString(), {}, QStringLiteral("sneak"),
                       {QStringLiteral("KEY_LEFT_SHIFT")}),
        };
    }
    return {
        makeButton(QStringLiteral("jump"), QString::fromUtf8("跳"), 0.86, 0.62, 0.09, 0.16,
                   QStringLiteral("jump"), {QStringLiteral("KEY_SPACE")},
                   QString::fromUtf8("空格键：跳跃。按住可以连续跳（跑酷常用）")),
        makeButton(QStringLiteral("mine"), QString::fromUtf8("挖"), 0.74, 0.74, 0.10, 0.18,
                   QStringLiteral("attack"), {lmb},
                   QString::fromUtf8("左键：攻击 / 挖掘。按住就是一直挖，不用反复点")),
        makeButton(QStringLiteral("place"), QString::fromUtf8("放"), 0.86, 0.74, 0.10, 0.18,
                   QStringLiteral("use"), {rmb},
                   QString::fromUtf8("右键：放置方块 / 使用物品 / 开门")),
        makeButton(QStringLiteral("sneak"), QString::fromUtf8("潜行"), 0.62, 0.80, 0.10, 0.10,
                   QStringLiteral("sneak"), {QStringLiteral("KEY_LEFT_SHIFT")},
                   QString::fromUtf8("Shift：潜行。双击 = 常驻潜行（挂机搭桥很省手）"),
                   QStringLiteral("right"), QStringLiteral("pill"), QString(), {}, QString(), {},
                   QStringLiteral("sneak"), {QStringLiteral("KEY_LEFT_SHIFT")}),
    };
}

QVector<KControl> hotbar(const QString &screen) {   // presets.py:_hotbar
    QVector<KControl> buttons;
    const int count = screen == QLatin1String("landscape") ? 9 : 5;
    const double width = screen == QLatin1String("landscape") ? 0.045 : 0.09;
    const double gap = 0.008;
    const double startX = 0.5 - (count * (width + gap) - gap) / 2.0;
    const double y = screen == QLatin1String("landscape") ? 0.94 : 0.45;
    for (int index = 0; index < count; ++index) {
        const QString n = QString::number(index + 1);
        buttons << makeButton(
            QStringLiteral("slot") + n, n, startX + index * (width + gap), y, width,
            screen == QLatin1String("landscape") ? 0.05 : 0.06,
            QStringLiteral("hotbar_") + n, {QStringLiteral("KEY_") + n},
            QStringLiteral("快捷栏第 %1 格（键盘 %2）").arg(index + 1).arg(index + 1),
            QStringLiteral("center"), QStringLiteral("square"), QString(), {}, QString(), {},
            QString(), {}, 0.45);
    }
    return buttons;
}

KLayout buildPreset(const QString &name, const QString &screen) { // presets.py:225-230
    KLayout layout;
    layout.screen = screen;
    QVector<KControl> buttons = coreButtons(screen);
    if (name == QLatin1String("survival")) {                // _survival
        layout.name = QString::fromUtf8("生存");
        layout.description = QString::fromUtf8("探索/挖矿/打怪用的一套完整布局");
        buttons << makeButton(QStringLiteral("inventory"), QString::fromUtf8("背包"), 0.62, 0.60,
                              0.09, 0.14, QStringLiteral("inventory"), {QStringLiteral("KEY_E")},
                              QString::fromUtf8("E：打开背包。长按 = 打开聊天（发消息）"),
                              QStringLiteral("right"), QStringLiteral("round"),
                              QStringLiteral("chat"), {QStringLiteral("KEY_T")});
        buttons << makeButton(QStringLiteral("drop"), QString::fromUtf8("丢弃"), 0.50, 0.68,
                              0.09, 0.14, QStringLiteral("drop"), {QStringLiteral("KEY_Q")},
                              QString::fromUtf8("Q：丢掉手上物品（小心别把钻石扔了）"));
    } else if (name == QLatin1String("building")) {         // _building
        layout.name = QString::fromUtf8("建造");
        layout.description = QString::fromUtf8("搭建筑/红石用：带快捷栏 1-9 与飞行上下");
        buttons += hotbar(screen);
        buttons << makeButton(QStringLiteral("inventory"), QString::fromUtf8("背包"), 0.62, 0.60,
                              0.09, 0.14, QStringLiteral("inventory"), {QStringLiteral("KEY_E")},
                              QString::fromUtf8("E：背包；长按 = 聊天"),
                              QStringLiteral("right"), QStringLiteral("round"),
                              QStringLiteral("chat"), {QStringLiteral("KEY_T")});
        buttons << makeButton(QStringLiteral("fly_up"), QString::fromUtf8("升"), 0.50, 0.44, 0.08,
                              0.10, QStringLiteral("jump"), {QStringLiteral("KEY_SPACE")},
                              QString::fromUtf8("创造模式飞行上升（双击空格开始/停止飞行）"),
                              QStringLiteral("center"), QStringLiteral("round"), QString(), {},
                              QString(), {}, QStringLiteral("jump"),
                              {QStringLiteral("KEY_SPACE")}, 0.55, QStringLiteral("jump"));
        buttons << makeButton(QStringLiteral("fly_down"), QString::fromUtf8("降"), 0.50, 0.56, 0.08,
                              0.10, QStringLiteral("sneak"), {QStringLiteral("KEY_LEFT_SHIFT")},
                              QString::fromUtf8("创造模式飞行下降"), QStringLiteral("center"),
                              QStringLiteral("round"), QString(), {}, QString(), {}, QString(), {},
                              0.55, QStringLiteral("sneak"));
        buttons << makeButton(QStringLiteral("sprint"), QString::fromUtf8("疾跑"), 0.30, 0.86, 0.10,
                              0.09, QStringLiteral("sprint"), {QStringLiteral("KEY_LEFT_CTRL")},
                              QString::fromUtf8("Ctrl：疾跑（也可以双击前进）"),
                              QStringLiteral("left"), QStringLiteral("pill"));
    } else if (name == QLatin1String("pvp")) {              // _pvp
        layout.name = QString::fromUtf8("对战");
        layout.description = QString::fromUtf8("PVP 向：疾跑、副手、盾牌、切视角都放手指边");
        buttons << makeButton(QStringLiteral("sprint"), QString::fromUtf8("疾跑"), 0.30, 0.86, 0.10,
                              0.09, QStringLiteral("sprint"), {QStringLiteral("KEY_LEFT_CTRL")},
                              QString::fromUtf8("Ctrl 疾跑：追击/逃跑必备"),
                              QStringLiteral("left"), QStringLiteral("pill"));
        buttons << makeButton(QStringLiteral("swap"), QString::fromUtf8("副手"), 0.74, 0.62, 0.08,
                              0.10, QStringLiteral("swap_offhand"), {QStringLiteral("KEY_F")},
                              QString::fromUtf8("F：把主手物品换到副手（盾牌/火把常用）"));
        buttons << makeButton(QStringLiteral("shield"), QString::fromUtf8("盾"), 0.62, 0.72, 0.09,
                              0.14, QStringLiteral("use"), {QStringLiteral("MOUSE_RIGHT")},
                              QString::fromUtf8("右键举盾（副手放盾牌）"), QStringLiteral("right"),
                              QStringLiteral("round"), QString(), {}, QString(), {}, QString(), {},
                              0.55, QStringLiteral("use"));
        buttons << makeButton(QStringLiteral("inventory"), QString::fromUtf8("背包"), 0.62, 0.58,
                              0.09, 0.12, QStringLiteral("inventory"), {QStringLiteral("KEY_E")},
                              QString::fromUtf8("E：背包"));
        buttons << makeButton(QStringLiteral("perspective"), QString::fromUtf8("视角"), 0.50, 0.90,
                              0.10, 0.07, QStringLiteral("perspective"),
                              {QStringLiteral("KEY_F5")},
                              QString::fromUtf8("F5：切换第一/第三人称（PVP 看背后很有用）"),
                              QStringLiteral("center"), QStringLiteral("pill"));
    } else if (name == QLatin1String("one_hand")) {         // _one_hand
        layout.name = QString::fromUtf8("单手");
        layout.screen = QStringLiteral("portrait");
        layout.description = QString::fromUtf8("单手模式：所有按键集中在右下，走路用触摸板");
        buttons = {
            makeButton(QStringLiteral("jump"), QString::fromUtf8("跳"), 0.86, 0.84, 0.11, 0.12,
                       QStringLiteral("jump"), {QStringLiteral("KEY_SPACE")},
                       QString::fromUtf8("空格：跳")),
            makeButton(QStringLiteral("mine"), QString::fromUtf8("挖"), 0.72, 0.84, 0.11, 0.12,
                       QStringLiteral("attack"), {QStringLiteral("MOUSE_LEFT")},
                       QString::fromUtf8("左键：挖/打（按住连续）")),
            makeButton(QStringLiteral("place"), QString::fromUtf8("放"), 0.86, 0.68, 0.11, 0.12,
                       QStringLiteral("use"), {QStringLiteral("MOUSE_RIGHT")},
                       QString::fromUtf8("右键：放方块/使用")),
            makeButton(QStringLiteral("sneak"), QString::fromUtf8("潜行"), 0.72, 0.68, 0.11, 0.12,
                       QStringLiteral("sneak"), {QStringLiteral("KEY_LEFT_SHIFT")},
                       QString::fromUtf8("Shift：潜行"), QStringLiteral("right"),
                       QStringLiteral("pill")),
            makeButton(QStringLiteral("inventory"), QString::fromUtf8("背包"), 0.86, 0.52, 0.11,
                       0.12, QStringLiteral("inventory"), {QStringLiteral("KEY_E")},
                       QString::fromUtf8("E：背包")),
        };
        layout.directions = {makeMove(QStringLiteral("portrait"))};
        return layout;
    } else {                                                // _minimal(未知名字的回落)
        layout.name = QString::fromUtf8("极简");
        layout.description = QString::fromUtf8("只留四个必用键，屏幕最干净（新手先用这套）");
        buttons << makeButton(QStringLiteral("inventory"), QString::fromUtf8("背包"), 0.62, 0.62,
                              0.09, 0.14, QStringLiteral("inventory"), {QStringLiteral("KEY_E")},
                              QString::fromUtf8("E：背包"));
    }
    layout.buttons = buttons;
    layout.directions = {makeMove(screen)};
    return layout;
}

QStringList presetNames() {                                 // presets.py:221-222
    return {QStringLiteral("minimal"), QStringLiteral("survival"), QStringLiteral("building"),
            QStringLiteral("pvp"), QStringLiteral("one_hand")};
}

QString presetLabel(const QString &name) {                  // presets.py:212-218
    if (name == QLatin1String("minimal")) return QString::fromUtf8("极简（推荐新手）");
    if (name == QLatin1String("survival")) return QString::fromUtf8("生存");
    if (name == QLatin1String("building")) return QString::fromUtf8("建造");
    if (name == QLatin1String("pvp")) return QString::fromUtf8("对战");
    if (name == QLatin1String("one_hand")) return QString::fromUtf8("单手");
    return name;
}

// ---- guide.py ----
struct QPair3 { const char *action; const char *title; const char *instruction; const char *why; };

const QPair3 kLessons[] = {                                 // guide.py:43-59
    {"forward", "会走路", "推住左边的摇杆/方向键往前走", "键盘上是 W。推到底会快走"},
    {"jump", "会跳", "点一下「跳」", "键盘是空格。长按可以连续跳，跳一格高的坎不用手忙脚乱"},
    {"attack", "会挖方块/打怪", "长按「挖」，对着方块/怪物", "键盘是鼠标左键。长按=一直挖，别一下一下点，手指会累"},
    {"use", "会放方块/用东西", "点一下「放」", "键盘是鼠标右键。开门、喂动物、吃食物都是它"},
    {"sneak", "不会掉下去", "按住「潜行」再走到方块边缘", "键盘是 Shift。边缘不会掉下去，搭桥必备；双击可以切成常驻潜行"},
    {"inventory", "会开背包", "点一下「背包」", "键盘是 E。长按一般是聊天"},
    {"drop", "会丢东西", "点一下「丢弃」", "键盘是 Q，小心别把好东西扔了"},
    {"sprint", "会疾跑", "按住「疾跑」同时往前推", "键盘是 Ctrl；也可以双击前进方向"},
    {"perspective", "会切视角", "点一下「视角」", "键盘是 F5，PVP 时看背后很有用"},
    {"swap_offhand", "会用副手", "点一下「副手」", "键盘是 F，把盾牌/火把塞到副手"},
    {"chat", "会发消息", "长按「背包」", "键盘是 T"},
    {"fly_toggle", "会飞", "双击「升」", "创造模式双击空格开始/停止飞行"},
    {"hotbar_1", "会选快捷栏", "点数字键切物品", "键盘就是 1-9，建造时最常用"},
};

QVector<KGuideStep> buildGuide(const KLayout &layout) {      // guide.py:97-141
    QMap<QString, QPair<QString, QStringList>> found;        // action -> (control_id, keys)
    for (const KControl &c : layout.buttons) {
        for (const auto &ev : c.events) {
            if (!ev.second.action.isEmpty() && !found.contains(ev.second.action))
                found.insert(ev.second.action, {c.id, ev.second.keys});
        }
    }
    if (!layout.directions.isEmpty()) {
        const KControl &d = layout.directions.first();
        if (!found.contains(QStringLiteral("forward")))
            found.insert(QStringLiteral("forward"),
                         {d.id, {d.dirKeys.value(QStringLiteral("up"), QStringLiteral("KEY_W"))}});
    }

    const struct { const char *action; bool optional; } order[] = {  // guide.py:82-94
        {"forward", false}, {"jump", false}, {"attack", false}, {"use", false},
        {"sneak", false},   {"inventory", false}, {"sprint", true}, {"drop", true},
        {"swap_offhand", true}, {"perspective", true}, {"fly_toggle", true},
    };

    QVector<KGuideStep> steps;
    for (const auto &entry : order) {
        const QString action = QLatin1String(entry.action);
        if (!found.contains(action)) continue;
        KGuideStep step;
        step.order = steps.size() + 1;
        QString title = action, instruction = QStringLiteral("试一下「%1」").arg(action), why;
        for (const QPair3 &lesson : kLessons) {
            if (action == QLatin1String(lesson.action)) {
                title = QString::fromUtf8(lesson.title);
                instruction = QString::fromUtf8(lesson.instruction);
                why = QString::fromUtf8(lesson.why);
                break;
            }
        }
        step.title = title;
        step.instruction = instruction;
        step.why = why;
        step.controlId = found.value(action).first;
        step.keys = found.value(action).second;
        step.optional = entry.optional;
        steps.append(step);
    }

    QStringList hotbarIds;                                   // guide.py:120-130
    for (const KControl &c : layout.buttons)
        if (c.id.startsWith(QLatin1String("slot"))) hotbarIds << c.id;
    if (!hotbarIds.isEmpty()) {
        QVector<int> numbers;
        for (const QString &cid : hotbarIds)
            numbers.append(cid.mid(4).toInt());
        std::sort(numbers.begin(), numbers.end());
        KGuideStep step;
        step.order = steps.size() + 1;
        step.title = QString::fromUtf8("会换物品");
        step.instruction = QStringLiteral("点底部 %1-%2 切换手上的物品")
                               .arg(numbers.first()).arg(numbers.last());
        step.why = QString::fromUtf8("键盘就是数字键 1-9；建造时不用开背包翻");
        step.controlId = hotbarIds.first();
        step.keys = {QStringLiteral("KEY_%1").arg(numbers.first())};
        step.optional = true;
        steps.append(step);
    }

    if (!layout.directions.isEmpty()) {                      // guide.py:132-140
        KGuideStep step;
        step.order = steps.size() + 1;
        step.title = QString::fromUtf8("会看四周");
        step.instruction =
            QString::fromUtf8("在屏幕空白处滑动 = 转动视角；两根手指滑动 = 移动（如果布局开了双区）");
        step.why = QString::fromUtf8("这是 FCL 里最容易卡住的一步：很多人以为要点了按钮才能转身");
        step.optional = true;
        steps.append(step);
    }
    return steps;
}

QString guideToMarkdown(const KLayout &layout) {             // guide.py:144-155
    const QVector<KGuideStep> steps = buildGuide(layout);
    QStringList lines;
    lines << QStringLiteral("### 「%1」按键教学（%2）").arg(layout.name, layout.screen) << QString();
    for (const KGuideStep &step : steps) {
        const QString flag = step.optional ? QString::fromUtf8("（可选）") : QString();
        lines << QStringLiteral("%1. **%2**%3：%4").arg(step.order).arg(step.title).arg(flag).arg(step.instruction);
        if (!step.why.isEmpty()) lines << QStringLiteral("   - ") + step.why;
        if (!step.keys.isEmpty())
            lines << QStringLiteral("   - 等价键盘：") + step.keys.join(QStringLiteral(" + "));
    }
    return lines.join(QStringLiteral("\n"));
}

// ---- 布局存取(model.py:363-389 的 to_dict/from_dict;真正的落盘在下面的 C 桥接里) ----
QJsonObject layoutToJson(const KLayout &layout) {
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("sxcl.keymap.v1"));
    root.insert(QStringLiteral("name"), layout.name);
    root.insert(QStringLiteral("screen"), layout.screen);
    root.insert(QStringLiteral("mc_version"), QString());
    root.insert(QStringLiteral("description"), layout.description);
    root.insert(QStringLiteral("meta"), layout.meta);      // model.py:to_dict() 里的 meta
    auto control = [](const KControl &c) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), c.id);
        o.insert(QStringLiteral("label"), c.label);
        o.insert(QStringLiteral("hint"), c.hint);
        o.insert(QStringLiteral("icon"), c.icon);
        o.insert(QStringLiteral("x"), c.x);
        o.insert(QStringLiteral("y"), c.y);
        o.insert(QStringLiteral("w"), c.w);
        o.insert(QStringLiteral("h"), c.h);
        o.insert(QStringLiteral("opacity"), c.opacity);
        o.insert(QStringLiteral("group"), c.group);
        if (c.isDirection) {
            o.insert(QStringLiteral("style"), c.style);
            o.insert(QStringLiteral("dead_zone"), c.deadZone);
            QJsonObject keys;
            for (auto it = c.dirKeys.constBegin(); it != c.dirKeys.constEnd(); ++it)
                keys.insert(it.key(), it.value());
            o.insert(QStringLiteral("keys"), keys);
            o.insert(QStringLiteral("sprint_key"), QStringLiteral("KEY_LEFT_SHIFT"));
        } else {
            o.insert(QStringLiteral("shape"), c.shape);
            o.insert(QStringLiteral("alias_of"), c.aliasOf);
            QJsonObject events;
            for (const auto &ev : c.events) {
                QJsonObject b;
                b.insert(QStringLiteral("action"), ev.second.action);
                b.insert(QStringLiteral("keys"), QJsonArray::fromStringList(ev.second.keys));
                b.insert(QStringLiteral("behavior"), ev.second.behavior);
                events.insert(ev.first, b);
            }
            o.insert(QStringLiteral("events"), events);
        }
        return o;
    };
    QJsonArray buttons, directions;
    for (const KControl &b : layout.buttons) buttons.append(control(b));
    for (const KControl &d : layout.directions) directions.append(control(d));
    root.insert(QStringLiteral("buttons"), buttons);
    root.insert(QStringLiteral("directions"), directions);
    return root;
}


double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

// model.py:378-389 KeymapLayout.from_dict(+ 两个控件各自的 from_dict)
bool layoutFromJson(const QJsonObject &root, KLayout *out) {
    if (!root.contains(QStringLiteral("name")) && !root.contains(QStringLiteral("buttons"))
        && !root.contains(QStringLiteral("directions")))
        return false;
    KLayout layout;
    layout.name = root.value(QStringLiteral("name")).toString(QStringLiteral("未命名"));
    layout.screen = root.value(QStringLiteral("screen")).toString(QStringLiteral("landscape"));
    layout.description = root.value(QStringLiteral("description")).toString();
    layout.meta = root.value(QStringLiteral("meta")).toObject();   // model.py:from_dict() 的 meta
    for (const QJsonValue &value : root.value(QStringLiteral("buttons")).toArray()) {
        const QJsonObject o = value.toObject();
        KControl control;
        control.id = o.value(QStringLiteral("id")).toString();
        control.label = o.value(QStringLiteral("label")).toString();
        control.hint = o.value(QStringLiteral("hint")).toString();
        control.icon = o.value(QStringLiteral("icon")).toString();
        control.x = clampd(o.value(QStringLiteral("x")).toDouble(0.1), 0.0, 1.0);
        control.y = clampd(o.value(QStringLiteral("y")).toDouble(0.6), 0.0, 1.0);
        control.w = clampd(o.value(QStringLiteral("w")).toDouble(0.09), 0.01, 1.0);
        control.h = clampd(o.value(QStringLiteral("h")).toDouble(0.16), 0.01, 1.0);
        const QString shape = o.value(QStringLiteral("shape")).toString(QStringLiteral("round"));
        control.shape = (shape == QLatin1String("round") || shape == QLatin1String("square")
                         || shape == QLatin1String("pill"))
                            ? shape : QStringLiteral("round");
        control.opacity = clampd(o.value(QStringLiteral("opacity")).toDouble(0.55), 0.0, 1.0);
        control.group = o.value(QStringLiteral("group")).toString();
        control.aliasOf = o.value(QStringLiteral("alias_of")).toString();
        const QJsonObject events = o.value(QStringLiteral("events")).toObject();
        for (const char *kind : {"press", "long_press", "click", "double_click"}) {
            const QJsonObject b = events.value(QLatin1String(kind)).toObject();
            if (b.isEmpty()) continue;
            KBinding binding;
            binding.action = b.value(QStringLiteral("action")).toString();
            for (const QJsonValue &k : b.value(QStringLiteral("keys")).toArray())
                binding.keys << k.toString();
            const QString behavior = b.value(QStringLiteral("behavior")).toString(QStringLiteral("hold"));
            binding.behavior = (behavior == QLatin1String("hold") || behavior == QLatin1String("toggle")
                                || behavior == QLatin1String("tap"))
                                   ? behavior : QStringLiteral("hold");
            control.bind(QLatin1String(kind), binding);
        }
        layout.buttons.append(control);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("directions")).toArray()) {
        const QJsonObject o = value.toObject();
        KControl control;
        control.isDirection = true;
        control.id = o.value(QStringLiteral("id")).toString();
        control.label = o.value(QStringLiteral("label")).toString();
        control.hint = o.value(QStringLiteral("hint")).toString();
        control.x = clampd(o.value(QStringLiteral("x")).toDouble(0.03), 0.0, 1.0);
        control.y = clampd(o.value(QStringLiteral("y")).toDouble(0.55), 0.0, 1.0);
        control.w = clampd(o.value(QStringLiteral("w")).toDouble(0.22), 0.02, 1.0);
        control.h = clampd(o.value(QStringLiteral("h")).toDouble(0.38), 0.02, 1.0);
        const QString style = o.value(QStringLiteral("style")).toString(QStringLiteral("dpad_compact"));
        control.style = (style == QLatin1String("dpad") || style == QLatin1String("rocker")
                         || style == QLatin1String("dpad_compact"))
                            ? style : QStringLiteral("dpad_compact");
        control.opacity = clampd(o.value(QStringLiteral("opacity")).toDouble(0.5), 0.0, 1.0);
        control.deadZone = clampd(o.value(QStringLiteral("dead_zone")).toDouble(0.18), 0.0, 0.6);
        control.group = o.value(QStringLiteral("group")).toString(QStringLiteral("left"));
        const QJsonObject keys = o.value(QStringLiteral("keys")).toObject();
        control.dirKeys = {{QStringLiteral("up"), keys.value(QStringLiteral("up")).toString(QStringLiteral("KEY_W"))},
                           {QStringLiteral("down"), keys.value(QStringLiteral("down")).toString(QStringLiteral("KEY_S"))},
                           {QStringLiteral("left"), keys.value(QStringLiteral("left")).toString(QStringLiteral("KEY_A"))},
                           {QStringLiteral("right"), keys.value(QStringLiteral("right")).toString(QStringLiteral("KEY_D"))}};
        layout.directions.append(control);
    }
    *out = layout;
    return true;
}


// ---- C 核心库桥接(include/sxcl/keymap.h) ----
// 这一页的画布用的是 Qt 侧的 KLayout;真正"落地"的动作(写 keymaps/、写 active.json、
// 读 FCL 布局)一律交给 C 核心库 —— 它与 Python 的 store.py / fcl.py 是同一套语义,
// 页面不自己拼文件名、不自己写 active.json,中间只过一层 JSON:
//   存:KLayout -> layoutToJson -> sxcl_keymap_parse ------------------> sxcl_keymap_store_save
//   读:sxcl_keymap_store_load / sxcl_keymap_import_fcl -> to_json -> layoutFromJson -> KLayout
QString coreError(const char *err)
{
    return (err && err[0]) ? QString::fromUtf8(err) : QString::fromUtf8("未知错误");
}

// KLayout -> JSON 文本 -> C 的 sxcl_keymap_layout(用完要 sxcl_keymap_layout_free)。
bool toCoreLayout(const KLayout &layout, sxcl_keymap_layout *out, QString *error)
{
    const QByteArray text = QJsonDocument(layoutToJson(layout)).toJson(QJsonDocument::Compact);
    char err[256];
    err[0] = '\0';
    sxcl_keymap_issues issues;
    sxcl_keymap_issues_reset(&issues);
    if (sxcl_keymap_parse(text.constData(), static_cast<size_t>(text.size()), out, &issues, err,
                          sizeof err) != SXCL_KEYMAP_OK) {
        *error = coreError(err);
        return false;
    }
    return true;
}

// C 的 sxcl_keymap_layout -> JSON 文本 -> KLayout。
bool fromCoreLayout(const sxcl_keymap_layout *core, KLayout *out, QString *error)
{
    char *text = nullptr;
    char err[256];
    err[0] = '\0';
    if (sxcl_keymap_to_json(core, &text, err, sizeof err) != SXCL_KEYMAP_OK) {
        *error = coreError(err);
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(text), &parseError);
    std::free(text);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()
        || !layoutFromJson(doc.object(), out)) {
        *error = QString::fromUtf8("核心库给出的 JSON 读不回来");
        return false;
    }
    return true;
}

// keymaps/ 里**用户自己存的**布局(keymap_page.py:339-343 _reload_list 用的那份清单;
// 内置预设已经在"预设"那一组里了,这里跳过)。
QVector<QPair<QString, QString>> userStoreLayouts()
{
    QVector<QPair<QString, QString>> out;
    sxcl_keymap_store_catalog catalog;
    char err[256];
    err[0] = '\0';
    if (sxcl_keymap_store_list("", &catalog, err, sizeof err) != SXCL_KEYMAP_OK) {
        return out;
    }
    for (size_t i = 0; i < catalog.count; ++i) {
        if (catalog.items[i].builtin) {
            continue;
        }
        out.append({QString::fromUtf8(catalog.items[i].key),
                    QString::fromUtf8(catalog.items[i].label)});
    }
    return out;
}


// ================================================================
// 二、BasePage —— 对应 Python src/app/common/base_page.py
// ================================================================
class PageShell : public ScrollArea {
public:
    PageShell(const QString &title, const QString &subtitle, const QString &objectName,
              QWidget *parent)
        : ScrollArea(parent) {
        setObjectName(objectName);
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

        m_view = new QWidget(this);
        m_view->setStyleSheet(QStringLiteral("background: transparent;"));
        setWidget(m_view);

        m_box = new QVBoxLayout(m_view);
        m_box->setContentsMargins(28, 24, 28, 24);
        m_box->setSpacing(16);
        m_box->setAlignment(Qt::AlignTop);

        m_title = new TitleLabel(title, m_view);
        m_subtitle = new SubtitleLabel(subtitle, m_view);
        m_subtitle->setTextColor(QColor(0x60, 0x60, 0x60), QColor(0xAA, 0xAA, 0xAA));
        m_box->addWidget(m_title);
        m_box->addWidget(m_subtitle);
    }
    QWidget *view() const { return m_view; }
    QVBoxLayout *box() const { return m_box; }
    void addContent(QWidget *w) { m_box->addWidget(w); }
    void addStretch() { m_box->addStretch(1); }

private:
    QWidget *m_view = nullptr;
    QVBoxLayout *m_box = nullptr;
    TitleLabel *m_title = nullptr;
    SubtitleLabel *m_subtitle = nullptr;
};

// ================================================================
// 三、KeymapCanvas —— 对应 keymap_page.py:80-218
// ================================================================
class KeymapCanvas : public QWidget {
public:
    explicit KeymapCanvas(QWidget *parent) : QWidget(parent) {
        m_layout = buildPreset(QStringLiteral("minimal"), QStringLiteral("landscape"));
        setMinimumSize(520, 320);                     // :91
        setMouseTracking(true);                       // :92
        m_blinkTimer.setInterval(450);                // :94-96
        QObject::connect(&m_blinkTimer, &QTimer::timeout, this, [this] { onBlink(); });
        // Python :97 on_theme_changed(self.update)
        fluent::FluentStyle::instance()->subscribe([this] { update(); });
    }

    void setLayoutData(const KLayout &layout, const QString &highlight = QString()) { // :99-102
        m_layout = layout;
        m_highlightId = highlight;
        update();
    }
    KLayout layoutData() const { return m_layout; }

    void highlight(const QString &controlId) {         // :104-111
        m_highlightId = controlId;
        m_blink = !controlId.isEmpty();
        if (!controlId.isEmpty()) m_blinkTimer.start();
        else m_blinkTimer.stop();
        update();
    }

    std::function<void()> changed;                     // Python changed 信号

protected:
    void paintEvent(QPaintEvent *) override {          // :169-218
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), token(QStringLiteral("bg")));   // :173

        const QRectF phone = phoneRect();                        // :175
        painter.setPen(QPen(token(QStringLiteral("borderStrong")), 2)); // :176
        painter.setBrush(QBrush(token(QStringLiteral("card"))));        // :177
        painter.drawRoundedRect(phone, 14, 14);                         // :178
        painter.setPen(QPen(token(QStringLiteral("textTertiary"))));    // :179
        QFont screenFont(painter.font().family(), 8);                   // :180
        painter.setFont(screenFont);
        const QPair<int, int> ratio = screenRatio(m_layout.screen);     // :182
        painter.drawText(QRectF(phone.left(), phone.top() - 16, phone.width(), 14),
                         Qt::AlignCenter,
                         QStringLiteral("%1  (%2, %3)")
                             .arg(m_layout.screen).arg(ratio.first).arg(ratio.second));

        for (const KControl &control : m_layout.controls()) {           // :184
            const QRectF rect = toScreen(control.x, control.y, control.w, control.h);
            const bool highlighted = control.id == m_highlightId && m_blink; // :186
            const QColor accent = token(QStringLiteral("accent"));      // :187
            const QColor border = highlighted ? accent : token(QStringLiteral("borderStrong")); // :188
            QColor fill(accent);                                        // :189
            fill.setAlpha(highlighted ? 90
                                      : int(255 * std::min(0.75, control.opacity))); // :190
            painter.setPen(QPen(border, highlighted ? 3 : 1.4));        // :191
            painter.setBrush(QBrush(fill));                             // :192
            if (control.shape == QLatin1String("round"))                // :193
                painter.drawEllipse(rect);
            else
                painter.drawRoundedRect(rect, 8, 8);                    // :196

            painter.setPen(QPen(token(QStringLiteral("text"))));        // :198
            QFont font = painter.font();                                // :199
            font.setPointSizeF(std::max(7.5, std::min(11.0, rect.height() / 3.2))); // :200
            font.setBold(highlighted);                                  // :201
            painter.setFont(font);                                      // :202
            const QString label = control.label.isEmpty() ? control.id : control.label; // :203
            if (control.isDirection) {                                  // :204
                painter.drawText(QRectF(rect.left(), rect.top(), rect.width(), rect.height() / 2),
                                 Qt::AlignCenter, label);               // :205-206
                const QPointF center = rect.center();                   // :207
                const double knob = std::min(rect.width(), rect.height() * 0.5) * 0.22; // :209
                painter.setBrush(QBrush(token(QStringLiteral("bg"))));  // :210
                painter.setPen(QPen(token(QStringLiteral("borderStrong")), 1.2)); // :211
                painter.drawEllipse(center, knob * 1.6, knob * 1.6);    // :212
                painter.setBrush(QBrush(accent));                       // :213
                painter.setPen(Qt::NoPen);                              // :214
                painter.drawEllipse(center, knob, knob);                // :215
            } else {
                painter.drawText(rect, Qt::AlignCenter, label);         // :217
            }
        }
        painter.end();                                                  // :218
    }

    void mousePressEvent(QMouseEvent *event) override {                 // :141-148
        KControl control;
        if (!controlAt(event->position(), &control)) return;
        m_dragging = control;
        m_hasDragging = true;
        const QRectF phone = phoneRect();
        m_dragOffset = QPointF(event->position().x() / phone.width() - control.x,
                               event->position().y() / phone.height() - control.y);
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override {                  // :150-160
        if (!m_hasDragging) return;
        const QRectF phone = phoneRect();
        if (phone.width() <= 0 || phone.height() <= 0) return;
        const double x = event->position().x() / phone.width() - m_dragOffset.x();
        const double y = event->position().y() / phone.height() - m_dragOffset.y();
        m_dragging.x = std::max(0.0, std::min(1.0 - m_dragging.w, x));
        m_dragging.y = std::max(0.0, std::min(1.0 - m_dragging.h, y));
        update();
    }

    void mouseReleaseEvent(QMouseEvent *) override {                    // :162-165
        if (!m_hasDragging) return;
        m_hasDragging = false;
        // 把拖动结果写回布局(Python 直接改的是同一个 ControlButton 对象)
        for (KControl &c : m_layout.buttons)
            if (c.id == m_dragging.id) c.x = m_dragging.x, c.y = m_dragging.y;
        for (KControl &c : m_layout.directions)
            if (c.id == m_dragging.id) c.x = m_dragging.x, c.y = m_dragging.y;
        if (changed) changed();
    }

private:
    static QPair<int, int> screenRatio(const QString &screen) {         // :77
        if (screen == QLatin1String("portrait")) return {9, 16};
        return {16, 9};
    }
    QRectF phoneRect() const {                                          // :119-126
        const QPair<int, int> ratio = screenRatio(m_layout.screen);
        const int margin = 18;
        const double availableW = std::max(80, width() - margin * 2);
        const double availableH = std::max(80, height() - margin * 2);
        const double scale = std::min(availableW / ratio.first, availableH / ratio.second);
        const double w = ratio.first * scale;
        const double h = ratio.second * scale;
        return QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h);
    }
    QRectF toScreen(double x, double y, double w, double h) const {     // :128-131
        const QRectF phone = phoneRect();
        return QRectF(phone.left() + x * phone.width(), phone.top() + y * phone.height(),
                      w * phone.width(), h * phone.height());
    }
    // 返回副本:Python 的 _control_at 返回的是布局里的对象引用,但拖动期间只用到
    // id/x/y/w/h,按值返回等价且不会悬空。
    bool controlAt(const QPointF &pos, KControl *out) const {           // :135-139
        for (const KControl &control : m_layout.controls()) {
            if (toScreen(control.x, control.y, control.w, control.h).contains(pos)) {
                *out = control;
                return true;
            }
        }
        return false;
    }
    void onBlink() {                                                    // :113-115
        m_blink = !m_blink;
        update();
    }
    static QColor token(const QString &name) {
        return ThemeBridge::instance().token(name);
    }

    KLayout m_layout;
    QString m_highlightId;
    bool m_blink = false;
    QTimer m_blinkTimer;
    bool m_hasDragging = false;
    KControl m_dragging;
    QPointF m_dragOffset;
};

} // namespace

// ================================================================
// 四、页面 —— 对应 keymap_page.py:221-436
// ================================================================
QWidget *createKeymapPage(QWidget *parent) {
    auto *page = new PageShell(
        QString::fromUtf8("按键映射"),
        QString::fromUtf8("给手机端用的虚拟按键布局；可在此编排、检查冲突、导出教学"),
        QStringLiteral("KeymapPage"), parent);              // :225

    QWidget *view = page->view();

    // ---- toolbar(:236-258) ----
    auto *toolbar = new QWidget(view);
    auto *row = new QHBoxLayout(toolbar);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);

    // keymap_page.py:226 ensure_presets() —— 首次运行时把 5 套内置预设落到 keymaps/
    // (手机端拿到这个目录就能直接选)。写不进去不拦着用页面:预设照样能现算、能导出,
    // 真点"保存"的时候核心库会把具体原因报出来(不吞错、也不假装成功)。
    {
        char err[256];
        err[0] = '\0';
        (void)sxcl_keymap_store_ensure_presets("", 0, nullptr, err, sizeof err);
    }

    auto *presetCombo = new ComboBox(toolbar);              // :241
    for (const QString &name : presetNames())               // :242-243
        presetCombo->addItem(presetLabel(name), name);
    // keymap_page.py:339-343 _reload_list() —— 自己存过的布局也放进下拉框
    for (const QPair<QString, QString> &entry : userStoreLayouts()) {
        if (presetCombo->findData(entry.first) < 0) {
            presetCombo->addItem(entry.second, entry.first);
        }
    }

    auto *screenCombo = new ComboBox(toolbar);              // :246
    screenCombo->addItems({QString::fromUtf8("横屏"), QString::fromUtf8("竖屏")}); // :247
    screenCombo->setFixedWidth(90);                         // :248

    auto *search = new SearchLineEdit(toolbar);             // :251
    search->setPlaceholderText(
        QString::fromUtf8("这个键在哪？输入 潜行 / jump / E …"));   // :252

    row->addWidget(new BodyLabel(QString::fromUtf8("预设"), toolbar)); // :255
    row->addWidget(presetCombo);                            // :256
    row->addWidget(screenCombo);                            // :257
    row->addWidget(search, 1);                              // :258

    auto *canvas = new KeymapCanvas(view);                  // :260
    auto *canvasCard = new CardWidget(view);                // :309
    auto *canvasCardLayout = new QVBoxLayout(canvasCard);   // :310
    canvasCardLayout->setContentsMargins(8, 8, 8, 8);       // :311
    canvasCardLayout->addWidget(canvas);                    // :312

    auto *conflictList = new QListWidget(view);             // :263
    conflictList->setMinimumHeight(110);                    // :264
    auto *guideList = new QListWidget(view);                // :267
    // Python 的 QListWidget 由 src/app/theme.py:global_qss() 的这条规则上色
    // (C 版的 appStyleSheet 只下发 FluentWindow 那条,所以这里按同一份规则就地上色,
    //  值仍全部取自令牌,不新增颜色)。
    const QString listQss =
        QStringLiteral("QListView, QListWidget, QTreeView, QTableView {"
                       " background: transparent; color: %1; border: none;"
                       " selection-background-color: %2; selection-color: %1; }")
            .arg(FluentTheme::instance().tokenText(QStringLiteral("text")),
                 FluentTheme::instance().tokenText(QStringLiteral("hoverStrong")));
    conflictList->setStyleSheet(listQss);
    guideList->setStyleSheet(listQss);

    const QColor danger = ThemeBridge::instance().token(QStringLiteral("danger"));
    const QColor warning = ThemeBridge::instance().token(QStringLiteral("warning"));

    // 刷新分析结果(:345-367)
    auto refreshAnalysis = [canvas, conflictList, guideList, danger, warning] {
        const KLayout layout = canvas->layoutData();
        conflictList->clear();
        const QStringList errors = layout.validate();
        const QVector<KConflict> conflicts = layout.conflicts();
        for (const QString &message : errors) {
            auto *item = new QListWidgetItem(QStringLiteral("✗ ") + message);
            item->setForeground(QBrush(danger));
            conflictList->addItem(item);
        }
        for (const KConflict &conflict : conflicts) {
            auto *item = new QListWidgetItem(QStringLiteral("⚠ ") + conflict.message);
            item->setForeground(QBrush(warning));
            item->setData(Qt::UserRole, conflict.controls.isEmpty() ? QString()
                                                                    : conflict.controls.first());
            conflictList->addItem(item);
        }
        if (errors.isEmpty() && conflicts.isEmpty())
            conflictList->addItem(
                new QListWidgetItem(QString::fromUtf8("✓ 没有发现问题，可以直接用")));

        guideList->clear();
        for (const KGuideStep &step : buildGuide(layout)) {
            const QString flag = step.optional ? QString::fromUtf8("（可选）") : QString();
            auto *item = new QListWidgetItem(QStringLiteral("%1. %2%3 — %4")
                                                 .arg(step.order).arg(step.title)
                                                 .arg(flag).arg(step.instruction));
            item->setData(Qt::UserRole, step.controlId);
            guideList->addItem(item);
        }
    };

    // 当前这份布局的 key(keymap_page.py 的 self._current_key):"设为当前布局"就是把它
    // 写进 active.json(store.py 的 set_active 会去掉 preset- 前缀,安卓端读的也是这一份)。
    auto currentKey = std::make_shared<QString>(QStringLiteral("preset-minimal"));

    // 应用预设(:318-337)
    std::function<void(const QString &)> applyPreset;
    applyPreset = [page, canvas, screenCombo, refreshAnalysis, currentKey](const QString &keyIn) {
        const QString key = keyIn.isEmpty() ? QStringLiteral("minimal") : keyIn;
        const QString screen = screenCombo->currentIndex() == 1 ? QStringLiteral("portrait")
                                                                : QStringLiteral("landscape");
        // Python: load_layout_by_key(key) or build_preset(key, screen) —— 先读 keymaps/ 里
        // 存的那份(预设名会先 ensure_presets 落盘),读不到才现算;屏幕方向对不上再重算。
        KLayout layout;
        bool loaded = false;
        {
            sxcl_keymap_layout core;
            sxcl_keymap_issues issues;
            sxcl_keymap_issues_reset(&issues);
            char err[256];
            err[0] = '\0';
            if (sxcl_keymap_store_load("", key.toUtf8().constData(), &core, &issues, err, sizeof err)
                == SXCL_KEYMAP_OK) {
                QString error;
                loaded = fromCoreLayout(&core, &layout, &error);
                sxcl_keymap_layout_free(&core);
            }
        }
        if (!loaded) layout = buildPreset(key, screen);
        if (layout.screen != screen) layout = buildPreset(key, screen);
        *currentKey = QStringLiteral("preset-") + key;
        if (layout.screen != screen) {
            // 有些预设自带屏幕方向(单手模式固定竖屏),下拉框跟着它走
            screenCombo->blockSignals(true);
            screenCombo->setCurrentIndex(layout.screen == QLatin1String("portrait") ? 1 : 0);
            screenCombo->blockSignals(false);
            const QPair<int, int> ratio = layout.screen == QLatin1String("portrait")
                                              ? QPair<int, int>{9, 16} : QPair<int, int>{16, 9};
            InfoBar::push(InfoBar::Type::Info,
                          QString::fromUtf8("这个预设固定屏幕方向"),
                          QStringLiteral("「%1」按(%2, %3)比例设计，已自动切过去")
                              .arg(layout.name).arg(ratio.first).arg(ratio.second),
                          page, 2500);
        }
        canvas->setLayoutData(layout);
        refreshAnalysis();
    };

    // Python :244-245 / :249-250 —— 两个下拉框都调 _apply_preset(currentData())
    QObject::connect(presetCombo, &QComboBox::currentIndexChanged, page,
                     [presetCombo, applyPreset] { applyPreset(presetCombo->currentData().toString()); });
    QObject::connect(screenCombo, &QComboBox::currentIndexChanged, page,
                     [presetCombo, applyPreset] { applyPreset(presetCombo->currentData().toString()); });
    canvas->changed = refreshAnalysis;                     // :261 canvas.changed -> _refresh_analysis

    // 搜索(:369-374)
    QObject::connect(search, &QLineEdit::textChanged, page, [canvas, page](const QString &text) {
        const QStringList hits = canvas->layoutData().find(text);
        canvas->highlight(hits.isEmpty() ? QString() : hits.first());
        if (!text.isEmpty() && hits.isEmpty()) {
            InfoBar::push(InfoBar::Type::Info, QString::fromUtf8("没有找到"),
                          QStringLiteral("没有控件匹配「%1」").arg(text), page, 2500);
        }
    });

    // 点列表项 -> 高亮画布上的控件(:265-269)
    QObject::connect(conflictList, &QListWidget::itemClicked, page, [canvas](QListWidgetItem *item) {
        canvas->highlight(item->data(Qt::UserRole).toString());
    });
    QObject::connect(guideList, &QListWidget::itemClicked, page, [canvas](QListWidgetItem *item) {
        canvas->highlight(item->data(Qt::UserRole).toString());
    });

    // ---- side(:271-276) ----
    auto *side = new QVBoxLayout;
    side->setSpacing(8);
    side->addWidget(new StrongBodyLabel(QString::fromUtf8("冲突检查"), view));
    side->addWidget(conflictList);
    side->addWidget(new StrongBodyLabel(QString::fromUtf8("新手教学（可导出给手机端）"), view));
    side->addWidget(guideList);

    // ---- body(:278-281) ----
    auto *body = new QHBoxLayout;
    body->setSpacing(16);
    body->addWidget(canvas, 3);
    body->addLayout(side, 2);

    // ---- buttons(:283-300) ----
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    auto *saveBtn = new PrimaryPushButton(QString::fromUtf8("保存为我的布局"), view);
    auto *activeBtn = new PushButton(QString::fromUtf8("设为当前布局"), view);
    auto *exportBtn = new PushButton(QString::fromUtf8("导出 JSON"), view);
    auto *importBtn = new PushButton(QString::fromUtf8("导入 JSON"), view);
    auto *fclBtn = new PushButton(QString::fromUtf8("导入 FCL 布局"), view);
    auto *guideBtn = new PushButton(QString::fromUtf8("导出教学 Markdown"), view);
    for (QWidget *w : {static_cast<QWidget *>(saveBtn), static_cast<QWidget *>(activeBtn),
                       static_cast<QWidget *>(exportBtn), static_cast<QWidget *>(importBtn),
                       static_cast<QWidget *>(fclBtn), static_cast<QWidget *>(guideBtn)})
        buttons->addWidget(w);
    buttons->addStretch(1);

    page->addContent(toolbar);                              // :302
    page->addContent(canvasCard);                           // :303
    page->box()->addLayout(body);                           // :304
    page->box()->addLayout(buttons);                        // :305
    page->addStretch();                                     // :306

    // ---- 动作(:378-436) ----
    // 保存为我的布局(:378-381 _on_save):存到 store.py 定的 keymaps/ 目录,key 用布局名。
    QObject::connect(saveBtn, &QAbstractButton::clicked, page,
                     [page, canvas, presetCombo, currentKey] {
        sxcl_keymap_layout core;
        QString error;
        if (!toCoreLayout(canvas->layoutData(), &core, &error)) {
            InfoBar::push(InfoBar::Type::Error, QString::fromUtf8("保存失败"), error, page, 5000);
            return;
        }
        char path[640];
        char err[256];
        err[0] = '\0';
        // key 传空 = 用布局名(store.py 的 save(layout, key="")),重名直接覆盖,
        // 文件名安全化(中文可用、标点过掉)也由核心库负责,页面不自己拼路径。
        const int rc = sxcl_keymap_store_save("", &core, nullptr, path, sizeof path, err, sizeof err);
        sxcl_keymap_layout_free(&core);
        if (rc != SXCL_KEYMAP_OK) {
            InfoBar::push(InfoBar::Type::Error, QString::fromUtf8("保存失败"), coreError(err), page,
                          5000);
            return;
        }
        const QFileInfo info(QString::fromUtf8(path));
        InfoBar::push(InfoBar::Type::Success, QString::fromUtf8("已保存"),
                      QString::fromUtf8("布局已存到 %1").arg(info.fileName()), page, 3000);
        // 存完把这份布局加进下拉框并选中它(Python 要重开页面才看得到,这里就地补上),
        // 屏蔽信号 —— 画布上刚调好的东西不该再从文件里读回来覆盖一遍。
        const QString key = info.completeBaseName();
        presetCombo->blockSignals(true);
        int index = presetCombo->findData(key);
        if (index < 0) {
            presetCombo->addItem(key, key);
            index = presetCombo->count() - 1;
        }
        presetCombo->setCurrentIndex(index);
        presetCombo->blockSignals(false);
        *currentKey = QStringLiteral("preset-") + key;
    });
    // 设为当前布局(:383-386 _on_set_active):写 active.json,安卓端启动时读它。
    QObject::connect(activeBtn, &QAbstractButton::clicked, page, [page, canvas, currentKey] {
        char path[640];
        char err[256];
        err[0] = '\0';
        const QByteArray key = currentKey->toUtf8();
        if (sxcl_keymap_store_set_active("", key.constData(), path, sizeof path, err, sizeof err)
            != SXCL_KEYMAP_OK) {
            InfoBar::push(InfoBar::Type::Error, QString::fromUtf8("设为当前失败"), coreError(err), page,
                          5000);
            return;
        }
        InfoBar::push(InfoBar::Type::Success, QString::fromUtf8("已设为当前"),
                      QString::fromUtf8("手机端启动时会读取这个布局"), page, 3000);
        // Python 不检查就写;这里多一句提醒(核心库的校验与手机端的冲突检查是同一套口径),
        // 但不拦着 —— active.json 已经写成功了,如实说清楚就行。
        sxcl_keymap_layout core;
        QString error;
        if (toCoreLayout(canvas->layoutData(), &core, &error)) {
            sxcl_keymap_issues issues;
            sxcl_keymap_issues_reset(&issues);
            const size_t errors = sxcl_keymap_validate(&core, &issues);
            sxcl_keymap_layout_free(&core);
            if (errors > 0) {
                InfoBar::push(InfoBar::Type::Warning, QString::fromUtf8("这份布局还有问题"),
                              QString::fromUtf8("有 %1 处 ✗（见左边冲突检查），手机端也会照单全收")
                                  .arg(errors),
                              page, 4000);
            }
        }
    });
    QObject::connect(exportBtn, &QAbstractButton::clicked, page, [page, canvas] {
        const KLayout layout = canvas->layoutData();
        const QString path = QFileDialog::getSaveFileName(
            page, QString::fromUtf8("导出按键布局"),
            layout.name + QStringLiteral(".keymap.json"),
            QString::fromUtf8("按键布局 (*.json)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return;
        file.write(QJsonDocument(layoutToJson(layout)).toJson(QJsonDocument::Indented));
        file.commit();
        InfoBar::push(InfoBar::Type::Success, QString::fromUtf8("已导出"), path, page, 3000);
    });
    QObject::connect(importBtn, &QAbstractButton::clicked, page, [page, canvas, refreshAnalysis] {
        const QString path = QFileDialog::getOpenFileName(
            page, QString::fromUtf8("导入按键布局"), QString(),
            QString::fromUtf8("按键布局 (*.json)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            InfoBar::push(InfoBar::Type::Error, QString::fromUtf8("导入失败"),
                          err.errorString(), page, 5000);
            return;
        }
        KLayout layout;
        if (!layoutFromJson(doc.object(), &layout)) {
            InfoBar::push(InfoBar::Type::Error, QString::fromUtf8("导入失败"),
                          QString::fromUtf8("不是按键布局 JSON"), page, 5000);
            return;
        }
        canvas->setLayoutData(layout);
        refreshAnalysis();
        InfoBar::push(InfoBar::Type::Success, QString::fromUtf8("已导入"), layout.name, page, 3000);
    });
    // 导入 FCL 布局(:412-427 _on_import_fcl):核心库认 FCL 的像素坐标 + GLFW 键码,
    // 归一化与"认不出的字段塞 meta.fcl_raw"都在 fcl.py 的对应实现里做。
    QObject::connect(fclBtn, &QAbstractButton::clicked, page, [page, canvas, refreshAnalysis] {
        const QString path = QFileDialog::getOpenFileName(
            page, QString::fromUtf8("选择 FCL 布局文件"), QString(),
            QString::fromUtf8("JSON (*.json)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            InfoBar::push(InfoBar::Type::Error, QString::fromUtf8("导入失败"), file.errorString(),
                          page, 5000);
            return;
        }
        const QByteArray data = file.readAll();
        sxcl_keymap_layout core;
        sxcl_keymap_issues issues;
        sxcl_keymap_issues_reset(&issues);
        char err[256];
        err[0] = '\0';
        // 2400x1080 是 fcl.py import_layout() 的默认屏幕尺寸(照抄,不自己选)。
        if (sxcl_keymap_import_fcl(data.constData(), static_cast<size_t>(data.size()), nullptr, 2400,
                                   1080, &core, &issues, err, sizeof err) != SXCL_KEYMAP_OK) {
            InfoBar::push(InfoBar::Type::Error, QString::fromUtf8("导入失败"), coreError(err), page,
                          5000);
            return;
        }
        const int buttons = static_cast<int>(core.button_count);
        KLayout layout;
        QString error;
        const bool ok = fromCoreLayout(&core, &layout, &error);
        sxcl_keymap_layout_free(&core);
        if (!ok) {
            InfoBar::push(InfoBar::Type::Error, QString::fromUtf8("导入失败"), error, page, 5000);
            return;
        }
        canvas->setLayoutData(layout);
        refreshAnalysis();
        InfoBar::push(InfoBar::Type::Success, QString::fromUtf8("已从 FCL 导入"),
                      QString::fromUtf8("%1 个按钮，坐标已按屏幕归一化").arg(buttons), page, 4000);
    });
    QObject::connect(guideBtn, &QAbstractButton::clicked, page, [page, canvas] {
        const KLayout layout = canvas->layoutData();
        const QString path = QFileDialog::getSaveFileName(
            page, QString::fromUtf8("导出教学"),
            layout.name + QString::fromUtf8("-教学.md"), QString::fromUtf8("Markdown (*.md)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&file);
        out << guideToMarkdown(layout);
        file.commit();
        InfoBar::push(InfoBar::Type::Success, QString::fromUtf8("已导出教学"), path, page, 3000);
    });

    // :231。Python 这里传的是 currentText()(下拉框的显示名),认不出预设名就回落到极简;
    // C 版改用 currentData()(真正的 key),开局同样是"极简",但意图更直白。
    applyPreset(presetCombo->currentData().toString());
    return page;
}

} // namespace sxcl::ui
