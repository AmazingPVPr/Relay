#include "ChannelTools.h"
#include "DiscordClient.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>
#include <functional>

namespace {
class ChannelHttpFixture final : public QTcpServer {
public:
    struct Response { int status = 200; QByteArray body = "{}"; int delay = 0; };
    QList<QByteArray> requests;
    std::function<Response(const QByteArray &, const QUrl &)> respond;
    ChannelHttpFixture() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    auto bytes = socket->property("bytes").toByteArray() + socket->readAll();
                    socket->setProperty("bytes", bytes);
                    const auto end = bytes.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    int contentLength = 0;
                    for (const auto &header : bytes.left(end).split('\n'))
                        if (header.toLower().startsWith("content-length:")) contentLength = header.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + contentLength) return;
                    socket->setProperty("handled", true);
                    requests.append(bytes);
                    const auto parts = bytes.left(bytes.indexOf("\r\n")).split(' ');
                    const auto response = respond ? respond(parts.value(0), QUrl::fromEncoded(parts.value(1))) : Response{};
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
QJsonObject message(QString id, QString channel = "101", QString content = "Fixture message") {
    return {{"id", id}, {"channel_id", channel}, {"content", content}, {"timestamp", "2026-10-21T10:00:00.000Z"},
            {"author", QJsonObject{{"id", "42"}, {"username", "Fixture"}}}};
}
QByteArray json(const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QByteArray json(const QJsonArray &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
QJsonObject pin(QJsonObject value, QString at = "2026-10-21T10:00:00.123Z") { return {{"message", value}, {"pinned_at", at}}; }
}

class ChannelToolsTest final : public QObject {
    Q_OBJECT
    static void configure(DiscordClient &client, const ChannelHttpFixture &fixture) {
        client.m_apiBase = fixture.base();
        client.m_token = "synthetic-local-fixture-token";
        client.m_currentUserId = "42";
        client.m_connected = true;
        client.m_currentGuildId = "100";
        client.m_currentGuildName = "Fixture server";
        client.m_currentChannelId = "101";
        client.m_currentChannelName = "general";
        client.m_currentChannelKind = "text";
        client.m_guilds.replace({{{"recordId", "100"}, {"name", "Fixture server"}}, {{"recordId", "200"}, {"name", "Second server"}}});
        client.m_channelCache.insert("100", QJsonArray{QJsonObject{{"id", "101"}, {"name", "general"}, {"type", 0}}, QJsonObject{{"id", "102"}, {"name", "other"}, {"type", 0}}});
        client.m_channelCache.insert("200", QJsonArray{QJsonObject{{"id", "201"}, {"name", "second"}, {"type", 0}}});
        client.rebuildChannels(false);
    }
private slots:

    void messageActionsValidateOwnershipAndUseConfirmedResponses() {
        ChannelHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](const QByteArray &method, const QUrl &url) {
            if (method == "PATCH") return ChannelHttpFixture::Response{200, json(message("500", "101", "Updated"))};
            if (method == "POST") return ChannelHttpFixture::Response{200, json(message("501", "101", "Reply"))};
            if (url.path().contains("/reactions/")) return ChannelHttpFixture::Response{204, ""};
            return ChannelHttpFixture::Response{204, ""};
        };
        DiscordClient client; configure(client, fixture); ChannelTools tools(&client);
        client.acceptMessage(message("500"));
        QSignalSpy finished(&tools, &ChannelTools::messageActionFinished);
        QVERIFY(!tools.messageAction("edit", "102", "500", "Wrong channel"));
        QVERIFY(!tools.messageAction("edit", "101", "500", ""));
        QVERIFY(!tools.messageAction("react", "101", "500", "../invalid"));
        QVERIFY(tools.messageAction("edit", "101", "500", "Updated"));
        QVERIFY(!tools.messageAction("delete", "101", "500"));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(client.m_messages.byId("500").value("content").toString(), QString("Updated"));
        const auto patch = fixture.requests.first();
        QVERIFY(patch.startsWith("PATCH /channels/101/messages/500 "));
        auto body = QJsonDocument::fromJson(patch.mid(patch.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(body.value("content").toString(), QString("Updated"));
        QVERIFY(body.value("allowed_mentions").toObject().value("parse").toArray().isEmpty());
        QVERIFY(tools.messageAction("reply", "101", "500", "Reply"));
        QTRY_COMPARE(finished.size(), 2);
        const auto post = fixture.requests.last();
        body = QJsonDocument::fromJson(post.mid(post.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(body.value("message_reference").toObject().value("message_id").toString(), QString("500"));
        QVERIFY(!body.value("allowed_mentions").toObject().value("replied_user").toBool());
        auto other = message("600"); other.insert("author", QJsonObject{{"id", "77"}});
        client.acceptMessage(other);
        QVERIFY(!tools.messageAction("delete", "101", "600"));
        QVERIFY(tools.messageAction("delete", "101", "500"));
        QTRY_COMPARE(finished.size(), 3);
        QVERIFY(client.m_messages.byId("500").isEmpty());
        QVERIFY(client.m_messages.byId("501").value("replyDeleted").toBool() == false);
    }


    void reactionRequestsAndForwardFailuresPreserveMessages() {
        ChannelHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](const QByteArray &, const QUrl &url) {
            if (url.path().contains("/reactions/")) return ChannelHttpFixture::Response{204, ""};
            return ChannelHttpFixture::Response{403, "{\"message\":\"Missing Permissions\",\"code\":50013}"};
        };
        DiscordClient client; configure(client, fixture); ChannelTools tools(&client);
        client.acceptMessage(message("500"));
        QSignalSpy finished(&tools, &ChannelTools::messageActionFinished);
        QVERIFY(tools.messageAction("react", "101", "500", "👍"));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(fixture.requests.first().startsWith("PUT /channels/101/messages/500/reactions/%F0%9F%91%8D/@me "));
        QVERIFY(tools.messageAction("unreact", "101", "500", "custom:123"));
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(fixture.requests.last().startsWith("DELETE /channels/101/messages/500/reactions/custom%3A123/@me "));
        QVERIFY(tools.messageAction("forward", "101", "500", "", "102"));
        QTRY_VERIFY(!tools.actionBusy());
        QVERIFY(!tools.actionError().isEmpty());
        QCOMPARE(finished.size(), 2);
        QVERIFY(!client.m_messages.byId("500").isEmpty());
        const auto post = fixture.requests.last();
        const auto body = QJsonDocument::fromJson(post.mid(post.indexOf("\r\n\r\n") + 4)).object();
        QCOMPARE(body.value("message_reference").toObject().value("type").toInt(), 1);
        QCOMPARE(body.value("message_reference").toObject().value("channel_id").toString(), QString("101"));
    }

    void reactionEventsTrackCountsAndOwnSelection() {
        DiscordClient client;
        client.m_currentChannelId = "101"; client.m_currentUserId = "42";
        client.acceptMessage(message("500"));
        QJsonObject event{{"channel_id", "101"}, {"message_id", "500"}, {"user_id", "42"},
            {"emoji", QJsonObject{{"id", QJsonValue::Null}, {"name", "👍"}}}};
        client.dispatch("MESSAGE_REACTION_ADD", event);
        auto reactions = client.m_messages.byId("500").value("reactions").toList();
        QCOMPARE(reactions.size(), 1); QCOMPARE(reactions.first().toMap().value("count").toInt(), 1);
        QVERIFY(reactions.first().toMap().value("me").toBool());
        event.insert("user_id", "77"); client.dispatch("MESSAGE_REACTION_ADD", event);
        reactions = client.m_messages.byId("500").value("reactions").toList();
        QCOMPARE(reactions.first().toMap().value("count").toInt(), 2);
        event.insert("user_id", "42"); client.dispatch("MESSAGE_REACTION_REMOVE", event);
        reactions = client.m_messages.byId("500").value("reactions").toList();
        QCOMPARE(reactions.first().toMap().value("count").toInt(), 1);
        QVERIFY(!reactions.first().toMap().value("me").toBool());
        client.dispatch("MESSAGE_REACTION_REMOVE_EMOJI", event);
        QVERIFY(client.m_messages.byId("500").value("reactions").toList().isEmpty());
    }


    void autocompleteFindsUncachedMembersAndIgnoresOldScopes() {
        ChannelHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](const QByteArray &, const QUrl &url) {
            if (!url.path().endsWith("/members/search")) return ChannelHttpFixture::Response{};
            return ChannelHttpFixture::Response{200, "[{\"user\":{\"id\":\"77\",\"username\":\"newperson\"},\"nick\":\"New Nick\"}]", 30};
        };
        DiscordClient client; configure(client, fixture); ChannelTools tools(&client);
        QSignalSpy updated(&tools, &ChannelTools::searchSuggestionsChanged);
        tools.requestSearchSuggestions("from:new", 8);
        QTRY_COMPARE_WITH_TIMEOUT(updated.size(), 1, 2000);
        QVERIFY(client.m_users.contains("77"));
        const auto choices = tools.searchSuggestions("from:\"New Nick\"", 15);
        QVERIFY(!choices.isEmpty());
        QCOMPARE(choices.first().toMap().value("label").toString(), QString("New Nick"));
        const QUrlQuery query(QUrl::fromEncoded(fixture.requests.first().split(' ').at(1)));
        QCOMPARE(query.queryItemValue("query"), QString("new"));
        tools.requestSearchSuggestions("mentions:other", 14);
        client.m_currentGuildId = "200";
        QTest::qWait(100);
        QCOMPARE(updated.size(), 1);
    }

    void smartSearchFiltersAndCompletion() {
        ChannelHttpFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](const QByteArray &, const QUrl &) { return ChannelHttpFixture::Response{200, "{\"total_results\":0,\"messages\":[]}"}; };
        DiscordClient client; configure(client, fixture);
        client.m_users.insert("42", QJsonObject{{"id", "42"}, {"username", "alice"}, {"global_name", "Alice Example"}});
        ChannelTools tools(&client);
        const auto filters = tools.searchSuggestions("fro", 3);
        QCOMPARE(filters.size(), 1);
        QCOMPARE(filters.first().toMap().value("text").toString(), QString("from:"));
        const auto users = tools.searchSuggestions("hello from:ali has:image", 14);
        QVERIFY(!users.isEmpty());
        QCOMPARE(users.first().toMap().value("text").toString(), QString("hello from:alice  has:image"));
        QVERIFY(tools.searchMessages("hello from:alice mentions:me in:general has:image has:-video pinned:true after:2026-01-01 before:2026-03-01 author:user sort:oldest"));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.searchBusy(), 2000);
        const auto url = QUrl::fromEncoded(fixture.requests.last().split(' ').at(1));
        const QUrlQuery query(url);
        QCOMPARE(query.queryItemValue("content"), QString("hello"));
        QCOMPARE(query.queryItemValue("author_id"), QString("42"));
        QCOMPARE(query.queryItemValue("mentions"), QString("42"));
        QCOMPARE(query.queryItemValue("channel_id"), QString("101"));
        QCOMPARE(query.allQueryItemValues("has"), QStringList({"image", "-video"}));
        QCOMPARE(query.queryItemValue("sort_order"), QString("asc"));
        QVERIFY(query.hasQueryItem("min_id")); QVERIFY(query.hasQueryItem("max_id"));
        QVERIFY(tools.searchMessages("from:\"Alice Example\" during:2026-02"));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.searchBusy(), 2000);
        const QUrlQuery dates(QUrl::fromEncoded(fixture.requests.last().split(' ').at(1)));
        QVERIFY(!dates.hasQueryItem("content"));
        QCOMPARE(dates.queryItemValue("min_id").toULongLong(), quint64(1769904000000LL - 1420070400000LL) << 22);
        QCOMPARE(dates.queryItemValue("max_id").toULongLong(), quint64(1772323200000LL - 1420070400000LL) << 22);
        const int requests = fixture.requests.size();
        for (const auto &bad : {"from:nobody", "before:2026-02-30", "pinned:maybe", "has:banana", "on:2026-02-01 before:2026-01-01", "from:"}) {
            QVERIFY2(!tools.searchMessages(bad), bad); QVERIFY(!tools.searchError().isEmpty());
        }
        QCOMPARE(fixture.requests.size(), requests);
        client.m_users.insert("43", QJsonObject{{"id", "43"}, {"username", "bob"}, {"global_name", "Alice Example"}});
        QVERIFY(!tools.searchMessages("from:\"Alice Example\""));
        QVERIFY(tools.searchError().contains("Several"));
    }

