#include "DiscordClient.h"
#include <QAbstractItemModelTester>
#include <QDateTime>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QUrlQuery>
#include <QtTest>

// Synthetic gateway fixtures only. No login, remote requests or desktop notices.
class MemberBackendTest final : public QObject {
    Q_OBJECT
    static QJsonObject user(QString id, QString name = "River") {
        return {{"id", id}, {"username", name}, {"avatar", "fixture_hash"}};
    }
    static QJsonObject member(QString id, QString name = "River") {
        return {{"user", user(id, name)}, {"roles", QJsonArray{"900"}}, {"presence", QJsonObject{{"status", "online"}}}};
    }
    static QJsonObject guild(QString id = "100") {
        return {{"id", id}, {"name", "Fixture server"}, {"icon", "fixture_icon"}, {"member_count", 3},
            {"default_message_notifications", 0},
            {"member", QJsonObject{{"user", user("42")}, {"roles", QJsonArray{"900"}}}},
            {"roles", QJsonArray{QJsonObject{{"id", id}, {"permissions", "1024"}, {"position", 0}},
                QJsonObject{{"id", "900"}, {"name", "Helpers"}, {"position", 2}, {"color", 0x23aaff}}}},
            {"channels", QJsonArray{QJsonObject{{"id", "101"}, {"name", "general"}, {"type", 0}},
                QJsonObject{{"id", "102"}, {"name", "other"}, {"type", 0}, {"parent_id", "110"}}}}};
    }
    static void setup(DiscordClient &client) {
        client.m_currentUserId = "42";
        client.m_users.insert("42", user("42", "Me"));
        client.acceptGuild(guild());
        client.selectGuild("100");
        client.m_connected = true;
        client.m_subscribedChannel.clear();
        client.subscribeMembers();
    }
    static QJsonObject list(const QJsonArray &ops, int count = 3, QString id = "everyone") {
        return {{"guild_id", "100"}, {"id", id}, {"member_count", count}, {"ops", ops}};
    }
    static QJsonObject sync(const QJsonArray &items, int start = 0, int end = 99) {
        return {{"op", "SYNC"}, {"range", QJsonArray{start, end}}, {"items", items}};
    }
    static QJsonObject item(QString id, QString name = "River") { return {{"member", member(id, name)}}; }
    static QJsonObject message(QString id, QString channel = "102", bool direct = false) {
        QJsonObject result{{"id", id}, {"channel_id", channel}, {"content", "Fixture notice"},
            {"author", user("55", "Sender")}, {"timestamp", QDateTime::currentDateTimeUtc().addSecs(1).toString(Qt::ISODateWithMs)}};
        if (!direct) result.insert("guild_id", "100");
        return result;
    }
private slots:
    void nativeImagesRequestMaximumCdnSizeAndKeepOriginalAttachments() {
        DiscordClient client;
        QCOMPARE(QUrlQuery(QUrl(client.avatarUrl(user("42")))).queryItemValue("size"), "4096");
        QCOMPARE(QUrlQuery(QUrl(client.guildRow(guild()).value("iconUrl").toString())).queryItemValue("size"), "4096");
        auto object = message("501");
        const QString original = "https://cdn.discordapp.com/attachments/101/501/photo.png?ex=1234&is=abcd&hm=signature";
        object.insert("attachments", QJsonArray{QJsonObject{{"id", "501"}, {"filename", "photo.png"},
            {"url", original}, {"proxy_url", "https://media.discordapp.net/attachments/101/501/photo.png?width=640"},
            {"width", 4096}, {"height", 2160}}});
        QCOMPARE(client.messageRow(object).value("attachments").toList().first().toMap().value("url").toString(), original);
    }
    void realMemberSyncHasRolesPresenceAndHonestPartialStatus() {
        DiscordClient client; setup(client);
        QAbstractItemModelTester model(client.members(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(client.membersLoading());
        auto bot = member("51", "Lake"); bot.insert("nick", "Nickname");
        auto botUser = bot.value("user").toObject(); botUser.insert("bot", true); bot.insert("user", botUser);
        bot.insert("avatar", "guild_avatar");
        client.dispatch("GUILD_MEMBER_LIST_UPDATE", list({sync({QJsonObject{{"group", QJsonObject{{"id", "online"}}}},
            QJsonObject{{"member", bot}}, item("52", "River")})}, 5000));
        QCOMPARE(client.members()->count(), 2);
        QCOMPARE(client.members()->get(0).value("name").toString(), "Nickname");
        QCOMPARE(client.members()->get(0).value("roleName").toString(), "Helpers");
        QCOMPARE(client.members()->get(0).value("roleColor").toString(), "#23aaff");
        QCOMPARE(client.members()->get(0).value("status").toString(), "online");
        QVERIFY(client.members()->get(0).value("bot").toBool());
        QVERIFY(client.members()->get(0).value("avatarUrl").toString().contains("/guilds/100/users/51/"));
        QVERIFY(client.membersPartial()); QVERIFY(!client.membersLoading()); QCOMPARE(client.memberCount(), 5000);
        QVERIFY(client.membersStatus().contains("partial"));
        QCOMPARE(client.memberSubscription().value("channels").toObject().value("101").toArray(),
                 (QJsonArray{QJsonArray{0, 99}, QJsonArray{100, 199}}));
        QVERIFY(client.memberSubscription().value("typing").toBool());
    }
    void incrementalListOperationsPreserveSlotsIncludingGroups() {
        DiscordClient client; setup(client);
        client.acceptMemberList(list({sync({item("51", "One"), item("52", "Two"), item("53", "Three")})}));
        QVERIFY(!client.membersPartial());
        client.acceptMemberList(list({QJsonObject{{"op", "INSERT"}, {"index", 1}, {"item", item("54", "Four")}},
            QJsonObject{{"op", "UPDATE"}, {"index", 2}, {"item", item("52", "Updated")}},
            QJsonObject{{"op", "DELETE"}, {"index", 0}}}));
        QCOMPARE(client.members()->get(0).value("recordId").toString(), "54");
        QCOMPARE(client.members()->get(1).value("name").toString(), "Updated");
        QCOMPARE(client.members()->get(2).value("recordId").toString(), "53");
        client.acceptMemberList(list({QJsonObject{{"op", "INVALIDATE"}, {"range", QJsonArray{1, 199}}}}));
        QCOMPARE(client.members()->count(), 1); QVERIFY(client.membersPartial());
    }
    void listBoundsAndWrongGuildOrPermissionListAreRejected() {
        DiscordClient client; setup(client);
        auto wrong = list({sync({item("51")})}); wrong.insert("guild_id", "999");
        client.acceptMemberList(wrong); QCOMPARE(client.members()->count(), 0);
        client.acceptMemberList(list({sync({item("51")})}, 1, "wrong-list")); QCOMPARE(client.members()->count(), 0);
        QJsonArray huge; for (int i = 0; i < 400; ++i) huge.append(item(QString::number(1000 + i)));
        client.acceptMemberList(list({sync(huge, 0, 1000000)}, 1000000));
        QCOMPARE(client.members()->count(), 200); QCOMPARE(client.m_memberSlots.size(), 200);
        client.acceptMemberList(list({sync({item("99999")}, -1, 100),
            QJsonObject{{"op", "INSERT"}, {"index", 2147483647}, {"item", item("99999")}}}));
        QCOMPARE(client.members()->count(), 200); QVERIFY(client.members()->byId("99999").isEmpty());
        client.selectGuild({}); QCOMPARE(client.members()->count(), 0);
        client.acceptMemberList(list({sync({item("51")})})); QCOMPARE(client.members()->count(), 0);
    }
    void presenceMembershipAndVoiceEventsUpdateRealRowsAndServerBadges() {
        DiscordClient client; setup(client);
        client.acceptMemberList(list({sync({item("51")})}, 1));
        QSignalSpy resets(client.members(),&QAbstractItemModel::modelReset);
        client.dispatch("PRESENCE_UPDATE", {{"guild_id", "100"}, {"user", user("51")}, {"status", "dnd"}});
        QCOMPARE(client.members()->get(0).value("status").toString(), "dnd");
        QCOMPARE(resets.count(),0);
        auto update = member("51"); update.insert("guild_id", "100"); update.insert("nick", "Renamed");
        client.dispatch("GUILD_MEMBER_UPDATE", update);
        QCOMPARE(client.members()->get(0).value("name").toString(), "Renamed");
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "100"}, {"channel_id", "201"}, {"user_id", "51"}, {"self_stream", true}, {"self_mute", true}});
        QVERIFY(client.guilds()->byId("100").value("voiceActive").toBool());
        QVERIFY(client.guilds()->byId("100").value("streaming").toBool());
        QVERIFY(client.members()->get(0).value("streaming").toBool()); QVERIFY(client.members()->get(0).value("muted").toBool());
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "100"}, {"channel_id", QJsonValue::Null}, {"user_id", "51"}});
        QVERIFY(!client.guilds()->byId("100").value("voiceActive").toBool());
        QVERIFY(!client.guilds()->byId("100").value("streaming").toBool());
        client.dispatch("GUILD_MEMBER_REMOVE", {{"guild_id", "100"}, {"user", user("51")}});
        QCOMPARE(client.members()->count(), 0);
    }
    void messageAuthorsNeverBecomeGuildMembersAndTimeoutIsHonest() {
        DiscordClient client; setup(client);
        client.dispatch("MESSAGE_CREATE", message("501", "101"));
        QCOMPARE(client.members()->count(), 0); QVERIFY(client.mentionCandidates("Sender").isEmpty());
        client.cacheMember("100", member("51"));
        QMetaObject::invokeMethod(&client.m_memberDeadline, "timeout", Qt::DirectConnection);
        QVERIFY(!client.membersLoading()); QVERIFY(client.membersPartial());
        QCOMPARE(client.members()->count(), 1); QVERIFY(client.membersStatus().contains("unavailable"));
    }
    void readyAndSupplementalUseRealMergedMemberAndPresenceData() {
        DiscordClient client;
        auto initial = guild(); initial.remove("members");
        const QJsonObject me{{"user_id", "42"}, {"roles", QJsonArray{"900"}}};
        const QJsonObject lake{{"user_id", "51"}, {"nick", "From snapshot"}};
        const QJsonObject presence{{"user_id", "51"}, {"status", "idle"}};
        client.dispatch("READY", {{"user", user("42", "Me")}, {"users", QJsonArray{user("51", "Lake")}},
            {"guilds", QJsonArray{initial}}, {"merged_members", QJsonArray{QJsonArray{me}}}});
        client.selectGuild("100");
        client.dispatch("READY_SUPPLEMENTAL", {{"guilds", QJsonArray{QJsonObject{{"id", "100"}}}},
            {"merged_members", QJsonArray{QJsonArray{lake}}},
            {"merged_presences", QJsonObject{{"guilds", QJsonArray{QJsonArray{presence}}}}}});
        QCOMPARE(client.members()->byId("51").value("name").toString(), "From snapshot");
        QCOMPARE(client.members()->byId("51").value("status").toString(), "idle");
        QVERIFY(client.membersPartial());
    }
    void notificationsExcludeHistorySelfSilencedDuplicatesAndReplay() {
        DiscordClient client; setup(client);
        client.m_notificationStartMs = QDateTime::currentMSecsSinceEpoch();
        QSignalSpy notifications(&client, &DiscordClient::notificationRequested);
        client.dispatch("MESSAGE_CREATE", message("501"));
        client.dispatch("MESSAGE_CREATE", message("501"));
        QCOMPARE(notifications.size(), 1);
        auto self = message("502"); self.insert("author", user("42")); client.dispatch("MESSAGE_CREATE", self);
        auto silent = message("503"); silent.insert("flags", 1 << 12); client.dispatch("MESSAGE_CREATE", silent);
        client.mergeMessages({message("504", "101")}); client.dispatch("MESSAGE_CREATE", message("504", "101"));
        auto old = message("505"); old.insert("timestamp", "2001-01-01T00:00:00Z"); client.dispatch("MESSAGE_CREATE", old);
        client.m_connected = false; client.dispatch("MESSAGE_CREATE", message("506"));
        client.m_connected = true; client.dispatch("MESSAGE_CREATE", message("506"));
        QCOMPARE(notifications.size(), 1);
        QCOMPARE(notifications.first().at(0).toString(), "102");
        QVERIFY(!notifications.first().at(3).toBool());
        QVERIFY(notifications.first().at(1).toString().contains("#other"));
    }
    void discordNotificationSettingsHonorMentionsMutesAndExpiredMutes() {
        DiscordClient client; setup(client); QSignalSpy notifications(&client, &DiscordClient::notificationRequested);
        client.acceptNotificationSettings({{"guild_id", "100"}, {"message_notifications", 1}, {"suppress_everyone", true}, {"suppress_roles", true}});
        client.dispatch("MESSAGE_CREATE", message("601")); QCOMPARE(notifications.size(), 0);
        auto everyone = message("602"); everyone.insert("mention_everyone", true); client.dispatch("MESSAGE_CREATE", everyone);
        auto role = message("603"); role.insert("mention_roles", QJsonArray{"900"}); client.dispatch("MESSAGE_CREATE", role);
        QCOMPARE(notifications.size(), 0);
        auto mention = message("604"); mention.insert("mentions", QJsonArray{user("42")}); client.dispatch("MESSAGE_CREATE", mention);
        QCOMPARE(notifications.size(), 1); QVERIFY(notifications.last().at(4).toBool());
        client.acceptNotificationSettings({{"guild_id", "100"}, {"muted", true}});
        mention.insert("id", "605"); client.dispatch("MESSAGE_CREATE", mention); QCOMPARE(notifications.size(), 1);
        client.acceptNotificationSettings({{"guild_id", "100"}, {"mute_config", QJsonObject{{"end_time", "2001-01-01T00:00:00.000Z"}}}});
        mention.insert("id", "606"); client.dispatch("MESSAGE_CREATE", mention); QCOMPARE(notifications.size(), 2);
        client.acceptNotificationSettings({{"guild_id", "100"}, {"channel_overrides", QJsonArray{QJsonObject{{"channel_id", "110"}, {"muted", true}}}}});
        mention.insert("id", "607"); client.dispatch("MESSAGE_CREATE", mention); QCOMPARE(notifications.size(), 2);
        client.acceptDm({{"id", "301"}, {"type", 1}, {"recipients", QJsonArray{user("55", "Sender")}}});
        client.dispatch("MESSAGE_CREATE", message("608", "301", true)); QCOMPARE(notifications.size(), 3);
        QVERIFY(notifications.last().at(3).toBool());
    }
    void mentionMetadataAndCandidatesUseKnownUsersAndRawTokens() {
        DiscordClient client; setup(client);
        client.cacheMember("100", member("51", "Alice")); client.cacheMember("100", member("52", "Lake"));
        client.rebuildMembers();
        const auto candidates = client.mentionCandidates("@ali"); QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().toMap().value("id").toString(), "51");
        QVERIFY(client.memberInfo("999").isEmpty());
        auto input = message("701"); input.insert("content", "Hi <@51> <@!42> and <@&900>");
        input.insert("mentions", QJsonArray{user("51", "Alice"), user("42", "Me")});
        input.insert("mention_roles", QJsonArray{"900"});
        const auto row = client.messageRow(input);
        QCOMPARE(row.value("rawContent").toString(), "Hi <@51> <@!42> and <@&900>");
        QCOMPARE(row.value("content").toString(), "Hi @Alice @Me and @Helpers");
        QCOMPARE(row.value("mentionUsers").toList().size(), 2);
        QCOMPARE(row.value("mentionRoles").toList().first().toMap().value("name").toString(), "Helpers");
        QVERIFY(row.value("currentUserMentioned").toBool());
        for (int i = 0; i < 40; ++i) client.cacheMember("100", member(QString::number(1000 + i), "Candidate" + QString::number(i)));
        QCOMPARE(client.mentionCandidates("").size(), 20);
    }
    void linksRejectUnsafeSchemesCredentialsAndControlCharacters() {
        for (const auto &raw : {"javascript:alert(1)", "file:///tmp/test", "discord://foo", "https://a:b@example.org/", "//example.org", "https:///", "https://example.org/%0Afoo", " https://example.org", "https://example.org:0"})
            QVERIFY2(DiscordClient::safeExternalUrl(raw).isEmpty(), raw);
        QCOMPARE(DiscordClient::safeExternalUrl("https://example.org/a?x=1&b=2#f").host(), "example.org");
        QCOMPARE(DiscordClient::safeExternalUrl("http://example.org/").scheme(), "http");
        DiscordClient client; QVERIFY(!client.openLink("javascript:alert(1)"));
    }
    void outgoingMentionsExplicitlyAllowUsersWithoutMassPings() {
        const auto allowed = DiscordClient::allowedMentions("Hi <@51> <@!52> <@51> @everyone <@&900>");
        QCOMPARE(allowed.value("users").toArray(), (QJsonArray{"51", "52"}));
        QVERIFY(allowed.value("parse").toArray().isEmpty());
        QVERIFY(!allowed.contains("roles")); QVERIFY(!allowed.value("replied_user").toBool());
        QString huge; for (int i = 0; i < 150; ++i) huge += "<@" + QString::number(1000 + i) + ">";
        QCOMPARE(DiscordClient::allowedMentions(huge).value("users").toArray().size(), 100);
    }
    void activitySoundsOnlyFollowLiveChangesInOwnCall() {
        DiscordClient client; setup(client);
        auto snapshot = guild(); snapshot.insert("voice_states", QJsonArray{
            QJsonObject{{"user_id", "42"}, {"channel_id", "201"}, {"session_id", "fixture-session"}},
            QJsonObject{{"user_id", "51"}, {"channel_id", "201"}}});
        QSignalSpy sounds(&client, &DiscordClient::activitySoundRequested);
        client.acceptGuild(snapshot); QCOMPARE(sounds.size(), 0);
        QJsonObject state{{"guild_id", "100"}, {"user_id", "51"}, {"channel_id", "201"}, {"self_stream", true}};
        client.dispatch("VOICE_STATE_UPDATE", state); QCOMPARE(sounds.size(), 1);
        QCOMPARE(sounds.last().first().toString(), "stream-start");
        client.dispatch("VOICE_STATE_UPDATE", state); QCOMPARE(sounds.size(), 1);
        state.insert("self_stream", false); client.dispatch("VOICE_STATE_UPDATE", state);
        QCOMPARE(sounds.last().first().toString(), "stream-stop");
        state.insert("channel_id", "202"); client.dispatch("VOICE_STATE_UPDATE", state);
        QCOMPARE(sounds.last().first().toString(), "user-leave");
        state.insert("self_stream", true); client.dispatch("VOICE_STATE_UPDATE", state); QCOMPARE(sounds.size(), 3);
        state.insert("channel_id", "201"); client.dispatch("VOICE_STATE_UPDATE", state);
        QCOMPARE(sounds.last().first().toString(), "user-join");
        client.m_connected = false; state.insert("channel_id", QJsonValue::Null); client.dispatch("VOICE_STATE_UPDATE", state);
        QCOMPARE(sounds.size(), 4);
    }
    void cameraActivitySoundsFollowLiveChangesInOwnCall() {
        DiscordClient client; setup(client);
        auto snapshot = guild(); snapshot.insert("voice_states", QJsonArray{
            QJsonObject{{"user_id", "42"}, {"channel_id", "201"}, {"session_id", "fixture-session"}},
            QJsonObject{{"user_id", "51"}, {"channel_id", "201"}, {"self_video", true}}});
        QSignalSpy sounds(&client, &DiscordClient::activitySoundRequested);
        client.acceptGuild(snapshot); QCOMPARE(sounds.size(), 0);
        QJsonObject state{{"guild_id", "100"}, {"user_id", "51"}, {"channel_id", "201"}, {"self_video", true}};
        client.dispatch("VOICE_STATE_UPDATE", state); QCOMPARE(sounds.size(), 0);
        state.insert("self_video", false); client.dispatch("VOICE_STATE_UPDATE", state);
        QCOMPARE(sounds.size(), 1); QCOMPARE(sounds.last().first().toString(), "stream-stop");
        state.insert("self_video", true); client.dispatch("VOICE_STATE_UPDATE", state);
        QCOMPARE(sounds.size(), 2); QCOMPARE(sounds.last().first().toString(), "stream-start");
        client.dispatch("VOICE_STATE_UPDATE", state); QCOMPARE(sounds.size(), 2);
        // An additional screen starts/stops independently of the live camera.
        state.insert("self_stream", true); client.dispatch("VOICE_STATE_UPDATE", state);
        QCOMPARE(sounds.size(), 3); QCOMPARE(sounds.last().first().toString(), "stream-start");
        state.insert("self_stream", false); client.dispatch("VOICE_STATE_UPDATE", state);
        QCOMPARE(sounds.size(), 4); QCOMPARE(sounds.last().first().toString(), "stream-stop");
        // Switching camera to screen in a single update does not play opposing cues.
        state.insert("self_stream", true); state.insert("self_video", false);
        client.dispatch("VOICE_STATE_UPDATE", state); QCOMPARE(sounds.size(), 4);
        state.insert("user_id", "42"); state.insert("self_video", true);
        client.dispatch("VOICE_STATE_UPDATE", state); QCOMPARE(sounds.size(), 4);
        state.insert("user_id", "52"); state.insert("channel_id", "202");
        client.dispatch("VOICE_STATE_UPDATE", state); QCOMPARE(sounds.size(), 4);
        state.insert("self_video", false); client.dispatch("VOICE_STATE_UPDATE", state); QCOMPARE(sounds.size(), 4);
        client.m_demoMode = true; state.insert("user_id", "51"); state.insert("channel_id", "201");
        state.insert("self_stream", false); client.dispatch("VOICE_STATE_UPDATE", state); QCOMPARE(sounds.size(), 4);
    }
    void localLiveVoiceJoinEstablishesMissingActivityBaseline() {
        DiscordClient client; setup(client);
        QSignalSpy sounds(&client, &DiscordClient::activitySoundRequested);
        QJsonObject own{{"guild_id", "100"}, {"user_id", "42"}, {"channel_id", "201"}, {"session_id", "fixture-session"}};
        client.dispatch("VOICE_STATE_UPDATE", own);
        QVERIFY(!client.m_voiceSnapshots.contains("100")); QCOMPARE(sounds.size(), 0);
        client.setVoiceGuild("100");
        own.remove("session_id"); client.dispatch("VOICE_STATE_UPDATE", own);
        QVERIFY(!client.m_voiceSnapshots.contains("100")); QCOMPARE(sounds.size(), 0);
        own.insert("session_id", "fixture-session"); client.dispatch("VOICE_STATE_UPDATE", own);
        QVERIFY(client.m_voiceSnapshots.contains("100")); QCOMPARE(sounds.size(), 0);
        QJsonObject other{{"guild_id", "100"}, {"user_id", "51"}, {"channel_id", "201"}};
        client.dispatch("VOICE_STATE_UPDATE", other);
        QCOMPARE(sounds.size(), 1); QCOMPARE(sounds.last().first().toString(), "user-join");
        other.insert("self_video", true); client.dispatch("VOICE_STATE_UPDATE", other);
        QCOMPARE(sounds.size(), 2); QCOMPARE(sounds.last().first().toString(), "stream-start");
        other.insert("channel_id", QJsonValue::Null); client.dispatch("VOICE_STATE_UPDATE", other);
        QCOMPARE(sounds.size(), 3); QCOMPARE(sounds.last().first().toString(), "user-leave");
    }
    void endedWatchedStreamStopIsIdempotent() {
        DiscordClient client; setup(client);
        client.acceptVoiceState({{"guild_id", "100"}, {"channel_id", "201"}, {"user_id", "42"}, {"session_id", "fixture-session"}});
        const QString key = "guild:100:201:51";
        QVERIFY(client.watchStream(key));
        QSignalSpy deleted(&client, &DiscordClient::streamDeleted);
        connect(&client, &DiscordClient::streamDeleted, &client, [&](const QJsonObject &event) {
            QVERIFY(client.m_watchingStream.isEmpty());
            // An explicit stop during the ended notification cancels the
            // retained follow intent exactly once, including reentrant calls.
            client.stopWatchingStream(event.value("stream_key").toString());
        });
        client.dispatch("STREAM_DELETE", {{"stream_key", key}});
        QCOMPARE(deleted.size(), 2); QVERIFY(client.m_watchingStream.isEmpty());
        QVERIFY(deleted.last().first().toJsonObject().value("relay_cancelled").toBool());
        QVERIFY(client.m_desiredStream.isEmpty());
        client.dispatch("STREAM_DELETE", {{"stream_key", key}}); client.stopWatchingStream(key);
        QCOMPARE(deleted.size(), 2);
    }
    void ownBroadcastRoutingIsIndependentOfWatchedStream() {
        DiscordClient client; setup(client);
        QVERIFY(!client.startBroadcast("100", "201"));
        client.acceptVoiceState({{"guild_id", "100"}, {"channel_id", "201"}, {"user_id", "42"}, {"session_id", "fixture-session"}});
        QVERIFY(client.startBroadcast("100", "201"));
        QVERIFY(client.watchStream("guild:100:201:51"));
        QSignalSpy ownCreate(&client, &DiscordClient::broadcastCreated), ownServer(&client, &DiscordClient::broadcastServerReceived),
            ownDelete(&client, &DiscordClient::broadcastDeleted), watchedCreate(&client, &DiscordClient::streamCreated);
        client.dispatch("STREAM_CREATE", {{"stream_key", "guild:100:201:42"}, {"rtc_server_id", "123"}});
        client.dispatch("STREAM_SERVER_UPDATE", {{"stream_key", "guild:100:201:42"}, {"endpoint", "fixture.discord.media"}, {"token", "fixture-only"}});
        QCOMPARE(ownCreate.size(), 1); QCOMPARE(ownServer.size(), 1); QCOMPARE(watchedCreate.size(), 0);
        client.dispatch("STREAM_CREATE", {{"stream_key", "guild:100:201:51"}, {"rtc_server_id", "124"}});
        QCOMPARE(watchedCreate.size(), 1); QCOMPARE(ownCreate.size(), 1);
        client.stopBroadcast(); QCOMPARE(ownDelete.size(), 1);
        QCOMPARE(client.m_watchingStream, "guild:100:201:51"); QVERIFY(client.m_broadcastStream.isEmpty());
    }
    void streamViewerMarkersFollowSnapshotsAndVoicePresence() {
        DiscordClient client; setup(client);
        for (const auto &id : {"42", "51", "52"})
            client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "100"}, {"channel_id", "201"},
                {"user_id", id}, {"self_stream", id == QString("42")}});
        const QString key = "guild:100:201:42";
        auto watching = [&](const QString &id) { return client.voiceMembers()->byId(id).value("watchingYourStream").toBool(); };
        QVERIFY(!watching("51"));
        client.dispatch("STREAM_CREATE", {{"stream_key", key}, {"viewer_ids", QJsonArray{"51", "51", "42", "invalid"}}, {"token", "fixture-only"}});
        QVERIFY(watching("51")); QVERIFY(!watching("42")); QVERIFY(!watching("52"));
        QVERIFY(!client.m_streamObjects.value(key).contains("token"));
        QCOMPARE(client.m_streamObjects.value(key).value("viewer_ids").toArray().size(), 2);
        client.dispatch("STREAM_UPDATE", {{"stream_key", key}, {"paused", true}});
        QVERIFY(watching("51"));
        client.dispatch("STREAM_UPDATE", {{"stream_key", "guild:100:201:52"}, {"viewer_ids", QJsonArray{"52"}}});
        QVERIFY(!watching("52"));
        client.dispatch("STREAM_UPDATE", {{"stream_key", key}, {"viewer_ids", QJsonArray{"52"}}});
        QVERIFY(!watching("51")); QVERIFY(watching("52"));
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "100"}, {"channel_id", "202"}, {"user_id", "52"}});
        QVERIFY(!watching("52"));
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "100"}, {"channel_id", "201"}, {"user_id", "52"}});
        QVERIFY(!watching("52")); // Rejoining voice is not rejoining the stream.
        client.dispatch("STREAM_UPDATE", {{"stream_key", key}, {"viewer_ids", QJsonArray{}}});
        QVERIFY(!watching("51"));
        client.dispatch("STREAM_UPDATE", {{"stream_key", key}, {"viewer_ids", QJsonArray{"51"}}});
        QVERIFY(watching("51"));
        client.dispatch("STREAM_DELETE", {{"stream_key", key}}); QVERIFY(!watching("51"));
        client.dispatch("STREAM_CREATE", {{"stream_key", key}, {"viewer_ids", QJsonArray{"51"}}});
        QVERIFY(watching("51"));
        client.m_broadcastStream = key; client.stopBroadcast(); QVERIFY(!watching("51"));
        client.dispatch("STREAM_CREATE", {{"stream_key", key}, {"viewer_ids", QJsonArray{"51"}}});
        client.dispatch("VOICE_STATE_UPDATE", {{"guild_id", "100"}, {"channel_id", "201"}, {"user_id", "42"}, {"self_stream", false}});
        QVERIFY(!watching("51"));
    }
    void notificationActivationUsesKnownChannelsOnly() {
        DiscordClient client; setup(client);
        client.acceptDm({{"id", "301"}, {"type", 1}, {"recipients", QJsonArray{user("55")}}});
        client.activateChannel("301"); QVERIFY(client.currentGuildId().isEmpty()); QCOMPARE(client.currentChannelId(), "301");
        client.activateChannel("102"); QCOMPARE(client.currentGuildId(), "100"); QCOMPARE(client.currentChannelId(), "102");
        client.activateChannel("99999"); QCOMPARE(client.currentChannelId(), "102");
    }
    void directMessageAvatarsAndStatusOnlyUseActualPresence() {
        DiscordClient client; setup(client);
        client.acceptDm({{"id", "301"}, {"type", 1}, {"recipients", QJsonArray{user("55", "Sender")}}});
        QCOMPARE(client.dms()->byId("301").value("status").toString(), "unknown");
        QVERIFY(client.dms()->byId("301").value("activity").toString().isEmpty());
        QVERIFY(client.dms()->byId("301").value("avatarUrl").toString().contains("size=4096"));
        client.dispatch("PRESENCE_UPDATE", {{"user", user("55")}, {"status", "online"},
            {"activities", QJsonArray{QJsonObject{{"type", 0}, {"name", "Fixture game"}}}}});
        QCOMPARE(client.dms()->byId("301").value("status").toString(), "online");
        QCOMPARE(client.dms()->byId("301").value("activity").toString(), "Playing Fixture game");
        client.acceptVoiceState({{"guild_id", "100"}, {"channel_id", "201"}, {"user_id", "55"}, {"self_stream", true}});
        QVERIFY(client.dms()->byId("301").value("inVoice").toBool());
        QVERIFY(client.dms()->byId("301").value("streaming").toBool());
        client.acceptDm({{"id", "302"}, {"type", 3}, {"name", "Group"}, {"icon", "group_icon"}});
        QVERIFY(client.dms()->byId("302").value("avatarUrl").toString().contains("/channel-icons/302/group_icon.png?size=4096"));
        QCOMPARE(client.dms()->byId("302").value("status").toString(), "unknown");
    }
    void explicitVoiceChatAndInviteHaveSeparateSideEffects() {
        DiscordClient client; setup(client);
        client.channels()->upsert({{"recordId", "201"}, {"name", "Lounge"}, {"kind", "voice"}, {"guildId", "100"}});
        client.openVoiceChat("201"); QCOMPARE(client.currentChannelId(), "201");
        QVERIFY(client.currentVoiceSessionId().isEmpty()); QVERIFY(client.m_queue.isEmpty());
        QVERIFY(!client.createVoiceInvite("201")); QVERIFY(!client.inviteError().isEmpty());
        client.m_token = "fixture-only-token-not-real";
        client.m_globalReset = QDateTime::currentMSecsSinceEpoch() + 60000; // Hold request; no socket is opened.
        QVERIFY(client.createVoiceInvite("201")); QVERIFY(client.inviteBusy());
        QVERIFY(!client.createVoiceInvite("201")); QCOMPARE(client.m_queue.size(), 1);
        const auto job = client.m_queue.dequeue();
        QCOMPARE(job->method, QByteArray("POST")); QCOMPARE(job->path, "/channels/201/invites");
        QCOMPARE(job->body.value("max_age").toInt(), 86400); QCOMPARE(job->body.value("max_uses").toInt(), 0);
        QVERIFY(!job->body.value("temporary").toBool()); QVERIFY(!job->body.value("unique").toBool());
        job->callback(QJsonObject{{"code", "fixture_Code-1"}}, {});
        QCOMPARE(client.inviteUrl(), "https://discord.gg/fixture_Code-1"); QVERIFY(!client.inviteBusy());
        QVERIFY(client.createVoiceInvite("201"));
        client.m_queue.dequeue()->callback(QJsonObject{{"code", "../not-a-link"}}, {});
        QVERIFY(client.inviteUrl().isEmpty()); QVERIFY(!client.inviteError().isEmpty());
    }
    void cachedProfilesContainHonestMetadataAndExistingDmDoesNotCreateAnother() {
        DiscordClient client; setup(client);
        auto record = member("51", "Profile");
        auto profile = record.value("user").toObject(); profile.insert("bio", "Known bio"); profile.insert("banner", "known_banner");
        record.insert("user", profile); client.cacheMember("100", record); client.rebuildMembers();
        const auto info = client.memberInfo("51");
        QCOMPARE(info.value("username").toString(), "Profile"); QCOMPARE(info.value("bio").toString(), "Known bio");
        QVERIFY(info.value("bannerUrl").toString().contains("size=4096"));
        QCOMPARE(info.value("roleNames").toStringList(), QStringList{"Helpers"});
        client.acceptDm({{"id", "301"}, {"type", 1}, {"recipients", QJsonArray{profile}}});
        client.openDirectMessage("51"); QCOMPARE(client.currentChannelId(), "301");
        QVERIFY(client.m_queue.isEmpty());
    }
    void serverTagsOnlyReflectEnabledRealPrimaryGuildMetadata() {
        DiscordClient client; setup(client);
        auto person = user("51");
        person.insert("primary_guild", QJsonObject{{"identity_guild_id", "999"}, {"identity_enabled", true}, {"tag", "AoC"}, {"badge", "fixture_badge"}});
        auto tag = client.primaryGuild(person);
        QCOMPARE(tag.value("tag").toString(), "AoC");
        QVERIFY(tag.value("badgeUrl").toString().contains("/guild-tag-badges/999/fixture_badge.png"));
        QVERIFY(tag.value("name").toString().isEmpty()); QVERIFY(!tag.value("joined").toBool());
        QVERIFY(!client.guildInfo("999").value("countKnown").toBool()); QVERIFY(!client.navigateGuild("999"));
        auto primary = person.value("primary_guild").toObject(); primary.insert("identity_enabled", false); person.insert("primary_guild", primary);
        QVERIFY(client.primaryGuild(person).isEmpty());
        primary.insert("identity_enabled", true); primary.insert("identity_guild_id", "100"); person.insert("primary_guild", primary);
        tag = client.primaryGuild(person); QCOMPARE(tag.value("name").toString(), "Fixture server"); QVERIFY(tag.value("joined").toBool());
        auto msg = message("901"); msg.insert("author", person);
        QCOMPARE(client.messageRow(msg).value("primaryGuild").toMap().value("tag").toString(), "AoC");
    }    void dmMembersContainOnlyActualRecipientsAndDeviceFlags() {
        DiscordClient client; client.m_currentUserId="42";
        client.acceptDm({{"id","301"},{"type",1},{"recipients",QJsonArray{user("55","Sender")}}});
        client.selectGuild({}); client.selectChannel("301");
        QCOMPARE(client.members()->count(),1); QCOMPARE(client.members()->get(0).value("recordId").toString(),"55");
        QVERIFY(!client.membersPartial()); QCOMPARE(client.memberCount(),1);
        client.dispatch("PRESENCE_UPDATE",{{"user",user("55")},{"status","dnd"},
            {"client_status",QJsonObject{{"desktop","online"},{"mobile","dnd"},{"web","nonsense"},{"other","online"}}}});
        auto row=client.dms()->byId("301");
        QCOMPARE(row.value("desktopStatus").toString(),"online"); QCOMPARE(row.value("mobileStatus").toString(),"dnd");
        QVERIFY(row.value("webStatus").toString().isEmpty());
        QCOMPARE(client.members()->get(0).value("mobileStatus").toString(),"dnd");
        client.dispatch("RELATIONSHIP_ADD",{{"id","55"},{"type",2}});
        QVERIFY(client.dms()->byId("301").value("blocked").toBool());
        QVERIFY(client.memberInfo("55").value("blocked").toBool());
        client.acceptNotificationSettings({{"guild_id",QJsonValue::Null},{"channel_overrides",QJsonArray{QJsonObject{{"channel_id","301"},{"muted",true}}}}});
        QVERIFY(client.dms()->byId("301").value("conversationMuted").toBool());
        QVERIFY(!client.dms()->byId("301").value("muted").toBool());
        client.acceptVoiceState({{"guild_id","100"},{"user_id","55"},{"channel_id","201"},{"self_mute",true},{"self_deaf",true}});
        QVERIFY(client.dms()->byId("301").value("muted").toBool()); QVERIFY(client.dms()->byId("301").value("deafened").toBool());
        client.dispatch("RELATIONSHIP_REMOVE",{{"id","55"}}); QVERIFY(!client.dms()->byId("301").value("blocked").toBool());
        client.dispatch("PRESENCE_UPDATE",{{"user",user("55")},{"status","offline"}});
        QVERIFY(client.dms()->byId("301").value("mobileStatus").toString().isEmpty());
    }
    void explicitProfilesKeepDisplayFieldsOnlyAndCacheIsBounded() {
        DiscordClient client; setup(client); client.cacheMember("100",member("51","Profile")); client.rebuildMembers();
        const auto created=client.memberInfo("51").value("createdAt").toString();
        QCOMPARE(created,"2015-01-01T00:00:00Z");
        QVERIFY(!client.memberInfo("51").contains("mutualFriendCount"));
        auto raw=user("51","Profile"); raw.insert("bio","Global bio"); raw.insert("email","not-retained@example.invalid");
        QJsonObject profile{{"user",raw},{"user_profile",QJsonObject{{"bio","Global about"},{"pronouns","they/them"},{"banner","global-banner"}}},
            {"guild_member_profile",QJsonObject{{"bio","Server about"},{"banner","guild_banner"}}},
            {"mutual_guilds",QJsonArray{QJsonObject{{"id","100"}},QJsonObject{{"id","999"}}}},{"mutual_friends_count",3}};
        client.acceptProfile("51","100",profile);
        auto info=client.memberInfo("51"); QCOMPARE(info.value("bio").toString(),"Server about");
        QCOMPARE(info.value("pronouns").toString(),"they/them"); QCOMPARE(info.value("mutualFriendCount").toInt(),3);
        QCOMPARE(info.value("mutualGuildCount").toInt(),2); QCOMPARE(info.value("mutualGuilds").toList()[0].toMap().value("name").toString(),"Fixture server");
        QVERIFY(info.value("mutualGuilds").toList()[1].toMap().value("name").toString().isEmpty());
        QVERIFY(info.value("bannerUrl").toString().contains("/guilds/100/users/51/banners/guild_banner.png"));
        QVERIFY(!client.m_users.value("51").contains("email"));
        QSignalSpy updated(&client,&DiscordClient::profileUpdated); client.fetchProfile("51"); QCOMPARE(updated.count(),1); QVERIFY(client.m_queue.isEmpty());
        for(int i=0;i<160;++i) { auto next=profile; next.insert("user",user(QString::number(1000+i))); client.acceptProfile(QString::number(1000+i),{},next); }
        QVERIFY(client.m_profiles.size()<=128); QVERIFY(client.m_profileFetchedAt.size()<=128);
        client.logout(); QVERIFY(client.m_profiles.isEmpty());
    }
    void privateChannelLockUsesEveryonePermissionsWithoutHidingRoleAccess() {
        DiscordClient client; setup(client);
        QJsonObject channel{{"id","101"},{"type",0},{"permission_overwrites",QJsonArray{
            QJsonObject{{"id","100"},{"type",0},{"deny","1024"},{"allow","0"}},
            QJsonObject{{"id","900"},{"type",0},{"deny","0"},{"allow","1024"}}}}};
        QVERIFY(client.channelRow(channel,"100").value("locked").toBool());
        QVERIFY(client.channelVisible(channel,"100"));
        channel.insert("permission_overwrites",QJsonArray{}); QVERIFY(!client.channelRow(channel,"100").value("locked").toBool());
        channel.insert("type",2); QVERIFY(client.channelRow(channel,"100").value("locked").toBool());
        QVERIFY(!client.channelRow(channel,"999").value("locked").toBool());
    }
    void voiceElapsedTimeUsesOnlyActualServerTimestamp() {
        DiscordClient client; setup(client);
        client.dispatch("CHANNEL_CREATE",{{"id","201"},{"guild_id","100"},{"name","Voice"},{"type",2}});
        QCOMPARE(client.channels()->byId("201").value("voiceStartedAt").toLongLong(),0LL);
        client.acceptVoiceState({{"guild_id","100"},{"user_id","55"},{"channel_id","201"}});
        QCOMPARE(client.channels()->byId("201").value("voiceStartedAt").toLongLong(),0LL);
        const qint64 started=QDateTime::currentSecsSinceEpoch()-3600;
        client.dispatch("CHANNEL_INFO",{{"guild_id","100"},{"channels",QJsonArray{QJsonObject{{"id","201"},{"voice_start_time",started}}}}});
        QCOMPARE(client.channels()->byId("201").value("voiceStartedAt").toLongLong(),started*1000);
        client.dispatch("VOICE_CHANNEL_START_TIME_UPDATE",{{"guild_id","100"},{"id","201"},{"voice_start_time",QJsonValue::Null}});
        QCOMPARE(client.channels()->byId("201").value("voiceStartedAt").toLongLong(),0LL);
        client.dispatch("VOICE_CHANNEL_START_TIME_UPDATE",{{"guild_id","100"},{"id","201"},{"voice_start_time",QDateTime::currentSecsSinceEpoch()+100000}});
        QCOMPARE(client.channels()->byId("201").value("voiceStartedAt").toLongLong(),0LL);
    }

};

QTEST_GUILESS_MAIN(MemberBackendTest)
#include "member_backend_test.moc"
