#include "control/GlobalShortcuts.h"

#include <QDBusVirtualObject>
#include <QProcess>
#include <QSignalSpy>
#include <QtTest>
#include <QUuid>
#include <memory>

namespace {
const QString Service = "org.example.RelayPortalFixture";
const QString Desktop = "/org/freedesktop/portal/desktop";
const QString Interface = "org.freedesktop.portal.GlobalShortcuts";
}

// Runs on a private dbus-daemon, never the user's desktop bus or permission UI.
class PortalFixture final : public QDBusVirtualObject {
public:
    explicit PortalFixture(QDBusConnection connection) : bus(std::move(connection)) {}
    QDBusConnection bus;
    int createCalls = 0, bindCalls = 0, closes = 0, createDelay = 0;
    uint bindResponse = 0;
    bool unavailable = false, invalidSession = false;
    QString session;
    QStringList subset;
    PortalShortcutList requested;
    QString introspect(const QString &) const override { return {}; }
    void signal(const QString &path, const QString &interface, const QString &name, const QVariantList &arguments) {
        auto message = QDBusMessage::createSignal(path, interface, name);
        message.setArguments(arguments);
        bus.send(message);
    }
    void activated(const QString &id, const QString &sessionOverride = {}) {
        signal(Desktop, Interface, "Activated", {QVariant::fromValue(QDBusObjectPath(sessionOverride.isEmpty() ? session : sessionOverride)),
            id, QVariant::fromValue<qulonglong>(1234), QVariantMap{}});
    }
    void deactivated(const QString &id) {
        signal(Desktop, Interface, "Deactivated", {QVariant::fromValue(QDBusObjectPath(session)),
            id, QVariant::fromValue<qulonglong>(1235), QVariantMap{}});
    }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
        if (message.member() == "Close") {
            ++closes;
            connection.send(message.createReply());
            return true;
        }
        if (message.interface() != Interface) return false;
        if (unavailable) {
            connection.send(message.createErrorReply(QDBusError::UnknownMethod, "Fixture portal unavailable"));
            return true;
        }
        auto sender = message.service().mid(1).replace('.', '_');
        if (message.member() == "CreateSession") {
            ++createCalls;
            const auto options = qdbus_cast<QVariantMap>(message.arguments().first());
            const auto request = Desktop + "/request/" + sender + "/" + options.value("handle_token").toString();
            const auto createdSession = Desktop + "/session/" + sender + "/" + options.value("session_handle_token").toString();
            session = createdSession;
            const auto responseSession = invalidSession ? QStringLiteral("/foreign/session") : createdSession;
            if (createDelay) {
                QTimer::singleShot(createDelay, this, [this, request, responseSession] {
                    signal(request, "org.freedesktop.portal.Request", "Response", {uint(0), QVariantMap{{"session_handle", responseSession}}});
                });
            } else {
                // Deliberately signal before replying: clients must subscribe first.
                signal(request, "org.freedesktop.portal.Request", "Response", {uint(0), QVariantMap{{"session_handle", responseSession}}});
            }
            connection.send(message.createReply(QVariant::fromValue(QDBusObjectPath(request))));
            return true;
        }
        if (message.member() == "BindShortcuts") {
            ++bindCalls;
            requested = qdbus_cast<PortalShortcutList>(message.arguments().at(1));
            const auto options = qdbus_cast<QVariantMap>(message.arguments().at(3));
            const auto request = Desktop + "/request/" + sender + "/" + options.value("handle_token").toString();
            PortalShortcutList accepted;
            for (const auto &shortcut : requested)
                if (subset.isEmpty() || subset.contains(shortcut.id))
                    accepted.append({shortcut.id, {{"trigger_description", "Desktop " + shortcut.id}}});
            signal(request, "org.freedesktop.portal.Request", "Response", {bindResponse,
                QVariantMap{{"shortcuts", QVariant::fromValue(accepted)}}});
            connection.send(message.createReply(QVariant::fromValue(QDBusObjectPath(request))));
            return true;
        }
        return false;
    }
};

