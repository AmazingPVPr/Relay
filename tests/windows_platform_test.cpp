#include <QtGlobal>
#ifdef Q_OS_WIN
#include "control/GlobalShortcuts.h"
#include "NotificationManager.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <QtTest>
class WindowsPlatformTest final : public QObject {
    Q_OBJECT
private slots:
    void invalidShortcutDoesNotRegisterOrRemainActive() {
        GlobalShortcuts shortcuts;
        shortcuts.enable({QVariantMap{{"action","toggle-mute"},{"sequence","Ctrl+K, Ctrl+M"}}});
        QVERIFY(!shortcuts.active());QVERIFY(!shortcuts.error().isEmpty());
        QVERIFY(!shortcuts.isBound("toggle-mute"));
        shortcuts.disable();QVERIFY(shortcuts.error().isEmpty());
        QVERIFY(shortcuts.bindings().isEmpty());
    }
    void previewNeverDisplaysPersonalNotifications() {
        qApp->setProperty("relayPreviewMode",true);
        NotificationManager manager;
        QSignalSpy displayed(&manager,&NotificationManager::notificationDisplayed);
        QVERIFY(!manager.available());manager.sendTest();QCOMPARE(displayed.count(),0);
        manager.clear();
    }
};
QTEST_MAIN(WindowsPlatformTest)
#include "windows_platform_test.moc"
#endif