    void pinsUseCurrentEndpointCursorAndDeduplicate() {
        ChannelHttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](const QByteArray &, const QUrl &url) {
            const QUrlQuery query(url);
            const bool next = query.hasQueryItem("before");
            const auto items = next ? QJsonArray{pin(message("500")), pin(message("499"), "2026-10-20T10:00:00.123Z"), pin(message("777", "999"))} : QJsonArray{pin(message("500"))};
            return ChannelHttpFixture::Response{200, json(QJsonObject{{"items", items}, {"has_more", !next}})};
        };
        DiscordClient client; configure(client, fixture);
        ChannelTools tools(&client);
        QVERIFY(tools.readPins("101"));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.pinsBusy(), 2000);
        QVERIFY2(tools.pinsError().isEmpty(), qPrintable(tools.pinsError()));
        QCOMPARE(tools.pins()->count(), 1);
        QVERIFY(tools.pinsHasMore());
        QCOMPARE(tools.pins()->get(0).value("pinned").toBool(), true);
        QCOMPARE(tools.pins()->get(0).value("channelName").toString(), QStringLiteral("general"));
        tools.loadMorePins();
        QTRY_VERIFY_WITH_TIMEOUT(!tools.pinsBusy(), 2000);
        QCOMPARE(tools.pins()->count(), 2);
        QVERIFY(!tools.pinsHasMore());
        QCOMPARE(fixture.requests.size(), 2);
        QVERIFY(fixture.requests.first().startsWith("GET /channels/101/messages/pins?limit=50 "));
        const QUrlQuery query(QUrl::fromEncoded(fixture.requests.last().split(' ').at(1)));
        QCOMPARE(query.queryItemValue("before", QUrl::FullyDecoded), QStringLiteral("2026-10-21T10:00:00.123Z"));
    }

    void replacedPinsAndLogoutIgnoreStaleResponses() {
        ChannelHttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](const QByteArray &, const QUrl &url) {
            const auto channel = url.path().section('/', 2, 2);
            return ChannelHttpFixture::Response{200, json(QJsonObject{{"items", QJsonArray{pin(message(channel == "101" ? "500" : "600", channel))}}, {"has_more", false}}), channel == "101" ? 150 : 0};
        };
        DiscordClient client; configure(client, fixture);
        ChannelTools tools(&client);
        QVERIFY(tools.readPins("101"));
        QVERIFY(tools.readPins("102"));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.pinsBusy(), 2000);
        QCOMPARE(tools.pins()->get(0).value("recordId").toString(), QStringLiteral("600"));
        QTest::qWait(180);
        QCOMPARE(tools.pins()->count(), 1);
        QCOMPARE(tools.pins()->get(0).value("recordId").toString(), QStringLiteral("600"));
        QVERIFY(tools.readPins("101"));
        client.logout();
        QVERIFY(!tools.pinsBusy());
        QVERIFY(tools.pinsChannelId().isEmpty());
        QTest::qWait(180);
        QCOMPARE(tools.pins()->count(), 0);
    }

    void searchRetriesIndexingAndPaginatesByRequestedLimit() {
        ChannelHttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        int calls = 0;
        fixture.respond = [&](const QByteArray &, const QUrl &url) {
            ++calls;
            if (calls == 1) return ChannelHttpFixture::Response{202, json(QJsonObject{{"code", 110000}, {"retry_after", 0.001}})};
            const int offset = QUrlQuery(url).queryItemValue("offset").toInt();
            auto hit = message(offset ? "499" : "500", "101");
            hit.insert("hit", true);
            auto context = message("999", "101", "Not a hit"); context.insert("hit", false);
            return ChannelHttpFixture::Response{200, json(QJsonObject{{"messages", QJsonArray{QJsonArray{context, hit}}}, {"total_results", 28}})};
        };
        DiscordClient client; configure(client, fixture);
        ChannelTools tools(&client);
        const auto queryText = QString::fromUtf8("words... + percent%2B & ñ");
        QVERIFY(tools.searchMessages(queryText));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.searchBusy(), 3000);
        QCOMPARE(calls, 2);
        QCOMPARE(tools.searchResults()->count(), 1);
        QCOMPARE(tools.totalResults(), 28);
        QVERIFY(tools.searchHasMore());
        QCOMPARE(tools.searchScope(), QStringLiteral("Fixture server"));
        const auto target = QUrl::fromEncoded(fixture.requests.last().split(' ').at(1));
        QCOMPARE(target.path(), QStringLiteral("/guilds/100/messages/search"));
        QCOMPARE(QUrlQuery(target).queryItemValue("content", QUrl::FullyDecoded), queryText);
        tools.loadMoreSearch();
        QTRY_VERIFY_WITH_TIMEOUT(!tools.searchBusy(), 2000);
        QCOMPARE(tools.searchResults()->count(), 2);
        QVERIFY(!tools.searchHasMore());
        QCOMPARE(QUrlQuery(QUrl::fromEncoded(fixture.requests.last().split(' ').at(1))).queryItemValue("offset"), QStringLiteral("25"));
    }

    void directSearchAndCancelledIndexRetryStayScoped() {
        ChannelHttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](const QByteArray &, const QUrl &) { return ChannelHttpFixture::Response{202, json(QJsonObject{{"code", 110000}, {"retry_after", 0}})}; };
        DiscordClient client; configure(client, fixture);
        client.m_currentGuildId.clear(); client.m_currentChannelId = "301"; client.m_currentChannelName = "Fixture DM";
        ChannelTools tools(&client);
        QVERIFY(tools.searchMessages("Hello"));
        QTRY_VERIFY_WITH_TIMEOUT(tools.searchStatus().contains("index"), 2000);
        QCOMPARE(fixture.requests.size(), 1);
        QVERIFY(fixture.requests.first().startsWith("GET /channels/301/messages/search?"));
        tools.clearSearch();
        QTest::qWait(350);
        QCOMPARE(fixture.requests.size(), 1);
        QVERIFY(!tools.searchBusy());
        QVERIFY(tools.searchQuery().isEmpty());
        fixture.respond = [](const QByteArray &, const QUrl &) { return ChannelHttpFixture::Response{202, json(QJsonObject{{"code", 110000}, {"retry_after", 60}})}; };
        QVERIFY(tools.searchMessages("Hello"));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.searchBusy(), 2000);
        QVERIFY(tools.searchError().contains("indexing"));
        QCOMPARE(fixture.requests.size(), 2);
    }

    void pinWritesAreExplicitAndPermissionFailurePreservesState() {
        ChannelHttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        int calls = 0;
        fixture.respond = [&](const QByteArray &, const QUrl &) {
            ++calls;
            return calls == 2 ? ChannelHttpFixture::Response{403, json(QJsonObject{{"message", "Missing Permissions"}})} : ChannelHttpFixture::Response{204, {}};
        };
        DiscordClient client; configure(client, fixture);
        client.acceptMessage(message("500"));
        ChannelTools tools(&client);
        QSignalSpy success(&tools, &ChannelTools::pinnedChanged);
        QCOMPARE(fixture.requests.size(), 0);
        QVERIFY(tools.setPinned("101", "500", true));
        QVERIFY(!tools.setPinned("101", "500", false));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.actionBusy(), 2000);
        QCOMPARE(success.size(), 1);
        QVERIFY(client.messages()->byId("500").value("pinned").toBool());
        QVERIFY(client.m_messageObjects.value("500").value("pinned").toBool());
        QVERIFY(fixture.requests.first().startsWith("PUT /channels/101/messages/pins/500 "));
        QVERIFY(tools.setPinned("101", "500", false));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.actionBusy(), 2000);
        QVERIFY(tools.actionError().contains("permission"));
        QVERIFY(client.messages()->byId("500").value("pinned").toBool());
        QCOMPARE(success.size(), 1);
        QVERIFY(tools.setPinned("101", "500", false));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.actionBusy(), 2000);
        QVERIFY(!client.messages()->byId("500").value("pinned").toBool());
        QCOMPARE(success.size(), 2);
        QCOMPARE(fixture.requests.size(), 3);
        QVERIFY(fixture.requests.last().startsWith("DELETE /channels/101/messages/pins/500 "));
    }

    void jumpSelectsGuildAndIgnoresRacingRecentHistory() {
        ChannelHttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](const QByteArray &, const QUrl &url) {
            if (url.path().endsWith("/messages")) {
                if (QUrlQuery(url).hasQueryItem("around"))
                    return ChannelHttpFixture::Response{200, json(QJsonArray{message("501", "201"), message("500", "201"), message("499", "201"), message("888", "999")}), 5};
                return ChannelHttpFixture::Response{200, json(QJsonArray{message("9999", "201", "Racing recent history")}), 160};
            }
            return ChannelHttpFixture::Response{};
        };
        DiscordClient client; configure(client, fixture);
        client.acceptMessage(message("900"));
        ChannelTools tools(&client);
        QSignalSpy opened(&tools, &ChannelTools::messageOpened);
        QVERIFY(tools.openMessage("201", "500"));
        QTRY_COMPARE_WITH_TIMEOUT(opened.size(), 1, 3000);
        QCOMPARE(client.currentGuildId(), QStringLiteral("200"));
        QCOMPARE(client.currentChannelId(), QStringLiteral("201"));
        QCOMPARE(client.messages()->count(), 3);
        QCOMPARE(opened.at(0).at(2).toInt(), 1);
        QVERIFY(!client.loadingMessages());
        QTest::qWait(200);
        QCOMPARE(client.messages()->count(), 3);
        QVERIFY(client.messages()->byId("9999").isEmpty());
        QVERIFY(client.messages()->byId("888").isEmpty());
        QVERIFY(tools.openMessage("201", "500"));
        QCOMPARE(opened.size(), 2);
        QVERIFY(!tools.jumpBusy());
    }

    void unknownChannelLookupAndSelectionChangeDoNotStealNavigation() {
        ChannelHttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](const QByteArray &, const QUrl &url) {
            if (url.path() == "/channels/202")
                return ChannelHttpFixture::Response{200, json(QJsonObject{{"id", "202"}, {"guild_id", "200"}, {"type", 11}, {"name", "thread"}}), 160};
            return ChannelHttpFixture::Response{200, json(QJsonArray{})};
        };
        DiscordClient client; configure(client, fixture);
        ChannelTools tools(&client);
        QSignalSpy opened(&tools, &ChannelTools::messageOpened);
        QVERIFY(tools.openMessage("202", "500"));
        client.selectChannel("102");
        QTest::qWait(220);
        QCOMPARE(client.currentChannelId(), QStringLiteral("102"));
        QVERIFY(!tools.jumpBusy());
        QCOMPARE(opened.size(), 0);
        auto *temporary = new ChannelTools(&client);
        QVERIFY(temporary->readPins("101"));
        delete temporary;
        QTest::qWait(30); // Queued callback must observe the dead QObject guard.
    }

    void replacingHistoryJumpWithMissingChannelReleasesLoading() {
        ChannelHttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](const QByteArray &, const QUrl &url) {
            if (url.path() == "/channels/999") return ChannelHttpFixture::Response{404, json(QJsonObject{{"message", "Unknown Channel"}})};
            return ChannelHttpFixture::Response{200, json(QJsonArray{message("500")}), 160};
        };
        DiscordClient client; configure(client, fixture);
        client.acceptMessage(message("900"));
        ChannelTools tools(&client);
        QSignalSpy opened(&tools, &ChannelTools::messageOpened);
        QVERIFY(tools.openMessage("101", "500"));
        QVERIFY(client.loadingMessages());
        QVERIFY(tools.openMessage("999", "600"));
        QTRY_VERIFY_WITH_TIMEOUT(!tools.jumpBusy(), 2000);
        QVERIFY(!tools.actionError().isEmpty());
        QVERIFY(!client.loadingMessages());
        QTest::qWait(200);
        QCOMPARE(opened.size(), 0);
        QVERIFY(client.messages()->byId("500").isEmpty());
    }
};

QTEST_GUILESS_MAIN(ChannelToolsTest)
#include "channel_tools_test.moc"
