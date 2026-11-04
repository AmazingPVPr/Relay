#include <QtTest>
#include <QSignalSpy>
#include <QUrl>
#include <cmath>
#include <cstring>
#include "VoiceEngine.h"

class VoiceMicTest : public QObject {
    Q_OBJECT
    static QByteArray tone(int frames=1) {
        QByteArray pcm(frames*3840,Qt::Uninitialized);
        for(int i=0;i<frames*960;++i) {
            const qint16 value=qint16(8000*std::sin(2*3.141592653589793*440*i/48000));
            std::memcpy(pcm.data()+i*4,&value,2);std::memcpy(pcm.data()+i*4+2,&value,2);
        }
        return pcm;
    }
private slots:
    void localProcessedPlaybackAndCleanup() {
        VoiceEngine voice;
        QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        QSignalSpy requested(&voice,&VoiceEngine::voiceStateRequested);
        QVERIFY(!voice.micTesting());
        QVERIFY(!voice.fixtureCounters().value("micTimerActive").toBool());
        AudioProcessor::Options options;options.inputVolume=.5;options.outputVolume=.5;
        voice.configureAudio(options);
        bool preflightIsPrivate=false;QSignalSpy starting(&voice,&VoiceEngine::micTestStarting);
        connect(&voice,&VoiceEngine::micTestStarting,this,[&]{
            preflightIsPrivate=!voice.micTesting() && voice.fixtureCounters().value("micFrames").toInt()==0
                && voice.fixtureCounters().value("micQueuedBytes").toInt()==0 && voice.takeFixtureAudio().isEmpty();
        });
        voice.startMicTest();QVERIFY(voice.micTesting());
        QCOMPARE(starting.count(),1);QVERIFY(preflightIsPrivate);
        QVERIFY(voice.muted());QVERIFY(voice.deafened());
        const auto input=tone();voice.queueFixtureAudio(input);
        QTRY_COMPARE(voice.fixtureCounters().value("micFrames").toInt(),1);
        const auto output=voice.takeFixtureAudio();QCOMPARE(output.size(),input.size());
        for(int offset=0;offset<input.size();offset+=2) {
            qint16 before,after;std::memcpy(&before,input.constData()+offset,2);std::memcpy(&after,output.constData()+offset,2);
            QCOMPARE(after,qint16(qint16(before*.5)*.5));
        }
        QVERIFY(voice.inputLevel()>0);QCOMPARE(requested.count(),0);
        QCOMPARE(voice.fixtureCounters().value("sent").toInt(),0);
        voice.queueFixtureAudio(tone(10));
        QVERIFY(voice.fixtureCounters().value("micQueuedBytes").toInt()<=3840*12);
        voice.stopMicTest();
        QVERIFY(!voice.micTesting());QCOMPARE(voice.inputLevel(),0.0);
        QVERIFY(!voice.muted());QVERIFY(!voice.deafened());
        QVERIFY(!voice.fixtureCounters().value("micTimerActive").toBool());
        QCOMPARE(voice.fixtureCounters().value("micQueuedBytes").toInt(),0);
        QVERIFY(voice.takeFixtureAudio().isEmpty());
        const auto frames=voice.fixtureCounters().value("micFrames");QTest::qWait(60);
        QCOMPARE(voice.fixtureCounters().value("micFrames"),frames);
        QCOMPARE(requested.count(),0);
    }
    void joinStopsPrivateTest() {
        VoiceEngine voice;
        QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        if(!voice.available())QSKIP("Voice encryption is unavailable in this build");
        QSignalSpy requested(&voice,&VoiceEngine::voiceStateRequested);
        voice.startMicTest();voice.queueFixtureAudio(tone());
        voice.join("1111","2222","Fixture","3333");
        QVERIFY(!voice.micTesting());QVERIFY(!voice.fixtureCounters().value("micTimerActive").toBool());
        QCOMPARE(voice.fixtureCounters().value("micQueuedBytes").toInt(),0);
        QTRY_COMPARE(requested.count(),1);QCOMPARE(voice.fixtureCounters().value("sent").toInt(),0);
        voice.startMicTest();QVERIFY(voice.micTesting());QVERIFY(voice.muted());QVERIFY(voice.deafened());
        QCOMPARE(voice.channelId(),QString("2222"));QTRY_COMPARE(requested.count(),2);
        voice.leave();voice.startMicTest();QVERIFY(voice.micTesting());voice.stopMicTest();
    }
    void testRestoresVoiceChoices_data() {
        QTest::addColumn<bool>("muted");QTest::addColumn<bool>("deafened");
        QTest::newRow("open")<<false<<false;QTest::newRow("muted")<<true<<false;
        QTest::newRow("deafened")<<false<<true;QTest::newRow("both")<<true<<true;
    }
    void testRestoresVoiceChoices() {
        QFETCH(bool,muted);QFETCH(bool,deafened);
        VoiceEngine voice;QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        if(!voice.available())QSKIP("Voice encryption is unavailable in this build");
        voice.setMuted(muted);voice.setDeafened(deafened);voice.join("1111","2222","Fixture","3333");QCoreApplication::processEvents();
        QSignalSpy requested(&voice,&VoiceEngine::voiceStateRequested);
        voice.startMicTest();QVERIFY(voice.micTesting());QVERIFY(voice.muted());QVERIFY(voice.deafened());
        QTRY_COMPARE(requested.count(),1);QCOMPARE(requested.last(),QVariantList({"1111","2222",true,true}));
        voice.queueFixtureAudio(tone());QTRY_COMPARE(voice.fixtureCounters().value("micFrames").toInt(),1);
        QVERIFY(!voice.takeFixtureAudio().isEmpty());QVERIFY(!voice.playSoundboardPcm(tone()));
        voice.queueFixtureAudio(tone(6));voice.stopMicTest();
        QCOMPARE(voice.muted(),muted||deafened);QCOMPARE(voice.deafened(),deafened);QVERIFY(!voice.micTesting());
        QCOMPARE(voice.channelId(),QString("2222"));QCOMPARE(voice.currentGuildId(),QString("1111"));
        QTRY_COMPARE(requested.count(),2);QCOMPARE(requested.last(),QVariantList({"1111","2222",muted||deafened,deafened}));
        QCOMPARE(voice.fixtureCounters().value("micQueuedBytes").toInt(),0);QVERIFY(voice.takeFixtureAudio().isEmpty());
        // Repeating a stop must not send another voice update or change choices.
        voice.stopMicTest();QCOMPARE(requested.count(),2);
        voice.setDeafened(false);QCOMPARE(voice.muted(),muted);voice.leave();
    }
    void testFailureRestoresVoiceChoices_data() { testRestoresVoiceChoices_data(); }
    void testFailureRestoresVoiceChoices() {
        QFETCH(bool,muted);QFETCH(bool,deafened);
        VoiceEngine voice;QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        if(!voice.available())QSKIP("Voice encryption is unavailable in this build");
        voice.setMuted(muted);voice.setDeafened(deafened);voice.join("1111","2222","Fixture","3333");QCoreApplication::processEvents();
        QSignalSpy requested(&voice,&VoiceEngine::voiceStateRequested);
        voice.startMicTest();voice.queueFixtureAudio(tone(6));voice.failFixtureAudio("Generated microphone failure");
        QVERIFY(!voice.micTesting());QCOMPARE(voice.muted(),muted||deafened);QCOMPARE(voice.deafened(),deafened);
        QCOMPARE(voice.channelId(),QString("2222"));QCOMPARE(voice.error(),QString("Generated microphone failure"));
        QTRY_COMPARE(requested.count(),2);QCOMPARE(requested.last(),QVariantList({"1111","2222",muted||deafened,deafened}));
        QCOMPARE(voice.fixtureCounters().value("micQueuedBytes").toInt(),0);
        QVERIFY(!voice.fixtureCounters().value("micTimerActive").toBool());QVERIFY(voice.takeFixtureAudio().isEmpty());
        voice.setDeafened(false);QCOMPARE(voice.muted(),muted);voice.leave();
    }
    void togglesAndDeviceChangesDuringTest() {
        VoiceEngine voice;QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        voice.startMicTest();voice.queueFixtureAudio(tone(6));voice.setMuted(false);
        QVERIFY(!voice.micTesting());QVERIFY(!voice.muted());QVERIFY(!voice.deafened());
        QCOMPARE(voice.fixtureCounters().value("micQueuedBytes").toInt(),0);
        voice.startMicTest();voice.setDeafened(true);QVERIFY(!voice.micTesting());QVERIFY(voice.deafened());
        voice.setMuted(true);voice.startMicTest();voice.queueFixtureAudio(tone(6));
        voice.setInputDeviceId("generated-input");QVERIFY(voice.micTesting());
        QCOMPARE(voice.fixtureCounters().value("micQueuedBytes").toInt(),0);
        const auto beforeLastFrame=voice.fixtureCounters().value("micFrames").toULongLong();
        voice.queueFixtureAudio(tone());voice.setOutputDeviceId("generated-output");QVERIFY(voice.micTesting());
        QTRY_COMPARE(voice.fixtureCounters().value("micFrames").toULongLong(),beforeLastFrame+1);
        voice.stopMicTest();QVERIFY(voice.muted());QVERIFY(voice.deafened());
        QCOMPARE(voice.fixtureCounters().value("micQueuedBytes").toInt(),0);
    }
    void switchingAndDisconnectingDiscardTestAudio() {
        VoiceEngine voice;QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        if(!voice.available())QSKIP("Voice encryption is unavailable in this build");
        voice.setMuted(true);voice.join("1111","2222","Fixture","3333");voice.startMicTest();voice.queueFixtureAudio(tone(6));
        voice.join("1111","5555","Other fixture","3333");
        QVERIFY(!voice.micTesting());QVERIFY(voice.muted());QVERIFY(!voice.deafened());
        QCOMPARE(voice.channelId(),QString("5555"));QCOMPARE(voice.fixtureCounters().value("micQueuedBytes").toInt(),0);
        voice.startMicTest();voice.queueFixtureAudio(tone(6));
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QJsonValue::Null},{"user_id","3333"}});
        QVERIFY(!voice.micTesting());QVERIFY(voice.muted());QVERIFY(!voice.deafened());QVERIFY(voice.channelId().isEmpty());
        QCOMPARE(voice.fixtureCounters().value("micQueuedBytes").toInt(),0);QVERIFY(voice.takeFixtureAudio().isEmpty());
        QVERIFY(!voice.fixtureCounters().value("micTimerActive").toBool());
    }
};
QTEST_GUILESS_MAIN(VoiceMicTest)
#include "voice_mic_test.moc"
