#include <QtTest>
#include <QTemporaryDir>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickStyle>
#include "RelayPeers.h"
#include "RelayStreams.h"
#include "BroadcastEngine.h"
#include "SoftwareVideo.h"
#include <QFileInfo>
#include <QUuid>
#include <QImage>

class GeneratedRelayConnections final : public QObject {
    Q_OBJECT
public:
    bool enabled=false; int requests=0;
    Q_INVOKABLE QVariantMap conversation(const QString &) const { return {{"enabled",enabled},{"pending",false},{"status",enabled ? "Invitation sent · waiting for them" : ""}}; }
    Q_INVOKABLE void setConversationEnabled(const QString &,const QString &,bool value) { enabled=value;++requests;emit changed(); }
signals:
    void changed();
};
class RelayPeerUiTest : public QObject {
    Q_OBJECT
private slots:
    void directViewerAndWatchButtonRender() {
        QTemporaryDir root;RelayPeers peers(root.path()+"/profile",{}),friendPeer(root.path()+"/friend",{});
        const auto friendId=peers.addContact(friendPeer.contactCard(),"Morgan");
        BroadcastEngine broadcast;RelayStreams streams(&peers,&broadcast);
        peers.streamControlReceived(friendId,{{"v",1},{"type","available"},{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"name","Generated OBS stream"}});
        peers.setSelectedPeer(friendId);
        QQmlEngine engine;engine.rootContext()->setContextProperty("relayPeers",&peers);
        engine.rootContext()->setContextProperty("relayStreams",&streams);engine.rootContext()->setContextProperty("softwareRendering",true);
        QQmlComponent panelComponent(&engine,QUrl::fromLocalFile(RELAY_PEER_UI_SOURCE));
        QTRY_VERIFY(panelComponent.status()!=QQmlComponent::Loading);QVERIFY2(panelComponent.isReady(),qPrintable(panelComponent.errorString()));
        QScopedPointer<QObject> panel(panelComponent.createWithInitialProperties({{"width",960},{"height",800}}));QVERIFY(panel);
        auto *button=panel->findChild<QQuickItem *>("watchRelayStream");QVERIFY(button);QVERIFY(button->isVisible());
        QCOMPARE(button->property("text").toString(),"Watch via Relay");
        const auto viewerPath=QFileInfo(QStringLiteral(RELAY_PEER_UI_SOURCE)).absolutePath()+"/RelayStreamWindow.qml";
        QQmlComponent viewerComponent(&engine,QUrl::fromLocalFile(viewerPath));
        QTRY_VERIFY(viewerComponent.status()!=QQmlComponent::Loading);QVERIFY2(viewerComponent.isReady(),qPrintable(viewerComponent.errorString()));
        QScopedPointer<QObject> viewer(viewerComponent.create());QVERIFY2(viewer,qPrintable(viewerComponent.errorString()));
        auto *window=qobject_cast<QQuickWindow *>(viewer.data());QVERIFY(window);QTRY_VERIFY(streams.videoSink());
        auto *sink=qobject_cast<QVideoSink *>(streams.videoSink());QVERIFY(sink);
        QImage image(960,540,QImage::Format_RGB32);image.fill(QColor("#1c4050"));sink->setVideoFrame(QVideoFrame(image));
        QTest::qWait(80);
        if(qEnvironmentVariableIsSet("RELAY_STREAM_VIEWER_SCREENSHOT"))QVERIFY(window->grabWindow().save(qEnvironmentVariable("RELAY_STREAM_VIEWER_SCREENSHOT")));
        window->close();
        peers.streamControlReceived(friendId,{{"v",1},{"type","available"},{"id",""}});QVERIFY(!button->isVisible());
    }
    void unpairedPageKeepsSetupHiddenAndCheckboxInvites() {
        QTemporaryDir root;RelayPeers peers(root.path()+"/profile",{});GeneratedRelayConnections connections;
        QQmlEngine engine;engine.rootContext()->setContextProperty("relayPeers",&peers);engine.rootContext()->setContextProperty("relayConnections",&connections);
        QQmlComponent component(&engine,QUrl::fromLocalFile(RELAY_PEER_UI_SOURCE));QTRY_VERIFY(component.status()!=QQmlComponent::Loading);
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        QScopedPointer<QObject> panel(component.createWithInitialProperties({{"width",960},{"height",800},{"bindingKey","account:friend"},{"channelId","fixture-channel"},{"contactName","Morgan"}}));
        QVERIFY2(panel,qPrintable(component.errorString()));
        QQuickWindow window;window.resize(960,800);auto *item=qobject_cast<QQuickItem *>(panel.data());QVERIFY(item);item->setParentItem(window.contentItem());window.show();
        auto *toggle=panel->findChild<QQuickItem *>("enableRelayConversation");auto *mcp=panel->findChild<QQuickItem *>("enableRelayMcp");
        auto *details=panel->findChild<QObject *>("relayDetailsDialog");auto *config=panel->findChild<QQuickItem *>("relayMcpConfiguration");
        QVERIFY(toggle);QVERIFY(mcp);QVERIFY(details);QVERIFY(config);
        QTRY_VERIFY(toggle->isVisible());QVERIFY(mcp->isVisible());QVERIFY(!details->property("visible").toBool());QVERIFY(!config->isVisible());
        QTest::qWait(100);
        if(qEnvironmentVariableIsSet("RELAY_PEER_EMPTY_SCREENSHOT"))QVERIFY(window.grabWindow().save(qEnvironmentVariable("RELAY_PEER_EMPTY_SCREENSHOT")));
        const auto at=toggle->mapToScene(QPointF(12,toggle->height()/2));QTest::mouseClick(&window,Qt::LeftButton,Qt::NoModifier,at.toPoint());
        QTRY_COMPARE(connections.requests,1);QVERIFY(connections.enabled);QVERIFY(toggle->property("checked").toBool());
        QVERIFY(!details->property("visible").toBool());
        panel->setProperty("setupOpen",true);QTRY_VERIFY(details->property("visible").toBool());
        if(qEnvironmentVariableIsSet("RELAY_PEER_EMPTY_DETAILS_SCREENSHOT")){QTest::qWait(100);QVERIFY(window.grabWindow().save(qEnvironmentVariable("RELAY_PEER_EMPTY_DETAILS_SCREENSHOT")));}
        item->setParentItem(nullptr);
    }
    void separatePanelsKeepPeerHistoriesAndPlainText() {
        QTemporaryDir root;
        RelayPeers a(root.path() + "/a", {}), b(root.path() + "/b", {}), c(root.path() + "/c", {});
        const auto peerB = a.addContact(b.contactCard(), "Morgan", "account:friend-b");
        const auto peerC = a.addContact(c.contactCard(), "Taylor", "account:friend-c");
        QVERIFY(a.sendText(peerB, "Hello Morgan. This conversation is saved only on our clients."));
        QVERIFY(a.sendText(peerB, "<img src='https://invalid.example/never-loaded.png'> remains plain text."));
        QVERIFY(a.sendText(peerC, "Only Taylor's conversation."));
        QQmlEngine engine; engine.rootContext()->setContextProperty("relayPeers", &a);
        QQmlComponent component(&engine, QUrl::fromLocalFile(RELAY_PEER_UI_SOURCE));
        QTRY_VERIFY(component.status() != QQmlComponent::Loading);
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        QScopedPointer<QObject> one(component.createWithInitialProperties({{"width", 960}, {"height", 800}, {"bindingKey", "account:friend-b"}}));
        QVERIFY2(one, qPrintable(component.errorString()));
        QScopedPointer<QObject> two(component.createWithInitialProperties({{"width", 960}, {"height", 800}, {"bindingKey", "account:friend-c"}}));
        QVERIFY2(two, qPrintable(component.errorString()));
        auto *mcpControl = one->findChild<QObject *>("enableRelayMcp");
        auto *details = one->findChild<QObject *>("relayDetailsDialog");
        auto *configuration = one->findChild<QObject *>("relayMcpConfiguration");
        QVERIFY(mcpControl); QVERIFY(details); QVERIFY(configuration);
        QVERIFY(mcpControl->property("visible").toBool());
        QVERIFY(!details->property("visible").toBool());
        // Detailed configuration is enclosed by the initially closed dialog.
        QVERIFY(configuration->parent());
        QCOMPARE(one->property("currentPeer").toString(), peerB);
        QCOMPARE(two->property("currentPeer").toString(), peerC);
        QCOMPARE(one->property("savedMessages").toList().size(), 2);
        QCOMPARE(two->property("savedMessages").toList().size(), 1);
        a.setSelectedPeer(peerC); // Independent window selection cannot redirect the bound DM panel.
        QCOMPARE(one->property("currentPeer").toString(), peerB);
        QQuickWindow window; window.resize(960, 800); auto *item = qobject_cast<QQuickItem *>(one.data()); QVERIFY(item);
        item->setParentItem(window.contentItem()); window.show();
        QTest::qWait(100);
        if (qEnvironmentVariableIsSet("RELAY_PEER_SCREENSHOT")) QVERIFY(window.grabWindow().save(qEnvironmentVariable("RELAY_PEER_SCREENSHOT")));
        one->setProperty("setupOpen", true);
        QTRY_VERIFY(details->property("visible").toBool());
        QTest::qWait(100);
        if (qEnvironmentVariableIsSet("RELAY_PEER_SETUP_SCREENSHOT")) QVERIFY(window.grabWindow().save(qEnvironmentVariable("RELAY_PEER_SETUP_SCREENSHOT")));
        one->setProperty("bindingKey", "account:not-paired");
        QCOMPARE(one->property("currentPeer").toString(), QString{});
        QCOMPARE(one->property("savedMessages").toList().size(), 0);
        item->setParentItem(nullptr);
    }
};
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv); QQuickStyle::setStyle("Basic");
    qmlRegisterType<SoftwareVideo>("Relay.Native",1,0,"SoftwareVideo");
    RelayPeerUiTest test; return QTest::qExec(&test, argc, argv);
}
#include "relay_peer_ui_test.moc"
