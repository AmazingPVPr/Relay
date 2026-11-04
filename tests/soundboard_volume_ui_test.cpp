#include <QtTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickStyle>
#include <QDir>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QFile>
#include "AppSettings.h"
#include "ContextActions.h"
#include "CrispImage.h"
#include "DiscordClient.h"
#include "GuildManagement.h"
#include "MemberActions.h"
#include "VoiceEngine.h"

// No media player, device capture, account or network is used by this fixture.
class SoundboardVolumeLibrary final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sounds READ sounds CONSTANT)
    Q_PROPERTY(QVariantList favorites READ sounds CONSTANT)
    Q_PROPERTY(QVariantList frequent READ sounds CONSTANT)
    Q_PROPERTY(QString guildId READ guildId CONSTANT)
    Q_PROPERTY(QString error READ guildId CONSTANT)
    Q_PROPERTY(bool busy READ inactive CONSTANT)
    Q_PROPERTY(bool sending READ inactive CONSTANT)
    Q_PROPERTY(bool canPlay READ inactive CONSTANT)
    Q_PROPERTY(bool previewLoading READ inactive CONSTANT)
public:
    QVariantList sounds() const { return {}; }
    QString guildId() const { return {}; }
    bool inactive() const { return false; }
    Q_INVOKABLE void stopPreview() {}
    Q_INVOKABLE bool isFavorite(const QString &) const { return false; }
};

#ifndef Q_MOC_RUN
static const char *fixtureQml = R"(
import QtQuick
import QtQuick.Controls
import "%1"
ApplicationWindow {
    width: 960; height: 640; visible: true; color: "#1b1b1f"
    AudioSettings { objectName: "fixtureAudioSettings"; anchors.fill: parent; visible: false }
    SoundboardDialog { objectName: "fixtureSoundboardDialog" }
    PersonContextMenu { objectName: "fixturePersonMenu"; userId: "111"; guildId: "100"; user: ({name:"Generated person",inVoice:true}) }
}
)";
#endif

