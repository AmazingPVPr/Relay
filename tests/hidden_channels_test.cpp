#include "DiscordClient.h"
#include <QtTest>
#include <QDateTime>

class HiddenChannelsTest final : public QObject {
    Q_OBJECT
    static QJsonObject channel(QString id, int type, bool hidden, QString parent = {}) {
        return {{"id",id},{"type",type},{"name",QStringLiteral("channel-")+id},{"parent_id",parent},
            {"topic","Metadata already received"},{"permission_overwrites",QJsonArray{QJsonObject{
                {"id","100"},{"type",0},{"allow","0"},{"deny",hidden ? "1024" : "0"}}}}};
    }
    static void configure(DiscordClient &client) {
        client.m_currentUserId="42";
        client.m_demoMode=true;
        client.m_currentGuildId="100";
        client.m_guildObjects.insert("100",{{"id","100"},{"owner_id","99"},
            {"roles",QJsonArray{QJsonObject{{"id","100"},{"permissions","1049600"}}}},
            {"member",QJsonObject{{"user_id","42"},{"roles",QJsonArray{}}}}});
        client.cacheChannels("100",{channel("200",4,true),channel("201",0,true,"200"),channel("202",2,true,"200"),channel("203",0,false)});
        client.rebuildChannels(false);
    }
private slots:
    void optInExposesOnlyReceivedMetadata() {
        DiscordClient client;configure(client);
        QCOMPARE(client.channels()->count(),1); QVERIFY(!client.showHiddenChannels());
        client.setShowHiddenChannels(true);
        QCOMPARE(client.channels()->count(),4);
        QVERIFY(client.channels()->byId("201").value("hidden").toBool());
        QCOMPARE(client.channels()->byId("201").value("topic").toString(),QStringLiteral("Metadata already received"));
        QVERIFY(!client.channels()->byId("203").value("hidden").toBool());
        QVERIFY(client.channels()->byId("999").isEmpty());
        client.selectChannel("201");
        QCOMPARE(client.currentChannelKind(),QStringLiteral("hidden"));
        QVERIFY(client.messages()->count()==0);QVERIFY(!client.loadingMessages());
        QVERIFY(client.m_subscribedChannel.isEmpty());
        client.loadOlderMessages();client.loadNewerMessages();client.refresh();
        QVERIFY(!client.loadingMessages());QVERIFY(client.m_queue.isEmpty());
        QVERIFY(!client.jumpToMessage("201","300"));
        QVERIFY(!client.sendMessage("must not send"));
        client.setShowHiddenChannels(false);
        QCOMPARE(client.channels()->count(),1);
        QCOMPARE(client.currentChannelId(),QStringLiteral("203"));
        QVERIFY(client.currentChannelKind()!=QStringLiteral("hidden"));
    }
    void voiceCannotJoinMetadataOnlyChannel() {
        DiscordClient client;configure(client);client.setShowHiddenChannels(true);
        // Use a signed-in reconnecting fixture. Its leave is queued locally;
        // no real gateway socket or credentials are used by this test.
        client.m_connected=false;client.m_connecting=true;client.m_demoMode=false;
        client.m_token="generated-fixture-token";
        QSignalSpy rejected(&client,&DiscordClient::voiceStateRequestRejected);
        QVERIFY(!client.sendVoiceState("100","202",false,false));
        QCOMPARE(rejected.size(),1);QVERIFY(rejected.first()[2].toString().contains("not accessible"));
        QVERIFY(client.sendVoiceState("100",{},false,false));
        QCOMPARE(client.m_pendingVoiceState.value("guild_id").toString(),QString("100"));
        QVERIFY(client.m_pendingVoiceState.value("channel_id").isNull());
        client.openVoiceChat("202");
        QCOMPARE(client.currentChannelKind(),QStringLiteral("hidden"));
        QVERIFY(client.m_subscribedChannel.isEmpty());
        QVERIFY(client.m_queue.isEmpty());
    }
    void defaultsSkipHiddenAndNormalRolesStillApply() {
        DiscordClient client;configure(client);client.setShowHiddenChannels(true);
        client.rebuildChannels(true);
        QCOMPARE(client.currentChannelId(),QStringLiteral("203"));
        auto guild=client.m_guildObjects.value("100");
        guild.insert("owner_id","42");client.m_guildObjects.insert("100",guild);
        client.setShowHiddenChannels(false);
        QCOMPARE(client.channels()->count(),4);
        QVERIFY(!client.channels()->byId("201").value("hidden").toBool());
    }
    void connectedMetadataSelectionCannotQueueReadsWritesOrMembers() {
        DiscordClient client;configure(client);client.setShowHiddenChannels(true);
        client.selectChannel("203");QVERIFY(client.messages()->count()>0);QCOMPARE(client.m_subscribedChannel,QString("203"));
        client.m_demoMode=false;client.m_connected=true;client.m_token="generated-fixture-token";
        // If any guard regresses, retain its job for assertion without issuing
        // network traffic or touching a real account.
        client.m_apiBase="http://127.0.0.1:1";client.m_globalReset=QDateTime::currentMSecsSinceEpoch()+120000;
        client.selectChannel("201");QCOMPARE(client.currentChannelKind(),QString("hidden"));
        QVERIFY(client.messages()->rows().isEmpty());QVERIFY(client.m_messageObjects.isEmpty());QVERIFY(client.m_deferredLiveMessages.isEmpty());
        QVERIFY(client.m_subscribedChannel.isEmpty());QVERIFY(!client.m_membersLoading);QVERIFY(!client.m_memberDeadline.isActive());
        QVERIFY(client.memberSubscription().value("channels").toObject().isEmpty());
        client.m_hasMoreMessages=true;client.m_hasNewerMessages=true;
        client.loadOlderMessages();client.loadNewerMessages();client.fetchMessages(false);client.fetchMessages(true);client.jumpToPresent();client.refresh();
        QVERIFY(!client.loadingMessages());QVERIFY(client.m_queue.isEmpty());QVERIFY(client.m_replies.isEmpty());
        QVERIFY(!client.jumpToMessage("201","999"));QVERIFY(!client.sendMessage("generated fixture message"));
        QVERIFY(client.m_queue.isEmpty());QVERIFY(client.m_replies.isEmpty());QVERIFY(!client.sendingMessage());
        client.openVoiceChat("202");QCOMPARE(client.currentChannelKind(),QString("hidden"));
        QVERIFY(!client.sendVoiceState("100","202",false,false));QVERIFY(client.m_subscribedChannel.isEmpty());
        QVERIFY(!client.createVoiceInvite("202"));QVERIFY(!client.inviteBusy());QVERIFY(client.m_queue.isEmpty());
        // Disabling the option removes hidden metadata and clears its selected
        // state even when there is no accessible channel to choose instead.
        client.cacheChannels("100",{channel("200",4,true),channel("201",0,true,"200"),channel("202",2,true,"200")});
        client.setShowHiddenChannels(false);QCOMPARE(client.channels()->count(),0);QVERIFY(client.currentChannelId().isEmpty());
        QVERIFY(client.currentChannelKind().isEmpty());QVERIFY(client.messages()->rows().isEmpty());QVERIFY(client.m_subscribedChannel.isEmpty());
        QVERIFY(client.m_queue.isEmpty());QVERIFY(client.m_replies.isEmpty());
    }
    void permissionRevocationInvalidatesAnOpenConversation() {
        DiscordClient client;configure(client);client.setShowHiddenChannels(true);client.selectChannel("203");
        QVERIFY(client.messages()->count()>0);QCOMPARE(client.m_subscribedChannel,QString("203"));
        auto revoked=channel("203",0,true);revoked.insert("guild_id","100");
        client.dispatch("CHANNEL_UPDATE",revoked);
        QVERIFY(client.channels()->byId("203").value("hidden").toBool());
        QCOMPARE(client.currentChannelKind(),QString("hidden"));QVERIFY(client.messages()->rows().isEmpty());
        QVERIFY(client.m_messageObjects.isEmpty());QVERIFY(client.m_subscribedChannel.isEmpty());
        QVERIFY(!client.sendMessage("not permitted"));QVERIFY(client.m_queue.isEmpty());
        client.setShowHiddenChannels(false);QVERIFY(client.channels()->byId("203").isEmpty());QVERIFY(client.currentChannelId().isEmpty());
    }
    void latePushesCannotPopulateMetadataOnlyMessageHistory() {
        DiscordClient client;configure(client);client.setShowHiddenChannels(true);client.selectChannel("201");
        client.acceptMessage({{"id","9001"},{"channel_id","201"},{"content","generated late push"},
            {"author",QJsonObject{{"id","55"},{"username","fixture"}}}});
        QVERIFY(client.messages()->rows().isEmpty());QVERIFY(client.m_messageObjects.isEmpty());
    }
    void grantedPermissionDoesNotLatchTheMetadataOnlyView() {
        DiscordClient client;configure(client);client.setShowHiddenChannels(true);client.selectChannel("201");
        QCOMPARE(client.currentChannelKind(),QString("hidden"));
        auto granted=channel("201",0,false,"200");granted.insert("guild_id","100");
        client.dispatch("CHANNEL_UPDATE",granted);
        QVERIFY(!client.channels()->byId("201").value("hidden").toBool());
        // Main avoids re-selecting the same channel ID. The backend must retire
        // stale metadata mode when permission changes so that click is useful.
        QVERIFY(client.currentChannelKind()!=QString("hidden"));
        if(client.currentChannelId()=="201")QCOMPARE(client.currentChannelKind(),QString("text"));
    }
};
QTEST_GUILESS_MAIN(HiddenChannelsTest)
#include "hidden_channels_test.moc"
