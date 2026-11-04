#include "DiscordClient.h"
#include "RecordModel.h"

#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>

// All messages, credentials, requests and delayed responses are generated here.
// This fixture binds loopback only and never contacts a Discord account.
class ReplyHttpFixture final : public QObject {
public:
    struct Response { int status=200; QByteArray body="[]"; int delayMs=0; };
    QTcpServer server;
    QQueue<Response> responses;
    QList<QByteArray> requests;
    ReplyHttpFixture() {
        connect(&server,&QTcpServer::newConnection,this,[this]{
            while(auto *socket=server.nextPendingConnection()) {
                connect(socket,&QTcpSocket::readyRead,socket,[this,socket]{
                    const auto request=socket->property("received").toByteArray()+socket->readAll();
                    socket->setProperty("received",request);
                    if(socket->property("handled").toBool() || !request.contains("\r\n\r\n"))return;
                    socket->setProperty("handled",true);requests.append(request);
                    const auto response=responses.isEmpty()?Response{}:responses.dequeue();
                    QTimer::singleShot(response.delayMs,socket,[socket,response]{
                        if(socket->state()!=QAbstractSocket::ConnectedState)return;
                        socket->write("HTTP/1.1 "+QByteArray::number(response.status)+" Fixture\r\nContent-Type: application/json\r\nContent-Length: "+
                                      QByteArray::number(response.body.size())+"\r\nConnection: close\r\n\r\n"+response.body);
                        socket->disconnectFromHost();
                    });
                });
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            }
        });
    }
    QString base() const {return "http://127.0.0.1:"+QString::number(server.serverPort());}
    QUrl requestUrl(int index)const {
        const auto path=requests.value(index).split(' ').value(1);
        return QUrl("https://fixture.invalid"+QString::fromUtf8(path));
    }
};

