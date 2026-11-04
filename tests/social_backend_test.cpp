#include "DiscordClient.h"
#include "ContextActions.h"
#include <QAbstractItemModelTester>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocketServer>
#include <QtTest>

// Generated data and an isolated localhost peer only. No real Discord session,
// requests, messages, account changes, credentials, or account files are used.
namespace {
class SocialHttpFixture final : public QTcpServer {
public:
    struct Response { int status = 204; QByteArray body; int delay = 0; };
    QQueue<Response> responses;
    QList<QByteArray> requests;
    SocialHttpFixture() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    const auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    int length = 0;
                    for (const auto &line : bytes.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("handled", true); requests.append(bytes);
                    const auto response = responses.isEmpty() ? Response{} : responses.dequeue();
                    QTimer::singleShot(response.delay, socket, [socket, response] {
                        if (socket->state() != QAbstractSocket::ConnectedState) return;
                        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(response.body.size()) + "\r\n\r\n" + response.body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
    QString base() const { return "http://127.0.0.1:" + QString::number(serverPort()); }
    static QJsonObject body(const QByteArray &request) { return QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4)).object(); }
};
}

class SocialBackendTest final : public QObject {
    Q_OBJECT
    static QJsonObject user(const QString &id, const QString &name) { return {{"id", id}, {"username", name}}; }
    static QJsonObject relation(const QString &id, int type, const QString &name = "Generated person") {
        return {{"id", id}, {"type", type}, {"user", user(id, name)}};
    }
    static QJsonObject dm(const QString &id, const QString &recipient, bool request, bool spam = false) {
        return {{"id", id}, {"type", 1}, {"is_message_request", request}, {"is_spam", spam},
            {"recipients", QJsonArray{user(recipient, "Generated sender")}}};
    }
    static QJsonObject message(const QString &id, const QString &channel, const QString &content = "Generated text") {
        return {{"id", id}, {"channel_id", channel}, {"content", content}, {"author", user("84", "Generated sender")}, {"timestamp", "2026-10-25T12:00:00Z"}};
    }
    static void configure(DiscordClient &client, const SocialHttpFixture &fixture) {
        client.m_apiBase = fixture.base(); client.m_token = "generated-social-test-token";
        client.m_connected = true; client.m_currentUserId = "42";
    }
private slots:
    void readyAndEventsMaintainSeparateFriendsAndPendingModels() {
        DiscordClient client; client.m_currentGuildId = "999";
        client.dispatch("READY", {{"user", user("42", "Self")}, {"users", QJsonArray{user("81", "Alpha"), user("82", "Beta")}},
            {"relationships", QJsonArray{QJsonObject{{"id", "81"}, {"type", 1}}, QJsonObject{{"id", "82"}, {"type", 3}}, relation("83", 4), relation("85", 2)}}});
        QCOMPARE(client.friends().size(), 1); QCOMPARE(client.friends().first().toMap().value("name").toString(), QString("Alpha"));
        QCOMPARE(client.pendingFriends().size(), 2); QVERIFY(client.m_blockedUsers.contains("85"));
        const QJsonObject presence{{"user_id", "81"}, {"status", "idle"},
            {"activities", QJsonArray{QJsonObject{{"type", 0}, {"name", "Generated game"}}}}};
        client.dispatch("READY_SUPPLEMENTAL", {{"merged_presences", QJsonObject{{"friends", QJsonArray{presence}}}}});
        auto row = client.friends().first().toMap(); QCOMPARE(row.value("status").toString(), QString("idle"));
        QCOMPARE(row.value("activity").toString(), QString("Playing Generated game"));
        client.dispatch("RELATIONSHIP_UPDATE", {{"id", "82"}, {"type", 1}});
        QCOMPARE(client.friends().size(), 2); QCOMPARE(client.pendingFriends().size(), 1);
        client.dispatch("RELATIONSHIP_REMOVE", {{"id", "81"}}); QCOMPARE(client.friends().size(), 1);
        client.dispatch("RELATIONSHIP_ADD", {{"id", "82"}, {"type", 2}});
        QCOMPARE(client.friends().size(), 0); QVERIFY(client.m_blockedUsers.contains("82"));
        client.dispatch("RELATIONSHIP_ADD", {{"id", "bad"}, {"type", 1}});
        client.dispatch("RELATIONSHIP_ADD", {{"id", "88"}, {"type", 1}, {"user", user("89", "Wrong ID")}});
        QCOMPARE(client.friends().size(), 0);
    }
    void friendMetadataAndVoicePresenceStayCurrent() {
        DiscordClient client; client.m_currentUserId = "42"; client.acceptRelationship(relation("81", 1, "Before"));
        client.dispatch("USER_UPDATE", {{"id", "81"}, {"global_name", "After"}});
        QCOMPARE(client.friends().first().toMap().value("name").toString(), QString("After"));
        client.acceptPresence({}, {{"user_id", "81"}, {"status", "dnd"}});
        QCOMPARE(client.friends().first().toMap().value("status").toString(), QString("dnd"));
        client.acceptVoiceState({{"guild_id", "900"}, {"user_id", "81"}, {"channel_id", "901"}});
        QVERIFY(client.friends().first().toMap().value("inVoice").toBool());
        client.acceptVoiceState({{"guild_id", "900"}, {"user_id", "81"}, {"channel_id", QJsonValue::Null}});
        QVERIFY(!client.friends().first().toMap().value("inVoice").toBool());
    }
    void requestAndSpamClassificationDoesNotPolluteNormalDms() {
        DiscordClient client; client.m_currentUserId = "42"; client.m_currentGuildId = "900";
        QAbstractItemModelTester tester(client.dms(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        client.acceptDm(dm("10", "81", false)); client.acceptDm(dm("20", "82", true)); client.acceptDm(dm("30", "83", true, true));
        QCOMPARE(client.dms()->count(), 1); QCOMPARE(client.messageRequests().size(), 1); QCOMPARE(client.spamRequests().size(), 1);
        client.acceptDm({{"id", "30"}, {"is_message_request", false}});
        QCOMPARE(client.dms()->count(), 2); QCOMPARE(client.spamRequests().size(), 0);
        client.dispatch("CHANNEL_UPDATE", {{"id", "20"}, {"is_spam", true}});
        QCOMPARE(client.messageRequests().size(), 0); QCOMPARE(client.spamRequests().size(), 1);
        client.dispatch("CHANNEL_DELETE", {{"id", "20"}}); QCOMPARE(client.spamRequests().size(), 0);
        client.dispatch("MESSAGE_CREATE", message("100", "30")); QCOMPARE(client.unreadDms()->count(), 1);
        client.acceptDm(dm("40", "84", true)); client.dispatch("MESSAGE_CREATE", message("101", "40"));
        QVERIFY(client.unreadDms()->byId("40").isEmpty()); QVERIFY(!client.m_unread.contains("40"));
    }
    void selectHomeNeverLoadsOrMarksAnotherConversationRead() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; configure(client, fixture); client.m_currentGuildId = "900";
        client.acceptDm(dm("10", "81", false)); client.m_unread.insert("10", 3); client.syncDm("10");
        client.selectHome(); QCOMPARE(client.currentGuildId(), QString()); QCOMPARE(client.currentChannelId(), QString());
        QCOMPARE(client.dms()->byId("10").value("unread").toInt(), 3);
        QTest::qWait(40); QCOMPARE(fixture.requests.size(), 0);
    }
    void lateDmSnapshotPreservesHomeAndDoesNotAcknowledgeMessages() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({200, QJsonDocument(QJsonArray{dm("10", "81", false)}).toJson(), 80});
        DiscordClient client; configure(client, fixture); client.m_currentGuildId = "900";
        client.m_unread.insert("10", 3);
        QSignalSpy ready(&client, &DiscordClient::modelsReady);
        client.fetchDms(); QTRY_COMPARE(fixture.requests.size(), 1);
        client.selectHome();
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(client.currentGuildId(), QString()); QCOMPARE(client.currentChannelId(), QString());
        QCOMPARE(client.channels()->count(), 1); QCOMPARE(client.m_unread.value("10"), 3);
        QTest::qWait(40); QCOMPARE(fixture.requests.size(), 1);
        QVERIFY(fixture.requests.first().startsWith("GET /users/@me/channels "));
        // Receiving another conversation while Home is visible is not a click.
        client.dispatch("CHANNEL_CREATE", dm("20", "82", false));
        QCOMPARE(client.currentChannelId(), QString());
        // Explicitly opening that person still selects the intended conversation.
        client.m_token.clear(); client.openDirectMessage("82");
        QCOMPARE(client.currentGuildId(), QString()); QCOMPARE(client.currentChannelId(), QString("20"));
    }
    void initialAndLateGuildSnapshotsPreserveHome() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        const QJsonObject guild{{"id", "900"}, {"name", "Generated server"}, {"channels", QJsonArray{
            QJsonObject{{"id", "901"}, {"type", 0}, {"name", "general"}}}}};
        fixture.responses.enqueue({200, QJsonDocument(QJsonArray{guild}).toJson(), 80});
        DiscordClient client; configure(client, fixture);
        QSignalSpy ready(&client, &DiscordClient::modelsReady);
        client.fetchGuilds(); QTRY_COMPARE(fixture.requests.size(), 1); client.selectHome();
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(client.guilds()->count(), 1);
        QVERIFY(client.currentGuildId().isEmpty()); QVERIFY(client.currentChannelId().isEmpty());
        QTest::qWait(40); QCOMPARE(fixture.requests.size(), 1);

