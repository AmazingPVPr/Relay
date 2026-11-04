#include <QtTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QQmlPropertyMap>
#include <QVideoSink>
#include "AppSettings.h"
#include "addons/AddonManager.h"
#include "addons/BroadcastSourceInterface.h"

class ShareVoiceFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected MEMBER connected CONSTANT)
    Q_PROPERTY(QString currentGuildId MEMBER guild CONSTANT)
    Q_PROPERTY(QString channelId MEMBER channel CONSTANT)
public:
    bool connected = true;
    QString guild = "100", channel = "101";
};
class ShareDiscordFixture final : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE bool isDirectCall(const QString &) const { return false; }
};
class ShareBroadcastFixture final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool available MEMBER available CONSTANT)
    Q_PROPERTY(bool usesSystemPicker MEMBER systemPicker CONSTANT)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(QString status MEMBER status NOTIFY changed)
    Q_PROPERTY(QString sourceName MEMBER sourceName NOTIFY changed)
    Q_PROPERTY(QString qualitySummary MEMBER quality NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
public:
    bool activeState = false, available = true, systemPicker = false;
    int screenStarts = 0, height = -1, fps = -1;
    bool audio = false;
    QString status = "Starting stream", sourceName, quality, error;
    bool active() const { return activeState; }
    QVariantList sources() const { return {QVariantMap{{"id", "generated-screen"}, {"name", "Generated screen"}}}; }
    Q_INVOKABLE void refreshSources() { emit sourcesChanged(); }
    Q_INVOKABLE void start(const QString &, const QString &, const QString &id, int h, int rate, bool sound) {
        ++screenStarts; height = h; fps = rate; audio = sound; sourceName = id; activeState = true; emit changed();
    }
    Q_INVOKABLE void stop() { activeState = false; emit changed(); }
    void pendingAddon() { activeState = true; emit changed(); }
signals:
    void changed();
    void sourcesChanged();
};
class ShareBridgeFixture final : public QObject {
    Q_OBJECT
public:
    AddonManager *addons = nullptr;
    ShareBroadcastFixture *broadcast = nullptr;
    int starts = 0, height = -1, fps = -1;
    bool audio = false;
    Q_INVOKABLE bool startSource(const QString &id, int h, int rate, bool sound) {
        auto *object = addons->openBroadcastSource(id).value("service").value<QObject *>();
        auto *source = qobject_cast<RelayBroadcastSourceInterface *>(object);
        if (!source || !object->property("receiving").toBool()) return false;
        ++starts; height = h; fps = rate; audio = sound;
        source->setBroadcastContext({{"active", true}, {"sharing", false}});
        broadcast->pendingAddon();
        return true;
    }
};

class ObsPreviewUiFixture final : public QQmlPropertyMap {
    Q_OBJECT
public:
    int released = 0, stopped = 0;
    ObsPreviewUiFixture() : QQmlPropertyMap(this, nullptr) {
        insert("videoSink", QVariant::fromValue<QObject *>(nullptr));insert("inputMode",1);
        insert("running",true);insert("receiving",true);insert("hasAudio",true);insert("broadcastAvailable",true);
        insert("broadcastContext",QVariantMap{{"active",true},{"sharing",true},{"voiceAvailable",true},{"channelName","Generated voice"}});
        insert("status","Generated input");insert("resolution","320 × 180");
        for(const auto *key:{"remoteServer","remoteStreamKey","obsServer","streamKey","inputStorageStatus","inputStorageError","error"})insert(key,"");
        for(const auto *key:{"rememberInput","inputSaved","inputStorageBusy"})insert(key,false);
    }
    Q_INVOKABLE void releasePreview(QObject *sink) {
        if(!sink || value("videoSink").value<QObject *>()!=sink)return;
        ++released;insert("videoSink",QVariant::fromValue<QObject *>(nullptr));
    }
    Q_INVOKABLE void stop() { ++stopped; }
    Q_INVOKABLE void start() {}
    Q_INVOKABLE void copyObsServer() {}
    Q_INVOKABLE void copyStreamKey() {}
    Q_INVOKABLE void forgetSavedInput() {}
    Q_INVOKABLE void retryInputSave() {}
    Q_INVOKABLE void requestBroadcast(int,int,bool) {}
    Q_INVOKABLE void stopBroadcast() {}
};

