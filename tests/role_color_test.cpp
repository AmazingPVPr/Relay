#include "DiscordClient.h"
#include <QJSEngine>
#include <QFile>
#include <QtTest>

// Synthetic display and queued-request fixtures; no account, network or audio.
class RoleColorTest final : public QObject {
    Q_OBJECT
    static QJsonObject user() { return {{"id","51"},{"username","Global name"},{"avatar","global_avatar"},{"bot",true},{"public_flags",1<<16}}; }
    static QJsonObject role(const QString &id,int position,int color,const QString &name) {
        return {{"id",id},{"position",position},{"color",color},{"name",name}};
    }
    static void setup(DiscordClient &client) {
        client.m_currentGuildId="100";client.m_currentChannelId="101";client.m_currentUserId="42";
        client.m_guildObjects.insert("100",{{"id","100"},{"roles",QJsonArray{role("900",20,0,"Top uncolored"),role("800",10,0x12ab34,"Green"),role("700",10,0x1234ab,"Blue wins tie")}}});
        client.m_guildObjects.insert("200",{{"id","200"},{"roles",QJsonArray{role("600",5,0xff8040,"Orange")}}});
        client.cacheChannels("100",{QJsonObject{{"id","101"},{"type",0},{"name","Chat A"}}});
        client.cacheChannels("200",{QJsonObject{{"id","201"},{"type",0},{"name","Chat B"}},QJsonObject{{"id","202"},{"type",2},{"name","Voice B"}}});
        client.cacheMember("100",{{"user",user()},{"nick","Nick A"},{"avatar","avatar_a"},{"roles",QJsonArray{"900","800","700"}}});
        client.cacheMember("200",{{"user",user()},{"nick","Nick B"},{"avatar","avatar_b"},{"roles",QJsonArray{"600"}}});
    }
    static QJsonObject message(const QString &channel) {
        return {{"id","501"},{"channel_id",channel},{"author",user()},{"content","Fixture"}};
    }
private slots:
    void sidebarGroupsOnlyActiveHoistedMembersAndSortsTheRemainingPresenceGroups() {
        QFile script(QFINDTESTDATA("../qml/components/MemberGroups.js"));QVERIFY(script.open(QIODevice::ReadOnly));
        QJSEngine engine;
        auto source=QString::fromUtf8(script.readAll());source.remove(".pragma library");
        QVERIFY2(!engine.evaluate(source).isError(),"Member grouping script failed to load");
        const auto result=engine.evaluate(R"(groupMembers([
            {recordId:'1',name:'Offline admin',status:'offline',hoistedRoleId:'900',hoistedRoleName:'Admin',hoistedRolePosition:20},
            {recordId:'2',name:'zebra',status:'online',hoistedRoleId:'900',hoistedRoleName:'Admin',hoistedRolePosition:20,channelId:'101'},
            {recordId:'3',name:'alpha',status:'dnd',hoistedRoleId:'900',hoistedRoleName:'Admin',hoistedRolePosition:20},
            {recordId:'4',name:'Bot',status:'online',bot:true,roleName:'Bot role',rolePosition:80},
            {recordId:'5',name:'Member',status:'idle',roleName:'Members',rolePosition:1},
            {recordId:'6',name:'Offline bot',status:'offline',bot:true,roleName:'Bot role'},
            {recordId:'7',name:'Other role with same name',status:'online',hoistedRoleId:'800',hoistedRoleName:'Admin',hoistedRolePosition:10},
            {recordId:'8',name:'Unknown',status:'unknown'},
            {recordId:'9',name:'Invisible',status:'invisible',hoistedRoleId:'900',hoistedRoleName:'Admin',hoistedRolePosition:20}
        ]))");
        QVERIFY2(!result.isError(),qPrintable(result.toString()));
        const auto groups=result.toVariant().toMap();const auto rows=groups.value("members").toList();
        QStringList names,keys;for(const auto &entry:rows){names.append(entry.toMap().value("name").toString());keys.append(entry.toMap().value("groupKey").toString());}
        QCOMPARE(names,QStringList({"alpha","zebra","Other role with same name","Bot","Member","Unknown","Invisible","Offline admin","Offline bot"}));
        QCOMPARE(keys,QStringList({"role:900","role:900","role:800","online","online","unknown","offline","offline","offline"}));
        QCOMPARE(groups.value("labels").toMap().value("role:900").toString(),"Admin — 2");
        QCOMPARE(groups.value("onlineCount").toInt(),5);QCOMPARE(groups.value("offlineCount").toInt(),3);QCOMPARE(groups.value("voiceCount").toInt(),1);
        QVERIFY(rows.last().toMap().value("offline").toBool());QVERIFY(!rows.first().toMap().value("offline").toBool());
    }
    void memberGroupsUseTheHighestHoistedRoleIndependentlyOfColorAndHighestRole() {
        DiscordClient client;setup(client);
        auto green=role("800",10,0x12ab34,"Separate group");green.insert("hoist",true);
        auto lower=role("600",5,0xff8040,"Lower group");lower.insert("hoist",true);
        client.m_guildObjects["100"].insert("roles",QJsonArray{role("900",20,0,"Top hidden role"),green,lower});
        auto row=client.memberPresentation("100",user(),{{"roles",QJsonArray{"900","800","600"}}});
        QCOMPARE(row.value("roleName").toString(),"Top hidden role");
        QCOMPARE(row.value("hoistedRoleId").toString(),"800");QCOMPARE(row.value("hoistedRoleName").toString(),"Separate group");
        QCOMPARE(row.value("hoistedRolePosition").toInt(),10);QCOMPARE(row.value("roleColor").toString(),"#12ab34");
        row=client.memberPresentation("100",user(),{{"roles",QJsonArray{"900"}}});
        QVERIFY(row.value("hoistedRoleId").toString().isEmpty());
        // Live role settings must refresh the grouping metadata too.
        client.rebuildMembers();green.insert("hoist",false);
        client.dispatch("GUILD_ROLE_UPDATE",{{"guild_id","100"},{"role",green}});
        QVERIFY(client.members()->byId("51").value("hoistedRoleId").toString().isEmpty());
    }
    void highestColoredRoleAndTieOrderingMatchAcrossViews() {
        DiscordClient client;setup(client);client.rebuildMembers();
        const auto row=client.members()->byId("51");
        QCOMPARE(row.value("roleName").toString(),"Top uncolored");
        QCOMPARE(row.value("roleColor").toString(),"#1234ab");
        QCOMPARE(row.value("rolePosition").toInt(),20);
        QVERIFY(row.value("bot").toBool());QVERIFY(row.value("verifiedBot").toBool());
        QCOMPARE(client.memberInfo("51").value("roleColor").toString(),"#1234ab");
        QCOMPARE(client.messageRow(message("101")).value("roleColor").toString(),"#1234ab");
        QJsonArray roles; const auto original=client.m_guildObjects["100"].value("roles").toArray(); for(int i=original.size()-1;i>=0;--i)roles.append(original[i]);
        client.m_guildObjects["100"].insert("roles",roles);
        QCOMPARE(client.memberInfo("51").value("roleColor").toString(),"#1234ab");
        auto role=roles[0].toObject();role.insert("colors",QJsonObject{{"primary_color",0xaabbcc}});roles[0]=role;
        client.m_guildObjects["100"].insert("roles",roles);
        QCOMPARE(client.memberInfo("51").value("roleColor").toString(),"#aabbcc");
    }
    void messageGuildUsesChannelContextAndGuildAvatar() {
        DiscordClient client;setup(client);
        const auto row=client.messageRow(message("201"));
        QCOMPARE(row.value("guildId").toString(),"200");QCOMPARE(row.value("author").toString(),"Nick B");
        QCOMPARE(row.value("roleColor").toString(),"#ff8040");QVERIFY(row.value("avatarUrl").toString().contains("/guilds/200/users/51/avatars/avatar_b"));
        QVERIFY(row.value("bot").toBool());QVERIFY(row.value("verifiedBot").toBool());
    }
    void directMessagesDoNotInheritSelectedServer() {
        DiscordClient client;setup(client);
        client.acceptDm({{"id","301"},{"type",1},{"recipients",QJsonArray{user()}}});
        const auto row=client.messageRow(message("301"));
        QVERIFY(row.value("guildId").toString().isEmpty());QVERIFY(row.value("roleColor").toString().isEmpty());
        QCOMPARE(row.value("author").toString(),"Global name");QVERIFY(row.value("avatarUrl").toString().contains("/avatars/51/global_avatar"));
        const auto profile=client.memberInfoForGuild("51",{});
        QCOMPARE(profile.value("name").toString(),"Global name");QVERIFY(profile.value("roleColor").toString().isEmpty());
        auto explicitDm=message("101");explicitDm.insert("guild_id",QJsonValue::Null);
        QVERIFY(client.messageRow(explicitDm).value("roleColor").toString().isEmpty());
    }
    void voiceAndProfileUseVoiceServerWhileBrowsingElsewhere() {
        DiscordClient client;setup(client);client.m_voiceGuildId="200";
        client.m_voiceStates["200"].insert("51",{{"channel_id","202"},{"nick","Stale nick"},{"streaming",true}});
        client.rebuildVoiceMembers();const auto row=client.voiceMembers()->byId("51");
        QCOMPARE(row.value("guildId").toString(),"200");QCOMPARE(row.value("name").toString(),"Nick B");QCOMPARE(row.value("roleColor").toString(),"#ff8040");
        QVERIFY(row.value("bot").toBool());QVERIFY(row.value("verifiedBot").toBool());QVERIFY(row.value("avatarUrl").toString().contains("avatar_b"));
        const auto profile=client.memberInfoForGuild("51","200");
        QCOMPARE(profile.value("name").toString(),"Nick B");QCOMPARE(profile.value("roleColor").toString(),"#ff8040");QCOMPARE(profile.value("channelName").toString(),"Voice B");
        QCOMPARE(profile.value("streamKey").toString(),"guild:200:202:51");
        QVERIFY(!client.memberInfoForGuild("51","100").value("streaming").toBool());
    }
    void verifiedBadgeRequiresBotAndPublicFlag() {
        DiscordClient client;setup(client);
        auto person=user();person.insert("bot",false);QVERIFY(!client.memberPresentation("100",person,{}).value("verifiedBot").toBool());
        person.insert("bot",true);person.remove("public_flags");person.insert("verified",true);person.insert("flags",1<<16);
        QVERIFY(!client.memberPresentation("100",person,{}).value("verifiedBot").toBool());
        client.m_users["51"].remove("public_flags");
        client.acceptProfile("51","100",{{"user",user()}});
        QVERIFY(client.memberInfo("51").value("verifiedBot").toBool());
    }
    void liveRoleAndMemberChangesRefreshAuthorAndVoiceWithoutResettingHistory() {
        DiscordClient client;setup(client);client.m_voiceGuildId="100";
        client.m_voiceStates["100"].insert("51",{{"channel_id","102"}});
        client.rebuildVoiceMembers();client.mergeMessages({message("101")});
        client.messages()->patch("501",{{"edited",true}});
        client.messages()->upsert({{"recordId","pending:fixture"},{"author","Pending"},{"content","Keep this draft"},{"pending",true}});
        QSignalSpy reset(client.messages(),&QAbstractItemModel::modelReset);
        const auto original=client.messages()->byId("501");
        client.dispatch("GUILD_ROLE_UPDATE",{{"guild_id","100"},{"role",role("700",10,0xaabbcc,"Updated color")}});
        QCOMPARE(client.messages()->byId("501").value("roleColor").toString(),"#aabbcc");
        QCOMPARE(client.voiceMembers()->byId("51").value("roleColor").toString(),"#aabbcc");
        client.dispatch("GUILD_ROLE_DELETE",{{"guild_id","100"},{"role_id","700"}});
        QCOMPARE(client.messages()->byId("501").value("roleColor").toString(),"#12ab34");
        QCOMPARE(client.voiceMembers()->byId("51").value("roleColor").toString(),"#12ab34");
        client.dispatch("GUILD_MEMBER_UPDATE",{{"guild_id","100"},{"user",user()},{"roles",QJsonArray{"900"}},{"nick","Renamed"},{"avatar","new_avatar"}});
        const auto updated=client.messages()->byId("501");
        QVERIFY(updated.value("roleColor").toString().isEmpty());QCOMPARE(updated.value("author").toString(),"Renamed");
        QCOMPARE(client.voiceMembers()->byId("51").value("name").toString(),"Renamed");
        QVERIFY(client.voiceMembers()->byId("51").value("roleColor").toString().isEmpty());
        QVERIFY(updated.value("avatarUrl").toString().contains("new_avatar"));
        QCOMPARE(updated.value("content"),original.value("content"));QVERIFY(updated.value("edited").toBool());
        QCOMPARE(client.messages()->count(),2);QCOMPARE(client.messages()->get(1).value("recordId").toString(),"pending:fixture");
        QVERIFY(client.messages()->byId("pending:fixture").value("pending").toBool());QCOMPARE(reset.count(),0);
        QCOMPARE(client.m_messageObjects.size(),1);
    }
    void profileRequestKeepsExplicitServerAndDmContext() {
        DiscordClient client;setup(client);client.m_connected=true;client.m_token="synthetic-fixture";client.m_globalReset=QDateTime::currentMSecsSinceEpoch()+60000;
        client.fetchProfileForGuild("51","200");QCOMPARE(client.m_queue.size(),1);
        QVERIFY(client.m_queue.head()->path.endsWith("&guild_id=200"));
        client.fetchProfileForGuild("51",{});QCOMPARE(client.m_queue.size(),2);
        QVERIFY(!client.m_queue.last()->path.contains("guild_id="));
        client.fetchProfileForGuild("51","../bad");QCOMPARE(client.m_queue.size(),2);
    }
};
QTEST_GUILESS_MAIN(RoleColorTest)
#include "role_color_test.moc"
