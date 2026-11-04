#include "AppSettings.h"
#include "ChatText.h"
#include <QtTest>
#include <QDateTime>
#include <QSettings>
#include <QTemporaryDir>
#include <limits>

class AccessibilitySettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void defaultsAndBoundedSavedPreferences() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto file = dir.filePath("prefs.ini");
        {
            AppSettings prefs(file);
            QCOMPARE(prefs.chatFontSize(), 14.); QCOMPARE(prefs.messageGroupGap(), 16.); QCOMPARE(prefs.roleColorsMode(), "names"); QCOMPARE(prefs.uiDensity(), "default");
            QVERIFY(prefs.underlineLinks()); QVERIFY(prefs.animateImages()); QVERIFY(!prefs.highContrast());
            QCOMPARE(prefs.notificationDuration(), 5); QCOMPARE(prefs.language(), "en_US"); QCOMPARE(prefs.timeFormat(), "auto");
            prefs.setChatFontSize(100); QCOMPARE(prefs.chatFontSize(), 24.); prefs.setChatFontSize(0); QCOMPARE(prefs.chatFontSize(), 12.);
            prefs.setChatFontSize(18); prefs.setChatFontSize(std::numeric_limits<double>::quiet_NaN()); QCOMPARE(prefs.chatFontSize(), 18.);
            prefs.setMessageGroupGap(-20); QCOMPARE(prefs.messageGroupGap(), 0.); prefs.setMessageGroupGap(100); QCOMPARE(prefs.messageGroupGap(), 24.);
            prefs.setNotificationDuration(100); QCOMPARE(prefs.notificationDuration(), 20); prefs.setNotificationDuration(0); QCOMPARE(prefs.notificationDuration(), 3);
            prefs.setRoleColorsMode("dots"); prefs.setRoleColorsMode("invalid"); QCOMPARE(prefs.roleColorsMode(), "dots");
            prefs.setUiDensity("compact"); prefs.setUiDensity("invalid"); QCOMPARE(prefs.uiDensity(), "compact");
            prefs.setLanguage("en_GB"); prefs.setLanguage("de_DE"); QCOMPARE(prefs.language(), "en_GB");
            prefs.setTimeFormat("24"); prefs.setTimeFormat("invalid"); QCOMPARE(prefs.timeFormat(), "24");
            prefs.setHighContrast(true); prefs.setUnderlineLinks(false); prefs.setAnimateImages(false);
        }
        AppSettings restored(file); QCOMPARE(restored.chatFontSize(), 18.); QCOMPARE(restored.messageGroupGap(), 24.); QCOMPARE(restored.roleColorsMode(), "dots");
        QCOMPARE(restored.uiDensity(), "compact"); QCOMPARE(restored.notificationDuration(), 3); QCOMPARE(restored.language(), "en_GB"); QCOMPARE(restored.timeFormat(), "24");
        QVERIFY(restored.highContrast()); QVERIFY(!restored.underlineLinks()); QVERIFY(!restored.animateImages());
    }
    void corruptedSettingsUseSafeDefaults() {
        QTemporaryDir dir; const auto file = dir.filePath("prefs.ini");
        { QSettings settings(file, QSettings::IniFormat); settings.setValue("accessibility/chatFontSize", "bad"); settings.setValue("accessibility/messageGroupGap", "nan");
          settings.setValue("accessibility/roleColorsMode", "html"); settings.setValue("accessibility/uiDensity", "tiny"); settings.setValue("language/locale", "../../bad"); settings.setValue("language/timeFormat", "code"); }
        AppSettings prefs(file); QCOMPARE(prefs.chatFontSize(), 14.); QCOMPARE(prefs.messageGroupGap(), 16.); QCOMPARE(prefs.roleColorsMode(), "names");
        QCOMPARE(prefs.uiDensity(), "default"); QCOMPARE(prefs.language(), "en_US"); QCOMPARE(prefs.timeFormat(), "auto");
    }
    void individualCuesAreAllowlistedAndPersisted() {
        QTemporaryDir dir; const auto file = dir.filePath("prefs.ini");
        const QStringList cues{"mute","unmute","deafen","undeafen","join","leave","user-join","user-leave","stream-start","stream-stop","watch-start","watch-stop","message"};
        { AppSettings prefs(file); QSignalSpy changes(&prefs, &AppSettings::changed);
          for (const auto &cue : cues) { QVERIFY(prefs.soundEnabled(cue)); prefs.setSoundEnabled(cue, false); QVERIFY(!prefs.soundEnabled(cue)); }
          QCOMPARE(changes.count(), 13); prefs.setSoundEnabled("../volume", false); prefs.setSoundEnabled("", true); QVERIFY(!prefs.soundEnabled("other")); QCOMPARE(changes.count(), 13); }
        AppSettings restored(file); for (const auto &cue : cues) QVERIFY(!restored.soundEnabled(cue));
        restored.setSoundEnabled("mute", true); QVERIFY(restored.soundEnabled("mute")); QVERIFY(!restored.soundEnabled("unmute"));
    }
    void localeTimeAndRelativeDatesFollowSavedChoice() {
        QTemporaryDir dir; AppSettings prefs(dir.filePath("prefs.ini")); const QString fixed = "2020-01-02T18:04:05";
        prefs.setTimeFormat("12"); QVERIFY(prefs.formatMessageTime(fixed).endsWith("at 6:04 PM")); QVERIFY(prefs.formatDateTime(fixed).endsWith("at 6:04:05 PM"));
        QVERIFY(prefs.formatMessageTime(fixed).startsWith("Jan 2, 2020"));
        prefs.setTimeFormat("24"); QVERIFY(prefs.formatMessageTime(fixed).endsWith("at 18:04")); QVERIFY(prefs.formatDateTime(fixed).endsWith("at 18:04:05"));
        prefs.setLanguage("en_GB"); QVERIFY(prefs.formatMessageTime(fixed).startsWith("2 Jan 2020")); prefs.setTimeFormat("auto"); QVERIFY(prefs.formatMessageTime(fixed).endsWith("18:04"));
        const auto today = QDateTime(QDate::currentDate(), QTime(9, 5)).toString(Qt::ISODate);
        const auto yesterday = QDateTime(QDate::currentDate().addDays(-1), QTime(9, 5)).toString(Qt::ISODate);
        QVERIFY(prefs.formatMessageTime(today).startsWith("Today at ")); QVERIFY(prefs.formatMessageTime(yesterday).startsWith("Yesterday at "));
        QVERIFY(prefs.formatMessageTime("bad").isEmpty()); QVERIFY(prefs.formatDateTime("").isEmpty());
    }
    void renderingOptionsInvalidateCacheWithoutWeakeningMarkupSafety() {
        ChatText text; const QString input = "[**Example**](https://example.test/a?x=1&y=2) <@123> `https://code.test`";
        const QVariantList users{QVariantMap{{"id","123"},{"name","<script>"}}};
        const auto ordinary = text.render(input, {{"mentionUsers",users}}); QVERIFY(ordinary.contains("text-decoration:underline"));
        const auto changed = text.render(input, {{"mentionUsers",users},{"underlineLinks",false},{"highContrast",true}});
        QVERIFY(changed.contains("color:#9bd7ff;text-decoration:none")); QVERIFY(changed.contains("<b>Example</b>")); QVERIFY(!changed.contains("<script>")); QVERIFY(changed.contains("&lt;script&gt;"));
        QVERIFY(!changed.contains("href=\"https://code.test")); QCOMPARE(text.render(input, {{"mentionUsers",users}}), ordinary); QVERIFY(changed != ordinary);
        const auto stamp = QDateTime(QDate(2020, 1, 2), QTime(18, 4)).toSecsSinceEpoch(); const auto token = QString("<t:%1:t>").arg(stamp);
        QVERIFY(text.render(token, {{"language","en_US"},{"timeFormat","12"}}).contains("6:04 PM"));
        QVERIFY(text.render(token, {{"language","en_GB"},{"timeFormat","24"}}).contains("18:04"));
    }
};
QTEST_GUILESS_MAIN(AccessibilitySettingsTest)
#include "accessibility_settings_test.moc"
