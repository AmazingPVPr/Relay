#include "DiscordClient.h"
#include "RecordModel.h"

#include <QAbstractItemModelTester>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocketServer>
#include <QtTest>
#include <algorithm>

// An isolated local HTTP peer. These tests never connect to Discord, read a real
// token, or create a real message. Every credential and payload below is a fixture.
class HttpFixture final : public QObject {
public:
    struct Response {
        int status = 200;
        QByteArray body = "{}";
        QByteArray extraHeaders;
        int delayMs = 0;
    };
    QTcpServer server;
    QQueue<Response> responses;
    QList<QByteArray> received;
    HttpFixture() {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto bytes = socket->property("received").toByteArray() + socket->readAll();
                    socket->setProperty("received", bytes);
                    if (socket->property("handled").toBool()) return;
                    const int end = bytes.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    int contentLength = 0;
                    for (const auto &line : bytes.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) contentLength = line.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + contentLength) return;
                    socket->setProperty("handled", true);
                    received.append(bytes);
                    const auto response = responses.isEmpty() ? Response{} : responses.dequeue();
                    QTimer::singleShot(response.delayMs, socket, [socket, response] {
                        if (socket->state() != QAbstractSocket::ConnectedState) return;
                        const QByteArray reason = response.status == 200 ? "OK" :
                            (response.status == 429 ? "Too Many Requests" : "Error");
                        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + ' ' + reason +
                            "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(response.body.size()) +
                            "\r\nConnection: close\r\n" + response.extraHeaders + "\r\n" + response.body);
                        socket->disconnectFromHost();
                    });
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QString base() const { return "http://127.0.0.1:" + QString::number(server.serverPort()); }
};

