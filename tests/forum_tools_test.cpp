#include "ForumTools.h"
#include "DiscordClient.h"
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QTimer>
#include <QSignalSpy>

namespace {
QByteArray json(const QJsonValue &v){return v.isArray()?QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact):QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);}
QJsonObject forumThread(QString id,QString parent="101",bool archived=false,QString stamp="2026-10-22T12:00:00.000Z"){
    return {{"id",id},{"guild_id","100"},{"parent_id",parent},{"type",11},{"name","Discussion "+id},{"owner_id","42"},
      {"last_message_id",id},{"message_count",3},{"applied_tags",QJsonArray{"201"}},
      {"thread_metadata",QJsonObject{{"archived",archived},{"locked",false},{"archive_timestamp",stamp}}}};
}
class Http final:public QTcpServer{
public:
    struct Response{int code=200;QJsonValue value=QJsonObject{};int delay=0;};
    QList<QByteArray> requests;std::function<Response(QByteArray,QUrl,QJsonObject)> respond;
    Http(){connect(this,&QTcpServer::newConnection,this,[this]{while(auto *socket=nextPendingConnection()){
        connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        connect(socket,&QTcpSocket::readyRead,this,[this,socket]{auto input=socket->property("input").toByteArray()+socket->readAll();socket->setProperty("input",input);
            const int end=input.indexOf("\r\n\r\n");if(end<0 || socket->property("handled").toBool())return;int length=0;
            for(const auto &line:input.left(end).split('\n'))if(line.toLower().startsWith("content-length:"))length=line.mid(15).trimmed().toInt();
            if(input.size()<end+4+length)return;socket->setProperty("handled",true);requests.append(input);
            const auto line=input.left(input.indexOf("\r\n")).split(' ');const auto response=respond?respond(line[0],QUrl::fromEncoded(line[1]),QJsonDocument::fromJson(input.mid(end+4)).object()):Response{};
            QTimer::singleShot(response.delay,socket,[socket,response]{if(socket->state()!=QAbstractSocket::ConnectedState)return;const auto body=json(response.value);
                socket->write("HTTP/1.1 "+QByteArray::number(response.code)+" Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);socket->disconnectFromHost();});
        });
    }});}
    QString base()const{return "http://127.0.0.1:"+QString::number(serverPort());}
};
}
class ForumToolsTest final:public QObject{
    Q_OBJECT
    static void configure(DiscordClient &client,Http &http){
        client.m_apiBase=http.base();client.m_token="local-fixture-token";client.m_currentUserId="42";client.m_connected=true;
        client.m_currentGuildId="100";client.m_currentGuildName="Fixture server";client.m_currentChannelId="101";client.m_currentChannelKind="forum";client.m_currentChannelName="Discussions";
        client.m_guilds.replace({{{"recordId","100"},{"name","Fixture server"}}});
        client.m_guildObjects["100"]={{"id","100"},{"owner_id","99"},{"member",QJsonObject{{"roles",QJsonArray{}}}},
          {"roles",QJsonArray{QJsonObject{{"id","100"},{"permissions",QString::number((1ULL<<10)|(1ULL<<11)|(1ULL<<16)|(1ULL<<38))}}}}};
        client.m_channelCache["100"]=QJsonArray{QJsonObject{{"id","101"},{"name","Discussions"},{"type",15},{"flags",16},
          {"available_tags",QJsonArray{QJsonObject{{"id","201"},{"name","Help"},{"moderated",false}},QJsonObject{{"id","202"},{"name","Official"},{"moderated",true}}}}},
          QJsonObject{{"id","102"},{"name","general"},{"type",0}}};
        client.rebuildChannels(false);
    }
    static Http::Response emptyResponse(const QUrl &url){
        if(url.path().endsWith("/threads/active"))return {200,QJsonObject{{"threads",QJsonArray{}}}};
        if(url.path().contains("/threads/archived/public"))return {200,QJsonObject{{"threads",QJsonArray{}},{"has_more",false}}};
        return {200,QJsonArray{}};
    }
private slots:
    void activeArchivedFilteringPagingAndNavigation(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));
        http.respond=[](QByteArray,const QUrl &url,QJsonObject){
            if(url.path().endsWith("/threads/active")){auto privateThread=forumThread("999");privateThread["type"]=12;auto foreign=forumThread("998");foreign["guild_id"]="200";
                return Http::Response{200,QJsonObject{{"threads",QJsonArray{forumThread("500"),privateThread,forumThread("997","102"),foreign}}}};}
            if(url.path().contains("/archived/public")){const bool next=QUrlQuery(url).hasQueryItem("before");return Http::Response{200,QJsonObject{
                {"threads",next?QJsonArray{forumThread("499","101",true),forumThread("498","101",true,"2026-10-21T12:00:00.000Z")}:QJsonArray{forumThread("499","101",true)}},{"has_more",!next}}};}
            return emptyResponse(url);
        };
        DiscordClient client;configure(client,http);ForumTools tools(&client);QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);
        QCOMPARE(tools.posts()->count(),2);QVERIFY(tools.hasMore());QVERIFY(tools.canPost());QVERIFY(tools.tagRequired());
        QCOMPARE(tools.posts()->get(0).value("recordId").toString(),QString("500"));
        tools.setFilterTag("202");QCOMPARE(tools.posts()->count(),0);tools.setFilterTag("");tools.setSearchText("499");QCOMPARE(tools.posts()->count(),1);
        tools.setSearchText("");tools.loadMore();QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);QCOMPARE(tools.posts()->count(),3);QVERIFY(!tools.hasMore());
        QCOMPARE(QUrlQuery(QUrl::fromEncoded(http.requests[2].split(' ')[1])).queryItemValue("before",QUrl::FullyDecoded),QString("2026-10-22T12:00:00.000Z"));
        QVERIFY(tools.openPost("499"));QCOMPARE(client.currentChannelKind(),QString("thread"));QCOMPARE(client.currentChannelId(),QString("499"));QVERIFY(tools.currentThread());QVERIFY(tools.canReply());
        QVERIFY(!tools.openPost("999"));tools.returnToForum();QCOMPARE(client.currentChannelId(),QString("101"));QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);
    }
    void postsStayNavigableWithoutSidebarRowsAtCacheBoundAndAfterRefresh(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));
        http.respond=[](QByteArray,const QUrl &url,QJsonObject){
            if(url.path().endsWith("/threads/active"))return Http::Response{200,QJsonObject{{"threads",QJsonArray{forumThread("700")}}}};
            return emptyResponse(url);
        };
        DiscordClient client;configure(client,http);
        auto channels=client.m_channelCache.value("100");
        for(int i=0;i<2100;++i)channels.append(forumThread(QString::number(10000+i)));
        client.cacheChannels("100",channels);client.rebuildChannels(false);
        ForumTools tools(&client);QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);
        QVERIFY(tools.openPost("700"));QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);
        QCOMPARE(client.currentChannelId(),QString("700"));QCOMPARE(client.channels()->count(),2);
        QVERIFY(client.channels()->byId("700").isEmpty());QVERIFY(tools.currentThread());QCOMPARE(client.m_subscribedChannel,QString("101"));
        client.m_unread["700"]=5;
        client.dispatch("THREAD_UPDATE",{{"id","700"},{"guild_id","100"},{"name","Renamed selected post"}});
        QCOMPARE(client.currentChannelName(),QString("Renamed selected post"));QCOMPARE(client.m_unread.value("700"),5);
        const QJsonArray structural{client.cachedChannel("100","101"),client.cachedChannel("100","102")};
        client.cacheChannels("100",structural,true);client.rebuildChannels(false);
        QCOMPARE(client.currentChannelId(),QString("700"));QVERIFY(tools.currentThread());QVERIFY(!client.cachedChannel("100","700").isEmpty());
        QCOMPARE(client.channels()->count(),2);QVERIFY(client.channels()->byId("700").isEmpty());
        QVERIFY(client.m_channelCache.value("100").size()<=client.channels()->maximumRows());
        QCOMPARE(client.m_pendingReadReceipts.size(),0);
        for(const auto &request:http.requests)QVERIFY(!request.split(' ').value(1).contains("/ack"));
        tools.returnToForum();QCOMPARE(client.currentChannelId(),QString("101"));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);QVERIFY(tools.openPost("700"));
        client.dispatch("THREAD_DELETE",{{"id","700"},{"guild_id","100"},{"parent_id","101"},{"type",11}});
        QVERIFY(client.currentChannelId()!=QString("700"));QVERIFY(client.cachedChannel("100","700").isEmpty());
        QCOMPARE(client.channels()->count(),2);
    }

    void selectedPostLosesAccessWhenItsParentChangesOrIsDeleted(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));
        http.respond=[](QByteArray,const QUrl &url,QJsonObject){
            if(url.path().endsWith("/threads/active"))return Http::Response{200,QJsonObject{{"threads",QJsonArray{forumThread("700")}}}};
            return emptyResponse(url);
        };
        DiscordClient client;configure(client,http);ForumTools tools(&client);QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);
        QVERIFY(tools.openPost("700"));QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);
        client.dispatch("CHANNEL_UPDATE",{{"id","101"},{"guild_id","100"},{"permission_overwrites",QJsonArray{
            QJsonObject{{"id","100"},{"type",0},{"allow","0"},{"deny",QString::number(1ULL<<10)}}}}});
        QVERIFY(client.currentChannelId()!=QString("700"));QVERIFY(!tools.currentThread());
        client.selectChannel("700");QVERIFY(client.currentChannelId()!=QString("700"));
        // Even a stale rendered viewport must not turn omitted-sidebar metadata
        // into permission to acknowledge a hidden post.
        client.m_currentChannelId="700";client.m_currentChannelKind="thread";client.m_readingActive=true;client.m_followingMessages=true;client.m_loadingMessages=false;
        client.acceptMessage({{"id","701"},{"channel_id","700"},{"author",QJsonObject{{"id","43"},{"username","Generated"}}},{"content","Fixture"}});
        QVERIFY(!client.readReceiptAllowed("700","701",false));QCOMPARE(client.m_pendingReadReceipts.size(),0);
        client.m_readingActive=false;
        client.dispatch("CHANNEL_UPDATE",{{"id","101"},{"guild_id","100"},{"permission_overwrites",QJsonArray{}}});
        QVERIFY(client.channelVisible(client.cachedChannel("100","700"),"100"));
        client.selectChannel("700");
        client.dispatch("CHANNEL_DELETE",{{"id","101"},{"guild_id","100"}});
        QVERIFY(client.currentChannelId()!=QString("700"));QVERIFY(client.cachedChannel("100","700").isEmpty());
        QCOMPARE(client.m_pendingReadReceipts.size(),0);
    }

    void createPostValidatesPermissionsTagsAndOpensReplyFeed(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));int creates=0;QJsonObject sent;
        http.respond=[&](QByteArray method,const QUrl &url,QJsonObject body){
            if(method=="POST" && url.path()=="/channels/101/threads"){++creates;sent=body;return Http::Response{200,forumThread("600")};}
            if(method=="POST" && url.path()=="/channels/600/messages")return Http::Response{200,QJsonObject{{"id","601"},{"channel_id","600"},{"content",body.value("content")},{"author",QJsonObject{{"id","42"},{"username","Fixture"}}}}};
            return emptyResponse(url);
        };
        DiscordClient client;configure(client,http);ForumTools tools(&client);QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);
        QVERIFY(!tools.createPost("Title","Message"));QVERIFY(!tools.createPost("Title","Message",{"202"}));QVERIFY(!tools.createPost("", "Message",{"201"}));
        QVERIFY(!tools.createPost("Title",QString(2001,'a'),{"201"}));QCOMPARE(creates,0);
        QSignalSpy created(&tools,&ForumTools::postCreated);QVERIFY(tools.createPost("A new discussion","Hello @everyone",{"201"}));QVERIFY(tools.posting());QVERIFY(!tools.createPost("Duplicate","Body",{"201"}));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.posting(),2000);QCOMPARE(creates,1);QCOMPARE(created.size(),1);QCOMPARE(sent.value("name").toString(),QString("A new discussion"));
        QCOMPARE(sent.value("applied_tags").toArray(),QJsonArray{"201"});QVERIFY(!sent.value("message").toObject().value("allowed_mentions").toObject().value("parse").toArray().contains("everyone"));
        QCOMPARE(client.currentChannelId(),QString("600"));QCOMPARE(client.currentChannelKind(),QString("thread"));QTRY_VERIFY_WITH_TIMEOUT(!client.loadingMessages(),2000);
        QVERIFY(client.sendMessage("A reply"));QTRY_VERIFY_WITH_TIMEOUT(!client.sendingMessage(),2000);
        bool reply=false;for(const auto &request:http.requests)if(request.startsWith("POST /channels/600/messages "))reply=true;QVERIFY(reply);
    }
    void deniedPermissionsAndStaleResponsesNeverPost(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));http.respond=[](QByteArray,const QUrl &url,QJsonObject){auto result=emptyResponse(url);result.delay=150;return result;};
        DiscordClient client;configure(client,http);
        auto channels=client.m_channelCache["100"];auto forum=channels[0].toObject();forum["permission_overwrites"]=QJsonArray{QJsonObject{{"id","42"},{"type",1},{"deny",QString::number(1ULL<<11)},{"allow","0"}}};channels[0]=forum;client.m_channelCache["100"]=channels;
        ForumTools tools(&client);QVERIFY(!tools.canPost());QVERIFY(!tools.createPost("Denied","No",{"201"}));QVERIFY(!tools.selectForum("102"));QVERIFY(!tools.selectForum("../"));
        client.logout();QVERIFY(!tools.loading());QVERIFY(tools.channelId().isEmpty());QTest::qWait(200);QCOMPARE(tools.posts()->count(),0);
        for(const auto &request:http.requests)QVERIFY(!request.startsWith("POST "));
    }
    void liveEventsUpdateRepliesAndExcludeOtherForums(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));http.respond=[](QByteArray,const QUrl &url,QJsonObject){return emptyResponse(url);};
        DiscordClient client;configure(client,http);ForumTools tools(&client);QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);
        client.dispatch("THREAD_CREATE",forumThread("700"));QCOMPARE(tools.posts()->count(),1);
        client.dispatch("THREAD_CREATE",forumThread("701","102"));QCOMPARE(tools.posts()->count(),1);
        client.dispatch("MESSAGE_CREATE",{{"id","702"},{"channel_id","700"},{"content","Reply"},{"author",QJsonObject{{"id","43"}}}});
        QCOMPARE(tools.posts()->get(0).value("replyCount").toInt(),4);
        client.dispatch("MESSAGE_CREATE",{{"id","702"},{"channel_id","700"}});QCOMPARE(tools.posts()->get(0).value("replyCount").toInt(),4);
        client.dispatch("THREAD_DELETE",{{"id","700"},{"guild_id","100"}});QCOMPARE(tools.posts()->count(),0);
    }
    void serverErrorsClearBusyAndKeepCreationExplicit(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));int creates=0;
        http.respond=[&](QByteArray method,const QUrl &,QJsonObject){if(method=="POST")++creates;return Http::Response{403,QJsonObject{{"message","Missing Permissions"},{"code",50013}}};};
        DiscordClient client;configure(client,http);ForumTools tools(&client);QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);
        QVERIFY(!tools.error().isEmpty());QCOMPARE(tools.posts()->count(),0);
        QVERIFY(tools.createPost("Title","Body",{"201"}));QTRY_VERIFY_WITH_TIMEOUT(!tools.posting(),2000);
        QVERIFY(!tools.postError().isEmpty());QCOMPARE(client.currentChannelId(),QString("101"));QCOMPARE(creates,1);
        QTest::qWait(100);QCOMPARE(creates,1);
    }
    void delayedSnapshotsKeepNewerGatewayState(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));bool delayed=false;
        http.respond=[&](QByteArray,const QUrl &url,QJsonObject){
            if(url.path().endsWith("/threads/active"))return Http::Response{200,QJsonObject{{"threads",QJsonArray{forumThread("700"),forumThread("701")}}},delayed?120:0};
            return Http::Response{200,QJsonObject{{"threads",QJsonArray{forumThread("702","101",true),forumThread("703","101",true)}},{"has_more",false}},delayed?120:0};
        };
        DiscordClient client;configure(client,http);ForumTools tools(&client);QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);QCOMPARE(tools.posts()->count(),4);
        delayed=true;tools.refresh();QTRY_VERIFY(http.requests.size()>=3);
        auto updated=forumThread("700");updated["name"]="Live title";client.dispatch("THREAD_UPDATE",updated);
        client.dispatch("MESSAGE_CREATE",{{"id","710"},{"channel_id","700"},{"content","Reply"}});
        client.dispatch("THREAD_DELETE",{{"id","701"},{"guild_id","100"},{"parent_id","101"}});
        client.dispatch("THREAD_CREATE",forumThread("704"));
        QTRY_VERIFY(http.requests.size()>=4);
        auto archived=forumThread("702","101",true);archived["name"]="Live archived title";client.dispatch("THREAD_UPDATE",archived);
        client.dispatch("THREAD_DELETE",{{"id","703"},{"guild_id","100"},{"parent_id","101"}});
        QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);
        QCOMPARE(tools.posts()->count(),3);QCOMPARE(tools.posts()->byId("700").value("name").toString(),QString("Live title"));
        QCOMPARE(tools.posts()->byId("700").value("replyCount").toInt(),4);QCOMPARE(tools.posts()->byId("702").value("name").toString(),QString("Live archived title"));
        QVERIFY(tools.posts()->byId("701").isEmpty());QVERIFY(tools.posts()->byId("703").isEmpty());QVERIFY(!tools.posts()->byId("704").isEmpty());
    }
    void temporaryGatewayDisconnectDoesNotStrandRestBusy(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));int creates=0;
        http.respond=[&](QByteArray method,const QUrl &url,QJsonObject){if(method=="POST"){++creates;return Http::Response{200,forumThread("800"),100};}auto result=emptyResponse(url);result.delay=80;return result;};
        DiscordClient client;configure(client,http);ForumTools tools(&client);client.m_connected=false;emit client.stateChanged();
        QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);QVERIFY(tools.error().isEmpty());
        client.m_connected=true;emit client.stateChanged();QVERIFY(tools.createPost("Title","Body",{"201"}));
        client.m_connected=false;emit client.stateChanged();QTRY_VERIFY_WITH_TIMEOUT(!tools.posting(),2000);QCOMPARE(creates,1);QVERIFY(tools.postError().isEmpty());
        client.m_connected=true;emit client.stateChanged();QVERIFY(!tools.posting());QVERIFY(!tools.posts()->byId("800").isEmpty());QTest::qWait(120);QCOMPARE(creates,1);
    }
    void archiveCursorMustMoveToStrictlyEarlierInstant(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));
        http.respond=[](QByteArray,const QUrl &url,QJsonObject){if(url.path().endsWith("/threads/active"))return emptyResponse(url);
            const bool next=QUrlQuery(url).hasQueryItem("before");return Http::Response{200,QJsonObject{{"threads",QJsonArray{forumThread(next?"899":"900","101",true,next?"2026-10-22T12:00:00.000+00:00":"2026-10-22T12:00:00.000Z")}},{"has_more",true}}};};
        DiscordClient client;configure(client,http);ForumTools tools(&client);QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);QVERIFY(tools.hasMore());
        tools.loadMore();QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);QVERIFY(!tools.hasMore());QVERIFY(!tools.error().isEmpty());
    }
    void directThreadInfersForumAndParentPermissionUpdates(){
        Http http;QVERIFY(http.listen(QHostAddress::LocalHost));http.respond=[](QByteArray,const QUrl &url,QJsonObject){return emptyResponse(url);};
        DiscordClient client;configure(client,http);client.m_channelCache["100"].append(forumThread("900"));client.rebuildChannels(false);
        client.m_currentChannelId="900";client.m_currentChannelKind="thread";ForumTools tools(&client);QTRY_VERIFY_WITH_TIMEOUT(!tools.loading(),2000);
        QCOMPARE(tools.channelId(),QString("101"));QVERIFY(tools.currentThread());QVERIFY(tools.canReply());
        client.dispatch("CHANNEL_UPDATE",{{"id","101"},{"guild_id","100"},{"type",15},{"permission_overwrites",QJsonArray{QJsonObject{{"id","42"},{"type",1},{"deny",QString::number(1ULL<<38)},{"allow","0"}}}}});
        QVERIFY(!tools.canReply());
    }
};
QTEST_GUILESS_MAIN(ForumToolsTest)
#include "forum_tools_test.moc"
