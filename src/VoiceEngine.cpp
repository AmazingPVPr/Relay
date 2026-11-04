#include "VoiceEngine.h"
#include "VoiceLimiter.h"
#include "VoiceDiagnostics.h"
#include "VoicePacket.h"
#include "VideoPacket.h"
#include "StreamDecoder.h"
#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMediaDevices>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QPointer>
#include <QScopedValueRollback>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>
#include <QUrlQuery>
#include <QWebSocket>
#include <QtEndian>
#include <opus/opus.h>
#include <sodium.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#ifdef RELAY_HAS_DAVE
#include <dave/dave.h>
#endif

namespace {
constexpr int sampleRate=48000, channels=2, samplesPerFrame=960, pcmBytes=3840;
constexpr int captureCapacityFrames=12, maxAudioSteps=10;
constexpr char transportMode[]="aead_xchacha20_poly1305_rtpsize";
const unsigned char *bytes(const QByteArray &v){return reinterpret_cast<const unsigned char*>(v.constData());}
unsigned char *bytes(QByteArray &v){return reinterpret_cast<unsigned char*>(v.data());}
QAudioFormat format() { QAudioFormat f;f.setSampleRate(sampleRate);f.setChannelCount(channels);f.setSampleFormat(QAudioFormat::Int16);return f; }
bool validUserId(const QString &id) {
    if(id.isEmpty() || id.size()>22)return false;
    for(const auto c:id)if(c<u'0' || c>u'9')return false;
    return true;
}
QAudioDevice selectedDevice(const QString &id,bool input) {
    const auto devices=input?QMediaDevices::audioInputs():QMediaDevices::audioOutputs();
    for(const auto &device:devices)if(QString::fromLatin1(device.id().toBase64())==id)return device;
    return input?QMediaDevices::defaultAudioInput():QMediaDevices::defaultAudioOutput();
}
QVariantList deviceList(bool input) {
    QVariantList out;
    const auto def=input?QMediaDevices::defaultAudioInput():QMediaDevices::defaultAudioOutput();
    out.append(QVariantMap{{"id",""},{"name",QString("System default%1").arg(def.isNull()?QString():QString(" · ")+def.description())},{"isDefault",true}});
    for(const auto &dev:input?QMediaDevices::audioInputs():QMediaDevices::audioOutputs())
        out.append(QVariantMap{{"id",QString::fromLatin1(dev.id().toBase64())},{"name",dev.description()},{"isDefault",dev==def}});
    return out;
}
}
struct VoiceEngine::Private : public QObject {
    explicit Private(VoiceEngine *owner):QObject(nullptr),q(owner),media(this),ws(QString(),QWebSocketProtocol::VersionLatest,this),udp(this) {for(auto *timer:{&heartbeat,&deadline,&tick,&keepAlive,&diagnosticTimer,&micTestTick,&reconnectTimer})timer->setParent(this);cameraClock.start();}
    VoiceEngine *q; QMediaDevices media;QWebSocket ws;QUdpSocket udp;
    QTimer heartbeat,deadline,tick,keepAlive,diagnosticTimer,micTestTick,reconnectTimer;
    bool resuming=false,awaitingLeave=false,freshAllocationUsed=false;
    int resumeAttempts=0;
    QString resumeStage;
    QSet<QString> supersededChannels;
    QSet<QString> leavePendingGuilds,expiredLeaveGuilds;
    VoiceDiagnostics history;
    bool receivedSelfState=false,receivedServerUpdate=false;
    int lastSocketError=-1;
    quint64 socketGeneration=0,accountGeneration=0;
    QElapsedTimer heartbeatRoundTrip;
    qint64 heartbeatToken=0;
    double latency=-1,averageLatency=-1;
    QVariantList latencySamples;
    QString serverLabel;
    QString stage="idle";
    struct Counters {quint64 gateway=0,binary=0,udpReceived=0,mediaReceived=0,mediaSent=0,decoded=0,transportFailures=0,mediaFailures=0,unknownSsrc=0,commits=0;
        quint64 audioRtp=0,otherRtp=0,rtcp=0,deafDrops=0,outputBytes=0,sinkErrors=0,peakPeers=0;
        quint64 captureSamples=0,captureDropped=0,captureCatchups=0,audioLateWakes=0,outputUnderruns=0,outputDropped=0;
        quint64 captureErrors=0,encodeErrors=0,writeErrors=0;
        quint64 captureReadyWakes=0,micTestUnderruns=0,micTestWriteErrors=0;
        quint64 receiveConcealedPackets=0,receiveLatePackets=0,receiveRebases=0,receiveSkippedSlots=0,receiveDiscardedPackets=0,receiveDiscardedSamples=0;
        qint64 receiveMaxQueuedMs=0;
        qint64 maxCaptureReadyGapUs=0,maxCaptureWorkUs=0;
        qint64 maxAudioLateMs=0;int maxCaptureQueued=0;
        int lastOpcode=-1,closeCode=0;} counters;
    // Retain bounded device error enums after stop/reset; never retain device audio.
    int lastCaptureError=0,lastSinkError=0,lastMicTestSinkError=0;
    QUrl fixtureGateway;
    bool fixtureMode=false;
    QByteArray fixtureOutput;
    qint64 fixturePlaybackElapsed=-1;
    QPointer<QIODevice> fixtureAudioOutput;int fixtureOutputBufferBytes=0;
    QHash<QString,bool> speakingUsers;
    struct UserAudio { bool muted=false; double volume=1,soundboardVolume=1; };
    struct ReceivedSoundboard {QString user;QByteArray pcm;qsizetype offset=0;double sourceVolume=1;};
    QList<ReceivedSoundboard> receivedSoundboards;
    double soundboardVolume=1;quint64 soundboardPlaybackGeneration=1;
    QHash<QString,UserAudio> userAudio;
    quint32 keepAliveCounter=0;
    QString status="Not connected",error,guildId,channelId,channelName,userId,sessionId,token,endpoint;
    QString inputId,outputId;
    bool connected=false,muted=false,deafened=false,micTesting=false,closing=false,transportReady=false,encryptedReady=false,discovery=false,heartbeatPending=false,speaking=false;
    bool serverMuted=false,serverDeafened=false;
    QAudioFormat captureFormat;QByteArray rawCapture;
    QElapsedTimer audioClock;quint64 audioSteps=0;quint32 audioTimestampBase=0;qint64 lastAudioInputMs=-1,captureOpenedMs=-1,lastLocalSpeakingAudioMs=-1;
    bool captureActive=false,captureSeen=false;
    bool processingAudio=false;
    QElapsedTimer captureReadyClock;
    double inputLevel=0;
    AudioProcessor processor;
    bool voiceLimiterEnabled = false;
    double voiceLimiterCeiling = .5;
    int gatewaySequence=-1;quint32 ssrc=0,timestamp=0,nonce=0;quint16 sequence=0;
    QHostAddress serverAddress;quint16 serverPort=0;
    QByteArray key,capture,outputPending,soundboardPcm;
    std::unique_ptr<QAudioSource> source;std::unique_ptr<QAudioSink> sink;QIODevice *captureDevice=nullptr,*playbackDevice=nullptr;
    std::unique_ptr<QAudioSink> micTestSink;
    QIODevice *micTestPlayback=nullptr;
    QByteArray micTestPending;
    bool micTestPrimed=false;
    quint64 micTestFrames=0;
    OpusEncoder *encoder=nullptr;
    struct Peer {
        QString user;OpusDecoder *decoder=nullptr;std::map<quint16,QByteArray> queue;quint16 next=0;bool started=false;int misses=0;
        quint16 lastSequence=0;quint64 replayWindow=0;bool sequenceSeen=false;
        QByteArray pcm;
        int activityFrames=0;
        VoiceLimiter limiter;
        ~Peer(){if(decoder)opus_decoder_destroy(decoder);}
    };
    std::map<quint32,std::unique_ptr<Peer>> peers;
    std::set<QString> users;
    QElapsedTimer cameraClock;
    QString cameraUser;
    QHash<QString,QJsonObject> cameraAnnouncements;
    struct CameraSource {
        quint32 ssrc=0,rtx=0,base=0,cycles=0,received=0,lastExpected=0,lastReceived=0,lsr=0;
        quint16 next=0,highest=0;
        qint64 gapSince=0,lastNack=-1000,lastPli=-1000,lastReport=0,lastSr=0;
        bool started=false;
        QString codec;
        int rotation=0;bool mirrored=false;
        qsizetype queuedBytes=0;
        std::map<quint16,VideoPacket::Rtp> pending;
        std::unique_ptr<VideoPacket::Depacketizer> depacketizer;
    } camera;
#ifdef RELAY_HAS_DAVE
    DAVESessionHandle session=nullptr;DAVEEncryptorHandle encryptor=nullptr;
    std::map<QString,DAVEDecryptorHandle> decryptors;
    std::map<quint16,quint16> transitions;
    quint16 preparedVersion=0;
#endif
    void notify(){q->notifyGui(&VoiceEngine::changed);q->notifyGui(&VoiceEngine::diagnosticsChanged);}
    void setStage(const QString &value){if(stage!=value)history.record(VoiceDiagnostics::Event::StageChanged,VoiceDiagnostics::stage(value));stage=value;q->notifyGui(&VoiceEngine::diagnosticsChanged);}
    void markSpeaking(const QString &id,bool active){
        if(id.isEmpty() || speakingUsers.value(id,false)==active)return;
        speakingUsers[id]=active;q->notifyGui(&VoiceEngine::speakingChanged,id,active);
    }
    void setStatus(const QString &v){status=v;notify();}
    void setError(const QString &v){error=v;notify();}
    void send(int op,const QJsonObject &data){if(ws.state()==QAbstractSocket::ConnectedState)ws.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op",op},{"d",data}}).toJson(QJsonDocument::Compact)));}
    void sendBinary(int op,const QByteArray &payload){QByteArray out(1,char(op));out+=payload;ws.sendBinaryMessage(out);}
    void heartbeatNow(){
        if(heartbeatPending){recoverSocket("heartbeat_timeout");return;}
        heartbeatPending=true;heartbeatToken=std::max(heartbeatToken+1,QDateTime::currentMSecsSinceEpoch());heartbeatRoundTrip.start();
        send(3,{{"t",double(heartbeatToken)},{"seq_ack",gatewaySequence}});
    }
    void heartbeatAck(const QJsonValue &data){
        const auto token=data.isObject()?data.toObject().value("t"):data;
        if(!heartbeatPending || !heartbeatRoundTrip.isValid() || !token.isDouble() || token.toDouble()!=double(heartbeatToken))return;
        heartbeatPending=false;latency=double(heartbeatRoundTrip.nsecsElapsed())/1000000.0;heartbeatRoundTrip.invalidate();
        latencySamples.append(QVariantMap{{"ms",latency},{"timestamp",QDateTime::currentMSecsSinceEpoch()}});
        while(latencySamples.size()>60)latencySamples.removeFirst();
        double sum=0;for(const auto &sample:latencySamples)sum+=sample.toMap().value("ms").toDouble();
        averageLatency=sum/latencySamples.size();q->notifyGui(&VoiceEngine::statsChanged);
    }
    void stopCapture(){if(source){source->stop();source.reset();}captureDevice=nullptr;capture.clear();rawCapture.clear();lastAudioInputMs=captureOpenedMs=-1;captureActive=captureSeen=false;captureReadyClock.invalidate();processor.reset();inputLevel=0;q->notifyGui(&VoiceEngine::inputLevelChanged);}
    void stopPlayback(){
        if(sink){sink->stop();sink.reset();}playbackDevice=nullptr;outputPending.clear();receivedSoundboards.clear();++soundboardPlaybackGeneration;
        for(auto &[_,peer]:peers){peer->queue.clear();peer->pcm.clear();peer->started=false;peer->misses=0;peer->activityFrames=0;markSpeaking(peer->user,false);}
    }
    void stopMicPlayback(){
        micTestTick.stop();
        if(micTestSink){micTestSink->stop();micTestSink.reset();}
        micTestPlayback=nullptr;micTestPending.clear();micTestPrimed=false;
    }
    void finishMicTest(bool resumeAudio,bool requestState){
        if(!micTesting)return;
        micTesting=false;stopMicPlayback();stopCapture();stopPlayback();fixtureOutput.clear();
        if(resumeAudio){updateCapture();if(connected)startPlayback();}
        notify();
        if(requestState)requestCurrentState();
    }
    bool startMicPlayback(){
        if(fixtureMode){micTestTick.start(20);return true;}
        const auto dev=selectedDevice(outputId,false);
        if(dev.isNull() || !dev.isFormatSupported(format())){
            setError("Select an output that can play 48 kHz stereo audio for the microphone test.");return false;
        }
        micTestSink=std::make_unique<QAudioSink>(dev,format());micTestSink->setBufferSize(pcmBytes*4);
        const QPointer<QAudioSink> expected(micTestSink.get());
        QObject::connect(micTestSink.get(),&QAudioSink::stateChanged,this,[this,expected](QAudio::State state){
            if(state==QAudio::IdleState && expected && micTestSink.get()==expected.data() && micTesting && micTestFrames>0)++counters.micTestUnderruns;
            if(state==QAudio::StoppedState && expected && expected->error()!=QAudio::NoError){
                lastMicTestSinkError=int(expected->error());++counters.sinkErrors;
                QTimer::singleShot(0,this,[this,expected]{
                    if(expected && micTestSink.get()==expected.data())audioError("Microphone playback stopped. Check the output device and try the test again.");
                });
            }
        });
        micTestPlayback=micTestSink->start();
        if(!micTestPlayback){lastMicTestSinkError=int(micTestSink->error());++counters.sinkErrors;stopMicPlayback();setError("Could not open the output for the microphone test.");return false;}
        micTestTick.start(20);return true;
    }
    void acceptCapture(const QByteArray &data){
        if(!micTesting && !(encryptedReady && !muted && !deafened && !serverMuted && !serverDeafened))return;
        if(!data.isEmpty()){
            captureSeen=true;
            if(captureReadyClock.isValid())counters.maxCaptureReadyGapUs=std::max(counters.maxCaptureReadyGapUs,captureReadyClock.nsecsElapsed()/1000);
            captureReadyClock.start();
        }
        counters.captureSamples+=data.size()/(channels*2);
        capture+=data;
        if(capture.size()>pcmBytes*captureCapacityFrames){
            const auto discarded=capture.size()/pcmBytes-maxAudioSteps;
            capture.remove(0,discarded*pcmBytes);counters.captureDropped+=discarded;
            // Retained samples keep their original source clock. A real loss
            // is an RTP gap; a late callback followed by catch-up is not.
            if(!micTesting)timestamp+=quint32(discarded*samplesPerFrame);
        }
        counters.maxCaptureQueued=std::max(counters.maxCaptureQueued,int(capture.size()/pcmBytes));
        // Capture and Opus use different block sizes. Dispatch complete samples
        // when they arrive instead of quantizing native callbacks to a second
        // 20 ms timer, which manufactured alternating packet bursts and gaps.
        if(capture.size()>=pcmBytes && !processingAudio){
            QElapsedTimer work;work.start();++counters.captureReadyWakes;
            if(micTesting)processMicTest();else if(connected)processAudio();
            counters.maxCaptureWorkUs=std::max(counters.maxCaptureWorkUs,work.nsecsElapsed()/1000);
        }
    }
    void processMicTest(){
        if(!micTesting || processingAudio)return;
        QScopedValueRollback<bool> processing(processingAudio,true);
        const auto steps=std::min<qsizetype>(maxAudioSteps,capture.size()/pcmBytes);
        for(qsizetype step=0;step<steps;++step){
            std::array<qint16,samplesPerFrame*channels> samples;
            std::memcpy(samples.data(),capture.constData(),pcmBytes);capture.remove(0,pcmBytes);
            const int delay=micTestSink?int(format().durationForBytes(micTestSink->bufferSize()-micTestSink->bytesFree())/1000):0;
            processor.capture(samples.data(),delay+40);inputLevel=processor.peak();
            processor.monitor(samples.data());++micTestFrames;
            micTestPending.append(reinterpret_cast<const char*>(samples.data()),pcmBytes);
        }
        if(steps)q->notifyGui(&VoiceEngine::inputLevelChanged);
        if(micTestPending.size()>pcmBytes*captureCapacityFrames)micTestPending.remove(0,micTestPending.size()-pcmBytes*maxAudioSteps);
        if(fixtureMode){fixtureOutput+=micTestPending;micTestPending.clear();if(fixtureOutput.size()>pcmBytes*50)fixtureOutput.remove(0,fixtureOutput.size()-pcmBytes*50);return;}
        // Native PipeWire starts consuming immediately. Buffer capacity alone
        // is not a prefill: collect 60 ms before the first write so independent
        // capture/output periods do not insert holes into the local test.
        if(!micTestPrimed && micTestPending.size()<pcmBytes*3)return;
        if(micTestPlayback && micTestSink && !micTestPending.isEmpty()){
            const auto written=micTestPlayback->write(micTestPending.constData(),std::min<qint64>(micTestSink->bytesFree(),micTestPending.size()));
            if(written>0){micTestPrimed=true;micTestPending.remove(0,written);}
            else if(written<0)++counters.micTestWriteErrors;
        }
    }
    void reset(){
        clearCamera(true);cameraAnnouncements.clear();
        closing=true;++socketGeneration;reconnectTimer.stop();resuming=false;resumeAttempts=0;heartbeat.stop();deadline.stop();tick.stop();keepAlive.stop();diagnosticTimer.stop();keepAliveCounter=0;stopMicPlayback();stopCapture();stopPlayback();ws.abort();udp.close();
        if(encoder){opus_encoder_destroy(encoder);encoder=nullptr;}
        peers.clear();users.clear();
        const auto speakingIds=speakingUsers.keys();for(const auto &id:speakingIds)markSpeaking(id,false);speakingUsers.clear();
        fixtureOutput.clear();soundboardPcm.clear();
#ifdef RELAY_HAS_DAVE
        for(auto &[_,decoder]:decryptors){daveDecryptorDestroy(decoder);}
        decryptors.clear();
        if(encryptor){daveEncryptorDestroy(encryptor);}
        encryptor=nullptr;
        if(session){daveSessionDestroy(session);}
        session=nullptr;transitions.clear();preparedVersion=0;
#endif
        if(!key.isEmpty()){sodium_memzero(key.data(),key.size());}
        key.clear();
        if(!token.isEmpty()){token.fill(QChar(0));}
        token.clear();sessionId.clear();endpoint.clear();
        connected=transportReady=encryptedReady=discovery=heartbeatPending=speaking=micTesting=false;audioClock.invalidate();audioSteps=0;fixturePlaybackElapsed=-1;lastLocalSpeakingAudioMs=-1;
        heartbeatRoundTrip.invalidate();latency=averageLatency=-1;latencySamples.clear();serverLabel.clear();q->notifyGui(&VoiceEngine::statsChanged);
        serverMuted=serverDeafened=false;gatewaySequence=-1;closing=false;
    }
    void recordFailure(VoiceDiagnostics::Failure reason){
        VoiceDiagnostics::Counts c;
        c.gateway=counters.gateway;c.binary=counters.binary;c.udp=counters.udpReceived;c.sent=counters.mediaSent;c.received=counters.mediaReceived;c.decoded=counters.decoded;
        c.transportErrors=counters.transportFailures;c.mediaErrors=counters.mediaFailures;c.opcode=counters.lastOpcode;c.closeCode=counters.closeCode;c.socketError=lastSocketError;
        c.selfState=receivedSelfState;c.serverUpdate=receivedServerUpdate;c.heartbeatPending=heartbeatPending;c.transportReady=transportReady;c.encryptedReady=encryptedReady;
        history.fail(reason,VoiceDiagnostics::stage(stage),c);
    }
    void fail(const QString &reason,bool requestLeave=true,VoiceDiagnostics::Failure kind=VoiceDiagnostics::Failure::Protocol){
        recordFailure(kind);
        const auto guild=guildId;reset();awaitingLeave=false;guildId.clear();channelId.clear();channelName.clear();
        error=reason;status="Voice disconnected";setStage("failed");notify();
        if(requestLeave && !guild.isEmpty() && !leavePendingGuilds.contains(guild)){leavePendingGuilds.insert(guild);requestVoiceState(guild,QString(),muted,deafened);}
    }
    void requestVoiceState(const QString &guild,const QString &channel,bool mute,bool deaf){
        const auto generation=accountGeneration;
        QMetaObject::invokeMethod(q,[owner=q,generation,guild,channel,mute,deaf]{
            if(owner->onRuntime([&]{return owner->d->accountGeneration==generation;}))
                emit owner->voiceStateRequested(guild,channel,mute,deaf);
        },Qt::QueuedConnection);
    }
    void requestCurrentState(){
        if(!awaitingLeave && !channelId.isEmpty())requestVoiceState(guildId,channelId,muted||deafened||micTesting,deafened||micTesting);
    }
    void requestJoin(){
        awaitingLeave=false;setStage("awaiting_gateway_state");setStatus("Requesting voice channel…");deadline.start(10000);
        requestCurrentState();
    }
    void refreshAllocation(){
        if(channelId.isEmpty())return;
        if(freshAllocationUsed){fail("Voice could not reconnect. Check your connection and try joining again.");return;}
        history.record(VoiceDiagnostics::Event::AllocationRefresh,VoiceDiagnostics::stage(stage));
        freshAllocationUsed=true;reset();receivedSelfState=receivedServerUpdate=false;diagnosticTimer.start();awaitingLeave=true;leavePendingGuilds.insert(guildId);
        setStage("awaiting_leave");setStatus("Reconnecting to voice…");deadline.start(10000);
        requestVoiceState(guildId,QString(),muted||deafened,deafened);
    }
    void connectionDeadline(){
        history.record(VoiceDiagnostics::Event::Deadline,VoiceDiagnostics::stage(stage));
        if(resuming){recoverSocket("resume_timeout");return;}
        auto kind=VoiceDiagnostics::Failure::EncryptionTimeout;
        if(stage=="awaiting_gateway_state" || stage=="awaiting_voice_server")kind=VoiceDiagnostics::Failure::AwaitingStateTimeout;
        else if(stage=="awaiting_leave")kind=VoiceDiagnostics::Failure::LeaveAckTimeout;
        else if(stage=="udp_discovery")kind=VoiceDiagnostics::Failure::UdpDiscoveryTimeout;
        else if(stage=="session_description")kind=VoiceDiagnostics::Failure::SessionDescriptionTimeout;
        else if(stage=="websocket")kind=VoiceDiagnostics::Failure::SocketConnect;
        if(stage=="awaiting_gateway_state" && !freshAllocationUsed){recordFailure(kind);refreshAllocation();return;}
        const bool expiredLeave=stage=="awaiting_leave";
        if(expiredLeave){leavePendingGuilds.remove(guildId);expiredLeaveGuilds.insert(guildId);}
        fail("Voice connection timed out. Check permissions and the network, then rejoin.",!expiredLeave,kind);
    }
    void recoverSocket(const char *reason,int code=0){
        if(closing || channelId.isEmpty() || awaitingLeave)return;
        if(code==4014 || code==4021 || code==4022){fail(QString("Voice connection closed (%1). Select the channel to join again.").arg(code),false,VoiceDiagnostics::Failure::SocketClosed);return;}
        if(reconnectTimer.isActive())return;
        const auto kind=qstrcmp(reason,"heartbeat_timeout")==0?VoiceDiagnostics::Failure::HeartbeatTimeout:
            qstrcmp(reason,"resume_timeout")==0?VoiceDiagnostics::Failure::ResumeTimeout:
            !transportReady?VoiceDiagnostics::Failure::SocketConnect:VoiceDiagnostics::Failure::SocketClosed;
        recordFailure(kind);
        if(code==4006 || code==4009){refreshAllocation();return;}
        if(!transportReady || sessionId.isEmpty() || token.isEmpty() || endpoint.isEmpty()){
            if(!freshAllocationUsed)refreshAllocation();else fail("Could not connect to Discord's voice server. Check your network and rejoin.");
            return;
        }
        if(resumeAttempts>=3){refreshAllocation();return;}
        if(!resuming)resumeStage=stage;
        resuming=true;heartbeat.stop();heartbeatPending=false;heartbeatRoundTrip.invalidate();deadline.stop();
        closing=true;++socketGeneration;ws.abort();closing=false;
        history.record(VoiceDiagnostics::Event::RecoveryScheduled,VoiceDiagnostics::stage(stage),code);
        setStage("reconnecting_websocket");setStatus("Reconnecting voice connection…");
        reconnectTimer.start(500*(1<<resumeAttempts));++resumeAttempts;
    }
    void audioError(const QString &reason){
        if(micTesting)finishMicTest(true,true);else stopCapture();
        setError(reason);
    }
    bool startCapture(){
        if(captureActive)return true;
        if(fixtureMode){captureActive=true;captureOpenedMs=audioClock.isValid()?audioClock.elapsed():0;return true;}
        if(source)return true;
        const auto dev=selectedDevice(inputId,true);
        if(dev.isNull()){setError("No microphone is available. Connect a device or select another input.");return false;}
        captureFormat=format();
        if(!dev.isFormatSupported(captureFormat))captureFormat.setChannelCount(1);
        if(!dev.isFormatSupported(captureFormat))captureFormat.setSampleFormat(QAudioFormat::Float);
        if(!dev.isFormatSupported(captureFormat)){setError("This input cannot provide 48 kHz PCM. Select a PipeWire/PulseAudio input with audio conversion enabled.");return false;}
        source=std::make_unique<QAudioSource>(dev,captureFormat);source->setBufferSize(captureFormat.bytesForDuration(240000));
        const QPointer<QAudioSource> expectedSource(source.get());
        QObject::connect(source.get(),&QAudioSource::stateChanged,this,[this,expectedSource](QAudio::State state){
            if(state==QAudio::StoppedState && expectedSource && source.get()==expectedSource.data() && source->error()!=QAudio::NoError){
                lastCaptureError=int(source->error());++counters.captureErrors;
                QTimer::singleShot(0,this,[this,expectedSource]{
                    if(expectedSource && source.get()==expectedSource.data())audioError("The microphone stopped. Check the system audio device and retry.");
                });
            }
        });
        captureDevice=source->start();
        if(!captureDevice){lastCaptureError=int(source->error());++counters.captureErrors;source.reset();setError("Could not open the selected microphone.");return false;}
        captureActive=true;captureOpenedMs=audioClock.isValid()?audioClock.elapsed():0;
        QObject::connect(captureDevice,&QIODevice::readyRead,this,[this]{readCapture();});return true;
    }
    void readCapture(){
            if(!captureDevice)return;
            rawCapture+=captureDevice->readAll();
            const int frameBytes=captureFormat.bytesPerFrame();
            const qsizetype frameCount=rawCapture.size()/frameBytes;
            if(!frameCount)return;
            QByteArray data(frameCount*4,Qt::Uninitialized);double peak=0;
            for(qsizetype i=0;i<frameCount;i++)for(int channel=0;channel<2;channel++){
                const auto *sample=rawCapture.constData()+i*frameBytes+std::min(channel,captureFormat.channelCount()-1)*captureFormat.bytesPerSample();
                qint16 value=0;
                if(captureFormat.sampleFormat()==QAudioFormat::Int16)std::memcpy(&value,sample,2);
                else{float f;std::memcpy(&f,sample,4);value=std::isfinite(f)?qint16(std::clamp(double(f)*32768.0,-32768.0,32767.0)):0;}
                std::memcpy(data.data()+i*4+channel*2,&value,2);peak=std::max(peak,std::abs(double(value))/32768.0);
            }
            rawCapture.remove(0,frameCount*frameBytes);
            inputLevel=peak;q->notifyGui(&VoiceEngine::inputLevelChanged);
            acceptCapture(data);
    }
    void startPlayback(){
        if(fixtureMode)return;
        if(sink || micTesting || deafened || serverDeafened)return;
        const auto dev=selectedDevice(outputId,false);
        if(dev.isNull()){setError("No audio output is available. Connect a device or select another output.");return;}
        if(!dev.isFormatSupported(format())){setError("This output cannot play 48 kHz stereo PCM. Select a PipeWire/PulseAudio output.");return;}
        sink=std::make_unique<QAudioSink>(dev,format());sink->setBufferSize(pcmBytes*4);
        const QPointer<QAudioSink> expectedSink(sink.get());
        QObject::connect(sink.get(),&QAudioSink::stateChanged,this,[this,expectedSink](QAudio::State state){
            if(state==QAudio::IdleState && expectedSink && sink.get()==expectedSink.data() &&
                (!outputPending.isEmpty() || std::any_of(peers.begin(),peers.end(),[](const auto &peer){return peer.second->activityFrames>0;})))++counters.outputUnderruns;
            if(state==QAudio::StoppedState && expectedSink && sink.get()==expectedSink.data() && expectedSink->error()!=QAudio::NoError){
                lastSinkError=int(expectedSink->error());
                QTimer::singleShot(0,this,[this,expectedSink]{
                    if(!expectedSink || sink.get()!=expectedSink.data())return;
                    ++counters.sinkErrors;stopPlayback();setError("Audio playback stopped. Check the system output device and select it again.");
                });
            }
        });
        playbackDevice=sink->start();
        if(!playbackDevice){lastSinkError=int(sink->error());++counters.sinkErrors;sink.reset();setError("Could not open the selected audio output.");}
    }
    void updateCapture(){if(micTesting || (connected && encryptedReady && !muted && !deafened && !serverMuted && !serverDeafened))startCapture();else stopCapture();}
    void updateSpeaking(bool active){
        lastLocalSpeakingAudioMs=active && audioClock.isValid()?audioClock.elapsed():-1;
        if(speaking==active)return;
        speaking=active;markSpeaking(userId,active);send(5,{{"speaking",active?1:0},{"delay",0},{"ssrc",double(ssrc)}});
    }
    void openGateway(){
        if(reconnectTimer.isActive() || sessionId.isEmpty() || token.isEmpty() || endpoint.isEmpty() || ws.state()!=QAbstractSocket::UnconnectedState)return;
        QUrl url(endpoint.startsWith("wss://")?endpoint:"wss://"+endpoint);
        const auto host=url.host().toLower();
        const bool trustedHost=host.endsWith(".discord.media") || host.endsWith(".discord.gg");
        if(url.scheme()!="wss" || !trustedHost || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()){
            fail("Discord supplied an invalid voice endpoint.");return;
        }
        serverLabel=host;q->notifyGui(&VoiceEngine::statsChanged);
        history.record(VoiceDiagnostics::Event::WebSocketOpening,VoiceDiagnostics::stage(stage));++socketGeneration;lastSocketError=-1;
        url.setPath("/");url.setQuery("v=8");if(fixtureMode)url=fixtureGateway;if(!resuming){setStage("websocket");setStatus("Connecting to voice…");}ws.open(url);deadline.start(resuming?10000:30000);
    }
    void addUser(const QString &id){
        if(id.isEmpty() || id.size()>22 || id==userId || users.size()>=512)return;
        bool validId=false;id.toULongLong(&validId);if(!validId)return;
        const bool isNewUser=users.insert(id).second;
        if(!isNewUser)return;
        if(connected && !encryptedReady){
            setStage("mls_exchange");setStatus("Connected · establishing encryption…");
            if(!deadline.isActive())deadline.start(30000);
        }
#ifdef RELAY_HAS_DAVE
        if(!decryptors.contains(id))decryptors[id]=daveDecryptorCreate();
        if(session && preparedVersion>0){auto ratchet=daveSessionGetKeyRatchet(session,id.toUtf8().constData());if(ratchet){daveDecryptorTransitionToKeyRatchet(decryptors[id],ratchet);daveKeyRatchetDestroy(ratchet);}}
#endif
    }
#ifdef RELAY_HAS_DAVE
    void initDave(quint16 version){
        if(version==0){
            encryptedReady=false;preparedVersion=0;deadline.stop();stopCapture();
            setStage("waiting_for_participants");setStatus("Connected · waiting for encrypted participants");return;
        }
        if(version>daveMaxSupportedProtocolVersion()){fail("The voice server did not negotiate a supported end-to-end encryption version.");return;}
        encryptedReady=false;updateCapture();
        setStage(users.empty()?"waiting_for_participants":"mls_exchange");
        setStatus(users.empty()?"Connected · waiting for someone to join":"Connected · establishing encryption…");
        if(!users.empty())deadline.start(30000);
        if(!session){session=daveSessionCreate(nullptr,nullptr,[](const char*,const char*,void *context){
            // Library diagnostics can contain peer metadata; expose only a bounded, general status.
            auto self=static_cast<Private*>(context);self->error="Voice encryption could not process a group update; requesting fresh keys.";self->notify();
        },this);}
        if(!session){fail("Could not create Discord's encryption session.");return;}
        daveSessionInit(session,version,channelId.toULongLong(),userId.toUtf8().constData());
        if(!encryptor)encryptor=daveEncryptorCreate();
        if(!encryptor){fail("Could not initialize audio encryption.");return;}
        daveEncryptorSetPassthroughMode(encryptor,false);daveEncryptorAssignSsrcToCodec(encryptor,ssrc,DAVE_CODEC_OPUS);
        sendKeyPackage();
    }
    void sendKeyPackage(){
        uint8_t *data=nullptr;size_t length=0;daveSessionGetMarshalledKeyPackage(session,&data,&length);
        if(data && length){sendBinary(26,QByteArray(reinterpret_cast<const char*>(data),length));}
        daveFree(data);
    }
    std::vector<QByteArray> recognizedBytes() const {
        std::vector<QByteArray> ids;ids.push_back(userId.toUtf8());for(const auto &id:users)ids.push_back(id.toUtf8());return ids;
    }
    void prepareTransition(quint16 id,quint16 version){
        // Never send or play unencrypted participant media, including during a downgrade.
        if(version==0){transitions[id]=0;if(id==0)executeTransition(id);else send(23,{{"transition_id",int(id)}});return;}
        for(const auto &user:users){
            addUser(user);auto ratchet=daveSessionGetKeyRatchet(session,user.toUtf8().constData());
            if(ratchet){daveDecryptorTransitionToKeyRatchet(decryptors[user],ratchet);daveKeyRatchetDestroy(ratchet);}
        }
        preparedVersion=version;transitions[id]=version;
        if(id==0)executeTransition(id);else send(23,{{"transition_id",int(id)}});
    }
    void executeTransition(quint16 id){
        auto it=transitions.find(id);if(it==transitions.end())return;
        auto version=it->second;transitions.erase(it);
        if(version==0){encryptedReady=false;preparedVersion=0;deadline.stop();daveSessionReset(session);stopCapture();updateSpeaking(false);setStage("waiting_for_participants");setStatus("Connected · waiting for encrypted participants");return;}
        auto ratchet=daveSessionGetKeyRatchet(session,userId.toUtf8().constData());
        if(!ratchet){encryptedReady=false;updateCapture();setStatus("Connected · waiting for encryption keys");return;}
        daveEncryptorSetKeyRatchet(encryptor,ratchet);daveKeyRatchetDestroy(ratchet);encryptedReady=true;error.clear();history.record(VoiceDiagnostics::Event::EncryptionReady,VoiceDiagnostics::stage(stage));
        setStage("encrypted");setStatus("Connected · end-to-end encrypted");deadline.stop();updateCapture();startPlayback();
        cameraKeyframe();
    }
    void binary(const QByteArray &message){
        if(message.size()<3 || !session)return;
        ++counters.binary;counters.lastOpcode=quint8(message[2]);
        gatewaySequence=qFromBigEndian<quint16>(message.constData());
        const int op=quint8(message[2]);QByteArray payload=message.mid(3);
        if(op==25){daveSessionSetExternalSender(session,bytes(payload),payload.size());return;}
        auto ids=recognizedBytes();std::vector<const char*> ptrs;for(const auto &id:ids)ptrs.push_back(id.constData());
        if(op==27){uint8_t *data=nullptr;size_t length=0;daveSessionProcessProposals(session,bytes(payload),payload.size(),ptrs.data(),ptrs.size(),&data,&length);if(data&&length)sendBinary(28,QByteArray(reinterpret_cast<char*>(data),length));daveFree(data);return;}
        if((op!=29 && op!=30)||payload.size()<2)return;
        const auto transition=qFromBigEndian<quint16>(payload.constData());payload.remove(0,2);bool success=false,ignored=false;
        if(op==29){auto result=daveSessionProcessCommit(session,bytes(payload),payload.size());if(result){ignored=daveCommitResultIsIgnored(result);success=!daveCommitResultIsFailed(result)&&!ignored;daveCommitResultDestroy(result);}}
        else{auto result=daveSessionProcessWelcome(session,bytes(payload),payload.size(),ptrs.data(),ptrs.size());success=result!=nullptr;if(result)daveWelcomeResultDestroy(result);}
        if(ignored)return;
        if(success){++counters.commits;prepareTransition(transition,daveSessionGetProtocolVersion(session));}
        else {send(31,{{"transition_id",int(transition)}});encryptedReady=false;updateCapture();initDave(daveSessionGetProtocolVersion(session));}
    }
#endif
    void text(const QString &message){
        QJsonParseError parse;const auto doc=QJsonDocument::fromJson(message.toUtf8(),&parse);
        if(parse.error!=QJsonParseError::NoError || !doc.isObject())return;
        const auto root=doc.object();if(root.contains("seq"))gatewaySequence=root.value("seq").toInt();
        const int op=root.value("op").toInt(-1);const auto data=root.value("d").toObject();
        ++counters.gateway;counters.lastOpcode=op;
        switch(op){
        case 8: {const int interval=data.value("heartbeat_interval").toInt();if(interval<1000){fail("Invalid voice heartbeat interval.");return;}heartbeat.start(std::min(interval,5000));heartbeatNow();break;}
        case 6: heartbeatAck(root.value("d"));break;
        case 9: {
            if(!resuming || !transportReady)break;
            history.record(VoiceDiagnostics::Event::Resumed,VoiceDiagnostics::stage(stage));
            resuming=false;resumeAttempts=0;freshAllocationUsed=false;reconnectTimer.stop();deadline.stop();error.clear();
            setStage(resumeStage);setStatus(encryptedReady?"Connected · end-to-end encrypted":"Connected · establishing encryption…");
            send(5,{{"speaking",speaking?1:0},{"delay",0},{"ssrc",double(ssrc)}});sendCameraWants();
            if(!encryptedReady && resumeStage=="mls_exchange")deadline.start(30000);
            break;
        }
        case 2: {
            ssrc=quint32(data.value("ssrc").toDouble());serverAddress=QHostAddress(data.value("ip").toString());serverPort=quint16(data.value("port").toInt());
            if(serverAddress.isNull() || !serverPort || !data.value("modes").toArray().contains(QString::fromLatin1(transportMode))){fail("The voice server does not offer a supported audio transport.");return;}
            udp.close();if(!udp.bind(serverAddress.protocol()==QAbstractSocket::IPv6Protocol?QHostAddress::AnyIPv6:QHostAddress::AnyIPv4,0)){fail("Could not open the voice UDP socket.");return;}
            udp.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,4*1024*1024);
            keepAlive.start(5000);discovery=true;udp.writeDatagram(VoicePacket::discoveryRequest(ssrc),serverAddress,serverPort);setStage("udp_discovery");setStatus("Discovering audio connection…");deadline.start(15000);break;
        }
        case 4: {
            if(transportReady){fail("Voice session keys changed. Rejoin to establish fresh encryption.");return;}
            if(data.value("mode").toString()!=transportMode){fail("Discord selected an unsupported transport cipher.");return;}
            const auto array=data.value("secret_key").toArray();if(array.size()!=32){fail("Discord returned an invalid voice transport key.");return;}
            key.resize(32);for(int i=0;i<32;i++)key[i]=char(array[i].toInt());
            int result=0;encoder=opus_encoder_create(sampleRate,channels,OPUS_APPLICATION_VOIP,&result);
            if(result!=OPUS_OK || !encoder){fail("Could not initialize the Opus audio encoder.");return;}
            opus_encoder_ctl(encoder,OPUS_SET_BITRATE(64000));opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(5));opus_encoder_ctl(encoder,OPUS_SET_INBAND_FEC(1));opus_encoder_ctl(encoder,OPUS_SET_PACKET_LOSS_PERC(5));
            transportReady=connected=true;freshAllocationUsed=false;resumeAttempts=0;history.record(VoiceDiagnostics::Event::TransportReady,VoiceDiagnostics::stage(stage));
            // Register the assigned audio SSRC even for a silent or muted join.
            // A Video announcement is not a substitute for the required first
            // Speaking payload, which must precede all outgoing audio RTP.
            send(5,{{"speaking",0},{"delay",0},{"ssrc",double(ssrc)}});
            setStatus("Connected · establishing encryption…");deadline.stop();audioClock.start();audioSteps=0;audioTimestampBase=timestamp;lastAudioInputMs=-1;tick.start(20);
            send(12,{{"audio_ssrc",double(ssrc)},{"video_ssrc",0},{"rtx_ssrc",0},{"streams",QJsonArray{}}});
            sendCameraWants();
#ifdef RELAY_HAS_DAVE
            initDave(quint16(data.value("dave_protocol_version").toInt()));
#endif
            break;
        }
        case 5: {
            const auto id=data.value("user_id").toString();
            mapPeer(id,quint32(data.value("ssrc").toDouble()));
            // The gateway flag can stay set while a sender transmits silence.
            // Only decoded audio turns the participant's activity ring on.
            if((data.value("speaking").toInt()&1)==0){
                for(auto &[_,peer]:peers)if(peer->user==id)peer->activityFrames=0;
                markSpeaking(id,false);
            }
            break;
        }
        case 11: for(const auto &id:data.value("user_ids").toArray())addUser(id.toString());break;
        case 12: mapPeer(data.value("user_id").toString(),quint32(data.value("audio_ssrc").toDouble()));mapCamera(data);break;
        case 13: {const auto id=data.value("user_id").toString();markSpeaking(id,false);speakingUsers.remove(id);users.erase(id);std::erase_if(peers,[&id](const auto &entry){return entry.second->user==id;});
            cameraAnnouncements.remove(id);if(cameraUser==id)clearCamera(true);
#ifdef RELAY_HAS_DAVE
            auto it=decryptors.find(id);if(it!=decryptors.end()){daveDecryptorDestroy(it->second);decryptors.erase(it);}
#endif
            break;}
#ifdef RELAY_HAS_DAVE
        case 21: if(session)prepareTransition(quint16(data.value("transition_id").toInt()),quint16(data.value("protocol_version").toInt()));break;
        case 22: if(session)executeTransition(quint16(data.value("transition_id").toInt()));break;
        case 24: if(data.value("epoch").toVariant().toULongLong()==1)initDave(quint16(data.value("protocol_version").toInt()));else if(session)daveSessionSetProtocolVersion(session,quint16(data.value("protocol_version").toInt()));break;
#endif
        default:break;
        }
    }
    void mapPeer(const QString &id,quint32 peerSsrc){
        if(id==userId || id.isEmpty() || peerSsrc==0)return;
        addUser(id);if(!users.contains(id))return;
        auto existing=peers.find(peerSsrc);
        if(existing!=peers.end() && existing->second->user!=id)peers.erase(existing);
        // Replaced microphone SSRCs must not retain an old Opus state or replay window.
        std::erase_if(peers,[&](const auto &entry){return entry.first!=peerSsrc && entry.second->user==id;});
        if(!peers.contains(peerSsrc) && peers.size()<512){
            auto p=std::make_unique<Peer>();p->user=id;int result=0;
            p->decoder=opus_decoder_create(sampleRate,channels,&result);
            if(result==OPUS_OK){peers[peerSsrc]=std::move(p);counters.peakPeers=std::max(counters.peakPeers,quint64(peers.size()));}
        }
    }
    QJsonArray mediaCodecs() const {
        QJsonArray list{QJsonObject{{"name","opus"},{"type","audio"},{"priority",1000},{"payload_type",120},{"encode",true},{"decode",true}}};
        for(const auto &video:VideoPacket::VideoCodecs) if(StreamDecoder::supports(video.name))
            list.append(QJsonObject{{"name",video.name},{"type","video"},{"priority",video.priority},
                {"payload_type",video.payloadType},{"rtx_payload_type",video.repairType},{"encode",false},{"decode",true}});
        return list;
    }
    void clearCamera(bool stop){
        camera=CameraSource{};
        if(stop)cameraUser.clear();
        if(transportReady)sendCameraWants();
        q->notifyGui(&VoiceEngine::cameraDiscontinuity);q->notifyGui(&VoiceEngine::cameraChanged);
    }
    void sendCameraWants(){
        // Keep ordinary voice subscribed, including newly announced microphone
        // SSRCs. Only known camera layers are suppressed until explicitly watched.
        QJsonObject wants{{"any",100}};
        for(const auto &announcement:cameraAnnouncements){
            for(const auto &item:announcement.value("streams").toArray()){
                const auto source=quint32(item.toObject().value("ssrc").toDouble());
                if(source)wants.insert(QString::number(source),source==camera.ssrc?100:0);
            }
        }
        if(camera.ssrc)wants.insert(QString::number(camera.ssrc),100);
        send(15,wants);
    }
    void selectCamera(){
        if(cameraUser.isEmpty())return;
        const auto announcement=cameraAnnouncements.value(cameraUser);
        if(announcement.isEmpty())return;
        QJsonObject best;int quality=-1,bestFps=-1; qint64 area=-1;
        for(const auto &entry:announcement.value("streams").toArray()){
            const auto layer=entry.toObject();
            if(layer.value("type").toString("video")!="video" || !layer.value("active").toBool(true) || !layer.value("ssrc").toDouble())continue;
            const auto resolution=layer.value("max_resolution").toObject();
            const qint64 pixels=resolution.value("type").toString()=="source"?std::numeric_limits<qint64>::max()
                :qint64(std::clamp(resolution.value("width").toInt(),0,16384))*std::clamp(resolution.value("height").toInt(),0,16384);
            const int rank=layer.value("quality").toInt(100);
            const int rate=layer.value("max_framerate").toInt();
            if(rank>quality || (rank==quality && (pixels>area || (pixels==area && rate>bestFps)))){best=layer;quality=rank;area=pixels;bestFps=rate;}
        }
        quint32 selected=quint32(best.value("ssrc").toDouble());
        if(!selected && !announcement.contains("streams"))selected=quint32(announcement.value("video_ssrc").toDouble());
        if(!selected){clearCamera(true);return;}
        const auto rtx=quint32(best.value("rtx_ssrc").toDouble(announcement.value("rtx_ssrc").toDouble()));
        if(camera.ssrc!=selected || camera.rtx!=rtx){
            camera=CameraSource{};camera.ssrc=selected;camera.rtx=rtx;
            q->notifyGui(&VoiceEngine::cameraDiscontinuity);q->notifyGui(&VoiceEngine::cameraChanged);
        }
        sendCameraWants();cameraKeyframe();
    }
    void mapCamera(const QJsonObject &announcement){
        const auto id=announcement.value("user_id").toString();
        if(!validUserId(id) || id==userId || (!cameraAnnouncements.contains(id) && cameraAnnouncements.size()>=512))return;
        if(!announcement.contains("streams") && !announcement.contains("video_ssrc"))return;
        QJsonObject metadata{{"user_id",id},{"video_ssrc",announcement.value("video_ssrc")},{"rtx_ssrc",announcement.value("rtx_ssrc")}};
        if(announcement.contains("streams")){
            QJsonArray layers;const auto incoming=announcement.value("streams").toArray();
            for(qsizetype i=0;i<std::min<qsizetype>(incoming.size(),16);++i){
                const auto item=incoming[i].toObject();QJsonObject layer;
                for(const auto name:{"type","ssrc","rtx_ssrc","quality","active","max_framerate","max_resolution"})layer.insert(name,item.value(name));
                layers.append(layer);
            }metadata.insert("streams",layers);
        }
        addUser(id);cameraAnnouncements[id]=metadata;
        if(cameraUser==id)selectCamera();else sendCameraWants();
    }
    void cameraRtcp(const QByteArray &plain){
        if(!transportReady || !encryptedReady || key.size()!=32)return;
        if(nonce==std::numeric_limits<quint32>::max()){fail("Voice transport key expired. Rejoin the channel.");return;}
        // VoicePacket uses a big-endian opaque nonce suffix. Share its counter
        // and byte representation so feedback can never reuse an audio nonce.
        const auto wire=VideoPacket::encryptRtcp(plain,key,qbswap(nonce++));
        if(!wire.isEmpty())udp.writeDatagram(wire,serverAddress,serverPort);
    }
    void cameraKeyframe(){
        const auto now=cameraClock.elapsed();
        if(!camera.ssrc || !encryptedReady || now-camera.lastPli<800)return;
        camera.lastPli=now;
        QByteArray pli(12,'\0');pli[0]=char(0x81);pli[1]=char(206);qToBigEndian<quint16>(2,pli.data()+2);
        qToBigEndian(ssrc,pli.data()+4);qToBigEndian(camera.ssrc,pli.data()+8);cameraRtcp(pli);
    }
    void cameraMissing(){
        const auto now=cameraClock.elapsed();if(now-camera.lastNack<30)return;camera.lastNack=now;
        QByteArray nack(16,'\0');nack[0]=char(0x81);nack[1]=char(205);qToBigEndian<quint16>(3,nack.data()+2);
        qToBigEndian(ssrc,nack.data()+4);qToBigEndian(camera.ssrc,nack.data()+8);qToBigEndian(camera.next,nack.data()+12);cameraRtcp(nack);
    }
    void drainCamera(){
        while(!camera.pending.empty()){
            auto next=camera.pending.find(camera.next);
            if(next==camera.pending.end()){
                const auto now=cameraClock.elapsed();if(!camera.gapSince)camera.gapSince=now;
                cameraMissing();if(now-camera.gapSince<90)return;
                next=camera.pending.begin();for(auto it=camera.pending.begin();it!=camera.pending.end();++it)
                    if(quint16(it->first-camera.next)<quint16(next->first-camera.next))next=it;
                camera.next=next->first;camera.depacketizer->reset();camera.gapSince=0;
                q->notifyGui(&VoiceEngine::cameraDiscontinuity);cameraKeyframe();continue;
            }
            auto packet=std::move(next->second);camera.queuedBytes-=packet.payload.size();camera.pending.erase(next);++camera.next;camera.gapSince=0;
            if(packet.hasOrientation){camera.rotation=packet.rotation;camera.mirrored=packet.mirrored;}
            auto complete=camera.depacketizer->push(packet.payload,packet.marker,packet.sequence,packet.timestamp);
            if(!complete)continue;
#ifdef RELAY_HAS_DAVE
            const auto decoder=decryptors.find(cameraUser);if(decoder==decryptors.end() || !encryptedReady)continue;
            QByteArray plain(complete->size(),Qt::Uninitialized);size_t length=0;
            if(daveDecryptorDecrypt(decoder->second,DAVE_MEDIA_TYPE_VIDEO,bytes(*complete),complete->size(),bytes(plain),plain.size(),&length)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS){++counters.mediaFailures;q->notifyGui(&VoiceEngine::cameraDiscontinuity);cameraKeyframe();continue;}
            plain.resize(length);q->notifyGui(&VoiceEngine::cameraFrameReceived,plain,packet.timestamp,camera.codec,camera.rotation,camera.mirrored);
#endif
        }
    }
    void receiveCamera(VideoPacket::Rtp packet){
        const auto *video=VideoPacket::videoCodec(packet.payloadType);
        if(!video || !StreamDecoder::supports(video->name))return;
        const bool repaired=packet.payloadType==video->repairType;
        if(repaired){if(packet.ssrc!=camera.rtx || packet.payload.size()<2)return;packet.ssrc=camera.ssrc;packet.sequence=qFromBigEndian<quint16>(packet.payload.constData());packet.payload.remove(0,2);packet.payloadType=video->payloadType;}
        if(packet.ssrc!=camera.ssrc)return;
        const QString codec=QString::fromLatin1(video->name);
        if(!camera.depacketizer || camera.codec!=codec){camera.codec=codec;camera.depacketizer=std::make_unique<VideoPacket::Depacketizer>(video->depacketizer);camera.pending.clear();camera.queuedBytes=0;camera.started=false;q->notifyGui(&VoiceEngine::cameraDiscontinuity);}
        if(!camera.started){camera.started=true;camera.next=camera.highest=packet.sequence;camera.base=packet.sequence;}
        const auto distance=qint16(packet.sequence-camera.next);if(distance<0 || camera.pending.contains(packet.sequence))return;
        if(distance>4096 || camera.pending.size()>=4096 || camera.queuedBytes+packet.payload.size()>16*1024*1024){
            camera.pending.clear();camera.queuedBytes=0;camera.next=packet.sequence;camera.depacketizer->reset();q->notifyGui(&VoiceEngine::cameraDiscontinuity);cameraKeyframe();
        }
        if(qint16(packet.sequence-camera.highest)>0){if(packet.sequence<camera.highest)camera.cycles+=65536;camera.highest=packet.sequence;}
        ++camera.received;camera.queuedBytes+=packet.payload.size();camera.pending.emplace(packet.sequence,std::move(packet));drainCamera();
    }
    void cameraFeedback(){
        if(!cameraUser.isEmpty() && encryptedReady){
            if(camera.depacketizer)drainCamera();
            const auto now=cameraClock.elapsed();if(!camera.ssrc || now-camera.lastReport<1000)return;camera.lastReport=now;
            if(!camera.started){cameraKeyframe();return;}
            const quint32 highest=camera.cycles+camera.highest,expected=highest-camera.base+1;
            const auto intervalExpected=expected-camera.lastExpected,intervalReceived=camera.received-camera.lastReceived;
            const auto lost=qint64(intervalExpected)-intervalReceived;
            const int fraction=intervalExpected && lost>0?std::min<qint64>(255,lost*256/intervalExpected):0;
            camera.lastExpected=expected;camera.lastReceived=camera.received;
            QByteArray report(32,'\0');report[0]=char(0x81);report[1]=char(201);qToBigEndian<quint16>(7,report.data()+2);
            qToBigEndian(ssrc,report.data()+4);qToBigEndian(camera.ssrc,report.data()+8);
            qToBigEndian((quint32(fraction)<<24)|quint32(std::clamp<qint64>(qint64(expected)-camera.received,0,0x7fffff)),report.data()+12);
            qToBigEndian(highest,report.data()+16);qToBigEndian(camera.lsr,report.data()+24);
            if(camera.lsr)qToBigEndian(quint32((now-camera.lastSr)*65536/1000),report.data()+28);
            cameraRtcp(report);
        }
    }
    void receive(){
        int processed=0;
        while(udp.hasPendingDatagrams() && ++processed<=512){
            const auto datagram=udp.receiveDatagram();if(datagram.senderAddress()!=serverAddress || datagram.senderPort()!=serverPort)continue;
            if(discovery){const auto result=VoicePacket::parseDiscovery(datagram.data(),ssrc);if(!result)continue;discovery=false;
                send(1,{{"protocol","udp"},{"data",QJsonObject{{"address",result->address},{"port",int(result->port)},{"mode",transportMode}}},
                        {"codecs",mediaCodecs()}});setStage("session_description");setStatus("Negotiating audio encryption…");continue;}
            ++counters.udpReceived;
            if(!transportReady || datagram.data().size()<=12)continue;
            const auto type=quint8(datagram.data()[1]);
            if(type>=192 && type<=223){
                ++counters.rtcp;
                const auto report=VideoPacket::decryptRtcp(datagram.data(),key);
                if(report)for(qsizetype at=0;at+8<=report->size();){
                    const auto *part=report->constData()+at;const qsizetype length=(qFromBigEndian<quint16>(part+2)+1)*4;
                    if(length<8 || at+length>report->size())break;
                    if(length>=28 && quint8(part[1])==200 && qFromBigEndian<quint32>(part+4)==camera.ssrc){
                        camera.lsr=(qFromBigEndian<quint32>(part+8)<<16)|(qFromBigEndian<quint32>(part+12)>>16);camera.lastSr=cameraClock.elapsed();
                    }at+=length;
                }
                continue;
            }
            if((type&0x7f)!=120){
                ++counters.otherRtp;
                if(!cameraUser.isEmpty() && encryptedReady){if(auto frame=VideoPacket::decryptRtp(datagram.data(),key))receiveCamera(std::move(*frame));else ++counters.transportFailures;}
                continue;
            }
            ++counters.audioRtp;
            if(micTesting || deafened || serverDeafened){++counters.deafDrops;continue;}
            if((quint8(datagram.data()[1])&0x7f)!=120)continue;
            const auto frame=VoicePacket::decrypt(datagram.data(),key);if(!frame){++counters.transportFailures;continue;}
            auto peer=peers.find(frame->ssrc);if(peer==peers.end()){++counters.unknownSsrc;continue;}auto &p=*peer->second;
            // RTP sequence numbers have a defined byte order; transport nonce bytes do not.
            // Authenticate first, then apply a per-SSRC window with 16-bit wrap handling.
            if(!p.sequenceSeen){p.sequenceSeen=true;p.lastSequence=frame->sequence;p.replayWindow=1;}
            else {
                const auto forward=qint16(quint16(frame->sequence-p.lastSequence));
                if(forward>0){p.replayWindow=forward>=64?1:(p.replayWindow<<forward)|1;p.lastSequence=frame->sequence;}
                else{const auto delta=quint16(p.lastSequence-frame->sequence);if(delta>=64 || (p.replayWindow&(quint64(1)<<delta)))continue;p.replayWindow|=quint64(1)<<delta;}
            }
            QByteArray opus;
#ifdef RELAY_HAS_DAVE
            auto dec=decryptors.find(p.user);if(dec==decryptors.end())continue;
            opus.resize(frame->payload.size());size_t size=0;
            if(daveDecryptorDecrypt(dec->second,DAVE_MEDIA_TYPE_AUDIO,bytes(frame->payload),frame->payload.size(),bytes(opus),opus.size(),&size)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS){++counters.mediaFailures;continue;}
            opus.resize(size);++counters.mediaReceived;
#endif
            if(opus.isEmpty())continue;
            if(!p.started && p.queue.empty())p.next=frame->sequence;
            const auto distance=qint16(frame->sequence-p.next);
            if(p.started && distance<0){++counters.receiveLatePackets;continue;}
            if(!p.started && distance<0 && distance>=-50)p.next=frame->sequence;
            if(distance>50 || p.queue.size()>25){p.queue.clear();p.pcm.clear();p.started=false;p.next=frame->sequence;}
            p.queue[frame->sequence]=opus;
            observeReceiveBacklog(p);
        }
        if(udp.hasPendingDatagrams())QTimer::singleShot(0,this,[this]{receive();});
    }
    qint64 playbackBufferBytes()const{
        if(fixtureMode && fixtureAudioOutput)return fixtureOutputBufferBytes;
        return sink?sink->bufferSize():0;
    }
    qint64 playbackFreeBytes()const{
        if(fixtureMode && fixtureAudioOutput)return std::max<qint64>(0,fixtureOutputBufferBytes-fixtureAudioOutput->bytesToWrite());
        return sink?std::max<qint64>(0,sink->bytesFree()):0;
    }
    qint64 peerQueuedSamples(const Peer &peer)const{
        qint64 samples=peer.pcm.size()/(channels*2);int packets=0,span=0;
        for(const auto &[sequence,packet]:peer.queue){
            const auto distance=qint16(sequence-peer.next);if(distance<0)continue;
            const int count=opus_packet_get_nb_samples(bytes(packet),packet.size(),sampleRate);
            samples+=count>0?count:samplesPerFrame;++packets;span=std::max(span,int(distance)+1);
        }
        // Missing sequence slots still occupy the existing 20 ms PLC timeline.
        return samples+qint64(std::max(0,span-packets))*samplesPerFrame;
    }
    qint64 receiveQueuedMs()const{
        qint64 samples=0;for(const auto &[_,peer]:peers)samples=std::max(samples,peerQueuedSamples(*peer));
        return samples*1000/sampleRate;
    }
    void observeReceiveBacklog(const Peer &peer){
        counters.receiveMaxQueuedMs=std::max(counters.receiveMaxQueuedMs,peerQueuedSamples(peer)*1000/sampleRate);
    }
    bool decodePeerAudio(Peer &peer,bool retiring=false){
        auto it=peer.queue.find(peer.next);QByteArray frame;
        if(it!=peer.queue.end()){frame=it->second;peer.queue.erase(it);peer.misses=0;}
        else if(++peer.misses>5){peer.started=false;peer.queue.clear();peer.pcm.clear();return false;}
        ++peer.next;
        std::array<opus_int16,5760*channels> decoded;
        const int count=opus_decode(peer.decoder,frame.isEmpty()?nullptr:bytes(frame),frame.size(),decoded.data(),frame.isEmpty()?samplesPerFrame:5760,0);
        if(count<=0)return false;
        peer.pcm.append(reinterpret_cast<const char*>(decoded.data()),count*channels*2);
        if(frame.isEmpty())++counters.receiveConcealedPackets;
        else{++counters.decoded;if(retiring)++counters.receiveDiscardedPackets;}
        return true;
    }
    void retireStaleReceive(quint64 skippedSlots,int renderSteps){
        counters.receiveSkippedSlots+=skippedSlots;
        // Already-rendered samples waited through the entire late wake. They
        // must not be played in front of the recovered, recent peer speech.
        counters.outputDropped+=(outputPending.size()+pcmBytes-1)/pcmBytes;outputPending.clear();
        const qint64 budget=std::min<quint64>(skippedSlots,quint64(std::numeric_limits<int>::max()/samplesPerFrame))*samplesPerFrame;
        const qint64 target=qint64(renderSteps+2)*samplesPerFrame;
        for(auto &[_,ptr]:peers){auto &peer=*ptr;observeReceiveBacklog(peer);
            // A fresh sender has no established playout age to retire.
            if(!peer.started)continue;
            qint64 discard=std::min(budget,std::max<qint64>(0,peerQueuedSamples(peer)-target));
            if(!discard)continue;
            const qint64 requested=discard;
            // Decode before discarding so Opus prediction continues exactly.
            // A partial long packet keeps its remaining samples for playback.
            for(int work=0;discard>0 && work<64;++work){
                if(peer.pcm.isEmpty() && !decodePeerAudio(peer,true))break;
                const qint64 samples=std::min<qint64>(discard,peer.pcm.size()/(channels*2));
                if(!samples)break;
                peer.pcm.remove(0,samples*channels*2);discard-=samples;counters.receiveDiscardedSamples+=samples;
            }
            if(discard<requested)++counters.receiveRebases;
        }
    }
    void processAudio(){
        if(!connected || processingAudio)return;
        QScopedValueRollback<bool> processing(processingAudio,true);
        // Source and socket notifications can be queued behind a busy UI timer.
        // Pull their pending data before considering either stream starved.
        readCapture();receive();
        if(!audioClock.isValid()){audioClock.start();audioSteps=0;audioTimestampBase=timestamp;}
        const auto elapsed=fixtureMode && fixturePlaybackElapsed>=0?fixturePlaybackElapsed:audioClock.elapsed();const auto expected=quint64(elapsed/20);
        const auto due=expected>audioSteps?expected-audioSteps:0;
        if(due>1){++counters.audioLateWakes;counters.maxAudioLateMs=std::max(counters.maxAudioLateMs,elapsed-qint64(audioSteps+1)*20);}
        const bool canSend=encryptedReady && !micTesting && !muted && !deafened && !serverMuted && !serverDeafened;
        const bool pendingDatagrams=udp.hasPendingDatagrams();
        int renderSteps=pendingDatagrams?0:int(std::min<quint64>(maxAudioSteps,due));
        if(!pendingDatagrams){
            audioSteps=expected;
            if(due>quint64(maxAudioSteps)){
                // A late wake must not refill a short native sink with 200 ms of
                // old catch-up PCM. Admit only what its current capacity holds.
                const int capacity=fixtureMode && !fixtureAudioOutput?maxAudioSteps:int(playbackFreeBytes()/pcmBytes);
                renderSteps=std::min(renderSteps,capacity);
                retireStaleReceive(due-quint64(renderSteps),renderSteps);
            }
        }
        // The microphone's sample count is its clock: callback batching must
        // never splice samples or manufacture RTP gaps. A soundboard without
        // microphone samples follows the elapsed playback clock instead.
        const auto captureFrames=capture.size()/pcmBytes;
        const auto lastCapture=lastAudioInputMs>=0?lastAudioInputMs:captureOpenedMs;
        const bool timelyCapture=(source || (fixtureMode && captureSeen)) && lastCapture>=0 && elapsed-lastCapture<=captureCapacityFrames*20;
        const auto boardSteps=!soundboardPcm.isEmpty() && !timelyCapture?qsizetype(std::min<quint64>(maxAudioSteps,due)):0;
        const auto queuedInput=std::max(captureFrames,boardSteps);
        const int inputSteps=canSend?int(std::min<qsizetype>(maxAudioSteps,queuedInput)):0;
        if(inputSteps>1)counters.captureCatchups+=inputSteps-1;
        if(inputSteps && (lastAudioInputMs<0 || elapsed-lastAudioInputMs>captureCapacityFrames*20)){
            // A stopped or genuinely overrun source cannot replay seconds of
            // missing audio. Keep retained data's age bounded to the queue.
            const auto oldest=expected>quint64(queuedInput)?expected-queuedInput:0;
            const auto current=audioTimestampBase+quint32(oldest*samplesPerFrame);
            if(qint32(current-timestamp)>0)timestamp=current;
        }
        if(inputSteps && captureFrames)lastAudioInputMs=elapsed;
        if(!canSend){const auto current=audioTimestampBase+quint32(expected*samplesPerFrame);if(qint32(current-timestamp)>0)timestamp=current;}
        // Native capture blocks need not align with the 20 ms voice timer.
        // Keep the indicator through a short empty slot, without extending
        // the processor's existing release when actual silence is received.
        if(!canSend || (!inputSteps && (lastLocalSpeakingAudioMs<0 || elapsed-lastLocalSpeakingAudioMs>=100)))updateSpeaking(false);
        for(int step=0;step<std::max(inputSteps,renderSteps);++step)processAudioFrame(step<inputSteps,step<renderSteps);
        if(inputSteps)q->notifyGui(&VoiceEngine::inputLevelChanged);
    }
    void flushPlayback(){
        for(int writes=0;writes<64 && !outputPending.isEmpty();++writes){
            if(fixtureMode && !fixtureAudioOutput){fixtureOutput+=outputPending;outputPending.clear();break;}
            QIODevice *device=fixtureMode?fixtureAudioOutput.data():playbackDevice;
            if(!device)break;
            const auto count=std::min<qint64>(playbackFreeBytes(),outputPending.size());if(count<=0)break;
            const auto written=device->write(outputPending.constData(),count);if(written<=0)break;
            if(fixtureMode)fixtureOutput+=outputPending.first(written);else counters.outputBytes+=written;
            outputPending.remove(0,written);
        }
        if(fixtureMode && fixtureOutput.size()>pcmBytes*50)fixtureOutput.remove(0,fixtureOutput.size()-pcmBytes*50);
    }
    void processAudioFrame(bool sendInput,bool renderOutput){
        if(sendInput && encryptedReady && !micTesting && !muted && !deafened && !serverMuted && !serverDeafened && (capture.size()>=pcmBytes || soundboardPcm.size()>=pcmBytes)){
            QByteArray pcm(pcmBytes,'\0');
            if(capture.size()>=pcmBytes){pcm=capture.left(pcmBytes);capture.remove(0,pcmBytes);}
            std::array<opus_int16,samplesPerFrame*channels> samples;std::memcpy(samples.data(),pcm.constData(),pcmBytes);
            const int outputDelay = sink ? int(format().durationForBytes(sink->bufferSize()-sink->bytesFree())/1000) : 0;
            bool active = processor.capture(samples.data(), outputDelay + 40);
            inputLevel=processor.peak();
            if(soundboardPcm.size()>=pcmBytes){
                for(int i=0;i<samplesPerFrame*channels;++i){qint16 sample;std::memcpy(&sample,soundboardPcm.constData()+i*2,2);samples[i]=qint16(std::clamp(int(samples[i])+int(sample),-32768,32767));if(std::abs(int(sample))>65)active=true;}
                soundboardPcm.remove(0,pcmBytes);
            }
            QByteArray opus(1275,Qt::Uninitialized);const int size=opus_encode(encoder,samples.data(),samplesPerFrame,bytes(opus),opus.size());
            if(size>0){opus.resize(size);QByteArray encrypted;
#ifdef RELAY_HAS_DAVE
                encrypted.resize(daveEncryptorGetMaxCiphertextByteSize(encryptor,DAVE_MEDIA_TYPE_AUDIO,opus.size()));size_t length=0;
                if(daveEncryptorEncrypt(encryptor,DAVE_MEDIA_TYPE_AUDIO,ssrc,bytes(opus),opus.size(),bytes(encrypted),encrypted.size(),&length)==DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS)encrypted.resize(length);else{encrypted.clear();++counters.mediaFailures;}
#endif
                if(!encrypted.isEmpty()){
                    if(nonce==std::numeric_limits<quint32>::max()){fail("Voice transport reached its encryption counter limit. Rejoin to obtain fresh keys.");return;}
                    updateSpeaking(active);const auto packet=VoicePacket::encrypt(encrypted,key,sequence++,timestamp,ssrc,nonce++);if(udp.writeDatagram(packet,serverAddress,serverPort)==packet.size())++counters.mediaSent;else ++counters.writeErrors;
                }
            }else ++counters.encodeErrors;
        }
        if(sendInput)timestamp+=samplesPerFrame;
        if(!renderOutput || micTesting || deafened || serverDeafened || (!fixtureMode && (!sink || !playbackDevice)))return;
        std::array<int,samplesPerFrame*channels> mixed{};bool hasAudio=false;
        for(auto &[_,ptr]:peers){auto &p=*ptr;
            if(p.activityFrames>0)--p.activityFrames;
            if(p.activityFrames==0)markSpeaking(p.user,false);
            if(!p.started){if(p.queue.size()<3)continue;p.started=true;p.misses=0;}
            if(p.pcm.size()<pcmBytes)decodePeerAudio(p);
            if(p.pcm.size()<pcmBytes)continue;
            const auto local=userAudio.value(p.user);
            const double gain=p.limiter.frameGain(p.pcm.constData(), samplesPerFrame*channels,
                local.muted?0:local.volume, voiceLimiterEnabled, voiceLimiterCeiling);
            // Continue decoding muted users so unmuting resumes at current audio,
            // with a valid Opus state and no stale speech queued for playback.
            double energy=0;
            if(gain>0)for(int i=0;i<samplesPerFrame*channels;i++){qint16 sample;std::memcpy(&sample,p.pcm.constData()+i*2,2);mixed[i]+=int(std::lround(double(sample)*gain));energy+=double(sample)*sample;}
            // Ignore Opus comfort noise; retain a short 200 ms release so the
            // ring does not flicker between syllables. Audio mixing is unchanged.
            if(gain>0 && std::sqrt(energy/(samplesPerFrame*channels))>=65)p.activityFrames=10;
            if(gain==0)p.activityFrames=0;
            markSpeaking(p.user,p.activityFrames>0);
            p.pcm.remove(0,pcmBytes);hasAudio=true;
        }
        // Received Discord soundboard assets are a distinct local source.
        // Apply their listening gains to samples before the final output mix;
        // microphone, peer speech and outgoing custom-board PCM never use it.
        for(auto it=receivedSoundboards.begin();it!=receivedSoundboards.end();){
            const auto local=userAudio.value(it->user);
            const double gain=local.muted?0:soundboardVolume*local.soundboardVolume*it->sourceVolume;
            const auto available=std::min<qsizetype>(pcmBytes,it->pcm.size()-it->offset);
            if(gain>0)for(qsizetype offset=0;offset+1<available;offset+=2){
                qint16 sample;std::memcpy(&sample,it->pcm.constData()+it->offset+offset,2);
                mixed[size_t(offset/2)]+=int(std::lround(double(sample)*gain));
            }
            it->offset+=available;hasAudio=true;
            if(it->offset>=it->pcm.size())it=receivedSoundboards.erase(it);else ++it;
        }
        if(hasAudio){std::array<qint16,samplesPerFrame*channels> pcm;for(int i=0;i<int(pcm.size());i++)pcm[i]=qint16(std::clamp(mixed[i],-32768,32767));processor.playback(pcm.data());outputPending.append(reinterpret_cast<const char*>(pcm.data()),pcmBytes);}
        if(outputPending.size()>pcmBytes*captureCapacityFrames){const auto dropped=outputPending.size()/pcmBytes-maxAudioSteps;counters.outputDropped+=dropped;outputPending.remove(0,dropped*pcmBytes);}
        flushPlayback();
    }
};

