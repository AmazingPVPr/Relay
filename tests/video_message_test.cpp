#include "DiscordClient.h"
#include <QtTest>
class VideoMessageTest final:public QObject {
    Q_OBJECT
    static QJsonObject message(QString content={}){return {{"id","1"},{"channel_id","2"},{"content",content},{"author",QJsonObject{{"id","3"},{"username","Fixture"}}}};}
    static QVariantMap embed(DiscordClient &client,const QJsonObject &payload){const auto items=client.messageRow(payload).value("embeds").toList();return items.isEmpty()?QVariantMap{}:items.first().toMap();}
private slots:
    void richEmbedKeepsVisualMetadataAndValidatedLinks(){
        DiscordClient client;auto input=message("[Tweet](<https://example.invalid/status/123>)");
        QJsonObject rich{{"type","rich"},{"color",0x5865f2},{"timestamp","2026-10-20T22:56:00Z"},
            {"author",QJsonObject{{"name","Fixture News"},{"url","https://example.invalid/profile"},{"icon_url","https://tracker.invalid/avatar.png"},{"proxy_icon_url","https://images-ext-1.discordapp.net/external/fixture/avatar.png"}}},
            {"footer",QJsonObject{{"text","Fixture provider"},{"icon_url","https://cdn.discordapp.com/emojis/1.png"}}},
            {"fields",QJsonArray{QJsonObject{{"name","**Replies**"},{"value","**218**"},{"inline",true}}}}};
        input.insert("embeds",QJsonArray{rich});auto row=embed(client,input);
        QVERIFY(row.value("rich").toBool());QCOMPARE(row.value("accentColor").toString(),"#5865f2");
        QCOMPARE(row.value("authorUrl").toString(),"https://example.invalid/profile");
        QCOMPARE(row.value("authorIconUrl").toString(),"https://images-ext-1.discordapp.net/external/fixture/avatar.png");
        QCOMPARE(row.value("footerIconUrl").toString(),"https://cdn.discordapp.com/emojis/1.png");
        QCOMPARE(row.value("timestamp").toString(),"2026-10-20T22:56:00.000Z");
        QVERIFY(row.value("fields").toList().first().toMap().value("inline").toBool());
        rich.insert("color",-1);rich.insert("timestamp","bad date");
        rich.insert("author",QJsonObject{{"name","Fixture"},{"url","javascript:alert(1)"},{"icon_url","file:///tmp/avatar.png"}});
        rich.insert("footer",QJsonObject{{"text","Fixture"},{"icon_url","https://tracker.invalid/icon.png"}});
        input.insert("embeds",QJsonArray{rich});row=embed(client,input);
        for(const auto key:{"accentColor","timestamp","authorUrl","authorIconUrl","footerIconUrl"})QVERIFY(row.value(key).toString().isEmpty());
    }
    void gifvKeepsProviderVideoAspectAndProxyPoster() {
        DiscordClient client;auto input=message();input.insert("embeds",QJsonArray{QJsonObject{{"type","gifv"},{"url","https://tenor.com/view/fixture"},{"provider",QJsonObject{{"name","Tenor"}}},{"video",QJsonObject{{"url","https://media.tenor.com/fixture.mp4"},{"width",640},{"height",360}}},{"thumbnail",QJsonObject{{"url","https://third-party.invalid/tracker.jpg"},{"proxy_url","https://images-ext-1.discordapp.net/external/fixture/poster.jpg"}}}}});
        auto row=embed(client,input);QCOMPARE(row.value("kind").toString(),QString("gifv"));QVERIFY(row.value("gif").toBool());QCOMPARE(row.value("videoUrl").toString(),QString("https://media.tenor.com/fixture.mp4"));QCOMPARE(row.value("videoWidth").toInt(),640);QCOMPARE(row.value("videoHeight").toInt(),360);QVERIFY(row.value("posterUrl").toString().startsWith("https://images-ext-1.discordapp.net/"));
        auto raw=input.value("embeds").toArray()[0].toObject();auto video=raw.value("video").toObject();video.insert("proxy_url","https://media.discordapp.net/external/fixture.mp4");raw.insert("video",video);input.insert("embeds",QJsonArray{raw});QVERIFY(embed(client,input).value("videoUrl").toString().startsWith("https://media.discordapp.net/"));
    }
    void youtubeEmbedsAndBareLinksUseExplicitResolverWithOffset() {
        DiscordClient client;auto input=message("https://youtu.be/b2tjnxX8IHU?t=262");auto row=embed(client,input);QCOMPARE(row.value("kind").toString(),QString("video"));QCOMPARE(row.value("pageUrl").toString(),QString("https://www.youtube.com/watch?v=b2tjnxX8IHU&t=262"));QCOMPARE(row.value("startSeconds").toInt(),262);QVERIFY(row.value("videoUrl").toString().isEmpty());
        input.insert("embeds",QJsonArray{QJsonObject{{"type","video"},{"url","https://youtu.be/b2tjnxX8IHU?t=262"},{"video",QJsonObject{{"url","https://www.youtube.com/embed/b2tjnxX8IHU"},{"width",1280},{"height",720}}}}});QCOMPARE(client.messageRow(input).value("embeds").toList().size(),1);row=embed(client,input);QCOMPARE(row.value("videoWidth").toInt(),1280);QVERIFY(row.value("pageUrl").toString().contains("t=262"));
    }
    void canonicalEmbedKeepsSingleLinkStartTimeWithoutDuplicate() {
        DiscordClient client;auto input=message("https://youtu.be/b2tjnxX8IHU?t=262");
        input.insert("embeds",QJsonArray{QJsonObject{{"type","video"},{"url","https://www.youtube.com/watch?v=b2tjnxX8IHU"},{"video",QJsonObject{{"url","https://www.youtube.com/embed/b2tjnxX8IHU"},{"width",1280},{"height",720}}}}});
        const auto rows=client.messageRow(input).value("embeds").toList();QCOMPARE(rows.size(),1);
        const auto row=rows.first().toMap();QCOMPARE(row.value("startSeconds").toInt(),262);
        QCOMPARE(row.value("pageUrl").toString(),QString("https://www.youtube.com/watch?v=b2tjnxX8IHU&t=262"));
        QVERIFY(row.value("url").toString().endsWith("&t=262"));
        input.insert("content","https://youtu.be/AAAAAAAAAAA?t=100");
        const auto different=client.messageRow(input).value("embeds").toList();QCOMPARE(different.size(),2);
        QCOMPARE(different.first().toMap().value("startSeconds").toInt(),0);
    }
    void suppressionSpoilersAndUntrustedVideosNeverGainFallbackPlayers() {
        DiscordClient client;auto input=message("https://youtu.be/b2tjnxX8IHU?t=262");input.insert("flags",4);input.insert("embeds",QJsonArray{QJsonObject{{"type","video"},{"url","https://youtu.be/b2tjnxX8IHU"}}});QVERIFY(client.messageRow(input).value("embeds").toList().isEmpty());
        for(const auto &content:QStringList{"<https://youtu.be/b2tjnxX8IHU>","||https://youtu.be/b2tjnxX8IHU||","`https://youtu.be/b2tjnxX8IHU`","https://youtube.com.evil.invalid/watch?v=b2tjnxX8IHU","https://127.0.0.1/clip.mp4"})QVERIFY(client.messageRow(message(content)).value("embeds").toList().isEmpty());
        input=message();input.insert("embeds",QJsonArray{QJsonObject{{"type","video"},{"video",QJsonObject{{"url","file:///tmp/private.mp4"}}}}});QVERIFY(embed(client,input).value("videoUrl").toString().isEmpty());QVERIFY(embed(client,input).value("pageUrl").toString().isEmpty());
    }
    void attachmentsAndComponentMediaKeepVideoAndGifHints() {
        DiscordClient client;auto input=message();const QJsonObject video{{"filename","SPOILER_clip.mp4"},{"url","https://cdn.discordapp.com/attachments/1/2/SPOILER_clip.mp4?ex=ffff&hm=a%2Fb"},{"content_type","video/mp4"},{"width",1920},{"height",1080}};
        input.insert("attachments",QJsonArray{video,QJsonObject{{"filename","loop.gif"},{"url","https://cdn.discordapp.com/attachments/1/3/loop.gif"},{"content_type","image/gif"}}});
        auto rows=client.messageRow(input).value("attachments").toList();QCOMPARE(rows.size(),2);QVERIFY(rows[0].toMap().value("isVideo").toBool());QVERIFY(rows[0].toMap().value("spoiler").toBool());QVERIFY(rows[0].toMap().value("videoUrl").toString().contains("hm=a%2Fb"));QVERIFY(rows[1].toMap().value("isGif").toBool());
        input.insert("flags",1<<15);input.insert("components",QJsonArray{QJsonObject{{"type",12},{"items",QJsonArray{QJsonObject{{"media",QJsonObject{{"url","attachment://SPOILER_clip.mp4"}}},{"spoiler",true}}}}}});
        const auto component=client.messageRow(input).value("components").toList().first().toMap();const auto media=component.value("items").toList().first().toMap().value("media").toMap();QVERIFY(media.value("isVideo").toBool());QVERIFY(media.value("videoUrl").toString().contains("SPOILER_clip.mp4"));
    }
};
QTEST_GUILESS_MAIN(VideoMessageTest)
#include "video_message_test.moc"
