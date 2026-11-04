#include "ChatText.h"
#include "AppSettings.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextBlock>
#include <QSignalSpy>
#include <limits>
class ChatTextTest final : public QObject {
    Q_OBJECT
private slots:
    void discordMarkdownFormatsMessageAndEmbedTextSafely() {
        ChatText formatter;
        const auto html=formatter.render("**Bold title**\n*italic* __underline__ ~~removed~~ ***both***\n**A https://example.com link**\n> quoted\n- item\n\\*literal\\* snake_case\n```cpp\n**code** https://code.invalid <script>\n```\n**<img src='file:///secret'>**");
        QVERIFY(html.contains("<b>Bold title</b>"));QVERIFY(html.contains("<i>italic</i>"));
        QVERIFY(html.contains("<u>underline</u>"));QVERIFY(html.contains("<s>removed</s>"));
        QVERIFY(html.contains("<b><i>both</i></b>"));QVERIFY(html.contains("<b>A <a href=\"https://example.com\""));
        QVERIFY(!html.contains("href=\"https://code.invalid"));QVERIFY(!html.contains("<script>"));QVERIFY(!html.contains("<img"));
        QTextDocument doc;doc.setHtml(html);const auto plain=doc.toPlainText();
        QVERIFY(plain.contains("Bold title"));QVERIFY(!plain.contains("**Bold title**"));
        QVERIFY(plain.contains("*literal* snake_case"));QVERIFY(plain.contains("**code** https://code.invalid <script>"));
        QVERIFY(plain.contains("<img src='file:///secret'>"));
        QVERIFY(formatter.render(QString(20000,'*')).size()<250000);
    }

