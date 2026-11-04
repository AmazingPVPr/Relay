#include "NotificationManager.h"
#include <QtTest>
#include <QDBusVirtualObject>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QProcess>
#include <QSignalSpy>
#include <memory>
class NotificationFixture final : public QDBusVirtualObject {
public:
    QDBusConnection bus; QVariantList last; int calls=0; uint nextId=1;
    bool delay=false; QList<QDBusMessage> waiting; QList<uint> closedIds;
    explicit NotificationFixture(QDBusConnection b):bus(std::move(b)){}
    QString introspect(const QString &)const override{return {};}
    bool handleMessage(const QDBusMessage &m,const QDBusConnection &connection) override {
        if(m.member()=="GetCapabilities") connection.send(m.createReply(QVariant::fromValue(QStringList{"actions","body","body-markup"})));
        else if(m.member()=="Notify") {last=m.arguments();++calls;const uint old=last.at(1).toUInt();if(delay) { m.setDelayedReply(true); waiting.append(m); } else connection.send(m.createReply(old?old:nextId++));}
        else if(m.member()=="CloseNotification") { closedIds.append(m.arguments().at(0).toUInt()); connection.send(m.createReply()); }
        else return false; return true;
    }
    void finishNext() { const auto m=waiting.takeFirst(); const uint old=m.arguments().at(1).toUInt(); bus.send(m.createReply(old?old:nextId++)); }
    void activate(uint id) { auto m=QDBusMessage::createSignal("/org/freedesktop/Notifications","org.freedesktop.Notifications","ActionInvoked");m.setArguments({id,"default"});bus.send(m); }
};
class NotificationTest final : public QObject {
    Q_OBJECT
    QProcess daemon; QString address;
private slots:
    void initTestCase(){daemon.start("dbus-daemon",{"--session","--nofork","--print-address=1"});QVERIFY(daemon.waitForStarted(2000));QVERIFY(daemon.waitForReadyRead(2000));address=QString::fromUtf8(daemon.readLine()).trimmed();}
    void cleanupTestCase(){daemon.terminate();if(!daemon.waitForFinished(1000)){daemon.kill();daemon.waitForFinished(1000);}}
    void notificationsCoalesceEscapeAndOpenCorrectChannel(){
        auto server=QDBusConnection::connectToBus(address,"relay-notify-server");
        const auto client=QDBusConnection::connectToBus(address,"relay-notify-client");
        NotificationFixture fixture(server); const QString service="org.example.RelayNotifications";
        QVERIFY(server.registerService(service));QVERIFY(server.registerVirtualObject("/org/freedesktop/Notifications",&fixture));
        { NotificationManager manager(client,service);QSignalSpy opened(&manager,&NotificationManager::openChannelRequested),played(&manager,&NotificationManager::notificationDisplayed);
          QTRY_VERIFY_WITH_TIMEOUT(manager.available(),2000);QCOMPARE(fixture.calls,0);
          manager.setMinimumDuration(9);
          manager.deliver("123","Old","Old body",true); manager.deliver("123","New","<b>hello</b>",false);
          QTRY_COMPARE_WITH_TIMEOUT(fixture.calls,1,2000);QTRY_COMPARE_WITH_TIMEOUT(played.count(),1,1000);
          QCOMPARE(fixture.last.at(3).toString(),QString("New"));QCOMPARE(fixture.last.at(4).toString(),QString("&lt;b&gt;hello&lt;/b&gt;"));
          QVERIFY(qdbus_cast<QVariantMap>(fixture.last.at(6)).value("suppress-sound").toBool());
          QCOMPARE(fixture.last.at(7).toInt(),9000);
          QVERIFY(!played.at(0).at(0).toBool()); fixture.activate(1);QTRY_COMPARE_WITH_TIMEOUT(opened.count(),1,1000);QCOMPARE(opened.at(0).at(0).toString(),QString("123"));
          manager.deliver("123","Next","Next body",true);QTRY_COMPARE_WITH_TIMEOUT(fixture.calls,2,2000);QCOMPARE(fixture.last.at(1).toUInt(),1U);
          manager.deliver("456","Pending","Will be cleared",true); manager.clear();QTest::qWait(300);QCOMPARE(fixture.calls,2);
          fixture.activate(1);QTest::qWait(50);QCOMPARE(opened.count(),1);
        }
        server.unregisterObject("/org/freedesktop/Notifications");server.unregisterService(service);
        QDBusConnection::disconnectFromBus("relay-notify-server");QDBusConnection::disconnectFromBus("relay-notify-client");
    }
    void delayedNotificationsAreSerializedAndClosedAfterClear() {
        auto server=QDBusConnection::connectToBus(address,"relay-notify-delay-server");
        const auto client=QDBusConnection::connectToBus(address,"relay-notify-delay-client");
        NotificationFixture fixture(server); fixture.delay=true; const QString service="org.example.RelayDelayNotifications";
        QVERIFY(server.registerService(service)); QVERIFY(server.registerVirtualObject("/org/freedesktop/Notifications",&fixture));
        { NotificationManager manager(client,service); QSignalSpy played(&manager,&NotificationManager::notificationDisplayed);
          QTRY_VERIFY_WITH_TIMEOUT(manager.available(),2000);
          manager.deliver("123","First","First",true); QTRY_COMPARE_WITH_TIMEOUT(fixture.calls,1,1000);
          manager.deliver("123","Latest","Latest",false); QTest::qWait(300); QCOMPARE(fixture.calls,1);
          fixture.finishNext(); QTRY_COMPARE_WITH_TIMEOUT(played.count(),1,1000);
          QTRY_COMPARE_WITH_TIMEOUT(fixture.calls,2,1000); QCOMPARE(fixture.last.at(1).toUInt(),1U);
          manager.clear(); QTRY_VERIFY_WITH_TIMEOUT(fixture.closedIds.contains(1),1000);
          fixture.closedIds.clear(); fixture.finishNext();
          QTRY_VERIFY_WITH_TIMEOUT(fixture.closedIds.contains(1),1000); QCOMPARE(played.count(),1);
          manager.deliver("456","Fresh","Fresh",false); QTRY_COMPARE_WITH_TIMEOUT(fixture.calls,3,1000);
          QCOMPARE(fixture.last.at(1).toUInt(),0U); fixture.finishNext(); QTRY_COMPARE_WITH_TIMEOUT(played.count(),2,1000);
          manager.clear();
        }
        server.unregisterObject("/org/freedesktop/Notifications"); server.unregisterService(service);
        QDBusConnection::disconnectFromBus("relay-notify-delay-server"); QDBusConnection::disconnectFromBus("relay-notify-delay-client");
    }

};
QTEST_GUILESS_MAIN(NotificationTest)
#include "notification_test.moc"
