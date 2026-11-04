#include "AppSettings.h"
#include "RecordModel.h"
#include "ServerLayout.h"
#include <QtTest>
#include <QAccessible>
#include <QFile>
#include <QDir>
#include <QInputMethodEvent>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <memory>

// Only the image primitive is substituted: all rail controls, delegates,
// pointer handlers and the persisted layout are the production components.
// Generated fixture icons deliberately cannot make a network request.
class SyntheticGuildIcon : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(qreal radius MEMBER m_radius NOTIFY sourceChanged)
    Q_PROPERTY(int decodeSize MEMBER m_decodeSize NOTIFY sourceChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY sourceChanged)
public:
    using QQuickPaintedItem::QQuickPaintedItem;
    QUrl source() const { return m_source; }
    void setSource(const QUrl &value) { if (value == m_source) return; m_source = value; update(); emit sourceChanged(); }
    bool ready() const { return !m_source.isEmpty(); }
    void paint(QPainter *painter) override {
        if (!ready()) return;
        painter->setRenderHint(QPainter::Antialiasing);
        const int n = m_source.toString().section(':', -1).toInt();
        painter->setPen(Qt::NoPen); painter->setBrush(QColor::fromHsv((n * 43) % 360, 140, 190));
        painter->drawRoundedRect(boundingRect(), m_radius, m_radius);
        painter->setPen(Qt::white); QFont font; font.setPixelSize(qMax(9, int(height() * .4))); font.setBold(true); painter->setFont(font);
        painter->drawText(boundingRect(), Qt::AlignCenter, QString::number(n + 1));
    }
signals:
    void sourceChanged();
private:
    QUrl m_source;
    qreal m_radius = 12;
    int m_decodeSize = 32;
};

class SyntheticDmInbox final : public QObject {
    Q_OBJECT
public:
    explicit SyntheticDmInbox(RecordModel *rows) : model(rows) {}
    RecordModel *model;
    int activations = 0;
    QString lastChannel;
    Q_INVOKABLE void activate(const QString &id) { ++activations; lastChannel = id; model->remove(id); }
};

class SyntheticReadingState final : public QObject {
    Q_OBJECT
public:
    QList<bool> states;
    Q_INVOKABLE void setReadingActive(bool active) { states.append(active); }
};

#ifndef Q_MOC_RUN
static const char *fixtureQml = R"(
import QtQuick
import QtQuick.Controls
ApplicationWindow {
    width: 560; height: 660; visible: true; color: "#1a1a1e"
    property string selectedGuild: "1002"
    property string selectedChannel: ""
    property string lastHover: ""
    property string lastDismiss: ""
    property string lastContext: ""
    property string lastDmContext: ""
    property var hoverAnchor: null
    ServerRail {
        id: rail; anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
        width: preferences.serverRailWidth; layoutModel: fixtureLayout; currentGuildId: selectedGuild
        unreadDmModel: fixtureUnreadDms; currentChannelId: selectedChannel
        onDmActivated: channelId => { selectedGuild = ""; selectedChannel = channelId; fixtureDmInbox.activate(channelId) }
        onDmContextRequested: channelId => lastDmContext = channelId
        onGuildActivated: guildId => selectedGuild = guildId
        onHomeActivated: selectedGuild = ""
        onGuildContextRequested: (guildId, name) => lastContext = guildId
        onHoverPrepared: (guildId, anchor) => { lastHover = guildId; hoverAnchor = anchor }
        onHoverDismissed: guildId => lastDismiss = guildId
    }
    Column {
        anchors.left: rail.right; anchors.leftMargin: 24; anchors.top: parent.top; anchors.topMargin: 30; spacing: 18
        Text { text: "Generated communities"; color: "#f1f1f3"; font.pixelSize: 22 * preferences.fontScale }
        Text { text: "Synthetic server rail fixture\nNo account or network connection"; color: "#a9a9b3"; font.pixelSize: 13 * preferences.fontScale }
        Text { text: selectedGuild ? "Selected: " + selectedGuild : "Direct messages"; color: "#a9a9b3"; font.pixelSize: 13 * preferences.fontScale }
    }
})";
#endif

