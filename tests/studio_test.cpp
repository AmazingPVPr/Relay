#include "StudioBridge.h"
#include <QtTest>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTcpServer>
#include <QVideoSink>
#include <QVideoFrame>
#include <QStandardPaths>
#include <QSignalSpy>
#include <cstring>
#include <QSet>
#include <QImage>
#include <QTemporaryDir>
#include <QFile>
#include <ctime>

#include "StudioTestMedia.h"

namespace {
QSet<int> h264Types(const QByteArray &frame) {
    QSet<int> types;
    for (qsizetype at = 0; at + 3 < frame.size(); ++at) {
        if (frame[at] || frame[at + 1]) continue;
        const qsizetype prefix = frame[at + 2] == 1 ? 3
            : (at + 4 < frame.size() && !frame[at + 2] && frame[at + 3] == 1 ? 4 : 0);
        if (prefix && at + prefix < frame.size()) types.insert(quint8(frame[at + prefix]) & 31);
    }
    return types;
}
}

class StudioTest final : public QObject {
    Q_OBJECT
private slots:
    void hiddenOriginalAvoidsDecodeAndConversionForTwelveSeconds() {
        RtmpPlaybackFixture source;source.size=QSize(960,540);source.fps=60;source.gopSeconds=2;source.durationSeconds=22;source.motion=true;source.start();
        QVERIFY(source.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(source.listening(),3000);
        StudioBridge studio;studio.setInputMode(StudioBridge::ExistingRtmpStream);studio.setRemoteServer(source.server());studio.setRemoteStreamKey("fixture-key");studio.start();
        studio.setBroadcastCaptureEnabled(true,0,0,true);
        qint64 forwarded=0,audioBytes=0;bool keysComplete=true;QElapsedTimer duration;duration.start();
        const auto hiddenCpuStart=std::clock();
        while(duration.elapsed()<12000 && studio.error().isEmpty()) {
            const auto encoded=studio.takeEncodedBroadcastVideo();
            for(const auto &frame:encoded.frames) {
                ++forwarded;
                if(frame.keyframe){const auto types=h264Types(frame.h264);keysComplete=keysComplete&&types.contains(5)&&types.contains(7)&&types.contains(8);}
            }
            audioBytes+=studio.takeBroadcastMedia().audio.size();QTest::qWait(20);
        }
        const double hiddenCpuMs=1000.0*double(std::clock()-hiddenCpuStart)/CLOCKS_PER_SEC;
        const auto stats=studio.processingDiagnostics();
        const auto packets=stats.value("videoPackets").toLongLong(),decoded=stats.value("decodedVideoFrames").toLongLong(),converted=stats.value("convertedVideoFrames").toLongLong();
        qInfo("GeneratedHiddenOriginal durationMs=%lld processCpuMs=%.1f packets=%lld decoded=%lld converted=%lld forwarded=%lld audioBytes=%lld",qlonglong(duration.elapsed()),hiddenCpuMs,qlonglong(packets),qlonglong(decoded),qlonglong(converted),qlonglong(forwarded),qlonglong(audioBytes));
        QVERIFY2(studio.error().isEmpty(),qPrintable(studio.error()));QVERIFY(packets>600);QVERIFY(forwarded>600);QVERIFY(audioBytes>1500000);QVERIFY(keysComplete);
        QVERIFY2(decoded<packets/4,"Hidden original-quality forwarding should avoid continuous software video decoding");
        QCOMPARE(converted,0);QVERIFY(stats.value("maximumReplayBytes").toLongLong()<=16*1024*1024);QVERIFY(stats.value("maximumReplayPackets").toLongLong()<=180);
        QVideoSink visible;studio.setVideoSink(&visible);
        // Exclude the one-time decoder warm-up from this steady-preview sample.
        QElapsedTimer settle;settle.start();while(settle.elapsed()<500){studio.takeEncodedBroadcastVideo();studio.takeBroadcastMedia();QTest::qWait(20);}
        const auto visibleBefore=studio.processingDiagnostics();const auto visibleCpuStart=std::clock();
        QElapsedTimer visibleTime;visibleTime.start();while(visibleTime.elapsed()<3000){studio.takeEncodedBroadcastVideo();studio.takeBroadcastMedia();QTest::qWait(20);}
        const auto visibleAfter=studio.processingDiagnostics();
        const double visibleCpuMs=1000.0*double(std::clock()-visibleCpuStart)/CLOCKS_PER_SEC;
        qInfo("GeneratedVisibleOriginal durationMs=%lld processCpuMs=%.1f decoded=%lld converted=%lld hiddenCpuMsPerSecond=%.1f visibleCpuMsPerSecond=%.1f",qlonglong(visibleTime.elapsed()),visibleCpuMs,
            qlonglong(visibleAfter.value("decodedVideoFrames").toLongLong()-visibleBefore.value("decodedVideoFrames").toLongLong()),
            qlonglong(visibleAfter.value("convertedVideoFrames").toLongLong()-visibleBefore.value("convertedVideoFrames").toLongLong()),
            hiddenCpuMs/12.0,visibleCpuMs/(visibleTime.elapsed()/1000.0));
        QVERIFY(visibleAfter.value("decodedVideoFrames").toLongLong()-visibleBefore.value("decodedVideoFrames").toLongLong()>100);
        QVERIFY(visibleAfter.value("convertedVideoFrames").toLongLong()-visibleBefore.value("convertedVideoFrames").toLongLong()>40);
        studio.stop();
    }
    void hiddenLongGopRestoresPreviewAndFallbackWithoutWaitingForIdr_data() {
        QTest::addColumn<int>("hiddenFrames");
        QTest::addColumn<bool>("previewFirst");
        QTest::addColumn<QSize>("sourceSize");
        QTest::newRow("cached-preview")<<60<<true<<QSize(960,540);
        QTest::newRow("cached-fallback")<<60<<false<<QSize(960,540);
        QTest::newRow("bounded-warm-fallback")<<240<<false<<QSize(960,540);
        QTest::newRow("1440p60-bounded-warm-fallback")<<240<<false<<QSize(2560,1440);
        QTest::newRow("1440p60-mid-warm-fallback")<<121<<false<<QSize(2560,1440);
    }
    void hiddenLongGopRestoresPreviewAndFallbackWithoutWaitingForIdr() {
        QFETCH(int,hiddenFrames);QFETCH(bool,previewFirst);QFETCH(QSize,sourceSize);
        RtmpPlaybackFixture source;source.size=sourceSize;source.fps=60;source.gopSeconds=30;source.durationSeconds=18;source.motion=true;source.start();
        QVERIFY(source.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(source.listening(),3000);
        StudioBridge studio;studio.setInputMode(StudioBridge::ExistingRtmpStream);studio.setRemoteServer(source.server());studio.setRemoteStreamKey("fixture-key");studio.start();
        studio.setBroadcastContext({{"active",true}});studio.setBroadcastCaptureEnabled(true,0,0,true);
        qint64 audioBytes=0,forwarded=0,maxAudioGapMs=0,maxVideoGapMs=0,lastAudio=-1,lastVideo=-1;int idrs=0;
        QElapsedTimer delivery;delivery.start();
        const auto drain=[&] {
            const auto frames=studio.takeEncodedBroadcastVideo().frames;
            if(!frames.isEmpty()){const auto now=delivery.elapsed();if(lastVideo>=0)maxVideoGapMs=std::max(maxVideoGapMs,now-lastVideo);lastVideo=now;}
            for(const auto &video:frames){++forwarded;if(video.keyframe)++idrs;}
            const auto media=studio.takeBroadcastMedia();
            if(!media.audio.isEmpty()){const auto now=delivery.elapsed();if(lastAudio>=0)maxAudioGapMs=std::max(maxAudioGapMs,now-lastAudio);lastAudio=now;}
            audioBytes+=media.audio.size();return media.video;
        };
        QTRY_VERIFY_WITH_TIMEOUT(([&]{
            drain();const auto state=studio.processingDiagnostics();
            // The worker publishes replayLimitRestores before decoding its first
            // warm-up packet. Observe actual decode progress with replay still
            // pending so this case tests recovery in the middle of warm-up.
            return hiddenFrames==121 ? state.value("replayLimitRestores").toLongLong()>0
                    && state.value("decodedVideoFrames").toLongLong()>1 && state.value("replayPackets").toLongLong()>0
                : state.value("videoPackets").toLongLong()>=hiddenFrames;
        })(),6000);
        auto stats=studio.processingDiagnostics();
        QCOMPARE(stats.value("convertedVideoFrames").toLongLong(),0);
        const auto received=stats.value("videoPackets").toLongLong();
        // High-bitrate sources can reach the 8 MiB warm-up threshold before 121 frames.
        // Compare delivery with actual ingest at that point, not the nominal frame target.
        QVERIFY(forwarded>received-8);QCOMPARE(idrs,1);QVERIFY(audioBytes>100000);
        if(hiddenFrames==121)
            qInfo("GeneratedMidWarmDelivery received=%lld forwarded=%lld remaining=%lld audioBytes=%lld",
                qlonglong(received),qlonglong(forwarded),qlonglong(stats.value("replayPackets").toLongLong()),qlonglong(audioBytes));
        if(hiddenFrames>180){
            QCOMPARE(stats.value("replayLimitRestores").toLongLong(),1);QCOMPARE(stats.value("replayPackets").toLongLong(),0);
            QVERIFY(stats.value("backgroundReplayChunks").toLongLong()>1);
            QCOMPARE(stats.value("backgroundBudgetOverruns").toLongLong(),0);
            QVERIFY2(stats.value("maxBackgroundReplayWorkUs").toLongLong()<50000,"Incremental warm-up must not monopolize ingest/audio");
            QVERIFY2(maxAudioGapMs<200,"Audio should continue through automatic decoder warm-up");
            QVERIFY2(maxVideoGapMs<200,"Original video forwarding should continue through automatic warm-up");
            qInfo("GeneratedIncrementalWarm source=%dx%d maxBatchUs=%lld audioGapMs=%lld videoGapMs=%lld",sourceSize.width(),sourceSize.height(),qlonglong(stats.value("maxBackgroundReplayWorkUs").toLongLong()),qlonglong(maxAudioGapMs),qlonglong(maxVideoGapMs));
        }
        else if(hiddenFrames==121) {QVERIFY(stats.value("decodedVideoFrames").toLongLong()>1);QVERIFY(stats.value("replayPackets").toLongLong()>0);}
        else {QCOMPARE(stats.value("decodedVideoFrames").toLongLong(),1);QVERIFY(stats.value("replayPackets").toLongLong()>0);}
        QVideoSink preview, replacement;
        if(previewFirst) {
            QElapsedTimer reopen;reopen.start();studio.setVideoSink(&preview);
            QTRY_VERIFY_WITH_TIMEOUT(([&]{drain();return preview.videoFrame().isValid();})(),1500);
            QVERIFY(reopen.elapsed()<1500);QVERIFY(studio.processingDiagnostics().value("replayRestores").toLongLong()>0);
            QCOMPARE(preview.videoFrame().size(),QSize(960,540));
            studio.releasePreview(&preview);QVERIFY(studio.running());
        }
        QElapsedTimer fallback;fallback.start();
        studio.setBroadcastCaptureEnabled(true,1440,60,true);
        QVideoFrame full;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{const auto current=drain();if(current.isValid())full=current;return full.isValid();})(),1500);
        QVERIFY(fallback.elapsed()<1500);QCOMPARE(full.size(),source.size);
        const auto decoded=full.toImage();QVERIFY(!decoded.isNull());
        const auto center=decoded.pixelColor(decoded.width()/2,decoded.height()/2);
        QVERIFY2(center.red()>180 && center.green()<80 && center.blue()<80,"Fallback must contain the current generated red center, not stale/invalid pixels");
        QCOMPARE(idrs,1); // All recovery happened before the source's next 30s IDR.
        stats=studio.processingDiagnostics();
        qInfo("GeneratedGopRecovery source=%dx%d maxReplayWorkUs=%lld",sourceSize.width(),sourceSize.height(),qlonglong(stats.value("maxReplayWorkUs").toLongLong()));
        QVERIFY2(stats.value("maxReplayWorkUs").toLongLong()<1500000,"Bounded replay must not monopolize RTMP ingest for multiple seconds");
        QCOMPARE(stats.value("replayBytes").toLongLong(),0);QVERIFY(stats.value("replayRestores").toLongLong()>0);
        studio.setVideoSink(&replacement);
        studio.releasePreview(&preview);QCOMPARE(studio.videoSink(),&replacement);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{drain();return replacement.videoFrame().isValid();})(),1000);
        studio.releasePreview(&replacement);QVERIFY(!studio.videoSink());
        studio.setBroadcastCaptureEnabled(true,0,0,true);
        drain();QTest::qWait(60);drain();
        const auto noPreviewConversions=studio.processingDiagnostics().value("convertedVideoFrames").toLongLong();
        QElapsedTimer warm;warm.start();while(warm.elapsed()<500){drain();QTest::qWait(20);}
        QCOMPARE(studio.processingDiagnostics().value("convertedVideoFrames").toLongLong(),noPreviewConversions);
        QVERIFY2(studio.error().isEmpty(),qPrintable(studio.error()));
        studio.setBroadcastCaptureEnabled(false,0,0,false);studio.setBroadcastContext({{"active",false}});
        QVERIFY(!studio.running());QVERIFY(studio.processingDiagnostics().isEmpty());
    }

    void hiddenOriginalTracksResolutionChangesAtIdr() {
        QTemporaryDir clips;
        QStringList paths;
        for(const auto &size:{QSize(320,180),QSize(640,360)}) {
            const auto path=clips.filePath(QString::number(size.width())+".flv");paths.append(path);
            QProcess encoder;encoder.start("ffmpeg",{"-hide_banner","-loglevel","error","-f","lavfi","-i",QString("color=c=red:size=%1x%2:rate=30").arg(size.width()).arg(size.height()),"-t","3","-c:v","libx264","-preset","ultrafast","-tune","zerolatency","-g","30","-bf","0","-pix_fmt","yuv420p","-x264-params","repeat-headers=1","-f","flv",path});
            QVERIFY(encoder.waitForFinished(15000));QCOMPARE(encoder.exitCode(),0);
        }
        QFile list(clips.filePath("concat.txt"));QVERIFY(list.open(QIODevice::WriteOnly));
        for(const auto &path:paths)list.write(("file '"+path+"'\n").toUtf8());list.close();
        RtmpPlaybackFixture source;QTcpServer reserve;QVERIFY(reserve.listen(QHostAddress::LocalHost,0));source.port=reserve.serverPort();reserve.close();
        source.process.start("ffmpeg",{"-hide_banner","-loglevel","error","-re","-f","concat","-safe","0","-i",list.fileName(),"-c","copy","-listen","1","-f","flv",source.server()+"/fixture-key"});
        QVERIFY(source.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(source.listening(),3000);
        StudioBridge studio;studio.setInputMode(StudioBridge::ExistingRtmpStream);studio.setRemoteServer(source.server());studio.setRemoteStreamKey("fixture-key");studio.start();studio.setBroadcastCaptureEnabled(true,0,0,false);
        bool sawInitial=false,sawChanged=false;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{
            const auto encoded=studio.takeEncodedBroadcastVideo();
            for(const auto &frame:encoded.frames)if(frame.keyframe) {
                sawInitial=sawInitial||(encoded.width==320 && encoded.height==180);
                sawChanged=sawChanged||(encoded.width==640 && encoded.height==360);
            }
            return sawChanged||!studio.error().isEmpty();
        })(),7000);
        QVERIFY2(studio.error().isEmpty(),qPrintable(studio.error()));QVERIFY(sawInitial);QVERIFY(sawChanged);
        QTRY_COMPARE(studio.resolution(),QString::fromUtf8("640 × 360"));
        QCOMPARE(studio.processingDiagnostics().value("convertedVideoFrames").toLongLong(),0);
        QVERIFY(studio.processingDiagnostics().value("decodedVideoFrames").toLongLong()<10);
        studio.stop();
    }

    void disarmRestoresAndClearsHiddenCachedFrames() {
        RtmpPlaybackFixture source;source.fps=60;source.gopSeconds=30;source.start();
        QVERIFY(source.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(source.listening(),3000);
        StudioBridge studio;studio.setInputMode(StudioBridge::ExistingRtmpStream);studio.setRemoteServer(source.server());studio.setRemoteStreamKey("fixture-key");studio.start();
        studio.setBroadcastCaptureEnabled(true,0,0,false);
        QTRY_VERIFY_WITH_TIMEOUT(([&]{studio.takeEncodedBroadcastVideo();return studio.processingDiagnostics().value("replayPackets").toLongLong()>30;})(),3000);
        studio.setBroadcastCaptureEnabled(false,0,0,false);
        QTRY_COMPARE_WITH_TIMEOUT(studio.processingDiagnostics().value("replayPackets").toLongLong(),0,1000);
        QVERIFY(!studio.takeEncodedBroadcastVideo().available);QVERIFY(!studio.takeBroadcastMedia().video.isValid());
        QVERIFY(studio.processingDiagnostics().value("replayRestores").toLongLong()>0);
        studio.stop();
    }

    void releasedEditorStopsHiddenInputAfterSharingAndPreservesOtherOwners() {
        StudioBridge studio;QVideoSink first,second;
        studio.setVideoSink(&first);studio.start();QVERIFY(studio.running());
        studio.setBroadcastContext({{"active",true}});studio.setBroadcastCaptureEnabled(true,720,30,false);
        studio.releasePreview(&first);QVERIFY(studio.running());QVERIFY(!studio.videoSink());
        studio.setBroadcastCaptureEnabled(false,720,30,false);QVERIFY(studio.running());
        studio.setBroadcastContext({{"active",false}});QVERIFY(!studio.running());
        // Reopening an editor cancels deferred cleanup. A stale editor cannot
        // clear a replacement editor's sink, or stop its preview.
        studio.setVideoSink(&first);studio.start();studio.setBroadcastContext({{"active",true}});
        studio.releasePreview(&first);studio.setVideoSink(&second);studio.releasePreview(&first);
        QCOMPARE(studio.videoSink(),&second);studio.setBroadcastContext({{"active",false}});QVERIFY(studio.running());
        studio.releasePreview(&second);QVERIFY(!studio.running());
        // A pending share may not have published its active context yet.
        studio.setVideoSink(&first);studio.start();studio.setBroadcastCaptureEnabled(true,720,30,false);
        studio.releasePreview(&first);QVERIFY(studio.running());studio.setBroadcastCaptureEnabled(false,720,30,false);QVERIFY(!studio.running());
        // Destroying an owner also releases preview work without interrupting
        // its live share, then stops the retained input when sharing ends.
        auto *owned = new QVideoSink;studio.setVideoSink(owned);studio.start();
        studio.setBroadcastContext({{"active",true}});studio.setBroadcastCaptureEnabled(true,0,0,false);
        delete owned;QVERIFY(!studio.videoSink());QVERIFY(studio.running());
        QVERIFY(!studio.processingDiagnostics().value("previewAttached").toBool());
        studio.setBroadcastCaptureEnabled(false,0,0,false);studio.setBroadcastContext({{"active",false}});QVERIFY(!studio.running());
        // Legacy programmatic sources do not opt into editor-owned lifetime.
        studio.start();studio.setBroadcastContext({{"active",true}});studio.setBroadcastContext({{"active",false}});QVERIFY(studio.running());studio.stop();
    }

    void bufferedOriginalVideoFollowsMediaTimeWithoutLosingItsChain() {
        if(QStandardPaths::findExecutable("ffmpeg").isEmpty())QSKIP("FFmpeg generates the buffered RTMP source");
        RtmpPlaybackFixture source;source.fps=60;source.gopSeconds=30;source.initialBurstSeconds=5;source.start();
        QVERIFY(source.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(source.listening(),3000);
        StudioBridge studio;studio.setInputMode(StudioBridge::ExistingRtmpStream);studio.setRemoteServer(source.server());studio.setRemoteStreamKey("fixture-key");studio.start();
        // Arm before probing consumes the first IDR. This fixture isolates
        // delivery bursts from BroadcastEngine and its recovery fallback.
        studio.setBroadcastCaptureEnabled(true,0,0,false);
        bool firstWasKeyframe=false,monotonic=true;int frames=0;quint32 first=0,last=0;qint64 maxLeadMs=0;QElapsedTimer timeline;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{
            const auto video=studio.takeEncodedBroadcastVideo();
            for(const auto &frame:video.frames){
                if(!timeline.isValid()){firstWasKeyframe=frame.keyframe;timeline.start();first=frame.timestamp;}
                else monotonic=monotonic && qint32(frame.timestamp-last)>0;
                last=frame.timestamp;++frames;
                maxLeadMs=std::max(maxLeadMs,qint64(quint32(frame.timestamp-first)/90)-timeline.elapsed());
            }
            return frames>=120 || !studio.error().isEmpty();
        })(),5000);
        QVERIFY2(studio.error().isEmpty(),qPrintable(studio.error()));QVERIFY(frames>=120);QVERIFY(firstWasKeyframe);QVERIFY(monotonic);
        QVERIFY2(maxLeadMs<150,"Buffered RTMP frames must not race ahead of their media clock and overflow the dependency chain");
        qInfo("GeneratedBufferedRtmp frames=%d maxLeadMs=%lld",frames,qlonglong(maxLeadMs));
        studio.stop();
    }

    void bFrameSourceUsesCompatibleVideoAtSourceSizeAndCadence() {
        if(QStandardPaths::findExecutable("ffmpeg").isEmpty())QSKIP("FFmpeg generates the loopback source");
        RtmpPlaybackFixture source;source.size=QSize(1280,720);source.fps=24;source.bFrames=2;source.start();
        QVERIFY(source.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(source.listening(),3000);
        StudioBridge studio;studio.setInputMode(StudioBridge::ExistingRtmpStream);studio.setRemoteServer(source.server());studio.setRemoteStreamKey("fixture-key");studio.start();
        QTRY_VERIFY_WITH_TIMEOUT(studio.receiving() || !studio.error().isEmpty(),10000);QVERIFY2(studio.receiving(),qPrintable(studio.error()));
        studio.setBroadcastCaptureEnabled(true,0,0,false);
        RelayBroadcastMedia media;RelayEncodedVideo encoded;QVideoFrame full;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{media=studio.takeBroadcastMedia();if(media.video.isValid())full=media.video;encoded=studio.takeEncodedBroadcastVideo();return full.isValid() && encoded.fps>0;})(),3000);
        QVERIFY(!encoded.available);QVERIFY(encoded.frames.isEmpty());QCOMPARE(encoded.fps,24);
        QCOMPARE(QSize(encoded.width,encoded.height),source.size);QCOMPARE(full.size(),source.size);
        QVERIFY(media.audio.isEmpty());QVERIFY(media.error.isEmpty());studio.stop();
    }

    void originalVideoMailboxResumesOnKeyframesAndClearsOnDisarm() {
        if(QStandardPaths::findExecutable("ffmpeg").isEmpty())QSKIP("FFmpeg generates the loopback source");
        RtmpPlaybackFixture source;source.fps=60;source.start();
        QVERIFY(source.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(source.listening(),3000);
        StudioBridge studio;studio.setInputMode(StudioBridge::ExistingRtmpStream);studio.setRemoteServer(source.server());studio.setRemoteStreamKey("fixture-key");studio.start();
        QTRY_VERIFY_WITH_TIMEOUT(studio.receiving() || !studio.error().isEmpty(),10000);QVERIFY2(studio.receiving(),qPrintable(studio.error()));
        QVERIFY(!studio.takeEncodedBroadcastVideo().available);
        studio.setBroadcastCaptureEnabled(true,0,0,true);
        RelayEncodedVideo video;
        QTRY_VERIFY_WITH_TIMEOUT(!video.frames.isEmpty() || (video=studio.takeEncodedBroadcastVideo(),!video.frames.isEmpty()),3000);
        QVERIFY(video.available);QCOMPARE(video.fps,60);QVERIFY(video.frames.first().keyframe);
        for(const auto &frame:video.frames)if(frame.keyframe){const auto types=h264Types(frame.h264);QVERIFY(types.contains(5));QVERIFY(types.contains(7));QVERIFY(types.contains(8));}
        QTest::qWait(650);video=studio.takeEncodedBroadcastVideo();QVERIFY(video.frames.size()<=16);
        // Any overflow discards the dependency chain. The next delivered batch
        // must begin at an original IDR, even if consumption stalls repeatedly.
        if(!video.frames.isEmpty())QVERIFY(video.frames.first().keyframe);
        for(const auto &frame:video.frames)if(frame.keyframe){const auto types=h264Types(frame.h264);QVERIFY(types.contains(5));QVERIFY(types.contains(7));QVERIFY(types.contains(8));}
        video={};QTRY_VERIFY_WITH_TIMEOUT(!video.frames.isEmpty() || (video=studio.takeEncodedBroadcastVideo(),!video.frames.isEmpty()),3000);
        studio.setBroadcastCaptureEnabled(false,0,0,false);QTest::qWait(100);
        video=studio.takeEncodedBroadcastVideo();QVERIFY(!video.available);QVERIFY(video.frames.isEmpty());QVERIFY(studio.takeBroadcastMedia().audio.isEmpty());
        studio.setBroadcastCaptureEnabled(true,720,30,false);QTest::qWait(100);QVERIFY(!studio.takeEncodedBroadcastVideo().available);
        QVERIFY(studio.takeBroadcastMedia().video.isValid());
        studio.setRelayOriginalVideoEnabled(true);video={};
        QTRY_VERIFY_WITH_TIMEOUT((video.available && !video.frames.isEmpty()) || (video=studio.takeEncodedBroadcastVideo(),video.available && !video.frames.isEmpty()),3000);
        QCOMPARE(video.width,source.size.width());QVERIFY(studio.takeBroadcastMedia().video.isValid());
        studio.setRelayOriginalVideoEnabled(false);QTest::qWait(100);QVERIFY(!studio.takeEncodedBroadcastVideo().available);
        studio.stop();
    }

    void broadcastMediaKeepsFullVideoAndResamplesBoundedAudio() {
        if(QStandardPaths::findExecutable("ffmpeg").isEmpty())QSKIP("FFmpeg generates the loopback source");
        RtmpPlaybackFixture source;source.size=QSize(1920,1080);source.start();
        QVERIFY(source.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(source.listening(),3000);
        StudioBridge studio;QVideoSink sink;studio.setVideoSink(&sink);
        studio.setInputMode(StudioBridge::ExistingRtmpStream);studio.setRemoteServer(source.server());studio.setRemoteStreamKey("fixture-key");studio.start();
        QTRY_VERIFY_WITH_TIMEOUT(studio.receiving() || !studio.error().isEmpty(),15000);QVERIFY2(studio.receiving(),qPrintable(studio.error()));
        // Receiving is published before the queued GUI preview. Wait for the
        // actual frame instead of relying on machine speed or signal ordering.
        QTRY_COMPARE_WITH_TIMEOUT(sink.videoFrame().size(),QSize(960,540),3000);
        auto media=studio.takeBroadcastMedia();QVERIFY(!media.video.isValid());QVERIFY(media.audio.isEmpty());
        studio.setBroadcastCaptureEnabled(true,1080,30,true);
        QVideoFrame full;QByteArray pcm;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{media=studio.takeBroadcastMedia();if(media.video.isValid())full=media.video;if(!media.audio.isEmpty())pcm=media.audio;return full.isValid()&&!pcm.isEmpty();})(),5000);
        QCOMPARE(full.size(),QSize(1920,1080));QVERIFY(media.error.isEmpty());QVERIFY(media.audioError.isEmpty());
        QVERIFY(pcm.size()%4==0);QVERIFY(pcm.size()<=38400);
        int nonzero=0;for(int at=0;at+3<pcm.size();at+=4){qint16 left=0,right=0;std::memcpy(&left,pcm.constData()+at,2);std::memcpy(&right,pcm.constData()+at+2,2);QCOMPARE(left,right);if(qAbs(int(left))>100)++nonzero;}QVERIFY(nonzero>50);
        QTest::qWait(500);media=studio.takeBroadcastMedia();QVERIFY(media.audio.size()<=38400);QVERIFY(media.video.isValid());
        studio.setBroadcastCaptureEnabled(false,1080,30,false);QTest::qWait(100);media=studio.takeBroadcastMedia();QVERIFY(media.audio.isEmpty());QVERIFY(!media.video.isValid());
        QTRY_COMPARE_WITH_TIMEOUT(sink.videoFrame().size(),QSize(960,540),2000);
        studio.setBroadcastCaptureEnabled(true,720,30,false);QTest::qWait(150);media=studio.takeBroadcastMedia();QVERIFY(media.audio.isEmpty());QCOMPARE(media.video.size(),QSize(1280,720));
        QSignalSpy stopped(&studio,&StudioBridge::broadcastStopRequested);studio.stop();QCOMPARE(stopped.size(),1);QVERIFY(studio.takeBroadcastMedia().ended);
    }

    void remoteAddressValidationDoesNotStartAWorkerOrExposeSecrets() {
        StudioBridge studio;
        studio.setInputMode(StudioBridge::ExistingRtmpStream);
        const QString secret="fixture-secret-do-not-log";
        studio.setRemoteStreamKey(secret);
        for (const auto &address : {QString{},QString("file:///tmp/fixture.flv"),QString("https://example.invalid/live"),
             QString("rtmp:///live"),QString("rtmp://127.0.0.1:0/live"),QString("rtmp://127.0.0.1/live#fragment"),
             QString("rtmp://127.0.0.1/live\nmalformed")}) {
            studio.setRemoteServer(address);studio.start();
            QVERIFY(!studio.running());QVERIFY(!studio.receiving());QVERIFY(!studio.error().isEmpty());
            QVERIFY(!studio.error().contains(secret));QVERIFY(!studio.status().contains(address) || address.isEmpty());
        }
        studio.setInputMode(StudioBridge::DirectFromObs);
        QVERIFY(studio.error().isEmpty());
    }

    void remoteHandshakeCanBeCancelledAndRetriedWithoutOpeningTheLocalListener() {
        QTcpServer stalled;QVERIFY(stalled.listen(QHostAddress::LocalHost,0));
        QTcpServer directPort;QVERIFY(directPort.listen(QHostAddress::LocalHost,45990));
        StudioBridge studio;studio.setInputMode(StudioBridge::ExistingRtmpStream);
        studio.setRemoteServer("rtmp://127.0.0.1:"+QString::number(stalled.serverPort())+"/live");
        studio.setRemoteStreamKey("fixture-private-key");
        for(int attempt=0;attempt<2;++attempt){
            studio.start();QVERIFY(studio.running());
            QTRY_VERIFY_WITH_TIMEOUT(stalled.hasPendingConnections(),3000);
            auto*connection=stalled.nextPendingConnection();QVERIFY(connection);
            QElapsedTimer timer;timer.start();studio.stop();
            QVERIFY2(timer.elapsed()<2500,"Stopping an RTMP connection must interrupt handshake waits");
            QVERIFY(!studio.running());QVERIFY(!studio.receiving());QCOMPARE(studio.frameCount(),0);
            delete connection;
        }
    }

    void readsExistingRtmpStream_data() {
        QTest::addColumn<bool>("completeUrl");
        QTest::newRow("server-plus-key")<<false;
        QTest::newRow("complete-stream-url")<<true;
    }

    void readsExistingRtmpStream() {
        if(QStandardPaths::findExecutable("ffmpeg").isEmpty())QSKIP("FFmpeg generates the loopback RTMP playback server");
        QFETCH(bool,completeUrl);
        RtmpPlaybackFixture source;source.start();
        QVERIFY(source.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(source.listening(),3000);
        StudioBridge studio;QVideoSink sink;studio.setVideoSink(&sink);
        studio.setInputMode(StudioBridge::ExistingRtmpStream);
        studio.setRemoteServer(source.server()+(completeUrl?"/fixture-key":"/"));
        studio.setRemoteStreamKey(completeUrl?QString{}:QString("fixture-key"));
        studio.start();
        QTRY_VERIFY_WITH_TIMEOUT(studio.frameCount()>=3 || !studio.error().isEmpty(),15000);
        QVERIFY2(studio.frameCount()>=3,qPrintable(studio.error()));
        QVERIFY(studio.receiving());QVERIFY(studio.hasAudio());QCOMPARE(studio.videoCodec(),"H264");
        QCOMPARE(studio.resolution(),QString::fromUtf8("320 × 180"));QVERIFY(sink.videoFrame().isValid());
        QVERIFY(studio.status().contains("RTMP"));QVERIFY(!studio.status().contains("fixture-key"));
        const auto server=studio.remoteServer();const auto key=studio.remoteStreamKey();
        studio.setInputMode(StudioBridge::DirectFromObs);studio.setRemoteServer("rtmp://example.invalid/live");studio.setRemoteStreamKey("different");
        QCOMPARE(studio.inputMode(),StudioBridge::ExistingRtmpStream);QCOMPARE(studio.remoteServer(),server);QCOMPARE(studio.remoteStreamKey(),key);
        QVERIFY(!studio.broadcastAvailable());
        QElapsedTimer timer;timer.start();studio.stop();QVERIFY(timer.elapsed()<2500);QVERIFY(!sink.videoFrame().isValid());
    }

    void cancellationWhileWaiting() {
        StudioBridge studio;
        studio.start();
        QVERIFY(studio.running());
        QTest::qWait(250);
        QElapsedTimer timer; timer.start();
        studio.stop();
        QVERIFY2(timer.elapsed() < 2500, "Stopping a listener must interrupt its network wait");
        QVERIFY(!studio.running());
        QVERIFY(!studio.receiving());
        QCOMPARE(studio.frameCount(), 0);
    }
    void reportsPortConflict() {
        QTcpServer blocker;
        QVERIFY(blocker.listen(QHostAddress::LocalHost, 45990));
        StudioBridge studio;
        studio.start();
        QTRY_VERIFY_WITH_TIMEOUT(!studio.running(), 5000);
        QVERIFY(!studio.error().isEmpty());
        QVERIFY(!studio.receiving());
    }
    void receivesRealVideoAndAudio() {
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) QSKIP("FFmpeg executable is needed to generate an OBS-compatible test source");
        StudioBridge studio;
        QVideoSink sink;
        studio.setVideoSink(&sink);
        studio.start();
        QTest::qWait(350);
        QProcess producer;
        producer.start("ffmpeg", {
            "-hide_banner", "-loglevel", "error", "-re", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=15",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-t", "5",
            "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-g", "15",
            "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "64k", "-f", "flv", studio.obsServer() + "/" + studio.streamKey()});
        QVERIFY(producer.waitForStarted(2000));
        QTRY_VERIFY_WITH_TIMEOUT(studio.frameCount() >= 3 || !studio.error().isEmpty() || producer.state() == QProcess::NotRunning, 15000);
        const auto diagnostic = (studio.error() + " " + QString::fromUtf8(producer.readAllStandardError())).toUtf8();
        QVERIFY2(studio.frameCount() >= 3, diagnostic.constData());
        QVERIFY(studio.receiving());
        QVERIFY(studio.hasAudio());
        QCOMPARE(studio.videoCodec(), QString("H264"));
        QCOMPARE(studio.resolution(), QString::fromUtf8("320 × 180"));
        QVERIFY(sink.videoFrame().isValid());
        QCOMPARE(sink.videoFrame().size(), QSize(320, 180));
        QVERIFY(!studio.broadcastAvailable());
        producer.terminate();
        if (!producer.waitForFinished(3000)) { producer.kill(); producer.waitForFinished(); }
        studio.stop();
        QVERIFY(!sink.videoFrame().isValid());
    }
};

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    StudioTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "studio_test.moc"