        client.m_token.clear();
        client.dispatch("READY", {{"user", user("42", "Generated self")}, {"guilds", QJsonArray{guild}},
            {"private_channels", QJsonArray{dm("10", "81", false)}}});
        QVERIFY(client.currentGuildId().isEmpty()); QVERIFY(client.currentChannelId().isEmpty());
        QCOMPARE(client.channels()->count(), 1);
        client.dispatch("GUILD_CREATE", {{"id", "910"}, {"name", "Another generated server"}});
        client.dispatch("GUILD_UPDATE", {{"id", "900"}, {"name", "Updated generated server"}});
        QVERIFY(client.currentGuildId().isEmpty()); QVERIFY(client.currentChannelId().isEmpty());
        client.selectGuild("900");
        QCOMPARE(client.currentGuildId(), QString("900")); QCOMPARE(client.currentChannelId(), QString("901"));
        client.dispatch("READY", {{"user", user("42", "Generated self")}, {"guilds", QJsonArray{guild}}});
        QCOMPARE(client.currentGuildId(), QString("900")); QCOMPARE(client.currentChannelId(), QString("901"));
    }
    void previewIsAnIsolatedReadWithNoAcknowledgementOrAccept() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({200, QJsonDocument(QJsonArray{message("101", "20", "Second"), message("100", "20", "First")}).toJson()});
        DiscordClient client; configure(client, fixture); client.m_currentGuildId = "900";
        client.acceptDm(dm("20", "84", true)); client.m_unread.insert("20", 3);
        QVERIFY(client.previewMessageRequest("20")); QTRY_VERIFY(!client.requestPreviewLoading());
        QCOMPARE(client.requestPreviewMessages().size(), 2);
        QCOMPARE(client.requestPreviewMessages().first().toMap().value("rawContent").toString(), QString("First"));
        QCOMPARE(client.currentGuildId(), QString("900")); QCOMPARE(client.currentChannelId(), QString()); QCOMPARE(client.messages()->count(), 0);
        QCOMPARE(client.m_unread.value("20"), 3); QCOMPARE(client.messageRequests().size(), 1); QCOMPARE(client.friends().size(), 0);
        QCOMPARE(fixture.requests.size(), 1); QVERIFY(fixture.requests.first().startsWith("GET /channels/20/messages?limit=50 "));
        QVERIFY(client.previewMessageRequest("")); QVERIFY(client.selectedMessageRequest().isEmpty()); QVERIFY(client.requestPreviewMessages().isEmpty());
    }
    void oldPreviewResponsesCannotReopenAClosedOrDifferentRequest() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({200, QJsonDocument(QJsonArray{message("101", "20", "Stale")}).toJson(), 150});
        fixture.responses.enqueue({200, QJsonDocument(QJsonArray{message("102", "30", "Current")}).toJson()});
        DiscordClient client; configure(client, fixture); client.acceptDm(dm("20", "84", true)); client.acceptDm(dm("30", "85", true));
        QVERIFY(client.previewMessageRequest("20")); QTRY_COMPARE(fixture.requests.size(), 1);
        QVERIFY(client.previewMessageRequest("30")); QTRY_VERIFY(!client.requestPreviewLoading());
        QTest::qWait(220); QCOMPARE(client.selectedMessageRequest().value("channelId").toString(), QString("30"));
        QCOMPARE(client.requestPreviewMessages().size(), 1); QCOMPARE(client.requestPreviewMessages().first().toMap().value("rawContent").toString(), QString("Current"));
    }
    void relationshipMutationsUseExactTargetAndValidateCurrentType() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; configure(client, fixture);
        client.acceptRelationship(relation("81", 3)); client.acceptRelationship(relation("82", 4)); client.acceptRelationship(relation("83", 1));
        QVERIFY(!client.acceptFriendRequest("82")); QVERIFY(!client.dismissFriendRequest("83")); QVERIFY(!client.removeFriend("81"));
        QCOMPARE(fixture.requests.size(), 0); QVERIFY(client.acceptFriendRequest("81")); QVERIFY(!client.acceptFriendRequest("81"));
        QTRY_VERIFY(!client.socialBusy()); QCOMPARE(client.m_relationships.value("81").value("type").toInt(), 1);
        QVERIFY(fixture.requests.at(0).startsWith("PUT /users/@me/relationships/81 ")); QCOMPARE(SocialHttpFixture::body(fixture.requests.at(0)), QJsonObject{});
        QVERIFY(client.dismissFriendRequest("82")); QTRY_VERIFY(!client.socialBusy()); QVERIFY(!client.m_relationships.contains("82"));
        QVERIFY(fixture.requests.at(1).startsWith("DELETE /users/@me/relationships/82 "));
        QVERIFY(client.removeFriend("83")); QTRY_VERIFY(!client.socialBusy()); QVERIFY(!client.m_relationships.contains("83"));
        QVERIFY(fixture.requests.at(2).startsWith("DELETE /users/@me/relationships/83 "));
    }
    void friendRequestSupportsModernAndLegacyUsernames() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({204, {}}); fixture.responses.enqueue({200, "[]"});
        fixture.responses.enqueue({204, {}}); fixture.responses.enqueue({200, "[]"});
        DiscordClient client; configure(client, fixture);
        QVERIFY(!client.sendFriendRequest("bad name")); QVERIFY(!client.sendFriendRequest(QString(100, 'x'))); QVERIFY(client.sendFriendRequest("fixture.person"));
        QTRY_COMPARE(fixture.requests.size(), 2); QTRY_VERIFY(!client.socialBusy());
        const auto modern = SocialHttpFixture::body(fixture.requests.at(0)); QCOMPARE(modern.value("username").toString(), QString("fixture.person")); QVERIFY(modern.value("discriminator").isNull());
        QVERIFY(client.sendFriendRequest("Generated Person#0123")); QTRY_COMPARE(fixture.requests.size(), 4);
        const auto legacy = SocialHttpFixture::body(fixture.requests.at(2)); QCOMPARE(legacy.value("username").toString(), QString("Generated Person")); QCOMPARE(legacy.value("discriminator").toInt(), 123);
    }
    void acceptingARequestOpensDmWithoutCreatingAFriendship() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({200, "{}"}); fixture.responses.enqueue({200, "[]"});
        DiscordClient client; configure(client, fixture); client.m_currentGuildId = "900";
        client.acceptDm(dm("20", "84", true, true)); QVERIFY(client.acceptMessageRequest("20"));
        QTRY_VERIFY(!client.socialBusy()); QTRY_COMPARE(client.currentChannelId(), QString("20"));
        QCOMPARE(client.spamRequests().size(), 0); QCOMPARE(client.friends().size(), 0); QCOMPARE(client.dms()->count(), 1);
        QTRY_COMPARE(fixture.requests.size(), 2); QVERIFY(fixture.requests.at(0).startsWith("PUT /channels/20/recipients/@me "));
        QCOMPARE(SocialHttpFixture::body(fixture.requests.at(0)).value("consent_status").toInt(), 2);
        QVERIFY(fixture.requests.at(1).startsWith("GET /channels/20/messages?limit=50 "));
    }
    void rejectingARequestUsesConsentRouteAndDoesNotBlockOrUnfriend() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; configure(client, fixture); client.acceptDm(dm("20", "84", true));
        QVERIFY(client.dismissMessageRequest("20")); QTRY_VERIFY(!client.socialBusy());
        QCOMPARE(client.messageRequests().size(), 0); QVERIFY(!client.m_dmObjects.contains("20")); QVERIFY(!client.m_blockedUsers.contains("84"));
        QCOMPARE(fixture.requests.size(), 1); QVERIFY(fixture.requests.first().startsWith("DELETE /channels/20/recipients/@me "));
        QVERIFY(!client.dismissMessageRequest("20"));
    }
    void mutationFailureAndLogoutNeverChangeLocalRelationship() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({403, "{\"message\":\"Generated failure\"}"});
        fixture.responses.enqueue({204, {}, 150});
        DiscordClient client; configure(client, fixture); client.acceptRelationship(relation("81", 3));
        QVERIFY(client.acceptFriendRequest("81")); QTRY_VERIFY(!client.socialBusy());
        QCOMPARE(client.m_relationships.value("81").value("type").toInt(), 3); QVERIFY(!client.socialError().isEmpty()); QCOMPARE(fixture.requests.size(), 1);
        QVERIFY(client.acceptFriendRequest("81")); QTRY_COMPARE(fixture.requests.size(), 2); client.logout();
        QTest::qWait(220); QVERIFY(client.friends().isEmpty()); QVERIFY(client.pendingFriends().isEmpty()); QVERIFY(!client.socialBusy());
    }
    void newerGatewayEventsWinOverSlowMutationAndSnapshot() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({204, {}, 100});
        fixture.responses.enqueue({200, QJsonDocument(QJsonArray{relation("81", 1)}).toJson(), 100});
        fixture.responses.enqueue({200, QJsonDocument(QJsonArray{dm("20", "84", true)}).toJson(), 100});
        DiscordClient client; configure(client, fixture); client.acceptRelationship(relation("81", 3));
        QVERIFY(client.acceptFriendRequest("81")); QTRY_COMPARE(fixture.requests.size(), 1);
        client.dispatch("RELATIONSHIP_UPDATE", {{"id", "81"}, {"type", 2}}); QTRY_VERIFY(!client.socialBusy());
        QVERIFY(client.m_blockedUsers.contains("81")); QCOMPARE(client.friends().size(), 0);
        client.acceptDm(dm("20", "84", true)); client.refreshSocial(); QTRY_COMPARE(fixture.requests.size(), 2);
        client.dispatch("RELATIONSHIP_REMOVE", {{"id", "81"}}); QTRY_COMPARE(fixture.requests.size(), 3);
        client.dispatch("CHANNEL_DELETE", {{"id", "20"}}); QTRY_VERIFY(!client.socialBusy());
        QVERIFY(!client.m_relationships.contains("81")); QVERIFY(!client.m_dmObjects.contains("20")); QCOMPARE(client.messageRequests().size(), 0);
    }
    void blockingRequestSenderRemovesPreviewWithoutDeletingChannel() {
        DiscordClient client; client.m_demoMode = true; client.m_currentUserId = "42"; client.acceptDm(dm("20", "84", true));
        QVERIFY(client.previewMessageRequest("20")); QVERIFY(!client.selectedMessageRequest().isEmpty());
        client.acceptRelationship(relation("84", 2)); QCOMPARE(client.messageRequests().size(), 0); QVERIFY(client.selectedMessageRequest().isEmpty());
        QVERIFY(client.m_dmObjects.contains("20"));
    }
    void presenceUpdatesUseGatewayAndSurviveFreshIdentify() {
        QWebSocketServer server("Generated presence fixture", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0)); QList<QJsonObject> packets;
        connect(&server, &QWebSocketServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                socket->setParent(&server);
                connect(socket, &QWebSocket::textMessageReceived, &server, [&](const QString &text) { packets.append(QJsonDocument::fromJson(text.toUtf8()).object()); });
            }
        });
        DiscordClient client; client.m_currentUserId = "42"; client.m_token = "generated-presence-token"; client.m_connected = true;
        QVERIFY(!client.setPresenceStatus("busy")); QVERIFY(!client.setPresenceStatus("dnd"));
        client.m_socket.open(QUrl("ws://127.0.0.1:" + QString::number(server.serverPort())));
        QTRY_COMPARE(client.m_socket.state(), QAbstractSocket::ConnectedState);
        QVERIFY(client.setPresenceStatus("invisible")); QTRY_COMPARE(packets.size(), 1);
        QCOMPARE(packets.first().value("op").toInt(), 3); QCOMPARE(packets.first().value("d").toObject().value("status").toString(), QString("invisible"));
        QCOMPARE(client.currentPresenceStatus(), QString("invisible"));
        client.identifyOrResume(); QTRY_COMPARE(packets.size(), 2);
        QCOMPARE(packets.last().value("d").toObject().value("presence").toObject().value("status").toString(), QString("invisible"));
        client.acceptPresence({}, {{"user_id", "42"}, {"status", "dnd"}}); QCOMPARE(client.currentPresenceStatus(), QString("dnd"));
        client.logout(); QCOMPARE(client.currentPresenceStatus(), QString("online"));
    }
    void notesNicknameAndIgnoreUseSeparateConfirmedEndpoints() {
        SocialHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({204, {}}); fixture.responses.enqueue({200, "{\"note\":\"Generated note\"}"});
        fixture.responses.enqueue({204, {}}); fixture.responses.enqueue({204, {}}); fixture.responses.enqueue({204, {}});
        DiscordClient client; configure(client, fixture); client.acceptRelationship(relation("81", 1, "Before"));
        QVERIFY(client.setFriendNickname("81", "My friend")); QTRY_VERIFY(!client.socialBusy());
        QCOMPARE(client.relationshipInfo("81").value("nickname").toString(), QString("My friend")); QCOMPARE(client.friends().first().toMap().value("name").toString(), QString("My friend"));
        QVERIFY(fixture.requests.first().startsWith("PATCH /users/@me/relationships/81 "));
        QVERIFY(client.fetchUserNote("81")); QTRY_COMPARE(client.userNote("81"), QString("Generated note"));
        QVERIFY(client.setUserNote("81", "Edited note")); QTRY_VERIFY(!client.socialBusy()); QCOMPARE(client.userNote("81"), QString("Edited note"));
        QVERIFY(fixture.requests.at(2).startsWith("PUT /users/@me/notes/81 ")); QCOMPARE(SocialHttpFixture::body(fixture.requests.at(2)).value("note").toString(), QString("Edited note"));
        QVERIFY(client.setUserIgnored("81", true)); QTRY_VERIFY(!client.socialBusy()); QVERIFY(client.relationshipInfo("81").value("ignored").toBool());
        QCOMPARE(client.relationshipInfo("81").value("type").toInt(), 1); QVERIFY(fixture.requests.at(3).startsWith("PUT /users/@me/relationships/81/ignore "));
        QVERIFY(client.setUserIgnored("81", false)); QTRY_VERIFY(!client.socialBusy()); QVERIFY(!client.relationshipInfo("81").value("ignored").toBool());
        QVERIFY(fixture.requests.at(4).startsWith("DELETE /users/@me/relationships/81/ignore "));
        QVERIFY(!client.setUserNote("81", QString(257, 'x'))); QVERIFY(!client.setFriendNickname("81", QString(33, 'x')));
    }
};
QTEST_GUILESS_MAIN(SocialBackendTest)
#include "social_backend_test.moc"
