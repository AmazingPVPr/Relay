#include "BroadcastEngine.h"
#include "BroadcastEncoder.h"
#include "BroadcastPacket.h"
#include "BroadcastTransport.h"
#include "SystemAudioCapture.h"
#include "addons/BroadcastSourceInterface.h"
#include "addons/EncodedBroadcastSourceInterface.h"
#include <QGuiApplication>
#include <QMediaCaptureSession>
#include <QScreenCapture>
#include <QWindowCapture>
#include <QVideoSink>
#include <QScreen>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <opus/opus.h>
#include <atomic>
#include <array>
#include <cstring>
#include <algorithm>
#include <deque>

namespace {
bool validId(const QString &id){bool ok=false;id.toULongLong(&ok);return ok && !id.isEmpty() && id.size()<=22;}
int streamBitrate(int height,int fps){
    const int at60=height>1440?40000000:height>1080?24000000:height>720?16000000:8000000;
    return fps>30?at60:at60*3/4;
}
}
struct BroadcastEngine::Private {
    BroadcastEngine *q;BroadcastTransport transport;
    QTimer poll,timeout;QElapsedTimer clock;
    QString guild,channel,user,session,sourceName,status="Not sharing",error;
    bool active=false,requested=false,shareAudio=false,audioActive=false,stopping=false;
    bool externalMode=false,audioAttempted=false,directViewers=false;
    bool audioSuppressed=false,audioSuppressionRequested=false,audioResumePending=false;
    QElapsedTimer audioPauseClock,audioResumeClock;
    QPointer<QObject> external;
    RelayBroadcastSourceInterface *externalInterface=nullptr;
    RelayEncodedBroadcastSourceInterface *encodedInterface=nullptr;
    bool sourceQuality=false,passthrough=false,waitingForIdr=true;
    QElapsedTimer originalProgress,originalKeyframeWait;
    struct OriginalSample { qint64 at=0;qsizetype bytes=0; };
    std::deque<OriginalSample> originalSamples;
    qsizetype originalBytes=0;
    qint64 originalSampleStart=-1,originalOverBudgetAt=-1;
    int originalMeasuredBitrate=0;
    bool encoderTimestampNeedsRebase=false,haveVideoTimestamp=false;
    quint32 encoderTimestampOffset=0,lastVideoTimestamp=0;
    qint64 lastVideoAt=0;
    QMetaObject::Connection sourceDestroyed;
    int fps=30,heightLimit=720,bitrate=4000000,width=0,height=0;
    quint64 previousFrames=0;quint32 audioTimestamp=0;
    quint64 captureGeneration=0;
    QVariantList sources;
    struct SourceStats {
        int path=0,width=0,height=0,sourceFps=0,outputFps=0,bitrate=0;
        quint64 offeredOriginal=0,initialGated=0,recoveryGated=0,publishRejected=0,encoderDropped=0,encoded=0;
        bool forwardedOriginal=false;quint64 originalRecoveries=0;QString recoveryReason;
    } sourceStats;
    QList<QPointer<QScreen>> screens;QList<QCapturableWindow> windows;
    std::unique_ptr<QMediaCaptureSession> captureSession;
    std::unique_ptr<QScreenCapture> screenCapture;
    std::unique_ptr<QWindowCapture> windowCapture;
    QVideoSink captureSink;
    QMutex frameMutex;QVideoFrame captured;std::atomic_bool frameQueued{false};
    std::unique_ptr<BroadcastEncoder> encoder;
    std::unique_ptr<SystemAudioCapture> audio;
    OpusEncoder *opus=nullptr;QByteArray pcm;
    explicit Private(BroadcastEngine *owner):q(owner),transport(owner),captureSink(owner){clock.start();}
    ~Private(){stopCapture();}
    void notify(){emit q->changed();}
    void clearAudio(){
        audio.reset();pcm.clear();audioActive=false;audioAttempted=false;
        if(opus)opus_encoder_destroy(opus);
        opus=nullptr;
    }
    void finishAudioResume(){
        clearAudio();
        if(audioPauseClock.isValid())audioTimestamp+=quint32(audioPauseClock.elapsed()/20*960);
        audioPauseClock.invalidate();audioResumeClock.invalidate();
        audioSuppressed=false;audioResumePending=false;notify();
    }
    void stopCapture(){
        poll.stop();timeout.stop();
        if(external && external->metaObject()->indexOfMethod("setRelayOriginalVideoEnabled(bool)")>=0)QMetaObject::invokeMethod(external,"setRelayOriginalVideoEnabled",Q_ARG(bool,false));
        if(external && externalInterface)externalInterface->setBroadcastCaptureEnabled(false,heightLimit,fps,false);
        QObject::disconnect(sourceDestroyed);external.clear();externalInterface=nullptr;encodedInterface=nullptr;externalMode=false;sourceQuality=false;passthrough=false;
        originalProgress.invalidate();originalKeyframeWait.invalidate();
        if(screenCapture)screenCapture->stop();
        if(windowCapture)windowCapture->stop();
        captureSession.reset();screenCapture.reset();windowCapture.reset();
        {QMutexLocker lock(&frameMutex);captured={};}
        encoder.reset();clearAudio();
        captureSink.setVideoFrame({});
    }
    void fail(const QString &reason){error=reason;q->stop();status="Stream stopped";notify();}
    void startTransport(int videoWidth,int videoHeight){
        if(requested)return;
        timeout.stop();requested=true;originalProgress.invalidate();originalKeyframeWait.invalidate();status="Connecting stream…";notify();
        emit q->startRequested(guild,channel);
        if(!active)return;
        if(!validId(user) || session.isEmpty()){fail("Join voice before starting a stream.");return;}
        const auto key="guild:"+guild+":"+channel+":"+user;
        sourceStats.width=videoWidth;sourceStats.height=videoHeight;sourceStats.outputFps=fps;sourceStats.bitrate=bitrate;
        transport.setIdentity(user,session);transport.publish(key,videoWidth,videoHeight,fps,bitrate);
    }
    void frameReady(){
        QVideoFrame frame;{QMutexLocker lock(&frameMutex);frame=std::move(captured);captured={};frameQueued=false;}
        if(!active || !frame.isValid() || !encoder)return;
        emit q->sourceFrame(frame,heightLimit,fps);
        if(!requested){
            const double scale=std::min({1.0,double(heightLimit)/frame.height(),3840.0/frame.width()});
            startTransport(std::max(2,int(frame.width()*scale)&~1),std::max(2,int(frame.height()*scale)&~1));
        }
        if(active && transport.encryptedReady())encoder->submit(frame);
    }
    void clearOriginalRate(){
        originalSamples.clear();originalBytes=0;originalSampleStart=-1;
        originalOverBudgetAt=-1;originalMeasuredBitrate=0;
    }
    bool observeOriginalRate(qsizetype bytes){
        const auto now=clock.elapsed();
        if(originalSampleStart<0)originalSampleStart=now;
        originalSamples.push_back({now,bytes});originalBytes+=bytes;
        while(!originalSamples.empty() && (now-originalSamples.front().at>1000 || originalSamples.size()>240)){
            originalBytes-=originalSamples.front().bytes;originalSamples.pop_front();
        }
        if(now-originalSampleStart<1000 || originalSamples.size()<2)return false;
        originalMeasuredBitrate=int(std::min<qint64>(1000000000,
            qint64(originalBytes)*8000/std::max<qint64>(1,now-originalSamples.front().at)));
        // A transient IDR burst may exceed the video budget; only sustained
        // source traffic that cannot fit requires conversion. Quiet original
        // streams remain untouched even if their configured ceiling is high.
        if(originalMeasuredBitrate>qint64(bitrate)*105/100){
            if(originalOverBudgetAt<0)originalOverBudgetAt=now;
            return now-originalOverBudgetAt>=1000;
        }
        originalOverBudgetAt=-1;return false;
    }
    void recoverOriginal(const QString &reason){
        // An RTMP playback client cannot ask OBS to produce an IDR. Keep the
        // original bitstream while healthy, but never leave viewers waiting
        // indefinitely for a long source GOP after startup or a lost chain.
        sourceQuality=false;passthrough=false;waitingForIdr=true;
        originalProgress.invalidate();originalKeyframeWait.invalidate();
        // Source quality is a maximum, not permission to exceed the receiving
        // server's current bandwidth allocation.
        const int currentBudget=transport.publishingBitrate();
        if(currentBudget>0)bitrate=std::min(bitrate,currentBudget);
        sourceStats.bitrate=bitrate;
        encoder=std::make_unique<BroadcastEncoder>(heightLimit,fps,bitrate);
        encoderTimestampNeedsRebase=true;
        externalInterface->setBroadcastCaptureEnabled(true,heightLimit,fps,shareAudio);
        sourceStats.path=2;++sourceStats.originalRecoveries;sourceStats.recoveryReason=reason;
        notify();
    }
    void videoPublished(quint32 timestamp){
        haveVideoTimestamp=true;lastVideoTimestamp=timestamp;lastVideoAt=clock.elapsed();
    }
    void process(){
        if(!active || (!encoder && !passthrough))return;
        if(externalMode){
            if(!external || !externalInterface){fail("The OBS input is no longer available.");return;}
            const auto encoded=encodedInterface?encodedInterface->takeEncodedBroadcastVideo():RelayEncodedVideo{};
            if(encoded.available) for(const auto &frame:encoded.frames) emit q->sourceVideo(frame.h264,frame.timestamp,frame.keyframe);
            if(sourceQuality && encodedInterface){
                if(encoded.fps>=1 && encoded.fps<=60)sourceStats.sourceFps=encoded.fps;
                if(!encoded.available && !passthrough && !requested && encoded.fps>=1 && encoded.fps<=60 && fps!=encoded.fps){
                    fps=encoded.fps;bitrate=streamBitrate(heightLimit,fps);
                    encoder=std::make_unique<BroadcastEncoder>(heightLimit,fps,bitrate);
                }
                if(encoded.available){
                    if(encoded.width<2 || encoded.width>3840 || encoded.height<2 || encoded.height>2160 || encoded.fps<1 || encoded.fps>60 || encoded.frames.size()>16){fail("The source supplied unsupported stream video.");return;}
                    if(!passthrough){passthrough=true;encoder.reset();fps=encoded.fps;notify();}
                    sourceStats.path=3;sourceStats.width=encoded.width;sourceStats.height=encoded.height;sourceStats.sourceFps=encoded.fps;sourceStats.outputFps=fps;sourceStats.bitrate=bitrate;
                    // Allocate from validated source metadata, before waiting
                    // for its next IDR. Otherwise the first IDR is consumed by
                    // allocation and we needlessly wait another entire GOP.
                    startTransport(encoded.width,encoded.height);
                    if(!active)return;
                    if(transport.encryptedReady()){
                        if(!originalProgress.isValid())originalProgress.start();
                    }else originalProgress.invalidate();
                    const bool stalled=originalProgress.isValid() && originalProgress.elapsed()>=2500;
                    const bool unanswered=originalKeyframeWait.isValid() && originalKeyframeWait.elapsed()>=2500;
                    if(stalled || unanswered)recoverOriginal(!sourceStats.forwardedOriginal?"initial-keyframe":unanswered?"viewer-keyframe":"source-keyframe");
                    else {
                        for(const auto &frame:encoded.frames){
                            ++sourceStats.offeredOriginal;
                            if(observeOriginalRate(frame.h264.size())){
                                recoverOriginal("receiver-bandwidth");break;
                            }
                            const auto gated=[&]{if(sourceStats.forwardedOriginal)++sourceStats.recoveryGated;else ++sourceStats.initialGated;};
                            if(!transport.encryptedReady()){waitingForIdr=true;gated();continue;}
                            if(waitingForIdr && !frame.keyframe){gated();continue;}
                            const auto normalized=BroadcastPacket::normalizeH264(frame.h264);
                            if(normalized.isEmpty() || !transport.publishVideo(normalized,frame.timestamp,encoded.width,encoded.height)){++sourceStats.publishRejected;waitingForIdr=true;continue;}
                            waitingForIdr=false;sourceStats.forwardedOriginal=true;originalProgress.restart();videoPublished(frame.timestamp);
                            if(frame.keyframe)originalKeyframeWait.invalidate();
                            if(width!=encoded.width || height!=encoded.height){width=encoded.width;height=encoded.height;notify();}
                        }
                    }
                }else if(passthrough){fail("The source video changed. Start sharing again to use its new format.");return;}
            }
            const auto media=externalInterface->takeBroadcastMedia();
            if(!media.error.isEmpty() || media.ended){fail(media.error.isEmpty()?"The OBS input ended. Reconnect it to stream again.":media.error);return;}
            if(!passthrough && media.video.isValid())captureSink.setVideoFrame(media.video);
            if(shareAudio && !audioSuppressed && !media.audio.isEmpty()) emit q->sourceAudio(media.audio);
            if(shareAudio && !audioSuppressed && transport.encryptedReady()){
                if(!media.audioError.isEmpty())error=media.audioError;
                if(!media.audio.isEmpty() && media.audio.size()<=48000*4/5 && media.audio.size()%4==0){pcm+=media.audio;if(!audioActive){audioActive=true;notify();}}
            }else pcm.clear();
        }
        auto snapshot=encoder?encoder->take():BroadcastEncoder::Snapshot{};
        if(encoder){sourceStats.encoderDropped=snapshot.dropped;sourceStats.encoded=snapshot.encoded;}
        if(!snapshot.error.isEmpty()){fail(snapshot.error);return;}
        for(const auto &frame:snapshot.frames){
            width=frame.width;height=frame.height;
            sourceStats.width=width;sourceStats.height=height;sourceStats.outputFps=fps;sourceStats.bitrate=bitrate;
            if(encoderTimestampNeedsRebase){
                // A replacement encoder starts at zero; preserve the running
                // RTP clock across recovery or receivers reject its pictures.
                encoderTimestampOffset=haveVideoTimestamp
                    ?lastVideoTimestamp+quint32(std::max<qint64>(1,clock.elapsed()-lastVideoAt)*90)-frame.timestamp:0;
                encoderTimestampNeedsRebase=false;
            }
            const auto timestamp=frame.timestamp+encoderTimestampOffset;
            if(transport.encryptedReady()){
                if(transport.publishVideo(frame.bytes,timestamp,frame.width,frame.height))videoPublished(timestamp);
                else ++sourceStats.publishRejected;
            }
        }
        if(shareAudio && !audioSuppressed && (transport.encryptedReady() || directViewers) && !audioAttempted){
            audioAttempted=true;
            if(!externalMode){audio=std::make_unique<SystemAudioCapture>();audio->start();}
            int result=0;opus=opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&result);
            if(opus && result==OPUS_OK){opus_encoder_ctl(opus,OPUS_SET_BITRATE(256000));opus_encoder_ctl(opus,OPUS_SET_COMPLEXITY(10));}
            else {if(opus)opus_encoder_destroy(opus);opus=nullptr;error="Video is sharing, but the computer-audio encoder is unavailable.";}
        }
        if(audio && !audioSuppressed){
            auto snapshot=audio->take();
            if(!snapshot.error.isEmpty())error=snapshot.error;
            if(audioActive!=snapshot.active){audioActive=snapshot.active;notify();}
            if(shareAudio && !audioSuppressed && !snapshot.pcm.isEmpty()) emit q->sourceAudio(snapshot.pcm);
            pcm+=snapshot.pcm;
        }
        if(shareAudio && !audioSuppressed){
            if(pcm.size()>3840*5){const int dropped=(pcm.size()/3840-3)*3840;pcm.remove(0,dropped);audioTimestamp+=dropped/4;}
            if(opus && transport.encryptedReady() && pcm.size()>=3840){
                std::array<opus_int16,1920> samples;std::memcpy(samples.data(),pcm.constData(),3840);pcm.remove(0,3840);
                QByteArray encoded(1275,Qt::Uninitialized);const int count=opus_encode(opus,samples.data(),960,reinterpret_cast<unsigned char*>(encoded.data()),encoded.size());
                if(count>0){encoded.resize(count);transport.publishAudio(encoded,audioTimestamp);}audioTimestamp+=960;
            }
        }
        // Drain and discard the current external batch before reopening audio.
        // This remains safe if the GUI was busy when the tail timer expired.
        if(audioResumePending && !audioSuppressionRequested && audioResumeClock.elapsed()>=300)finishAudioResume();
        if(previousFrames!=transport.frameCount()){previousFrames=transport.frameCount();notify();}
    }
};
BroadcastEngine::BroadcastEngine(QObject *parent):QObject(parent),d(std::make_unique<Private>(this)){
    d->poll.setInterval(20);d->poll.setTimerType(Qt::PreciseTimer);d->timeout.setSingleShot(true);
    connect(&d->poll,&QTimer::timeout,this,[this]{d->process();});
    connect(&d->timeout,&QTimer::timeout,this,[this]{d->fail(d->externalMode?"No OBS video arrived. Reconnect the input and try again.":"No screen was selected. Start sharing again and choose a screen or window.");});
    connect(&d->captureSink,&QVideoSink::videoFrameChanged,this,[this](const QVideoFrame &frame){
        if(!frame.isValid())return;
        {QMutexLocker lock(&d->frameMutex);d->captured=frame;}
        if(!d->frameQueued.exchange(true))QMetaObject::invokeMethod(this,[this]{d->frameReady();},Qt::QueuedConnection);
    },Qt::DirectConnection);
    connect(&d->transport,&BroadcastTransport::keyFrameRequested,this,[this]{
        // Keep a valid original P-picture chain flowing while waiting for a
        // source IDR. If OBS cannot supply one within a normal two-second GOP,
        // recover through our encoder so a new viewer can actually start.
        if(d->passthrough){if(!d->originalKeyframeWait.isValid())d->originalKeyframeWait.start();}
        else if(d->encoder){d->encoder->submit(d->captureSink.videoFrame());d->encoder->requestKeyframe();}
    });
    connect(&d->transport,&BroadcastTransport::publishingBitrateChanged,this,[this](int bitrate){
        if(!d->active || bitrate<500000)return;
        d->bitrate=bitrate;d->sourceStats.bitrate=bitrate;
        if(d->encoder)d->encoder->setBitrate(bitrate);
        emit changed();
    });
    connect(&d->transport,&BroadcastTransport::changed,this,[this]{
        if(!d->active || d->stopping)return;
        if(!d->transport.error().isEmpty()){d->fail(d->transport.error());return;}
        if(d->requested && d->transport.active()){d->status=d->transport.status();emit changed();}
    });
    connect(&d->transport,&BroadcastTransport::stopRequested,this,[this](const QString &){if(d->active && !d->stopping)stop();});
}
BroadcastEngine::~BroadcastEngine(){stop();}
bool BroadcastEngine::available()const{return d->transport.available() && BroadcastEncoder::available();}
bool BroadcastEngine::active()const{return d->active;}bool BroadcastEngine::sharing()const{return d->active && d->transport.connected();}
bool BroadcastEngine::audioActive()const{return d->audioActive;}bool BroadcastEngine::usesSystemPicker()const{return QGuiApplication::platformName().contains("wayland");}
QString BroadcastEngine::status()const{return d->status;}QString BroadcastEngine::error()const{return d->error;}QString BroadcastEngine::sourceName()const{return d->sourceName;}
QString BroadcastEngine::diagnosticSummary()const{
    const auto &s=d->sourceStats;
    const char *path=s.path==3?"obs-original":s.path==2?"obs-converted":s.path==1?"screen-encoded":"none";
    return d->transport.diagnosticSummary()+QString(" source(path=%1,size=%2x%3,sourceFps=%4,outputFps=%5,targetBitrate=%6,offered=%7,initialGate=%8,recoveryGate=%9,publishReject=%10,encoderDrop=%11,encoded=%12,recoveries=%13,recovery=%14)")
        .arg(QLatin1String(path)).arg(s.width).arg(s.height).arg(s.sourceFps).arg(s.outputFps).arg(s.bitrate)
        .arg(s.offeredOriginal).arg(s.initialGated).arg(s.recoveryGated).arg(s.publishRejected).arg(s.encoderDropped).arg(s.encoded).arg(s.originalRecoveries).arg(s.recoveryReason);
}
QVariantMap BroadcastEngine::publishingDiagnostics()const{
    auto result=d->transport.publishingDiagnostics();const auto &s=d->sourceStats;
    result.insert("active",d->active);result.insert("sharing",sharing());result.insert("audioActive",d->audioActive);
    result.insert("sourcePath",s.path);result.insert("sourceWidth",s.width);result.insert("sourceHeight",s.height);
    result.insert("sourceFps",s.sourceFps);result.insert("outputFps",s.outputFps);result.insert("targetBitrate",s.bitrate);
    result.insert("originalOffered",s.offeredOriginal);result.insert("originalInitialGated",s.initialGated);
    result.insert("originalRecoveryGated",s.recoveryGated);result.insert("publishRejected",s.publishRejected);
    result.insert("encoderDropped",s.encoderDropped);result.insert("encoded",s.encoded);
    result.insert("originalMeasuredBitrate",d->originalMeasuredBitrate);
    result.insert("originalForwarded",s.forwardedOriginal);result.insert("originalRecoveries",s.originalRecoveries);
    if(d->external){
        const auto input=d->external->property("processingDiagnostics").toMap();
        for(const auto *key:{"videoPackets","decodedVideoFrames","convertedVideoFrames","replayedVideoPackets",
                            "replayRestores","replayLimitRestores","maxReplayWorkUs","backgroundReplayChunks",
                            "maxBackgroundReplayWorkUs","backgroundBudgetOverruns","replayBytes","replayPackets",
                            "maximumReplayBytes","maximumReplayPackets"}){
            const auto value=input.value(QLatin1String(key));
            const auto type=value.metaType().id();
            if(type==QMetaType::Int || type==QMetaType::UInt || type==QMetaType::LongLong || type==QMetaType::ULongLong)
                result.insert("source_"+QLatin1String(key),value);
        }
    }
    return result;
}
QVariantList BroadcastEngine::sources()const{return d->sources;}quint64 BroadcastEngine::framesSent()const{return d->transport.frameCount();}
int BroadcastEngine::videoWidth()const{return d->width;}int BroadcastEngine::videoHeight()const{return d->height;}
bool BroadcastEngine::externalSource()const{return d->active && d->externalMode;}
bool BroadcastEngine::audioSuppressed()const{return d->audioSuppressed;}
void BroadcastEngine::setAudioSuppressed(bool value){
    if(d->audioSuppressionRequested==value)return;
    d->audioSuppressionRequested=value;
    if(value){
        if(!d->audioSuppressed)d->audioPauseClock.start();
        d->audioSuppressed=true;d->audioResumePending=false;d->audioResumeClock.invalidate();
        d->clearAudio();emit changed();
    }else{
        d->audioResumePending=true;d->audioResumeClock.start();
        // No active input is polling while idle. An active input must drain its
        // last offered batch in process() before lifting this privacy gate.
        QTimer::singleShot(300,this,[this]{
            if(d->audioResumePending && !d->audioSuppressionRequested && d->audioResumeClock.elapsed()>=300 && !d->active)d->finishAudioResume();
        });
    }
}
QString BroadcastEngine::qualitySummary()const{
    if(!d->active || !d->width || !d->height)return {};
    return QString("%1 × %2 · %3 fps · %4").arg(d->width).arg(d->height).arg(d->fps)
        .arg(d->passthrough?"Original video":"High quality encoding");
}
bool BroadcastEngine::configureLocalFixture(const QUrl &url){return d->transport.configureLocalFixture(url);}
void BroadcastEngine::setIdentity(const QString &user,const QString &session){d->user=user;d->session=session;d->transport.setIdentity(user,session);}
void BroadcastEngine::refreshSources(){
    if(d->active)return;
    d->sources.clear();d->screens.clear();d->windows.clear();
    if(usesSystemPicker())d->sources.append(QVariantMap{{"id","portal"},{"name","Choose a screen or window with your desktop"}});
    else {
        for(auto *screen:QGuiApplication::screens()){
            const auto id=QString("screen:%1").arg(d->screens.size());d->screens.append(screen);
            d->sources.append(QVariantMap{{"id",id},{"name",QString("Screen · %1 (%2 × %3)").arg(screen->name()).arg(screen->size().width()).arg(screen->size().height())}});
        }
        d->windows=QWindowCapture::capturableWindows();
        for(qsizetype i=0;i<d->windows.size();++i)if(d->windows[i].isValid())d->sources.append(QVariantMap{{"id",QString("window:%1").arg(i)},{"name","Window · "+d->windows[i].description()}});
    }
    emit sourcesChanged();
}
void BroadcastEngine::start(const QString &guild,const QString &channel,const QString &source,int maximumHeight,int fps,bool computerAudio){
    if(d->active)return;
    if(!available() || !validId(guild) || !validId(channel)){d->error="Join a voice channel before sharing your screen.";emit changed();return;}
    bool sourceFound=false;for(const auto &entry:d->sources)if(entry.toMap().value("id").toString()==source){sourceFound=true;d->sourceName=entry.toMap().value("name").toString();break;}
    if(!sourceFound){d->error="Choose a screen or window to share.";emit changed();return;}
    d->error.clear();d->active=true;d->requested=false;d->guild=guild;d->channel=channel;d->shareAudio=computerAudio;d->audioTimestamp=0;
    d->encoderTimestampNeedsRebase=false;d->haveVideoTimestamp=false;d->encoderTimestampOffset=0;d->lastVideoTimestamp=0;d->lastVideoAt=0;
    d->originalProgress.invalidate();d->originalKeyframeWait.invalidate();
    d->clearOriginalRate();d->sourceStats={};d->sourceStats.path=1;
    const auto generation=++d->captureGeneration;
    d->fps=(fps==15 || fps==60)?fps:30;d->heightLimit=(maximumHeight==1080 || maximumHeight==1440 || maximumHeight==2160)?maximumHeight:720;
    d->bitrate=streamBitrate(d->heightLimit,d->fps);
    d->width=d->height=0;d->previousFrames=0;d->status=usesSystemPicker()?"Choose what to share in your desktop picker…":"Starting screen capture…";
    d->encoder=std::make_unique<BroadcastEncoder>(d->heightLimit,d->fps,d->bitrate);
    d->poll.setInterval(d->fps==60?10:20);
    d->captureSession=std::make_unique<QMediaCaptureSession>();d->captureSession->setVideoSink(&d->captureSink);
    d->poll.start();d->timeout.start(90000);emit changed();
    if(source=="portal" || source.startsWith("screen:")){
        d->screenCapture=std::make_unique<QScreenCapture>();
        if(source!="portal"){
            const auto index=source.section(':',1).toInt();if(index<0 || index>=d->screens.size() || !d->screens[index]){d->fail("The selected screen is no longer available.");return;}
            d->screenCapture->setScreen(d->screens[index]);
        }
#if QT_VERSION >= QT_VERSION_CHECK(6,12,0)
        d->screenCapture->setMaximumFrameRate(d->fps);
#endif
        d->captureSession->setScreenCapture(d->screenCapture.get());
        connect(d->screenCapture.get(),&QScreenCapture::errorOccurred,this,[this,generation](QScreenCapture::Error,const QString &error){QTimer::singleShot(0,this,[this,generation,error]{if(d->active && d->captureGeneration==generation)d->fail("Screen capture: "+error);});});
        connect(d->screenCapture.get(),&QScreenCapture::activeChanged,this,[this,generation](bool active){if(!active)QTimer::singleShot(0,this,[this,generation]{if(d->active && d->captureGeneration==generation && d->screenCapture && d->screenCapture->error()==QScreenCapture::NoError)stop();});});
        d->screenCapture->start();
    }else{
        const auto index=source.section(':',1).toInt();if(index<0 || index>=d->windows.size() || !d->windows[index].isValid()){d->fail("The selected window is no longer available.");return;}
        d->windowCapture=std::make_unique<QWindowCapture>();d->windowCapture->setWindow(d->windows[index]);
#if QT_VERSION >= QT_VERSION_CHECK(6,12,0)
        d->windowCapture->setMaximumFrameRate(d->fps);
#endif
        d->captureSession->setWindowCapture(d->windowCapture.get());
        connect(d->windowCapture.get(),&QWindowCapture::errorOccurred,this,[this,generation](QWindowCapture::Error,const QString &error){QTimer::singleShot(0,this,[this,generation,error]{if(d->active && d->captureGeneration==generation)d->fail("Window capture: "+error);});});
        connect(d->windowCapture.get(),&QWindowCapture::activeChanged,this,[this,generation](bool active){if(!active)QTimer::singleShot(0,this,[this,generation]{if(d->active && d->captureGeneration==generation && d->windowCapture && d->windowCapture->error()==QWindowCapture::NoError)stop();});});
        d->windowCapture->start();
    }
}
void BroadcastEngine::startExternal(const QString &guild,const QString &channel,QObject *source,int maximumHeight,int fps,bool streamAudio){
    if(d->active)return;
    auto *provider=source?qobject_cast<RelayBroadcastSourceInterface *>(source):nullptr;
    if(!available() || !validId(guild) || !validId(channel) || !provider){d->error="Join voice and connect the OBS input before streaming.";emit changed();return;}
    d->error.clear();d->active=true;d->requested=false;d->guild=guild;d->channel=channel;
    d->encoderTimestampNeedsRebase=false;d->haveVideoTimestamp=false;d->encoderTimestampOffset=0;d->lastVideoTimestamp=0;d->lastVideoAt=0;
    d->originalProgress.invalidate();d->originalKeyframeWait.invalidate();
    d->clearOriginalRate();d->sourceStats={};d->sourceStats.path=2;
    d->shareAudio=streamAudio;d->audioTimestamp=0;d->sourceName="OBS input";
    const auto generation=++d->captureGeneration;
    d->fps=fps==0?60:(fps==15 || fps==60)?fps:30;
    d->heightLimit=maximumHeight==0?2160:(maximumHeight==720 || maximumHeight==1440 || maximumHeight==2160)?maximumHeight:1080;
    d->bitrate=streamBitrate(d->heightLimit,d->fps);
    d->width=d->height=0;d->previousFrames=0;d->status="Starting OBS stream…";
    d->externalMode=true;d->external=source;d->externalInterface=provider;
    d->sourceQuality=maximumHeight==0 && fps==0;d->passthrough=false;d->waitingForIdr=true;
    d->encodedInterface=qobject_cast<RelayEncodedBroadcastSourceInterface *>(source);
    d->encoder=std::make_unique<BroadcastEncoder>(d->heightLimit,d->fps,d->bitrate);
    d->sourceDestroyed=connect(source,&QObject::destroyed,this,[this,generation]{if(d->active && d->captureGeneration==generation)stop();});
    provider->setBroadcastCaptureEnabled(true,d->encodedInterface && maximumHeight==0?0:d->heightLimit,d->encodedInterface && fps==0?0:d->fps,streamAudio);
    d->poll.setInterval(d->fps==60?10:20);d->poll.start();d->timeout.start(15000);emit changed();
}
void BroadcastEngine::stop(){
    if(d->stopping)return;
    const bool requested=d->requested;d->stopping=true;d->active=false;d->requested=false;++d->captureGeneration;
    d->stopCapture();d->transport.stop();d->status="Not sharing";d->sourceName.clear();d->width=d->height=0;d->stopping=false;
    emit changed();if(requested)emit stopRequested();
}
void BroadcastEngine::onStreamCreate(const QJsonObject &event){d->transport.onStreamCreate(event);}
void BroadcastEngine::onStreamServer(const QJsonObject &event){d->transport.onStreamServer(event);}
void BroadcastEngine::onStreamDelete(const QJsonObject &event){if(d->active && event.value("stream_key").toString()==d->transport.streamKey())stop();}

void BroadcastEngine::setDirectViewersActive(bool active){
    if(d->directViewers==active)return;
    d->directViewers=active;
    if(d->external && d->external->metaObject()->indexOfMethod("setRelayOriginalVideoEnabled(bool)")>=0)
        QMetaObject::invokeMethod(d->external,"setRelayOriginalVideoEnabled",Q_ARG(bool,active));
}
