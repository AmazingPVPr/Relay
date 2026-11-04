#include "GuildManagement.h"
#include "DiscordClient.h"
#include <QImage>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtTest>
#include <functional>

namespace {
constexpr quint64 View=1ULL<<10, Connect=1ULL<<20, ManageChannels=1ULL<<4, ManageRoles=1ULL<<28;
constexpr quint64 Manager=View|Connect|ManageChannels|ManageRoles|(1ULL<<1)|(1ULL<<2)|(1ULL<<5)|(1ULL<<7)|(1ULL<<22)|(1ULL<<23)|(1ULL<<24)|(1ULL<<33)|(1ULL<<40)|(1ULL<<44)|1ULL;
QByteArray json(const QJsonObject &v){return QJsonDocument(v).toJson(QJsonDocument::Compact);}
QByteArray json(const QJsonArray &v){return QJsonDocument(v).toJson(QJsonDocument::Compact);}
QJsonObject person(const QString &id,QJsonArray roles={}) {return {{"user",QJsonObject{{"id",id},{"username","Fixture "+id}}},{"roles",roles}};}
QJsonObject role(QString id,int position,quint64 bits=0,bool managed=false){return {{"id",id},{"name","Role "+id},{"position",position},{"permissions",QString::number(bits)},{"managed",managed}};}
QJsonObject fixtureEvent(QString id="301",QString start="2026-01-01T12:00:00+00:00") {return {{"id",id},{"name","Fixture event"},{"entity_type",3},{"creator_id","1"},{"scheduled_start_time",start},{"scheduled_end_time","2036-01-02T12:00:00Z"},{"status",2},{"entity_metadata",QJsonObject{{"location","Fixture hall"}}}};}
class Http final: public QTcpServer {
public:
    struct Response {int status=200;QByteArray body="{}";int delay=0;};
    QList<QByteArray> requests;
    std::function<Response(const QByteArray&,const QUrl&,const QJsonObject&)> respond;
    Http(){connect(this,&QTcpServer::newConnection,this,[this]{while(auto *socket=nextPendingConnection()){
        connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        connect(socket,&QTcpSocket::readyRead,this,[this,socket]{
            auto bytes=socket->property("bytes").toByteArray()+socket->readAll();socket->setProperty("bytes",bytes);
            const auto end=bytes.indexOf("\r\n\r\n");if(end<0 || socket->property("handled").toBool())return;
            int length=0;for(const auto &line:bytes.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
            if(bytes.size()<end+4+length)return;
            socket->setProperty("handled",true);requests.append(bytes);
            const auto parts=bytes.left(bytes.indexOf("\r\n")).split(' ');
            const auto response=respond?respond(parts.value(0),QUrl::fromEncoded(parts.value(1)),QJsonDocument::fromJson(bytes.mid(end+4)).object()):Response{};
            QTimer::singleShot(response.delay,socket,[socket,response]{if(socket->state()!=QAbstractSocket::ConnectedState)return;
                socket->write("HTTP/1.1 "+QByteArray::number(response.status)+" Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+QByteArray::number(response.body.size())+"\r\n\r\n"+response.body);socket->disconnectFromHost();});
        });
    }});}
    QString base()const{return "http://127.0.0.1:"+QString::number(serverPort());}
};
}
class GuildManagementTest final:public QObject {
    Q_OBJECT
    static void configure(DiscordClient &client,GuildManagement &service,const Http *http=nullptr) {
        client.m_connected=true;client.m_currentUserId="1";client.m_currentGuildId="100";client.m_token="synthetic-local-fixture-token";
        if(http)client.m_apiBase=http->base();
        const auto self=person("1",{"110"});
        client.m_guildObjects.insert("100",{{"id","100"},{"name","Fixture server"},{"owner_id","9"},{"roles",QJsonArray{role("100",0,View|Connect),role("110",10,Manager),role("120",1),role("130",20),role("140",2,0,true)}},{"member",self}});
        client.m_guildObjects.insert("200",{{"id","200"},{"name","Other server"},{"owner_id","1"},{"roles",QJsonArray{role("200",0,View|Connect)}}});
        for(const auto &user:QList<QJsonObject>{self,person("2",{"120"}),person("3",{"130"}),person("9")})client.cacheMember("100",user);
        client.m_channelCache.insert("100",QJsonArray{QJsonObject{{"id","101"},{"name","general"},{"type",0}},QJsonObject{{"id","102"},{"name","Voice"},{"type",2}},QJsonObject{{"id","103"},{"name","Category"},{"type",4}}});
        client.m_channelCache.insert("200",QJsonArray{QJsonObject{{"id","201"},{"name","Other"},{"type",0}}});
        client.m_voiceStates["100"].insert("2",{{"user_id","2"},{"channel_id","102"},{"muted",true},{"deafened",true},{"server_muted",false},{"server_deafened",true}});
        for (const auto &id : {QString("100"),QString("200")}) {
            client.m_guilds.upsert(client.guildRow(client.m_guildObjects.value(id)));
            client.m_guildMembershipVersions.insert(id,++client.m_nextGuildMembershipVersion);
        }
        service.m_guildId="100";service.m_guild=client.m_guildObjects.value("100");
    }
    static bool prepare(GuildManagement &s,QString action,QString target,QVariantMap values,QJsonObject *result=nullptr,QString *route=nullptr,QByteArray *verb=nullptr) {
        QByteArray method;QString path;QJsonObject body;const bool ok=s.prepare(action,target,values,method,path,body);
        if(result)*result=body;
        if(route)*route=path;
        if(verb)*verb=method;
        return ok;
    }
private slots:
    void leaveRequiresCurrentMembershipAndRejectsOwnersBeforeAnyRequest() {
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));
        DiscordClient client;GuildManagement service(&client);configure(client,service,&http);
        QVERIFY(service.capabilities().value("leaveGuild").toBool());
        QByteArray method;QString path;QJsonObject body;
        QVERIFY(prepare(service,"leaveGuild","100",{},&body,&path,&method));
        QCOMPARE(method,QByteArray("DELETE"));QCOMPARE(path,QString("/users/@me/guilds/100"));QVERIFY(body.isEmpty());
        QVERIFY(!service.execute("leaveGuild","200"));QVERIFY(!service.execute("leaveGuild","100/other"));
        client.m_guildObjects["100"].insert("owner_id","1");
        QVERIFY(service.capabilities().value("owner").toBool());QVERIFY(!service.capabilities().value("leaveGuild").toBool());
        QVERIFY(!service.execute("leaveGuild","100"));QVERIFY(service.error().contains("ownership"));
        client.m_guildObjects["100"].insert("owner_id","");service.m_guild.insert("owner_id","");
        QVERIFY(!service.execute("leaveGuild","100"));
        client.m_guildObjects["100"].insert("owner_id","9");
        client.m_connected=false;QVERIFY(!service.execute("leaveGuild","100"));client.m_connected=true;
        client.m_demoMode=true;QVERIFY(!service.execute("leaveGuild","100"));client.m_demoMode=false;
        client.m_token.clear();QVERIFY(!service.execute("leaveGuild","100"));
        QTest::qWait(30);QVERIFY(http.requests.isEmpty());
    }
    void leaveWaitsFor204ThenUsesAuthoritativeCleanupOnce() {
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));
        http.respond=[](const QByteArray&,const QUrl&,const QJsonObject&){return Http::Response{204,{},90};};
        DiscordClient client;GuildManagement service(&client);configure(client,service,&http);
        client.m_currentChannelId="101";client.m_currentChannelName="Generated channel";client.m_currentChannelKind="text";
        client.m_messages.upsert({{"recordId","501"},{"content","Generated history"}});
        client.m_messageObjects.insert("501",{{"id","501"},{"channel_id","101"}});
        client.m_loadingMessages=true;client.m_hasMoreMessages=true;
        client.m_notificationSettings.insert("100",{{"muted",true}});client.m_channelInfoRequestedAt.insert("100",123);
        client.m_unread.insert("100",3);client.m_unread.insert("101",3);client.m_unread.insert("201",7);
        client.m_profiles.insert("2:100",{{"bio","Generated guild profile"}});client.m_profiles.insert("2:200",{{"bio","Other guild"}});
        client.m_voiceSnapshots.insert("100");client.m_voiceGuildId="100";
        client.m_streamObjects.insert("guild:100:102:2",{{"paused",false}});
        QSignalSpy removed(&client,&DiscordClient::guildRemoved),completed(&service,&GuildManagement::actionCompleted);
        QString activeVoiceGuild="100";int voiceLeaves=0;
        connect(&client,&DiscordClient::guildRemoved,&client,[&](const QString &guild){if(activeVoiceGuild==guild){activeVoiceGuild.clear();++voiceLeaves;}});
        QVERIFY(service.execute("leaveGuild","100"));QVERIFY(service.mutating());
        QVERIFY(client.m_guildObjects.contains("100"));QCOMPARE(client.currentChannelId(),QString("101"));QCOMPARE(client.messages()->count(),1);
        QVERIFY(!service.execute("leaveGuild","100"));
        QTRY_COMPARE_WITH_TIMEOUT(removed.count(),1,2000);QTRY_COMPARE_WITH_TIMEOUT(completed.count(),1,2000);
        QCOMPARE(http.requests.size(),1);QVERIFY(http.requests.first().startsWith("DELETE /users/@me/guilds/100 "));
        QCOMPARE(removed.first(),QVariantList({"100"}));QCOMPARE(voiceLeaves,1);QVERIFY(activeVoiceGuild.isEmpty());
        QVERIFY(!client.m_guildObjects.contains("100"));QVERIFY(client.m_guildObjects.contains("200"));
        QVERIFY(!client.m_channelCache.contains("100"));QVERIFY(client.m_channelCache.contains("200"));
        QVERIFY(!client.m_guildMembers.contains("100"));QVERIFY(!client.m_voiceStates.contains("100"));QVERIFY(!client.m_voiceSnapshots.contains("100"));
        QVERIFY(!client.m_notificationSettings.contains("100"));QVERIFY(!client.m_channelInfoRequestedAt.contains("100"));
        QVERIFY(!client.m_profiles.contains("2:100"));QVERIFY(client.m_profiles.contains("2:200"));
        QVERIFY(!client.m_unread.contains("100"));QVERIFY(!client.m_unread.contains("101"));QCOMPARE(client.m_unread.value("201"),7);
        QVERIFY(client.m_streamObjects.isEmpty());QVERIFY(client.currentGuildId().isEmpty());QVERIFY(client.currentChannelId().isEmpty());
        QCOMPARE(client.messages()->count(),0);QVERIFY(client.m_messageObjects.isEmpty());QVERIFY(!client.loadingMessages());QVERIFY(!client.hasMoreMessages());
        QVERIFY(service.guildId().isEmpty());QVERIFY(service.error().isEmpty());
        client.dispatch("GUILD_DELETE",{{"id","100"}});QCOMPARE(removed.count(),1);
        client.acceptGuild({{"id","100"},{"name","Stale list snapshot"}});QVERIFY(!client.m_guildObjects.contains("100"));
    }
    void leaveErrorsPreserveCurrentServerAndHistory_data() {
        QTest::addColumn<int>("status");
        QTest::newRow("forbidden")<<403;QTest::newRow("server-error")<<500;
        QTest::newRow("wrong-success-code")<<200;QTest::newRow("unconfirmed-accepted")<<202;
    }
    void leaveErrorsPreserveCurrentServerAndHistory() {
        QFETCH(int,status);Http http;QVERIFY(http.listen(QHostAddress::LocalHost));
        http.respond=[&](const QByteArray&,const QUrl&,const QJsonObject&){return Http::Response{status,json(QJsonObject{{"message","Generated error"}}),30};};
        DiscordClient client;GuildManagement service(&client);configure(client,service,&http);
        client.m_currentChannelId="101";client.m_messages.upsert({{"recordId","501"},{"content","Generated history"}});
        const auto guilds=client.guilds()->rows(),messages=client.messages()->rows();const auto channels=client.m_channelCache;
        QSignalSpy removed(&client,&DiscordClient::guildRemoved),completed(&service,&GuildManagement::actionCompleted);
        QVERIFY(service.execute("leaveGuild","100"));QTRY_VERIFY_WITH_TIMEOUT(!service.mutating(),2000);
        QVERIFY(!service.error().isEmpty());QCOMPARE(removed.count(),0);QCOMPARE(completed.count(),0);
        QCOMPARE(client.guilds()->rows(),guilds);QCOMPARE(client.messages()->rows(),messages);QCOMPARE(client.m_channelCache,channels);
        QCOMPARE(client.currentGuildId(),QString("100"));QCOMPARE(client.currentChannelId(),QString("101"));QVERIFY(client.m_guildMembers.contains("100"));
    }
    void leaveGatewayRaceAndTemporaryUnavailableAreIdempotent() {
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));DiscordClient client;GuildManagement service(&client);configure(client,service,&http);
        QSignalSpy removed(&client,&DiscordClient::guildRemoved),completed(&service,&GuildManagement::actionCompleted);
        client.dispatch("GUILD_DELETE",{{"id","100"},{"unavailable",true}});QCOMPARE(removed.count(),0);QVERIFY(client.m_guildObjects.contains("100"));
        http.respond=[&](const QByteArray&,const QUrl&,const QJsonObject&){client.dispatch("GUILD_DELETE",{{"id","100"}});return Http::Response{204,{},70};};
        QVERIFY(service.execute("leaveGuild","100"));QTRY_COMPARE_WITH_TIMEOUT(completed.count(),1,2000);
        QCOMPARE(removed.count(),1);client.dispatch("GUILD_DELETE",{{"id","100"}});QCOMPARE(removed.count(),1);QVERIFY(service.error().isEmpty());
    }
    void leaveSelectionChangesDoNotAffectAnotherServerOrCall() {
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));
        http.respond=[](const QByteArray &verb,const QUrl&,const QJsonObject&){return verb=="DELETE"?Http::Response{204,{},120}:Http::Response{200,json(QJsonObject{{"id","200"},{"owner_id","1"},{"name","Other server"}})};};
        DiscordClient client;GuildManagement service(&client);configure(client,service,&http);
        QSignalSpy removed(&client,&DiscordClient::guildRemoved);QString voiceGuild="200";int voiceLeaves=0;
        connect(&client,&DiscordClient::guildRemoved,&client,[&](const QString &id){if(id==voiceGuild){++voiceLeaves;voiceGuild.clear();}});
        QVERIFY(service.execute("leaveGuild","100"));service.selectGuild("200");client.m_currentGuildId="200";client.m_currentChannelId="201";
        client.m_messages.upsert({{"recordId","601"},{"content","Other generated conversation"}});
        QTRY_COMPARE_WITH_TIMEOUT(removed.count(),1,2000);QCOMPARE(voiceLeaves,0);QCOMPARE(voiceGuild,QString("200"));
        QCOMPARE(client.currentGuildId(),QString("200"));QCOMPARE(client.currentChannelId(),QString("201"));QCOMPARE(client.messages()->count(),1);
        QCOMPARE(service.guildId(),QString("200"));QVERIFY(client.m_guildObjects.contains("200"));QVERIFY(!client.m_guildObjects.contains("100"));
    }
    void leaveOldAccountAndRejoinedMembershipRejectStaleResponses() {
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));
        http.respond=[](const QByteArray&,const QUrl&,const QJsonObject&){return Http::Response{204,{},120};};
        DiscordClient client;GuildManagement service(&client);configure(client,service,&http);QSignalSpy removed(&client,&DiscordClient::guildRemoved);
        QVERIFY(service.execute("leaveGuild","100"));QTRY_COMPARE_WITH_TIMEOUT(http.requests.size(),1,1000);
        client.logout();configure(client,service,&http);client.m_currentUserId="7";
        QTest::qWait(180);QVERIFY(client.m_guildObjects.contains("100"));QCOMPARE(removed.count(),0);QVERIFY(!service.mutating());
        client.m_currentUserId="1";
        QSignalSpy completed(&service,&GuildManagement::actionCompleted);
        http.respond=[&](const QByteArray &verb,const QUrl&,const QJsonObject&){
            if(verb=="DELETE"){
                const auto fresh=client.m_guildObjects.value("100");client.dispatch("GUILD_DELETE",{{"id","100"}});
                client.dispatch("GUILD_CREATE",fresh);return Http::Response{204,{},90};
            }
            return Http::Response{};
        };
        QVERIFY(service.execute("leaveGuild","100"));QTRY_COMPARE_WITH_TIMEOUT(removed.count(),1,1000);QTest::qWait(180);
        QVERIFY(client.m_guildObjects.contains("100"));QCOMPARE(completed.count(),0);QVERIFY(!service.mutating());
    }
    void permissionsUseLatestSelfRolesAndHierarchy() {
        DiscordClient client;GuildManagement s(&client);configure(client,s);
        QVERIFY(s.capabilities().value("manageChannels").toBool());
        QVERIFY(s.userPermissions("100","2").value("kick").toBool());
        QVERIFY(!s.userPermissions("100","3").value("kick").toBool());
        QVERIFY(!s.userPermissions("100","9").value("ban").toBool());
        QVERIFY(!s.userPermissions("100","1").value("timeout").toBool());
        QVERIFY(!s.userPermissions("100","2").value("serverMuted").toBool());
        QVERIFY(s.userPermissions("100","2").value("serverDeafened").toBool());
        client.cacheMember("100",person("1",{}));
        QVERIFY(!s.capabilities().value("manageChannels").toBool());
        QVERIFY(!prepare(s,"kickMember","2",{}));
        QVERIFY(!s.snapshot("100").value("member").toObject().value("roles").toArray().isEmpty());
    }
    void permissionsHonorOverwritesAndUnknownChannelsEvenForOwner() {
        DiscordClient client;GuildManagement s(&client);configure(client,s);
        auto channels=client.m_channelCache.value("100");auto channel=channels[0].toObject();
        channel.insert("permission_overwrites",QJsonArray{QJsonObject{{"id","100"},{"type",0},{"deny",QString::number(View)},{"allow","0"}},QJsonObject{{"id","110"},{"type",0},{"deny","0"},{"allow",QString::number(View)}}});channels[0]=channel;client.m_channelCache["100"]=channels;
        QVERIFY(s.channelPermissions("100","101").value("view").toBool());
        client.cacheMember("100",person("1",{}));QVERIFY(!s.channelPermissions("100","101").value("view").toBool());
        client.m_guildObjects["100"].insert("owner_id","1");
        QVERIFY(s.capabilities().value("owner").toBool());
        QVERIFY(!s.channelPermissions("100","201").value("manage").toBool());
        QVERIFY(!prepare(s,"createInvite","201",{}));
        QVERIFY(!prepare(s,"editGuild","200",{{"name","Wrong server"}}));
    }
    void privateChannelEditsPreserveUnrelatedOverwritesAndNeedManageRoles() {
        DiscordClient client;GuildManagement s(&client);configure(client,s);
        auto channels=client.m_channelCache.value("100");auto channel=channels[1].toObject();
        const QJsonObject other{{"id","555"},{"type",1},{"allow","2048"},{"deny","8192"}};
        channel.insert("permission_overwrites",QJsonArray{QJsonObject{{"id","100"},{"type",0},{"allow","2048"},{"deny",QString::number(8192|View|Connect)}},other});channels[1]=channel;client.m_channelCache["100"]=channels;
        client.m_guildObjects["100"].insert("owner_id","1");
        QJsonObject body;QVERIFY(prepare(s,"editChannel","102",{{"private",false}},&body));
        const auto overwrites=body.value("permission_overwrites").toArray();QCOMPARE(overwrites[1].toObject(),other);
        QCOMPARE(overwrites[0].toObject().value("deny").toString(),QString("8192"));
        QCOMPARE(overwrites[0].toObject().value("allow").toString().toULongLong(),quint64(2048|View|Connect));
        client.m_guildObjects["100"].insert("owner_id","9");client.m_channelCache["100"]=QJsonArray{QJsonObject{{"id","101"},{"type",0}}};
        auto roles=client.m_guildObjects["100"].value("roles").toArray();roles[1]=role("110",10,ManageChannels|View);client.m_guildObjects["100"].insert("roles",roles);
        QVERIFY(!prepare(s,"editChannel","101",{{"private",true}}));
        QVERIFY(prepare(s,"editChannel","101",{{"name","Renamed"}}));
    }
    void roleGrantBoundsAndSelfAssignment() {
        DiscordClient client;GuildManagement s(&client);configure(client,s);QJsonObject body;
        QVERIFY(!prepare(s,"deleteRole","100",{}));QVERIFY(!prepare(s,"deleteRole","140",{}));
        QVERIFY(!prepare(s,"editRole","130",{{"name","Higher"}}));
        QVERIFY(!prepare(s,"createRole","",{{"name","Admin"},{"permissions","8"}}));
        QVERIFY(prepare(s,"assignRole","1",{{"roleId","120"}}));
        QVERIFY(!prepare(s,"assignRole","2",{{"roleId","140"}}));
        QVERIFY(prepare(s,"createRole","",{{"name","Helper"},{"permissionKeys",QStringList{"viewChannel","connect"}}},&body));
        QCOMPARE(body.value("permissions").toString(),QString::number(View|Connect));
        QVERIFY(!prepare(s,"createChannel","",{{"name","Bad"},{"type",2.9}}));
        QVERIFY(!prepare(s,"editChannel","102",{{"bitrate",96000.5}}));
    }
    void moderationValidatesTimeoutAndVoiceTarget() {
        DiscordClient client;GuildManagement s(&client);configure(client,s);QJsonObject body;QString path;QByteArray method;
        QVERIFY(prepare(s,"serverMute","2",{{"enabled",true}},&body,&path,&method));
        QCOMPARE(method,QByteArray("PATCH"));QCOMPARE(path,QString("/guilds/100/members/2"));QCOMPARE(body,QJsonObject({{"mute",true}}));
        QVERIFY(prepare(s,"moveMember","2",{{"channelId",""}},&body));QVERIFY(body.value("channel_id").isNull());
        QVERIFY(!prepare(s,"moveMember","2",{{"channelId","101"}}));
        QVERIFY(!prepare(s,"timeoutMember","2",{{"until",QDateTime::currentDateTimeUtc().addDays(29).toString(Qt::ISODate)}}));
        QVERIFY(prepare(s,"timeoutMember","2",{{"until",QDateTime::currentDateTimeUtc().addSecs(300).toString(Qt::ISODate)}},&body));
        QVERIFY(!prepare(s,"banMember","2",{{"deleteMessageSeconds",604801}}));
        QVERIFY(!prepare(s,"kickMember","3",{}));
    }
    void activeEventAcceptsEquivalentTimestampAndPermissionsAreContextual() {
        DiscordClient client;GuildManagement s(&client);configure(client,s);s.m_events.insert("301",fixtureEvent());QJsonObject body;QString path;QByteArray method;
        QVERIFY(prepare(s,"editEvent","301",{{"name","Edited"},{"scheduledStartTime","2026-01-01T12:00:00.000Z"}},&body));
        QVERIFY(!body.contains("scheduled_start_time"));
        QVERIFY(s.itemRow("events",fixtureEvent()).value("canEdit").toBool());
        QVERIFY(prepare(s,"rsvpEvent","301",{{"enabled",true}},&body,&path,&method));QCOMPARE(method,QByteArray("PUT"));QCOMPARE(path,QString("/guilds/100/scheduled-events/301/users/@me"));
        QVERIFY(!prepare(s,"createEvent","",{{"name","Future"},{"entityType",3},{"scheduledStartTime",QDateTime::currentDateTimeUtc().addDays(1).toString(Qt::ISODate)}}));
        auto restricted=fixtureEvent("302");restricted.insert("entity_type",2);restricted.insert("channel_id","999");s.m_events.insert("302",restricted);
        QVERIFY(!prepare(s,"editEvent","302",{{"entityType",3},{"location","Elsewhere"}}));
    }
    void imageUploadUsesOnlyExplicitLocalFiles() {
        DiscordClient client;GuildManagement s(&client);configure(client,s);QTemporaryDir directory;QVERIFY(directory.isValid());
        QImage image(16,16,QImage::Format_ARGB32);image.fill(Qt::blue);const auto file=directory.filePath("icon.png");QVERIFY(image.save(file));
        QString encoded;QVERIFY(s.imageData(QUrl::fromLocalFile(file).toString(),encoded));QVERIFY(encoded.startsWith("data:image/png;base64,"));
        QVERIFY(!s.imageData("https://example.com/image.png",encoded));QVERIFY(!s.imageData("../image.png",encoded));
        QJsonObject body;QVERIFY(prepare(s,"editGuild","100",{{"iconPath",QUrl::fromLocalFile(file).toString()}},&body));QVERIFY(body.value("icon").toString().startsWith("data:image/png;base64,"));
    }
    void readsAreBoundedPaginatedAndReplaceRemovedEvents() {
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));int events=0;
        http.respond=[&](const QByteArray&,const QUrl &url,const QJsonObject&){
            if(url.path().endsWith("/members")) {QJsonArray rows;const bool more=QUrlQuery(url).hasQueryItem("after");for(int n=0;n<(more?1:100);++n)rows.append(person(QString::number(1000+(more?100:n))));return Http::Response{200,json(rows)};}
            if(url.path().endsWith("/scheduled-events")){++events;return Http::Response{200,json(events==1?QJsonArray{fixtureEvent("301"),fixtureEvent("302")}:QJsonArray{fixtureEvent("301")})};}
            return Http::Response{204,{}};
        };
        DiscordClient client;GuildManagement s(&client);configure(client,s,&http);
        s.loadSection("members");QTRY_VERIFY_WITH_TIMEOUT(!s.busy(),2000);QCOMPARE(s.items().size(),100);QVERIFY(s.hasMore());s.loadMore();QTRY_VERIFY_WITH_TIMEOUT(!s.busy(),2000);QCOMPARE(s.items().size(),101);QVERIFY(!s.hasMore());QVERIFY(http.requests.last().contains("after=1099"));
        s.loadSection("events");QTRY_VERIFY_WITH_TIMEOUT(!s.busy(),2000);QCOMPARE(s.items().size(),2);s.m_events["301"].insert("current_user_interested",true);
        s.refresh();QTRY_VERIFY_WITH_TIMEOUT(!s.busy(),2000);QCOMPARE(s.m_events.size(),1);QVERIFY(s.m_events.value("301").value("current_user_interested").toBool());
        QVERIFY(s.execute("rsvpEvent","301",{{"enabled",false}}));QTRY_VERIFY_WITH_TIMEOUT(!s.mutating(),2000);QCOMPARE(s.items().size(),1);
    }
    void createRoleUpsertsGatewayRaceAndAuditReasonIsEncoded() {
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));DiscordClient client;GuildManagement s(&client);configure(client,s,&http);
        http.respond=[&](const QByteArray &verb,const QUrl&,const QJsonObject &body){
            if(verb=="POST"){auto created=body;created.insert("id","150");created.insert("position",1);client.dispatch("GUILD_ROLE_CREATE",{{"guild_id","100"},{"role",created}});return Http::Response{200,json(created),30};}
            return Http::Response{};
        };
        QSignalSpy completed(&s,&GuildManagement::actionCompleted);
        QVERIFY(s.execute("createRole","",{{"name","New role"},{"reason","Fixture / reason\nsecond"}}));QTRY_COMPARE_WITH_TIMEOUT(completed.count(),1,2000);
        int matches=0;for(const auto &row:s.roles())if(row.toMap().value("id").toString()=="150")++matches;QCOMPARE(matches,1);
        const auto request=http.requests.first();QVERIFY(request.contains("X-Audit-Log-Reason: Fixture%20%2F%20reason%0Asecond\r\n"));
        QVERIFY(request.startsWith("POST /guilds/100/roles "));QVERIFY(request.contains("synthetic-local-fixture-token"));
    }
    void staleReadsAndGuildWritesCannotReplaceCurrentState() {
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));int memberReads=0;
        http.respond=[&](const QByteArray &verb,const QUrl &url,const QJsonObject &body){
            if(verb=="PATCH")return Http::Response{200,json(QJsonObject{{"id","100"},{"name",body.value("name")}}),100};
            if(url.path().endsWith("/members")){++memberReads;return Http::Response{200,json(QJsonArray{person("2")}),180};}
            return Http::Response{200,json(QJsonObject{{"id",url.path().section('/',2,2)},{"name","Remote server"}}),50};
        };
        DiscordClient client;GuildManagement s(&client);configure(client,s,&http);
        s.fetchOverview();s.loadSection("members");QTest::qWait(90);QVERIFY(s.busy());QTRY_VERIFY_WITH_TIMEOUT(!s.busy(),2000);QCOMPARE(s.items().size(),1);
        QVERIFY(s.execute("editGuild","100",{{"name","Updated"}}));s.loadSection("members");QCOMPARE(memberReads,1);QVERIFY(!s.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!s.mutating() && !s.busy(),2000);QCOMPARE(memberReads,2);
        QVERIFY(s.execute("editGuild","100",{{"name","Old server response"}}));s.selectGuild("200");QTRY_VERIFY_WITH_TIMEOUT(!s.busy(),2000);QTest::qWait(130);QCOMPARE(s.guildId(),QString("200"));QCOMPARE(s.guild().value("name").toString(),QString("Remote server"));
        QVERIFY(s.execute("editGuild","200",{{"name","Before logout"}}));client.logout();QTest::qWait(130);QVERIFY(s.guildId().isEmpty());QVERIFY(!s.mutating());
    }
    void auditRowsHaveBoundedReadableDetailsAndBoostMetadata() {
        DiscordClient client;GuildManagement s(&client);configure(client,s);
        QJsonArray changes;for(int i=0;i<30;++i)changes.append(QJsonObject{{"key",i?"name":"token"},{"old_value",QString(900,'x')},{"new_value","Updated"}});
        const auto row=s.itemRow("audit",{{"id","123456789"},{"action_type",31},{"user_id","1"},{"target_id","120"},{"changes",changes}});
        QCOMPARE(row.value("actionName").toString(),QString("Role updated"));QVERIFY(!row.value("actorName").toString().isEmpty());QCOMPARE(row.value("targetName").toString(),QString("Role 120"));QCOMPARE(row.value("changes").toList().size(),25);QCOMPARE(row.value("changes").toList().first().toMap().value("before").toString(),QString("Hidden"));
        s.acceptGuild({{"id","100"},{"premium_tier",3},{"premium_subscription_count",14},{"features",QJsonArray{"ANIMATED_ICON"}}});QCOMPARE(s.guild().value("maxBitrate").toInt(),384000);QCOMPARE(s.guild().value("premiumSubscriptionCount").toInt(),14);QCOMPARE(s.channels().size(),3);
    }
};
QTEST_GUILESS_MAIN(GuildManagementTest)
#include "guild_management_test.moc"
