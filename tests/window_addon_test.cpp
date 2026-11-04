#include <QtTest>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QPointer>
#include "addons/AddonManager.h"
class WindowAddonTest final : public QObject {
 Q_OBJECT
private slots:
 void privateWindowLifecycle() {
    QTemporaryDir dir;
    AddonManager absent({},dir.path(),dir.filePath("absent.ini"));
    QVERIFY(absent.items().isEmpty());
    QVERIFY(!absent.openSettings("org.relay.windowfixture"));
    AddonManager manager({},QFileInfo(QStringLiteral(RELAY_WINDOW_FIXTURE)).absolutePath(),dir.filePath("prefs.ini"));
    QCOMPARE(manager.items().size(),1);
    QVERIFY(!manager.enabled("org.relay.windowfixture"));
    QVERIFY(!manager.openSettings("org.relay.windowfixture"));
    QVERIFY(manager.setEnabled("org.relay.windowfixture",true));
    QVERIFY(manager.openSettings("org.relay.windowfixture"));
    QPointer<QObject> service=manager.service("org.relay.windowfixture");
    QVERIFY(service);QCOMPARE(service->property("windows").toInt(),1);
    QVERIFY(manager.activeId().isEmpty());
    QVERIFY(manager.openSettings("org.relay.windowfixture"));
    QCOMPARE(service->property("windows").toInt(),2);
    QVERIFY(manager.setEnabled("org.relay.windowfixture",false));
    QCOMPARE(service->property("windows").toInt(),0);
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    QVERIFY(!service);
 }
};
QTEST_MAIN(WindowAddonTest)
#include "window_addon_test.moc"
