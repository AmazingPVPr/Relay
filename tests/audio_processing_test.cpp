#include "AudioProcessor.h"
#include "VoiceLimiter.h"
#include "AppSettings.h"
#include <QtTest>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

class AudioProcessingTest final : public QObject {
    Q_OBJECT
private slots:
    void voiceLimiterCapsShoutsAndReleasesWithoutBoosting() {
        VoiceLimiter limiter;
        std::array<std::int16_t,1920> pcm; pcm.fill(30000);
        QCOMPARE(limiter.frameGain(reinterpret_cast<const char *>(pcm.data()), pcm.size(), 1, false, .5), 1.0);
        const auto limited=limiter.frameGain(reinterpret_cast<const char *>(pcm.data()), pcm.size(), 1, true, .5);
        QVERIFY(30000*limited <= 32767*.5 + .01);
        pcm.fill(1000);
        const auto released=limiter.frameGain(reinterpret_cast<const char *>(pcm.data()), pcm.size(), 1, true, .5);
        QVERIFY(released > limited); QVERIFY(released <= 1);
        VoiceLimiter other; QCOMPARE(other.frameGain(reinterpret_cast<const char *>(pcm.data()), pcm.size(), 1, true, .5), 1.0);
        pcm.fill(-32768);
        const auto boosted=other.frameGain(reinterpret_cast<const char *>(pcm.data()), pcm.size(), 2, true, .3);
        QVERIFY(32768*boosted <= 32767*.3 + .01);
        QCOMPARE(other.frameGain(reinterpret_cast<const char *>(pcm.data()), pcm.size(), 0, true, .3), 0.0);
    }
    void neutralPreservesContinuousStereoChirps() {
        AudioProcessor processor;
        constexpr double pi=3.14159265358979323846;
        for(int block=0;block<150;++block){
            std::array<std::int16_t,1920> pcm;
            for(int n=0;n<960;++n){const double t=(block*960+n)/48000.;pcm[n*2]=std::int16_t(9000*std::sin(2*pi*(130*t+45*t*t)));pcm[n*2+1]=std::int16_t(7000*std::sin(2*pi*(430*t+65*t*t)));}
            const auto before=pcm;QVERIFY(processor.capture(pcm.data()));QVERIFY(pcm==before);QVERIFY(processor.healthy());
        }
    }
    void realSpeechProcessingPreservesOrderAcrossCaptureBatches() {
        using Frame=std::array<std::int16_t,1920>;
        constexpr int count=150;constexpr double pi=3.14159265358979323846;
        std::vector<Frame> source(count),render(count),expected(count);double phase=0;
        for(int block=0;block<count;++block)for(int n=0;n<960;++n){
            const double t=(block*960+n)/48000.;const double fundamental=130+35*std::sin(2*pi*.6*t);phase+=2*pi*fundamental/48000.;double voice=0;
            for(int harmonic=1;harmonic<18;++harmonic){const double hz=harmonic*fundamental;const double formant=std::exp(-std::pow((hz-700)/300.,2))+.7*std::exp(-std::pow((hz-1250)/400.,2))+.3*std::exp(-std::pow((hz-2500)/600.,2));voice+=std::sin(phase*harmonic)*formant/harmonic;}
            const double envelope=.15+.7*std::pow(.5+.5*std::sin(t*2*pi*3),2);
            source[block][n*2]=source[block][n*2+1]=std::int16_t(voice*envelope*.43*32768.);
            render[block][n*2]=render[block][n*2+1]=std::int16_t(std::sin(2*pi*620*t)*300);
        }
        AudioProcessor::Options options;options.noiseSuppression=true;options.echoCancellation=true;
        AudioProcessor normal,batched;normal.configure(options);batched.configure(options);int active=0;
        for(int n=0;n<count;++n){auto reference=render[n];normal.playback(reference.data());expected[n]=source[n];active+=normal.capture(expected[n].data(),60);}
        QVERIFY(active>count*8/10); // Generated voiced input remains audible.
        const int batches[]={1,7,2,5,3};int next=0,batch=0;
        while(next<count){const int end=std::min(count,next+batches[batch++%5]);for(;next<end;++next){auto reference=render[next],pcm=source[next];batched.playback(reference.data());batched.capture(pcm.data(),60);QVERIFY(pcm==expected[next]);}}
        QVERIFY(normal.healthy());QVERIFY(batched.healthy());
    }
    void microphoneMonitorPreservesVoicedInputWithoutEchoReference() {
        // A voiced vowel has strong correlations between successive frames.
        // Treating its local monitor as remote playback makes AEC suppress it.
        AudioProcessor monitored, captureOnly;
        AudioProcessor::Options options;
        options.noiseSuppression=true;options.echoCancellation=true;
        options.outputVolume=.65;
        monitored.configure(options);captureOnly.configure(options);
        constexpr double pi=3.14159265358979323846;
        int audibleAfterWarmup=0;
        for(int block=0;block<1000;++block) {
            std::array<std::int16_t,1920> frame;
            for(int n=0;n<960;++n) {
                const double phase=2*pi*150*(block*960+n)/48000.;
                double voiced=0;
                for(int harmonic=1;harmonic<18;++harmonic) {
                    const double hz=150*harmonic;
                    const double formant=std::exp(-std::pow((hz-700)/300.,2))
                        +.7*std::exp(-std::pow((hz-1250)/400.,2))
                        +.3*std::exp(-std::pow((hz-2500)/600.,2));
                    voiced+=std::sin(phase*harmonic)*formant/harmonic;
                }
                frame[n*2]=frame[n*2+1]=std::int16_t(voiced*.5*.43*32768.);
            }
            auto expected=frame;
            const bool expectedActive=captureOnly.capture(expected.data(),60);
            QCOMPARE(monitored.capture(frame.data(),60),expectedActive);
            // Measure monitor gain against this processor's own capture output:
            // independent speech processors (notably Speex) are not bit-identical.
            // The warmup/energy check below detects microphone echo suppression.
            const auto captured = frame;
            monitored.monitor(frame.data());
            double energy=0;
            for(int n=0;n<int(frame.size());++n) {
                QCOMPARE(frame[n],std::int16_t(captured[n]*options.outputVolume));
                energy+=double(frame[n])*frame[n];
            }
            if(block>=100 && energy/frame.size()>500*500)++audibleAfterWarmup;
        }
        QCOMPARE(audibleAfterWarmup,900);
        QVERIFY(monitored.healthy());QVERIFY(captureOnly.healthy());

        options.outputVolume=2;monitored.configure(options);
        std::array<std::int16_t,1920> clipping;
        clipping.fill(30000);monitored.monitor(clipping.data());QCOMPARE(clipping[0],32767);
        clipping.fill(-30000);monitored.monitor(clipping.data());QCOMPARE(clipping[0],-32768);
        monitored.monitor(nullptr);
    }
    void bypassGainAndGate() {
        AudioProcessor processor;
        std::array<std::int16_t,1920> frame; frame.fill(4000);
        QVERIFY(processor.capture(frame.data()));
        QCOMPARE(frame[0], 4000);
        AudioProcessor::Options options; options.inputVolume=.5; options.outputVolume=2;
        processor.configure(options);
        frame.fill(4000); QVERIFY(processor.capture(frame.data())); QCOMPARE(frame[0],2000);
        processor.playback(frame.data()); QCOMPARE(frame[0],4000);
        frame.fill(30000); processor.playback(frame.data()); QCOMPARE(frame[0],32767);
        for(int i=0;i<12;++i){frame.fill(1);processor.capture(frame.data());}
        QCOMPARE(frame[0],0);
        QVERIFY(!processor.capture(frame.data()));
    }
    void realProcessorsAndReset() {
        AudioProcessor processor;
        AudioProcessor::Options options;
        options.noiseSuppression=true; options.echoCancellation=true; options.automaticGainControl=true;
        processor.configure(options); QVERIFY(processor.healthy());
        std::array<std::int16_t,1920> frame,reference;
        for(int n=0;n<50;++n) {
            for(int i=0;i<960;++i) frame[i*2]=frame[i*2+1]=std::int16_t(std::sin((n*960+i)*.05)*5000);
            reference=frame; processor.playback(reference.data()); processor.capture(frame.data(),60);
            QVERIFY(processor.healthy()); QVERIFY(std::isfinite(processor.peak()));
            QVERIFY(processor.peak()>=0 && processor.peak()<=1);
        }
        processor.configure({}); processor.reset();
        frame.fill(4000); QVERIFY(processor.capture(frame.data())); QCOMPARE(frame[0],4000);
    }
    void preferencesPersistAndClamp() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.filePath("settings.ini");
        { AppSettings prefs(path); QVERIFY(prefs.hardwareAcceleration()); QVERIFY(!prefs.graphicsRestartRequired());
          prefs.setHardwareAcceleration(false); QVERIFY(prefs.graphicsRestartRequired());
          prefs.setInputVolume(8); QCOMPARE(prefs.inputVolume(),2.0);
          prefs.setOutputVolume(-2); QCOMPARE(prefs.outputVolume(),0.0);
          prefs.setFontScale(std::numeric_limits<double>::quiet_NaN()); QCOMPARE(prefs.fontScale(),1.0);
          prefs.setNoiseSuppression(true); prefs.setEchoCancellation(true); prefs.setAutomaticGainControl(true);
          prefs.setInputSensitivityDb(-85); QCOMPARE(prefs.inputSensitivityDb(),-70.0); }
        AppSettings restored(path); QVERIFY(!restored.hardwareAcceleration()); QVERIFY(!restored.graphicsRestartRequired());
        QVERIFY(restored.noiseSuppression()); QVERIFY(restored.echoCancellation()); QVERIFY(restored.automaticGainControl());
        QCOMPARE(restored.inputVolume(),2.0);
    }
};
QTEST_GUILESS_MAIN(AudioProcessingTest)
#include "audio_processing_test.moc"