class GlobalShortcutsTest final : public QObject {
    Q_OBJECT
    QProcess daemon;
    QString serverName, clientName;
    std::unique_ptr<QDBusConnection> serverBus, clientBus;
    std::unique_ptr<PortalFixture> portal;
    static QVariantList bindings() {
        return {QVariantMap{{"action", "toggle-mute"}, {"sequence", "Ctrl+Shift+M"}},
            QVariantMap{{"action", "toggle-deafen"}, {"sequence", "Ctrl+Shift+D"}},
            QVariantMap{{"action", "disconnect-voice"}, {"sequence", "Ctrl+Shift+H"}}};
    }
private slots:
    void initTestCase() {
        daemon.start("dbus-daemon", {"--session", "--nofork", "--print-address=1"});
        QVERIFY2(daemon.waitForStarted(2000), "A private dbus-daemon is required for the portal fixture.");
        QVERIFY(daemon.waitForReadyRead(2000));
        const auto address = QString::fromUtf8(daemon.readLine()).trimmed();
        QVERIFY(address.startsWith("unix:"));
        serverName = "relay-portal-server-" + QUuid::createUuid().toString(QUuid::Id128);
        clientName = "relay-portal-client-" + QUuid::createUuid().toString(QUuid::Id128);
        serverBus = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address, serverName));
        clientBus = std::make_unique<QDBusConnection>(QDBusConnection::connectToBus(address, clientName));
        QVERIFY(serverBus->isConnected());
        QVERIFY(clientBus->isConnected());
    }
    void init() {
        portal = std::make_unique<PortalFixture>(*serverBus);
        QVERIFY(serverBus->registerService(Service));
        QVERIFY(serverBus->registerVirtualObject(Desktop, portal.get(), QDBusConnection::SubPath));
    }
    void cleanup() {
        serverBus->unregisterObject(Desktop, QDBusConnection::UnregisterTree);
        serverBus->unregisterService(Service);
        portal.reset();
    }
    void cleanupTestCase() {
        clientBus.reset(); serverBus.reset();
        QDBusConnection::disconnectFromBus(clientName);
        QDBusConnection::disconnectFromBus(serverName);
        daemon.terminate();
        if (!daemon.waitForFinished(1000)) { daemon.kill(); daemon.waitForFinished(1000); }
    }
    void explicitSetupHandlesEarlyResponsesAndActivationOnce() {
        GlobalShortcuts shortcuts(*clientBus, Service);
        QTest::qWait(20);
        QCOMPARE(portal->createCalls, 0);
        QVERIFY(!shortcuts.active());
        shortcuts.enable(bindings());
        QTRY_VERIFY_WITH_TIMEOUT(shortcuts.active(), 2000);
        QVERIFY(!shortcuts.busy());
        QCOMPARE(portal->createCalls, 1);
        QCOMPARE(portal->bindCalls, 1);
        QCOMPARE(portal->requested.size(), 3);
        QCOMPARE(portal->requested.first().properties.value("preferred_trigger").toString(), "CTRL+SHIFT+m");
        QCOMPARE(shortcuts.bindings().size(), 3);
        QSignalSpy actions(&shortcuts, &GlobalShortcuts::actionRequested);
        portal->activated("toggle-mute");
        portal->activated("toggle-mute");
        portal->activated("send-message");
        portal->activated("toggle-deafen", "/wrong/session");
        QTRY_COMPARE_WITH_TIMEOUT(actions.size(), 1, 1000);
        portal->deactivated("toggle-mute");
        portal->activated("toggle-mute");
        QTRY_COMPARE_WITH_TIMEOUT(actions.size(), 2, 1000);
        shortcuts.disable();
        QVERIFY(!shortcuts.active());
        QVERIFY(!shortcuts.isBound("toggle-mute"));
        QTRY_VERIFY(portal->closes > 0);
        portal->activated("toggle-mute");
        QTest::qWait(20);
        QCOMPARE(actions.size(), 2);
    }
    void cancellationAndUnsupportedDesktopAreHonest() {
        GlobalShortcuts shortcuts(*clientBus, Service);
        portal->bindResponse = 1;
        shortcuts.enable(bindings());
        QTRY_VERIFY_WITH_TIMEOUT(!shortcuts.busy(), 2000);
        QVERIFY(!shortcuts.active());
        QVERIFY(shortcuts.error().isEmpty());
        QVERIFY(shortcuts.status().contains("cancelled"));
        portal->unavailable = true;
        shortcuts.enable(bindings());
        QTRY_VERIFY_WITH_TIMEOUT(!shortcuts.busy(), 2000);
        QVERIFY(!shortcuts.active());
        QVERIFY(!shortcuts.error().isEmpty());
    }
    void cancelledRequestCannotReactivateThroughLateResponse() {
        GlobalShortcuts shortcuts(*clientBus, Service);
        portal->createDelay = 150;
        shortcuts.enable(bindings());
        QTRY_COMPARE(portal->createCalls, 1);
        shortcuts.disable();
        portal->createDelay = 0;
        shortcuts.enable(bindings());
        QTRY_VERIFY_WITH_TIMEOUT(shortcuts.active(), 2000);
        QTest::qWait(200);
        QCOMPARE(portal->createCalls, 2);
        QCOMPARE(portal->bindCalls, 1);
        QVERIFY(shortcuts.active());
    }
    void desktopSubsetAndSessionClosureUpdateState() {
        GlobalShortcuts shortcuts(*clientBus, Service);
        portal->subset = {"toggle-mute"};
        shortcuts.enable(bindings());
        QTRY_VERIFY_WITH_TIMEOUT(shortcuts.active(), 2000);
        QVERIFY(shortcuts.isBound("toggle-mute"));
        QVERIFY(!shortcuts.isBound("toggle-deafen"));
        const PortalShortcutList changed{{"toggle-deafen", {{"trigger_description", "F9"}}},
                                         {"send-message", {{"trigger_description", "F10"}}}};
        portal->signal(Desktop, Interface, "ShortcutsChanged", {QVariant::fromValue(QDBusObjectPath(portal->session)), QVariant::fromValue(changed)});
        QTRY_VERIFY(shortcuts.isBound("toggle-deafen"));
        QVERIFY(!shortcuts.isBound("toggle-mute"));
        QVERIFY(!shortcuts.isBound("send-message"));
        portal->signal(portal->session, "org.freedesktop.portal.Session", "Closed", {QVariantMap{}});
        QTRY_VERIFY(!shortcuts.active());
        QVERIFY(shortcuts.status().contains("closed"));
    }
    void untrustedOrDisabledBindingsCannotBecomeActions() {
        GlobalShortcuts shortcuts(*clientBus, Service);
        shortcuts.enable({QVariantMap{{"action", "send-message"}, {"sequence", "Ctrl+M"}},
                          QVariantMap{{"action", "toggle-mute"}, {"sequence", ""}}});
        QVERIFY(!shortcuts.busy());
        QCOMPARE(portal->createCalls, 0);
        QVERIFY(!shortcuts.error().isEmpty());
        portal->invalidSession = true;
        shortcuts.enable(bindings());
        QTRY_VERIFY_WITH_TIMEOUT(!shortcuts.busy(), 2000);
        QCOMPARE(portal->bindCalls, 0);
        QVERIFY(!shortcuts.active());
        QVERIFY(!shortcuts.error().isEmpty());
    }
    void preferredTriggersUseXdgGrammar() {
        QCOMPARE(GlobalShortcuts::preferredTrigger("Ctrl+Shift+M"), "CTRL+SHIFT+m");
        QCOMPARE(GlobalShortcuts::preferredTrigger("Meta+Alt+F8"), "ALT+LOGO+F8");
        QCOMPARE(GlobalShortcuts::preferredTrigger("Ctrl+Return"), "CTRL+Return");
        QCOMPARE(GlobalShortcuts::preferredTrigger("Ctrl++"), "CTRL+plus");
        QVERIFY(GlobalShortcuts::preferredTrigger("Ctrl+K, Ctrl+M").isEmpty());
        QVERIFY(GlobalShortcuts::preferredTrigger("Banana").isEmpty());
        QVERIFY(GlobalShortcuts::preferredTrigger(QString(200, 'x')).isEmpty());
    }
};

QTEST_GUILESS_MAIN(GlobalShortcutsTest)
#include "global_shortcuts_test.moc"
