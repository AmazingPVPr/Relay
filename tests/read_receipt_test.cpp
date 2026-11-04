#include "DiscordClient.h"
#include <QDateTime>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

// Generated messages and a private loopback HTTP peer only. Never a live ACK.
namespace {
class ReadHttpFixture final : public QTcpServer {
public:
    struct Response { int status = 204; QByteArray body; int delay = 0; };
    QQueue<Response> responses;
    QList<QByteArray> requests;
    ReadHttpFixture() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    const int end = bytes.indexOf("\r\n\r\n");
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
};
}

class ReadReceiptTest final : public QObject {
    Q_OBJECT
    static QJsonObject message(const QString &id, const QString &channel = "10") {
        return {{"id", id}, {"channel_id", channel}, {"content", "Generated fixture"},
            {"author", QJsonObject{{"id", "81"}, {"username", "Generated person"}}}};
    }
    static void prepare(DiscordClient &client, const ReadHttpFixture &fixture, bool guild = false) {
        client.m_apiBase = fixture.base(); client.m_token = "generated-read-receipt-token";
        client.m_currentUserId = "42"; client.m_connected = true;
        if (guild) {
            client.acceptGuild({{"id", "900"}, {"name", "Generated server"}, {"channels", QJsonArray{
                QJsonObject{{"id", "10"}, {"type", 0}, {"name", "general"}}}}});
            client.m_currentGuildId = "900"; client.m_currentChannelKind = "text";
            client.rebuildChannels(false);
        } else {
            client.acceptDm({{"id", "10"}, {"type", 1}, {"last_message_id", "100"},
                {"recipients", QJsonArray{QJsonObject{{"id", "81"}, {"username", "Generated person"}}}}});
            client.m_currentChannelKind = "dm";
        }
        client.m_currentChannelId = "10"; client.m_readingActive = true; client.m_followingMessages = true;
        client.mergeMessages(QJsonArray{message("100")}, false);
        client.m_unread.insert("10", 3);
        if (guild) { client.m_unread.insert("900", 5); client.m_channels.patch("10", {{"unread", 3}}); }
        else client.syncDm("10");
    }
private slots:
    void onlyRenderedTailProofSendsAuthenticatedAck_data() {
        QTest::addColumn<bool>("guild"); QTest::newRow("direct") << false; QTest::newRow("server") << true;
    }
    void onlyRenderedTailProofSendsAuthenticatedAck() {
        QFETCH(bool, guild); ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, fixture, guild);
        client.setReadingActive(true); client.setFollowingMessages(true);
        QCOMPARE(client.m_unread.value("10"), 3); QTest::qWait(300); QVERIFY(fixture.requests.isEmpty());
        QVERIFY(client.acknowledgeVisibleMessages("10", "100"));
        QCOMPARE(client.m_unread.value("10"), 0);
        if (guild) QCOMPARE(client.m_unread.value("900"), 2);
        QVERIFY(client.m_confirmedReadMessageIds.isEmpty());
        QTRY_COMPARE(fixture.requests.size(), 1); QTRY_VERIFY(!client.m_readReceiptInFlight);
        QVERIFY(fixture.requests.first().startsWith("POST /channels/10/messages/100/ack "));
        QVERIFY(fixture.requests.first().contains("Authorization: generated-read-receipt-token"));
        const auto body = fixture.requests.first().mid(fixture.requests.first().indexOf("\r\n\r\n") + 4);
        QCOMPARE(QJsonDocument::fromJson(body).object(), (QJsonObject{{"token", QJsonValue::Null}}));
        QCOMPARE(client.m_confirmedReadMessageIds.value("10"), QString("100"));
        for (int i = 0; i < 5; ++i) QVERIFY(client.acknowledgeVisibleMessages("10", "100"));
        QTest::qWait(350); QCOMPARE(fixture.requests.size(), 1);
    }
    void latestObservedMessagesCoalesceWithoutDuplicateRequests() {
        ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({204, {}, 150});
        DiscordClient client; prepare(client, fixture);
        QVERIFY(client.acknowledgeVisibleMessages("10", "100"));
        client.acceptMessage(message("101")); QVERIFY(client.acknowledgeVisibleMessages("10", "101"));
        client.acceptMessage(message("102")); QVERIFY(client.acknowledgeVisibleMessages("10", "102"));
        QTRY_COMPARE(fixture.requests.size(), 1);
        QVERIFY(fixture.requests.first().startsWith("POST /channels/10/messages/102/ack "));
        client.acceptMessage(message("103"));
        for (int i = 0; i < 5; ++i) QVERIFY(client.acknowledgeVisibleMessages("10", "103"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 2, 2500);
        QVERIFY(fixture.requests.last().startsWith("POST /channels/10/messages/103/ack "));
        QTRY_VERIFY(!client.m_readReceiptInFlight); QTest::qWait(300); QCOMPARE(fixture.requests.size(), 2);
    }
    void failedAndPendingLocalTailRowsDoNotHideTheReceivedTail() {
        ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, fixture);
        client.m_messages.upsert({{"recordId", "local-pending"}, {"channelId", "10"}, {"pending", true}});
        client.m_messages.upsert({{"recordId", "local-failed"}, {"channelId", "10"}, {"failed", true}});
        QVERIFY(!client.acknowledgeVisibleMessages("10", "local-pending"));
        QVERIFY(!client.acknowledgeVisibleMessages("10", "local-failed"));
        QVERIFY(client.acknowledgeVisibleMessages("10", "100"));
        QTRY_COMPARE(fixture.requests.size(), 1);
        QVERIFY(fixture.requests.first().startsWith("POST /channels/10/messages/100/ack "));
    }
    void inactiveHistoryRequestsAndUnloadedIdsCannotSendReceipts() {
        ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, fixture);
        client.setReadingActive(false); QVERIFY(!client.acknowledgeVisibleMessages("10", "100"));
        client.setReadingActive(true); client.setFollowingMessages(false); QVERIFY(!client.acknowledgeVisibleMessages("10", "100"));
        client.setFollowingMessages(true); client.m_hasNewerMessages = true; QVERIFY(!client.acknowledgeVisibleMessages("10", "100"));
        client.m_hasNewerMessages = false; client.m_loadingMessages = true; QVERIFY(!client.acknowledgeVisibleMessages("10", "100"));
        client.m_loadingMessages = false; QVERIFY(!client.acknowledgeVisibleMessages("20", "100"));
        QVERIFY(!client.acknowledgeVisibleMessages("10", "999"));
        client.acceptMessage(message("101")); QVERIFY(!client.acknowledgeVisibleMessages("10", "100"));
        client.m_dmObjects["10"].insert("is_message_request", true); QVERIFY(!client.acknowledgeVisibleMessages("10", "101"));
        client.m_dmObjects["10"].remove("is_message_request"); client.m_dmObjects["10"].insert("is_spam", true); QVERIFY(!client.acknowledgeVisibleMessages("10", "101"));
        client.m_dmObjects["10"].insert("is_spam", false); client.m_currentChannelKind = "hidden"; QVERIFY(!client.acknowledgeVisibleMessages("10", "101"));
        client.m_currentChannelKind = "dm"; client.m_token = "Bot generated-token"; QVERIFY(!client.acknowledgeVisibleMessages("10", "101"));
        client.m_token = "generated-read-receipt-token"; client.m_demoMode = true; QVERIFY(!client.acknowledgeVisibleMessages("10", "101"));
        client.m_demoMode = false; client.m_connected = false; QVERIFY(!client.acknowledgeVisibleMessages("10", "101"));
        QTest::qWait(350); QVERIFY(fixture.requests.isEmpty()); QCOMPARE(client.m_unread.value("10"), 3);
    }
    void queuedRateLimitedObservedReceiptsSurviveFocusLossAndRespectRemoteWatermarks() {
        ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, fixture);
        DiscordClient::RestRequest job; job.method = "POST"; job.path = "/channels/10/messages/100/ack";
        client.m_rateReset.insert(client.routeKey(job), QDateTime::currentMSecsSinceEpoch() + 750);
        QVERIFY(client.acknowledgeVisibleMessages("10", "100")); QTRY_VERIFY(client.m_readReceiptInFlight);
        client.setReadingActive(false); QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 1, 1500);
        QTRY_VERIFY(!client.m_readReceiptInFlight); QVERIFY(client.readReceiptError().isEmpty());
        client.setReadingActive(true);
        client.acceptMessage(message("101"));
        QVERIFY(client.acknowledgeVisibleMessages("10", "101"));
        client.dispatch("MESSAGE_ACK", {{"channel_id", "10"}, {"message_id", "101"}, {"mention_count", 0}});
        QTest::qWait(350); QCOMPARE(fixture.requests.size(), 1);
    }
    void switchingBetweenObservedConversationsKeepsBothReceiptsButNotUnseenArrivals() {
        ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, fixture);
        QVERIFY(client.acknowledgeVisibleMessages("10", "100"));
        client.acceptDm({{"id", "20"}, {"type", 1}, {"last_message_id", "200"}});
        client.clearSelection(); client.m_currentChannelId = "20"; client.m_currentChannelKind = "dm";
        client.mergeMessages(QJsonArray{message("200", "20")}, false);
        QVERIFY(client.acknowledgeVisibleMessages("20", "200"));
        client.setReadingActive(false);
        client.dispatch("MESSAGE_CREATE", message("101", "10")); client.dispatch("MESSAGE_CREATE", message("201", "20"));
        QCOMPARE(client.m_unread.value("10"), 1); QCOMPARE(client.m_unread.value("20"), 1);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 2, 3000);
        QTRY_VERIFY(!client.m_readReceiptInFlight);
        QVERIFY(fixture.requests.at(0).startsWith("POST /channels/10/messages/100/ack "));
        QVERIFY(fixture.requests.at(1).startsWith("POST /channels/20/messages/200/ack "));
        QCOMPARE(client.m_unread.value("10"), 1); QCOMPARE(client.m_unread.value("20"), 1);
        QCOMPARE(client.m_confirmedReadMessageIds.value("10"), QString("100"));
        QCOMPARE(client.m_confirmedReadMessageIds.value("20"), QString("200"));
    }
    void queuedProofIsCanceledForAccountChangeRequestsAndPermissionRevocation_data() {
        QTest::addColumn<QString>("change");
        QTest::newRow("logout") << QString("logout");
        QTest::newRow("request") << QString("request");
        QTest::newRow("permission") << QString("permission");
        QTest::newRow("evicted-permission-snapshot") << QString("evicted-permission");
    }
    void queuedProofIsCanceledForAccountChangeRequestsAndPermissionRevocation() {
        QFETCH(QString, change); ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, fixture, change.contains("permission"));
        QVERIFY(client.acknowledgeVisibleMessages("10", "100"));
        if (change == "logout") { client.logout(); client.m_currentUserId = "43"; }
        else if (change == "request") client.m_dmObjects["10"].insert("is_message_request", true);
        else {
            if (change == "evicted-permission") client.m_channelCache.remove("900");
            client.m_guildObjects["900"].insert("member", QJsonObject{{"roles", QJsonArray{}}});
            client.m_guildObjects["900"].insert("roles", QJsonArray{QJsonObject{{"id", "900"}, {"permissions", "0"}}});
        }
        QTest::qWait(400); QVERIFY(fixture.requests.isEmpty());
        QVERIFY(client.m_pendingReadReceipts.isEmpty());
    }
    void olderMessageFailurePreservesOnlyAStillAuthorizedNewerProof_data() {
        QTest::addColumn<int>("status"); QTest::addColumn<bool>("preserved");
        QTest::newRow("older-message-deleted") << 404 << true;
        QTest::newRow("permission-denied") << 403 << false;
    }
    void olderMessageFailurePreservesOnlyAStillAuthorizedNewerProof() {
        QFETCH(int, status); QFETCH(bool, preserved);
        ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({status, "{}", 180});
        DiscordClient client; prepare(client, fixture);
        QVERIFY(client.acknowledgeVisibleMessages("10", "100")); QTRY_COMPARE(fixture.requests.size(), 1);
        client.dispatch("MESSAGE_CREATE", message("101"));
        QVERIFY(client.acknowledgeVisibleMessages("10", "101"));
        if (preserved) {
            QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 2, 2500);
            QVERIFY(fixture.requests.last().startsWith("POST /channels/10/messages/101/ack "));
            QTRY_VERIFY(!client.m_readReceiptInFlight);
            QCOMPARE(client.m_confirmedReadMessageIds.value("10"), QString("101"));
            QVERIFY(client.readReceiptError().isEmpty());
        } else {
            QTRY_VERIFY(!client.m_readReceiptInFlight); QTest::qWait(1100);
            QCOMPARE(fixture.requests.size(), 1); QVERIFY(client.m_pendingReadReceipts.isEmpty());
            QVERIFY(!client.readReceiptError().isEmpty());
        }
    }
    void exhaustedOlderTransientFailureRetainsNewerProofThroughCooldown() {
        ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({503, "{}", 180});
        DiscordClient client; prepare(client, fixture);
        QVERIFY(client.acknowledgeVisibleMessages("10", "100"));
        client.m_pendingReadReceipts["10"].failures = 2;
        QTRY_COMPARE(fixture.requests.size(), 1);
        client.dispatch("MESSAGE_CREATE", message("101"));
        QVERIFY(client.acknowledgeVisibleMessages("10", "101"));
        client.setReadingActive(false);
        QTRY_VERIFY(!client.m_readReceiptInFlight);
        QCOMPARE(client.m_pendingReadReceipts.value("10").messageId, QString("101"));
        QCOMPARE(client.m_pendingReadReceipts.value("10").failures, 0);
        QVERIFY(client.m_pendingReadReceipts.value("10").retryAfter > QDateTime::currentMSecsSinceEpoch() + 50000);
        QVERIFY(!client.readReceiptError().isEmpty());
        client.m_readReceiptLastSentAt = 0; client.m_readReceiptTimer.stop(); client.flushReadReceipt();
        QTest::qWait(300); QCOMPARE(fixture.requests.size(), 1);
        QVERIFY(!client.m_readReceiptInFlight);
        QCOMPARE(client.m_pendingReadReceipts.value("10").messageId, QString("101"));
        // A held channel must not delay a different channel's fresh proof.
        client.acceptDm({{"id", "20"}, {"type", 1}, {"last_message_id", "200"}});
        client.clearSelection(); client.m_currentChannelId = "20"; client.m_currentChannelKind = "dm";
        client.mergeMessages(QJsonArray{message("200", "20")}, false); client.setReadingActive(true);
        QVERIFY(client.acknowledgeVisibleMessages("20", "200")); client.setReadingActive(false);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 2, 1200); QTRY_VERIFY(!client.m_readReceiptInFlight);
        QVERIFY(fixture.requests.last().startsWith("POST /channels/20/messages/200/ack "));
        QCOMPARE(client.m_pendingReadReceipts.value("10").messageId, QString("101"));
        client.dispatch("MESSAGE_CREATE", message("103", "10"));
        client.clearSelection(); client.m_currentChannelId = "10"; client.m_currentChannelKind = "dm";
        client.mergeMessages(QJsonArray{message("103")}, false); client.setReadingActive(true);
        QVERIFY(client.acknowledgeVisibleMessages("10", "103")); client.setReadingActive(false);
        QCOMPARE(client.m_pendingReadReceipts.value("10").messageId, QString("103"));
        QVERIFY(client.m_pendingReadReceipts.value("10").retryAfter > QDateTime::currentMSecsSinceEpoch() + 50000);
        client.dispatch("MESSAGE_CREATE", message("104"));
        QCOMPARE(client.m_unread.value("10"), 1);
        // Advance the private fixture's cooldown; no real-time minute or live ACK.
        client.m_readReceiptTimer.stop(); client.m_pendingReadReceipts["10"].retryAfter = 0;
        client.m_readReceiptLastSentAt = 0; client.flushReadReceipt();
        QTRY_COMPARE(fixture.requests.size(), 3); QTRY_VERIFY(!client.m_readReceiptInFlight);
        QVERIFY(fixture.requests.last().startsWith("POST /channels/10/messages/103/ack "));
        QCOMPARE(client.m_confirmedReadMessageIds.value("10"), QString("103"));
        QCOMPARE(client.m_unread.value("10"), 1);
        QVERIFY(client.readReceiptError().isEmpty());
    }
    void deletingKnownLatestAllowsFreshTailProofButPreservesNewerUnseenMessages_data() {
        QTest::addColumn<bool>("away"); QTest::addColumn<bool>("newer"); QTest::addColumn<bool>("bulk");
        QTest::newRow("selected-latest") << false << false << false;
        QTest::newRow("background-latest") << true << false << false;
        QTest::newRow("selected-newer-unseen") << false << true << false;
        QTest::newRow("background-newer-unseen") << true << true << false;
        QTest::newRow("background-bulk-newer-unseen") << true << true << true;
    }
    void deletingKnownLatestAllowsFreshTailProofButPreservesNewerUnseenMessages() {
        QFETCH(bool, away); QFETCH(bool, newer); QFETCH(bool, bulk);
        ReadHttpFixture fixture; DiscordClient client; prepare(client, fixture);
        client.dispatch("MESSAGE_ACK", {{"channel_id", "10"}, {"message_id", "100"}, {"mention_count", 0}});
        client.setReadingActive(false);
        if (away) {
            client.acceptDm({{"id", "20"}, {"type", 1}}); client.clearSelection();
            client.m_currentChannelId = "20"; client.m_currentChannelKind = "dm";
        }
        client.dispatch("MESSAGE_CREATE", message("101"));
        if (newer) client.dispatch("MESSAGE_CREATE", message("102"));
        QCOMPARE(client.m_unread.value("10"), newer ? 2 : 1);
        if (bulk) client.dispatch("MESSAGE_DELETE_BULK", {{"channel_id", "10"}, {"ids", QJsonArray{"90", "101"}}});
        else client.dispatch("MESSAGE_DELETE", {{"channel_id", "10"}, {"id", "101"}});
        QCOMPARE(client.m_unread.value("10"), newer ? 2 : 1); // A deletion is not a read.
        if (newer) QCOMPARE(client.m_latestChannelMessageIds.value("10"), QString("102"));
        else {
            QVERIFY(client.m_latestChannelMessageIds.value("10").isEmpty());
            QVERIFY(client.m_dmObjects.value("10").value("last_message_id").toString().isEmpty());
            client.acceptDm({{"id", "10"}, {"last_message_id", "101"}}); // A late snapshot cannot resurrect it.
            QVERIFY(client.m_dmObjects.value("10").value("last_message_id").toString().isEmpty());
        }
        client.dispatch("MESSAGE_ACK", {{"channel_id", "10"}, {"message_id", "100"}, {"mention_count", 0}});
        QCOMPARE(client.m_unread.value("10"), newer ? 2 : 1);
        if (away) {
            client.clearSelection(); client.m_currentChannelId = "10"; client.m_currentChannelKind = "dm";
            client.mergeMessages(QJsonArray{message("100")}, false);
        }
        client.setReadingActive(true);
        if (newer && !away) QVERIFY(!client.acknowledgeVisibleMessages("10", "100"));
        else QVERIFY(client.acknowledgeVisibleMessages("10", "100"));
        QCOMPARE(client.m_unread.value("10"), newer ? 2 : 0);
    }
    void deletingLatestDoesNotLetAnOldAckInferUnknownEarlierMessagesRead() {
        ReadHttpFixture fixture; DiscordClient client; prepare(client, fixture);
        client.acceptDm({{"id", "10"}, {"last_message_id", "101"}});
        client.dispatch("MESSAGE_DELETE", {{"channel_id", "10"}, {"id", "101"}});
        QCOMPARE(client.m_unread.value("10"), 3);
        client.dispatch("MESSAGE_ACK", {{"channel_id", "10"}, {"message_id", "100"}, {"mention_count", 0}});
        QCOMPARE(client.m_unread.value("10"), 3);
        QVERIFY(client.acknowledgeVisibleMessages("10", "100")); QCOMPARE(client.m_unread.value("10"), 0);
    }
    void activeArrivalsRemainUnreadUntilTheirOwnViewportProof_data() {
        QTest::addColumn<bool>("guild"); QTest::newRow("direct") << false; QTest::newRow("server") << true;
    }
    void activeArrivalsRemainUnreadUntilTheirOwnViewportProof() {
        QFETCH(bool, guild); ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        DiscordClient client; prepare(client, fixture, guild);
        client.dispatch("MESSAGE_ACK", {{"channel_id", "10"}, {"message_id", "100"}, {"mention_count", 0}});
        QVERIFY(client.acknowledgeVisibleMessages("10", "100"));
        auto arrival = message("101"); if (guild) arrival.insert("guild_id", "900");
        client.dispatch("MESSAGE_CREATE", arrival);
        QCOMPARE(client.m_unread.value("10"), 1);
        client.setReadingActive(false); QTest::qWait(350);
        QVERIFY(fixture.requests.isEmpty()); QCOMPARE(client.m_unread.value("10"), 1);
        client.setReadingActive(true); QVERIFY(client.acknowledgeVisibleMessages("10", "101"));
        QCOMPARE(client.m_unread.value("10"), 0);
        QTRY_COMPARE(fixture.requests.size(), 1);
        QVERIFY(fixture.requests.first().startsWith("POST /channels/10/messages/101/ack "));
    }
    void staleSuccessCannotClearNewArrivalsOrAnotherAccountsState() {
        ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({204, {}, 180});
        DiscordClient client; prepare(client, fixture);
        QVERIFY(client.acknowledgeVisibleMessages("10", "100")); QTRY_COMPARE(fixture.requests.size(), 1);
        client.setReadingActive(false); client.dispatch("MESSAGE_CREATE", message("101"));
        QCOMPARE(client.m_unread.value("10"), 1);
        QTRY_VERIFY(!client.m_readReceiptInFlight); QCOMPARE(client.m_unread.value("10"), 1);
        QCOMPARE(client.m_confirmedReadMessageIds.value("10"), QString("100"));
        fixture.responses.enqueue({204, {}, 180});
        client.setReadingActive(true); QVERIFY(client.acknowledgeVisibleMessages("10", "101"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 2, 2500);
        client.logout(); client.m_currentUserId = "43";
        QTest::qWait(250); QVERIFY(client.m_confirmedReadMessageIds.isEmpty()); QVERIFY(client.m_localReadMessageIds.isEmpty());
    }
    void remoteServerReadStatesClearOnlyTheirExactWatermark() {
        ReadHttpFixture fixture; DiscordClient client; prepare(client, fixture, true);
        client.setReadingActive(false);
        auto next = message("101"); next.insert("guild_id", "900"); client.dispatch("MESSAGE_CREATE", next);
        QCOMPARE(client.m_unread.value("10"), 4); QCOMPARE(client.m_unread.value("900"), 6);
        client.dispatch("MESSAGE_ACK", {{"channel_id", "10"}, {"message_id", "100"}, {"mention_count", 0}});
        QCOMPARE(client.m_unread.value("10"), 4); QCOMPARE(client.m_unread.value("900"), 6);
        client.dispatch("READ_STATE_UPDATE", {{"entries", QJsonArray{QJsonObject{{"id", "10"}, {"last_message_id", "101"}, {"mention_count", 0}}}}});
        QCOMPARE(client.m_unread.value("10"), 0); QCOMPARE(client.m_unread.value("900"), 2);
        client.m_seenLiveMessages.clear(); client.m_seenLiveOrder.clear(); client.dispatch("MESSAGE_CREATE", next);
        QCOMPARE(client.m_unread.value("10"), 0);
    }
    void failedReceiptRetriesAreBoundedAndFailureCanBeDismissed() {
        ReadHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost, 0));
        fixture.responses.enqueue({503, "{}"}); fixture.responses.enqueue({503, "{}"}); fixture.responses.enqueue({503, "{}"});
        DiscordClient client; prepare(client, fixture);
        QVERIFY(client.acknowledgeVisibleMessages("10", "100"));
        QTRY_COMPARE(fixture.requests.size(), 1); QTRY_COMPARE(client.m_pendingReadReceipts.value("10").failures, 1);
        QVERIFY(client.readReceiptError().isEmpty());
        client.m_pendingReadReceipts["10"].retryAfter = 0; client.m_readReceiptLastSentAt = 0; client.m_readReceiptTimer.start(0);
        QTRY_COMPARE(fixture.requests.size(), 2); QTRY_COMPARE(client.m_pendingReadReceipts.value("10").failures, 2);
        QVERIFY(client.readReceiptError().isEmpty());
        client.m_pendingReadReceipts["10"].retryAfter = 0; client.m_readReceiptLastSentAt = 0; client.m_readReceiptTimer.start(0);
        QTRY_COMPARE(fixture.requests.size(), 3); QTRY_VERIFY(!client.readReceiptError().isEmpty());
        QVERIFY(!client.acknowledgeVisibleMessages("10", "100")); QTest::qWait(350); QCOMPARE(fixture.requests.size(), 3);
        client.clearReadReceiptError(); QVERIFY(client.readReceiptError().isEmpty());
        client.m_blockedReadReceipts.remove("10"); client.m_readReceiptLastSentAt = 0;
        QVERIFY(client.acknowledgeVisibleMessages("10", "100")); QTRY_COMPARE(fixture.requests.size(), 4);
        QTRY_VERIFY(!client.m_readReceiptInFlight); QCOMPARE(client.m_confirmedReadMessageIds.value("10"), QString("100"));
    }
};

QTEST_GUILESS_MAIN(ReadReceiptTest)
#include "read_receipt_test.moc"
