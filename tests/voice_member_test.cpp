#include "VoiceEngine.h"

#include <QSignalSpy>
#include <QIODevice>
#include <QUrl>
#include <QtTest>
#include <opus/opus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>
#include <mutex>

// Generated push-mode output: an independent synthetic device clock consumes
// queued samples, while writes obey both capacity and partial-write limits.
class ClockedAudioOutput final : public QIODevice {
    mutable std::mutex mutex;
    QByteArray queued,played;
    const qint64 capacity,maxWrite;bool accepting=true;
public:
    ClockedAudioOutput(qint64 bufferBytes,qint64 maxWriteBytes):capacity(bufferBytes),maxWrite(maxWriteBytes){open(QIODevice::WriteOnly);}
    bool isSequential()const override{return true;}
    void setAccepting(bool value){std::lock_guard lock(mutex);accepting=value;}
    qint64 bytesToWrite()const override{std::lock_guard lock(mutex);return queued.size();}
    void advance(int milliseconds){std::lock_guard lock(mutex);const auto count=std::min<qsizetype>(queued.size(),milliseconds*192);played+=queued.first(count);queued.remove(0,count);}
    QByteArray takePlayed(){std::lock_guard lock(mutex);QByteArray result;result.swap(played);return result;}
protected:
    qint64 readData(char *,qint64)override{return -1;}
    qint64 writeData(const char *data,qint64 size)override{
        std::lock_guard lock(mutex);if(!accepting)return 0;const auto count=std::max<qint64>(0,std::min({size,capacity-queued.size(),maxWrite}));
        queued.append(data,count);return count;
    }
};