class BackendTests final : public QObject {
    Q_OBJECT
private:
    static QJsonObject message(const QString &id, const QString &channel = "101", const QString &content = "Hello") {
        return {{"id", id}, {"channel_id", channel}, {"content", content},
            {"timestamp", "2026-10-21T12:00:00.000Z"},
            {"author", QJsonObject{{"id", "42"}, {"username", "Fixture"}}}};
    }
    static void useFixture(DiscordClient &client, const HttpFixture &fixture) {
        client.m_apiBase = fixture.base();
        client.m_token = "fixture-token-not-a-real-discord-token";
        client.m_currentUserId = "42";
    }
    static void openLocalGateway(DiscordClient &client,QWebSocketServer &server,QList<QJsonObject> &voiceStates) {
        if (!server.isListening()) {
            QVERIFY(server.listen(QHostAddress::LocalHost,0));
            connect(&server,&QWebSocketServer::newConnection,&server,[&server,&voiceStates] {
                while (auto *socket=server.nextPendingConnection()) {
                    socket->setParent(&server);
                    connect(socket,&QWebSocket::textMessageReceived,&server,[&voiceStates](const QString &text) {
                        const auto packet=QJsonDocument::fromJson(text.toUtf8()).object();
                        if (packet.value("op").toInt()==4) voiceStates.append(packet.value("d").toObject());
                    });
                }
            });
        }
        client.m_socket.open(QUrl("ws://127.0.0.1:"+QString::number(server.serverPort())));
        QTRY_COMPARE_WITH_TIMEOUT(client.m_socket.state(),QAbstractSocket::ConnectedState,1500);
    }
    static void reconnectingVoiceAccount(DiscordClient &client) {
        client.m_token="generated-voice-intent-token";client.m_currentUserId="42";
        client.m_connected=false;client.m_connecting=true;
    }
    static QJsonObject directMessage(const QString &id,const QString &channel,const QString &author="84") {
        auto result=message(id,channel);
        result.insert("author",QJsonObject{{"id",author},{"username","Generated sender"}});
        return result;
    }
private slots:
    void unreadDmRailTracksRealEventsAndActivationReadsOnlyTheClickedConversation() {
        HttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));fixture.responses.enqueue({200,"[]"});
        DiscordClient client;useFixture(client,fixture);client.m_connected=true;client.m_currentGuildId="900";
        QAbstractItemModelTester tester(client.unreadDms(),QAbstractItemModelTester::FailureReportingMode::QtTest);
        client.acceptDm({{"id","10"},{"type",1},{"recipients",QJsonArray{QJsonObject{{"id","84"},{"username","Maya"},{"avatar","generated"}}}}});
        client.acceptDm({{"id","20"},{"type",3},{"name","Generated group"},{"icon","groupicon"}});
        QCOMPARE(client.unreadDms()->count(),0);
        client.dispatch("MESSAGE_CREATE",directMessage("100","10"));
        QCOMPARE(client.unreadDms()->count(),1);QCOMPARE(client.unreadDms()->get(0).value("unread").toInt(),1);
        QCOMPARE(client.unreadDms()->get(0).value("name").toString(),QString("Maya"));
        QVERIFY(client.unreadDms()->get(0).value("avatarUrl").toString().contains("/avatars/84/generated.png"));
        client.dispatch("MESSAGE_CREATE",directMessage("100","10"));
        QCOMPARE(client.unreadDms()->get(0).value("unread").toInt(),1);
        client.dispatch("MESSAGE_CREATE",directMessage("200","20"));
        QCOMPARE(client.unreadDms()->count(),2);QCOMPARE(client.unreadDms()->get(0).value("recordId").toString(),QString("20"));
        QVERIFY(client.unreadDms()->get(0).value("avatarUrl").toString().contains("/channel-icons/20/groupicon.png"));
        client.dispatch("MESSAGE_CREATE",directMessage("201","10"));
        QCOMPARE(client.unreadDms()->get(0).value("recordId").toString(),QString("10"));
        QCOMPARE(client.unreadDms()->get(0).value("unread").toInt(),2);
        client.dispatch("MESSAGE_CREATE",directMessage("202","20","42"));
        QCOMPARE(client.unreadDms()->byId("20").value("unread").toInt(),1);
        // A newer group is first in the DM list. Opening the other rail icon
        // must not briefly select/read that first conversation as a side effect.
        client.activateChannel("10");
        QCOMPARE(client.currentGuildId(),QString());QCOMPARE(client.currentChannelId(),QString("10"));
        QCOMPARE(client.unreadDms()->count(),2); // Selection alone has not rendered or read either conversation.
        QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(),1,1500);
        QVERIFY(fixture.received.first().startsWith("GET /channels/10/messages"));
        QTRY_VERIFY(!client.loadingMessages());
        QTest::qWait(40);QCOMPARE(fixture.received.size(),1); // Fetching alone sends no remote ACK.
        client.mergeMessages(QJsonArray{directMessage("201","10")},false);
        QVERIFY(client.acknowledgeVisibleMessages("10","201"));
        QCOMPARE(client.unreadDms()->count(),1);QCOMPARE(client.unreadDms()->get(0).value("recordId").toString(),QString("20"));
        QTRY_COMPARE(fixture.received.size(),2);QVERIFY(fixture.received.last().startsWith("POST /channels/10/messages/201/ack "));
        client.dispatch("MESSAGE_CREATE",directMessage("203","10"));
        QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),1);
        QVERIFY(client.acknowledgeVisibleMessages("10","203"));
        QVERIFY(client.unreadDms()->byId("10").isEmpty());
    }
    void selectedDmsRemainUnreadWhileUnfocusedOrReadingOlderHistory() {
        HttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));fixture.responses.enqueue({200,"[]"});
        DiscordClient client;useFixture(client,fixture);client.m_connected=true;
        client.acceptDm({{"id","10"},{"type",1}});client.acceptDm({{"id","20"},{"type",3}});client.activateChannel("10");
        QTRY_VERIFY(!client.loadingMessages());
        client.dispatch("MESSAGE_CREATE",directMessage("100","10"));QCOMPARE(client.unreadDms()->count(),1);
        QVERIFY(client.acknowledgeVisibleMessages("10","100"));QCOMPARE(client.unreadDms()->count(),0);
        client.setReadingActive(false);client.dispatch("MESSAGE_CREATE",directMessage("101","10"));
        QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),1);QCOMPARE(client.messages()->count(),2);
        client.dispatch("MESSAGE_CREATE",directMessage("101","10"));QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),1);
        client.setReadingActive(true);QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),1);
        QVERIFY(client.acknowledgeVisibleMessages("10","101"));QVERIFY(client.unreadDms()->byId("10").isEmpty());
        client.setFollowingMessages(false);client.dispatch("MESSAGE_CREATE",directMessage("102","10"));
        QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),1);
        client.setReadingActive(false);client.setReadingActive(true);QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),1);
        client.dispatch("MESSAGE_CREATE",directMessage("103","20"));
        client.setFollowingMessages(true);QVERIFY(client.acknowledgeVisibleMessages("10","102"));QVERIFY(client.unreadDms()->byId("10").isEmpty());
        QCOMPARE(client.unreadDms()->byId("20").value("unread").toInt(),1);
        client.setReadingActive(false);client.dispatch("MESSAGE_CREATE",directMessage("104","10"));
        client.activateChannel("10");QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),1);
        client.m_hasNewerMessages=true;client.setFollowingMessages(true);client.setReadingActive(true);
        client.dispatch("MESSAGE_CREATE",directMessage("105","10"));
        client.setFollowingMessages(true);client.setReadingActive(true);
        QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),2);
        QVERIFY(!client.acknowledgeVisibleMessages("10","104"));
        client.m_hasNewerMessages=false;client.setFollowingMessages(true);
        client.mergeMessages(QJsonArray{directMessage("105","10")},false);
        QVERIFY(client.acknowledgeVisibleMessages("10","105"));QVERIFY(client.unreadDms()->byId("10").isEmpty());
        // A duplicate that has fallen out of the bounded seen-event cache
        // remains read because local reading retained its message watermark.
        client.m_seenLiveMessages.clear();client.m_seenLiveOrder.clear();client.dispatch("MESSAGE_CREATE",directMessage("104","10"));
        QVERIFY(client.unreadDms()->byId("10").isEmpty());
        // Server channels use the same active-reading guard as conversations.
        client.m_currentGuildId="900";client.m_currentChannelId="901";
        auto serverMessage=directMessage("106","901");serverMessage.insert("guild_id","900");
        client.dispatch("MESSAGE_CREATE",serverMessage);QCOMPARE(client.m_unread.value("901"),1);
        client.setReadingActive(false);serverMessage.insert("id","107");client.dispatch("MESSAGE_CREATE",serverMessage);QCOMPARE(client.m_unread.value("901"),2);
    }
    void unreadDmRailFiltersMutedAndBlockedConversationsAndRefreshesMetadata() {
        DiscordClient client;client.m_currentUserId="42";client.m_currentGuildId="900";
        client.acceptDm({{"id","10"},{"type",1},{"recipients",QJsonArray{QJsonObject{{"id","84"},{"username","Before"},{"avatar","old"}}}}});
        client.acceptDm({{"id","20"},{"type",3},{"name","Before group"},{"icon","oldgroup"},{"recipients",QJsonArray{QJsonObject{{"id","84"},{"username","Before"}}}}});
        client.dispatch("MESSAGE_CREATE",directMessage("100","10"));client.dispatch("MESSAGE_CREATE",directMessage("101","20"));
        client.dispatch("USER_UPDATE",{{"id","84"},{"global_name","After"},{"avatar","new"}});
        QCOMPARE(client.unreadDms()->byId("10").value("name").toString(),QString("After"));
        QVERIFY(client.unreadDms()->byId("10").value("avatarUrl").toString().contains("/new.png"));
        client.dispatch("CHANNEL_UPDATE",{{"id","20"},{"type",3},{"name","After group"},{"icon","newgroup"}});
        QCOMPARE(client.unreadDms()->byId("20").value("name").toString(),QString("After group"));
        QVERIFY(client.unreadDms()->byId("20").value("avatarUrl").toString().contains("/newgroup.png"));
        client.dispatch("USER_GUILD_SETTINGS_UPDATE",{{"guild_id",QJsonValue::Null},{"channel_overrides",QJsonArray{QJsonObject{{"channel_id","10"},{"muted",true}}}}});
        QVERIFY(client.unreadDms()->byId("10").isEmpty());
        client.dispatch("MESSAGE_CREATE",directMessage("102","10"));QCOMPARE(client.dms()->byId("10").value("unread").toInt(),2);
        client.dispatch("USER_GUILD_SETTINGS_UPDATE",{{"guild_id",QJsonValue::Null},{"channel_overrides",QJsonArray{}}});
        QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),2);
        client.dispatch("RELATIONSHIP_ADD",{{"id","84"},{"type",2}});
        QVERIFY(client.unreadDms()->byId("10").isEmpty());QVERIFY(!client.unreadDms()->byId("20").isEmpty());
        client.dispatch("MESSAGE_CREATE",directMessage("103","20"));QCOMPARE(client.unreadDms()->byId("20").value("unread").toInt(),1);
        client.dispatch("MESSAGE_CREATE",directMessage("104","20","85"));QCOMPARE(client.unreadDms()->byId("20").value("unread").toInt(),2);
        client.dispatch("RELATIONSHIP_REMOVE",{{"id","84"}});QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),2);
        const auto expiry=QDateTime::currentDateTimeUtc().addMSecs(120).toString(Qt::ISODateWithMs);
        client.dispatch("USER_GUILD_SETTINGS_UPDATE",{{"guild_id",QJsonValue::Null},{"channel_overrides",QJsonArray{QJsonObject{{"channel_id","10"},{"muted",true},{"mute_config",QJsonObject{{"end_time",expiry}}}}}}});
        QVERIFY(client.unreadDms()->byId("10").isEmpty());QTRY_VERIFY_WITH_TIMEOUT(!client.unreadDms()->byId("10").isEmpty(),1200);
    }
    void unreadDmRailRejectsMalformedSuppressedUnknownAndSelfEvents() {
        DiscordClient client;client.m_currentUserId="42";client.m_currentGuildId="900";
        client.acceptDm({{"id","10"},{"type",1}});client.acceptDm({{"id","20"},{"type",3}});client.acceptDm({{"id","30"},{"type",0}});
        client.dispatch("MESSAGE_CREATE",directMessage("invalid","10"));client.dispatch("MESSAGE_CREATE",directMessage("100","10","invalid"));
        auto suppressed=directMessage("101","10");suppressed.insert("flags",1<<12);client.dispatch("MESSAGE_CREATE",suppressed);
        client.dispatch("MESSAGE_CREATE",directMessage("102","10","42"));client.dispatch("MESSAGE_CREATE",directMessage("103","999"));
        client.dispatch("MESSAGE_CREATE",directMessage("104","30"));QCOMPARE(client.unreadDms()->count(),0);
        QVERIFY(!client.m_unread.contains("999"));client.acceptDm({{"id","999"},{"type",1}});QCOMPARE(client.unreadDms()->count(),0);
        client.dispatch("MESSAGE_CREATE",directMessage("105","20"));QCOMPARE(client.unreadDms()->count(),1);
        client.dispatch("MESSAGE_CREATE",directMessage("105","20"));QCOMPARE(client.unreadDms()->get(0).value("unread").toInt(),1);
    }
    void unreadDmReadAcknowledgementsAndDeletionCannotEraseNewerMessagesOrResurrectBadges() {
        DiscordClient client;client.m_currentUserId="42";client.m_currentGuildId="900";
        client.acceptDm({{"id","10"},{"type",1}});
        client.dispatch("MESSAGE_CREATE",directMessage("100","10"));client.dispatch("MESSAGE_CREATE",directMessage("101","10"));
        client.dispatch("MESSAGE_ACK",{{"channel_id","10"},{"message_id","100"},{"mention_count",0}});
        QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),2);
        client.dispatch("MESSAGE_ACK",{{"channel_id","10"},{"message_id","101"},{"mention_count",0}});QCOMPARE(client.unreadDms()->count(),0);
        client.m_seenLiveMessages.clear();client.m_seenLiveOrder.clear();client.dispatch("MESSAGE_CREATE",directMessage("101","10"));QCOMPARE(client.unreadDms()->count(),0);
        client.dispatch("MESSAGE_CREATE",directMessage("102","10"));QCOMPARE(client.unreadDms()->count(),1);
        client.dispatch("READ_STATE_UPDATE",{{"entries",QJsonArray{QJsonObject{{"id","10"},{"last_message_id","102"},{"mention_count",0}}}}});
        QCOMPARE(client.unreadDms()->count(),0);
        client.dispatch("MESSAGE_CREATE",directMessage("103","10"));client.dispatch("MESSAGE_DELETE",{{"channel_id","10"},{"id","100"}});
        QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),1); // An old deletion is not a read acknowledgement.
        client.dispatch("CHANNEL_DELETE",{{"id","10"}});QCOMPARE(client.unreadDms()->count(),0);QVERIFY(!client.m_unread.contains("10"));QVERIFY(!client.m_dmReadMessageIds.contains("10"));
        client.acceptDm({{"id","10"},{"type",1}});QCOMPARE(client.unreadDms()->count(),0);
        client.dispatch("MESSAGE_CREATE",directMessage("104","10"));QCOMPARE(client.unreadDms()->count(),1);
        client.logout();QCOMPARE(client.unreadDms()->count(),0);QCOMPARE(client.dms()->count(),0);QVERIFY(client.m_dmReadMessageIds.isEmpty());
        client.m_currentUserId="43";client.acceptDm({{"id","10"},{"type",1}});QCOMPARE(client.unreadDms()->count(),0);
    }
    void unreadDmStartupReadCountsSupportBothSnapshotShapesAndStayBounded_data() {
        QTest::addColumn<bool>("objectShape");QTest::newRow("array")<<false;QTest::newRow("object")<<true;
    }
    void unreadDmStartupReadCountsSupportBothSnapshotShapesAndStayBounded() {
        QFETCH(bool,objectShape);DiscordClient client;
        client.acceptGuild({{"id","900"},{"name","Generated server"},{"channels",QJsonArray{}}});client.selectGuild("900");
        const QJsonArray states{QJsonObject{{"id","10"},{"last_message_id","100"},{"mention_count",4}},QJsonObject{{"id","20"},{"last_message_id","101"},{"mention_count",20000}},QJsonObject{{"id","invalid"},{"mention_count",3}}};
        const QJsonArray dms{QJsonObject{{"id","10"},{"type",1},{"last_message_id","100"}},QJsonObject{{"id","20"},{"type",3},{"last_message_id","101"}}};
        client.dispatch("READY",{{"user",QJsonObject{{"id","42"},{"username","Generated"}}},{"read_state",objectShape?QJsonValue(QJsonObject{{"entries",states}}):QJsonValue(states)},{"private_channels",dms}});
        QCOMPARE(client.unreadDms()->count(),2);QCOMPARE(client.unreadDms()->get(0).value("recordId").toString(),QString("20"));
        QCOMPARE(client.unreadDms()->byId("10").value("unread").toInt(),4);QCOMPARE(client.unreadDms()->byId("20").value("unread").toInt(),9999);
        client.dispatch("MESSAGE_CREATE",directMessage("101","20"));QCOMPARE(client.unreadDms()->byId("20").value("unread").toInt(),9999);
        client.dispatch("READY",{{"user",QJsonObject{{"id","42"},{"username","Generated"}}},{"read_state",QJsonArray{}}});
        QCOMPARE(client.unreadDms()->count(),0);
        client.logout();client.acceptGuild({{"id","900"},{"name","Generated server"},{"channels",QJsonArray{}}});client.selectGuild("900");
        QJsonArray manyDms,manyStates;
        for(int i=1;i<=1010;++i){const auto id=QString::number(i);manyStates.append(QJsonObject{{"id",id},{"mention_count",1}});manyDms.append(QJsonObject{{"id",id},{"type",1}});}
        QSignalSpy insertions(client.unreadDms(),&QAbstractItemModel::rowsInserted);
        client.dispatch("READY",{{"user",QJsonObject{{"id","42"},{"username","Generated"}}},{"read_state",manyStates},{"private_channels",manyDms}});
        QCOMPARE(client.unreadDms()->count(),1000);QCOMPARE(client.dms()->count(),1000);
        QCOMPARE(insertions.count(),1); // One projection update for a bounded startup batch.
    }
    void voiceIntentCoalescesDuringReconnectAndReplaysOnlyOnce_data() {
        QTest::addColumn<QString>("event");QTest::newRow("resumed")<<QString("RESUMED");QTest::newRow("ready")<<QString("READY");
    }
    void voiceIntentCoalescesDuringReconnectAndReplaysOnlyOnce() {
        QFETCH(QString,event);QWebSocketServer server("Generated voice intent",QWebSocketServer::NonSecureMode);QList<QJsonObject> packets;
        DiscordClient client;reconnectingVoiceAccount(client);
        QVERIFY(client.sendVoiceState("10","101",false,false));
        QVERIFY(client.sendVoiceState("10","102",false,true));
        QCOMPARE(client.m_pendingVoiceState.value("channel_id").toString(),QString("102"));QVERIFY(packets.isEmpty());
        openLocalGateway(client,server,packets);
        const QJsonObject ready{{"user",QJsonObject{{"id","42"},{"username","Generated"}}},{"session_id","generated-main-session"}};
        client.dispatch(event,event=="READY"?ready:QJsonObject{});
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,1500);
        QCOMPARE(packets.first().value("channel_id").toString(),QString("102"));QVERIFY(packets.first().value("self_mute").toBool());QVERIFY(packets.first().value("self_deaf").toBool());
        QVERIFY(client.m_pendingVoiceState.isEmpty());client.dispatch("RESUMED",{});QTest::qWait(40);QCOMPARE(packets.size(),1);
        // A later resume with no new explicit intent preserves the independent
        // voice session and must not replay its previously sent join.
        client.reconnect(false,60000);client.m_reconnect.stop();openLocalGateway(client,server,packets);client.dispatch("RESUMED",{});
        QTest::qWait(40);QCOMPARE(packets.size(),1);
    }
    void queuedLeaveSupersedesJoinAndOldAccountsNeverReplay() {
        QWebSocketServer server("Generated leave intent",QWebSocketServer::NonSecureMode);QList<QJsonObject> packets;DiscordClient client;reconnectingVoiceAccount(client);
        QVERIFY(client.sendVoiceState("10","101",false,false));QVERIFY(client.sendVoiceState("10",{},false,false));
        QVERIFY(client.m_pendingVoiceState.value("channel_id").isNull());openLocalGateway(client,server,packets);client.dispatch("RESUMED",{});
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,1500);QVERIFY(packets.first().value("channel_id").isNull());
        client.reconnect(false,60000);client.m_reconnect.stop();QVERIFY(client.sendVoiceState("10","101",false,false));
        client.logout();reconnectingVoiceAccount(client);client.m_currentUserId="43";
        openLocalGateway(client,server,packets);client.dispatch("RESUMED",{});QTest::qWait(40);
        QCOMPARE(packets.size(),1);QVERIFY(client.m_pendingVoiceState.isEmpty());
        // Even without logout, an authoritative READY for another account
        // cannot replay the old account's pending request.
        client.m_connected=false;client.m_connecting=true;QVERIFY(client.sendVoiceState("10","102",false,false));
        client.dispatch("READY",{{"user",QJsonObject{{"id","44"},{"username","Other generated"}}},{"session_id","other-main-session"}});
        QTest::qWait(40);QCOMPARE(packets.size(),1);QVERIFY(client.m_pendingVoiceState.isEmpty());
    }
    void acknowledgedLeaveCannotReplayAfterItsReplacementJoin_data() {
        QTest::addColumn<bool>("snapshot");QTest::newRow("replayed-live-event")<<false;QTest::newRow("explicit-ready-snapshot")<<true;
    }
    void acknowledgedLeaveCannotReplayAfterItsReplacementJoin() {
        QFETCH(bool,snapshot);QWebSocketServer server("Generated leave acknowledgement",QWebSocketServer::NonSecureMode);QList<QJsonObject> packets;
        DiscordClient client;reconnectingVoiceAccount(client);QVERIFY(client.sendVoiceState("10",{},false,false));
        int acknowledgements=0;
        connect(&client,&DiscordClient::voiceStateReceived,&client,[&](const QJsonObject &state) {
            if(state.value("guild_id").toString()!="10" || !state.value("channel_id").isNull())return;
            ++acknowledgements;
            // The voice worker's replacement join is delivered to the GUI
            // after the main gateway's current dispatch has completed.
            QTimer::singleShot(0,&client,[&]{QVERIFY(client.sendVoiceState("10","101",false,false));});
        });
        openLocalGateway(client,server,packets);
        const QJsonObject own{{"guild_id","10"},{"user_id","42"},{"channel_id",QJsonValue::Null}};
        if(snapshot) {
            client.m_channelCache.insert("10",{});
            client.dispatch("READY",{{"user",QJsonObject{{"id","42"},{"username","Generated"}}},{"session_id","generated-main"},
                {"guilds",QJsonArray{QJsonObject{{"id","10"},{"name","Generated"},{"voice_states",QJsonArray{own}}}}}});
        } else {client.dispatch("VOICE_STATE_UPDATE",own);client.dispatch("RESUMED",{});}
        QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,1500);QCOMPARE(acknowledgements,1);
        QCOMPARE(packets.first().value("channel_id").toString(),QString("101"));QVERIFY(client.m_pendingVoiceState.isEmpty());
    }
    void unsentVoiceFramesQueueDuringTransportLossAndRejectPermanentFailures() {
        QWebSocketServer server("Generated transport loss",QWebSocketServer::NonSecureMode);QList<QJsonObject> packets;
        DiscordClient client;reconnectingVoiceAccount(client);client.m_connected=true;client.m_connecting=false;
        // A connected-state notification can precede the socket's disconnected
        // callback. Do not claim the frame was sent or lose explicit intent.
        QVERIFY(client.sendVoiceState("10","101",false,false));client.m_reconnect.stop();
        QVERIFY(client.connecting());QVERIFY(!client.m_pendingVoiceState.isEmpty());
        openLocalGateway(client,server,packets);client.dispatch("RESUMED",{});QTRY_COMPARE_WITH_TIMEOUT(packets.size(),1,1500);
        QSignalSpy rejected(&client,&DiscordClient::voiceStateRequestRejected);
        client.m_connected=false;client.m_connecting=false;QVERIFY(!client.sendVoiceState("10","102",false,false));
        QCOMPARE(rejected.count(),1);QVERIFY(client.m_pendingVoiceState.isEmpty());QVERIFY(!client.error().isEmpty());
        client.m_connected=true;client.m_connecting=false;
        client.m_guildObjects.insert("10",{{"id","10"},{"owner_id","9"},{"member",QJsonObject{{"user",QJsonObject{{"id","42"}}},{"roles",QJsonArray{}}}},
            {"roles",QJsonArray{QJsonObject{{"id","10"},{"permissions","0"}}}}});
        client.m_channelCache.insert("10",QJsonArray{QJsonObject{{"id","102"},{"type",2}}});
        QVERIFY(!client.sendVoiceState("10","102",false,false));QCOMPARE(rejected.count(),2);QCOMPARE(packets.size(),1);
        client.m_connected=false;client.m_connecting=true;
        QVERIFY(client.sendVoiceState("10","101",false,false));QVERIFY(!client.m_pendingVoiceState.isEmpty());
        client.m_connecting=false;
        QVERIFY(!client.sendVoiceState("10","102",false,false));QVERIFY(client.m_pendingVoiceState.isEmpty());QCOMPARE(rejected.count(),4);
        QCOMPARE(rejected.at(2).at(1).toString(),QString("101"));QCOMPARE(rejected.at(2).at(2).toString(),QString("Voice request superseded."));
    }
    void supersededQueuedLeavesNotifyTheVoiceEngineWithoutRejectingSameTargetUpdates() {
        DiscordClient client;reconnectingVoiceAccount(client);QSignalSpy rejected(&client,&DiscordClient::voiceStateRequestRejected);
        QVERIFY(client.sendVoiceState("10",{},false,false));
        QVERIFY(client.sendVoiceState("10",{},true,false));QCOMPARE(rejected.count(),0);
        QVERIFY(client.sendVoiceState("20","201",false,false));QCOMPARE(rejected.count(),1);
        QCOMPARE(rejected.at(0),QVariantList({"10","","Voice request superseded."}));QVERIFY(client.error().isEmpty());
        QVERIFY(client.sendVoiceState("20","201",false,false));
        QVERIFY(client.sendVoiceState("20","201",true,true));QCOMPARE(rejected.count(),1);
        QVERIFY(client.sendVoiceState("30","301",false,false));QCOMPARE(rejected.count(),2);
        QCOMPARE(rejected.at(1),QVariantList({"20","201","Voice request superseded."}));QVERIFY(client.error().isEmpty());
        // Rejecting a new target also cancels an older unsent null request so
        // the voice worker cannot wait forever for its never-sent leave ACK.
        QVERIFY(client.sendVoiceState("10",{},false,false));QCOMPARE(rejected.count(),3);
        client.m_connecting=false;
        QVERIFY(!client.sendVoiceState("20","202",false,false));QCOMPARE(rejected.count(),5);
        QCOMPARE(rejected.at(3),QVariantList({"10","","Voice request superseded."}));QVERIFY(!client.error().isEmpty());QVERIFY(client.m_pendingVoiceState.isEmpty());
    }
    void rotatedCredentialSurvivesOlderRejectedReadAndRetriesWithTheNewSession() {
        HttpFixture fixture; QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        fixture.responses.enqueue({401, R"({"message":"Old authorization"})", {}, 120});
        fixture.responses.enqueue({200, R"({"ok":true})", {}, 0});
        DiscordClient client; useFixture(client,fixture); client.m_connected=true;
        QSignalSpy rejected(&client,&DiscordClient::authenticationRejected);
        int completed=0; QString problem;
        client.request("GET","/fixture-read",{},[&](const QJsonValue &value,const QString &error) { ++completed;problem=error;QVERIFY(value.toObject().value("ok").toBool()); });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(),1,2000);
        client.m_token="rotated-synthetic-fixture-session";
        QTRY_COMPARE_WITH_TIMEOUT(completed,1,2000);
        QCOMPARE(fixture.received.size(),2);QVERIFY(problem.isEmpty());QCOMPARE(rejected.count(),0);
        QCOMPARE(client.m_currentUserId,QString("42"));QVERIFY(client.connected());
        QVERIFY(fixture.received.last().contains("Authorization: rotated-synthetic-fixture-session"));
    }
    void olderRejectedWriteDoesNotRetryOrLogoutButCurrentRejectionStillDoes() {
        HttpFixture fixture; QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        fixture.responses.enqueue({401,R"({"message":"Old authorization"})",{},120});
        fixture.responses.enqueue({401,R"({"message":"Rejected current session"})",{},0});
        DiscordClient client;useFixture(client,fixture);client.m_connected=true;
        QSignalSpy rejected(&client,&DiscordClient::authenticationRejected);
        int completed=0;
        client.request("POST","/fixture-write",{{"value",1}},[&](const QJsonValue &,const QString &error) { ++completed;QVERIFY(!error.isEmpty()); });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(),1,2000);
        client.m_token="rotated-synthetic-fixture-session";
        QTRY_COMPARE_WITH_TIMEOUT(completed,1,2000);QCOMPARE(fixture.received.size(),1);QCOMPARE(rejected.count(),0);QVERIFY(client.connected());
        client.request("GET","/fixture-rejected",{},[&](const QJsonValue &,const QString &error) { ++completed;QVERIFY(!error.isEmpty()); });
        QTRY_COMPARE_WITH_TIMEOUT(rejected.count(),1,2000);QCOMPARE(completed,2);QVERIFY(!client.connected());QVERIFY(client.m_token.isEmpty());
    }

    void profileFetchIsExplicitCachedAndLogoutCancelsLateData() {
        HttpFixture fixture; QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        DiscordClient client; useFixture(client,fixture); client.m_connected=true;
        client.m_users.insert("42",QJsonObject{{"id","42"},{"username","Self"}});
        QCOMPARE(fixture.received.size(),0);
        fixture.responses.enqueue({200,R"({"user":{"id":"42","username":"Self","bio":"Actual about"},"user_profile":{"pronouns":"they/them"},"mutual_friends_count":0})",{},0});
        QSignalSpy updated(&client,&DiscordClient::profileUpdated);
        client.fetchProfile("42"); QVERIFY(client.memberInfo("42").value("profileLoading").toBool());
        client.fetchProfile("42");
        QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(),1,2000);
        QTRY_VERIFY_WITH_TIMEOUT(!client.memberInfo("42").value("profileLoading").toBool(),2000);
        QCOMPARE(client.memberInfo("42").value("bio").toString(),"Actual about");
        QCOMPARE(client.memberInfo("42").value("mutualFriendCount").toInt(),0);
        QVERIFY(!client.memberInfo("42").contains("mutualGuildCount"));
        QVERIFY(fixture.received.first().startsWith("GET /users/42/profile?with_mutual_guilds=true&with_mutual_friends=false&with_mutual_friends_count=true "));
        client.fetchProfile("42"); QTest::qWait(30); QCOMPARE(fixture.received.size(),1);
        client.m_profileFetchedAt.clear();
        fixture.responses.enqueue({200,R"({"user":{"id":"42","username":"Self","bio":"Late data"}})",{},150});
        client.fetchProfile("42"); QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(),2,2000);
        client.logout(); QTest::qWait(200); QVERIFY(client.m_profiles.isEmpty()); QVERIFY(client.memberInfo("42").isEmpty());
    }

    void appInteractionsOnlySendExplicitValidCurrentControls() {
        HttpFixture fixture; QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        DiscordClient client; useFixture(client,fixture); client.m_connected=true;
        client.m_currentChannelId="101"; client.m_currentGuildId="100"; client.m_sessionId="fixture-session";
        auto input=message("901"); input.insert("application_id","777");
        const QJsonArray controls{
            QJsonObject{{"type",2},{"style",1},{"custom_id","start"},{"label","Start"}},
            QJsonObject{{"type",2},{"style",1},{"custom_id","disabled"},{"disabled",true}},
            QJsonObject{{"type",3},{"custom_id","choice"},{"options",QJsonArray{QJsonObject{{"label","One"},{"value","one"}}}}}
        };
        input.insert("components",QJsonArray{QJsonObject{{"type",1},{"components",controls}}});
        client.acceptMessage(input); QCOMPARE(fixture.received.size(),0);
        QSignalSpy error(&client,&DiscordClient::componentError),finished(&client,&DiscordClient::componentFinished);
        QVERIFY(!client.executeComponent("901","missing",2,{}));
        QVERIFY(!client.executeComponent("901","disabled",2,{}));
        QVERIFY(!client.executeComponent("901","choice",3,{QString("other")}));
        QVERIFY(!client.executeComponent("901","start",2,{QString("value")}));
        QCOMPARE(error.count(),4); QCOMPARE(fixture.received.size(),0);
        fixture.responses.enqueue({204,{}, {},70});
        QVERIFY(client.executeComponent("901","start",2,{}));
        QVERIFY(!client.executeComponent("901","start",2,{}));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(),1,2000); QCOMPARE(fixture.received.size(),1);
        const auto request=fixture.received.first(); QVERIFY(request.startsWith("POST /interactions "));
        const auto payload=QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n")+4)).object();
        QCOMPARE(payload.value("type").toInt(),3); QCOMPARE(payload.value("application_id").toString(),"777");
        QCOMPARE(payload.value("session_id").toString(),"fixture-session"); QCOMPARE(payload.value("guild_id").toString(),"100");
        QCOMPARE(payload.value("data").toObject().value("custom_id").toString(),"start");
        QVERIFY(DiscordClient::validId(payload.value("nonce").toString()));
        fixture.responses.enqueue({429,R"({"retry_after":0.01})",{},0});
        QVERIFY(client.executeComponent("901","choice",3,{QString("one")}));
        QTRY_COMPARE_WITH_TIMEOUT(error.count(),5,2000); QTest::qWait(200); QCOMPARE(fixture.received.size(),2);
        const auto select=fixture.received.last();
        const auto selected=QJsonDocument::fromJson(select.mid(select.indexOf("\r\n\r\n")+4)).object().value("data").toObject();
        QCOMPARE(selected.value("values").toArray(),QJsonArray{"one"});
        client.m_currentChannelId="102"; QVERIFY(!client.executeComponent("901","start",2,{}));
    }

    void equalCategoryPositionsKeepChildrenWithTheirHeader() {
        DiscordClient client; client.m_currentGuildId="100";
        client.m_channelCache.insert("100", QJsonArray{
            QJsonObject{{"id","222"},{"type",4},{"name","Voice"},{"position",0}},
            QJsonObject{{"id","111"},{"type",4},{"name","Text"},{"position",0}},
            QJsonObject{{"id","333"},{"type",0},{"name","general"},{"parent_id","111"},{"position",0}},
            QJsonObject{{"id","444"},{"type",2},{"name","Lounge"},{"parent_id","222"},{"position",0}}});
        client.rebuildChannels(false);
        QCOMPARE(client.channels()->count(),4);
        QCOMPARE(client.channels()->get(0).value("recordId").toString(),"111");
        QCOMPARE(client.channels()->get(1).value("recordId").toString(),"333");
        QCOMPARE(client.channels()->get(2).value("recordId").toString(),"222");
        QCOMPARE(client.channels()->get(3).value("recordId").toString(),"444");
    }

    void streamPresenceAndVoiceSessionStayPrivate() {
        DiscordClient client;
        client.m_currentUserId="42"; client.m_currentGuildId="100"; client.m_connected=true;
        client.m_users.insert("43", QJsonObject{{"id","43"},{"username","Streamer"}});
        client.acceptVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","42"},{"session_id","private-fixture-session"}});
        client.acceptVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","43"},{"self_stream",true}});
        client.rebuildVoiceMembers();
        QCOMPARE(client.currentVoiceSessionId(),"private-fixture-session");
        QCOMPARE(client.streams()->count(),1);
        QCOMPARE(client.streams()->get(0).value("recordId").toString(),"guild:100:200:43");
        QCOMPARE(client.voiceMembers()->byId("43").value("streamKey").toString(),"guild:100:200:43");
        QVERIFY(!client.voiceMembers()->byId("42").contains("session_id"));
        QVERIFY(!client.watchStream("guild:100:201:43"));
        QVERIFY(!client.watchStream("guild:100:200:42"));
        QVERIFY(client.watchStream("guild:100:200:43"));
        QSignalSpy stopped(&client,&DiscordClient::streamDeleted);
        client.acceptVoiceState({{"guild_id","100"},{"channel_id","201"},{"user_id","42"},{"session_id","new-private-session"}});
        QCOMPARE(stopped.count(),1); QVERIFY(client.m_watchingStream.isEmpty());
        client.dispatch("STREAM_DELETE",{{"stream_key","guild:100:200:43"},{"unavailable",true}});
        QCOMPARE(client.streams()->count(),1); // A transport outage must keep the retry entry.
        client.dispatch("VOICE_STATE_UPDATE",{{"guild_id","100"},{"channel_id","200"},{"user_id","43"},{"self_stream",false}});
        QCOMPARE(client.streams()->count(),0);
        QVERIFY(!client.voiceMembers()->byId("43").value("streaming").toBool());
        client.logout(); QVERIFY(client.currentVoiceSessionId().isEmpty());
    }

    void streamRestartKeepsTheSamePersonUntilExplicitCancel() {
        DiscordClient client;client.m_currentUserId="42";client.m_currentGuildId="100";client.m_connected=true;
        client.acceptVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","42"},{"session_id","generated-session"}});
        QVERIFY(client.watchStream("guild:100:200:43"));
        QSignalSpy restarted(&client,&DiscordClient::streamRestarting),created(&client,&DiscordClient::streamCreated);
        client.dispatch("STREAM_DELETE",{{"stream_key","guild:100:200:43"}});
        QVERIFY(client.m_watchingStream.isEmpty());QCOMPARE(client.m_desiredStream,"guild:100:200:43");
        client.dispatch("STREAM_CREATE",{{"stream_key","guild:100:200:44"}});QCOMPARE(restarted.count(),0);
        client.dispatch("STREAM_CREATE",{{"stream_key","guild:100:200:43"},{"rtc_server_id","999"},{"rtc_channel_id","998"}});
        QCOMPARE(restarted.count(),1);QCOMPARE(created.count(),1);QCOMPARE(client.m_watchingStream,"guild:100:200:43");
        client.dispatch("STREAM_CREATE",{{"stream_key","guild:100:200:43"},{"rtc_server_id","999"},{"rtc_channel_id","998"}});
        QCOMPARE(restarted.count(),1); // A duplicate announcement is not another subscription.
        client.dispatch("STREAM_DELETE",{{"stream_key","guild:100:200:43"}});
        client.dispatch("VOICE_STATE_UPDATE",{{"guild_id","100"},{"channel_id","200"},{"user_id","43"},{"self_stream",false}});
        client.dispatch("VOICE_STATE_UPDATE",{{"guild_id","100"},{"channel_id","200"},{"user_id","43"},{"self_stream",true}});
        QCOMPARE(restarted.count(),2);
        client.dispatch("VOICE_STATE_UPDATE",{{"guild_id","100"},{"channel_id","200"},{"user_id","43"},{"self_stream",true}});
        QCOMPARE(restarted.count(),2);
        client.dispatch("STREAM_DELETE",{{"stream_key","guild:100:200:43"}});client.stopWatchingStream();
        QVERIFY(client.m_desiredStream.isEmpty());client.dispatch("STREAM_CREATE",{{"stream_key","guild:100:200:43"}});QCOMPARE(restarted.count(),2);
    }
    void streamRestartNeverFollowsAnotherChannelOrPreviousSelection() {
        DiscordClient client;client.m_currentUserId="42";client.m_currentGuildId="100";client.m_connected=true;
        client.acceptVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","42"},{"session_id","generated-session"}});
        QVERIFY(client.watchStream("guild:100:200:43"));client.dispatch("STREAM_DELETE",{{"stream_key","guild:100:200:43"}});
        QSignalSpy restarted(&client,&DiscordClient::streamRestarting);
        client.acceptVoiceState({{"guild_id","100"},{"channel_id","201"},{"user_id","42"},{"session_id","generated-next"}});
        QVERIFY(client.m_desiredStream.isEmpty());client.dispatch("STREAM_CREATE",{{"stream_key","guild:100:200:43"}});QCOMPARE(restarted.count(),0);
        client.acceptVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","42"},{"session_id","generated-next"}});
        QVERIFY(client.watchStream("guild:100:200:44"));client.dispatch("STREAM_CREATE",{{"stream_key","guild:100:200:43"}});QCOMPARE(restarted.count(),0);
        QCOMPARE(client.m_desiredStream,"guild:100:200:44");client.logout();QVERIFY(client.m_desiredStream.isEmpty());
    }
    void streamEventsRouteOnlyToRequestedStream() {
        DiscordClient client; client.m_currentUserId="42"; client.m_currentGuildId="100"; client.m_connected=true;
        client.acceptVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","42"},{"session_id","fixture"}});
        QVERIFY(client.watchStream("guild:100:200:43"));
        QSignalSpy created(&client,&DiscordClient::streamCreated), server(&client,&DiscordClient::streamServerReceived);
        client.dispatch("STREAM_CREATE",{{"stream_key","guild:100:200:44"},{"rtc_server_id","999"}});
        client.dispatch("STREAM_SERVER_UPDATE",{{"stream_key","guild:100:200:44"},{"token","fixture"}});
        QCOMPARE(created.count(),0); QCOMPARE(server.count(),0);
        client.dispatch("STREAM_CREATE",{{"stream_key","guild:100:200:43"},{"rtc_server_id","999"},{"rtc_channel_id","998"}});
        client.dispatch("STREAM_SERVER_UPDATE",{{"stream_key","guild:100:200:43"},{"token","fixture"}});
        QCOMPARE(created.count(),1); QCOMPARE(server.count(),1);
        QVERIFY(!client.m_streamObjects.value("guild:100:200:43").contains("token"));
        QVERIFY(!client.m_streamObjects.value("guild:100:200:43").contains("rtc_server_id"));
        client.stopWatchingStream("guild:100:200:44"); QVERIFY(!client.m_watchingStream.isEmpty());
        client.stopWatchingStream(); QVERIFY(client.m_watchingStream.isEmpty());
    }

    void modelDeduplicatesAndBounds() {
        RecordModel model(nullptr, 3);
        QAbstractItemModelTester invariant(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.replace({{{"recordId", "1"}, {"name", "first"}}, {{"recordId", "2"}},
                       {{"recordId", "1"}, {"name", "updated"}}, {{"recordId", "3"}}, {{"recordId", "4"}}});
        QCOMPARE(model.count(), 3);
        QCOMPARE(model.get(0).value("name").toString(), "updated");
        QCOMPARE(model.indexOf("4"), -1);
        QVERIFY(model.get(0).contains("attachments"));
        QCOMPARE(model.get(0).value("pending").toBool(), false);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.patch("1", {{"name", "patched"}, {"recordId", "bad"}});
        QCOMPARE(changed.count(), 1);
        QCOMPARE(model.indexOf("1"), 0);
        model.upsert({{"recordId", "4"}, {"name", "newest"}});
        QCOMPARE(model.count(), 3);
        QCOMPARE(model.indexOf("1"), -1);
        QCOMPARE(model.indexOf("2"), 0);
        QCOMPARE(model.indexOf("4"), 2);
        model.remove("3");
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.indexOf("4"), 1);
        QVERIFY(model.get(-1).isEmpty());
        QVERIFY(model.get(999).isEmpty());
        QVERIFY(!model.data({}, Qt::UserRole + 1).isValid());
        model.clear();
        QCOMPARE(model.count(), 0);
    }

    void modelMovesKeepPersistentIndicesAndData() {
        RecordModel model;
        QAbstractItemModelTester invariant(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.replace({{{"recordId", "1"}}, {{"recordId", "2"}}, {{"recordId", "3"}}});
        QPersistentModelIndex index(model.index(2));
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        model.move("3", 0);
        QCOMPARE(index.row(), 0);
        QCOMPARE(model.get(0).value("recordId").toString(), "3");
        model.move("3", 2);
        QCOMPARE(index.row(), 2);
        QCOMPARE(model.indexOf("2"), 1);
        QCOMPARE(reset.count(), 0);
    }

    void historyReconciliationPreservesRowsAndOnlyNotifiesChangedRoles() {
        RecordModel model;
        QAbstractItemModelTester invariant(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        model.replace({{{"recordId","2"},{"name","second"}},{{"recordId","3"},{"name","third"}}});
        QPersistentModelIndex retained(model.index(1));
        QSignalSpy resets(&model,&QAbstractItemModel::modelReset), changes(&model,&QAbstractItemModel::dataChanged), inserts(&model,&QAbstractItemModel::rowsInserted);
        auto page=model.rows();page.prepend({{"recordId","1"},{"name","first"}});model.reconcile(page);
        QCOMPARE(resets.count(),0);QCOMPARE(changes.count(),0);QCOMPARE(inserts.count(),1);
        QCOMPARE(retained.row(),2);QCOMPARE(retained.data(Qt::UserRole+1).toString(),QString("3"));
        model.reconcile(model.rows());QCOMPARE(changes.count(),0);QCOMPARE(inserts.count(),1);
        model.patch("3",{{"name","updated"}});QCOMPARE(changes.count(),1);
        const auto roles=changes.last().at(2).value<QList<int>>();QCOMPARE(roles,QList<int>{Qt::UserRole+2});
        auto reordered=model.rows();std::swap(reordered[0],reordered[2]);model.reconcile(reordered);
        QCOMPARE(retained.row(),0);QCOMPARE(resets.count(),0);
        reordered.removeLast();model.reconcile(reordered);QVERIFY(retained.isValid());QCOMPARE(model.count(),2);
    }

    void serverHoverUsesCachedVoiceAndHonestCounts() {
        DiscordClient client;client.m_currentUserId="42";
        client.acceptGuild({{"id","100"},{"name","Fixture"},{"member_count",7},{"approximate_presence_count",3}});
        client.m_users.insert("43",{{"id","43"},{"username","Speaker"}});
        client.m_voiceStates["100"]["43"]={{"channel_id","200"},{"streaming",true}};
        client.m_channelCache["100"]=QJsonArray{QJsonObject{{"id","200"},{"name","Voice"},{"type",2}}};
        const auto info=client.guildHoverInfo("100");QCOMPARE(info.value("voiceCount").toInt(),1);QCOMPARE(info.value("memberCount").toInt(),7);
        const auto channels=info.value("voiceChannels").toList();QCOMPARE(channels.size(),1);QCOMPARE(channels.first().toMap().value("name").toString(),QString("Voice"));
        QVERIFY(channels.first().toMap().value("people").toList().first().toMap().value("streaming").toBool());
        QVERIFY(client.guildHoverInfo("999").isEmpty());
    }

    void threadHeavySnapshotsRetainStructuralSidebarAndDefault() {
        DiscordClient client; client.m_currentUserId="42";
        QJsonArray mixed;
        for(int i=0;i<2500;++i) mixed.append(QJsonObject{{"id",QString::number(10000+i)},{"type",11},{"parent_id","102"},{"name","Bug post "+QString::number(i)}});
        mixed.append(QJsonObject{{"id","100"},{"type",4},{"name","Community"},{"position",0}});
        mixed.append(QJsonObject{{"id","101"},{"type",0},{"name","general"},{"parent_id","100"},{"position",0}});
        mixed.append(QJsonObject{{"id","102"},{"type",15},{"name","Bug reports"},{"parent_id","100"},{"position",1}});
        mixed.append(QJsonObject{{"id","103"},{"type",2},{"name","Voice"},{"parent_id","100"},{"position",2}});
        client.acceptGuild({{"id","10"},{"name","Generated server"},{"channels",mixed}});
        client.selectGuild("10");
        QVERIFY(client.m_channelCache.value("10").size()<=client.channels()->maximumRows());
        QCOMPARE(client.channels()->count(),4);
        QCOMPARE(client.currentChannelId(),QString("101"));
        QCOMPARE(client.channels()->get(0).value("recordId").toString(),QString("100"));
        QCOMPARE(client.channels()->get(1).value("recordId").toString(),QString("101"));
        QCOMPARE(client.channels()->get(2).value("recordId").toString(),QString("102"));
        QCOMPARE(client.channels()->get(3).value("recordId").toString(),QString("103"));
        for(const auto &row:client.channels()->rows()) QVERIFY(row.value("kind").toString()!="thread");
    }

    void structuralEventsBeatFullThreadCacheAndSparseThreadsStayThreads() {
        DiscordClient client; client.m_currentUserId="42";
        QJsonArray mixed{QJsonObject{{"id","101"},{"type",0},{"name","general"}},QJsonObject{{"id","102"},{"type",15},{"name","Bug reports"}}};
        for(int i=0;i<2000;++i) mixed.append(QJsonObject{{"id",QString::number(10000+i)},{"type",11},{"parent_id","102"},{"name","Post"}});
        client.acceptGuild({{"id","10"},{"name","Generated server"},{"channels",mixed}});client.selectGuild("10");
        client.dispatch("CHANNEL_CREATE",{{"id","103"},{"guild_id","10"},{"type",0},{"name","announcements"}});
        QVERIFY(!client.channels()->byId("103").isEmpty());
        QCOMPARE(client.channels()->count(),3);
        client.dispatch("THREAD_UPDATE",{{"id","99999"},{"guild_id","10"},{"parent_id","102"},{"name","Sparse unknown post"}});
        QVERIFY(client.channels()->byId("99999").isEmpty());
        client.dispatch("THREAD_CREATE",{{"id","99998"},{"guild_id","10"},{"parent_id","102"},{"type",11},{"name","New post"}});
        client.dispatch("THREAD_UPDATE",{{"id","99998"},{"guild_id","10"},{"name","Renamed post"}});
        QVERIFY(client.channels()->byId("99998").isEmpty());
        QCOMPARE(client.currentChannelId(),QString("101"));
        QVERIFY(client.m_channelCache.value("10").size()<=client.channels()->maximumRows());
    }

    void selectedThreadsUseRealParentAndNeverBecomeServerDefaults() {
        DiscordClient client; client.m_currentUserId="42";
        client.acceptGuild({{"id","10"},{"name","Generated server"},{"channels",QJsonArray{
            QJsonObject{{"id","100"},{"type",4},{"name","Community"}},
            QJsonObject{{"id","101"},{"type",0},{"parent_id","100"},{"name","general"}},
            QJsonObject{{"id","102"},{"type",15},{"parent_id","100"},{"name","Forum"}},
            QJsonObject{{"id","201"},{"type",11},{"parent_id","101"},{"name","Selected discussion"}},
            QJsonObject{{"id","202"},{"type",11},{"parent_id","102"},{"name","Forum post"}},
            QJsonObject{{"id","203"},{"type",11},{"parent_id","999"},{"name","Orphan"}},
            QJsonObject{{"id","204"},{"type",11},{"parent_id","205"},{"name","Cycle A"}},
            QJsonObject{{"id","205"},{"type",11},{"parent_id","204"},{"name","Cycle B"}}}}});
        client.selectGuild("10");QCOMPARE(client.currentChannelId(),QString("101"));QCOMPARE(client.channels()->count(),3);
        client.selectChannel("201");QCOMPARE(client.currentChannelId(),QString("201"));QCOMPARE(client.channels()->count(),4);
        QCOMPARE(client.channels()->indexOf("201"),client.channels()->indexOf("101")+1);
        client.activateChannel("202");QCOMPARE(client.currentChannelId(),QString("202"));QCOMPARE(client.channels()->count(),3);
        QVERIFY(client.channels()->byId("202").isEmpty());QCOMPARE(client.m_subscribedChannel,QString("102"));
        client.selectChannel("203");QCOMPARE(client.currentChannelId(),QString("202"));
        client.selectChannel("204");QCOMPARE(client.currentChannelId(),QString("202"));
        client.dispatch("THREAD_UPDATE",{{"id","202"},{"guild_id","10"},{"name","Renamed post"}});
        QCOMPARE(client.currentChannelName(),QString("Renamed post"));
        client.selectGuild({});client.selectGuild("10");QCOMPARE(client.currentChannelId(),QString("101"));
        client.acceptGuild({{"id","20"},{"name","Forum only"},{"channels",QJsonArray{
            QJsonObject{{"id","301"},{"type",15},{"name","Discussions"}},
            QJsonObject{{"id","302"},{"type",11},{"parent_id","301"},{"name","A post"}}}}});
        client.selectGuild("20");QCOMPARE(client.currentChannelId(),QString("301"));QCOMPARE(client.currentChannelKind(),QString("forum"));
    }

    void threadVisibilityInheritsParentAllowAndRejectsMissingOrCyclicParents() {
        DiscordClient client;client.m_currentUserId="42";
        const QJsonObject parent{{"id","101"},{"type",15},{"name","Allowed forum"},{"permission_overwrites",QJsonArray{
            QJsonObject{{"id","10"},{"type",0},{"allow",QString::number(1ULL<<10)},{"deny","0"}}}}};
        client.acceptGuild({{"id","10"},{"name","Generated server"},{"member",QJsonObject{{"roles",QJsonArray{}}}},
            {"roles",QJsonArray{QJsonObject{{"id","10"},{"permissions","0"}}}},
            {"channels",QJsonArray{parent,QJsonObject{{"id","201"},{"type",11},{"parent_id","101"},{"name","Post"}}}}});
        QVERIFY(client.channelVisible(client.cachedChannel("10","201"),"10"));
        client.selectGuild("10");client.selectChannel("201");QCOMPARE(client.currentChannelId(),QString("201"));
        client.dispatch("CHANNEL_UPDATE",{{"id","101"},{"guild_id","10"},{"permission_overwrites",QJsonArray{}}});
        QVERIFY(client.currentChannelId()!=QString("201"));
        QVERIFY(!client.channelVisible(client.cachedChannel("10","201"),"10"));
        client.selectChannel("201");QVERIFY(client.currentChannelId()!=QString("201"));
        QCOMPARE(client.m_pendingReadReceipts.size(),0);
    }

    void deletingCategoryPreservesItsStructuralChildren() {
        DiscordClient client;client.m_currentUserId="42";
        client.acceptGuild({{"id","10"},{"name","Generated server"},{"channels",QJsonArray{
            QJsonObject{{"id","100"},{"type",4},{"name","Category"}},
            QJsonObject{{"id","101"},{"type",0},{"parent_id","100"},{"name","general"}},
            QJsonObject{{"id","102"},{"type",15},{"parent_id","100"},{"name","Forum"}},
            QJsonObject{{"id","103"},{"type",2},{"parent_id","100"},{"name","Voice"}}}}});
        client.selectGuild("10");client.dispatch("CHANNEL_DELETE",{{"id","100"},{"guild_id","10"}});
        QCOMPARE(client.channels()->count(),3);QCOMPARE(client.currentChannelId(),QString("101"));
        for(const auto &id:{"101","102","103"})QVERIFY(!client.cachedChannel("10",id).isEmpty());
    }

    void directMessagesSortByLatestMessageWithoutLosingSelection() {
        DiscordClient client;
        client.m_currentUserId = "42";
        client.acceptDm({{"id", "10"}, {"type", 1}, {"name", "Older"}, {"last_message_id", "123456789012345678"}});
        client.acceptDm({{"id", "20"}, {"type", 3}, {"name", "Newer"}, {"last_message_id", "123456789012345679"}});
        QCOMPARE(client.dms()->get(0).value("recordId").toString(), "20");
        client.selectChannel("20");
        QSignalSpy resets(client.channels(), &QAbstractItemModel::modelReset);
        client.dispatch("MESSAGE_CREATE", message("123456789012345680", "10"));
        QCOMPARE(client.dms()->get(0).value("recordId").toString(), "10");
        QCOMPARE(client.channels()->get(0).value("recordId").toString(), "10");
        QCOMPARE(client.currentChannelId(), "20");
        QCOMPARE(resets.count(), 0);
        // A delayed snapshot and duplicate/replayed message do not demote it.
        client.acceptDm({{"id", "10"}, {"type", 1}, {"last_message_id", "123456789012345678"}});
        client.dispatch("MESSAGE_CREATE", message("123456789012345677", "20"));
        QCOMPARE(client.dms()->get(0).value("recordId").toString(), "10");
        // Outgoing confirmations also move the conversation when no gateway event has arrived.
        client.recordDmActivity(message("123456789012345681", "20"));
        QCOMPARE(client.dms()->get(0).value("recordId").toString(), "20");
        client.acceptGuild({{"id", "99"}, {"name", "Server"}, {"channels", QJsonArray{}}});
        client.selectGuild("99");
        client.recordDmActivity(message("123456789012345682", "10"));
        QCOMPARE(client.channels()->count(), 0);
        client.selectGuild({});
        QCOMPARE(client.channels()->get(0).value("recordId").toString(), "10");
    }

    void emptyDirectMessagesUseCreationOrderAndServerIconsUseValidSize() {
        DiscordClient client;
        client.acceptDm({{"id", "10"}, {"type", 1}});
        client.acceptDm({{"id", "20"}, {"type", 1}});
        QCOMPARE(client.dms()->get(0).value("recordId").toString(), "20");
        client.acceptDm({{"id", "10"}, {"type", 1}, {"last_message_id", "30"}});
        QCOMPARE(client.dms()->get(0).value("recordId").toString(), "10");
        QCOMPARE(client.guildRow({{"id", "10"}, {"icon", "abc123"}}).value("iconUrl").toString(),
                 "https://cdn.discordapp.com/icons/10/abc123.png?size=4096");
    }

    void eventsStayInSelectedChannel() {
        DiscordClient client;
        client.m_currentChannelId = "101";
        client.m_currentUserId = "42";
        client.dispatch("MESSAGE_CREATE", message("1001"));
        client.dispatch("MESSAGE_CREATE", message("1001"));
        client.dispatch("MESSAGE_CREATE", message("1002", "102"));
        QCOMPARE(client.messages()->count(), 1);
        QVERIFY(client.messages()->get(0).value("own").toBool());
        client.dispatch("MESSAGE_UPDATE", QJsonObject{{"id", "1001"}, {"channel_id", "101"},
                        {"content", "Edited"}, {"edited_timestamp", "2026-10-21T12:01:00Z"}});
        const auto row = client.messages()->get(0);
        QCOMPARE(row.value("author").toString(), "Fixture");
        QCOMPARE(row.value("content").toString(), "Edited");
        QVERIFY(row.value("edited").toBool());
        client.dispatch("MESSAGE_DELETE", QJsonObject{{"id", "1001"}, {"channel_id", "102"}});
        QCOMPARE(client.messages()->count(), 1);
        client.dispatch("MESSAGE_DELETE", QJsonObject{{"id", "1001"}, {"channel_id", "101"}});
        QCOMPARE(client.messages()->count(), 0);
    }

    void historyAndLiveEventsDoNotLoseEditsOrResurrectDeletes() {
        DiscordClient client;
        client.m_currentChannelId = "101";
        client.m_loadingMessages = true;
        client.dispatch("MESSAGE_UPDATE", QJsonObject{{"id", "1001"}, {"channel_id", "101"}, {"content", "Live edit"}});
        client.dispatch("MESSAGE_DELETE", QJsonObject{{"id", "1002"}, {"channel_id", "101"}});
        client.dispatch("MESSAGE_CREATE", message("1003", "101", "Newest"));
        client.mergeMessages(QJsonArray{message("1002"), message("1001"), message("1003", "101", "Stale")});
        QCOMPARE(client.messages()->count(), 2);
        QCOMPARE(client.messages()->get(0).value("recordId").toString(), "1001");
        QCOMPARE(client.messages()->get(0).value("content").toString(), "Live edit");
        QCOMPARE(client.messages()->get(0).value("author").toString(), "Fixture");
        QCOMPARE(client.messages()->get(1).value("content").toString(), "Newest");
    }

    void messageCacheRemainsBounded() {
        DiscordClient client;
        client.m_currentChannelId = "101";
        for (int i = 1; i <= 1100; ++i) client.acceptMessage(message(QString::number(i)));
        QCOMPARE(client.messages()->count(), 1000);
        QCOMPARE(client.m_messageObjects.size(), 1000);
        QCOMPARE(client.messages()->get(0).value("recordId").toString(), "101");
        QCOMPARE(client.messages()->get(999).value("recordId").toString(), "1100");
        QVERIFY(client.hasMoreMessages());
    }

    void historyWindowCanPagePastOneThousandAndReturnToLiveWithoutLosingEvents() {
        HttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        DiscordClient client;useFixture(client,fixture);client.m_currentChannelId="101";
        QJsonArray initial;for(int id=1001;id<=2000;++id)initial.append(message(QString::number(id)));
        client.mergeMessages(initial);client.m_hasMoreMessages=true;
        QSignalSpy reset(client.messages(),&QAbstractItemModel::modelReset);
        QPersistentModelIndex anchor(client.messages()->index(10));
        auto enqueue=[&](int first,int last,int delay=0){QJsonArray page;for(int id=last;id>=first;--id)page.append(message(QString::number(id)));fixture.responses.enqueue({200,QJsonDocument(page).toJson(),{},delay});};
        enqueue(951,1000);client.loadOlderMessages();client.loadOlderMessages();
        QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);
        QCOMPARE(fixture.received.size(),1);QVERIFY(fixture.received.last().startsWith("GET /channels/101/messages?limit=50&before=1001 "));
        QCOMPARE(client.messages()->count(),1000);QCOMPARE(client.m_messageObjects.size(),1000);
        QCOMPARE(client.messages()->get(0).value("recordId").toString(),"951");QCOMPARE(client.messages()->get(999).value("recordId").toString(),"1950");
        QVERIFY(anchor.isValid());QCOMPARE(anchor.row(),60);QVERIFY(client.hasNewerMessages());
        client.acceptMessage(message("2001","101","Live content"));
        client.acceptMessage({{"id","2001"},{"channel_id","101"},{"content","Live edit"}},true);
        client.acceptMessage(message("2002"));client.dispatch("MESSAGE_DELETE",{{"id","2002"},{"channel_id","101"}});
        QCOMPARE(client.messages()->get(999).value("recordId").toString(),"1950");
        enqueue(1951,2000);client.loadNewerMessages();QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);
        QVERIFY(fixture.received.last().startsWith("GET /channels/101/messages?limit=50&after=1950 "));
        QVERIFY(client.hasNewerMessages());QCOMPARE(client.messages()->get(0).value("recordId").toString(),"1001");
        enqueue(2001,2002);client.loadNewerMessages();QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);
        QVERIFY(!client.hasNewerMessages());QCOMPARE(client.messages()->count(),1000);QCOMPARE(client.m_messageObjects.size(),1000);
        QCOMPARE(client.messages()->get(999).value("recordId").toString(),"2001");
        QCOMPARE(client.messages()->byId("2001").value("content").toString(),"Live edit");QVERIFY(client.messages()->byId("2002").isEmpty());
        QVERIFY(client.m_deferredLiveMessages.isEmpty());QCOMPARE(reset.count(),0);
    }

    void historyStopsOnDuplicatePagesAndErrorsAndJumpCancelsOldResponses() {
        HttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        DiscordClient client;useFixture(client,fixture);client.m_currentChannelId="101";
        QJsonArray page;for(int id=1001;id<=1050;++id)page.append(message(QString::number(id)));
        client.mergeMessages(page);client.m_hasMoreMessages=true;
        fixture.responses.enqueue({200,QJsonDocument(page).toJson()});client.loadOlderMessages();
        QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);QVERIFY(!client.hasMoreMessages());
        client.m_hasNewerMessages=true;fixture.responses.enqueue({200,QJsonDocument(page).toJson()});client.loadNewerMessages();
        QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);QVERIFY(client.hasNewerMessages());QVERIFY(!client.error().isEmpty());
        client.clearError();
        client.m_hasMoreMessages=true;fixture.responses.enqueue({403,R"({"message":"Fixture history denied"})"});client.loadOlderMessages();
        QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);QVERIFY(!client.error().isEmpty());
        client.clearError();client.m_hasNewerMessages=true;
        fixture.responses.enqueue({200,QJsonDocument(QJsonArray{message("9000")}).toJson(),{},150});client.loadNewerMessages();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(),4,2000);
        fixture.responses.enqueue({200,QJsonDocument(QJsonArray{message("2000")}).toJson()});client.jumpToPresent();
        QTRY_COMPARE_WITH_TIMEOUT(client.messages()->count(),1,2000);QTest::qWait(200);
        QCOMPARE(client.messages()->get(0).value("recordId").toString(),"2000");QVERIFY(!client.hasNewerMessages());QVERIFY(!client.hasMoreMessages());
        QVERIFY(fixture.received.last().startsWith("GET /channels/101/messages?limit=50 "));
    }

    void repeatedOlderPagesDoNotRollBackAWindowAdvancedByLiveMessages() {
        HttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        DiscordClient client;useFixture(client,fixture);client.m_currentChannelId="101";
        QJsonArray initial;for(int id=1001;id<=2000;++id)initial.append(message(QString::number(id)));
        client.mergeMessages(initial);client.m_hasMoreMessages=true;
        QJsonArray echoed;for(int id=1001;id<=1050;++id)echoed.append(message(QString::number(id)));
        fixture.responses.enqueue({200,QJsonDocument(echoed).toJson(),{},100});
        QSignalSpy resets(client.messages(),&QAbstractItemModel::modelReset);
        client.loadOlderMessages();QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(),1,1000);
        client.acceptMessage(message("2001","101","New first"));client.acceptMessage(message("2002","101","New second"));
        QCOMPARE(client.messages()->get(0).value("recordId").toString(),QString("1003"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);
        QCOMPARE(client.messages()->count(),1000);QCOMPARE(client.messages()->get(0).value("recordId").toString(),QString("1003"));
        QCOMPARE(client.messages()->get(999).value("recordId").toString(),QString("2002"));
        QCOMPARE(client.messages()->byId("2001").value("content").toString(),QString("New first"));
        QVERIFY(!client.hasMoreMessages());QVERIFY(!client.hasNewerMessages());QCOMPARE(resets.count(),0);
        client.loadOlderMessages();client.loadOlderMessages();QTest::qWait(30);QCOMPARE(fixture.received.size(),1);
    }

    void aFullWindowBeingReadKeepsItsAnchorAndDefersLiveArrivalsUntilExplicitlyCaughtUp() {
        HttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        DiscordClient client;useFixture(client,fixture);client.m_currentChannelId="101";
        QJsonArray initial;for(int id=1001;id<=2000;++id)initial.append(message(QString::number(id)));
        client.mergeMessages(initial);client.setFollowingMessages(false);
        QPersistentModelIndex anchor(client.messages()->index(0));QSignalSpy removed(client.messages(),&QAbstractItemModel::rowsRemoved);
        QSignalSpy changed(&client,&DiscordClient::stateChanged);
        client.acceptMessage(message("2001","101","Live while reading"));
        client.acceptMessage({{"id","2001"},{"channel_id","101"},{"content","Live edit while reading"}},true);
        QCOMPARE(client.messages()->count(),1000);QCOMPARE(client.messages()->get(0).value("recordId").toString(),QString("1001"));
        QVERIFY(anchor.isValid());QCOMPARE(anchor.row(),0);QCOMPARE(removed.count(),0);QVERIFY(client.hasNewerMessages());QVERIFY(changed.count()>0);
        QCOMPARE(client.m_deferredLiveMessages.value("2001").value("content").toString(),QString("Live edit while reading"));
        client.setFollowingMessages(true); // A viewport state change alone does not discard the old window.
        QCOMPARE(client.messages()->get(0).value("recordId").toString(),QString("1001"));QVERIFY(client.messages()->byId("2001").isEmpty());
        fixture.responses.enqueue({200,QJsonDocument(QJsonArray{message("2001","101","Older REST snapshot")}).toJson()});
        client.loadNewerMessages();QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);
        QVERIFY(!client.hasNewerMessages());QVERIFY(client.m_deferredLiveMessages.isEmpty());QCOMPARE(client.messages()->count(),1000);
        QCOMPARE(client.messages()->get(0).value("recordId").toString(),QString("1002"));
        QCOMPARE(client.messages()->byId("2001").value("content").toString(),QString("Live edit while reading"));
        client.acceptMessage(message("2002","101","Following the tail"));
        QCOMPARE(client.messages()->get(0).value("recordId").toString(),QString("1003"));QCOMPARE(client.messages()->get(999).value("recordId").toString(),QString("2002"));
        QVERIFY(client.m_deferredLiveMessages.isEmpty());QVERIFY(!client.hasNewerMessages());
        client.setFollowingMessages(false);client.clearSelection();QVERIFY(client.m_followingMessages);
    }

    void liveMessagesCannotEvictTheReadAnchorWhileAnOlderPageIsInFlight() {
        HttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        DiscordClient client;useFixture(client,fixture);client.m_currentChannelId="101";
        QJsonArray initial;for(int id=1001;id<=2000;++id)initial.append(message(QString::number(id)));
        client.mergeMessages(initial);client.m_hasMoreMessages=true;client.setFollowingMessages(false);
        QJsonArray older;for(int id=951;id<=1000;++id)older.append(message(QString::number(id)));
        fixture.responses.enqueue({200,QJsonDocument(older).toJson(),{},100});
        QPersistentModelIndex anchor(client.messages()->index(0));client.loadOlderMessages();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(),1,1000);client.acceptMessage(message("2001","101","Deferred during page"));
        QVERIFY(anchor.isValid());QCOMPARE(anchor.row(),0);QCOMPARE(client.messages()->get(0).value("recordId").toString(),QString("1001"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);QVERIFY(anchor.isValid());QCOMPARE(anchor.row(),50);
        QCOMPARE(client.messages()->get(0).value("recordId").toString(),QString("951"));QVERIFY(client.messages()->byId("2001").isEmpty());
        QCOMPARE(client.m_deferredLiveMessages.value("2001").value("content").toString(),QString("Deferred during page"));
        QVERIFY(client.hasNewerMessages());QCOMPARE(client.m_messageObjects.size(),1000);
    }

    void paginationRejectsOtherChannelsAndWrongSideRecordsBeforeMerging() {
        HttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        DiscordClient client;useFixture(client,fixture);client.m_currentChannelId="101";
        QJsonArray initial;for(int id=1001;id<=1050;++id)initial.append(message(QString::number(id)));
        client.mergeMessages(initial);client.m_hasMoreMessages=true;
        const auto before=client.messages()->rows();
        QJsonArray foreign;for(int id=951;id<=1000;++id)foreign.append(message(QString::number(id),"102"));
        fixture.responses.enqueue({200,QJsonDocument(foreign).toJson()});client.loadOlderMessages();
        QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);QCOMPARE(client.messages()->rows(),before);QVERIFY(!client.hasMoreMessages());
        client.m_hasMoreMessages=true;
        fixture.responses.enqueue({200,QJsonDocument(QJsonArray{message("999","101","Accepted older"),message("1001"),message("1100"),message("998","102")}).toJson()});
        client.loadOlderMessages();QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);
        QCOMPARE(client.messages()->count(),51);QCOMPARE(client.messages()->get(0).value("recordId").toString(),QString("999"));
        QVERIFY(client.messages()->byId("1100").isEmpty());QVERIFY(client.messages()->byId("998").isEmpty());QVERIFY(!client.hasMoreMessages());
    }

    void anInvalidNewerPageStopsRetryingAndDoesNotFlushDeferredLiveMessagesAcrossAGap() {
        HttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        DiscordClient client;useFixture(client,fixture);client.m_currentChannelId="101";
        QJsonArray initial;for(int id=500;id<=549;++id)initial.append(message(QString::number(id)));
        client.mergeMessages(initial);client.m_hasNewerMessages=true;
        client.acceptMessage(message("9000","101","Live"));client.acceptMessage({{"id","9000"},{"channel_id","101"},{"content","Live edit"}},true);
        const auto before=client.messages()->rows();fixture.responses.enqueue({200,QJsonDocument(initial).toJson()});
        client.loadNewerMessages();QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);
        QCOMPARE(client.messages()->rows(),before);QVERIFY(client.hasNewerMessages());QVERIFY(!client.error().isEmpty());
        QCOMPARE(client.m_deferredLiveMessages.value("9000").value("content").toString(),QString("Live edit"));
        client.loadNewerMessages();client.loadNewerMessages();QTest::qWait(30);QCOMPARE(fixture.received.size(),1);
        client.clearError();fixture.responses.enqueue({200,"[]"});client.loadNewerMessages();
        QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);QCOMPARE(fixture.received.size(),2);QVERIFY(!client.hasNewerMessages());
        QCOMPARE(client.messages()->byId("9000").value("content").toString(),QString("Live edit"));QVERIFY(client.m_deferredLiveMessages.isEmpty());
    }

    void aFullPageDeletedWhileLoadingDoesNotPretendNewerHistoryIsExhausted() {
        HttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        DiscordClient client;useFixture(client,fixture);client.m_currentChannelId="101";
        client.mergeMessages(QJsonArray{message("500"),message("549")});client.m_hasNewerMessages=true;
        QJsonArray next;for(int id=550;id<=599;++id)next.append(message(QString::number(id)));
        fixture.responses.enqueue({200,QJsonDocument(next).toJson(),{},100});client.loadNewerMessages();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(),1,1000);
        for(int id=550;id<=599;++id)client.dispatch("MESSAGE_DELETE",{{"id",QString::number(id)},{"channel_id","101"}});
        client.acceptMessage(message("9000","101","Deferred newest"));
        QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);QVERIFY(client.hasNewerMessages());QVERIFY(!client.error().isEmpty());
        QCOMPARE(client.messages()->count(),2);QCOMPARE(client.messages()->get(1).value("recordId").toString(),QString("549"));
        QVERIFY(client.messages()->byId("9000").isEmpty());QCOMPARE(client.m_deferredLiveMessages.size(),1);
    }

    void delayedSendConfirmationPreservesChronology() {
        DiscordClient client;
        client.m_currentChannelId = "101";
        client.acceptMessage(message("1003", "101", "Gateway event arrived first"));
        client.acceptMessage(message("1002", "101", "Older send confirmation arrived later"));
        QCOMPARE(client.messages()->count(), 2);
        QCOMPARE(client.messages()->get(0).value("recordId").toString(), "1002");
        QCOMPARE(client.messages()->get(1).value("recordId").toString(), "1003");
    }

    void remoteContentIsPlainAndAttachmentUrlsAreRestricted() {
        DiscordClient client;
        auto object = message("1", "101", "<img src='file:///etc/passwd'>\nplain & text");
        object.insert("attachments", QJsonArray{
            QJsonObject{{"filename", "image.png"}, {"url", "https://cdn.discordapp.com/attachments/1/2/image.png?ex=123"}},
            QJsonObject{{"filename", "bad"}, {"url", "file:///etc/passwd"}},
            QJsonObject{{"filename", "bad2"}, {"url", "https://cdn.discordapp.com.attacker.invalid/anything"}}});
        const auto row = client.messageRow(object);
        QCOMPARE(row.value("content").toString(), object.value("content").toString());
        QCOMPARE(row.value("attachments").toList().size(), 1);
        QVERIFY(DiscordClient::safeCdnUrl("https://user:password@cdn.discordapp.com/x").isEmpty());
        QVERIFY(DiscordClient::safeCdnUrl("http://cdn.discordapp.com/x").isEmpty());
        QVERIFY(DiscordClient::safeCdnUrl("https://cdn.discordapp.com:8443/x").isEmpty());
        QVERIFY(DiscordClient::safeCdnUrl("https://media.discordapp.net/attachments/x").startsWith("https://"));
        QVERIFY(!DiscordClient::validId("42/../@me"));
    }

    void permissionOverwriteOrder() {
        DiscordClient client;
        client.m_currentUserId = "42";
        QJsonObject guild{{"id", "10"}, {"owner_id", "99"},
            {"member", QJsonObject{{"roles", QJsonArray{"20", "21"}}}},
            {"roles", QJsonArray{QJsonObject{{"id", "10"}, {"permissions", "1024"}},
                                  QJsonObject{{"id", "20"}, {"permissions", "0"}},
                                  QJsonObject{{"id", "21"}, {"permissions", "0"}}}}};
        client.m_guildObjects.insert("10", guild);
        QJsonObject channel{{"permission_overwrites", QJsonArray{
            QJsonObject{{"id", "20"}, {"type", 0}, {"deny", "1024"}, {"allow", "0"}},
            QJsonObject{{"id", "21"}, {"type", 0}, {"deny", "0"}, {"allow", "1024"}}}}};
        QVERIFY(client.channelVisible(channel, "10"));
        auto overwrites = channel.value("permission_overwrites").toArray();
        overwrites.append(QJsonObject{{"id", "42"}, {"type", 1}, {"deny", "1024"}, {"allow", "0"}});
        channel.insert("permission_overwrites", overwrites);
        QVERIFY(!client.channelVisible(channel, "10"));
        guild.insert("owner_id", "42");
        client.m_guildObjects.insert("10", guild);
        QVERIFY(client.channelVisible(channel, "10"));
    }

    void demoStaysOfflineAndLogoutClearsSession() {
        DiscordClient client;
        QSignalSpy sent(&client, &DiscordClient::messageSent);
        client.loadDemo();
        QVERIFY(client.demoMode());
        QVERIFY(!client.connected());
        QVERIFY(client.m_token.isEmpty());
        QCOMPARE(client.guilds()->count(), 3);
        QVERIFY(client.channels()->count() >= 4);
        QVERIFY(client.messages()->count() > 0);
        const auto before = client.messages()->count();
        QVERIFY(client.sendMessage("A local test"));
        QCOMPARE(sent.count(), 1);
        QCOMPARE(client.messages()->count(), before + 1);
        QCOMPARE(client.m_queue.size(), 0);
        QCOMPARE(client.m_replies.size(), 0);
        QVERIFY(!client.sendVoiceState("2000", "2201", false, false));
        client.logout();
        QVERIFY(!client.demoMode());
        QVERIFY(client.currentUserId().isEmpty());
        QCOMPARE(client.messages()->count(), 0);
        QCOMPARE(client.guilds()->count(), 0);
        client.login("invalid\ntoken");
        QVERIFY(!client.error().isEmpty());
        QVERIFY(client.m_token.isEmpty());
    }

    void getRateLimitRetriesButPostDoesNot() {
        HttpFixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        fixture.responses.enqueue({429, R"({"retry_after":0.01,"global":true})", {}, 0});
        fixture.responses.enqueue({200, R"({"ok":true})", {}, 0});
        DiscordClient client;
        useFixture(client, fixture);
        int callbacks = 0;
        bool successful = false;
        client.request("GET", "/fixture", {}, [&](const QJsonValue &value, const QString &error) {
            ++callbacks;
            successful = error.isEmpty() && value.toObject().value("ok").toBool();
        });
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 1, 3000);
        QVERIFY(successful);
        QCOMPARE(fixture.received.size(), 2);
        fixture.responses.enqueue({429, R"({"retry_after":0.01})", {}, 0});
        QString failure;
        client.request("POST", "/fixture", {{"content", "fixture"}}, [&](const QJsonValue &, const QString &error) {
            ++callbacks;
            failure = error;
        });
        QTRY_COMPARE_WITH_TIMEOUT(callbacks, 2, 3000);
        QVERIFY(!failure.isEmpty());
        QTest::qWait(250);
        QCOMPARE(fixture.received.size(), 3);
    }

    void logoutCancelsOutstandingCallbacks() {
        HttpFixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        fixture.responses.enqueue({200, R"({"old_session":true})", {}, 250});
        DiscordClient client;
        useFixture(client, fixture);
        int callbacks = 0;
        client.request("GET", "/fixture", {}, [&](const QJsonValue &, const QString &) { ++callbacks; });
        QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(), 1, 1000);
        client.logout();
        QTest::qWait(350);
        QCOMPARE(callbacks, 0);
        QVERIFY(client.m_token.isEmpty());
        QCOMPARE(client.m_queue.size(), 0);
        QCOMPARE(client.m_replies.size(), 0);
    }

    void staleChannelHistoryCannotOverwriteNewSelection() {
        HttpFixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        fixture.responses.enqueue({200, QJsonDocument(QJsonArray{message("1", "101", "Old channel")}).toJson(), {}, 250});
        fixture.responses.enqueue({200, QJsonDocument(QJsonArray{message("2", "102", "New channel")}).toJson(), {}, 0});
        DiscordClient client;
        useFixture(client, fixture);
        client.m_currentChannelId = "101";
        client.fetchMessages(false);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.received.size(), 1, 1000);
        client.clearSelection();
        client.m_currentChannelId = "102";
        client.fetchMessages(false);
        QTRY_COMPARE_WITH_TIMEOUT(client.messages()->count(), 1, 1000);
        QTest::qWait(350);
        QCOMPARE(client.currentChannelId(), "102");
        QCOMPARE(client.messages()->get(0).value("content").toString(), "New channel");
        QVERIFY(!client.loadingMessages());
    }

    void liveArrivalDuringFirstPageDoesNotDisablePagination() {
        HttpFixture fixture;
        QVERIFY(fixture.server.listen(QHostAddress::LocalHost));
        QJsonArray page;
        for (int id = 1000; id < 1050; ++id) page.append(message(QString::number(id)));
        fixture.responses.enqueue({200, QJsonDocument(page).toJson(), {}, 100});
        DiscordClient client;
        useFixture(client, fixture);
        client.m_currentChannelId = "101";
        client.fetchMessages(false);
        client.acceptMessage(message("1050"));
        QTRY_COMPARE_WITH_TIMEOUT(client.messages()->count(), 51, 1000);
        QVERIFY(client.hasMoreMessages());
        QVERIFY(!client.loadingMessages());
    }

    void voiceEventsForwardUnmodified() {
        DiscordClient client;
        QSignalSpy state(&client, &DiscordClient::voiceStateReceived);
        QSignalSpy server(&client, &DiscordClient::voiceServerReceived);
        const QJsonObject voice{{"user_id", "42"}, {"channel_id", "101"}, {"session_id", "fixture"}};
        const QJsonObject endpoint{{"guild_id", "10"}, {"endpoint", "fixture.discord.media"}, {"token", "fixture"}};
        client.dispatch("VOICE_STATE_UPDATE", voice);
        client.dispatch("VOICE_SERVER_UPDATE", endpoint);
        QCOMPARE(state.count(), 1);
        QCOMPARE(server.count(), 1);
        QCOMPARE(qvariant_cast<QJsonObject>(state.at(0).at(0)), voice);
        QCOMPARE(qvariant_cast<QJsonObject>(server.at(0).at(0)), endpoint);
    }

    void voiceParticipantsFollowChannelMovesAndMuteState() {
        DiscordClient client;
        client.m_currentUserId = "42";
        const QJsonObject friendUser{{"id", "50"}, {"username", "Friend"}};
        const QJsonObject friendMember{{"user", friendUser}};
        const QJsonObject friendVoice{{"user_id", "50"}, {"channel_id", "101"}, {"self_mute", true}};
        client.acceptGuild({{"id", "10"}, {"name", "Fixture"}, {"channels", QJsonArray{}},
            {"members", QJsonArray{friendMember}}, {"voice_states", QJsonArray{friendVoice}}});
        client.selectGuild("10");
        QCOMPARE(client.voiceMembers()->count(), 1);
        auto row = client.voiceMembers()->byId("50");
        QCOMPARE(row.value("name").toString(), "Friend");
        QCOMPARE(row.value("channelId").toString(), "101");
        QVERIFY(row.value("muted").toBool());
        QVERIFY(!row.contains("session_id"));
        client.setVoiceGuild("10");
        client.setSpeaking("50", true);
        QVERIFY(client.voiceMembers()->byId("50").value("speaking").toBool());
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "10"}, {"user_id", "50"}, {"channel_id", "102"},
            {"self_mute", false}, {"deaf", true}, {"member", QJsonObject{{"nick", "Nickname"}}}});
        row = client.voiceMembers()->byId("50");
        QCOMPARE(row.value("channelId").toString(), "102");
        QCOMPARE(row.value("name").toString(), "Nickname");
        QVERIFY(row.value("deafened").toBool());
        QVERIFY(!row.value("muted").toBool());
        client.selectGuild({});
        QCOMPARE(client.voiceMembers()->count(), 1); // Joined channel stays visible when browsing DMs.
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "10"}, {"user_id", "50"}, {"channel_id", QJsonValue::Null}});
        QCOMPARE(client.voiceMembers()->count(), 0);
        client.logout();
        QVERIFY(client.m_voiceStates.isEmpty());
    }

    void camerasRemainDistinctFromGoLiveAcrossPresenceUpdates() {
        DiscordClient client;
        client.m_currentUserId = "42";
        const QJsonObject cameraUser{{"id", "50"}, {"username", "Mobile camera"}};
        const QJsonObject otherUser{{"id", "51"}, {"username", "Voice only"}};
        client.acceptGuild({{"id", "10"}, {"name", "Fixture"},
            {"channels", QJsonArray{QJsonObject{{"id", "101"}, {"type", 2}, {"name", "Lounge"}},
                                    QJsonObject{{"id", "102"}, {"type", 2}, {"name", "Other"}}}},
            {"members", QJsonArray{QJsonObject{{"user", cameraUser}}, QJsonObject{{"user", otherUser}}}},
            {"voice_states", QJsonArray{
                QJsonObject{{"user_id", "50"}, {"channel_id", "101"}, {"self_video", true}, {"session_id", "private-camera-session"}},
                QJsonObject{{"user_id", "51"}, {"channel_id", "101"}}}}});
        client.selectGuild("10");
        client.setVoiceGuild("10");
        client.acceptDm({{"id", "201"}, {"type", 1}, {"recipients", QJsonArray{cameraUser}}});
        const auto checkCamera = [&] (bool enabled) {
            const auto row = client.voiceMembers()->byId("50");
            QCOMPARE(row.value("camera").toBool(), enabled);
            QVERIFY(!row.value("streaming").toBool());
            QVERIFY(row.value("streamKey").toString().isEmpty());
            QVERIFY(!row.contains("self_video"));
            QVERIFY(!row.contains("session_id"));
            QCOMPARE(client.members()->byId("50").value("camera").toBool(), enabled);
            QCOMPARE(client.memberInfoForGuild("50", "10").value("camera").toBool(), enabled);
            QCOMPARE(client.dms()->byId("201").value("camera").toBool(), enabled);
            QCOMPARE(client.userIndicators("50").value("camera").toBool(), enabled);
            QCOMPARE(client.streams()->count(), 0);
        };
        checkCamera(true);
        QVERIFY(client.voiceMembers()->byId("51").contains("camera"));
        QCOMPARE(client.voiceMembers()->byId("51").value("camera").metaType().id(), int(QMetaType::Bool));
        QVERIFY(!client.voiceMembers()->byId("51").value("camera").toBool());
        const auto hovered = client.guildHoverInfo("10").value("voiceChannels").toList();
        const auto hoveredPeople = hovered.first().toMap().value("people").toList();
        QVERIFY(std::any_of(hoveredPeople.cbegin(), hoveredPeople.cend(), [](const QVariant &person) {
            return person.toMap().value("camera").toBool();
        }));

        QSignalSpy resets(client.voiceMembers(), &QAbstractItemModel::modelReset);
        QSignalSpy updates(client.voiceMembers(), &QAbstractItemModel::dataChanged);
        client.setSpeaking("50", true);
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "10"}, {"user_id", "50"}, {"channel_id", "101"}, {"self_video", false}});
        checkCamera(false);
        QVERIFY(client.voiceMembers()->byId("50").value("speaking").toBool());
        QVERIFY(updates.count() >= 1);
        QCOMPARE(resets.count(), 0);
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "10"}, {"user_id", "50"}, {"channel_id", "102"}, {"self_video", true}});
        checkCamera(true);
        QCOMPARE(client.voiceMembers()->byId("50").value("channelId").toString(), "102");
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "10"}, {"user_id", "50"}, {"channel_id", "102"}});
        checkCamera(false); // An absent self_video value defaults to false.
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "10"}, {"user_id", "50"}, {"channel_id", "102"},
            {"self_video", true}, {"self_stream", true}});
        QVERIFY(client.voiceMembers()->byId("50").value("camera").toBool());
        QVERIFY(client.voiceMembers()->byId("50").value("streaming").toBool());
        QCOMPARE(client.voiceMembers()->byId("50").value("streamKey").toString(), "guild:10:102:50");
        QCOMPARE(client.streams()->count(), 1);
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "10"}, {"user_id", "50"}, {"channel_id", QJsonValue::Null}});
        QVERIFY(client.voiceMembers()->byId("50").isEmpty());
        QVERIFY(!client.dms()->byId("201").value("camera").toBool());
        QCOMPARE(client.streams()->count(), 0);
        QCOMPARE(resets.count(), 0);
    }

    void cameraPresenceIsScopedToTheGuildWhileDmIndicatorsFollowTheUser() {
        DiscordClient client;
        client.m_currentUserId = "42";
        const QJsonObject user{{"id", "50"}, {"username", "Camera friend"}};
        for (const auto &guildId : {"10", "20"})
            client.acceptGuild({{"id", guildId}, {"name", "Fixture"}, {"members", QJsonArray{QJsonObject{{"user", user}}}}});
        client.selectGuild("10");
        client.setVoiceGuild("10");
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "10"}, {"user_id", "50"}, {"channel_id", "101"}, {"self_video", true}});
        client.selectGuild("20");
        QVERIFY(!client.members()->byId("50").value("camera").toBool());
        QVERIFY(!client.memberInfoForGuild("50", "20").value("camera").toBool());
        QVERIFY(client.memberInfoForGuild("50", "10").value("camera").toBool());
        QVERIFY(client.voiceMembers()->byId("50").value("camera").toBool()); // Joined voice remains visible.
        client.acceptDm({{"id", "201"}, {"type", 1}, {"recipients", QJsonArray{user}}});
        client.selectGuild({});
        client.m_currentChannelId = "201";
        client.rebuildMembers();
        QVERIFY(client.members()->byId("50").value("camera").toBool());
        QVERIFY(client.memberInfoForGuild("50", {}).value("camera").toBool());
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "10"}, {"user_id", "50"}, {"channel_id", QJsonValue::Null}});
        QVERIFY(!client.members()->byId("50").value("camera").toBool());
        QVERIFY(!client.memberInfoForGuild("50", {}).value("camera").toBool());
        client.selectGuild("20");
        client.setVoiceGuild("20");
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "20"}, {"user_id", "50"}, {"channel_id", "202"}});
        QVERIFY(!client.voiceMembers()->byId("50").value("camera").toBool());
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "20"}, {"user_id", "50"}, {"channel_id", "202"}, {"self_video", true}});
        QVERIFY(client.voiceMembers()->byId("50").value("camera").toBool());
        QVERIFY(!client.memberInfoForGuild("50", "10").value("camera").toBool());
        QVERIFY(client.dms()->byId("201").value("camera").toBool());
        client.logout();
        QVERIFY(client.m_voiceStates.isEmpty());
        QCOMPARE(client.voiceMembers()->count(), 0);
    }

    void supplementalVoiceSnapshotRemovesStaleMembers() {
        DiscordClient client;
        client.m_currentGuildId = "10";
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "10"}, {"user_id", "50"}, {"channel_id", "101"}});
        QCOMPARE(client.voiceMembers()->count(), 1);
        client.dispatch("READY_SUPPLEMENTAL", {{"guilds", QJsonArray{QJsonObject{{"id", "10"}, {"voice_states", QJsonArray{}}}}}});
        QCOMPARE(client.voiceMembers()->count(), 0);
    }
};

QTEST_GUILESS_MAIN(BackendTests)
#include "backend_tests.moc"
