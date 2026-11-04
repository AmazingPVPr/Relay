#include <QtTest>
#include <QTemporaryDir>
#include <QFileInfo>
#include "addons/AddonManager.h"
#include "addons/AddonBroadcastBridge.h"
#include "addons/BroadcastSourceInterface.h"
#include "BroadcastEngine.h"

class BroadcastSourceFixture final : public QObject, public RelayBroadcastSourceInterface {
    Q_OBJECT
    Q_INTERFACES(RelayBroadcastSourceInterface)
    Q_PROPERTY(bool running MEMBER running)
    Q_PROPERTY(bool receiving MEMBER receiving)
public:
    bool running=true,receiving=true,capturing=false;
    int height=0,rate=0;bool audio=false;
    QVariantMap context;
    void setBroadcastContext(const QVariantMap &value) override {context=value;}
    void setBroadcastCaptureEnabled(bool enabled,int maximumHeight,int fps,bool sound) override {capturing=enabled;height=maximumHeight;rate=fps;audio=sound;}
    RelayBroadcastMedia takeBroadcastMedia() override {return {};}
    void request(bool streamAudio=true){emit broadcastRequested(1080,30,streamAudio);}
    void stop(){emit broadcastStopRequested();}
signals:
    void broadcastRequested(int height,int fps,bool audio);
    void broadcastStopRequested();
};