class VoiceMemberTest final : public QObject {
    Q_OBJECT
    static QList<QByteArray> packets(double frequency, double amplitude, int frames=8,int frameSamples=960) {
        int error = OPUS_OK;
        OpusEncoder *encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
        if (!encoder || error != OPUS_OK) return {};
        QList<QByteArray> result;
        for (int frame = 0; frame < frames; ++frame) {
            std::vector<opus_int16> pcm(frameSamples*2);
            for (int i = 0; i < frameSamples; ++i) pcm[i * 2] = pcm[i * 2 + 1] = qint16(amplitude * std::sin(2 * 3.141592653589793 * frequency * (i + frame * frameSamples) / 48000));
            QByteArray packet(1275, Qt::Uninitialized);
            const int size = opus_encode(encoder, pcm.data(), frameSamples, reinterpret_cast<unsigned char *>(packet.data()), packet.size());
            if (size <= 0) { result.clear(); break; }
            packet.resize(size); result.append(packet);
        }
        opus_encoder_destroy(encoder);
        return result;
    }
    static bool receive(VoiceEngine &voice,const QString &user,const QByteArray &pcm,double volume) {
        return voice.playReceivedSoundboardPcm(user,pcm,volume,voice.soundboardPlaybackGeneration());
    }
    static void queue(VoiceEngine &voice, const QString &id, quint32 ssrc, const QList<QByteArray> &audio) {
        for (int i = 0; i < audio.size(); ++i) voice.queueFixturePeerAudio(id, ssrc, quint16(i), audio.at(i));
    }
private slots:
    void remoteVoiceStatesNeverRequestLocalActionCues() {
        VoiceEngine voice;
        QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        QVERIFY(voice.available());
        QSignalSpy cues(&voice,&VoiceEngine::localAudioCueRequested);
        QSignalSpy requested(&voice,&VoiceEngine::voiceStateRequested);
        voice.join("100","200","Fixture","42");
        QVERIFY(voice.matchesAccount("42"));
        QCOMPARE(voice.currentGuildId(),QString("100"));
        QCOMPARE(voice.channelId(),QString("200"));
        voice.onVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","42"},{"session_id","fixture"},
            {"self_mute",false},{"self_deaf",false},{"mute",false},{"deaf",false}});
        for(const auto &channel:{QString("200"),QString("201"),QString()}) {
            for(bool mute:{true,false})for(bool deaf:{true,false}) {
                voice.onVoiceState({{"guild_id","100"},{"channel_id",channel.isEmpty()?QJsonValue::Null:QJsonValue(channel)},
                    {"user_id","43"},{"session_id","peer-fixture"},{"self_mute",mute},{"self_deaf",deaf},{"mute",mute},{"deaf",deaf}});
                QVERIFY(voice.matchesAccount("42"));
                QCOMPARE(voice.channelId(),QString("200"));
                QVERIFY(!voice.muted());QVERIFY(!voice.deafened());
            }
        }
        // Server moderation of the selected account also is not a local action.
        voice.onVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","42"},{"mute",true},{"deaf",true}});
        voice.onVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","42"},{"mute",false},{"deaf",false}});
        QTRY_COMPARE(requested.size(),1);
        QCoreApplication::processEvents();QCOMPARE(cues.size(),0);
        voice.leave();voice.onAccountState("44");
        QVERIFY(voice.matchesAccount("44"));QVERIFY(voice.channelId().isEmpty());
        QCoreApplication::processEvents();QCOMPARE(cues.size(),0);
    }

    void localActionCuesKeepEachRapidSnapshotAndIgnoreNoops() {
        VoiceEngine voice;
        QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        QSignalSpy cues(&voice,&VoiceEngine::localAudioCueRequested);
        voice.setMuted(false);voice.setDeafened(false);
        voice.setMuted(true);voice.setMuted(true);voice.setMuted(false);voice.setMuted(false);
        voice.setDeafened(true);voice.setDeafened(true);voice.setDeafened(false);voice.setDeafened(false);
        QVERIFY(!voice.muted());QVERIFY(!voice.deafened());
        // Runtime actions finish before their GUI notifications are delivered.
        QCOMPARE(cues.size(),0);
        QTRY_COMPARE(cues.size(),4);
        const QStringList expected{"mute","unmute","deafen","undeafen"};
        for(int i=0;i<expected.size();++i)QCOMPARE(cues.at(i),QVariantList{expected.at(i)});
        voice.setMuted(false);voice.setDeafened(false);voice.leave();voice.onAccountState("44");
        QCoreApplication::processEvents();QCOMPARE(cues.size(),4);
    }

    void micTestIsolationIsQuietButExplicitExitActionsHaveCues() {
        VoiceEngine voice;
        QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        QSignalSpy cues(&voice,&VoiceEngine::localAudioCueRequested);
        voice.startMicTest();QVERIFY(voice.micTesting());QVERIFY(voice.muted());QVERIFY(voice.deafened());
        voice.stopMicTest();QVERIFY(!voice.micTesting());QVERIFY(!voice.muted());QVERIFY(!voice.deafened());
        QCoreApplication::processEvents();QCOMPARE(cues.size(),0);
        // Exiting temporary isolation via an explicit unmute/undeafen is audible,
        // even when the saved choice was already open before the test started.
        voice.startMicTest();voice.setMuted(false);QVERIFY(!voice.micTesting());
        voice.startMicTest();voice.setDeafened(false);QVERIFY(!voice.micTesting());
        // An explicit change of the saved choice is audible even when the test
        // already made the effective state muted/deafened.
        voice.startMicTest();voice.setMuted(true);QVERIFY(!voice.micTesting());QVERIFY(voice.muted());
        voice.startMicTest();voice.stopMicTest();QVERIFY(voice.muted());QVERIFY(!voice.deafened());
        voice.startMicTest();voice.setDeafened(true);QVERIFY(!voice.micTesting());QVERIFY(voice.deafened());
        QTRY_COMPARE(cues.size(),4);
        const QStringList expected{"unmute","undeafen","mute","deafen"};
        for(int i=0;i<expected.size();++i)QCOMPARE(cues.at(i),QVariantList{expected.at(i)});
        voice.setMuted(true);voice.setDeafened(true);voice.startMicTest();voice.stopMicTest();
        QCoreApplication::processEvents();QCOMPARE(cues.size(),4);
    }

    void timedPeerPlaybackBoundsStaleAudioAfterStall_data() {
        QTest::addColumn<int>("gapFrames");
        QTest::newRow("normal-no-stall")<<1;
        QTest::newRow("400ms-receive-stall")<<20;
    }
    void timedPeerPlaybackBoundsStaleAudioAfterStall() {
        QFETCH(int,gapFrames);
        VoiceEngine voice;QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        voice.configureAudio({false,false,false,1.,1.,-70});
        const auto audio=packets(431,6000,64);QCOMPARE(audio.size(),64);
        int error=0;auto *decoder=opus_decoder_create(48000,2,&error);QCOMPARE(error,OPUS_OK);
        QByteArray reference;
        for(const auto &packet:audio){
            std::array<opus_int16,1920> pcm{};
            QCOMPARE(opus_decode(decoder,reinterpret_cast<const unsigned char*>(packet.constData()),packet.size(),pcm.data(),960,0),960);
            reference.append(reinterpret_cast<const char*>(pcm.data()),3840);
        }
        opus_decoder_destroy(decoder);
        const auto offer=[&](int frame){voice.queueFixturePeerAudio("111",10,quint16(frame),audio.at(frame));};
        for(int frame=0;frame<3;++frame)offer(frame);
        voice.processFixturePlaybackAt(20);offer(3);voice.processFixturePlaybackAt(40);
        QCOMPARE(voice.takeFixtureAudio(),reference.left(2*3840));
        // Packets arrive as a complete burst after a late receive wake. Invoke
        // the real wall-clock scheduler; the direct mixer fixture misses this.
        for(int frame=4;frame<4+gapFrames;++frame)offer(frame);
        const int skippedFrames=std::max(0,gapFrames-10);
        const qint64 resumeAt=40+gapFrames*20;
        voice.processFixturePlaybackAt(resumeAt);
        const auto recovery=voice.takeFixtureAudio();
        qInfo("Timed receive: gap=%d ms, expected skipped=%d frames, recovery=%d frames",gapFrames*20,skippedFrames,int(recovery.size()/3840));
        QVERIFY2(recovery==reference.mid((2+skippedFrames)*3840,std::min(10,gapFrames)*3840),
            "A late receive wake retained old speech after the bounded catch-up budget");
        for(int frame=4+gapFrames;frame<24+gapFrames;++frame){
            offer(frame);voice.processFixturePlaybackAt(resumeAt+(frame-(3+gapFrames))*20);
            QVERIFY2(voice.takeFixtureAudio()==reference.mid((frame-2)*3840,3840),
                "Playback kept a stale tail after normal receive timing resumed");
        }
        const auto diagnostics=voice.playbackDiagnostics();
        QCOMPARE(diagnostics.value("receiveRebases").toULongLong(),quint64(skippedFrames?1:0));
        QCOMPARE(diagnostics.value("receiveSkippedSlots").toULongLong(),quint64(skippedFrames));
        QCOMPARE(diagnostics.value("receiveDiscardedSamples").toULongLong(),quint64(skippedFrames*960));
        QCOMPARE(diagnostics.value("receiveDiscardedPackets").toULongLong(),quint64(skippedFrames));
        QCOMPARE(diagnostics.value("receiveConcealedPackets").toULongLong(),quint64(0));
        QCOMPARE(diagnostics.value("receiveQueuedMs").toInt(),40);
        QCOMPARE(voice.fixtureCounters().value("sent").toULongLong(),quint64(0));
    }

    void boundedOutputRecoveryDoesNotKeepSoftwarePcmTail_data() {
        QTest::addColumn<int>("gapFrames");QTest::addColumn<int>("maxWriteBytes");QTest::addColumn<bool>("missingOldest");
        QTest::newRow("normal-80ms-device")<<1<<3840<<false;
        QTest::newRow("normal-short-writes")<<1<<1280<<false;
        QTest::newRow("400ms-stall-80ms-device")<<20<<3840<<false;
        QTest::newRow("400ms-stall-short-writes")<<20<<1280<<false;
        QTest::newRow("400ms-stall-missing-oldest")<<20<<3840<<true;
    }
    void boundedOutputRecoveryDoesNotKeepSoftwarePcmTail() {
        QFETCH(int,gapFrames);QFETCH(int,maxWriteBytes);QFETCH(bool,missingOldest);
        ClockedAudioOutput output(4*3840,maxWriteBytes);VoiceEngine voice;
        QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));voice.setFixtureAudioOutput(&output,4*3840);
        voice.configureAudio({false,false,false,1.,1.,-70});
        const auto audio=packets(431,6000,64);int error=0;auto *decoder=opus_decoder_create(48000,2,&error);QCOMPARE(error,OPUS_OK);
        QByteArray reference;
        for(int frame=0;frame<audio.size();++frame){const auto &packet=audio.at(frame);std::array<opus_int16,1920> pcm{};
            const bool missing=missingOldest && frame==2;
            QCOMPARE(opus_decode(decoder,missing?nullptr:reinterpret_cast<const unsigned char*>(packet.constData()),missing?0:packet.size(),pcm.data(),960,0),960);
            reference.append(reinterpret_cast<const char*>(pcm.data()),3840);
        }
        opus_decoder_destroy(decoder);
        const auto offer=[&](int frame){voice.queueFixturePeerAudio("111",10,quint16(frame),audio.at(frame));};
        offer(0);offer(1);offer(missingOldest?3:2);
        voice.processFixturePlaybackAt(20);output.advance(20);if(!missingOldest)offer(3);
        // An old partially delivered block must also retire on the long wake.
        if(gapFrames>10)output.setAccepting(false);
        voice.processFixturePlaybackAt(40);
        QCOMPARE(output.takePlayed(),reference.first(3840));
        for(int frame=4;frame<4+gapFrames;++frame)offer(frame);
        output.advance(gapFrames*20);output.takePlayed();output.setAccepting(true);
        const qint64 resumeAt=40+gapFrames*20;voice.processFixturePlaybackAt(resumeAt);
        const auto recovery=voice.playbackDiagnostics();qInfo()<<"Bounded device recovery"<<recovery;
        QVERIFY2(recovery.value("outputPendingMs").toInt()==0,"Catch-up left a persistent software PCM tail behind the bounded device");
        const int renderedFrames=gapFrames>10?4:gapFrames;
        QCOMPARE(recovery.value("outputDeviceQueuedMs").toInt(),renderedFrames*20);
        QCOMPARE(recovery.value("receiveQueuedMs").toInt(),40);
        QCOMPARE(recovery.value("outputDroppedFrames").toULongLong(),quint64(gapFrames>10?1:0));
        QCOMPARE(recovery.value("receiveDiscardedSamples").toULongLong(),quint64((gapFrames>10?gapFrames-renderedFrames:0)*960));
        QCOMPARE(recovery.value("receiveConcealedPackets").toULongLong(),quint64(missingOldest?1:0));
        for(int frame=4+gapFrames;frame<24+gapFrames;++frame){
            output.advance(20);offer(frame);voice.processFixturePlaybackAt(resumeAt+(frame-(3+gapFrames))*20);
            QVERIFY2(output.takePlayed()==reference.mid((frame-(2+renderedFrames))*3840,3840),
                "Heard output retained old speech after device and receive timing recovered");
            QCOMPARE(voice.playbackDiagnostics().value("outputPendingMs").toInt(),0);
        }
        QCOMPARE(voice.fixtureCounters().value("sent").toULongLong(),quint64(0));
    }

    void staleDiscardPreservesPartiallyPlayedLongOpusPrediction_data() {
        QTest::addColumn<int>("frameSamples");
        QTest::newRow("20ms")<<960;QTest::newRow("60ms-partial-PCM")<<2880;QTest::newRow("120ms-partial-PCM")<<5760;
    }
    void staleDiscardPreservesPartiallyPlayedLongOpusPrediction() {
        QFETCH(int,frameSamples);
        VoiceEngine voice;QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        voice.configureAudio({false,false,false,1.,1.,-70});
        const auto audio=packets(431,6000,64,frameSamples);QCOMPARE(audio.size(),64);
        int error=0;auto *decoder=opus_decoder_create(48000,2,&error);QCOMPARE(error,OPUS_OK);
        QByteArray reference;
        for(const auto &packet:audio){std::array<opus_int16,5760*2> pcm{};
            QCOMPARE(opus_decode(decoder,reinterpret_cast<const unsigned char*>(packet.constData()),packet.size(),pcm.data(),5760,0),frameSamples);
            reference.append(reinterpret_cast<const char*>(pcm.data()),frameSamples*4);
        }
        opus_decoder_destroy(decoder);
        const auto offer=[&](int frame){voice.queueFixturePeerAudio("111",10,quint16(frame),audio.at(frame));};
        for(int frame=0;frame<3;++frame)offer(frame);
        voice.processFixturePlaybackAt(20);voice.processFixturePlaybackAt(40);
        QCOMPARE(voice.takeFixtureAudio(),reference.left(2*3840));
        const int burstPackets=20*960/frameSamples;
        for(int frame=3;frame<3+burstPackets;++frame)offer(frame);
        voice.processFixturePlaybackAt(440);
        const int skippedFrames=std::min(10,(3+burstPackets)*frameSamples/960-2-12);
        QVERIFY2(voice.takeFixtureAudio()==reference.mid((2+skippedFrames)*3840,10*3840),
            "Discarding stale long Opus packets or their partial PCM changed decoder prediction");
        int nextPacket=3+burstPackets;
        for(int step=1;step<=18;++step){
            if(step%(frameSamples/960)==0)offer(nextPacket++);
            voice.processFixturePlaybackAt(440+step*20);
            QCOMPARE(voice.takeFixtureAudio(),reference.mid((11+skippedFrames+step)*3840,3840));
        }
        const auto diagnostics=voice.playbackDiagnostics();
        QCOMPARE(diagnostics.value("receiveRebases").toULongLong(),quint64(1));
        QCOMPARE(diagnostics.value("receiveDiscardedSamples").toULongLong(),quint64(skippedFrames*960));
        QCOMPARE(diagnostics.value("receiveConcealedPackets").toULongLong(),quint64(0));
    }

    void lateWakeDoesNotRetireFreshSpeech_data() {
        QTest::addColumn<int>("frameSamples");
        QTest::newRow("20ms")<<960;QTest::newRow("60ms")<<2880;QTest::newRow("120ms")<<5760;
    }
    void lateWakeDoesNotRetireFreshSpeech() {
        QFETCH(int,frameSamples);
        VoiceEngine voice;QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        const auto audio=packets(431,6000,3,frameSamples);queue(voice,"111",10,audio);
        int error=0;auto *decoder=opus_decoder_create(48000,2,&error);QCOMPARE(error,OPUS_OK);
        QByteArray reference;
        for(const auto &packet:audio){std::array<opus_int16,5760*2> pcm{};
            QCOMPARE(opus_decode(decoder,reinterpret_cast<const unsigned char*>(packet.constData()),packet.size(),pcm.data(),5760,0),frameSamples);
            reference.append(reinterpret_cast<const char*>(pcm.data()),frameSamples*4);
        }
        opus_decoder_destroy(decoder);voice.processFixturePlaybackAt(400);
        const auto output=voice.takeFixtureAudio();const auto diagnostics=voice.playbackDiagnostics();
        const int realFrames=3*frameSamples/960;
        QCOMPARE(output.left(std::min(10,realFrames)*3840),reference.left(std::min(10,realFrames)*3840));
        QCOMPARE(diagnostics.value("receiveRebases").toULongLong(),quint64(0));
        QCOMPARE(diagnostics.value("receiveDiscardedSamples").toULongLong(),quint64(0));
        QCOMPARE(diagnostics.value("receiveConcealedPackets").toULongLong(),quint64(std::min(5,std::max(0,10-realFrames))));
        QCOMPARE(output.size(),std::min(10,realFrames+5)*3840);
    }

    void continuousSilencePacketsDoNotKeepTheRingOn() {
        VoiceEngine voice;
        QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        QSignalSpy activity(&voice,&VoiceEngine::speakingChanged);
        const auto speech=packets(440,6000), silence=packets(440,0,24);
        queue(voice,"111",10,speech);
        for(int i=0;i<8;++i)voice.processFixturePlayback();
        QTRY_VERIFY(activity.contains(QList<QVariant>{QString("111"),true}));
        for(int i=0;i<silence.size();++i)voice.queueFixturePeerAudio("111",10,quint16(i+8),silence[i]);
        for(int i=0;i<24;++i)voice.processFixturePlayback();
        QTRY_COMPARE(activity.last(),QList<QVariant>({QString("111"),false}));
        QCOMPARE(voice.fixtureCounters().value("decoded").toULongLong(),quint64(32));
    }

    void activityFollowsDecodedAudioAndExpiresDuringSilenceOrPacketLoss() {
        VoiceEngine voice;
        QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        QSignalSpy activity(&voice,&VoiceEngine::speakingChanged);
        const auto silence=packets(440,0), speech=packets(440,6000), quiet=packets(440,10);
        queue(voice,"111",10,silence);queue(voice,"222",20,quiet);
        for(int i=0;i<8;++i)voice.processFixturePlayback();
        QCOMPARE(activity.size(),0);
        for(int i=0;i<speech.size();++i)voice.queueFixturePeerAudio("111",10,quint16(i+8),speech[i]);
        voice.processFixturePlayback();
        QTRY_VERIFY(activity.contains(QList<QVariant>{QString("111"),true}));
        QVERIFY(!activity.contains(QList<QVariant>{QString("222"),true}));
        for(int i=0;i<30;++i)voice.processFixturePlayback();
        QTRY_COMPARE(activity.last(),QList<QVariant>({QString("111"),false}));
        activity.clear();
        for(int i=0;i<speech.size();++i)voice.queueFixturePeerAudio("111",10,quint16(i+40),speech[i]);
        voice.processFixturePlayback();
        QTRY_VERIFY(activity.contains(QList<QVariant>{QString("111"),true}));
        QVERIFY(voice.setUserMuted("111",true));voice.processFixturePlayback();
        QTRY_COMPARE(activity.last(),QList<QVariant>({QString("111"),false}));
        activity.clear();
        voice.setUserMuted("111",false);
        voice.processFixturePlayback();
        QTRY_VERIFY(activity.contains(QList<QVariant>{QString("111"),true}));
        voice.setDeafened(true);
        QTRY_COMPARE(activity.last(),QList<QVariant>({QString("111"),false}));
    }

    void perPersonGainAndMuteAffectDecodedMixWithoutTransmitting() {
        VoiceEngine first, second, mixed;
        for (auto *voice : {&first, &second, &mixed}) QVERIFY(voice->configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        const auto a = packets(440, 16000), b = packets(660, 12000);
        QCOMPARE(a.size(), 8); QCOMPARE(b.size(), 8);
        queue(first, "111", 10, a); queue(second, "222", 20, b);
        queue(mixed, "111", 10, a); queue(mixed, "222", 20, b);
        QSignalSpy sentState(&mixed, &VoiceEngine::voiceStateRequested);
        QSignalSpy controls(&mixed, &VoiceEngine::userAudioChanged);
        bool heardAudio = false, sawClipping = false;
        const QList<QPair<double, double>> gains{{.5, 0}, {.5, 0}, {2, 2}, {0, .25}, {1, 1}, {1, 1}};
        for (int frame = 0; frame < gains.size(); ++frame) {
            const auto [gainA, gainB] = gains.at(frame);
            QVERIFY(mixed.setUserMuted("111", gainA == 0));
            QVERIFY(mixed.setUserMuted("222", gainB == 0));
            QVERIFY(mixed.setUserVolume("111", gainA == 0 ? 1 : gainA));
            QVERIFY(mixed.setUserVolume("222", gainB == 0 ? 1 : gainB));
            if (frame == 4) mixed.clearUserAudioSettings();
            first.processFixturePlayback(); second.processFixturePlayback(); mixed.processFixturePlayback();
            const auto left = first.takeFixtureAudio(), right = second.takeFixtureAudio(), output = mixed.takeFixtureAudio();
            QCOMPARE(left.size(), 3840); QCOMPARE(right.size(), 3840); QCOMPARE(output.size(), 3840);
            for (int offset = 0; offset < 3840; offset += 2) {
                qint16 x, y, actual;
                std::memcpy(&x, left.constData() + offset, 2); std::memcpy(&y, right.constData() + offset, 2); std::memcpy(&actual, output.constData() + offset, 2);
                const int sum = int(std::lround(x * gainA) + std::lround(y * gainB));
                QCOMPARE(actual, qint16(std::clamp(sum, -32768, 32767)));
                heardAudio |= actual != 0; sawClipping |= sum > 32767 || sum < -32768;
            }
        }
        QVERIFY(heardAudio); QVERIFY(sawClipping); QTRY_VERIFY(!controls.isEmpty());
        QCOMPARE(sentState.size(), 0);
        QCOMPARE(mixed.fixtureCounters().value("sent").toULongLong(), quint64(0));
        QCOMPARE(mixed.fixtureCounters().value("decoded").toULongLong(), quint64(12));
        QVERIFY(!mixed.connected()); QVERIFY(!mixed.micTesting());
    }

    void receivedSoundboardGainsChangeOnlyLocalClipSamplesWhileMutedClipsDrain() {
        VoiceEngine reference,mixed;
        for(auto *voice:{&reference,&mixed})QVERIFY(voice->configureLocalFixture(QUrl("ws://127.0.0.1:9")));
        const auto speech=packets(330,6000,12);queue(reference,"111",10,speech);queue(mixed,"111",10,speech);
        const auto clip=[](int base){QByteArray pcm(3840*12,Qt::Uninitialized);for(int frame=0;frame<12;++frame)for(int sample=0;sample<1920;++sample){const qint16 value=qint16(base+frame*100);std::memcpy(pcm.data()+frame*3840+sample*2,&value,2);}return pcm;};
        QVERIFY(receive(mixed,"111",clip(1000),.5));QVERIFY(receive(mixed,"222",clip(2000),1));
        QSignalSpy state(&mixed,&VoiceEngine::voiceStateRequested);
        struct Gain{double master,a,b;};const QList<Gain> gains{{0,1,1},{0,2,.5},{.25,1,1},{1,0,1},{1,2,0},{.5,.5,2},{1,1,1},{0,0,0}};
        for(int frame=0;frame<gains.size();++frame){const auto g=gains[frame];mixed.setSoundboardVolume(g.master);
            QVERIFY(mixed.setUserSoundboardVolume("111",g.a));QVERIFY(mixed.setUserSoundboardVolume("222",g.b));
            QCOMPARE(mixed.userVolume("111"),1.);reference.processFixturePlayback();mixed.processFixturePlayback();
            const auto speechPcm=reference.takeFixtureAudio(),output=mixed.takeFixtureAudio();QCOMPARE(speechPcm.size(),3840);QCOMPARE(output.size(),3840);
            for(int at=0;at<3840;at+=2){qint16 voice,actual;std::memcpy(&voice,speechPcm.constData()+at,2);std::memcpy(&actual,output.constData()+at,2);
                const int expected=voice+int(std::lround((1000+frame*100)*g.master*g.a*.5))+int(std::lround((2000+frame*100)*g.master*g.b));QCOMPARE(actual,qint16(std::clamp(expected,-32768,32767)));}
        }
        QCOMPARE(state.size(),0);QCOMPARE(mixed.fixtureCounters().value("sent").toULongLong(),quint64(0));
        mixed.stopReceivedSoundboards();QCOMPARE(mixed.fixtureCounters().value("receivedSoundboards").toInt(),0);
    }

    void receivedSoundboardsRespectMuteDeafenBoundsAndSettingsReset() {
        VoiceEngine voice;QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:9")));const QByteArray pcm(3840*2,'\x10');
        QVERIFY(!voice.setUserSoundboardVolume("../bad",1));QVERIFY(!voice.setUserSoundboardVolume("111",std::numeric_limits<double>::quiet_NaN()));
        QVERIFY(voice.setUserSoundboardVolume("111",3));QCOMPARE(voice.userSoundboardVolume("111"),2.);QCOMPARE(voice.userVolume("111"),1.);
        QVERIFY(voice.setUserVolume("111",.5));QVERIFY(voice.setUserSoundboardVolume("111",1));QCOMPARE(voice.userVolume("111"),.5);
        QVERIFY(voice.setUserSoundboardVolume("111",0));QVERIFY(voice.setUserVolume("111",1));QCOMPARE(voice.userSoundboardVolume("111"),0.);
        voice.leave();QCOMPARE(voice.userSoundboardVolume("111"),0.);voice.clearUserAudioSettings();QCOMPARE(voice.userSoundboardVolume("111"),1.);
        QVERIFY(!receive(voice,"111",{},1));QVERIFY(!receive(voice,"111",QByteArray(3,'x'),1));QVERIFY(!receive(voice,"111",pcm,std::numeric_limits<double>::quiet_NaN()));
        for(int i=0;i<8;++i)QVERIFY(receive(voice,"111",pcm,1));QVERIFY(!receive(voice,"111",pcm,1));
        voice.setUserMuted("111",true);voice.processFixturePlayback();auto muted=voice.takeFixtureAudio();QCOMPARE(muted.size(),3840);QCOMPARE(muted,QByteArray(3840,'\0'));
        voice.setDeafened(true);QCOMPARE(voice.fixtureCounters().value("receivedSoundboards").toInt(),0);QVERIFY(!receive(voice,"111",pcm,1));
        const auto oldGeneration=voice.soundboardPlaybackGeneration();voice.setDeafened(false);voice.processFixturePlayback();QVERIFY(voice.takeFixtureAudio().isEmpty());
        voice.stopReceivedSoundboards();QVERIFY(!voice.playReceivedSoundboardPcm("111",pcm,1,oldGeneration));
        voice.setUserMuted("111",false);QVERIFY(receive(voice,"111",pcm,1));voice.leave();QCOMPARE(voice.fixtureCounters().value("receivedSoundboards").toInt(),0);
        for(int i=1;i<=1000;++i)QVERIFY(voice.setUserSoundboardVolume(QString::number(i),.5));QVERIFY(!voice.setUserSoundboardVolume("1001",.5));
        QVERIFY(voice.setUserSoundboardVolume("1",1));QVERIFY(voice.setUserSoundboardVolume("1001",.5));voice.clearUserAudioSettings();
    }

    void settingsValidateBoundAndSurviveChannelLeave() {
        VoiceEngine voice;
        QVERIFY(!voice.setUserVolume("../bad", 1));
        QVERIFY(!voice.setUserVolume("111", std::numeric_limits<double>::quiet_NaN()));
        QVERIFY(!voice.setUserMuted("", true));
        QVERIFY(voice.setUserVolume("111", 3)); QCOMPARE(voice.userVolume("111"), 2.0);
        QVERIFY(voice.setUserMuted("111", true)); voice.leave();
        QVERIFY(voice.userMuted("111")); QCOMPARE(voice.userVolume("111"), 2.0);
        voice.clearUserAudioSettings(); QVERIFY(!voice.userMuted("111")); QCOMPARE(voice.userVolume("111"), 1.0);
        for (int i = 1; i <= 1000; ++i) QVERIFY(voice.setUserVolume(QString::number(i), .5));
        QVERIFY(!voice.setUserVolume("1001", .5));
        QVERIFY(voice.setUserVolume("1", 1));
        QVERIFY(voice.setUserVolume("1001", .5));
        voice.clearUserAudioSettings(); QCOMPARE(voice.userVolume("1001"), 1.0);
    }
};

QTEST_GUILESS_MAIN(VoiceMemberTest)
#include "voice_member_test.moc"