class ServerRailTest final : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> settings;
    std::unique_ptr<AppSettings> prefs;
    std::unique_ptr<RecordModel> guilds;
    std::unique_ptr<RecordModel> unreadDms;
    std::unique_ptr<SyntheticDmInbox> inbox;
    std::unique_ptr<ServerLayout> layout;
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QQuickWindow> window;
    QQuickItem *rail = nullptr, *list = nullptr;
    QList<QQmlError> warnings;

    QQuickItem *find(QQuickItem *parent, const QString &name) const {
        if (parent->objectName() == name) return parent;
        for (auto *child : parent->childItems()) if (auto *result = find(child, name)) return result;
        return nullptr;
    }
    QQuickItem *item(const QString &name) const { return find(window->contentItem(), name); }
    QPoint center(QQuickItem *value, qreal fraction = .5) const { return value->mapToScene(QPointF(value->width() / 2, value->height() * fraction)).toPoint(); }
    QQuickItem *button(const QString &id) const { return item((id.startsWith("folder:") ? "folderToggle_" : "railButton_") + id); }
    void screenshot(const QString &name) { const auto directory=qEnvironmentVariable("RELAY_RAIL_SCREENSHOTS");if(!directory.isEmpty()){QVERIFY(QDir().mkpath(directory));QVERIFY(window->grabWindow().save(directory+"/"+name+".png"));} }
    QStringList ids() const { QStringList result; for (int i = 0; i < layout->count(); ++i) result << layout->get(i).value("id").toString(); return result; }
    void load() {
        engine = std::make_unique<QQmlEngine>();
        engine->rootContext()->setContextProperty("preferences", prefs.get());
        engine->rootContext()->setContextProperty("fixtureLayout", layout.get());
        engine->rootContext()->setContextProperty("fixtureUnreadDms", unreadDms.get());
        engine->rootContext()->setContextProperty("fixtureDmInbox", inbox.get());
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const auto &values) { warnings += values; });
        const QString path = QFINDTESTDATA("../qml/components/ServerRail.qml");
        QVERIFY(!path.isEmpty());
        QQmlComponent component(engine.get());
        component.setData(fixtureQml, QUrl::fromLocalFile(QFileInfo(path).absolutePath() + "/ServerRailHarness.qml"));
        auto *created = component.create();
        QVERIFY2(created, qPrintable(component.errorString()));
        window.reset(qobject_cast<QQuickWindow *>(created));
        QVERIFY(window); QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        QTest::qWait(40);
        if (!qEnvironmentVariableIsEmpty("RELAY_RAIL_REQUIRE_GPU")) { const auto api=window->rendererInterface()->graphicsApi();QVERIFY(api!=QSGRendererInterface::Software&&api!=QSGRendererInterface::Unknown); }
        rail = item("serverRail"); list = item("serverRailList"); QVERIFY(rail); QVERIFY(list); QVERIFY(button("1001"));
    }
    void startDrag(const QString &id) {
        auto *source = button(id); QVERIFY(source);
        const auto from = center(source);
        QTest::mousePress(window.get(), Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(window.get(), from + QPoint(0, 18), 20);
        QTRY_VERIFY_WITH_TIMEOUT(rail->property("dragging").toBool(), 600);
    }
    void moveDrag(const QPoint &point) {
        QTest::mouseMove(window.get(), point, 20);
        QCoreApplication::processEvents();
    }
    void dropOn(const QString &sourceId, const QString &targetId, qreal fraction, bool group = false) {
        startDrag(sourceId); QVERIFY(rail->property("dragging").toBool());
        auto *target = item("railEntry_" + targetId); QVERIFY(target);
        const auto to = center(target, fraction); moveDrag(to);
        if (group) QTRY_COMPARE_WITH_TIMEOUT(rail->property("dropPlacement").toString(), QString("into"), 800);
        QTest::mouseRelease(window.get(), Qt::LeftButton, Qt::NoModifier, to);
        QTRY_VERIFY_WITH_TIMEOUT(!rail->property("dragging").toBool(), 600);
        QTest::qWait(35);
    }
    void openArrangement(const QString &id) {
        auto *control = button(id); QVERIFY(control); control->forceActiveFocus();
        QTest::keyClick(window.get(), Qt::Key_Menu);
        auto *popup = rail->findChild<QObject *>("serverArrangementMenu"); QVERIFY(popup);
        QTRY_VERIFY_WITH_TIMEOUT(popup->property("visible").toBool(), 600);
        QTest::qWait(30);
    }
    QQuickItem *menuItem(QQuickItem *parent, const QString &text) const {
        if (parent->property("text").toString() == text && parent->isVisible() && parent->isEnabled()) return parent;
        for (auto *child : parent->childItems()) if (auto *result = menuItem(child, text)) return result;
        return nullptr;
    }
    void activateMenu(const QString &text) {
        auto *control = menuItem(window->contentItem(), text); QVERIFY2(control, qPrintable(text));
        QTest::mouseClick(window.get(), Qt::LeftButton, Qt::NoModifier, center(control)); QTest::qWait(35);
    }
private slots:
    void initTestCase() { qmlRegisterType<SyntheticGuildIcon>("Relay.Native", 1, 0, "CrispImage"); }
    void init() {
        settings = std::make_unique<QTemporaryDir>(); QVERIFY(settings->isValid());
        prefs = std::make_unique<AppSettings>(settings->path() + "/prefs.ini"); prefs->setReducedMotion(true);
        guilds = std::make_unique<RecordModel>(); QVector<QVariantMap> rows;
        for (int n = 0; n < 40; ++n) rows.append({{"recordId", QString::number(1001+n)}, {"name", QString("Community %1").arg(n+1)}, {"initials", QString::number(n+1)}, {"iconUrl", QString("fixture:%1").arg(n)}, {"unread", n == 0 || n == 3}, {"voiceActive", n == 0}, {"streaming", n == 2}});
        guilds->replace(rows); layout = std::make_unique<ServerLayout>(guilds.get(), settings->path() + "/layout.ini"); layout->setAccountKey("90000001"); warnings.clear();
        unreadDms = std::make_unique<RecordModel>(); inbox = std::make_unique<SyntheticDmInbox>(unreadDms.get());
    }
    void cleanup() {
        window.reset(); engine.reset(); layout.reset(); guilds.reset(); inbox.reset(); unreadDms.reset(); prefs.reset(); settings.reset(); rail = list = nullptr;
        QStringList messages; for (const auto &warning : warnings) messages << warning.toString();
        QVERIFY2(warnings.isEmpty(), qPrintable(messages.join('\n')));
    }
    void selectionHoverBadgesAndAccessibleNames() {
        load();
        auto *guild = button("1001"); QVERIFY(guild);
        auto *accessible = QAccessible::queryAccessibleInterface(guild); QVERIFY(accessible); QCOMPARE(accessible->text(QAccessible::Name), "Community 1");
        QVERIFY(item("railUnreadBadge_1001")->isVisible()); QVERIFY(item("railVoiceBadge_1001")->isVisible()); QVERIFY(item("railVoiceBadge_1003")->isVisible());
        auto *indicator = item("railIndicator_1002"); QVERIFY(indicator); QCOMPARE(indicator->height(), 36.);
        QTest::mouseMove(window.get(), center(guild)); QTRY_COMPARE(window->property("lastHover").toString(), QString("1001"));
        QCOMPARE(window->property("hoverAnchor").value<QObject *>(), guild); QCOMPARE(item("railIndicator_1001")->height(), 20.);
        QTest::mouseClick(window.get(), Qt::LeftButton, Qt::NoModifier, center(guild));
        QTRY_COMPARE(window->property("selectedGuild").toString(), QString("1001")); QVERIFY(!item("railUnreadBadge_1001")->isVisible());
        QTest::mouseClick(window.get(), Qt::RightButton, Qt::NoModifier, center(button("1003"))); QTRY_COMPARE(window->property("lastContext").toString(), QString("1003"));
        QTest::mouseMove(window.get(), {300, 250}); QTRY_COMPARE(window->property("lastDismiss").toString(), QString("1003"));
        QTest::mouseClick(window.get(), Qt::LeftButton, Qt::NoModifier, center(item("railHome"))); QTRY_COMPARE(window->property("selectedGuild").toString(), QString());
    }
    void unreadDmAvatarsOpenClearAndStayAboveHome() {
        unreadDms->replace({{{"recordId","6001"},{"name","A <b>friend</b>"},{"initials","AF"},{"avatarUrl","fixture:50"},{"unread",1}},
                           {{"recordId","6002"},{"name","Group friends"},{"initials","GF"},{"kind","dm"},{"unread",5}}});
        load();auto *dms=item("railUnreadDms"),*home=item("railHome");QVERIFY(dms&&home);QVERIFY(dms->isVisible());QVERIFY(dms->mapToScene({0,dms->height()}).y()<home->mapToScene({0,0}).y());
        auto *person=item("railDmButton_6001");QVERIFY(person);auto *accessible=QAccessible::queryAccessibleInterface(person);QVERIFY(accessible);QCOMPARE(accessible->text(QAccessible::Name),QString("A <b>friend</b>, 1 unread message"));
        QCOMPARE(item("railDmUnreadCount_6001")->property("text").toString(),QString("1"));QVERIFY(item("railDmUnreadBadge_6001")->isVisible());
        unreadDms->patch("6001",{{"unread",123}});QTRY_COMPARE(item("railDmUnreadCount_6001")->property("text").toString(),QString("99+"));
        screenshot("rail-unread-normal");
        QTest::mouseMove(window.get(),center(person));QTRY_COMPARE(item("railDmIndicator_6001")->height(),20.);
        QTRY_VERIFY_WITH_TIMEOUT(item("railDmTooltipText_6001")&&item("railDmTooltipText_6001")->isVisible(),900);
        QCOMPARE(item("railDmTooltipText_6001")->property("textFormat").toInt(),0);QVERIFY(item("railDmTooltipText_6001")->property("text").toString().contains("<b>friend</b>"));
        prefs->setFontScale(1.4);unreadDms->patch("6001",{{"name",QString("A <b>friend</b> with a very long synthetic group chat name that wraps clearly without leaving the window")}});QTest::qWait(40);QVERIFY(item("railDmTooltipText_6001")->width()<=320);QVERIFY(item("railDmTooltipText_6001")->height()>25);screenshot("rail-unread-tooltip-140");
        QTest::mouseClick(window.get(),Qt::LeftButton,Qt::NoModifier,center(person));QTRY_COMPARE(inbox->activations,1);QCOMPARE(inbox->lastChannel,QString("6001"));QCOMPARE(unreadDms->count(),1);QCOMPARE(window->property("selectedChannel").toString(),QString("6001"));QVERIFY(!rail->property("dragging").toBool());
        auto *group=item("railDmButton_6002");QVERIFY(group);QVERIFY(item("railDmInitials_6002")->isVisible());QCOMPARE(item("railDmInitials_6002")->property("text").toString(),QString("GF"));
        group->forceActiveFocus();QTest::keyClick(window.get(),Qt::Key_Space);QTRY_COMPARE(inbox->activations,2);QCOMPARE(inbox->lastChannel,QString("6002"));QTRY_VERIFY(!dms->isVisible());QVERIFY(home->isVisible());QVERIFY(button("1001")->isVisible());
    }
    void unreadDmContextDoesNotOpenOrMarkRead() {
        unreadDms->replace({{{"recordId","6001"},{"name","Generated friend"},{"recipientId","9001"},{"unread",3}}});
        load();auto *dm=item("railDmButton_6001");QVERIFY(dm);
        const auto selected=window->property("selectedGuild");
        QTest::mouseClick(window.get(),Qt::RightButton,Qt::NoModifier,center(dm));
        QTRY_COMPARE(window->property("lastDmContext").toString(),QString("6001"));
        QCOMPARE(inbox->activations,0);QCOMPARE(unreadDms->count(),1);QCOMPARE(unreadDms->get(0).value("unread").toInt(),3);
        QCOMPARE(window->property("selectedGuild"),selected);QVERIFY(!rail->property("dragging").toBool());
        window->setProperty("lastDmContext",QString{});dm->forceActiveFocus();QTest::keyClick(window.get(),Qt::Key_Menu);
        QTRY_COMPARE(window->property("lastDmContext").toString(),QString("6001"));
        window->setProperty("lastDmContext",QString{});QTest::keyClick(window.get(),Qt::Key_F10,Qt::ShiftModifier);
        QTRY_COMPARE(window->property("lastDmContext").toString(),QString("6001"));QCOMPARE(inbox->activations,0);
    }
    void mainReadingBridgeRequiresVisibleFocusedUncoveredConversation() {
        QFile source(QFINDTESTDATA("../qml/Main.qml"));QVERIFY(source.open(QIODevice::ReadOnly));const auto main=QString::fromUtf8(source.readAll());
        const auto match=QRegularExpression("readonly property bool readingConversation:\\s*(.*?)\\n\\s*onReadingConversationChanged",QRegularExpression::DotMatchesEverythingOption).match(main);QVERIFY(match.hasMatch());
        QVERIFY(main.contains("onReadingConversationChanged: discord.setReadingActive(readingConversation)"));QVERIFY(main.contains("Component.onCompleted: discord.setReadingActive(readingConversation)"));
        SyntheticReadingState recorder;QQmlEngine localEngine;localEngine.rootContext()->setContextProperty("discord",&recorder);connect(&localEngine,&QQmlEngine::warnings,this,[this](const auto &values){warnings+=values;});
        const QString harness=QString(R"(
import QtQuick
import QtQuick.Window
Item {
    property QtObject window: QtObject { property bool active: false; property bool visible: true; property int visibility: Window.Windowed; property bool workspaceVisible: true; property bool streamCoveringChat: false; property bool socialVisible: false; property bool conversationModalOpen: false }
    property QtObject messages: QtObject { property bool visible: true }
    property QtObject mediaViewer: QtObject { property bool visible: false }
    property QtObject switcher: QtObject { property bool visible: false }
    property QtObject searchPins: QtObject { property bool visible: false }
    readonly property bool readingConversation: %1
    onReadingConversationChanged: discord.setReadingActive(readingConversation)
    Component.onCompleted: discord.setReadingActive(readingConversation)
})").arg(match.captured(1));
        QQmlComponent component(&localEngine);component.setData(harness.toUtf8(),QUrl("file:///synthetic-reading-state.qml"));std::unique_ptr<QObject> created(component.create());QVERIFY2(created,qPrintable(component.errorString()));
        QVERIFY(!recorder.states.isEmpty());QCOMPARE(recorder.states.first(),false);
        auto *focus=created->property("window").value<QObject*>(),*messages=created->property("messages").value<QObject*>(),*viewer=created->property("mediaViewer").value<QObject*>(),*switcher=created->property("switcher").value<QObject*>();QVERIFY(focus&&messages&&viewer&&switcher);
        focus->setProperty("active",true);QCOMPARE(recorder.states.last(),true);
        viewer->setProperty("visible",true);QCOMPARE(recorder.states.last(),false);
        switcher->setProperty("visible",true);viewer->setProperty("visible",false);QCOMPARE(recorder.states.last(),false);
        switcher->setProperty("visible",false);QCOMPARE(recorder.states.last(),true);
        focus->setProperty("conversationModalOpen",true);QCOMPARE(recorder.states.last(),false);
        focus->setProperty("conversationModalOpen",false);QCOMPARE(recorder.states.last(),true);
        auto *search=created->property("searchPins").value<QObject*>();QVERIFY(search);
        search->setProperty("visible",true);QCOMPARE(recorder.states.last(),false);
        search->setProperty("visible",false);QCOMPARE(recorder.states.last(),true);
        focus->setProperty("socialVisible",true);QCOMPARE(recorder.states.last(),false);
        focus->setProperty("socialVisible",false);QCOMPARE(recorder.states.last(),true);
        focus->setProperty("streamCoveringChat",true);QCOMPARE(recorder.states.last(),false);
        messages->setProperty("visible",false);focus->setProperty("streamCoveringChat",false);QCOMPARE(recorder.states.last(),false);
        messages->setProperty("visible",true);QCOMPARE(recorder.states.last(),true);
        focus->setProperty("visibility",int(QWindow::Minimized));QCOMPARE(recorder.states.last(),false);
        focus->setProperty("visibility",int(QWindow::Windowed));QCOMPARE(recorder.states.last(),true);
        focus->setProperty("visible",false);QCOMPARE(recorder.states.last(),false);
        focus->setProperty("workspaceVisible",false);focus->setProperty("visible",true);QCOMPARE(recorder.states.last(),false);
        focus->setProperty("workspaceVisible",true);QCOMPARE(recorder.states.last(),true);
        focus->setProperty("active",false);QCOMPARE(recorder.states.last(),false);
    }
    void manyUnreadDmsStayBoundedScrollableAndDoNotAffectGuildDrag() {
        QVector<QVariantMap> rows;for(int n=0;n<30;++n)rows.append({{"recordId",QString::number(6101+n)},{"name",QString("Synthetic chat %1").arg(n+1)},{"initials",QString::number(n+1)},{"avatarUrl",QString("fixture:%1").arg(50+n)},{"unread",n+1}});unreadDms->replace(rows);
        prefs->setFontScale(1.4);prefs->setServerRailWidth(56);load();window->resize(420,280);QTest::qWait(80);
        auto *dms=item("railUnreadDms"),*home=item("railHome");QVERIFY(dms&&home);QVERIFY(dms->height()<=rail->height()*.30+1);QVERIFY(home->mapToScene({0,0}).y()>dms->mapToScene({0,dms->height()}).y());QVERIFY(list->height()>=rail->property("rowHeight").toReal());
        screenshot("rail-unread-short-56-140");
        auto *first=item("railDmButton_6101");QVERIFY(first);const auto beforeDrag=ids();const auto dmStart=center(first);QTest::mousePress(window.get(),Qt::LeftButton,Qt::NoModifier,dmStart);QTest::mouseMove(window.get(),dmStart+QPoint(80,80),40);QVERIFY(!rail->property("dragging").toBool());QTest::mouseRelease(window.get(),Qt::LeftButton,Qt::NoModifier,dmStart+QPoint(80,80));QCOMPARE(ids(),beforeDrag);QCOMPARE(inbox->activations,0);
        first->forceActiveFocus();for(int n=0;n<12;++n)QTest::keyClick(window.get(),Qt::Key_Down);QTRY_VERIFY(dms->property("contentY").toReal()>100.);auto *target=item("railDmButton_6113");QVERIFY(target&&target->hasActiveFocus());QTest::keyClick(window.get(),Qt::Key_Return);QTRY_COMPARE(inbox->lastChannel,QString("6113"));QVERIFY(!rail->property("dragging").toBool());
        window->resize(560,660);QTest::qWait(80);const auto selected=window->property("selectedGuild");dropOn("1001","1003",.92);QCOMPARE(ids().mid(0,3),QStringList({"1002","1003","1001"}));QCOMPARE(window->property("selectedGuild"),selected);QCOMPARE(unreadDms->count(),29);
        unreadDms->clear();QTRY_VERIFY(!dms->isVisible());QVERIFY(home->isVisible());
    }
    void realDragReordersWithoutSelecting() {
        load(); const auto selected = window->property("selectedGuild").toString();
        dropOn("1001", "1003", .92);
        QCOMPARE(ids().mid(0, 4), QStringList({"1002", "1003", "1001", "1004"})); QCOMPARE(window->property("selectedGuild").toString(), selected);
        dropOn("1001", "1002", .08);
        QCOMPARE(ids().mid(0, 3), QStringList({"1001", "1002", "1003"}));
    }
    void realDragGroupsAndKeyboardMovesOutAndUngroups() {
        load(); dropOn("1001", "1002", .5, true);
        const auto folder = layout->folderForGuild("1001"); QVERIFY(!folder.isEmpty()); QCOMPARE(layout->folderForGuild("1002"), folder);
        QVERIFY(button(folder)); QVERIFY(button("1001"));
        QTest::mouseClick(window.get(), Qt::LeftButton, Qt::NoModifier, center(button(folder))); QTRY_VERIFY(!button("1001") || !button("1001")->isVisible());
        QCOMPARE(item("railIndicator_" + folder)->height(), 36.); // Selected server remains represented in a collapsed folder.
        QTest::mouseClick(window.get(), Qt::LeftButton, Qt::NoModifier, center(button(folder))); QTRY_VERIFY(button("1001") && button("1001")->isVisible());
        button("1001")->forceActiveFocus(); QTest::keyClick(window.get(), Qt::Key_Left, Qt::ControlModifier);
        QTRY_VERIFY(layout->folderForGuild("1001").isEmpty()); QCOMPARE(layout->folderForGuild("1002"), folder);
        openArrangement(folder); activateMenu("Ungroup servers"); QTRY_VERIFY(layout->folderForGuild("1002").isEmpty()); QVERIFY(!ids().contains(folder));
    }
    void realDragAddsToExistingFolderMovesOutAndReordersFolder() {
        load(); dropOn("1001", "1002", .5, true);
        const auto folder = layout->folderForGuild("1001"); QVERIFY(!folder.isEmpty());
        dropOn("1003", folder, .5, true); QCOMPARE(layout->folderForGuild("1003"), folder);
        dropOn("1001", "1004", .92); QVERIFY(layout->folderForGuild("1001").isEmpty());
        QCOMPARE(layout->folderForGuild("1002"), folder); QCOMPARE(layout->folderForGuild("1003"), folder);
        dropOn(folder, "1004", .92); QCOMPARE(ids().first(), QString("1004"));
        QCOMPARE(ids().at(1), folder); QCOMPARE(window->property("selectedGuild").toString(), QString("1002"));
    }
    void draggingOntoCollapsedFolderDoesNotExpandOrSelectIt() {
        load(); dropOn("1001", "1002", .5, true);
        const auto folder = layout->folderForGuild("1001"); QVERIFY(!folder.isEmpty());
        QVERIFY(layout->toggleFolder(folder)); QTest::qWait(40);
        QVERIFY(!layout->get(0).value("expanded").toBool());
        const auto before = ids(); const auto selected = window->property("selectedGuild");
        startDrag("1003"); const auto target = center(item("railEntry_" + folder)); moveDrag(target);
        QCOMPARE(rail->property("dropPlacement").toString(), QString("into"));
        QTest::qWait(1000); // Holding over the folder used to expand and shift the rows.
        QCOMPARE(ids(), before); QVERIFY(!layout->get(0).value("expanded").toBool());
        QCOMPARE(window->property("selectedGuild"), selected);
        QTest::mouseRelease(window.get(), Qt::LeftButton, Qt::NoModifier, target);
        QTRY_COMPARE(layout->folderForGuild("1003"), folder);
        QVERIFY(!layout->get(0).value("expanded").toBool());
        ServerLayout restored(guilds.get(), settings->path() + "/layout.ini"); restored.setAccountKey("90000001");
        QCOMPARE(restored.folderForGuild("1003"), folder); QVERIFY(!restored.get(0).value("expanded").toBool());
        QTest::mouseClick(window.get(), Qt::LeftButton, Qt::NoModifier, center(button(folder)));
        QTRY_VERIFY(layout->get(0).value("expanded").toBool()); QVERIFY(button("1003"));
    }
    void edgeAutoscrollEscapeAndAccountSwitchCancelPendingDrop() {
        load(); const auto original = ids(); startDrag("1001");
        const QPoint bottom = list->mapToScene(QPointF(list->width() / 2, list->height() - 5)).toPoint(); moveDrag(bottom);
        QTRY_VERIFY_WITH_TIMEOUT(list->property("contentY").toReal() > 100., 1000);
        QTest::keyClick(window.get(), Qt::Key_Escape); QTRY_VERIFY(!rail->property("dragging").toBool());
        QTest::mouseRelease(window.get(), Qt::LeftButton, Qt::NoModifier, bottom); QTest::qWait(60); QCOMPARE(ids(), original);
        list->setProperty("contentY", 0.); QTest::qWait(40); startDrag("1001"); moveDrag(center(item("railEntry_1003"), .92));
        layout->setAccountKey("90000002"); QTRY_VERIFY(!rail->property("dragging").toBool());
        QTest::mouseRelease(window.get(), Qt::LeftButton, Qt::NoModifier, center(button("1003"))); QTest::qWait(60); QCOMPARE(ids(), original);
    }
    void folderEditorPersistsNameColorAndState() {
        load(); dropOn("1001", "1002", .5, true); const auto folder = layout->folderForGuild("1001"); QVERIFY(!folder.isEmpty());
        openArrangement(folder); activateMenu("Folder settings");
        QTRY_VERIFY(item("folderName") && item("folderName")->isVisible());
        QInputMethodEvent input; input.setCommitString("Games & friends"); QCoreApplication::sendEvent(window.get(), &input);
        auto *color = item("folderColor_ed4245"); QVERIFY(color); QTest::mouseClick(window.get(), Qt::LeftButton, Qt::NoModifier, center(color));
        activateMenu("Save");
        QTRY_COMPARE(layout->get(0).value("name").toString(), QString("Games & friends")); QCOMPARE(layout->get(0).value("color").toString(), QString("#ed4245"));
        QTest::mouseClick(window.get(), Qt::LeftButton, Qt::NoModifier, center(button(folder))); QTRY_VERIFY(!layout->get(0).value("expanded").toBool());
        ServerLayout restored(guilds.get(), settings->path() + "/layout.ini"); restored.setAccountKey("90000001");
        QCOMPARE(restored.get(0), layout->get(0));
    }
    void narrowAndWideRailAtLargeText() {
        prefs->setFontScale(1.4); load(); QVERIFY(layout->drop("1001", "1002", "into"));
        const auto folder = layout->folderForGuild("1001"); QVERIFY(layout->updateFolder(folder, "Friends & games", "#9b59b6")); QTest::qWait(50);
        const auto screenshots = qEnvironmentVariable("RELAY_RAIL_SCREENSHOTS");
        for (const int width : {56, 104}) {
            prefs->setServerRailWidth(width); QTest::qWait(80); QCOMPARE(rail->width(), qreal(width));
            for (const auto &id : {folder, QString("1001"), QString("1002"), QString("1003")}) {
                auto *control = button(id); QVERIFY(control); const auto left = control->mapToItem(rail, {0,0}).x();
                QVERIFY2(left >= 0 && left + control->width() <= width, qPrintable(id));
            }
            if (!screenshots.isEmpty()) { QVERIFY(QDir().mkpath(screenshots)); QVERIFY(window->grabWindow().save(screenshots + QString("/rail-%1-expanded-140.png").arg(width))); }
            QTest::mouseClick(window.get(), Qt::LeftButton, Qt::NoModifier, center(button(folder))); QTRY_VERIFY(!layout->get(0).value("expanded").toBool());
            if (!screenshots.isEmpty()) { QTest::qWait(60); QVERIFY(window->grabWindow().save(screenshots + QString("/rail-%1-collapsed-140.png").arg(width))); }
            QTest::mouseClick(window.get(), Qt::LeftButton, Qt::NoModifier, center(button(folder))); QTRY_VERIFY(layout->get(0).value("expanded").toBool());
        }
    }
};
QTEST_MAIN(ServerRailTest)
#include "server_rail_test.moc"