VoiceEngine::VoiceEngine(QObject *parent):QObject(parent),m_runtimeThread(std::make_unique<QThread>()) {
    m_runtimeThread->setObjectName("Relay voice");
    // Construct every media, encryption and transport object on one owner.
    // This thread never waits on the GUI, including while delivering signals.
    auto *bootstrap=new QObject;bootstrap->moveToThread(m_runtimeThread.get());
    m_runtimeThread->start();
    QMetaObject::invokeMethod(bootstrap,[this]{
        d=std::make_unique<Private>(this);m_runtimeContext=d.get();
        if(sodium_init()<0)d->error="Could not initialize audio transport encryption.";
#ifdef RELAY_HAS_DAVE
        daveSetLogSinkCallback([](DAVELoggingSeverity,const char*,int,const char*){});
#endif
        d->ws.setMaxAllowedIncomingMessageSize(2*1024*1024);
        d->ws.setMaxAllowedIncomingFrameSize(2*1024*1024);
        d->deadline.setSingleShot(true);d->reconnectTimer.setSingleShot(true);d->deadline.setObjectName("voiceDeadline");d->reconnectTimer.setObjectName("voiceReconnect");d->tick.setTimerType(Qt::PreciseTimer);d->micTestTick.setTimerType(Qt::PreciseTimer);
        connect(&d->micTestTick,&QTimer::timeout,d.get(),[this]{d->processMicTest();});
        connect(&d->heartbeat,&QTimer::timeout,d.get(),[this]{d->heartbeatNow();});
        d->diagnosticTimer.setInterval(1000);
        connect(&d->diagnosticTimer,&QTimer::timeout,d.get(),[this]{notifyGui(&VoiceEngine::diagnosticsChanged);});
        connect(&d->keepAlive,&QTimer::timeout,d.get(),[this]{
            // Match Discord's UDP keepalive wire format; preserve NAT mappings while muted.
            QByteArray packet(8,'\0');qToLittleEndian<quint32>(d->keepAliveCounter++,packet.data());
            d->udp.writeDatagram(packet,d->serverAddress,d->serverPort);
        });
        connect(&d->deadline,&QTimer::timeout,d.get(),[this]{d->connectionDeadline();});
        connect(&d->reconnectTimer,&QTimer::timeout,d.get(),[this]{d->openGateway();});
        connect(&d->tick,&QTimer::timeout,d.get(),[this]{d->processAudio();});
        connect(&d->tick,&QTimer::timeout,d.get(),[this]{d->cameraFeedback();});
        connect(&d->ws,&QWebSocket::connected,d.get(),[this]{
            d->history.record(VoiceDiagnostics::Event::WebSocketConnected,VoiceDiagnostics::stage(d->stage));
            if(d->resuming){
                d->send(7,{{"server_id",d->guildId},{"channel_id",d->channelId},{"session_id",d->sessionId},{"token",d->token},{"seq_ack",d->gatewaySequence}});return;
            }
            int version=0;
#ifdef RELAY_HAS_DAVE
            version=daveMaxSupportedProtocolVersion();
            // Create the MLS session before the server may send its external-sender package.
            d->session=daveSessionCreate(nullptr,nullptr,nullptr,nullptr);
#endif
            d->send(0,{{"server_id",d->guildId},{"channel_id",d->channelId},{"user_id",d->userId},{"session_id",d->sessionId},{"token",d->token},{"video",true},{"streams",QJsonArray{}},{"max_dave_protocol_version",version}});
        });
        connect(&d->ws,&QWebSocket::textMessageReceived,d.get(),[this](const QString &message){d->text(message);});
        connect(&d->ws,&QWebSocket::binaryMessageReceived,d.get(),[this](const QByteArray &message){
#ifdef RELAY_HAS_DAVE
            d->binary(message);
#else
            Q_UNUSED(message);
#endif
        });
        connect(&d->ws,&QWebSocket::disconnected,d.get(),[this]{
            if(d->closing || d->channelId.isEmpty())return;
            // Qt retains a previous close frame code across reuse. A fresh TCP
            // failure has no close frame and must not inherit an old kick or
            // rejected-resume code from a different connection.
            d->counters.closeCode=d->lastSocketError==int(QAbstractSocket::RemoteHostClosedError)?1006:int(d->ws.closeCode());d->history.record(VoiceDiagnostics::Event::WebSocketClosed,VoiceDiagnostics::stage(d->stage),d->counters.closeCode);d->recoverSocket("socket_closed",d->counters.closeCode);
        });
        connect(&d->ws,&QWebSocket::errorOccurred,d.get(),[this](QAbstractSocket::SocketError error){
            if(d->closing)return;
            d->lastSocketError=int(error);d->history.record(VoiceDiagnostics::Event::WebSocketError,VoiceDiagnostics::stage(d->stage),int(error));
            const auto generation=d->socketGeneration;
            // A peer close can emit error before disconnected. Defer so its
            // protocol close code (including a kick) controls recovery.
            QTimer::singleShot(0,d.get(),[this,generation]{if(generation==d->socketGeneration && !d->closing && !d->channelId.isEmpty() && !d->reconnectTimer.isActive() && d->ws.state()==QAbstractSocket::UnconnectedState)d->recoverSocket("socket_error");});
        });
        connect(&d->udp,&QUdpSocket::readyRead,d.get(),[this]{d->receive();});
        connect(&d->media,&QMediaDevices::audioInputsChanged,d.get(),[this]{notifyGui(&VoiceEngine::devicesChanged);const bool active=d->source!=nullptr;d->stopCapture();if(active){if(d->micTesting && !d->startCapture())stopMicTest();else d->updateCapture();}});
        connect(&d->media,&QMediaDevices::audioOutputsChanged,d.get(),[this]{notifyGui(&VoiceEngine::devicesChanged);d->stopPlayback();if(d->connected)d->startPlayback();if(d->micTesting){d->stopMicPlayback();if(!d->startMicPlayback())stopMicTest();}});
    },Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(bootstrap,&QObject::deleteLater,Qt::QueuedConnection);
}
VoiceEngine::~VoiceEngine(){
    QMetaObject::invokeMethod(m_runtimeContext,[this]{d->reset();d.reset();m_runtimeContext=nullptr;},Qt::BlockingQueuedConnection);
    m_runtimeThread->quit();m_runtimeThread->wait();
}
bool VoiceEngine::canPlaySoundboard()const{return onRuntime([&]() -> bool {return d->connected && d->encryptedReady && !d->micTesting && !d->muted && !d->deafened && !d->serverMuted && !d->serverDeafened;
});}
bool VoiceEngine::playSoundboardPcm(const QByteArray &pcm){return onRuntime([&]() -> bool {
    if(!canPlaySoundboard() || pcm.isEmpty() || pcm.size()>pcmBytes*1500 || pcm.size()%4)return false;
    d->soundboardPcm=pcm;
    if(d->soundboardPcm.size()%pcmBytes)d->soundboardPcm.append(pcmBytes-d->soundboardPcm.size()%pcmBytes,'\0');
    return true;

});}
void VoiceEngine::stopSoundboard(){return onRuntime([&]() -> void {d->soundboardPcm.clear();
});}
QString VoiceEngine::status()const{return onRuntime([&]() -> QString {return d->status;
});}bool VoiceEngine::connected()const{return onRuntime([&]() -> bool {return d->connected;
});}
bool VoiceEngine::available()const{return onRuntime([&]() -> bool {
#ifdef RELAY_HAS_DAVE
    return true;
#else
    return false;
#endif

});}
bool VoiceEngine::muted()const{return onRuntime([&]() -> bool {return d->muted||d->deafened||d->micTesting;
});}bool VoiceEngine::deafened()const{return onRuntime([&]() -> bool {return d->deafened||d->micTesting;
});}
QString VoiceEngine::channelName()const{return onRuntime([&]() -> QString {return d->channelName;
});}QString VoiceEngine::error()const{return onRuntime([&]() -> QString {return d->error;
});}
QVariantList VoiceEngine::inputDevices()const{return onRuntime([&]() -> QVariantList {return deviceList(true);
});}QVariantList VoiceEngine::outputDevices()const{return onRuntime([&]() -> QVariantList {return deviceList(false);
});}
QString VoiceEngine::inputDeviceId()const{return onRuntime([&]() -> QString {return d->inputId;
});}QString VoiceEngine::outputDeviceId()const{return onRuntime([&]() -> QString {return d->outputId;
});}
double VoiceEngine::inputLevel()const{return onRuntime([&]() -> double {return d->inputLevel;
});}bool VoiceEngine::micTesting()const{return onRuntime([&]() -> bool {return d->micTesting;
});}
double VoiceEngine::latencyMs()const{return onRuntime([&]() -> double {return d->latency;
});}double VoiceEngine::averageLatencyMs()const{return onRuntime([&]() -> double {return d->averageLatency;
});}
QVariantList VoiceEngine::latencyHistory()const{return onRuntime([&]() -> QVariantList {return d->latencySamples;
});}QString VoiceEngine::voiceServerLabel()const{return onRuntime([&]() -> QString {return d->serverLabel;
});}
void VoiceEngine::join(const QString &guild,const QString &channel,const QString &name,const QString &user){return onRuntime([&]() -> void {
    if(!available()){d->setError("Native voice requires libdave. Build the bundled encryption dependency and rebuild Relay.");return;}
    if(guild.isEmpty() || channel.isEmpty() || user.isEmpty()){d->setError("Sign in and choose a server voice channel first.");return;}
    if(d->userId!=user){++d->accountGeneration;d->leavePendingGuilds.clear();d->expiredLeaveGuilds.clear();d->supersededChannels.clear();}
    if(d->userId==user && d->guildId==guild && d->channelId==channel && (d->connected || d->awaitingLeave || d->deadline.isActive() || d->reconnectTimer.isActive()))return;
    const auto oldChannel=d->guildId==guild?d->channelId:QString();
    const auto oldChannels=d->guildId==guild?d->supersededChannels:QSet<QString>{};
    // A previous leave can still be in flight on the main gateway. Wait for its
    // acknowledgement rather than letting its null state cancel this new join.
    if(!d->channelId.isEmpty() && d->guildId!=guild)leave();else d->reset();
    d->counters={};d->receivedSelfState=d->receivedServerUpdate=false;d->lastSocketError=-1;d->history.beginAttempt();d->diagnosticTimer.start();d->freshAllocationUsed=false;
    d->supersededChannels=oldChannels;
    if(!oldChannel.isEmpty() && oldChannel!=channel && d->supersededChannels.size()<32)d->supersededChannels.insert(oldChannel);
    d->supersededChannels.remove(channel);
    d->error.clear();d->guildId=guild;d->channelId=channel;d->channelName=name;d->userId=user;
    d->sequence=quint16(QRandomGenerator::global()->generate());d->timestamp=QRandomGenerator::global()->generate();d->nonce=0x80000000u;
    if(d->leavePendingGuilds.contains(guild)){d->awaitingLeave=true;d->setStage("awaiting_leave");d->setStatus("Reconnecting to voice…");d->deadline.start(10000);}
    else d->requestJoin();
});}
void VoiceEngine::leave(){return onRuntime([&]() -> void {d->history.record(VoiceDiagnostics::Event::Left,VoiceDiagnostics::stage(d->stage));const auto guild=d->guildId;d->reset();d->awaitingLeave=false;d->guildId.clear();d->channelId.clear();d->channelName.clear();d->error.clear();d->setStage("idle");d->setStatus("Not connected");if(!guild.isEmpty() && !d->leavePendingGuilds.contains(guild)){d->leavePendingGuilds.insert(guild);d->requestVoiceState(guild,QString(),d->muted,d->deafened);}
});}
void VoiceEngine::setMuted(bool value){return onRuntime([&]() -> void {
    const bool requestCue=d->muted!=value || (d->muted||d->micTesting)!=value;
    if(d->micTesting)stopMicTest();
    // An explicit unmute also releases the mute imposed by deafen.
    if(!value && d->deafened)setDeafened(false);
    if(d->muted!=value){
        d->muted=value;d->updateCapture();
        if(value){d->soundboardPcm.clear();d->updateSpeaking(false);}
        d->notify();d->requestCurrentState();
    }
    if(requestCue)notifyGui(&VoiceEngine::localAudioCueRequested,QString(value?"mute":"unmute"));
});}
void VoiceEngine::setDeafened(bool value){return onRuntime([&]() -> void {
    const bool requestCue=d->deafened!=value || (d->deafened||d->micTesting)!=value;
    if(d->micTesting)stopMicTest();
    if(d->deafened!=value){
        d->deafened=value;
        if(value){d->soundboardPcm.clear();d->stopPlayback();d->updateSpeaking(false);}
        else if(d->connected)d->startPlayback();
        d->updateCapture();d->notify();d->requestCurrentState();
    }
    if(requestCue)notifyGui(&VoiceEngine::localAudioCueRequested,QString(value?"deafen":"undeafen"));
});}
void VoiceEngine::setInputDeviceId(const QString &id){return onRuntime([&]() -> void {if(d->inputId==id)return;d->inputId=id;d->stopCapture();if(d->micTesting && !d->startCapture())stopMicTest();else d->updateCapture();notifyGui(&VoiceEngine::devicesChanged);
});}
void VoiceEngine::setOutputDeviceId(const QString &id){return onRuntime([&]() -> void {if(d->outputId==id)return;d->outputId=id;d->stopPlayback();if(d->connected)d->startPlayback();if(d->micTesting){d->stopMicPlayback();if(!d->startMicPlayback())stopMicTest();}notifyGui(&VoiceEngine::devicesChanged);
});}
bool VoiceEngine::userMuted(const QString &id)const{return onRuntime([&]() -> bool {return d->userAudio.value(id).muted;
});}
double VoiceEngine::userVolume(const QString &id)const{return onRuntime([&]() -> double {return d->userAudio.value(id).volume;
});}
bool VoiceEngine::setUserMuted(const QString &id,bool value){return onRuntime([&]() -> bool {
    if(!validUserId(id) || (!d->userAudio.contains(id) && d->userAudio.size()>=1000))return false;
    auto settings=d->userAudio.value(id);if(settings.muted==value)return true;settings.muted=value;
    if(!settings.muted && settings.volume==1 && settings.soundboardVolume==1)d->userAudio.remove(id);else d->userAudio.insert(id,settings);
    d->outputPending.clear();notifyGui(&VoiceEngine::userAudioChanged,id);return true;

});}
bool VoiceEngine::setUserVolume(const QString &id,double value){return onRuntime([&]() -> bool {
    if(!validUserId(id) || !std::isfinite(value) || (!d->userAudio.contains(id) && d->userAudio.size()>=1000))return false;
    value=std::clamp(value,0.0,2.0);auto settings=d->userAudio.value(id);if(settings.volume==value)return true;settings.volume=value;
    if(!settings.muted && settings.volume==1 && settings.soundboardVolume==1)d->userAudio.remove(id);else d->userAudio.insert(id,settings);
    d->outputPending.clear();notifyGui(&VoiceEngine::userAudioChanged,id);return true;

});}
double VoiceEngine::userSoundboardVolume(const QString &id)const{return onRuntime([&]() -> double {return d->userAudio.value(id).soundboardVolume;});}
bool VoiceEngine::setUserSoundboardVolume(const QString &id,double value){return onRuntime([&]() -> bool {
    if(!validUserId(id) || !std::isfinite(value) || (!d->userAudio.contains(id) && d->userAudio.size()>=1000))return false;
    value=std::clamp(value,0.0,2.0);auto settings=d->userAudio.value(id);if(settings.soundboardVolume==value)return true;settings.soundboardVolume=value;
    if(!settings.muted && settings.volume==1 && settings.soundboardVolume==1)d->userAudio.remove(id);else d->userAudio.insert(id,settings);
    notifyGui(&VoiceEngine::userAudioChanged,id);return true;
});}
void VoiceEngine::setSoundboardVolume(double value){return onRuntime([&]() -> void {
    if(std::isfinite(value))d->soundboardVolume=std::clamp(value,0.0,1.0);
});}
quint64 VoiceEngine::soundboardPlaybackGeneration()const{return onRuntime([&]() -> quint64 {return d->soundboardPlaybackGeneration;});}
bool VoiceEngine::playReceivedSoundboardPcm(const QString &id,const QByteArray &pcm,double sourceVolume,quint64 generation){return onRuntime([&]() -> bool {
    if(generation!=d->soundboardPlaybackGeneration || (!d->connected && !d->fixtureMode) || d->micTesting || d->deafened || d->serverDeafened || !validUserId(id)
        || pcm.isEmpty() || pcm.size()%4 || pcm.size()>pcmBytes*500 || !std::isfinite(sourceVolume) || d->receivedSoundboards.size()>=8)return false;
    d->receivedSoundboards.append({id,pcm,0,std::clamp(sourceVolume,0.0,1.0)});return true;
});}
void VoiceEngine::stopReceivedSoundboards(){return onRuntime([&]() -> void {d->receivedSoundboards.clear();++d->soundboardPlaybackGeneration;});}
void VoiceEngine::clearUserAudioSettings(){return onRuntime([&]() -> void {if(d->userAudio.isEmpty())return;d->userAudio.clear();d->outputPending.clear();notifyGui(&VoiceEngine::userAudioChanged,QString());
});}
void VoiceEngine::configureAudio(const AudioProcessor::Options &options){return onRuntime([&]() -> void {d->voiceLimiterEnabled=options.voiceLimiterEnabled; d->voiceLimiterCeiling=std::isfinite(options.voiceLimiterCeiling)?std::clamp(options.voiceLimiterCeiling,.1,1.):.5; d->processor.configure(options);if(!d->processor.healthy())d->setError("Audio processing could not initialize. Disable processing and try again.");
});}
void VoiceEngine::startMicTest(){
    // Local playback must be excluded from every outgoing capture route before
    // it starts. Run this preflight on the facade owner; the audio owner never
    // waits for GUI work, even if a non-GUI caller requests a microphone test.
    if(QThread::currentThread()!=thread()){
        QMetaObject::invokeMethod(this,[this]{startMicTest();},Qt::QueuedConnection);return;
    }
    if(micTesting())return;
    emit micTestStarting();
    return onRuntime([&]() -> void {
    if(d->micTesting)return;
    // The saved mute/deafen choices remain underneath this temporary local
    // state. Keep the channel and its transport alive while isolating the test.
    d->error.clear();d->micTesting=true;d->stopCapture();d->stopPlayback();d->soundboardPcm.clear();d->updateSpeaking(false);
    d->fixtureOutput.clear();d->micTestFrames=0;
    if(!d->startMicPlayback() || !d->startCapture()){
        const auto reason=d->error;d->finishMicTest(true,false);d->setError(reason);return;
    }
    d->notify();
    d->requestCurrentState();

});}
void VoiceEngine::stopMicTest(){return onRuntime([&]() -> void {d->finishMicTest(true,true);
});}
void VoiceEngine::onVoiceState(const QJsonObject &state){return onRuntime([&]() -> void {
    if(!d->cameraUser.isEmpty() && state.value("guild_id").toString()==d->guildId && state.value("user_id").toString()==d->cameraUser
        && (state.value("channel_id").toString()!=d->channelId || (state.contains("self_video") && !state.value("self_video").toBool()))){
        d->cameraAnnouncements.remove(d->cameraUser);d->clearCamera(true);
    }
    const bool ownState=state.value("user_id").toString()==d->userId;
    const auto stateGuild=state.value("guild_id").toString();
    const auto stateChannel=state.value("channel_id").toString();
    if(ownState && (stateGuild==d->guildId || d->leavePendingGuilds.contains(stateGuild)))d->history.record(VoiceDiagnostics::Event::SelfState,VoiceDiagnostics::stage(d->stage),stateChannel.isEmpty()?0:1);
    if(ownState && stateChannel.isEmpty() && d->expiredLeaveGuilds.remove(stateGuild) &&
        (d->channelId.isEmpty() || (!d->receivedSelfState && d->stage=="awaiting_gateway_state")))return;
    if(ownState && d->leavePendingGuilds.contains(stateGuild) && stateChannel.isEmpty()){
        d->leavePendingGuilds.remove(stateGuild);
        if(d->awaitingLeave && stateGuild==d->guildId && !d->channelId.isEmpty()){d->requestJoin();return;}
    }
    if(d->channelId.isEmpty() || stateGuild!=d->guildId || !ownState || d->awaitingLeave)return;
    if(stateChannel.isEmpty()){d->fail("You were disconnected from the voice channel.",false,VoiceDiagnostics::Failure::RemoteDisconnect);return;}
    if(!d->receivedSelfState && d->stage=="awaiting_gateway_state" && d->supersededChannels.contains(stateChannel))return;
    if(stateChannel!=d->channelId){d->fail("Your voice channel changed elsewhere. Select the new channel to reconnect.",false,VoiceDiagnostics::Failure::ChannelChanged);return;}
    d->receivedSelfState=true;d->supersededChannels.clear();d->expiredLeaveGuilds.remove(stateGuild);
    d->serverMuted=state.value("mute").toBool();d->serverDeafened=state.value("deaf").toBool();
    if(d->serverMuted || d->serverDeafened)d->soundboardPcm.clear();
    if(d->serverDeafened)d->stopPlayback();else if(d->connected)d->startPlayback();d->updateCapture();
    const auto session=state.value("session_id").toString();if(!session.isEmpty())d->sessionId=session;d->openGateway();

});}

bool VoiceEngine::watchCamera(const QString &id){return onRuntime([&]() -> bool {
    if(!d->connected || !validUserId(id) || id==d->userId)return false;
    if(d->cameraUser!=id){d->cameraUser=id;d->clearCamera(false);}
    d->selectCamera();notifyGui(&VoiceEngine::cameraChanged);return d->cameraUser==id;

});}
void VoiceEngine::stopCamera(){return onRuntime([&]() -> void {if(d->cameraUser.isEmpty())return;d->clearCamera(true);
});}
void VoiceEngine::requestCameraKeyframe(){return onRuntime([&]() -> void {d->cameraKeyframe();
});}
QString VoiceEngine::cameraUserId()const{return onRuntime([&]() -> QString {return d->cameraUser;
});}
void VoiceEngine::onVoiceServer(const QJsonObject &server){return onRuntime([&]() -> void {
    if(d->channelId.isEmpty() || d->awaitingLeave || server.value("guild_id").toString()!=d->guildId)return;
    d->receivedServerUpdate=true;d->history.record(VoiceDiagnostics::Event::ServerUpdate,VoiceDiagnostics::stage(d->stage));
    const auto endpoint=server.value("endpoint").toString(),token=server.value("token").toString();
    if(!endpoint.isEmpty() && endpoint==d->endpoint && token==d->token){d->history.record(VoiceDiagnostics::Event::DuplicateServer,VoiceDiagnostics::stage(d->stage));return;}
    // An allocation change needs fresh UDP and MLS, not a gateway leave. The
    // call's session remains valid while Discord allocates a replacement.
    const auto session=d->sessionId;const bool muted=d->serverMuted,deafened=d->serverDeafened;
    if(d->transportReady || d->ws.state()!=QAbstractSocket::UnconnectedState || endpoint.isEmpty()){
        d->history.record(VoiceDiagnostics::Event::AllocationChanged,VoiceDiagnostics::stage(d->stage));
        d->reset();d->sessionId=session;d->serverMuted=muted;d->serverDeafened=deafened;d->diagnosticTimer.start();
    }
    if(endpoint.isEmpty()){d->setStage("awaiting_voice_server");d->setStatus("Waiting for voice server…");d->deadline.start(30000);return;}
    d->endpoint=endpoint;d->token=token;d->openGateway();
});}

void VoiceEngine::onVoiceRequestFailed(const QString &guild,const QString &channel,const QString &reason){return onRuntime([&]() -> void {
    if(channel.isEmpty()){d->leavePendingGuilds.remove(guild);d->expiredLeaveGuilds.remove(guild);}
    if(d->guildId==guild && (d->channelId==channel || (channel.isEmpty() && d->awaitingLeave)))d->fail(reason,false,VoiceDiagnostics::Failure::GatewayRequestRejected);
});}

void VoiceEngine::onAccountState(const QString &id){return onRuntime([&]() -> void {
    if(d->userId==id)return;
    ++d->accountGeneration;
    d->leavePendingGuilds.clear();d->expiredLeaveGuilds.clear();d->supersededChannels.clear();
    if(!d->channelId.isEmpty() || d->micTesting){
        d->reset();d->awaitingLeave=false;d->guildId.clear();d->channelId.clear();d->channelName.clear();d->error.clear();
        d->setStage("idle");d->setStatus("Not connected");
    }
    d->userId=id;
});}

QVariantMap VoiceEngine::playbackDiagnostics()const{return onRuntime([&]() -> QVariantMap {
    const auto &c=d->counters;
    const auto sinkQueued=std::max<qint64>(0,d->playbackBufferBytes()-d->playbackFreeBytes());
    return {{"receiveQueuedMs",d->receiveQueuedMs()},{"receiveMaxQueuedMs",c.receiveMaxQueuedMs},
        {"receiveConcealedPackets",qulonglong(c.receiveConcealedPackets)},{"receiveLatePackets",qulonglong(c.receiveLatePackets)},
        {"receiveRebases",qulonglong(c.receiveRebases)},{"receiveSkippedSlots",qulonglong(c.receiveSkippedSlots)},
        {"receiveDiscardedPackets",qulonglong(c.receiveDiscardedPackets)},{"receiveDiscardedSamples",qulonglong(c.receiveDiscardedSamples)},
        {"outputPendingMs",d->outputPending.size()*1000/(sampleRate*channels*2)},{"outputDeviceQueuedMs",sinkQueued*1000/(sampleRate*channels*2)},
        {"outputBufferMs",d->playbackBufferBytes()*1000/(sampleRate*channels*2)},
        {"outputState",d->sink?int(d->sink->state()):-1},{"outputError",d->sink?int(d->sink->error()):d->lastSinkError},
        {"audioLateWakes",qulonglong(c.audioLateWakes)},{"maxAudioLateMs",c.maxAudioLateMs},
        {"outputUnderruns",qulonglong(c.outputUnderruns)},{"outputDroppedFrames",qulonglong(c.outputDropped)}};
});}
QJsonObject VoiceEngine::diagnosticHistory()const{return onRuntime([&]() -> QJsonObject {return d->history.summary();});}
bool VoiceEngine::matchesAccount(const QString &id)const{return onRuntime([&]() -> bool {return !id.isEmpty() && d->userId==id;});}

QString VoiceEngine::stage()const{return onRuntime([&]() -> QString {return d->stage;
});}
bool VoiceEngine::encryptedReady()const{return onRuntime([&]() -> bool {return d->encryptedReady;
});}
QString VoiceEngine::channelId()const{return onRuntime([&]() -> QString {return d->channelId;
});}
QString VoiceEngine::currentGuildId()const{return onRuntime([&]() -> QString {return d->guildId;
});}
QString VoiceEngine::diagnosticSummary()const{return onRuntime([&]() -> QString {
    const auto &c=d->counters;
    return QString("Stage %1 · gateway %2/%3 (last %4) · group updates %5 · peers %6 · UDP %7 · audio received/sent/decoded %8/%9/%10 · transport/media errors %11/%12 · unknown streams %13 · close %14")
        .arg(d->stage).arg(c.gateway).arg(c.binary).arg(c.lastOpcode).arg(c.commits).arg(d->users.size()).arg(c.udpReceived)
        .arg(c.mediaReceived).arg(c.mediaSent).arg(c.decoded).arg(c.transportFailures).arg(c.mediaFailures).arg(c.unknownSsrc).arg(c.closeCode)
        + QString(" · RTP audio/other/RTCP %1/%2/%3 · mapped peak %4 · deaf drops %5 · output bytes/errors %6/%7")
            .arg(c.audioRtp).arg(c.otherRtp).arg(c.rtcp).arg(c.peakPeers).arg(c.deafDrops).arg(c.outputBytes).arg(c.sinkErrors)
        + QString(" · capture frames/dropped/catch-up %1/%2/%3 · queued/max frames %4/%5 · late wakes/max ms %6/%7 · output underruns/dropped frames %8/%9")
            .arg(c.captureSamples/samplesPerFrame).arg(c.captureDropped).arg(c.captureCatchups).arg(d->capture.size()/pcmBytes).arg(c.maxCaptureQueued)
            .arg(c.audioLateWakes).arg(c.maxAudioLateMs).arg(c.outputUnderruns).arg(c.outputDropped)
        + QString(" · receive queued/max ms %1/%2 · conceal/late/rebases %3/%4/%5 · skipped slots/discarded packets/samples %6/%7/%8")
            .arg(d->receiveQueuedMs()).arg(c.receiveMaxQueuedMs).arg(c.receiveConcealedPackets).arg(c.receiveLatePackets).arg(c.receiveRebases)
            .arg(c.receiveSkippedSlots).arg(c.receiveDiscardedPackets).arg(c.receiveDiscardedSamples)
        + QString(" · capture/encode/send errors %1/%2/%3 · last capture/output/test error %4/%5/%6")
            .arg(c.captureErrors).arg(c.encodeErrors).arg(c.writeErrors).arg(d->lastCaptureError).arg(d->lastSinkError).arg(d->lastMicTestSinkError)
        + QString(" · capture-ready wakes/max gap/work us %1/%2/%3 · test underruns/write errors %4/%5")
            .arg(c.captureReadyWakes).arg(c.maxCaptureReadyGapUs).arg(c.maxCaptureWorkUs).arg(c.micTestUnderruns).arg(c.micTestWriteErrors);

});}
bool VoiceEngine::configureLocalFixture(const QUrl &url){return onRuntime([&]() -> bool {
    if(!d->channelId.isEmpty() || d->micTesting || url.scheme()!="ws" || url.host()!="127.0.0.1" || url.port()<=0)return false;
    d->fixtureGateway=url;d->fixtureMode=true;return true;

});}
void VoiceEngine::expireFixtureDeadline(){return onRuntime([&]() -> void {if(d->fixtureMode)d->connectionDeadline();});}
void VoiceEngine::requestFixtureHeartbeat(){return onRuntime([&]() -> void {if(d->fixtureMode)d->heartbeatNow();});}

void VoiceEngine::queueFixtureAudio(const QByteArray &pcm){return onRuntime([&]() -> void {if(d->fixtureMode)d->acceptCapture(pcm.left(pcmBytes*64));
});}
void VoiceEngine::failFixtureAudio(const QString &reason){return onRuntime([&]() -> void {if(d->fixtureMode)d->audioError(reason);
});}
QByteArray VoiceEngine::takeFixtureAudio(){return onRuntime([&]() -> QByteArray {if(!d->fixtureMode)return {};auto out=std::move(d->fixtureOutput);d->fixtureOutput.clear();return out;
});}
QVariantMap VoiceEngine::fixtureCounters()const{return onRuntime([&]() -> QVariantMap {
    return {{"received",qulonglong(d->counters.mediaReceived)},{"sent",qulonglong(d->counters.mediaSent)},{"decoded",qulonglong(d->counters.decoded)},
        {"unknownSsrc",qulonglong(d->counters.unknownSsrc)},{"failures",qulonglong(d->counters.transportFailures+d->counters.mediaFailures)},
        {"micFrames",qulonglong(d->micTestFrames)},{"micQueuedBytes",d->capture.size()+d->micTestPending.size()},{"micTimerActive",d->micTestTick.isActive()},
        {"captureReadyWakes",qulonglong(d->counters.captureReadyWakes)},{"captureReadyMaxGapUs",d->counters.maxCaptureReadyGapUs},{"captureReadyMaxWorkUs",d->counters.maxCaptureWorkUs},
        {"micUnderruns",qulonglong(d->counters.micTestUnderruns)},{"micWriteErrors",qulonglong(d->counters.micTestWriteErrors)},
        {"captureFrames",qulonglong(d->counters.captureSamples/samplesPerFrame)},{"captureDropped",qulonglong(d->counters.captureDropped)},
        {"captureQueued",d->capture.size()/pcmBytes},{"captureMaxQueued",d->counters.maxCaptureQueued},{"captureCatchups",qulonglong(d->counters.captureCatchups)},
        {"soundboardQueued",d->soundboardPcm.size()/pcmBytes},{"receivedSoundboards",d->receivedSoundboards.size()},
        {"captureErrors",qulonglong(d->counters.captureErrors)},{"encodeErrors",qulonglong(d->counters.encodeErrors)},{"writeErrors",qulonglong(d->counters.writeErrors)},
        {"lastCaptureError",d->lastCaptureError},{"lastSinkError",d->lastSinkError},{"lastMicTestSinkError",d->lastMicTestSinkError},
        {"audioLateWakes",qulonglong(d->counters.audioLateWakes)},{"maxAudioLateMs",d->counters.maxAudioLateMs}};

});}
void VoiceEngine::queueFixturePeerAudio(const QString &id,quint32 ssrc,quint16 sequence,const QByteArray &opus){return onRuntime([&]() -> void {
    if(!d->fixtureMode || !validUserId(id) || opus.isEmpty() || opus.size()>1275)return;
    d->mapPeer(id,ssrc);const auto found=d->peers.find(ssrc);if(found==d->peers.end())return;
    auto &peer=*found->second;if(peer.queue.empty() && !peer.started)peer.next=sequence;
    if(peer.queue.size()<25){peer.queue[sequence]=opus;d->observeReceiveBacklog(peer);}

});}
void VoiceEngine::processFixturePlayback(){return onRuntime([&]() -> void {if(!d->fixtureMode)return;d->processAudioFrame(false,true);
});}

void VoiceEngine::processFixturePlaybackAt(qint64 elapsedMs){return onRuntime([&]() -> void {
    if(!d->fixtureMode || elapsedMs<0 || elapsedMs<d->fixturePlaybackElapsed || elapsedMs>60000)return;
    if(d->udp.state()==QAbstractSocket::UnconnectedState && !d->udp.bind(QHostAddress::LocalHost,quint16(0)))return;
    d->fixturePlaybackElapsed=elapsedMs;
    const bool wasConnected=d->connected;d->connected=true;d->processAudio();d->connected=wasConnected;
});}

void VoiceEngine::setFixtureAudioOutput(QIODevice *output,int bufferBytes){return onRuntime([&]() -> void {
    if(!d->fixtureMode || (output && (bufferBytes<=0 || bufferBytes>pcmBytes*captureCapacityFrames)))return;
    d->fixtureAudioOutput=output;d->fixtureOutputBufferBytes=output?bufferBytes:0;
});}
