// multiplayer_page.cpp —— 联机页(占位 + 方案说明)
//
// 1:1 移植 Python 版 src/app/pages/multiplayer_page.py。
// 结构逐条对照(行号指 Python 源):
//   BasePage(title="联机", subtitle="和朋友一起玩：房间码加入 / P2P 打洞 / 中继兜底（开发中）")
//     vBoxLayout: contentsMargins(28,24,28,24) / spacing 16 / AlignTop      base_page.py:51-53
//       TitleLabel(title) / SubtitleLabel(subtitle, #606060|#AAAAAA)        base_page.py:55-60
//       add_content(_status_card())                                         multiplayer_page.py:65
//       add_content(_room_card())                                           multiplayer_page.py:66
//       add_content(_notes_card())                                          multiplayer_page.py:67
//       add_stretch()                                                       multiplayer_page.py:68
//
// 三张卡片都是 qf CardWidget(自绘背景 = 白色 13/255 叠在页面底色上),
// 内边距/间距逐条照抄 Python:
//   _status_card  QVBoxLayout margins(20,16,20,16) spacing 6   :70-90
//   _room_card    QVBoxLayout margins(20,16,20,16) spacing 10  :92-129
//   _notes_card   QVBoxLayout margins(20,16,20,16) spacing 6   :131-149
#include "page_factory.h"

#include "fluent_theme.h"
#include "libqf.h"
#include "theme_bridge.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_input.h"
#include "fluent/fluent_labels.h"
#include "fluent/fluent_scroll.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>

namespace sxcl::ui {
namespace {

// ---------------------------------------------------------------- BasePage
// 对应 Python src/app/common/base_page.py:BasePage(ScrollArea 子类)。
// 只搬结构,不加任何 Python 里没有的东西。
class PageShell : public ScrollArea {
public:
    PageShell(const QString &title, const QString &subtitle, const QString &objectName,
              QWidget *parent)
        : ScrollArea(parent) {
        setObjectName(objectName);                          // base_page.py:42
        setWidgetResizable(true);                           // base_page.py:43
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // base_page.py:44

        m_view = new QWidget(this);                         // base_page.py:46
        m_view->setStyleSheet(QStringLiteral("background: transparent;")); // base_page.py:47
        setWidget(m_view);                                  // base_page.py:48

        m_box = new QVBoxLayout(m_view);                    // base_page.py:50-53
        m_box->setContentsMargins(28, 24, 28, 24);
        m_box->setSpacing(16);
        m_box->setAlignment(Qt::AlignTop);

        m_title = new TitleLabel(title, m_view);            // base_page.py:55
        m_subtitle = new SubtitleLabel(subtitle, m_view);   // base_page.py:56
        m_subtitle->setTextColor(QColor(0x60, 0x60, 0x60), QColor(0xAA, 0xAA, 0xAA)); // :57

        m_box->addWidget(m_title);                          // base_page.py:59
        m_box->addWidget(m_subtitle);                       // base_page.py:60
    }

