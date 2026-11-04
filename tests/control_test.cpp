#include "control/ControlServer.h"
#include "control/ShortcutSettings.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <chrono>
#include <future>
#include <limits>
#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

class ControlTest final : public QObject {
    Q_OBJECT
    static QJsonObject exchange(const QString &path, const QByteArray &request, int deadline = 2200) {
        QLocalSocket socket;
        socket.connectToServer(path);
        if (!socket.waitForConnected(200)) return {{"fixture_error", "connect"}};
        socket.write(request);
        socket.flush();
        QElapsedTimer elapsed;
        elapsed.start();
        while (!socket.canReadLine() && elapsed.elapsed() < deadline && socket.state() != QLocalSocket::UnconnectedState)
            QTest::qWait(10);
        return QJsonDocument::fromJson(socket.readAll().trimmed()).object();
    }
private slots:
    void emptySettingsPathUsesApplicationConfigLocation() {
        const bool wasTestMode = QStandardPaths::isTestModeEnabled();
        QStandardPaths::setTestModeEnabled(true);
        {
            ShortcutSettings settings(QString{});
            QCOMPARE(settings.m_settings->fileName(), QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/shortcuts.ini");
        }
        QStandardPaths::setTestModeEnabled(wasTestMode);
    }
    void localEndpointIsPrivateAndOnlyAllowedActionsDispatch() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto endpoint = directory.path() + "/controls.sock";
        ControlServer server;
        QVERIFY2(server.start(endpoint), qPrintable(server.error()));
        QVERIFY(server.running());
        #ifdef Q_OS_UNIX
        const QFileInfo socketFile(endpoint);
        const auto unsafe = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                            QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        QVERIFY(!(socketFile.permissions() & unsafe));
        #endif
        QSignalSpy actions(&server, &ControlServer::actionRequested);
        for (const auto &action : ControlServer::supportedActions()) {
            const auto request = QJsonDocument(QJsonObject{{"version", 1}, {"action", action}}).toJson(QJsonDocument::Compact) + '\n';
            const auto result = exchange(endpoint, request);
            QVERIFY2(result.value("ok").toBool(), qPrintable(QString::fromUtf8(QJsonDocument(result).toJson())));
        }
        QCOMPARE(actions.size(), 7);
        QCOMPARE(actions.at(0).first().toString(), "mute");
        QCOMPARE(actions.at(6).first().toString(), "disconnect-voice");
        QVERIFY(!server.execute("send-message"));
        QCOMPARE(actions.size(), 7);
    }
    void cliRoundtripReturnsOnlySanitizedState() {
        QTemporaryDir directory;
        ControlServer server;
        QVERIFY(server.start(directory.path() + "/controls.sock"));
        bool muted = false;
        connect(&server, &ControlServer::actionRequested, this, [&](const QString &action) {
            if (action == "toggle-mute") muted = !muted;
        });
        server.setStatusProvider([&] { return QVariantMap{{"muted", muted}, {"voiceConnected", true},
            {"deafened", false}, {"discordConnected", true}, {"token", "fixture-must-not-leak"}, {"name", "fixture-must-not-leak"}}; });
        auto future = std::async(std::launch::async, [endpoint = server.serverName()] {
            return ControlServer::sendCommand("toggle-mute", 1000, endpoint);
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 2000);
        const auto result = future.get();
        QVERIFY(result.value("ok").toBool());
        const auto state = result.value("status").toMap();
        QVERIFY(state.value("muted").toBool());
        QVERIFY(state.value("voiceConnected").toBool());
        QCOMPARE(state.size(), 5);
        QVERIFY(!state.contains("token"));
        QVERIFY(!state.contains("name"));
        QVERIFY(!QJsonDocument(QJsonObject::fromVariantMap(result)).toJson().contains("fixture-must-not-leak"));
    }
    void diagnosticsRoundtripIsReadOnlyAndStrictlyContentFree() {
        QTemporaryDir directory;
        ControlServer server;
        QVERIFY(server.start(directory.path() + "/controls.sock"));
        QSignalSpy actions(&server, &ControlServer::actionRequested);
        server.setStatusProvider([] { return QVariantMap{{"muted", true}, {"voiceConnected", true}}; });
        const auto privateText = QStringLiteral("fixture-token-address-message-never-crosses-ipc");
        server.setDiagnosticsProvider([&] { return QVariantMap{
            {"voiceConnected", true}, {"muted", true}, {"deafened", false}, {"discordConnected", true},
            {"micTesting", false}, {"broadcastActive", true}, {"streamActive", false}, {"cameraActive", false},
            {"voiceGatewayPingMs", 27.5}, {"voiceGatewayAveragePingMs", 32},
            {"token", privateText}, {"sourceUrl", privateText}, {"userId", qulonglong(123456789)},
            {"error", privateText}, {"voiceHistory", QVariantMap{{"message", privateText}}},
            {"broadcast", QVariantMap{{"active", true}, {"connected", true}, {"encrypted", true},
                {"sharing", true}, {"audioActive", true}, {"originalForwarded", true},
                {"videoPackets", qulonglong(1985)}, {"queuedPackets", 12}, {"queueDrops", 0},
                {"targetBitrate", 20000000}, {"sourcePath", 3}, {"originalRecoveries", qulonglong(2)},
                {"width", 2560}, {"height", 1440}, {"fractionLost", 4}, {"rttMs", 32},
                {"token", privateText}, {"input", privateText}, {"media", QByteArray("private-fixture")},
                {"extra", QVariantMap{{"videoPackets", 999}, {"message", privateText}}}}},
            {"voicePlayback", QVariantMap{{"outputState", -1}, {"receiveQueuedMs", 40},
                {"outputUnderruns", qulonglong(3)}, {"user", privateText}}},
            {"streamPlayback", QVariantMap{{"audioPrimed", true}, {"audioRecoveryPending", false},
                {"decodedFrames", qulonglong(300)}, {"audioPendingMs", qint64(0)}, {"url", privateText}}}
        }; });
        auto future = std::async(std::launch::async, [endpoint = server.serverName()] {
            return ControlServer::sendCommand("diagnostics", 1000, endpoint);
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 2000);
        const auto result = future.get();
        QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.size(), 2);
        QVERIFY(!result.contains("status"));
        const auto diagnostic = result.value("diagnostics").toMap();
        const QVariantMap expected{
            {"version", 1}, {"running", true}, {"voiceConnected", true}, {"muted", true},
            {"deafened", false}, {"discordConnected", true}, {"micTesting", false},
            {"broadcastActive", true}, {"streamActive", false}, {"cameraActive", false},
            {"voiceGatewayPingMs", 27.5}, {"voiceGatewayAveragePingMs", 32},
            {"broadcast", QVariantMap{{"active", true}, {"connected", true}, {"encrypted", true},
                {"sharing", true}, {"audioActive", true}, {"originalForwarded", true},
                {"videoPackets", 1985}, {"queuedPackets", 12}, {"queueDrops", 0},
                {"targetBitrate", 20000000}, {"sourcePath", 3}, {"originalRecoveries", 2},
                {"width", 2560}, {"height", 1440}, {"fractionLost", 4}, {"rttMs", 32}}},
            {"voicePlayback", QVariantMap{{"outputState", -1}, {"receiveQueuedMs", 40}, {"outputUnderruns", 3}}},
            {"streamPlayback", QVariantMap{{"audioPrimed", true}, {"audioRecoveryPending", false},
                {"decodedFrames", 300}, {"audioPendingMs", 0}}}};
        QCOMPARE(QJsonObject::fromVariantMap(diagnostic), QJsonObject::fromVariantMap(expected));
        const auto wire = QJsonDocument::fromVariant(result).toJson(QJsonDocument::Compact);
        QVERIFY(wire.size() < 8192);
        QVERIFY(!wire.contains(privateText.toUtf8()));
        QCOMPARE(actions.size(), 0);
        const auto ordinary = exchange(server.serverName(), "{\"version\":1,\"action\":\"status\"}\n");
        QCOMPARE(ordinary.value("status").toObject().size(), 5);
        QVERIFY(!ordinary.contains("diagnostics"));
        QCOMPARE(actions.size(), 0);
    }
    void diagnosticsRejectWrongTypesAndUnboundedCallerData() {
        QTemporaryDir directory;
        ControlServer server;
        QVERIFY(server.start(directory.path() + "/controls.sock"));
        QSignalSpy actions(&server, &ControlServer::actionRequested);
        server.setDiagnosticsProvider([] { return QVariantMap{
            {"version", 999}, {"running", false}, {"voiceConnected", "true"}, {"muted", 1},
            {"discordConnected", QVariantList{true}}, {"voiceGatewayPingMs", "42"},
            {"untrusted", QString(100000, 'X')},
            {"broadcast", QVariantMap{{"active", "true"}, {"sharing", 1}, {"videoPackets", "555"},
                {"queueDrops", true}, {"sourcePath", QVariantMap{{"token", "private"}}},
                {"targetBitrate", std::numeric_limits<double>::infinity()},
                {"rttMs", std::numeric_limits<double>::quiet_NaN()},
                {"wireBytes", std::numeric_limits<qulonglong>::max()},
                {"receiverReports", QByteArray("42")}, {"width", 1920}}},
            {"voicePlayback", "not-a-map"},
            {"streamPlayback", QVariantList{QVariantMap{{"polls", 10}}}}
        }; });
        const auto result = exchange(server.serverName(), "{\"version\":1,\"action\":\"diagnostics\"}\n");
        QVERIFY(result.value("ok").toBool());
        const QJsonObject expected{{"version", 1}, {"running", true},
            {"broadcast", QJsonObject{{"width", 1920}}},
            {"voicePlayback", QJsonObject{}}, {"streamPlayback", QJsonObject{}}};
        QCOMPARE(result.value("diagnostics").toObject(), expected);
        QVERIFY(QJsonDocument(result).toJson().size() < 256);
        QCOMPARE(actions.size(), 0);
        const auto invalid = exchange(server.serverName(),
            "{\"version\":1,\"action\":\"diagnostics\",\"include\":\"token\"}\n");
        QVERIFY(!invalid.value("ok").toBool());
        QCOMPARE(actions.size(), 0);
    }
    void malformedOversizedAndTimedOutRequestsDoNotAct() {
        QTemporaryDir directory;
        ControlServer server;
        QVERIFY(server.start(directory.path() + "/controls.sock"));
        QSignalSpy actions(&server, &ControlServer::actionRequested);
        const QList<QByteArray> badRequests{
            "not-json\n", "[]\n", "{\"version\":2,\"action\":\"mute\"}\n",
            "{\"version\":1,\"action\":\"mute\",\"token\":\"fixture\"}\n",
            "{\"version\":1,\"action\":\"send-message\"}\n",
            "{\"version\":1,\"action\":\"mute\"}\n{\"version\":1,\"action\":\"unmute\"}\n",
            QByteArray(1100, 'a') + '\n',
            "{\"version\":1" // No newline: the bounded request timer must close it.
        };
        for (const auto &request : badRequests) {
            const auto result = exchange(server.serverName(), request);
            QVERIFY(result.contains("ok"));
            QVERIFY(!result.value("ok").toBool());
        }
        QCOMPARE(actions.size(), 0);
    }
    void liveEndpointAndOrdinaryFilesAreNeverRemoved() {
        QTemporaryDir directory;
        const auto endpoint = directory.path() + "/controls.sock";
        ControlServer first, second;
        QVERIFY(first.start(endpoint));
        QVERIFY(!second.start(endpoint));
        QVERIFY(first.running());
        #ifdef Q_OS_UNIX
        QVERIFY(QFileInfo::exists(endpoint));
        #endif
        QLocalServer existingListener;
        const auto otherEndpoint = directory.path() + "/existing.sock";
        QVERIFY(existingListener.listen(otherEndpoint));
        QVERIFY(!second.start(otherEndpoint));
        QVERIFY(existingListener.isListening());
        const auto path = directory.path() + "/ordinary-file";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("keep this fixture");
        file.close();
        QVERIFY(!second.start(path));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("keep this fixture"));
    }
    void abandonedSocketCanRecover() {
#ifdef Q_OS_UNIX
        QTemporaryDir directory;
        const auto endpoint = directory.path() + "/stale.sock";
        const auto encoded = QFile::encodeName(endpoint);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        QVERIFY(encoded.size() < qsizetype(sizeof(address.sun_path)));
        memcpy(address.sun_path, encoded.constData(), size_t(encoded.size()));
        const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
        QVERIFY(socket >= 0);
        const int bound = ::bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
        ::close(socket);
        QCOMPARE(bound, 0);
        QVERIFY(QFileInfo::exists(endpoint));
        ControlServer server;
        QVERIFY2(server.start(endpoint), qPrintable(server.error()));
#else
        QSKIP("Unix socket recovery is Linux-specific.");
#endif
    }
    void privateDirectoryRequiredAndMissingInstanceFailsQuickly() {
        QTemporaryDir directory;
        #ifdef Q_OS_UNIX
        QVERIFY(QFile::setPermissions(directory.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ExeOwner | QFileDevice::ReadOther | QFileDevice::ExeOther));
        ControlServer server;
        QVERIFY(!server.start(directory.path() + "/controls.sock"));
        #endif
        const auto result = ControlServer::sendCommand("status", 100, directory.path() + "/missing.sock");
        QVERIFY(!result.value("ok").toBool());
        QVERIFY(!result.value("error").toString().isEmpty());
    }
    void customShortcutsPersistAndRejectConflicts() {
        QTemporaryDir directory;
        const auto file = directory.path() + "/shortcuts.ini";
        {
            ShortcutSettings settings(file);
            QCOMPARE(settings.muteShortcut(), "Ctrl+Shift+M");
            QCOMPARE(settings.bindings().size(), 3);
            QVERIFY(!settings.setBinding("toggle-mute", "m"));
            QVERIFY(!settings.error().isEmpty());
            QVERIFY(!settings.setBinding("toggle-mute", "Ctrl+Shift+D"));
            QVERIFY(!settings.setBinding("toggle-mute", "Ctrl+K, Ctrl+M"));
            QVERIFY(!settings.setBinding("toggle-mute", "Banana"));
            QVERIFY(!settings.setBinding("send-message", "Ctrl+M"));
            QVERIFY(settings.setBinding("toggle-mute", "Ctrl+Alt+M"));
            QVERIFY(settings.error().isEmpty());
            QCOMPARE(settings.muteShortcut(), "Ctrl+Alt+M");
            QVERIFY(settings.setBinding("disconnect-voice", ""));
            QVERIFY(settings.disconnectShortcut().isEmpty());
        }
        ShortcutSettings loaded(file);
        QCOMPARE(loaded.muteShortcut(), "Ctrl+Alt+M");
        QVERIFY(loaded.disconnectShortcut().isEmpty());
        loaded.reset();
        QCOMPARE(loaded.muteShortcut(), "Ctrl+Shift+M");
        QCOMPARE(loaded.disconnectShortcut(), "Ctrl+Shift+H");
        QFile persisted(file);
        QVERIFY(persisted.open(QIODevice::ReadOnly));
        const auto bytes = persisted.readAll();
        QVERIFY(bytes.contains("toggle-mute"));
        QVERIFY(!bytes.contains("token"));
    }
    void invalidSavedBindingsCannotBecomeConflictingShortcuts() {
        QTemporaryDir directory;
        const auto path = directory.path() + "/shortcuts.ini";
        QSettings stored(path, QSettings::IniFormat);
        stored.setValue("bindings/toggle-mute", "F8");
        stored.setValue("bindings/toggle-deafen", "F8");
        stored.setValue("bindings/disconnect-voice", "plain text");
        stored.sync();
        ShortcutSettings settings(path);
        QCOMPARE(settings.muteShortcut(), "F8");
        QVERIFY(settings.deafenShortcut().isEmpty());
        QCOMPARE(settings.disconnectShortcut(), "Ctrl+Shift+H");
    }
};

QTEST_GUILESS_MAIN(ControlTest)
#include "control_test.moc"
