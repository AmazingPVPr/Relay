#include "DiscordClient.h"
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocketServer>
#include <QtTest>

// Generated main-gateway/HTTP peers only. No microphone, real calls, account
// data, credentials, or Discord endpoints are used by this fixture.
namespace {
class CallHttpFixture final : public QTcpServer {
public:
    struct Response { int status = 204; QByteArray body; int delay = 0; };
    QQueue<Response> responses; QList<QByteArray> requests;
    CallHttpFixture() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    const auto bytes = socket->property("request").toByteArray() + socket->readAll(); socket->setProperty("request", bytes);
                    const int end = bytes.indexOf("\r\n\r\n"); if (end < 0 || socket->property("handled").toBool()) return;
                    int length = 0; for (const auto &line : bytes.left(end).split('\n')) if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("handled", true); requests.append(bytes); const auto response = responses.isEmpty() ? Response{} : responses.dequeue();
                    QTimer::singleShot(response.delay, socket, [socket, response] {
                        if (socket->state() != QAbstractSocket::ConnectedState) return;
                        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(response.body.size()) + "\r\n\r\n" + response.body); socket->disconnectFromHost();
                    });
                });
            }
        });
    }
    QString base() const { return "http://127.0.0.1:" + QString::number(serverPort()); }
    static QJsonObject body(const QByteArray &request) { return QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4)).object(); }
};
class CallGatewayFixture final : public QWebSocketServer {
public:
    QList<QJsonObject> packets;
    CallGatewayFixture() : QWebSocketServer("Generated direct-call gateway", QWebSocketServer::NonSecureMode) {
        connect(this, &QWebSocketServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                socket->setParent(this);
                connect(socket, &QWebSocket::textMessageReceived, this, [this](const QString &text) { packets.append(QJsonDocument::fromJson(text.toUtf8()).object()); });
            }
        });
    }
};
}
class DirectCallTest final : public QObject {
    Q_OBJECT
    static QJsonObject dm(const QString &id = "10", const QString &recipient = "81") {
        return {{"id", id}, {"type", 1}, {"recipients", QJsonArray{QJsonObject{{"id", recipient}, {"username", "Generated caller"}}}}};
    }
    static void prepare(DiscordClient &client, const CallHttpFixture &http, CallGatewayFixture &gateway, bool friendTarget = true) {
        client.m_apiBase = http.base(); client.m_token = "generated-direct-call-token"; client.m_currentUserId = "42"; client.m_connected = true;
        client.m_currentGuildId = "900"; client.acceptDm(dm());
        if (friendTarget) client.acceptRelationship({{"id", "81"}, {"type", 1}});
        client.m_socket.open(QUrl("ws://127.0.0.1:" + QString::number(gateway.serverPort())));
        QTRY_COMPARE(client.m_socket.state(), QAbstractSocket::ConnectedState);
    }
    static void simulateNativeJoin(DiscordClient &client) {
        connect(&client, &DiscordClient::directCallRequested, &client, [&client](const QString &id, const QString &) {
            client.setVoiceGuild(id); QVERIFY(client.sendVoiceState(id, id, false, false));
        });
    }
private slots:
    void outgoingCallUsesPrivateGatewayAllocationAndRingsOnlyOnceAfterVoiceReady() {
        CallHttpFixture http; CallGatewayFixture gateway; QVERIFY(http.listen(QHostAddress::LocalHost, 0)); QVERIFY(gateway.listen(QHostAddress::LocalHost, 0));
        http.responses.enqueue({200, "[]"}); http.responses.enqueue({204, {}}); http.responses.enqueue({204, {}});
        DiscordClient client; prepare(client, http, gateway); simulateNativeJoin(client); QSignalSpy joins(&client, &DiscordClient::directCallRequested);
        QVERIFY(client.startDirectCall("81")); QTRY_COMPARE(gateway.packets.size(), 2); QCOMPARE(joins.size(), 1);
        QCOMPARE(gateway.packets.at(0).value("op").toInt(), 13); QCOMPARE(gateway.packets.at(0).value("d").toObject().value("channel_id").toString(), QString("10"));
        const auto join = gateway.packets.at(1).value("d").toObject(); QVERIFY(join.value("guild_id").isNull()); QCOMPARE(join.value("channel_id").toString(), QString("10"));
        QTRY_COMPARE(http.requests.size(), 1); QVERIFY(http.requests.first().startsWith("GET /channels/10/messages?limit=50 "));
        client.onDirectCallConnected("10"); client.onDirectCallConnected("10"); QTRY_COMPARE(http.requests.size(), 2);
        QVERIFY(http.requests.at(1).startsWith("POST /channels/10/call/ring ")); QCOMPARE(CallHttpFixture::body(http.requests.at(1)).value("recipients").toArray(), QJsonArray{"81"});
        QVERIFY(client.startDirectCall("81")); QCOMPARE(joins.size(), 1); QTest::qWait(30); QCOMPARE(http.requests.size(), 2);
        QVERIFY(client.sendVoiceState("10", "10", false, true)); QTRY_COMPARE(gateway.packets.size(), 3);
        const auto deafen = gateway.packets.last().value("d").toObject(); QVERIFY(deafen.value("guild_id").isNull()); QVERIFY(deafen.value("self_mute").toBool()); QVERIFY(deafen.value("self_deaf").toBool());
        QVERIFY(client.sendVoiceState("10", "", false, false)); QTRY_COMPARE(http.requests.size(), 3);
        QVERIFY(http.requests.last().startsWith("POST /channels/10/call/stop-ringing ")); QTRY_COMPARE(gateway.packets.size(), 4);
        const auto leave = gateway.packets.last().value("d").toObject(); QVERIFY(leave.value("guild_id").isNull()); QVERIFY(leave.value("channel_id").isNull());
    }
    void privateVoiceEventsAreNormalizedButUnknownEventsCannotAcquireAnAllocation() {
        DiscordClient client; client.m_currentUserId = "42"; client.acceptDm(dm()); client.m_directCallChannel = "10"; client.m_voiceGuildId = "10";
        QSignalSpy states(&client, &DiscordClient::voiceStateReceived), servers(&client, &DiscordClient::voiceServerReceived);
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", QJsonValue::Null}, {"channel_id", "10"}, {"user_id", "42"}, {"session_id", "generated-session"}});
        QCOMPARE(states.size(), 1); QCOMPARE(states.first().at(0).toJsonObject().value("guild_id").toString(), QString("10")); QCOMPARE(client.currentVoiceSessionId(), QString("generated-session"));
        client.dispatch("VOICE_SERVER_UPDATE", {{"guild_id", QJsonValue::Null}, {"channel_id", "10"}, {"token", "generated-voice-token"}, {"endpoint", "generated.discord.media"}});
        QCOMPARE(servers.size(), 1); QCOMPARE(servers.first().at(0).toJsonObject().value("guild_id").toString(), QString("10"));
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", QJsonValue::Null}, {"channel_id", "999"}, {"user_id", "42"}});
        client.dispatch("VOICE_SERVER_UPDATE", {{"guild_id", QJsonValue::Null}, {"channel_id", "999"}}); QCOMPARE(states.size(), 2); QCOMPARE(servers.size(), 2);
        QVERIFY(states.last().at(0).toJsonObject().value("guild_id").isNull()); QVERIFY(servers.last().at(0).toJsonObject().value("guild_id").isNull());
    }
    void unavailableStrangerNeverInterruptsTheCurrentGuildCall() {
        CallHttpFixture http; CallGatewayFixture gateway; QVERIFY(http.listen(QHostAddress::LocalHost, 0)); QVERIFY(gateway.listen(QHostAddress::LocalHost, 0));
        http.responses.enqueue({200, "{\"ringable\":false}"}); DiscordClient client; prepare(client, http, gateway, false); client.m_voiceGuildId = "900";
        QSignalSpy joins(&client, &DiscordClient::directCallRequested); QVERIFY(client.startDirectCall("81")); QTRY_VERIFY(!client.directCallStarting());
        QCOMPARE(joins.size(), 0); QCOMPARE(gateway.packets.size(), 0); QCOMPARE(client.m_voiceGuildId, QString("900")); QCOMPARE(client.currentGuildId(), QString("900"));
        QCOMPARE(http.requests.size(), 1); QVERIFY(http.requests.first().startsWith("GET /channels/10/call ")); QVERIFY(!client.error().isEmpty());
    }
    void activePeerCallIsJoinedWithoutAnotherRingAndFinalDeleteEndsIt() {
        CallHttpFixture http; CallGatewayFixture gateway; QVERIFY(http.listen(QHostAddress::LocalHost, 0)); QVERIFY(gateway.listen(QHostAddress::LocalHost, 0)); http.responses.enqueue({200, "[]"});
        DiscordClient client; prepare(client, http, gateway, false); simulateNativeJoin(client);
        client.dispatch("CALL_CREATE", {{"channel_id", "10"}, {"voice_states", QJsonArray{QJsonObject{{"user_id", "81"}, {"self_mute", false}}}}});
        QVERIFY(client.m_voiceStates.value("10").contains("81")); QVERIFY(client.startDirectCall("81")); client.onDirectCallConnected("10");
        QTRY_COMPARE(http.requests.size(), 1); QVERIFY(http.requests.first().startsWith("GET /channels/10/messages"));
        QSignalSpy ends(&client, &DiscordClient::directCallEnded); client.dispatch("CALL_DELETE", {{"channel_id", "10"}, {"unavailable", true}}); QCOMPARE(ends.size(), 0); QVERIFY(client.m_privateCalls.contains("10"));
        client.dispatch("CALL_DELETE", {{"channel_id", "10"}}); QCOMPARE(ends.size(), 1); QVERIFY(!client.m_privateCalls.contains("10")); QVERIFY(client.m_voiceStates.value("10").isEmpty());
    }
    void privateLeaveAcknowledgementsCannotCancelANewerPrivateCall() {
        CallHttpFixture http; CallGatewayFixture gateway; QVERIFY(http.listen(QHostAddress::LocalHost, 0)); QVERIFY(gateway.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, http, gateway); client.acceptDm(dm("20", "82")); client.m_directCallChannel = "20"; client.m_voiceGuildId = "20";
        QVERIFY(client.sendVoiceState("10", "", true, false)); QVERIFY(client.sendVoiceState("20", "20", false, false));
        QSignalSpy states(&client, &DiscordClient::voiceStateReceived);
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", QJsonValue::Null}, {"channel_id", QJsonValue::Null}, {"user_id", "42"}});
        QCOMPARE(states.size(), 1); QCOMPARE(states.first().at(0).toJsonObject().value("guild_id").toString(), QString("10")); QCOMPARE(client.m_directCallChannel, QString("20"));
        QSignalSpy rejected(&client, &DiscordClient::voiceStateRequestRejected); QVERIFY(!client.sendVoiceState("10", "20", false, false)); QCOMPARE(rejected.size(), 1);
    }
    void privateVoiceIntentSurvivesMainGatewayResumeWithANullWireGuild() {
        CallHttpFixture http; CallGatewayFixture gateway; QVERIFY(http.listen(QHostAddress::LocalHost, 0)); QVERIFY(gateway.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, http, gateway); client.m_connected = false; client.m_connecting = true;
        QVERIFY(client.sendVoiceState("10", "10", true, false)); QCOMPARE(client.m_pendingVoiceState.value("guild_id").toString(), QString("10")); QCOMPARE(gateway.packets.size(), 0);
        client.m_connected = true; client.flushPendingVoiceState(); QTRY_COMPARE(gateway.packets.size(), 1);
        QVERIFY(gateway.packets.first().value("d").toObject().value("guild_id").isNull()); QVERIFY(client.m_pendingVoiceState.isEmpty());
    }
    void ringFailureEndsTheAttemptAndDoesNotRingAgain() {
        CallHttpFixture http; CallGatewayFixture gateway; QVERIFY(http.listen(QHostAddress::LocalHost, 0)); QVERIFY(gateway.listen(QHostAddress::LocalHost, 0));
        http.responses.enqueue({200, "[]"}); http.responses.enqueue({403, "{\"message\":\"Generated call failure\"}"});
        DiscordClient client; prepare(client, http, gateway); simulateNativeJoin(client); QSignalSpy ends(&client, &DiscordClient::directCallEnded);
        QVERIFY(client.startDirectCall("81")); QTRY_COMPARE(http.requests.size(), 1); client.onDirectCallConnected("10"); QTRY_COMPARE(ends.size(), 1);
        client.onDirectCallConnected("10"); QTest::qWait(40); QCOMPARE(http.requests.size(), 2); QVERIFY(!client.error().isEmpty());
    }
    void delayedDmCreationCannotStartACallAfterLogout() {
        CallHttpFixture http; CallGatewayFixture gateway; QVERIFY(http.listen(QHostAddress::LocalHost, 0)); QVERIFY(gateway.listen(QHostAddress::LocalHost, 0));
        http.responses.enqueue({200, QJsonDocument(dm("20", "82")).toJson(), 150});
        DiscordClient client; prepare(client, http, gateway); client.acceptRelationship({{"id", "82"}, {"type", 1}}); QSignalSpy joins(&client, &DiscordClient::directCallRequested);
        QVERIFY(client.startDirectCall("82")); QTRY_COMPARE(http.requests.size(), 1); client.logout(); QTest::qWait(220);
        QCOMPARE(joins.size(), 0); QVERIFY(!client.directCallStarting()); QVERIFY(client.m_directCallChannel.isEmpty());
    }
    void normalGuildVoiceWireAndEventsRemainUnchanged() {
        CallHttpFixture http; CallGatewayFixture gateway; QVERIFY(http.listen(QHostAddress::LocalHost, 0)); QVERIFY(gateway.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, http, gateway); QVERIFY(client.sendVoiceState("900", "901", false, false)); QTRY_COMPARE(gateway.packets.size(), 1);
        QCOMPARE(gateway.packets.first().value("d").toObject().value("guild_id").toString(), QString("900"));
        QSignalSpy servers(&client, &DiscordClient::voiceServerReceived); const QJsonObject server{{"guild_id", "900"}, {"endpoint", "generated.discord.media"}, {"token", "generated-token"}};
        client.dispatch("VOICE_SERVER_UPDATE", server); QCOMPARE(servers.first().at(0).toJsonObject(), server);
    }
};
QTEST_GUILESS_MAIN(DirectCallTest)
#include "direct_call_test.moc"