class SoundboardVolumeUiTest final : public QObject {
    Q_OBJECT
    QTemporaryDir files;
    std::unique_ptr<DiscordClient> discord;
    std::unique_ptr<VoiceEngine> voice;
    std::unique_ptr<MemberActions> members;
    std::unique_ptr<ContextActions> contexts;
    std::unique_ptr<GuildManagement> guilds;
    std::unique_ptr<AppSettings> preferences;
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root;
    SoundboardVolumeLibrary library;
    QObject inert;
    QStringList warnings;
    QObject *find(const char *name) { return root->findChild<QObject *>(QString::fromLatin1(name)); }
    bool evaluate(QObject *scope, const QString &code) {
        QQmlExpression expression(engine->rootContext(), scope, code);
        expression.evaluate();
        if (expression.hasError()) { qWarning() << expression.error(); return false; }
        QCoreApplication::processEvents();
        return true;
    }
    void screenshot(const QString &name) {
        const auto directory = qEnvironmentVariable("RELAY_SOUNDBOARD_VOLUME_SCREENSHOTS");
        if (directory.isEmpty()) return;
        QVERIFY(QDir().mkpath(directory));
        auto *window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        QVERIFY(window->grabWindow().save(directory + "/" + name + ".png"));
    }
    void enter(QObject *control, const QString &number) {
        QVERIFY(control);
        QVERIFY(evaluate(control, "beginEditing(); editText = '" + number + "'; commit()"));
    }
    void move(QObject *control, double value) {
        QVERIFY(control);
        QVERIFY(control->setProperty("value", value));
        QVERIFY(QMetaObject::invokeMethod(control, "moved"));
        QCoreApplication::processEvents();
    }
private slots:
    void initTestCase() { QQuickStyle::setStyle("Basic"); qmlRegisterType<CrispImage>("Relay.Native", 1, 0, "CrispImage"); }
    void init() {
        QVERIFY(files.isValid()); warnings.clear();
        const auto path = files.filePath(QString::number(QDateTime::currentMSecsSinceEpoch()) + ".ini");
        preferences = std::make_unique<AppSettings>(path);
        discord = std::make_unique<DiscordClient>();
        voice = std::make_unique<VoiceEngine>();
        members = std::make_unique<MemberActions>(discord.get(), voice.get(), path + ".members");
        contexts = std::make_unique<ContextActions>(discord.get(), path + ".context");
        guilds = std::make_unique<GuildManagement>(discord.get());
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.append(error.toString());
        });
        auto *context = engine->rootContext();
        context->setContextProperty("preferences", preferences.get());
        context->setContextProperty("discord", discord.get());
        context->setContextProperty("voice", voice.get());
        context->setContextProperty("memberActions", members.get());
        context->setContextProperty("contextActions", contexts.get());
        context->setContextProperty("guildManagement", guilds.get());
        context->setContextProperty("soundboard", &library);
        context->setContextProperty("soundEffects", &inert);
        context->setContextProperty("chatText", &inert);
        const auto directory = QFileInfo(QFINDTESTDATA("../qml/components/AudioSettings.qml")).absolutePath();
        QQmlComponent component(engine.get());
        component.setData(QString::fromUtf8(fixtureQml).arg(QUrl::fromLocalFile(directory).toString()).toUtf8(), QUrl::fromLocalFile(directory + "/generated-soundboard-volume-fixture.qml"));
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QVERIFY(find("soundboardListeningVolume"));
        QVERIFY(find("soundboardPopupListeningVolume"));
        QVERIFY(find("personSoundboardVolume"));
    }
    void cleanup() {
        root.reset(); engine.reset(); guilds.reset(); contexts.reset(); members.reset(); voice.reset(); discord.reset(); preferences.reset();
        QCoreApplication::processEvents();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }
    void customEditorLoadsAndFitsTheSoundboard() {
        const auto directory=QFileInfo(QFINDTESTDATA("../qml/components/AudioSettings.qml")).absolutePath();
        QFile source(QFINDTESTDATA("../addons/soundboard/Board.qml"));QVERIFY(source.open(QIODevice::ReadOnly));
        auto data=source.readAll();data.replace("import Relay",("import \""+QUrl::fromLocalFile(directory).toString()+"\"").toUtf8());
        data.replace("qrc:/Relay/components/Theme.js",QUrl::fromLocalFile(directory+"/Theme.js").toEncoded());
        QQmlComponent component(engine.get());component.setData(data,QUrl::fromLocalFile(directory+"/generated-custom-board.qml"));
        std::unique_ptr<QObject> editor(component.create());QVERIFY2(editor,qPrintable(component.errorString()));
        auto *item=qobject_cast<QQuickItem *>(editor.get());QVERIFY(item);
        auto *window=qobject_cast<QQuickWindow *>(root.get());item->setParentItem(window->contentItem());item->setWidth(590);item->setHeight(450);
        QVERIFY(editor->setProperty("editing",true));QTest::qWait(50);
        screenshot("custom-sound-trim-editor");
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    }
    void masterControlsStaySynchronizedAndAcceptZero() {
        QCOMPARE(preferences->soundboardVolume(), .5);
        enter(find("soundboardListeningPercent"), "0");
        QCOMPARE(preferences->soundboardVolume(), 0.0);
        QCOMPARE(find("soundboardPopupListeningVolume")->property("value").toDouble(), 0.0);
        QCOMPARE(find("soundboardPopupListeningPercent")->property("value").toDouble(), 0.0);
        move(find("soundboardPopupListeningVolume"), .73);
        QCOMPARE(preferences->soundboardVolume(), .73);
        QCOMPARE(find("soundboardListeningPercent")->property("value").toDouble(), 73.0);
        enter(find("soundboardPopupListeningPercent"), "100");
        QCOMPARE(preferences->soundboardVolume(), 1.0);
        QCOMPARE(find("soundboardListeningVolume")->property("value").toDouble(), 1.0);
        enter(find("soundboardListeningPercent"), "101");
        QCOMPARE(preferences->soundboardVolume(), 1.0);
        enter(find("soundboardPopupListeningPercent"), "-1");
        QCOMPARE(preferences->soundboardVolume(), 1.0);
        QCOMPARE(voice->userVolume("111"), 1.0);
        QCOMPARE(voice->userSoundboardVolume("111"), 1.0);
    }
    void soundboardPopupListeningControlsFitSmallWindow() {
        auto *dialog = find("fixtureSoundboardDialog"); QVERIFY(dialog);
        QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
        QTest::qWait(30);
        QVERIFY(evaluate(dialog, "volumePopup.open()"));
        QTest::qWait(30);
        auto *window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        screenshot("soundboard-listening-popup");
        for (const char *name : {"soundboardPopupListeningVolume", "soundboardPopupListeningPercent"}) {
            auto *control = qobject_cast<QQuickItem *>(find(name)); QVERIFY(control);
            QVERIFY(control->isVisible());
            const auto bounds = control->mapRectToScene(QRectF(0, 0, control->width(), control->height()));
            QVERIFY2(QRectF(0, 0, window->width(), window->height()).contains(bounds), qPrintable(QString::fromLatin1(name) + QString(" bounds %1,%2 %3x%4").arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height())));
        }
    }
    void personControlsChangeOnlyTheirRespectiveGain() {
        move(find("personVoiceVolumeSlider"), .4);
        QCOMPARE(voice->userVolume("111"), .4);
        QCOMPARE(voice->userSoundboardVolume("111"), 1.0);
        enter(find("personSoundboardVolume"), "0");
        QCOMPARE(voice->userSoundboardVolume("111"), 0.0);
        QCOMPARE(voice->userVolume("111"), .4);
        move(find("personSoundboardVolumeSlider"), 2);
        QCOMPARE(voice->userSoundboardVolume("111"), 2.0);
        QCOMPARE(find("personSoundboardVolume")->property("value").toDouble(), 200.0);
        QCOMPARE(voice->userVolume("111"), .4);
        QCOMPARE(preferences->soundboardVolume(), .5);
        enter(find("personSoundboardVolume"), "201");
        QCOMPARE(voice->userSoundboardVolume("111"), 2.0);
        QVERIFY(members->setLocalSoundboardVolume("111", .85));
        QTRY_COMPARE(find("personSoundboardVolume")->property("value").toDouble(), 85.0);
        QCOMPARE(find("personSoundboardVolumeSlider")->property("value").toDouble(), .85);
        QCOMPARE(voice->userSoundboardVolume("222"), 1.0);
    }
    void soundboardSettingsRowFitsAndIsVisibleAfterScrolling() {
        auto *page = find("fixtureAudioSettings"); QVERIFY(page);
        QVERIFY(page->setProperty("visible", true));
        QTest::qWait(30);
        auto *view = page->property("contentItem").value<QQuickItem *>(); QVERIFY(view);
        auto *content = view->property("contentItem").value<QQuickItem *>(); QVERIFY(content);
        auto *heading = qobject_cast<QQuickItem *>(find("settingsSection_soundboard")); QVERIFY(heading);
        const auto top = heading->mapToItem(content, QPointF{}).y();
        view->setProperty("contentY", qBound(0.0, top - 10, qMax(0.0, view->property("contentHeight").toDouble() - view->height())));
        QTest::qWait(30);
        screenshot("soundboard-listening-settings");
        for (const char *name : {"soundboardListeningVolume", "soundboardListeningPercent"}) {
            auto *control = qobject_cast<QQuickItem *>(find(name)); QVERIFY(control);
            QVERIFY(control->isVisible());
            const auto bounds = control->mapRectToItem(view, QRectF(0, 0, control->width(), control->height()));
            QVERIFY2(QRectF(0, 0, view->width(), view->height()).contains(bounds), qPrintable(QString::fromLatin1(name) + QString(" bounds %1,%2 %3x%4 viewport %5x%6").arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height()).arg(view->width()).arg(view->height())));
        }
    }
    void personMenuFitsSmallWindowAndVolumeCanBeReached_data() {
        QTest::addColumn<double>("scale");
        QTest::newRow("normal") << 1.0;
        QTest::newRow("larger-text") << 1.25;
    }
    void personMenuFitsSmallWindowAndVolumeCanBeReached() {
        QFETCH(double, scale); preferences->setFontScale(scale);
        auto *menu = find("fixturePersonMenu"); QVERIFY(menu);
        QVERIFY(QMetaObject::invokeMethod(menu, "open"));
        QTest::qWait(30);
        auto *window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        QVERIFY(menu->property("visible").toBool());
        const auto height = menu->property("height").toDouble();
        QVERIFY(height > 0); QVERIFY(height <= window->height() - 16);
        auto *view = menu->property("contentItem").value<QQuickItem *>(); QVERIFY(view);
        const auto contentHeight = view->property("contentHeight").toDouble();
        QVERIFY(view->property("interactive").toBool() || contentHeight <= view->height());
        auto *slider = qobject_cast<QQuickItem *>(find("personSoundboardVolumeSlider")); QVERIFY(slider);
        const auto content = view->property("contentItem").value<QQuickItem *>(); QVERIFY(content);
        const auto top = slider->mapToItem(content, QPointF{}).y();
        view->setProperty("contentY", qBound(0.0, top - view->height() / 2, qMax(0.0, contentHeight - view->height())));
        QCoreApplication::processEvents();
        const auto bounds = slider->mapRectToItem(view, slider->boundingRect());
        QVERIFY(slider->isVisible());
        QVERIFY2(QRectF(0, 0, view->width(), view->height()).contains(bounds), "Per-person soundboard volume must be reachable inside the scrollable menu");
        screenshot(scale == 1 ? "person-menu" : "person-menu-larger-text");
    }
};
QTEST_MAIN(SoundboardVolumeUiTest)
#include "soundboard_volume_ui_test.moc"
