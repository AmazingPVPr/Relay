#include "DiscordClient.h"
#include "RecordModel.h"

#include <QtTest>

// All messages below are local fixtures. No account or remote connection is used.
class MessageRenderTest final : public QObject {
    Q_OBJECT
    static QJsonObject message(int type = 0, const QString &content = {}) {
        return {{"id", "1001"}, {"channel_id", "101"}, {"type", type}, {"content", content},
            {"timestamp", "2026-10-21T12:00:00Z"},
            {"author", QJsonObject{{"id", "42"}, {"username", "Fixture"}, {"global_name", "River"}}}};
    }
    static QJsonObject attachment(const QString &name = "photo.png") {
        return {{"id", "2001"}, {"filename", name}, {"content_type", "image/png"},
            {"size", 1280}, {"width", 800}, {"height", 600},
            {"url", "https://cdn.discordapp.com/attachments/101/2001/" + name},
            {"proxy_url", "https://media.discordapp.net/attachments/101/2001/" + name}};
    }
    static QVariantMap first(const QVariantMap &row, const QString &key) {
        const auto list = row.value(key).toList();
        return list.isEmpty() ? QVariantMap{} : list.first().toMap();
    }
private slots:
    void noticeDescriptions_data() {
        QTest::addColumn<int>("type");
        QTest::addColumn<QString>("expected");
        QTest::newRow("join") << 7 << "River joined the server.";
        QTest::newRow("pin") << 6 << "River pinned a message to this channel.";
        QTest::newRow("boost") << 8 << "River boosted the server.";
        QTest::newRow("boost-level") << 10 << "River boosted the server. It reached Boost Level 2.";
        QTest::newRow("stage-end") << 28 << "River ended the stage.";
    }
    void noticeDescriptions() {
        QFETCH(int, type);
        QFETCH(QString, expected);
        DiscordClient client;
        const auto row = client.messageRow(message(type));
        QCOMPARE(row.value("content").toString(), expected);
        QVERIFY(row.value("systemMessage").toBool());
        QCOMPARE(row.value("messageType").toInt(), type);
    }
    void noticesPreserveRelevantDetails() {
        DiscordClient client;
        QCOMPARE(client.messageRow(message(4, "lounge")).value("content").toString(),
                 "River changed the channel name to “lounge”.");
        auto added = message(1);
        added.insert("mentions", QJsonArray{QJsonObject{{"id", "43"}, {"username", "Lake"}}});
        QCOMPARE(client.messageRow(added).value("content").toString(), "River added Lake to the group.");
        auto removed = message(2);
        removed.insert("mentions", QJsonArray{removed.value("author")});
        QCOMPARE(client.messageRow(removed).value("content").toString(), "River left the group.");
    }
    void repliesAndCommandsWithMediaAreOrdinaryMessages() {
        DiscordClient client;
        for (int type : {0, 19, 20, 23}) {
            auto input = message(type);
            input.insert("attachments", QJsonArray{attachment()});
            const auto row = client.messageRow(input);
            QVERIFY2(row.value("content").toString().isEmpty(), "A media-only message must not gain a fake body.");
            QVERIFY(!row.value("systemMessage").toBool());
            QCOMPARE(row.value("attachments").toList().size(), 1);
        }
    }
    void attachmentPreviewAndSpoilerSafety() {
        DiscordClient client;
        auto input = message();
        auto malicious = attachment();
        malicious.insert("url", "file:///etc/passwd");
        malicious.insert("proxy_url", "https://cdn.discordapp.com.evil.invalid/photo.png");
        auto video = attachment("clip.mp4");
        video.insert("content_type", "video/mp4");
        input.insert("attachments", QJsonArray{attachment("SPOILER_photo.png"), malicious, video});
        const auto row = client.messageRow(input);
        const auto items = row.value("attachments").toList();
        QCOMPARE(items.size(), 2);
        const auto photo = items.first().toMap();
        QVERIFY(photo.value("spoiler").toBool());
        QVERIFY(photo.value("isImage").toBool());
        QCOMPARE(photo.value("width").toInt(), 800);
        QVERIFY(photo.value("previewUrl").toString().startsWith("https://media.discordapp.net/"));
        QVERIFY(items.last().toMap().value("isVideo").toBool());
        QVERIFY(items.last().toMap().value("previewUrl").toString().isEmpty());
    }
    void embedsKeepTextFieldsAndSafeProxyImages() {
        DiscordClient client;
        auto input = message(19);
        input.insert("embeds", QJsonArray{
            QJsonObject{{"title", "A title"}, {"description", "<b>Plain content</b>"},
                {"url", "https://example.org/article"}, {"provider", QJsonObject{{"name", "Example"}}},
                {"fields", QJsonArray{QJsonObject{{"name", "Detail"}, {"value", "Value"}}}},
                {"image", QJsonObject{{"url", "https://third-party.invalid/pixel.png"},
                    {"proxy_url", "https://images-ext-1.discordapp.net/external/fixture/https/example.org/photo.png"}}}},
            QJsonObject{{"image", QJsonObject{{"url", "https://cdn.discordapp.com/attachments/101/2001/image.gif"}}}},
            QJsonObject{{"author", QJsonObject{{"name", "Author-only embed"}}}},
            QJsonObject{{"image", QJsonObject{{"url", "data:image/png;base64,AAAA"}}}},
            QJsonObject{{"title", "Unsafe link"}, {"url", "javascript:alert(1)"},
                        {"thumbnail", QJsonObject{{"url", "https://images-ext-2.discordapp.net.evil.invalid/pixel.png"}}}}
        });
        const auto row = client.messageRow(input);
        const auto items = row.value("embeds").toList();
        QCOMPARE(items.size(), 4);
        QCOMPARE(items.first().toMap().value("description").toString(), "<b>Plain content</b>");
        QCOMPARE(items.first().toMap().value("fields").toList().first().toMap().value("value").toString(), "Value");
        QVERIFY(items.first().toMap().value("imageUrl").toString().startsWith("https://images-ext-1.discordapp.net/"));
        QVERIFY(!items.at(1).toMap().value("imageUrl").toString().isEmpty());
        QCOMPARE(items.at(2).toMap().value("author").toString(), "Author-only embed");
        QVERIFY(items.last().toMap().value("url").toString().isEmpty());
        QVERIFY(items.last().toMap().value("thumbnailUrl").toString().isEmpty());
        QVERIFY(row.value("content").toString().isEmpty());
    }
    void stickerFormatsHaveUsefulFallbacks() {
        DiscordClient client;
        auto input = message(19);
        input.insert("sticker_items", QJsonArray{
            QJsonObject{{"id", "5001"}, {"name", "Wave"}, {"format_type", 2}},
            QJsonObject{{"id", "5002"}, {"name", "Dance"}, {"format_type", 4}},
            QJsonObject{{"id", "5003"}, {"name", "Lottie sticker"}, {"format_type", 3}}
        });
        const auto row = client.messageRow(input);
        const auto items = row.value("stickers").toList();
        QCOMPARE(items.size(), 3);
        QCOMPARE(items.first().toMap().value("url").toString(), "https://cdn.discordapp.com/stickers/5001.png");
        QCOMPARE(items.at(1).toMap().value("url").toString(), "https://media.discordapp.net/stickers/5002.gif");
        QVERIFY(items.last().toMap().value("url").toString().isEmpty());
        QCOMPARE(items.last().toMap().value("name").toString(), "Lottie sticker");
        QVERIFY(row.value("content").toString().isEmpty());
    }
    void threadStarterUsesOriginalBodyAndMedia() {
        DiscordClient client;
        auto original = message(0, "Original post text");
        original.insert("attachments", QJsonArray{attachment()});
        original.insert("member", QJsonObject{{"nick", "River nickname"}});
        auto input = message(21);
        input.insert("id", "1002");
        // Thread starters belong to a server; DMs must not inherit its nickname.
        input.insert("guild_id", "100");
        input.insert("referenced_message", original);
        input.insert("message_reference", QJsonObject{{"message_id", "1001"}, {"channel_id", "101"}});
        const auto row = client.messageRow(input);
        QCOMPARE(row.value("recordId").toString(), "1002");
        QCOMPARE(row.value("content").toString(), "Original post text");
        QCOMPARE(row.value("author").toString(), "River nickname");
        QCOMPARE(row.value("attachments").toList().size(), 1);
        QVERIFY(!row.value("systemMessage").toBool());
    }
    void forwardedSnapshotRetainsForwarderAndRichContent() {
        DiscordClient client;
        auto input = message();
        input.insert("message_reference", QJsonObject{{"type", 1}, {"message_id", "9000"}});
        input.insert("message_snapshots", QJsonArray{QJsonObject{{"message", QJsonObject{
            {"content", "Forwarded fixture"}, {"attachments", QJsonArray{attachment()}}}}}});
        const auto row = client.messageRow(input);
        QVERIFY(row.value("forwarded").toBool());
        QCOMPARE(row.value("content").toString(), "Forwarded fixture");
        QCOMPARE(row.value("author").toString(), "River");
        QCOMPARE(row.value("attachments").toList().size(), 1);
        QVERIFY(row.value("replyId").toString().isEmpty());
    }
    void repliesDistinguishDeletedFromUnfetched() {
        DiscordClient client;
        auto input = message(19, "A reply");
        input.insert("message_reference", QJsonObject{{"message_id", "9000"}});
        auto row = client.messageRow(input);
        QVERIFY(row.value("replyMissing").toBool());
        QCOMPARE(row.value("replyContent").toString(), "Original message is unavailable.");
        input.insert("referenced_message", QJsonValue::Null);
        QCOMPARE(client.messageRow(input).value("replyContent").toString(), "Original message was deleted.");
        auto referenced = message(0, "Earlier text");
        input.insert("referenced_message", referenced);
        row = client.messageRow(input);
        QVERIFY(!row.value("replyMissing").toBool());
        QCOMPARE(row.value("replyContent").toString(), "Earlier text");
        QCOMPARE(row.value("replyAuthor").toString(), "River");
    }
    void componentsV2KeepsNativeLayoutWithoutDuplicateFlattenedContent() {
        DiscordClient client;
        auto input = message(20);
        input.insert("flags", 1 << 15);
        input.insert("attachments", QJsonArray{attachment(), attachment("unused.png")});
        const QJsonObject galleryItem{{"spoiler", true}, {"media", QJsonObject{{"url", "attachment://photo.png"}}}};
        const QJsonObject gallery{{"type", 12}, {"items", QJsonArray{galleryItem}}};
        const QJsonObject hiddenText{{"type", 10}, {"content", "Hidden secret fixture"}};
        const QJsonObject hiddenContainer{{"type", 17}, {"spoiler", true}, {"components", QJsonArray{hiddenText}}};
        const QJsonArray visibleParts{QJsonObject{{"type", 10}, {"content", "Component text"}}, gallery, hiddenContainer};
        input.insert("components", QJsonArray{QJsonObject{{"type", 17}, {"components", visibleParts}}});
        const auto row = client.messageRow(input);
        QVERIFY(row.value("content").toString().isEmpty());
        QVERIFY(row.value("attachments").toList().isEmpty());
        const auto container=row.value("components").toList().first().toMap();
        const auto parts=container.value("components").toList();
        QCOMPARE(parts[0].toMap().value("content").toString(),"Component text");
        const auto galleryRow=parts[1].toMap().value("items").toList().first().toMap();
        QVERIFY(galleryRow.value("spoiler").toBool());
        QVERIFY(galleryRow.value("media").toMap().value("url").toString().contains("/photo.png"));
        QVERIFY(parts[2].toMap().value("spoiler").toBool());
        QVERIFY(!row.value("systemMessage").toBool());
    }
    void mentionsAndEmojiRemainPlainText() {
        DiscordClient client;
        auto input = message(0, "Hello <@43> and <@!43> <:wave:5001> <a:dance:5002> <img src='file:///bad'>");
        input.insert("mentions", QJsonArray{QJsonObject{{"id", "43"}, {"username", "Lake"},
            {"member", QJsonObject{{"nick", "Lake nickname"}}}}});
        const auto row = client.messageRow(input);
        QCOMPARE(row.value("content").toString(), "Hello @Lake nickname and @Lake nickname :wave: :dance: <img src='file:///bad'>");
    }
    void richPayloadsAreBounded() {
        DiscordClient client;
        auto input = message(0, QString(9000, 'a'));
        QJsonArray manyAttachments, manyEmbeds, manyStickers, manyComponents;
        for (int i = 0; i < 100; ++i) {
            manyAttachments.append(attachment(QString::number(i) + ".png"));
            manyEmbeds.append(QJsonObject{{"title", QString(1000, 'b')}, {"description", QString(8000, 'c')}});
            manyStickers.append(QJsonObject{{"id", QString::number(5000 + i)}, {"format_type", 1}});
            manyComponents.append(QJsonObject{{"type", 10}, {"content", QString(8000, 'd')}});
        }
        input.insert("attachments", manyAttachments);
        input.insert("embeds", manyEmbeds);
        input.insert("sticker_items", manyStickers);
        input.insert("components", manyComponents);
        const auto row = client.messageRow(input);
        QCOMPARE(row.value("content").toString().size(), 8000);
        QCOMPARE(row.value("attachments").toList().size(), 10);
        QVERIFY(row.value("embeds").toList().size() <= 10);
        QCOMPARE(row.value("stickers").toList().size(), 3);
        int textSize = 0;
        for (const auto &value : row.value("embeds").toList()) {
            const auto embed = value.toMap();
            textSize += embed.value("title").toString().size() + embed.value("description").toString().size();
        }
        QVERIFY(textSize <= 6000);
        QVERIFY(row.value("components").toList().size()<=40);
        int componentText=0;
        for (const auto &entry : row.value("components").toList()) componentText+=entry.toMap().value("content").toString().size();
        QVERIFY(componentText<=16000);
    }
    void componentControlsRejectExternalImagesUnsafeLinksAndInvalidEmojiIds() {
        auto input=message(0,"Text"); input.insert("components",QJsonArray{
            QJsonObject{{"type",2},{"style",5},{"url","javascript:alert(1)"},{"label","Unsafe"}},
            QJsonObject{{"type",2},{"style",1},{"custom_id","id"},{"emoji",QJsonObject{{"id","42/../../bad"},{"name","Wave"}}}},
            QJsonObject{{"type",11},{"media",QJsonObject{{"url","https://tracking.example/image.png"}}}}});
        const auto parts=DiscordClient::normalizedComponents(input);
        QVERIFY(parts[0].toMap().value("disabled").toBool()); QVERIFY(parts[0].toMap().value("url").toString().isEmpty());
        QVERIFY(parts[1].toMap().value("emoji").toMap().value("id").toString().isEmpty());
        QVERIFY(parts[2].toMap().value("media").toMap().isEmpty());
        const auto readonly=DiscordClient::normalizedComponents(input,true); QVERIFY(readonly[1].toMap().value("disabled").toBool());
    }
    void gatewayRowsExposeRichRolesToQml() {
        DiscordClient client;
        client.m_currentChannelId = "101";
        auto joined = message(7);
        client.dispatch("MESSAGE_CREATE", joined);
        auto rich = message(19);
        rich.insert("id", "1002");
        rich.insert("attachments", QJsonArray{attachment()});
        rich.insert("embeds", QJsonArray{QJsonObject{{"title", "Useful preview"}}});
        client.dispatch("MESSAGE_CREATE", rich);
        const auto roles = client.messages()->roleNames();
        for (const auto &role : {"embeds", "stickers", "replyAuthor", "replyContent", "replyId",
                                "replyMissing", "forwarded", "systemMessage", "messageType", "components", "pinned", "channelName", "pinnedAt"})
            QVERIFY2(roles.values().contains(role), role);
        const auto systemRole = roles.key("systemMessage");
        const auto contentRole = roles.key("content");
        QCOMPARE(client.messages()->data(client.messages()->index(0), systemRole).toBool(), true);
        QCOMPARE(client.messages()->data(client.messages()->index(0), contentRole).toString(), "River joined the server.");
        QCOMPARE(client.messages()->data(client.messages()->index(1), systemRole).toBool(), false);
        QCOMPARE(client.messages()->get(1).value("embeds").toList().first().toMap().value("title").toString(), "Useful preview");
        QCOMPARE(client.messages()->get(1).value("attachments").toList().size(), 1);
    }
};

QTEST_GUILESS_MAIN(MessageRenderTest)
#include "message_render_test.moc"