class ShareSourceUiTest final : public QObject {
    Q_OBJECT
    QTemporaryDir files;
    std::unique_ptr<AddonManager> addons;
    std::unique_ptr<AppSettings> preferences;
    std::unique_ptr<QQmlEngine> engine;
    std::unique_ptr<QObject> root;
    ShareVoiceFixture voice;
    ShareDiscordFixture discord;
    ShareBroadcastFixture broadcast;
    ShareBridgeFixture bridge;
    QObject *dialog = nullptr;
    QStringList warnings;
    QQmlPropertyMap relayStreaming;
    QObject *find(const char *name) {
        const auto wanted = QString::fromLatin1(name);
        if (auto *object = root->findChild<QObject *>(wanted)) return object;
        // Loader-created Items are visual children but need not be QObject
        // children of the declarative owner. Inspect their exposed item roots.
        for (auto *object : root->findChildren<QObject *>()) {
            auto *loaded = object->property("item").value<QObject *>();
            if (!loaded) continue;
            if (loaded->objectName() == wanted) return loaded;
            if (auto *child = loaded->findChild<QObject *>(wanted)) return child;
        }
        return nullptr;
    }
    bool evaluate(const QString &code) {
        QQmlExpression expression(engine->rootContext(), dialog, code); expression.evaluate();
        if (expression.hasError()) { qWarning() << expression.error(); return false; }
        QCoreApplication::processEvents(); return true;
    }
    void open() { QVERIFY(QMetaObject::invokeMethod(dialog, "open")); QCoreApplication::processEvents(); }
    void chooseAddon() {
        QVERIFY(evaluate("selectSource('addon:org.relay.fixture')"));
        QTRY_VERIFY_WITH_TIMEOUT(find("fixtureShareEditor"), 1500);
    }
private slots:
    void init() {
        QVERIFY(files.isValid()); warnings.clear();
        broadcast.activeState = false; broadcast.screenStarts = 0;
        bridge.starts = 0;
        qApp->setProperty("addonFixtureServicesCreated", 0);
        qApp->setProperty("addonFixtureShareUrl", QUrl{});
        const auto file = files.filePath(QString::number(QDateTime::currentMSecsSinceEpoch()) + ".ini");
        addons = std::make_unique<AddonManager>(QStringList{QFileInfo(QStringLiteral(RELAY_TEST_ADDON_PATH)).absolutePath()}, QString{}, file);
        preferences = std::make_unique<AppSettings>(file);
        bridge.addons = addons.get(); bridge.broadcast = &broadcast;
        engine = std::make_unique<QQmlEngine>();
        connect(engine.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors) warnings.append(error.toString());
        });
        relayStreaming.insert("publishEnabled",true);relayStreaming.insert("bitrateMbps",20);relayStreaming.insert("viewerCount",0);
        auto *context = engine->rootContext();
        context->setContextProperty("relayStreams", &relayStreaming);
        context->setContextProperty("addons", addons.get());
        context->setContextProperty("addonBroadcast", &bridge);
        context->setContextProperty("broadcast", &broadcast);
        context->setContextProperty("voice", &voice);
        context->setContextProperty("discord", &discord);
        context->setContextProperty("preferences", preferences.get());
        const auto directory = QFileInfo(QFINDTESTDATA("../qml/components/ScreenShareDialog.qml")).absolutePath();
        QQmlComponent component(engine.get());
        const auto fixture = QString(R"(
import QtQuick
import QtQuick.Controls
import "%1"
ApplicationWindow {
    width: 1024; height: 900; visible: true
    ScreenShareDialog { objectName: "fixtureShareDialog" }
}
)").arg(QUrl::fromLocalFile(directory).toString());
        component.setData(fixture.toUtf8(), QUrl::fromLocalFile(directory + "/generated-share-fixture.qml"));
        root.reset(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        dialog = find("fixtureShareDialog"); QVERIFY(dialog);
    }
    void cleanup() {
        root.reset(); engine.reset(); addons.reset(); preferences.reset();
        QCoreApplication::processEvents();
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }
    void obsSettingsHiddenDuringBroadcastReleasesOnlyItsPreview() {
        // Import the real add-on QML with only qrc paths mapped to the source
        // tree. No behavior is substituted; the service uses generated state.
        qmlRegisterType<QQuickItem>("Relay.Native",1,0,"SoftwareVideo");
        engine->rootContext()->setContextProperty("softwareRendering",false);
        ObsPreviewUiFixture obs;
        engine->rootContext()->setContextProperty("fixtureObs",&obs);
        QTemporaryDir qml;
        const auto components=QUrl::fromLocalFile(QFileInfo(QFINDTESTDATA("../qml/components/ScreenShareDialog.qml")).absolutePath()).toString();
        for(const auto &name:{QString("Input.qml"),QString("Settings.qml")}) {
            QFile original(QFileInfo(QFINDTESTDATA("../addons/obs/Input.qml")).absolutePath()+"/"+name);QVERIFY(original.open(QIODevice::ReadOnly));
            auto source=original.readAll();
            source.replace("qrc:/Relay/components",components.toUtf8());
            source.replace("import Relay\n",("import \""+components+"\"\n").toUtf8());
            QFile output(qml.filePath(name));QVERIFY(output.open(QIODevice::WriteOnly));QCOMPARE(output.write(source),source.size());output.close();
        }
        QQmlComponent component(engine.get());
        component.setData(R"(
import QtQuick
import QtQuick.Controls
ApplicationWindow {
    visible: true; width: 1024; height: 900
    Settings { id: settings; objectName: "generatedObsSettings"; service: fixtureObs }
})",QUrl::fromLocalFile(qml.filePath("GeneratedObsSettingsFixture.qml")));
        QScopedPointer<QObject> holder(component.create());QVERIFY2(holder,qPrintable(component.errorString()));
        auto *settings=holder->findChild<QObject *>("generatedObsSettings");QVERIFY(settings);
        QCoreApplication::processEvents();
        QVERIFY(!obs.value("videoSink").value<QObject *>());
        QVERIFY(QMetaObject::invokeMethod(settings,"open"));
        QTRY_VERIFY(obs.value("videoSink").value<QObject *>());
        auto *first=obs.value("videoSink").value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(settings,"close"));
        QTRY_VERIFY(!obs.value("videoSink").value<QObject *>());
        QCOMPARE(obs.released,1);QCOMPARE(obs.stopped,0);
        QVERIFY(QMetaObject::invokeMethod(settings,"open"));
        QTRY_COMPARE(obs.value("videoSink").value<QObject *>(),first);
        QVideoSink replacement;obs.insert("videoSink",QVariant::fromValue<QObject *>(&replacement));
        QVERIFY(QMetaObject::invokeMethod(settings,"close"));QTest::qWait(30);
        QCOMPARE(obs.value("videoSink").value<QObject *>(),&replacement);
        QCOMPARE(obs.released,1);QCOMPARE(obs.stopped,0);
        holder.reset();QCOMPARE(obs.value("videoSink").value<QObject *>(),&replacement);
    }
    void openingAndCancelNeverCaptureOrShare() {
        open();
        QCOMPARE(dialog->property("sourceChoices").toList().size(), 2);
        QCOMPARE(qApp->property("addonFixtureServicesCreated").toInt(), 0);
        chooseAddon();
        QCOMPARE(qApp->property("addonFixtureServicesCreated").toInt(), 1);
        auto *service = addons->openBroadcastSource("org.relay.fixture").value("service").value<QObject *>();
        QVERIFY(service); QVERIFY(!service->property("running").toBool());
        QVERIFY(!find("screenShareStart")->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(find("fixtureConnectInput"), "clicked"));
        QVERIFY(service->property("receiving").toBool());
        QTRY_VERIFY(find("screenShareStart")->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!service->property("running").toBool());
        QCOMPARE(bridge.starts, 0); QCOMPARE(broadcast.screenStarts, 0);
    }
    void explicitStartUsesSourceQualityAndKeepsPendingInputAlive() {
        open(); chooseAddon();
        auto *service = addons->openBroadcastSource("org.relay.fixture").value("service").value<QObject *>();
        QVERIFY(QMetaObject::invokeMethod(find("fixtureConnectInput"), "clicked"));
        QTRY_VERIFY(find("screenShareStart")->property("enabled").toBool());
        QCOMPARE(dialog->property("selectedHeight").toInt(), 0);
        QCOMPARE(dialog->property("selectedRate").toInt(), 0);
        QVERIFY(QMetaObject::invokeMethod(find("screenShareStart"), "clicked"));
        QCOMPARE(bridge.starts, 1); QCOMPARE(bridge.height, 0); QCOMPARE(bridge.fps, 0); QVERIFY(bridge.audio);
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(service->property("running").toBool());
        QVERIFY(service->property("broadcastContext").toMap().value("active").toBool());
        QVERIFY(!service->property("broadcastContext").toMap().value("sharing").toBool());
        QCOMPARE(broadcast.screenStarts, 0);
    }
    void disablingSelectionRemovesEditorAndReturnsToScreen() {
        open(); chooseAddon();
        QVERIFY(addons->setEnabled("org.relay.fixture", false));
        QTRY_VERIFY_WITH_TIMEOUT(!find("fixtureShareEditor"), 1500);
        QVERIFY(!dialog->property("addonSelected").toBool());
        QCOMPARE(dialog->property("sourceChoices").toList().size(), 1);
        QCOMPARE(bridge.starts, 0);
    }
    void commonControlsStayVisibleInNarrowWindow() {
        auto *window = qobject_cast<QQuickWindow *>(root.get()); QVERIFY(window);
        window->resize(960, 640); open(); chooseAddon(); QTest::qWait(30);
        const QRectF windowBounds(0, 0, window->width(), window->height());
        for (const char *name : {"screenShareResolution", "screenShareFrameRate", "screenShareAudio", "screenShareStart"}) {
            auto *control = qobject_cast<QQuickItem *>(find(name)); QVERIFY(control);
            QVERIFY(control->isVisible());
            const auto bounds = control->mapRectToScene(control->boundingRect());
            QVERIFY2(windowBounds.contains(bounds), name);
        }
    }
    void regularScreenPathRetainsQualityAudioAndStart() {
        open(); QVERIFY(!dialog->property("addonSelected").toBool());
        QVERIFY(find("relayStreamEnabled")->property("checked").toBool());
        QCOMPARE(find("relayStreamBitrate")->property("value").toInt(),20);
        QCOMPARE(dialog->property("selectedHeight").toInt(), 2160);
        QCOMPARE(dialog->property("selectedRate").toInt(), 60);
        QVERIFY(QMetaObject::invokeMethod(find("screenShareStart"), "clicked"));
        QCOMPARE(broadcast.screenStarts, 1); QCOMPARE(broadcast.sourceName, "generated-screen");
        QCOMPARE(broadcast.height, 2160); QCOMPARE(broadcast.fps, 60); QVERIFY(!broadcast.audio);
        QCOMPARE(bridge.starts, 0); QCOMPARE(qApp->property("addonFixtureServicesCreated").toInt(), 0);
    }
};
QTEST_MAIN(ShareSourceUiTest)
#include "share_source_ui_test.moc"