    void linksAreEscapedAndValidated() {
        ChatText formatter;
        const auto html=formatter.render("<img src='file:///tmp/private'> Visit https://example.com/a?q=1&b=2. [Docs](https://qt.io)\n`https://code.test`");
        QVERIFY(!html.contains("<img")); QVERIFY(html.contains("&lt;img"));
        QVERIFY(html.contains("href=\"https://example.com/a?q=1&amp;b=2\""));
        QVERIFY(html.contains("href=\"https://qt.io\"")); QVERIFY(!html.contains("href=\"https://code.test\""));
        for(const auto value : {"javascript:alert(1)","file:///tmp/a","https://user:pw@example.com","https://example.com/\nno","https:///","https://example.com/\"onclick","https://example.com/%0Ainjected","https://example.com:0"}) QVERIFY(!ChatText::safeUrl(value));
        QVERIFY(ChatText::safeUrl("https://example.com/hello_(world)"));
        QTextDocument doc; doc.setHtml(html); QVERIFY(doc.toPlainText().contains("<img src='file:///tmp/private'>"));
    }
    void twitterFooterMaskedLinksDoNotShowMarkdown() {
        ChatText formatter;
        const QString input="[Tweet](<https://x.com/i/status/2112679445891105003>) · [@nypost](<https://x.com/nypost>) · FxTwitter";
        const auto html=formatter.render(input);QTextDocument doc;doc.setHtml(html);
        QCOMPARE(doc.toPlainText(),QString("Tweet · @nypost · FxTwitter"));
        QVERIFY(html.contains("href=\"https://x.com/i/status/2112679445891105003\""));
        QVERIFY(html.contains("href=\"https://x.com/nypost\""));QCOMPARE(html.count("<a "),2);
        QCOMPARE(formatter.render(input),html); // The existing rendering cache remains usable.
    }
    void maskedLinkDestinationsAndTitles_data() {
        QTest::addColumn<QString>("input");QTest::addColumn<QString>("url");QTest::addColumn<QString>("plain");
        QTest::newRow("bare-balanced")<<QString("[Article](https://example.com/a_(b(c))) after")<<QString("https://example.com/a_(b(c))")<<QString("Article after");
        QTest::newRow("angle-unbalanced")<<QString("[Article](<https://example.com/a_(b>)")<<QString("https://example.com/a_(b")<<QString("Article");
        QTest::newRow("escaped-parentheses")<<QString("[Article](https://example.com/a_\\(b\\))")<<QString("https://example.com/a_(b)")<<QString("Article");
        QTest::newRow("query-and-title")<<QString("[Docs]( https://example.com/a?q=1&n=2 \"A title\" )")<<QString("https://example.com/a?q=1&n=2")<<QString("Docs");
        QTest::newRow("angle-single-title")<<QString("[Docs](<https://example.com> 'A title')")<<QString("https://example.com")<<QString("Docs");
        QTest::newRow("parenthesized-title")<<QString("[Docs](https://example.com (A title))")<<QString("https://example.com")<<QString("Docs");
        QTest::newRow("escaped-label-brackets")<<QString("[a \\[b\\] c](<https://example.com>)")<<QString("https://example.com")<<QString("a [b] c");
        QTest::newRow("balanced-label-brackets")<<QString("[a [b] c](https://example.com)")<<QString("https://example.com")<<QString("a [b] c");
        QTest::newRow("code-label-bracket")<<QString("[`a ] b`](https://example.com)")<<QString("https://example.com")<<QString("a ] b");
    }
    void maskedLinkDestinationsAndTitles() {
        QFETCH(QString,input);QFETCH(QString,url);QFETCH(QString,plain);ChatText formatter;const auto html=formatter.render(input);
        QVERIFY2(html.contains("href=\""+url.toHtmlEscaped()+"\""),qPrintable(html));QCOMPARE(html.count("<a "),1);
        QTextDocument doc;doc.setHtml(html);QCOMPARE(doc.toPlainText(),plain);
    }
    void maskedLabelsFormatWithoutNestedLinksOrHtmlInjection() {
        ChatText formatter;
        const auto html=formatter.render("[**Tweet** *news* __more__ ~~old~~ `**raw**` <img src='file:///secret'> <@123> https://label.invalid](<https://example.com>)");
        QVERIFY(html.contains("<b>Tweet</b>"));QVERIFY(html.contains("<i>news</i>"));QVERIFY(html.contains("<u>more</u>"));QVERIFY(html.contains("<s>old</s>"));
        QVERIFY(html.contains(">**raw**</code>"));QCOMPARE(html.count("<a "),1);QVERIFY(!html.contains("<img"));QVERIFY(!html.contains("relay-user:"));
        QTextDocument doc;doc.setHtml(html);QCOMPARE(doc.toPlainText(),QString("Tweet news more old **raw** <img src='file:///secret'> <@123> https://label.invalid"));
        const auto tooltip=formatter.render("[safe](https://example.com \"a \\\" onclick=\\\"evil & <b>\")");
        QVERIFY(tooltip.contains("title=\"a &quot; onclick=&quot;evil &amp; &lt;b&gt;\""));QVERIFY(!tooltip.contains("\" onclick=\""));
    }
    void maskedLinksPreserveCodeAndRejectUnsafeDestinations() {
        ChatText formatter;
        for(const auto input:{"[bad](javascript:alert(1))","[bad](<file:///tmp/private>)","[bad](<https://user:secret@example.com>)","[bad](<https://example.com/%0Ainjected>)","[bad](<https://example.com/a b>)"}){
            const auto html=formatter.render(input);QVERIFY2(!html.contains("<a "),input);QTextDocument doc;doc.setHtml(html);QCOMPARE(doc.toPlainText(),QString(input));
        }
        const QString code="`[Tweet](<https://x.com/test>)`\n```\n[Docs](https://example.com)\n```";
        const auto html=formatter.render(code);QVERIFY(!html.contains("<a "));QTextDocument doc;doc.setHtml(html);
        QVERIFY(doc.toPlainText().contains("[Tweet](<https://x.com/test>)"));QVERIFY(doc.toPlainText().contains("[Docs](https://example.com)"));
        const auto escaped=formatter.render("\\[literal](https://example.com)");QTextDocument escapedDoc;escapedDoc.setHtml(escaped);
        QCOMPARE(escapedDoc.toPlainText(),QString("[literal](https://example.com)"));QVERIFY(!escaped.contains(">literal</a>"));
        QVERIFY(formatter.render(QString(20000,'[')).size()<250000);
    }
    void mentionsCannotInjectMarkup() {
        ChatText formatter;
        const QVariantMap metadata{{"mentionUsers",QVariantList{QVariantMap{{"id","123"},{"name","<b>A & B</b>"}}}},
            {"mentionRoles",QVariantList{QVariantMap{{"id","456"},{"name","Artists"}}}}};
        const auto html=formatter.render("Hi <@123> <@!123> <@&456> @everyone",metadata);
        QVERIFY(html.contains("relay-user:123")); QVERIFY(!html.contains("<b>A")); QVERIFY(html.contains("@Artists"));
        QTextDocument doc;doc.setHtml(html);QCOMPARE(doc.toPlainText(),QString("Hi @<b>A & B</b> @<b>A & B</b> @Artists @everyone"));
    }
    void persistedLayoutAndCropValidation() {
        QTemporaryDir dir;const auto file=dir.path()+"/settings.ini";
        { AppSettings prefs(file);QSignalSpy changes(&prefs,&AppSettings::changed);
          QVERIFY(prefs.setStreamCropRatio(21,9)); QCOMPARE(changes.count(),1);
          QVERIFY(!prefs.setStreamCropRatio(10000,.1)); QVERIFY(!prefs.setStreamCropRatio(std::numeric_limits<double>::infinity(),9));
          QCOMPARE(prefs.streamCropWidth(),21.); prefs.setChannelSidebarWidth(9999); QCOMPARE(prefs.channelSidebarWidth(),520.);
          prefs.setInputVolume(.73); prefs.setScrollMultiplier(4.25); prefs.setNotificationPreviews(false); }
        AppSettings restored(file); QCOMPARE(restored.streamCropWidth(),21.); QCOMPARE(restored.inputVolume(),.73); QCOMPARE(restored.scrollMultiplier(),4.25); QVERIFY(!restored.notificationPreviews());
    }
    void discordTokensRenderWithoutUntrustedMarkup() {
        ChatText formatter;
        const auto html = formatter.render("Use </play:123456> <:music:456789> <a:dance:678901> <t:0:D> <t:0:R>");
        QVERIFY(html.contains(">/play</span>"));
        QVERIFY(html.contains("https://cdn.discordapp.com/emojis/456789.png?size=128"));
        QVERIFY(html.contains("https://cdn.discordapp.com/emojis/678901.png?size=128"));
        QVERIFY(!html.contains("&lt;t:"));
        QVERIFY(html.contains("days ago"));
        const auto bad = formatter.render("<:bad-name:123> <:hi:javascript> <t:hello:D> </<img:1>");
        QVERIFY(!bad.contains("<img"));
        QVERIFY(bad.contains("&lt;"));
    }
};
QTEST_MAIN(ChatTextTest)
#include "chat_text_test.moc"
