#include <QtTest>
#include <QPointer>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include <QQmlEngine>
#include <QQmlComponent>
#include "addons/AddonManager.h"

class AddonTest final : public QObject {
    Q_OBJECT
    static QString fixtureDirectory() { return QFileInfo(QStringLiteral(RELAY_TEST_ADDON_PATH)).absolutePath(); }
    static constexpr auto id = "org.relay.fixture";
private slots:
    void lazyServiceAndSafeDisable() {
        QTemporaryDir settings; QVERIFY(settings.isValid());
        qApp->setProperty("addonFixtureServicesCreated", 0);
        AddonManager manager({fixtureDirectory()}, {}, settings.filePath("settings.ini"));
        QCOMPARE(manager.items().size(), 1); QVERIFY(manager.enabled(id));
        QCOMPARE(qApp->property("addonFixtureServicesCreated").toInt(), 0);
        QVERIFY(manager.openSettings(id));
        QPointer<QObject> service = manager.activeService(); QVERIFY(service);
        QCOMPARE(qApp->property("addonFixtureServicesCreated").toInt(), 1);
        QVERIFY(QMetaObject::invokeMethod(service, "start")); QVERIFY(service->property("running").toBool());
        QQmlEngine engine;
        QQmlComponent component(&engine, manager.activeSettingsUrl());
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> panel(component.createWithInitialProperties({{"service", QVariant::fromValue(service.data())}}));
        QVERIFY2(panel != nullptr, qPrintable(component.errorString()));
        QVERIFY(panel->property("serviceRunning").toBool());
        QSignalSpy activeChanges(&manager, &AddonManager::activeChanged);
        QVERIFY(manager.setEnabled(id, false));
        QVERIFY(manager.activeId().isEmpty()); QVERIFY(!manager.activeService()); QCOMPARE(activeChanges.size(), 1);
        QVERIFY(service); QVERIFY(service->property("stopped").toBool()); QVERIFY(!panel->property("serviceRunning").toBool());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!service); QVERIFY(!panel->property("serviceRunning").toBool());
        QVERIFY(!manager.openSettings(id));
        QVERIFY(manager.setEnabled(id, true)); QVERIFY(manager.openSettings(id));
        QCOMPARE(qApp->property("addonFixtureServicesCreated").toInt(), 2);
        manager.closeSettings(); QVERIFY(manager.activeId().isEmpty());
    }
    void shareSourceDiscoveryIsLazyAndRejectsForeignResources() {
        QTemporaryDir settings; QVERIFY(settings.isValid());
        qApp->setProperty("addonFixtureServicesCreated", 0);
        qApp->setProperty("addonFixtureShareUrl", QUrl{});
        AddonManager manager({fixtureDirectory()}, {}, settings.filePath("settings.ini"));
        QCOMPARE(manager.broadcastSources().size(), 1);
        QCOMPARE(qApp->property("addonFixtureServicesCreated").toInt(), 0);
        const auto source = manager.openBroadcastSource(id);
        QVERIFY(source.value("service").value<QObject *>());
        QCOMPARE(source.value("editorUrl").toUrl(), QUrl("qrc:/relay/addons/org.relay.fixture/Input.qml"));
        QVERIFY(manager.activeId().isEmpty()); // Share selection does not open Settings.
        QCOMPARE(qApp->property("addonFixtureServicesCreated").toInt(), 1);
        qApp->setProperty("addonFixtureShareUrl", QUrl("qrc:/relay/addons/org.someone.else/Input.qml"));
        QVERIFY(manager.openBroadcastSource(id).contains("error"));
        qApp->setProperty("addonFixtureShareUrl", QUrl("file:///tmp/foreign.qml"));
        QVERIFY(manager.openBroadcastSource(id).contains("error"));
        qApp->setProperty("addonFixtureShareUrl", QUrl{});
        QVERIFY(manager.setEnabled(id, false));
        QVERIFY(manager.broadcastSources().isEmpty());
        QVERIFY(manager.openBroadcastSource(id).isEmpty());
        QVERIFY(manager.setEnabled(id, true));
        QCOMPARE(manager.broadcastSources().size(), 1);
    }
    void disabledPreferenceSurvivesRestart() {
        QTemporaryDir settings; QVERIFY(settings.isValid());
        const auto file = settings.filePath("settings.ini");
        { AddonManager first({fixtureDirectory()}, {}, file); QVERIFY(first.setEnabled(id, false)); }
        AddonManager second({fixtureDirectory()}, {}, file);
        QVERIFY(!second.enabled(id)); QVERIFY(!second.service(id));
    }
    void localModulesRequireEnableAndShutdownStops() {
        QTemporaryDir settings; QVERIFY(settings.isValid());
        AddonManager manager({}, fixtureDirectory(), settings.filePath("settings.ini"));
        QCOMPARE(manager.items().size(), 1); QVERIFY(!manager.enabled(id));
        QVERIFY(!manager.items().first().toMap().value("bundled").toBool());
        QVERIFY(!manager.service(id)); QVERIFY(manager.setEnabled(id, true));
        QPointer<QObject> service = manager.service(id); QVERIFY(service);
        QVERIFY(QMetaObject::invokeMethod(service, "start"));
        manager.shutdown(); QVERIFY(!service); QVERIFY(!manager.service(id));
    }
    void bundledIdWinsAndRescanDoesNotDuplicate() {
        QTemporaryDir settings; QVERIFY(settings.isValid());
        AddonManager manager({fixtureDirectory()}, fixtureDirectory(), settings.filePath("settings.ini"));
        manager.rescan(); QCOMPARE(manager.items().size(), 1);
        QVERIFY(manager.items().first().toMap().value("bundled").toBool());
        QVERIFY(!manager.openSettings("org.unknown.addon")); QVERIFY(!manager.setEnabled("org.unknown.addon", true));
    }
};
QTEST_GUILESS_MAIN(AddonTest)
#include "addon_test.moc"