class AddonBroadcastTest final : public QObject {
    Q_OBJECT
private slots:
    void regularShareContributionUsesHostLifecycle() {
        QTemporaryDir files; QVERIFY(files.isValid());
        AddonManager addons({QFileInfo(QStringLiteral(RELAY_TEST_ADDON_PATH)).absolutePath()}, {}, files.filePath("settings.ini"));
        BroadcastEngine engine; if (!engine.available()) QSKIP("Broadcast codec/encryption dependencies unavailable");
        AddonBroadcastBridge bridge(&addons, &engine);
        QVERIFY(!bridge.startSource("org.unknown.source", 0, 0, true));
        auto *service = addons.openBroadcastSource("org.relay.fixture").value("service").value<QObject *>();
        QVERIFY(service);
        QVERIFY(!bridge.startSource("org.relay.fixture", 0, 0, true));
        bridge.setVoiceContext(true, "100", "101", "Fixture room");
        QVERIFY(!bridge.startSource("org.relay.fixture", 0, 0, true));
        QVERIFY(QMetaObject::invokeMethod(service, "start"));
        QVERIFY(bridge.startSource("org.relay.fixture", 0, 0, true));
        QVERIFY(engine.active()); QVERIFY(engine.externalSource()); QVERIFY(service->property("capturing").toBool());
        const auto state = service->property("broadcastContext").toMap();
        QVERIFY(state.value("active").toBool()); QVERIFY(!state.value("sharing").toBool());
        QVERIFY(addons.setEnabled("org.relay.fixture", false));
        QVERIFY(!engine.active()); QVERIFY(!service->property("capturing").toBool());
        QVERIFY(!bridge.startSource("org.relay.fixture", 0, 0, true));
    }
    void temporaryAudioGatePreservesSourceAndRequestedAudio() {
        QTemporaryDir files;QVERIFY(files.isValid());
        AddonManager addons({QFileInfo(QStringLiteral(RELAY_TEST_OBS_ADDON_PATH)).absolutePath()},{},files.filePath("settings.ini"));
        BroadcastEngine engine;if(!engine.available())QSKIP("Broadcast codec/encryption dependencies unavailable");
        AddonBroadcastBridge bridge(&addons,&engine);BroadcastSourceFixture source;addons.serviceCreated("org.relay.obs",&source);
        bridge.setVoiceContext(true,"100","101","Fixture room");
        engine.setAudioSuppressed(true);QVERIFY(engine.audioSuppressed());source.request();
        QVERIFY(engine.active());QVERIFY(source.capturing);QVERIFY(source.audio);QVERIFY(!engine.audioActive());
        engine.setAudioSuppressed(false);QVERIFY(engine.audioSuppressed());QTest::qWait(150);QVERIFY(engine.audioSuppressed());
        QTRY_VERIFY_WITH_TIMEOUT(!engine.audioSuppressed(),1000);
        QVERIFY(engine.active());QVERIFY(source.capturing);QVERIFY(source.audio);
        engine.setAudioSuppressed(true);engine.stop();QVERIFY(engine.audioSuppressed());
        QVERIFY(!source.capturing);QVERIFY(source.running);
        source.request(false);QVERIFY(engine.active());QVERIFY(source.capturing);QVERIFY(!source.audio);
        engine.setAudioSuppressed(false);QTRY_VERIFY_WITH_TIMEOUT(!engine.audioSuppressed(),1000);
        QVERIFY(!engine.audioActive());QVERIFY(source.capturing);QVERIFY(!source.audio);
        engine.stop();engine.setAudioSuppressed(true);engine.setAudioSuppressed(false);
        QVERIFY(engine.audioSuppressed());QTRY_VERIFY_WITH_TIMEOUT(!engine.audioSuppressed(),1000);
        QVERIFY(!engine.active());QVERIFY(source.running);
    }
    void hostOwnsConsentChannelAndLifecycle() {
        QTemporaryDir files;QVERIFY(files.isValid());
        AddonManager addons({QFileInfo(QStringLiteral(RELAY_TEST_OBS_ADDON_PATH)).absolutePath()},{},files.filePath("settings.ini"));
        BroadcastEngine engine;if(!engine.available())QSKIP("Broadcast codec/encryption dependencies unavailable");
        AddonBroadcastBridge bridge(&addons,&engine);
        BroadcastSourceFixture unknown;addons.serviceCreated("org.unknown.source",&unknown);unknown.request();QVERIFY(!engine.active());QVERIFY(unknown.context.isEmpty());
        auto source=std::make_unique<BroadcastSourceFixture>();addons.serviceCreated("org.relay.obs",source.get());
        QVERIFY(source->context.value("available").toBool());QVERIFY(!source->context.value("voiceAvailable").toBool());
        source->request();QVERIFY(!engine.active());QVERIFY(source->context.value("error").toString().contains("Join"));
        bridge.setVoiceContext(true,"100","101","Fixture room");source->receiving=false;source->request();QVERIFY(!engine.active());QVERIFY(source->context.value("error").toString().contains("Connect"));
        source->receiving=true;source->request();QVERIFY(engine.active());QVERIFY(engine.externalSource());QVERIFY(source->capturing);QCOMPARE(source->height,1080);QCOMPARE(source->rate,30);QVERIFY(source->audio);
        QVERIFY(source->context.value("active").toBool());QCOMPARE(source->context.value("channelName").toString(),"Fixture room");
        for(const auto&key:source->context.keys())QVERIFY(!key.contains("token",Qt::CaseInsensitive)&&!key.contains("session",Qt::CaseInsensitive)&&!key.endsWith("Id"));
        source->stop();QVERIFY(!engine.active());QVERIFY(!source->capturing);QVERIFY(source->running);
        source->request();QVERIFY(engine.active());bridge.setVoiceContext(true,"100","102","Other room");QVERIFY(!engine.active());QVERIFY(!source->capturing);
        source->request();QVERIFY(engine.active());bridge.setVoiceContext(false,{},{},{});QVERIFY(!engine.active());QVERIFY(!source->context.value("voiceAvailable").toBool());
        bridge.setVoiceContext(true,"100","101");source->request();QVERIFY(engine.active());QVERIFY(addons.setEnabled("org.relay.obs",false));QVERIFY(!engine.active());QVERIFY(!source->capturing);QVERIFY(!source->context.value("available").toBool());
        source->request();QVERIFY(!engine.active());
        QVERIFY(addons.setEnabled("org.relay.obs",true));source->request();QVERIFY(engine.active());source.reset();QVERIFY(!engine.active());
    }
};
QTEST_MAIN(AddonBroadcastTest)
#include "addon_broadcast_test.moc"