    QWidget *view() const { return m_view; }
    void addContent(QWidget *w) { m_box->addWidget(w); }    // base_page.py:62-63
    void addStretch() { m_box->addStretch(1); }             // base_page.py:65-66

private:
    QWidget *m_view = nullptr;
    QVBoxLayout *m_box = nullptr;
    TitleLabel *m_title = nullptr;
    SubtitleLabel *m_subtitle = nullptr;
};

QString tokenText(const char *name) {
    return FluentTheme::instance().tokenText(QLatin1String(name));
}

QColor tokenColor(const char *name) { return ThemeBridge::instance().token(QLatin1String(name)); }

// Python :151-153 _not_ready():InfoBar.info(..., TOP, isClosable=True, duration=2500)
void notReady(QWidget *host) {
    InfoBar::push(InfoBar::Type::Info, QString::fromUtf8("还没上线"),
                  QString::fromUtf8("联机功能在开发中，先把位置占住 😄"), host, 2500);
}

// ---------------------------------------------------------------- _status_card
// Python :70-90 —— StrongBodyLabel(warning,15px) + BodyLabel(text_secondary, 自动换行)
CardWidget *buildStatusCard(QWidget *parent) {
    auto *card = new CardWidget(parent);                    // :71
    auto *layout = new QVBoxLayout(card);                   // :72
    layout->setContentsMargins(20, 16, 20, 16);             // :73
    layout->setSpacing(6);                                  // :74

    auto *title = new StrongBodyLabel(QString::fromUtf8("🚧 联机功能正在开发"), card); // :76
    // Python :77 title.setStyleSheet(f"color: {token('warning')}; font-size: 15px;")
    title->setStyleSheet(QStringLiteral("color: %1; font-size: 15px;")
                             .arg(tokenText("warning")));
    layout->addWidget(title);                               // :78

    auto *body = new BodyLabel(
        QString::fromUtf8("第一版会做的四件事：\n"
                          "1. 一键开房：启动器自己拉起服务端并把存档挂上，不用进游戏手动点「对局域网开放」；\n"
                          "2. 房间码：房主把 6 位码发给朋友，对方输码即进；\n"
                          "3. P2P 打洞：同一房间的成员先尝试直连，延迟最低；\n"
                          "4. 中继兜底：打洞失败（运营商 NAT）自动走服务器转发，不让任何人卡在门外。"),
        card);                                              // :80-86
    body->setWordWrap(true);                                // :87
    const QColor secondary = tokenColor("textSecondary");
    body->setTextColor(secondary, secondary);               // :88
    layout->addWidget(body);                                // :89
    return card;
}

// ---------------------------------------------------------------- _room_card
// Python :92-129 —— 房间码输入 + 加入/创建(功能没上,全部禁用)
CardWidget *buildRoomCard(QWidget *parent) {
    auto *card = new CardWidget(parent);                    // :94
    auto *layout = new QVBoxLayout(card);                   // :95
    layout->setContentsMargins(20, 16, 20, 16);             // :96
    layout->setSpacing(10);                                 // :97

    layout->addWidget(new StrongBodyLabel(QString::fromUtf8("加入房间"), card)); // :99

    auto *row = new QWidget(card);                          // :101
    auto *rowLayout = new QHBoxLayout(row);                 // :102
    rowLayout->setContentsMargins(0, 0, 0, 0);              // :103
    rowLayout->setSpacing(8);                               // :104

    auto *roomInput = new LineEdit(row);                    // :106
    roomInput->setPlaceholderText(QString::fromUtf8("输入 6 位房间码，例如 A1B2C3")); // :107
    roomInput->setMaxLength(6);                             // :108
    roomInput->setEnabled(false);                           // :109
    rowLayout->addWidget(roomInput, 1);                     // :110

    auto *joinBtn = new PrimaryPushButton(QString::fromUtf8("加入房间"), row); // :112
    joinBtn->setEnabled(false);                             // :113
    QObject::connect(joinBtn, &QAbstractButton::clicked, joinBtn,
                     [joinBtn] { notReady(joinBtn->window()); }); // :114
    rowLayout->addWidget(joinBtn);                          // :115

    auto *hostBtn = new PushButton(QString::fromUtf8("创建房间（我来当主机）"), row); // :117
    hostBtn->setEnabled(false);                             // :118
    QObject::connect(hostBtn, &QAbstractButton::clicked, hostBtn,
                     [hostBtn] { notReady(hostBtn->window()); }); // :119
    rowLayout->addWidget(hostBtn);                          // :120

    layout->addWidget(row);                                 // :122

    auto *hint = new BodyLabel(
        QString::fromUtf8("当主机的机器要自己跑服务端，对单核性能和上行带宽有要求（开房前会先做一次体检）"),
        card);                                              // :124-125
    hint->setWordWrap(true);                                // :126
    const QColor tertiary = tokenColor("textTertiary");
    hint->setTextColor(tertiary, tertiary);                 // :127
    layout->addWidget(hint);                                // :128
    return card;
}

// ---------------------------------------------------------------- _notes_card
// Python :131-149 —— 一条标题 + 四条「· 」说明
CardWidget *buildNotesCard(QWidget *parent) {
    auto *card = new CardWidget(parent);                    // :132
    auto *layout = new QVBoxLayout(card);                   // :133
    layout->setContentsMargins(20, 16, 20, 16);             // :134
    layout->setSpacing(6);                                  // :135

    layout->addWidget(
        new StrongBodyLabel(QString::fromUtf8("几个已经定下来的设计"), card)); // :137

    const char *kNotes[] = {                                // :138-143
        "星型拓扑：Minecraft 的客户端之间不直连，所有数据都过服务端 —— 所以联机一定有一个人当主机。",
        "主机可以不是玩家的电脑：后续会支持把存档推到服务器上开房（云端主机），掉线也不会散伙。",
        "手机端只做加入方：安卓跑服务端会发热降频、被后台杀掉，体验很差，所以不提供「手机当主机」。",
        "兼容局域网：同一个 WiFi 下直接连，不打洞、不走中继，最稳也最快。",
    };
    const QColor secondary = tokenColor("textSecondary");
    for (const char *note : kNotes) {                       // :144-148
        auto *item = new BodyLabel(QString::fromUtf8("· ") + QString::fromUtf8(note), card);
        item->setWordWrap(true);
        item->setTextColor(secondary, secondary);
        layout->addWidget(item);
    }
    return card;
}


} // namespace

QWidget *createMultiplayerPage(QWidget *parent) {
    auto *page = new PageShell(
        QString::fromUtf8("联机"),
        QString::fromUtf8("和朋友一起玩：房间码加入 / P2P 打洞 / 中继兜底（开发中）"),
        QStringLiteral("MultiplayerPage"), parent);         // :57-59

    page->addContent(buildStatusCard(page->view()));        // :65
    page->addContent(buildRoomCard(page->view()));          // :66
    page->addContent(buildNotesCard(page->view()));         // :67
    page->addStretch();                                     // :68
    return page;
}

} // namespace sxcl::ui
