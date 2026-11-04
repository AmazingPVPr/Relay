#include <QtTest>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QSignalSpy>
#include "DiscordClient.h"
#include "RelayPeers.h"
#include "RelayPeerTransport.h"
#include "RelayDiscordBridge.h"

// Generated Discord-shaped REST/Gateway service. No Discord access or real token.
class SetupHttp final : public QObject {
public:
    QTcpServer server;
    QList<QJsonObject> sent;
    std::function<void(QJsonObject)> deliver;
    int responseStatus=200;
    quint64 serial=400000000000000000ULL;
    SetupHttp() {
        server.listen(QHostAddress::LocalHost,0);
        connect(&server,&QTcpServer::newConnection,this,[this]{
            while(auto *socket=server.nextPendingConnection()) {
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,socket,[this,socket]{
                    auto bytes=socket->property("buffer").toByteArray()+socket->readAll();socket->setProperty("buffer",bytes);
                    const auto split=bytes.indexOf("\r\n\r\n");if(split<0 || socket->property("handled").toBool())return;
                    int size=0;for(const auto &line:bytes.left(split).split('\n'))if(line.toLower().startsWith("content-length:"))size=line.mid(15).trimmed().toInt();
                    if(bytes.size()<split+4+size)return;socket->setProperty("handled",true);
                    QByteArray body="[]";
                    if(bytes.startsWith("POST ")) {
                        auto data=QJsonDocument::fromJson(bytes.mid(split+4,size)).object();sent.append(data);
                        data.insert("id",QString::number(++serial));data.insert("channel_id","333333333333333333");
                        data.insert("timestamp",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));data.insert("type",0);
                        body=QJsonDocument(data).toJson(QJsonDocument::Compact);
                        if(responseStatus==200 && deliver)QTimer::singleShot(0,this,[this,data]{deliver(data);});
                    }
                    socket->write("HTTP/1.1 "+QByteArray::number(responseStatus)+" Fixture\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();
                });
            }
        });
    }
    QString base() const {return "http://127.0.0.1:"+QString::number(server.serverPort())+"/api/v10";}
};
class RelayDiscordBridgeTest : public QObject {
    Q_OBJECT
    const QString aid="111111111111111111",bid="222222222222222222",channel="333333333333333333";
    void prepare(DiscordClient &client,SetupHttp &http,const QString &own,const QString &other) {
        client.m_apiBase=http.base();client.m_token="generated.fixture.token.only";client.m_connected=true;
        client.m_currentUserId=own;client.m_currentUserName="Generated "+own.left(3);
        client.acceptDm({{"id",channel},{"type",1},{"recipients",QJsonArray{QJsonObject{{"id",other},{"username","Generated friend"}}}}});
    }
    QJsonObject invitation(const RelayPeers &sender,quint64 id=500000000000000001ULL) {
        return {{"id",QString::number(id)},{"channel_id",channel},{"type",0},{"timestamp",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"author",QJsonObject{{"id",bid},{"username","Generated friend"}}},
            {"content",RelayDiscordBridge::encode({{"v",1},{"kind","invite"},{"from",bid},{"to",aid},{"key",sender.identity()},
            {"offer","11111111-2222-4333-8444-555555555555"},{"replyTo",""}})}};
    }
private slots:
    void twoCheckboxesEstablishRealDirectTextWithoutSetupServer() {
        QTemporaryDir root;SetupHttp ah,bh;DiscordClient ac,bc;
        prepare(ac,ah,aid,bid);prepare(bc,bh,bid,aid);
        RelayPeers a(root.path()+"/a",{}),b(root.path()+"/b",{});
        RelayDiscordBridge ab(&ac,&a),bb(&bc,&b);
        ah.deliver=[&](QJsonObject message){message.insert("author",QJsonObject{{"id",aid}});bc.dispatch("MESSAGE_CREATE",message);};
        bh.deliver=[&](QJsonObject message){message.insert("author",QJsonObject{{"id",bid}});ac.dispatch("MESSAGE_CREATE",message);};
        QSignalSpy invitationNotice(&bb,&RelayDiscordBridge::invitationReceived);
        ab.setConversationEnabled(aid+":"+bid,channel,true);
        QTRY_COMPARE_WITH_TIMEOUT(invitationNotice.size(),1,5000);
        QCOMPARE(bb.invitations().size(),1);QVERIFY(a.m_contacts.isEmpty());QVERIFY(b.m_contacts.isEmpty());
        QVERIFY(a.m_transports.isEmpty());QVERIFY(b.m_transports.isEmpty());
        QCOMPARE(a.m_signal.state(),QAbstractSocket::UnconnectedState);QVERIFY(!a.m_ready);
        bb.setConversationEnabled(bid+":"+aid,channel,true);
        QTRY_VERIFY_WITH_TIMEOUT(a.m_transports.contains(b.identity()) && a.m_transports.value(b.identity())->connected(),20000);
        QTRY_VERIFY_WITH_TIMEOUT(b.m_transports.contains(a.identity()) && b.m_transports.value(a.identity())->connected(),10000);
        QCOMPARE(a.m_signal.state(),QAbstractSocket::UnconnectedState);QCOMPARE(b.m_signal.state(),QAbstractSocket::UnconnectedState);
        const QString privateText="PRIVATE_DIRECT_ONLY_MESSAGE_hello_world";
        QVERIFY(a.sendText(b.identity(),privateText));
        QTRY_COMPARE_WITH_TIMEOUT(b.messagesForPeer(a.identity()).size(),1,5000);
        QTRY_VERIFY_WITH_TIMEOUT(!a.messagesForPeer(b.identity()).first().toMap().value("pending").toBool(),5000);
        QCOMPARE(b.messagesForPeer(a.identity()).first().toMap().value("text").toString(),privateText);
        for(const auto &request:ah.sent+bh.sent) {
            const auto text=request.value("content").toString();QVERIFY(text.size()<=2000);QVERIFY(text.contains("Please don't reply"));
            QVERIFY(!text.contains(privateText));QVERIFY(!text.contains("candidate:"));QVERIFY(!text.contains("a=fingerprint"));
            QVERIFY(!RelayDiscordBridge::setupSummary(text).isEmpty());
            QVERIFY(request.value("allowed_mentions").toObject().value("parse").toArray().isEmpty());
        }
        a.setAiAccess(b.identity(),true);b.setAiAccess(a.identity(),true);
        bb.setConversationEnabled(bid+":"+aid,channel,false);
        QTRY_VERIFY_WITH_TIMEOUT(!a.m_contacts.value(b.identity()).active,5000);
        QVERIFY(!b.m_contacts.value(a.identity()).active);QVERIFY(!a.m_contacts.value(b.identity()).ai);QVERIFY(!b.m_contacts.value(a.identity()).ai);
        QVERIFY(!a.sendText(b.identity(),"blocked after disable"));QVERIFY(!b.sendText(a.identity(),"blocked after disable"));
        QCOMPARE(b.messagesForPeer(a.identity()).size(),1);
        a.setMcpEnabled(true);
        auto result=a.mcpRequest({{"jsonrpc","2.0"},{"id",1},{"method","tools/call"},{"params",QJsonObject{{"name","relay_list_conversations"},{"arguments",QJsonObject{}}}}});
        QVERIFY(result.value("result").toObject().value("content").toArray().first().toObject().value("text").toString()=="[]");
    }
    void rejectsWrongContextAndPinsChangedIdentities() {
        QTemporaryDir root;SetupHttp http;DiscordClient client;prepare(client,http,aid,bid);
        RelayPeers local(root.path()+"/local",{}),remote(root.path()+"/remote",{}),changed(root.path()+"/changed",{});
        RelayDiscordBridge bridge(&client,&local);QSignalSpy notice(&bridge,&RelayDiscordBridge::invitationReceived);
        const auto original=invitation(remote);
        auto bad=original;bad.insert("guild_id","444");bridge.receive(bad);
        bad=original;bad.insert("author",QJsonObject{{"id",bid},{"bot",true}});bridge.receive(bad);
        bad=original;bad.insert("webhook_id","555");bridge.receive(bad);
        bad=original;bad.insert("author",QJsonObject{{"id","666"}});bridge.receive(bad);
        bad=original;bad.insert("timestamp","invalid");bridge.receive(bad);
        client.m_blockedUsers.insert(bid);bridge.receive(original);client.m_blockedUsers.remove(bid);
        QCOMPARE(notice.size(),0);QVERIFY(bridge.invitations().isEmpty());
        bridge.receive(original);bridge.receive(original);QCOMPARE(notice.size(),1);QCOMPARE(bridge.invitations().size(),1);
        bridge.setConversationEnabled(aid+":"+bid,channel,true);
        QCOMPARE(local.peerForBinding(aid+":"+bid),remote.identity());
        QVERIFY(local.sendText(remote.identity(),"Keep history with old identity"));local.setAiAccess(remote.identity(),true);
        bridge.receive(invitation(changed,500000000000000002ULL));
        QVERIFY(bridge.conversation(aid+":"+bid).value("identityChanged").toBool());
        QCOMPARE(local.peerForBinding(aid+":"+bid),remote.identity());QVERIFY(!local.m_contacts.value(remote.identity()).active);QVERIFY(!local.m_contacts.value(remote.identity()).ai);
        bridge.setConversationEnabled(aid+":"+bid,channel,true);QVERIFY(!local.m_contacts.contains(changed.identity()));
        bridge.resetIdentity(aid+":"+bid);QVERIFY(local.peerForBinding(aid+":"+bid).isEmpty());
        QCOMPARE(local.messagesForPeer(remote.identity()).size(),1);
        bridge.setConversationEnabled(aid+":"+bid,channel,true);QCOMPARE(local.peerForBinding(aid+":"+bid),changed.identity());
    }
    void preferenceIsEncryptedAndAccountSwitchRevokesAccess() {
        QTemporaryDir root;QString identity;
        {
            SetupHttp http;DiscordClient client;prepare(client,http,aid,bid);
            RelayPeers local(root.path()+"/local",{}),remote(root.path()+"/remote",{});
            identity=remote.identity();RelayDiscordBridge bridge(&client,&local);
            bridge.receive(invitation(remote));bridge.setConversationEnabled(aid+":"+bid,channel,true);
            local.setAiAccess(identity,true);local.setMcpEnabled(true);
            QFile saved(root.path()+"/local/contacts.relay");QVERIFY(saved.open(QIODevice::ReadOnly));const auto bytes=saved.readAll();
            QVERIFY(!bytes.contains("pairings"));QVERIFY(!bytes.contains(aid.toUtf8()));
            client.m_currentUserId="777777777777777777";++client.m_generation;emit client.stateChanged();
            QVERIFY(!local.enabled());QVERIFY(!local.mcpEnabled());QVERIFY(!local.m_contacts.value(identity).active);QVERIFY(!local.m_contacts.value(identity).ai);
            QVERIFY(bridge.invitations().isEmpty());QVERIFY(bridge.conversation(aid+":"+bid).isEmpty());
        }
        RelayPeers restored(root.path()+"/local",{});
        QVERIFY(restored.m_pairings.value(aid+":"+bid).toObject().value("enabled").toBool());
        QCOMPARE(restored.m_pairings.value(aid+":"+bid).toObject().value("peer").toString(),identity);
    }
    void pendingRestRepliesCannotOutliveTheBridge() {
        QTemporaryDir root;SetupHttp http;DiscordClient client;prepare(client,http,aid,bid);
        RelayPeers local(root.path()+"/local",{});
        auto *bridge=new RelayDiscordBridge(&client,&local);
        bridge->setConversationEnabled(aid+":"+bid,channel,true);
        delete bridge;
        QTest::qWait(100);
        QVERIFY(local.m_contacts.isEmpty());
    }
    void setupSendFailuresAreVisibleAndNoChatToolIsAdded() {
        QTemporaryDir root;SetupHttp http;http.responseStatus=403;DiscordClient client;prepare(client,http,aid,bid);
        RelayPeers local(root.path()+"/local",{});RelayDiscordBridge bridge(&client,&local);
        bridge.setConversationEnabled(aid+":"+bid,channel,true);
        QTRY_VERIFY_WITH_TIMEOUT(bridge.conversation(aid+":"+bid).value("status").toString().contains("could not send"),5000);
        QVERIFY(!bridge.m_sending);QVERIFY(bridge.m_outbound.isEmpty());
        QVERIFY(bridge.metaObject()->indexOfMethod("sendMessage(QString)")<0);
        local.setMcpEnabled(true);const auto tools=QJsonDocument(local.mcpRequest({{"jsonrpc","2.0"},{"id",1},{"method","tools/list"}})).toJson();
        QVERIFY(!tools.contains("discord_send"));QVERIFY(!tools.contains("relay_setup"));
    }
};
QTEST_GUILESS_MAIN(RelayDiscordBridgeTest)
#include "relay_discord_bridge_test.moc"
