#include "ContextActions.h"
#include "DiscordClient.h"
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace {
class ContextHttpFixture final : public QTcpServer {
public:
    struct Response { int status = 204; QByteArray body; int delay = 0; };
    QQueue<Response> responses; QList<QByteArray> requests;
    ContextHttpFixture() {
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
}
class ContextActionsTest final : public QObject {
    Q_OBJECT
    static void prepare(DiscordClient &client, const ContextHttpFixture &fixture) {
        client.m_apiBase = fixture.base(); client.m_token = "generated-context-token"; client.m_connected = true; client.m_currentUserId = "42";
        client.m_currentGuildId = "999";
        client.acceptDm({{"id", "10"}, {"type", 1}, {"last_message_id", "100"}, {"recipients", QJsonArray{QJsonObject{{"id", "81"}, {"username", "Generated"}}}}});
        client.acceptDm({{"id", "20"}, {"type", 1}, {"last_message_id", "200"}, {"recipients", QJsonArray{QJsonObject{{"id", "82"}, {"username", "Fixture"}}}}});
        client.acceptGuild({{"id", "900"}, {"name", "Generated server"}, {"channels", QJsonArray{
            QJsonObject{{"id", "901"}, {"type", 0}, {"name", "general"}}, QJsonObject{{"id", "902"}, {"type", 0}, {"name", "muted"}}}}});
        client.m_currentGuildId = "900";
    }
private slots:
    void dmOrganizationIsPersistentAccountScopedAndSurvivesNewMessages() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); ContextHttpFixture fixture; DiscordClient client; prepare(client, fixture);
        const auto path = directory.filePath("context.ini");
        {
            ContextActions actions(&client, path); QCOMPARE(actions.dmChannelForUser("81"), QString("10"));
            QVERIFY(actions.setDmPinned("10", true)); QVERIFY(actions.dmInfo("10").value("pinned").toBool()); QCOMPARE(client.dms()->get(0).value("recordId").toString(), QString("10"));
            QVERIFY(actions.setDmFavorite("20", true));
            client.recordDmActivity({{"id", "300"}, {"channel_id", "20"}}); QCOMPARE(client.dms()->get(0).value("recordId").toString(), QString("10"));
        }
        ContextActions restored(&client, path); QVERIFY(restored.dmInfo("10").value("pinned").toBool()); QVERIFY(restored.dmInfo("20").value("favorite").toBool());
        client.m_currentUserId = "43"; emit client.stateChanged(); QVERIFY(!restored.dmInfo("10").value("pinned").toBool());
        client.m_currentUserId = "42"; emit client.stateChanged(); QVERIFY(restored.dmInfo("10").value("pinned").toBool());
        QVERIFY(!restored.setDmPinned("invalid", true));
    }
    void markingDmReadUsesExactWatermarkAndRetainsNewArrivals() {
        QTemporaryDir directory; ContextHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0)); fixture.responses.enqueue({200, "{}", 100});
        DiscordClient client; prepare(client, fixture); client.m_unread.insert("10", 3); client.syncDm("10"); ContextActions actions(&client, directory.filePath("context.ini"));
        QVERIFY(actions.markDmRead("10")); QTRY_COMPARE(fixture.requests.size(), 1);
        QVERIFY(fixture.requests.first().startsWith("POST /channels/10/messages/100/ack "));
        client.recordDmActivity({{"id", "101"}, {"channel_id", "10"}}); client.m_unread.insert("10", 4);
        QTRY_VERIFY(!actions.busy()); QCOMPARE(client.m_unread.value("10"), 4);
        QVERIFY(actions.markDmRead("10")); QTRY_VERIFY(!actions.busy()); QCOMPARE(client.m_unread.value("10"), 0);
    }
    void muteDurationsUseAccountNotificationRoutesAndPreserveOtherDms() {
        QTemporaryDir directory; ContextHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, fixture); client.acceptNotificationSettings({{"guild_id", QJsonValue::Null}, {"channel_overrides", QJsonArray{QJsonObject{{"channel_id", "20"}, {"muted", true}}}}});
        ContextActions actions(&client, directory.filePath("context.ini")); QVERIFY(actions.muteDm("10", 900)); QTRY_VERIFY(!actions.busy());
        QVERIFY(fixture.requests.first().startsWith("PATCH /users/@me/guilds/@me/settings "));
        const auto override = ContextHttpFixture::body(fixture.requests.first()).value("channel_overrides").toObject().value("10").toObject();
        QVERIFY(override.value("muted").toBool()); QCOMPARE(override.value("mute_config").toObject().value("selected_time_window").toInt(), 900);
        QVERIFY(actions.dmInfo("10").value("muted").toBool()); QVERIFY(actions.dmInfo("20").value("muted").toBool());
        QVERIFY(actions.muteDm("10", -1)); QTRY_VERIFY(!actions.busy()); QVERIFY(!actions.dmInfo("10").value("muted").toBool());
        QVERIFY(actions.muteGuild("900", 0)); QTRY_VERIFY(!actions.busy()); QVERIFY(fixture.requests.last().startsWith("PATCH /users/@me/guilds/900/settings ")); QVERIFY(actions.guildInfo("900").value("muted").toBool());
        QVERIFY(!actions.muteGuild("900", 37));
        QVERIFY(actions.setGuildNotificationLevel("900", 1)); QTRY_VERIFY(!actions.busy()); QCOMPARE(actions.guildInfo("900").value("notificationLevel").toInt(), 1);
    }
    void hideMutedActuallyFiltersChannelsAndPersists() {
        QTemporaryDir directory; ContextHttpFixture fixture; DiscordClient client; prepare(client, fixture);
        client.acceptNotificationSettings({{"guild_id", "900"}, {"channel_overrides", QJsonArray{QJsonObject{{"channel_id", "902"}, {"muted", true}}}}});
        client.rebuildChannels(false); QCOMPARE(client.channels()->count(), 2);
        ContextActions actions(&client, directory.filePath("context.ini")); QVERIFY(actions.setGuildHideMuted("900", true));
        QCOMPARE(client.channels()->count(), 1); QCOMPARE(client.channels()->get(0).value("recordId").toString(), QString("901"));
        QVERIFY(actions.guildInfo("900").value("hideMuted").toBool()); QVERIFY(!actions.guildInfo("900").value("showAllAvailable").toBool());
        QVERIFY(!actions.setGuildShowAll("900", false)); QVERIFY(actions.setGuildHideMuted("900", false)); QCOMPARE(client.channels()->count(), 2);
    }
    void closeDmNeverLeavesGroupOrDeletesAChannel() {
        QTemporaryDir directory; ContextHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, fixture); ContextActions actions(&client, directory.filePath("context.ini"));
        client.acceptDm({{"id", "30"}, {"type", 3}, {"name", "Generated group"}});
        QVERIFY(!actions.closeDm("30")); QVERIFY(!actions.closeDm("901")); QVERIFY(actions.closeDm("10")); QTRY_VERIFY(!actions.busy());
        QCOMPARE(fixture.requests.size(), 1); QVERIFY(fixture.requests.first().startsWith("DELETE /channels/10 "));
        QVERIFY(!client.m_dmObjects.contains("10")); QVERIFY(client.m_dmObjects.contains("20")); QVERIFY(client.m_dmObjects.contains("30"));
    }
    void serverReadAndFailuresAreExplicitAndNeverRetriedAsMutations() {
        QTemporaryDir directory; ContextHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({204, {}, 100}); fixture.responses.enqueue({403, "{\"message\":\"Generated failure\"}"});
        DiscordClient client; prepare(client, fixture); client.m_unread.insert("900", 5); client.m_unread.insert("901", 5);
        ContextActions actions(&client, directory.filePath("context.ini")); QVERIFY(actions.markGuildRead("900")); QTRY_COMPARE(fixture.requests.size(), 1);
        client.m_unread.insert("900", 6); client.m_unread.insert("901", 6); QTRY_VERIFY(!actions.busy()); QCOMPARE(client.m_unread.value("900"), 1); QCOMPARE(client.m_unread.value("901"), 1);
        QVERIFY(fixture.requests.first().startsWith("POST /guilds/900/ack "));
        QVERIFY(actions.muteDm("10", 0)); QTRY_VERIFY(!actions.busy()); QVERIFY(!actions.error().isEmpty()); QVERIFY(!actions.dmInfo("10").value("muted").toBool());
        QCOMPARE(fixture.requests.size(), 2);
    }
    void ownServerNicknameUsesSelfRouteAndGlobalNameRemainsSeparate() {
        QTemporaryDir directory; ContextHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({200, "{\"nick\":\"Server name\"}"}); fixture.responses.enqueue({200, "{\"nick\":null}"});
        DiscordClient client; prepare(client, fixture); client.m_users.insert("42", {{"id", "42"}, {"username", "Global name"}});
        ContextActions actions(&client, directory.filePath("context.ini")); QVERIFY(actions.setGuildNickname("900", "Server name")); QTRY_VERIFY(!actions.busy());
        QVERIFY(fixture.requests.first().startsWith("PATCH /guilds/900/members/@me ")); QCOMPARE(ContextHttpFixture::body(fixture.requests.first()).value("nick").toString(), QString("Server name"));
        QCOMPARE(actions.guildInfo("900").value("nickname").toString(), QString("Server name")); QCOMPARE(client.m_users.value("42").value("username").toString(), QString("Global name"));
        QVERIFY(actions.setGuildNickname("900", "")); QTRY_VERIFY(!actions.busy()); QVERIFY(ContextHttpFixture::body(fixture.requests.last()).value("nick").isNull()); QVERIFY(actions.guildInfo("900").value("nickname").toString().isEmpty());
        QVERIFY(!actions.setGuildNickname("unknown", "Name")); QVERIFY(!actions.setGuildNickname("900", QString(33, 'x')));
    }
    void laterNotificationAndDmEventsWinOverSlowResponses() {
        QTemporaryDir directory; ContextHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({204, {}, 100}); fixture.responses.enqueue({204, {}, 100});
        DiscordClient client; prepare(client, fixture); ContextActions actions(&client, directory.filePath("context.ini"));
        QVERIFY(actions.muteDm("10", 0)); QTRY_COMPARE(fixture.requests.size(), 1);
        client.acceptNotificationSettings({{"guild_id", QJsonValue::Null}, {"channel_overrides", QJsonArray{QJsonObject{{"channel_id", "10"}, {"muted", false}}, QJsonObject{{"channel_id", "20"}, {"muted", true}}}}});
        QTRY_VERIFY(!actions.busy()); QVERIFY(!actions.dmInfo("10").value("muted").toBool()); QVERIFY(actions.dmInfo("20").value("muted").toBool());
        QVERIFY(actions.closeDm("10")); QTRY_COMPARE(fixture.requests.size(), 2);
        client.recordDmActivity({{"id", "101"}, {"channel_id", "10"}}); QTRY_VERIFY(!actions.busy()); QVERIFY(client.m_dmObjects.contains("10"));
    }
};
QTEST_GUILESS_MAIN(ContextActionsTest)
#include "context_actions_test.moc"