class ReplyJumpTest final : public QObject {
    Q_OBJECT
    static QJsonObject message(const QString &id,const QString &content="Fixture",const QString &channel="101") {
        return {{"id",id},{"channel_id",channel},{"content",content},{"timestamp","2026-10-22T12:00:00.000Z"},
                {"author",QJsonObject{{"id","42"},{"username","Fixture user"}}}};
    }
    static QByteArray page(std::initializer_list<QJsonObject> messages) {
        QJsonArray result;for(const auto &entry:messages)result.append(entry);return QJsonDocument(result).toJson(QJsonDocument::Compact);
    }
    static void prepare(DiscordClient &client,const ReplyHttpFixture &fixture) {
        client.m_apiBase=fixture.base();client.m_token="fake-reply-jump-token";client.m_currentUserId="42";
        client.m_currentGuildId="10";client.m_currentChannelId="101";client.m_currentChannelKind="text";
        client.m_channels.replace({{{"recordId","101"},{"name","Fixture"},{"guildId","10"},{"kind","text"}},
                                  {{"recordId","102"},{"name","Other"},{"guildId","10"},{"kind","text"}}});
        client.acceptMessage(message("9000","Existing window"));client.acceptMessage(message("9001","Existing end"));
    }
private slots:
    void loadedTargetUsesTheNextEventTickWithoutAnyRestRequest() {
        ReplyHttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        DiscordClient client;prepare(client,fixture);
        client.m_token.clear(); // Local navigation also works after the network session ends.
        QSignalSpy ready(&client,&DiscordClient::messageJumpReady),failed(&client,&DiscordClient::messageJumpFailed);
        const auto before=client.messages()->rows();
        QVERIFY(client.jumpToMessage("101","9000"));QCOMPARE(ready.count(),0);
        QTRY_COMPARE(ready.count(),1);QCOMPARE(ready.first(),QList<QVariant>({QString("101"),QString("9000")}));
        QCOMPARE(client.messages()->rows(),before);QCOMPARE(fixture.requests.size(),0);QCOMPARE(failed.count(),0);
        QVERIFY(!client.jumpToMessage("102","9000"));QVERIFY(!client.jumpToMessage("101","bad/id"));
        QTest::qWait(20);QCOMPARE(fixture.requests.size(),0);QCOMPARE(ready.count(),1);
    }
    void missingTargetLoadsAnAroundWindowAndAllowsPagingInBothDirections() {
        ReplyHttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        fixture.responses.enqueue({200,page({message("501"),message("500","Reply target"),message("499")}),0});
        DiscordClient client;prepare(client,fixture);QSignalSpy ready(&client,&DiscordClient::messageJumpReady),failed(&client,&DiscordClient::messageJumpFailed);
        QVERIFY(client.jumpToMessage("101","500"));QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,2000);
        QCOMPARE(fixture.requests.size(),1);QVERIFY(fixture.requests.first().startsWith("GET "));
        const auto request=fixture.requestUrl(0);QCOMPARE(request.path(),QString("/channels/101/messages"));
        const QUrlQuery query(request);QCOMPARE(query.queryItemValue("limit"),QString("50"));QCOMPARE(query.queryItemValue("around"),QString("500"));
        QCOMPARE(client.messages()->count(),3);QCOMPARE(client.messages()->get(0).value("recordId").toString(),QString("499"));
        QCOMPARE(client.messages()->byId("500").value("content").toString(),QString("Reply target"));QVERIFY(client.messages()->byId("9000").isEmpty());
        QVERIFY(client.hasMoreMessages());QVERIFY(client.hasNewerMessages());QVERIFY(!client.loadingMessages());QCOMPARE(failed.count(),0);
        fixture.responses.enqueue({200,page({message("498")}),0});client.loadOlderMessages();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(),2,2000);QTRY_VERIFY(!client.loadingMessages());
        QCOMPARE(QUrlQuery(fixture.requestUrl(1)).queryItemValue("before"),QString("499"));QVERIFY(!client.messages()->byId("498").isEmpty());
        fixture.responses.enqueue({200,page({message("502")}),0});client.loadNewerMessages();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(),3,2000);QTRY_VERIFY(!client.loadingMessages());
        QCOMPARE(QUrlQuery(fixture.requestUrl(2)).queryItemValue("after"),QString("501"));QVERIFY(!client.messages()->byId("502").isEmpty());
    }
    void absentDeletedAndInvalidAroundResponsesLeaveTheCurrentWindowIntact() {
        ReplyHttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        DiscordClient client;prepare(client,fixture);QSignalSpy ready(&client,&DiscordClient::messageJumpReady),failed(&client,&DiscordClient::messageJumpFailed);
        const auto before=client.messages()->rows();
        fixture.responses.enqueue({200,page({message("499"),message("501")}),0});
        QVERIFY(client.jumpToMessage("101","500"));QTRY_COMPARE_WITH_TIMEOUT(failed.count(),1,2000);
        QCOMPARE(client.messages()->rows(),before);QCOMPARE(ready.count(),0);QVERIFY(!client.loadingMessages());
        client.dispatch("MESSAGE_DELETE",{{"id","600"},{"channel_id","101"}});
        QVERIFY(!client.jumpToMessage("101","600"));QTest::qWait(20);QCOMPARE(fixture.requests.size(),1);QCOMPARE(client.messages()->rows(),before);
        fixture.responses.enqueue({200,R"({"unexpected":"object"})",0});
        const int failures=failed.count();QVERIFY(client.jumpToMessage("101","700"));QTRY_COMPARE_WITH_TIMEOUT(failed.count(),failures+1,2000);
        QCOMPARE(client.messages()->rows(),before);QCOMPARE(ready.count(),0);QVERIFY(!client.loadingMessages());
    }
    void httpErrorsDoNotDiscardReadableHistory() {
        ReplyHttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        fixture.responses.enqueue({403,R"({"message":"Fixture denied"})",0});
        DiscordClient client;prepare(client,fixture);QSignalSpy ready(&client,&DiscordClient::messageJumpReady),failed(&client,&DiscordClient::messageJumpFailed);
        const auto before=client.messages()->rows();
        QVERIFY(client.jumpToMessage("101","500"));QTRY_COMPARE_WITH_TIMEOUT(failed.count(),1,2000);
        QCOMPARE(failed.first().value(0).toString(),QString("101"));QCOMPARE(failed.first().value(1).toString(),QString("500"));QVERIFY(!failed.first().value(2).toString().isEmpty());
        QCOMPARE(client.messages()->rows(),before);QCOMPARE(ready.count(),0);QVERIFY(!client.loadingMessages());
    }
    void gatewayEditsAndDeletesDuringTheJumpWinOverTheOlderRestSnapshot() {
        ReplyHttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        fixture.responses.enqueue({200,page({message("499","Snapshot sibling"),message("500","Snapshot target"),message("501")}),150});
        DiscordClient client;prepare(client,fixture);QSignalSpy ready(&client,&DiscordClient::messageJumpReady),failed(&client,&DiscordClient::messageJumpFailed);
        QVERIFY(client.jumpToMessage("101","500"));QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(),1,1000);
        client.dispatch("MESSAGE_UPDATE",{{"id","500"},{"channel_id","101"},{"content","New gateway edit"}});
        client.dispatch("MESSAGE_DELETE",{{"id","499"},{"channel_id","101"}});
        client.dispatch("MESSAGE_CREATE",message("9002","New live message"));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,2000);QCOMPARE(failed.count(),0);
        QCOMPARE(client.messages()->byId("500").value("content").toString(),QString("New gateway edit"));QVERIFY(client.messages()->byId("499").isEmpty());
        auto live=client.m_messageObjects.value("9002");if(live.isEmpty())live=client.m_deferredLiveMessages.value("9002");
        QCOMPARE(live.value("content").toString(),QString("New live message"));
    }
    void aTargetDeletedWhileAroundIsLoadingCannotBeResurrectedOrReplaceTheWindow() {
        ReplyHttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        fixture.responses.enqueue({200,page({message("500"),message("501")}),130});
        DiscordClient client;prepare(client,fixture);QSignalSpy ready(&client,&DiscordClient::messageJumpReady),failed(&client,&DiscordClient::messageJumpFailed);
        const auto before=client.messages()->rows();QVERIFY(client.jumpToMessage("101","500"));QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(),1,1000);
        client.dispatch("MESSAGE_DELETE",{{"id","500"},{"channel_id","101"}});
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(),1,2000);QCOMPARE(ready.count(),0);QCOMPARE(client.messages()->rows(),before);QVERIFY(!client.loadingMessages());
    }
    void newerJumpsSupersedeBothPendingFetchesAndQueuedLoadedTargets() {
        ReplyHttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        fixture.responses.enqueue({200,page({message("500")}),180});fixture.responses.enqueue({200,page({message("600")}),0});
        DiscordClient client;prepare(client,fixture);QSignalSpy ready(&client,&DiscordClient::messageJumpReady),failed(&client,&DiscordClient::messageJumpFailed);
        QVERIFY(client.jumpToMessage("101","500"));QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(),1,1000);
        QVERIFY(client.jumpToMessage("101","600"));QTRY_COMPARE_WITH_TIMEOUT(ready.count(),1,2000);
        QCOMPARE(ready.first().value(1).toString(),QString("600"));QTest::qWait(220);
        QCOMPARE(ready.count(),1);QCOMPARE(failed.count(),0);QVERIFY(!client.messages()->byId("600").isEmpty());QVERIFY(client.messages()->byId("500").isEmpty());
        fixture.responses.enqueue({200,page({message("700")}),120});
        QVERIFY(client.jumpToMessage("101","600"));QVERIFY(client.jumpToMessage("101","700"));
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(),2,2000);QCOMPARE(ready.last().value(1).toString(),QString("700"));QCOMPARE(failed.count(),0);
    }
    void changingChannelSuppressesStaleFetchAndLoadedTargetCallbacks() {
        ReplyHttpFixture fixture;QVERIFY(fixture.server.listen(QHostAddress::LocalHost,0));
        fixture.responses.enqueue({200,page({message("500")}),140});fixture.responses.enqueue({200,page({message("9900","New channel","102")}),0});
        DiscordClient client;prepare(client,fixture);QSignalSpy ready(&client,&DiscordClient::messageJumpReady),failed(&client,&DiscordClient::messageJumpFailed);
        QVERIFY(client.jumpToMessage("101","500"));QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(),1,1000);
        client.selectChannel("102");QTRY_VERIFY_WITH_TIMEOUT(!client.messages()->byId("9900").isEmpty(),2000);QTest::qWait(180);
        QCOMPARE(client.currentChannelId(),QString("102"));QCOMPARE(ready.count(),0);QCOMPARE(failed.count(),0);QVERIFY(client.messages()->byId("500").isEmpty());
        QVERIFY(client.jumpToMessage("102","9900"));
        fixture.responses.enqueue({200,page({message("9100","Back to first")}),0});client.selectChannel("101");
        QTRY_VERIFY_WITH_TIMEOUT(!client.messages()->byId("9100").isEmpty(),2000);QCOMPARE(ready.count(),0);QCOMPARE(failed.count(),0);
    }
    void referenceRolesDistinguishUnfetchedFromKnownDeletedAndRetainTheChannel() {
        DiscordClient client;client.m_currentChannelId="101";
        auto reply=message("9000","Reply");reply.insert("type",19);
        reply.insert("message_reference",QJsonObject{{"message_id","500"},{"channel_id","101"}});
        auto row=client.messageRow(reply);QCOMPARE(row.value("replyId").toString(),QString("500"));QCOMPARE(row.value("replyChannelId").toString(),QString("101"));
        QVERIFY(row.value("replyMissing").toBool());QVERIFY(!row.value("replyDeleted").toBool());
        reply.insert("referenced_message",QJsonValue::Null);row=client.messageRow(reply);QVERIFY(row.value("replyDeleted").toBool());
        reply.remove("referenced_message");client.m_deletedMessages.insert("500");QVERIFY(client.messageRow(reply).value("replyDeleted").toBool());
        reply["message_reference"]=QJsonObject{{"message_id","600"},{"channel_id","102"}};row=client.messageRow(reply);
        QCOMPARE(row.value("replyChannelId").toString(),QString("102"));QVERIFY(!row.value("replyDeleted").toBool());
        RecordModel model;model.upsert({{"recordId","1"}});QVERIFY(model.byId("1").contains("replyChannelId"));
        QCOMPARE(model.byId("1").value("replyDeleted").metaType().id(),int(QMetaType::Bool));QVERIFY(!model.byId("1").value("replyDeleted").toBool());
    }
    void aLiveDeleteMarksExistingRepliesAsDeletedWithoutResettingTheFeed() {
        DiscordClient client;client.m_currentChannelId="101";
        const auto target=message("500","Original target");client.acceptMessage(target);
        auto reply=message("9000","Reply");reply.insert("type",19);
        reply.insert("message_reference",QJsonObject{{"message_id","500"},{"channel_id","101"}});reply.insert("referenced_message",target);
        client.acceptMessage(reply);QVERIFY(!client.messages()->byId("9000").value("replyDeleted").toBool());
        QSignalSpy reset(client.messages(),&QAbstractItemModel::modelReset);
        client.dispatch("MESSAGE_DELETE",{{"id","500"},{"channel_id","101"}});
        QVERIFY(client.messages()->byId("500").isEmpty());QVERIFY(client.messages()->byId("9000").value("replyDeleted").toBool());
        QCOMPARE(reset.count(),0);
    }
};

QTEST_GUILESS_MAIN(ReplyJumpTest)
#include "reply_jump_test.moc"
