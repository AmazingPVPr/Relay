#include "StreamEngine.h"
#include "StreamDecoder.h"
#include "VideoPacket.h"
#include "VoicePacket.h"
#include "BroadcastPacket.h"
#include "BroadcastPacer.h"
#include "BroadcastRepairBudget.h"
#include "H264Bitstream.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMediaDevices>
#include <QNetworkDatagram>
#include <QPointer>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QTimer>
#include <QUdpSocket>
#include <QVideoSink>
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
#include <deque>
#include <tuple>
#include <vector>
#ifdef RELAY_HAS_DAVE
#include <dave/dave.h>
#endif

namespace {
constexpr auto mode = "aead_xchacha20_poly1305_rtpsize";
constexpr int h264Pt = 105, h264RtxPt = 106;
const unsigned char *bytes(const QByteArray &value) { return reinterpret_cast<const unsigned char *>(value.constData()); }
unsigned char *bytes(QByteArray &value) { return reinterpret_cast<unsigned char *>(value.data()); }
bool validId(const QString &value) { bool ok = false; value.toULongLong(&ok); return ok && !value.isEmpty() && value.size() <= 22; }
QAudioFormat audioFormat() { QAudioFormat f; f.setSampleRate(48000); f.setChannelCount(2); f.setSampleFormat(QAudioFormat::Int16); return f; }
QAudioDevice outputDevice(const QString &id) {
    for (const auto &device : QMediaDevices::audioOutputs()) if (QString::fromLatin1(device.id().toBase64()) == id) return device;
    return QMediaDevices::defaultAudioOutput();
}
}

struct StreamEngine::Private {
    explicit Private(StreamEngine *owner) : q(owner), ws(QString(), QWebSocketProtocol::VersionLatest, owner), udp(owner), media(owner) { clock.start(); }
    StreamEngine *q;
    QWebSocket ws;
    QUdpSocket udp;
    QMediaDevices media;
    QTimer heartbeat, deadline, tick, feedback, keepAlive, diagnosticTimer,sendTick;
    QElapsedTimer clock;
    QString key, title, userId, sessionId, ownerId, rtcServerId, rtcChannelId, endpoint, token;
    QString status = "No stream selected", error, codec, stage = "idle", outputId;
    bool active = false, connected = false, encrypted = false, receiving = false, closing = false, discovery = false, heartbeatPending = false;
    bool audioMuted = false, deafened = false, fixtureMode = false, waitingForRestart = false;
    bool publishing=false,audioAnnounced=false;
    quint32 sendVideoSsrc=0,sendRtxSsrc=0,sendTimestamp=0,sendPackets=0,sendOctets=0,audioPackets=0,audioOctets=0,audioTimestamp=0;
    quint16 sendSequence=0,rtxSequence=0,audioSequence=0,transportSequence=0;
    // RTCP must describe the current media clock, not the timestamp of the
    // last packet that happened to leave the paced queue (RFC 3550, 6.4.1).
    quint32 videoClockBase=0,audioClockBase=0;
    qint64 videoClockAtUs=-1,audioClockAtUs=-1;
    QString sendRid="100";
    int sendWidth=1280,sendHeight=720,sendFps=30,sendBitrate=4000000,selectedBitrate=4000000;
    qint64 receiverWireBitrate=-1,lastBitrateChange=-2000;
    bool awaitingPublishKeyframe=false,bandwidthPaused=false;
    struct Outgoing {QByteArray payload;quint32 timestamp=0;quint16 sequence=0;bool marker=false,repaired=false;qint64 queuedAt=0;};
    std::deque<Outgoing> outgoing;
    std::deque<Outgoing> repairs;
    BroadcastPacer pacer;
    BroadcastRepairBudget repairBudget;
    std::set<quint16> pendingRepairs;
    struct Cached { Outgoing packet; qint64 firstSentAt=-1,lastRepairAt=-1; };
    std::map<quint16,Cached> cache;
    std::deque<quint16> cacheOrder;
    struct SentReport { quint32 source,compact; qint64 at; };
    std::deque<SentReport> sentReports;
    int repairRttMs=100,repairsSinceOriginal=0;
    qint64 lastPliAt=-1000;
    QUrl fixtureUrl;
    double volume = 1;
    quint32 ssrc = 0, selectedVideo = 0, rtcpNonce = 0x80000000u, keepAliveCounter = 0;
    int gatewaySequence = -1, width = 0, height = 0;
    quint64 frames = 0, decoderDrops = 0;
    qint64 lastFrameAt = 0;
    QHostAddress address;
    quint16 port = 0;
    QByteArray transportKey, audioPending, fixtureAudio;
    QPointer<QIODevice> fixtureAudioOutput;
    qint64 lastPollAtUs=-1;
    QPointer<QVideoSink> videoSink;
    std::unique_ptr<StreamDecoder> decoder;
    std::unique_ptr<QAudioSink> audioSink;
    QIODevice *playback = nullptr;
    bool audioSinkHasWritten=false;
    OpusDecoder *opus = nullptr;
    struct Audio {
        std::map<quint16,QByteArray> queue;
        quint16 next=0;bool started=false,primed=false;int misses=0,frameSamples=960;
        QElapsedTimer clock,primingClock,recoveryClock;qint64 renderedSamples=0,leadSamples=0;bool recoveryPending=false;
    } audio;
    struct Source {
        QString user;
        quint32 rtxSsrc = 0;
        std::unique_ptr<VideoPacket::Depacketizer> depacketizer;
        QString codec;
        std::map<quint16,VideoPacket::Rtp> pending;
        qsizetype pendingBytes = 0;
        quint16 next = 0, highest = 0;
        quint32 cycles = 0, base = 0, received = 0, lastExpected = 0, lastReceived = 0, lsr = 0;
        bool started = false;
        qint64 gapSince = 0, lastNack = -1000, lastPli = -10000, lastSr = 0;
    };
    std::map<quint32,Source> sources;
    std::map<quint32,QString> audioUsers;
    std::map<quint32,qint64> retiredAudioSources;
    qint64 lastAudioPacketAt=-1;
    std::map<quint32,quint32> rtxSources;
    std::set<QString> users;
    struct Counters {
        quint64 json=0,binary=0,udp=0,videoPackets=0,videoFrames=0,audioPackets=0,transportErrors=0,mediaErrors=0,decodedAudio=0,feedback=0,unknown=0,groups=0;
        quint64 audioRtp=0,audioMutedDrops=0,audioDeafenedDrops=0,audioKeyDrops=0,audioUnmappedDrops=0,audioDecoderDrops=0,audioRetiredDrops=0,audioAnnouncements=0,audioMappings=0;
        quint64 audioConcealed=0,audioLateSequences=0,audioQueueResets=0,audioUnderruns=0,audioLateWakes=0,audioDecodeErrors=0,audioOutputDrops=0;
        qint64 maxAudioLateMs=0;
        quint64 polls=0,pollLateWakes=0,sinkSubmittedFrames=0,audioSinkIdleEvents=0,audioRebases=0,audioRetiredSamples=0;
        qint64 lastPollGapUs=0,maxPollGapUs=0,lastPollWorkUs=0,maxPollWorkUs=0;
        quint64 txVideo=0,txRepair=0,txBytes=0,txAudio=0,pli=0,nack=0,repairMiss=0,queueDrops=0,rtcpReceived=0,rtcpInvalid=0,pings=0,pongs=0;
        quint64 repairRepeated=0,repairExpired=0,repairDeferred=0,repairUnsent=0,receiverReports=0,remb=0,twcc=0,maxRepairAge=0;
        quint64 lastRembBitrate=0,wants=0,bitrateChanges=0,congestionResets=0,congestionGated=0;
        int lastFractionLost=0,maxFractionLost=0,lastRttMs=0,lastWanted=-1;
        int lastOp=-1,close=0;
    } counters;
#ifdef RELAY_HAS_DAVE
    DAVESessionHandle session = nullptr;
    DAVEEncryptorHandle encryptor=nullptr;
    std::map<QString,DAVEDecryptorHandle> decryptors;
    std::map<quint16,quint16> transitions;
    quint16 preparedVersion = 0;
#endif
    void notify() { emit q->changed(); emit q->diagnosticsChanged(); }
    void setStatus(const QString &value, const QString &nextStage) { status=value; stage=nextStage; notify(); }
    void send(int op, const QJsonObject &data) {
        if (ws.state()==QAbstractSocket::ConnectedState)
            ws.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op",op},{"d",data}}).toJson(QJsonDocument::Compact)));
    }
    void sendBinary(int op,const QByteArray &data) { QByteArray message(1,char(op)); message+=data; ws.sendBinaryMessage(message); }
    void stopAudio() {
        if (audioSink) { audioSink->stop(); audioSink.reset(); }
        playback=nullptr;audioSinkHasWritten=false; audioPending.clear(); fixtureAudio.clear(); audio={};
    }
    void startAudio() {
        if (publishing || fixtureMode || audioMuted || deafened || audioSink || !encrypted) return;
        const auto device=outputDevice(outputId);
        if (device.isNull() || !device.isFormatSupported(audioFormat())) { error="Select an output that supports 48 kHz stereo audio."; notify(); return; }
        audioSink=std::make_unique<QAudioSink>(device,audioFormat()); audioSink->setBufferSize(3840*4); audioSink->setVolume(float(volume));
        QPointer<QAudioSink> expected(audioSink.get());
        QObject::connect(audioSink.get(),&QAudioSink::stateChanged,q,[this,expected](QAudio::State state) {
            if(!expected || audioSink.get()!=expected.data())return;
            if(state==QAudio::IdleState && audioSinkHasWritten)++counters.audioSinkIdleEvents;
            if (state==QAudio::StoppedState && expected->error()!=QAudio::NoError)
                QTimer::singleShot(0,q,[this,expected] { if(expected && audioSink.get()==expected.data()) { stopAudio(); error="Stream audio output stopped. Select the output again to retry."; notify(); } });
        });
        playback=audioSink->start();
        if (!playback) { audioSink.reset(); error="Could not open the stream audio output."; notify(); }
    }
    void reset() {
        closing=true; heartbeat.stop(); deadline.stop(); tick.stop(); feedback.stop(); keepAlive.stop(); diagnosticTimer.stop();sendTick.stop();
        ws.abort(); udp.close(); stopAudio(); decoder.reset();
        if (opus) opus_decoder_destroy(opus);
        opus=nullptr;
        if (videoSink) videoSink->setVideoFrame({});
        sources.clear(); audioUsers.clear(); retiredAudioSources.clear(); lastAudioPacketAt=-1; rtxSources.clear(); users.clear();
#ifdef RELAY_HAS_DAVE
        for (auto &[_,value]:decryptors) daveDecryptorDestroy(value);
        decryptors.clear(); transitions.clear(); preparedVersion=0;
        if(encryptor)daveEncryptorDestroy(encryptor);
        encryptor=nullptr;
        if (session) daveSessionDestroy(session);
        session=nullptr;
#endif
        if (!transportKey.isEmpty()) sodium_memzero(transportKey.data(),transportKey.size());
        transportKey.clear();
        token.fill(QChar(0)); token.clear(); endpoint.clear(); rtcServerId.clear(); rtcChannelId.clear();
        connected=encrypted=receiving=discovery=heartbeatPending=false; active=waitingForRestart=false;
        width=height=0; frames=decoderDrops=0;lastFrameAt=0;lastPollAtUs=-1;selectedVideo=0;gatewaySequence=-1;keepAliveCounter=0;rtcpNonce=0x80000000u;
        outgoing.clear();repairs.clear();pendingRepairs.clear();pacer.reset();repairBudget.reset();sentReports.clear();repairRttMs=100;repairsSinceOriginal=0;cache.clear();cacheOrder.clear();sendVideoSsrc=sendRtxSsrc=sendTimestamp=sendPackets=sendOctets=audioPackets=audioOctets=audioTimestamp=0;
        receiverWireBitrate=-1;lastBitrateChange=-2000;sendBitrate=selectedBitrate;awaitingPublishKeyframe=bandwidthPaused=false;
        audioAnnounced=false;lastPliAt=-1000;videoClockAtUs=audioClockAtUs=-1;videoClockBase=audioClockBase=0;
        closing=false;
    }
    void prepareWatch() {
        reset(); counters={}; error.clear(); codec.clear(); ownerId=key.section(':',3,3); active=true;
        addUser(ownerId); deadline.start(30000); diagnosticTimer.start();
        setStatus("Requesting stream…","awaiting_stream_server");
    }
    void fail(const QString &message) {
        const auto oldKey=key; reset(); key.clear(); title.clear(); error=message;
        setStatus("Stream disconnected","failed");
        if(!oldKey.isEmpty()) emit q->stopRequested(oldKey);
    }
    void heartbeatNow() {
        if (heartbeatPending) { fail("Stream heartbeat timed out. Open the stream again to reconnect."); return; }
        heartbeatPending=true; send(3,{{"t",double(QDateTime::currentMSecsSinceEpoch())},{"seq_ack",gatewaySequence}});
    }
    QJsonArray codecs() const {
        if(publishing)return {QJsonObject{{"name","opus"},{"type","audio"},{"priority",1000},{"payload_type",120},{"encode",true},{"decode",false}},
            QJsonObject{{"name","H264"},{"type","video"},{"priority",1000},{"payload_type",h264Pt},{"rtx_payload_type",h264RtxPt},{"encode",true},{"decode",false}}};
        QJsonArray list{QJsonObject{{"name","opus"},{"type","audio"},{"priority",1000},{"payload_type",120},{"encode",false},{"decode",true}}};
        for(const auto &video:VideoPacket::VideoCodecs) if(StreamDecoder::supports(video.name))
            list.append(QJsonObject{{"name",video.name},{"type","video"},{"priority",video.priority},
                {"payload_type",video.payloadType},{"rtx_payload_type",video.repairType},{"encode",false},{"decode",true}});
        return list;
    }
    void openGateway() {
        if (!active || userId.isEmpty() || sessionId.isEmpty() || rtcServerId.isEmpty() || rtcChannelId.isEmpty() || token.isEmpty() || endpoint.isEmpty() || ws.state()!=QAbstractSocket::UnconnectedState) return;
        QUrl url(endpoint.startsWith("wss://")?endpoint:"wss://"+endpoint);
        const auto host=url.host().toLower();
        if(url.scheme()!="wss" || !(host.endsWith(".discord.media") || host.endsWith(".discord.gg")) || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
            fail("Discord supplied an invalid stream endpoint."); return;
        }
        url.setPath("/");url.setQuery("v=9");if(fixtureMode)url=fixtureUrl;
        setStatus("Connecting to stream…","websocket");ws.open(url);deadline.start(30000);
    }
    void addUser(const QString &id) {
        if(id==userId || !validId(id) || users.size()>=512) return;
        if(!users.insert(id).second)return;
        if(publishing && connected && !encrypted){deadline.start(30000);setStatus("Securing screen sharing…","mls_exchange");}
#ifdef RELAY_HAS_DAVE
        auto handle=daveDecryptorCreate(); if(handle)decryptors[id]=handle;
        if(session && preparedVersion>0 && handle) {
            auto ratchet=daveSessionGetKeyRatchet(session,id.toUtf8().constData());
            if(ratchet) { daveDecryptorTransitionToKeyRatchet(handle,ratchet);daveKeyRatchetDestroy(ratchet); }
        }
#endif
    }
#ifdef RELAY_HAS_DAVE
    void sendKeyPackage() {
        uint8_t *data=nullptr;size_t size=0;daveSessionGetMarshalledKeyPackage(session,&data,&size);
        if(data && size)sendBinary(26,QByteArray(reinterpret_cast<char*>(data),qsizetype(size)));
        daveFree(data);
    }
    void initDave(quint16 version) {
        encrypted=false;stopAudio();deadline.start(30000);
        if(!version) { if(publishing)deadline.stop();setStatus(publishing?"Screen sharing ready · waiting for viewers":"Connected · waiting for stream encryption","waiting_for_encryption"); return; }
        if(version>daveMaxSupportedProtocolVersion() || !session) { fail("The stream did not negotiate supported end-to-end encryption.");return; }
        daveSessionInit(session,version,rtcServerId.toULongLong()-1,userId.toUtf8().constData());
        setStatus(publishing && users.empty()?"Screen sharing ready · waiting for viewers":"Securing the stream…","mls_exchange");sendKeyPackage();
        if(publishing && users.empty())deadline.stop();
    }
    void executeTransition(quint16 id) {
        const auto it=transitions.find(id);if(it==transitions.end())return;
        const auto version=it->second;transitions.erase(it);
        if(!version) { encrypted=false;preparedVersion=0;videoClockAtUs=audioClockAtUs=-1;daveSessionReset(session);stopAudio();outgoing.clear();repairs.clear();pendingRepairs.clear();pacer.reset();repairBudget.reset();cache.clear();cacheOrder.clear();sentReports.clear();repairRttMs=100;repairsSinceOriginal=0;setStatus(publishing?"Screen sharing ready · waiting for viewers":"Connected · waiting for stream encryption","waiting_for_encryption");if(publishing)deadline.stop();else deadline.start(30000);return; }
        // Peers prepare their receiving keys before acknowledging a transition.
        // The sending key becomes active only when the gateway executes it.
        if(publishing && encryptor){
            auto ratchet=daveSessionGetKeyRatchet(session,userId.toUtf8().constData());
            if(ratchet){daveEncryptorSetKeyRatchet(encryptor,ratchet);daveKeyRatchetDestroy(ratchet);}
        }
        encrypted=true;error.clear();deadline.stop();startAudio();
        setStatus(publishing?"Screen sharing · end-to-end encrypted":"Connected · waiting for video","encrypted");
        if(publishing)emit q->keyFrameRequested();
        if(selectedVideo)requestKeyframe(selectedVideo);
    }
    void prepareTransition(quint16 id,quint16 version) {
        if(version)for(const auto &user:users) {
            const auto decryptor=decryptors.find(user);if(decryptor==decryptors.end())continue;
            auto ratchet=daveSessionGetKeyRatchet(session,user.toUtf8().constData());
            if(ratchet){daveDecryptorTransitionToKeyRatchet(decryptor->second,ratchet);daveKeyRatchetDestroy(ratchet);}
        }
        preparedVersion=version;transitions[id]=version;
        if(!id)executeTransition(id);else send(23,{{"transition_id",int(id)}});
    }
    void binary(const QByteArray &message) {
        if(message.size()<3 || !session)return;
        gatewaySequence=qFromBigEndian<quint16>(message.constData());const int op=quint8(message[2]);counters.lastOp=op;++counters.binary;
        auto payload=message.mid(3);
        if(op==25){daveSessionSetExternalSender(session,bytes(payload),payload.size());return;}
        std::vector<QByteArray> ids{userId.toUtf8()};for(const auto &id:users)ids.push_back(id.toUtf8());
        std::vector<const char*> pointers;for(const auto &id:ids)pointers.push_back(id.constData());
        if(op==27){uint8_t *data=nullptr;size_t size=0;daveSessionProcessProposals(session,bytes(payload),payload.size(),pointers.data(),pointers.size(),&data,&size);if(data&&size)sendBinary(28,QByteArray(reinterpret_cast<char*>(data),size));daveFree(data);return;}
        if((op!=29 && op!=30)||payload.size()<2)return;
        const auto transition=qFromBigEndian<quint16>(payload.constData());payload.remove(0,2);bool success=false,ignored=false;
        if(op==29){auto result=daveSessionProcessCommit(session,bytes(payload),payload.size());if(result){ignored=daveCommitResultIsIgnored(result);success=!daveCommitResultIsFailed(result)&&!ignored;daveCommitResultDestroy(result);}}
        else{auto result=daveSessionProcessWelcome(session,bytes(payload),payload.size(),pointers.data(),pointers.size());success=result!=nullptr;if(result)daveWelcomeResultDestroy(result);}
        if(ignored)return;
        if(success){++counters.groups;prepareTransition(transition,daveSessionGetProtocolVersion(session));}
        else{send(31,{{"transition_id",int(transition)}});initDave(daveSessionGetProtocolVersion(session));}
    }
    QByteArray decryptMedia(const QString &user,const QByteArray &payload,DAVEMediaType type) {
        if(!encrypted)return {};
        const auto decoder=decryptors.find(user);if(decoder==decryptors.end())return {};
        QByteArray plain(payload.size(),Qt::Uninitialized);size_t length=0;
        if(daveDecryptorDecrypt(decoder->second,type,bytes(payload),payload.size(),bytes(plain),plain.size(),&length)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS){++counters.mediaErrors;return {};}
        plain.resize(length);return plain;
    }
#endif
    void text(const QString &message) {
        const auto doc=QJsonDocument::fromJson(message.toUtf8());if(!doc.isObject())return;
        const auto root=doc.object();if(root.contains("seq"))gatewaySequence=root.value("seq").toInt();
        const auto data=root.value("d").toObject();const int op=root.value("op").toInt(-1);++counters.json;counters.lastOp=op;
        switch(op) {
        case 8:{const int interval=data.value("heartbeat_interval").toInt();if(interval<1000){fail("Invalid stream heartbeat interval.");return;}heartbeat.start(std::min(interval,5000));heartbeatNow();break;}
        case 6:heartbeatPending=false;break;
        case 2:{
            ssrc=quint32(data.value("ssrc").toDouble());address=QHostAddress(data.value("ip").toString());port=quint16(data.value("port").toInt());
            if(publishing){
                int quality=-1;
                for(const auto &entry:data.value("streams").toArray()){
                    const auto stream=entry.toObject();const auto type=stream.value("type").toString();
                    if((type!="video" && type!="screen") || stream.value("quality").toInt()<quality || !stream.value("ssrc").toDouble())continue;
                    quality=stream.value("quality").toInt();sendVideoSsrc=quint32(stream.value("ssrc").toDouble());sendRtxSsrc=quint32(stream.value("rtx_ssrc").toDouble());sendRid=stream.value("rid").toString("100");
                }
                if(!sendVideoSsrc){fail("Discord did not allocate a screen-sharing video stream.");return;}
#ifdef RELAY_HAS_DAVE
                if(encryptor){daveEncryptorAssignSsrcToCodec(encryptor,sendVideoSsrc,DAVE_CODEC_H264);daveEncryptorAssignSsrcToCodec(encryptor,ssrc,DAVE_CODEC_OPUS);}
#endif
            }
            if(address.isNull() || !port || !data.value("modes").toArray().contains(QString(mode))){fail("The stream server does not support the native encrypted transport.");return;}
            udp.close();if(!udp.bind(address.protocol()==QAbstractSocket::IPv6Protocol?QHostAddress::AnyIPv6:QHostAddress::AnyIPv4,0)){fail("Could not open the stream UDP socket.");return;}
            udp.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,4*1024*1024);
            if(publishing)udp.setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,4*1024*1024);
            discovery=true;udp.writeDatagram(VoicePacket::discoveryRequest(ssrc),address,port);keepAlive.start(5000);deadline.start(15000);setStatus("Connecting video transport…","udp_discovery");break;
        }
        case 4:{
            if(connected){fail("The stream session changed. Open it again to reconnect.");return;}
            if(data.value("mode").toString()!=mode || data.value("secret_key").toArray().size()!=32){fail("Invalid stream transport encryption settings.");return;}
            transportKey.resize(32);const auto secret=data.value("secret_key").toArray();for(int i=0;i<32;++i)transportKey[i]=char(secret[i].toInt());
            codec=data.value("video_codec").toString().toUpper();
            if((publishing && codec!="H264") || (!codec.isEmpty() && !StreamDecoder::supports(codec))){fail("The stream selected an unsupported video codec.");return;}
            if(!publishing){int result=0;opus=opus_decoder_create(48000,2,&result);if(result!=OPUS_OK || !opus){fail("Could not initialize stream audio.");return;}}
            connected=true;send(5,{{"speaking",0},{"delay",0},{"ssrc",double(ssrc)}});send(15,{{"any",publishing?0:100}});
            if(publishing){announceVideo();sendTick.start(1);}
            // A 20 ms presentation poll limited a 60 fps source to at most 50
            // updates per second even when every packet arrived on time.
            tick.start(10);feedback.start(1000);
#ifdef RELAY_HAS_DAVE
            initDave(quint16(data.value("dave_protocol_version").toInt()));
#endif
            break;
        }
        case 5:{const auto id=data.value("user_id").toString();addUser(id);mapAudio(id,quint32(data.value("ssrc").toDouble()));break;}
        case 11:for(const auto &id:data.value("user_ids").toArray())addUser(id.toString());break;
        case 12:if(!publishing)mapVideo(data);break;
        case 13:{const auto id=data.value("user_id").toString();users.erase(id);
#ifdef RELAY_HAS_DAVE
            auto decryptor=decryptors.find(id);if(decryptor!=decryptors.end()){daveDecryptorDestroy(decryptor->second);decryptors.erase(decryptor);}
#endif
            if(id==ownerId){receiving=false;selectedVideo=0;sources.clear();rtxSources.clear();audioUsers.clear();retiredAudioSources.clear();stopAudio();setStatus("Waiting for the stream to resume…","waiting_for_video");}break;}
        case 14:{const auto changed=data.value("video_codec").toString().toUpper();if(!changed.isEmpty() && changed!=codec){if((publishing && changed!="H264") || !StreamDecoder::supports(changed)){fail("The stream switched to an unsupported video codec.");return;}codec=changed;decoder.reset();for(auto &[_,source]:sources){source.depacketizer.reset();source.pending.clear();source.pendingBytes=0;source.started=false;}notify();}break;}
        case 15:if(publishing){const auto wanted=data.contains(QString::number(sendVideoSsrc))?data.value(QString::number(sendVideoSsrc)):data.value("any");if(wanted.isDouble() && wanted.toDouble()==wanted.toInt(-1) && wanted.toInt()>=0 && wanted.toInt()<=100){++counters.wants;counters.lastWanted=wanted.toInt();}}break;
#ifdef RELAY_HAS_DAVE
        case 21:if(session)prepareTransition(quint16(data.value("transition_id").toInt()),quint16(data.value("protocol_version").toInt()));break;
        case 22:if(session)executeTransition(quint16(data.value("transition_id").toInt()));break;
        case 24:if(data.value("epoch").toVariant().toULongLong()==1)initDave(quint16(data.value("protocol_version").toInt()));break;
#endif
        default:break;
        }
    }
    void mapAudio(const QString &user,quint32 audioSsrc,bool announced=true) {
        if(user!=ownerId || !audioSsrc)return;
        if(announced)++counters.audioAnnouncements;
        if(!audioUsers.contains(audioSsrc)) {
            for(const auto &[source,_]:audioUsers)retiredAudioSources[source]=clock.elapsed();
            while(retiredAudioSources.size()>16)retiredAudioSources.erase(retiredAudioSources.begin());
            retiredAudioSources.erase(audioSsrc);
            audioUsers.clear();audioUsers[audioSsrc]=user;audio={};audioPending.clear();
            if(opus)opus_decoder_ctl(opus,OPUS_RESET_STATE);
            ++counters.audioMappings;
        }
        // Owner removal stops playback without necessarily changing the MLS
        // epoch. Its later announcement must reopen the output independently.
        startAudio();
    }
    void mapVideo(const QJsonObject &data) {
        const auto user=data.value("user_id").toString();addUser(user);if(user!=ownerId)return;
        mapAudio(user,quint32(data.value("audio_ssrc").toDouble()));
        std::tuple<int,qint64,int,qint64,bool> bestRank{-1,0,0,0,false};
        // Keep unspecified media (including audio) forwarded, while disabling
        // only the video layers whose assignments we actually know.
        quint32 best=0;QJsonObject wants{{"any",100}};
        for(const auto &[source,_]:sources)wants[QString::number(source)]=0;
        const auto streams=data.value("streams").toArray();
        for(const auto &value:streams) {
            const auto stream=value.toObject();const auto mediaSsrc=quint32(stream.value("ssrc").toDouble());
            const auto type=stream.value("type").toString();if(!type.isEmpty() && type!="video" && type!="screen")continue;
            if(!mediaSsrc || (!sources.contains(mediaSsrc) && sources.size()>=16))continue;
            auto &source=sources[mediaSsrc];source.user=user;source.rtxSsrc=quint32(stream.value("rtx_ssrc").toDouble());
            if(source.rtxSsrc)rtxSources[source.rtxSsrc]=mediaSsrc;
            wants[QString::number(mediaSsrc)]=0;
            bool numericRid=false;const int rid=stream.value("rid").toString().toInt(&numericRid);
            const int quality=stream.value("quality").toInt(numericRid?rid:100);
            const auto resolution=stream.value("max_resolution").toObject();
            const qint64 pixels=resolution.value("type").toString()=="source" ? std::numeric_limits<qint64>::max()
                : qint64(std::clamp(resolution.value("width").toInt(),0,16384))*std::clamp(resolution.value("height").toInt(),0,16384);
            const auto rank=std::tuple{quality,pixels,stream.value("max_framerate").toInt(),qint64(std::clamp(stream.value("max_bitrate").toDouble(),0.0,1000000000.0)),mediaSsrc==selectedVideo};
            if(stream.value("active").toBool(true) && rank>bestRank){bestRank=rank;best=mediaSsrc;}
        }
        // An explicit inactive streams list is authoritative. Falling back to
        // its top-level SSRC would request a stopped or low-quality rendition.
        if(!best && streams.isEmpty() && data.value("video_ssrc").toDouble()>0 && sources.size()<16) {
            best=quint32(data.value("video_ssrc").toDouble());auto &source=sources[best];source.user=user;
            source.rtxSsrc=quint32(data.value("rtx_ssrc").toDouble());if(source.rtxSsrc)rtxSources[source.rtxSsrc]=best;
        }
        if(best){
            const bool changed=selectedVideo!=best;
            if(changed){decoder.reset();receiving=false;decoderDrops=0;auto &source=sources[best];source.depacketizer.reset();source.pending.clear();source.pendingBytes=0;source.started=false;}
            selectedVideo=best;wants[QString::number(best)]=100;send(15,wants);if(changed || !receiving)requestKeyframe(best);
            // A sender's simulcast/RTX assignments can change while watching.
            // Retain only the selected rendition; its packet queue stays bounded.
            std::erase_if(sources,[best](const auto &entry){return entry.first!=best;});
            std::erase_if(rtxSources,[best](const auto &entry){return entry.second!=best;});
        } else {
            send(15,wants);selectedVideo=0;receiving=false;decoder.reset();decoderDrops=0;
            sources.clear();rtxSources.clear();if(videoSink)videoSink->setVideoFrame({});
            setStatus("Connected · waiting for video","waiting_for_video");
        }
    }
    void announceVideo(){
        if(!publishing || !connected || !sendVideoSsrc)return;
        send(12,{{"audio_ssrc",double(ssrc)},{"video_ssrc",double(sendVideoSsrc)},{"rtx_ssrc",double(sendRtxSsrc)},
            {"streams",QJsonArray{QJsonObject{{"type","screen"},{"rid",sendRid},{"quality",100},{"active",true},
                {"ssrc",double(sendVideoSsrc)},{"rtx_ssrc",double(sendRtxSsrc)},{"max_bitrate",sendBitrate},{"max_framerate",sendFps},
                {"max_resolution",QJsonObject{{"type","fixed"},{"width",sendWidth},{"height",sendHeight}}}}}}});
    }
    qint64 publishingWireBudget(bool audioActive) const {
        const qint64 configured=qint64(sendBitrate)*5/4+(audioActive?320000:0);
        return receiverWireBitrate>=0?std::min(configured,receiverWireBitrate):configured;
    }
    void discardCongestedVideo(){
        if(outgoing.empty())return;
        // Only the unsent tail is rewound. Its sequence numbers have never
        // appeared on the network; nonce and transport sequence never rewind.
        sendSequence=outgoing.front().sequence;
        outgoing.clear();repairs.clear();pendingRepairs.clear();cache.clear();cacheOrder.clear();
        repairsSinceOriginal=0;repairBudget.reset();
        awaitingPublishKeyframe=true;++counters.congestionResets;
        emit q->keyFrameRequested();
    }
    void applyReceiverBudget(quint64 estimate){
        receiverWireBitrate=qint64(estimate);
        const bool paused=receiverWireBitrate<945000;
        if(paused!=bandwidthPaused){
            bandwidthPaused=paused;
            if(paused){
                discardCongestedVideo();repairs.clear();pendingRepairs.clear();
                awaitingPublishKeyframe=true;pacer.reset();
            }else {
                // Pictures produced during the pause have not reached viewers.
                awaitingPublishKeyframe=true;emit q->keyFrameRequested();
            }
            emit q->diagnosticsChanged();
        }
        // Reserve audio, RTP/AEAD and bounded RTX inside the receiver's
        // session budget. A quality selection is a ceiling, not a fixed rate.
        const int desired=int(std::min<qint64>(selectedBitrate,
            std::max<qint64>(500000,(receiverWireBitrate-320000)*4/5)));
        const auto now=clock.elapsed();
        int next=desired;
        if(desired>sendBitrate){
            if(now-lastBitrateChange<2000)return;
            next=std::min(desired,sendBitrate+std::max(50000,sendBitrate/4));
        }
        if(next==sendBitrate)return;
        const bool reduced=next<sendBitrate;
        sendBitrate=next;lastBitrateChange=now;++counters.bitrateChanges;
        if(reduced){
            pacer.reset();
            qint64 queued=0;for(const auto &packet:outgoing)queued+=packet.payload.size()+96;
            if(queued>std::max<qint64>(1200,publishingWireBudget(audioAnnounced)/16))
                discardCongestedVideo();
        }
        announceVideo();
        emit q->publishingBitrateChanged(sendBitrate);
        emit q->diagnosticsChanged();
    }
    void sendPacketsNow(){
        if(!publishing || !encrypted || !connected)return;
        const auto now=clock.nsecsElapsed()/1000;
        pacer.updateRate(now,publishingWireBudget(audioAnnounced));repairBudget.update(now,sendBitrate);
        const auto rid=sendRid.toLatin1();
        const auto ridSize=rid.isEmpty()?3:std::min<qsizetype>(16,rid.size());
        const int extensionBytes=int((40+1+ridSize+3)/4*4);
        const auto wireSize=[extensionBytes](const Outgoing &packet){return int(packet.payload.size())+16+extensionBytes+20;};
        for(int count=0;count<BroadcastPacer::packetsPerWakeup;++count){
            while(!repairs.empty()){
                const auto &packet=repairs.front();const auto sequence=qFromBigEndian<quint16>(packet.payload.constData());const auto found=cache.find(sequence);
                if(found!=cache.end() && found->second.packet.timestamp==packet.timestamp && found->second.firstSentAt>=0 && now/1000-found->second.firstSentAt<=1500)break;
                pendingRepairs.erase(sequence);repairs.pop_front();++counters.repairExpired;
            }
            const bool repairReady=!repairs.empty() && repairBudget.ready(wireSize(repairs.front()));
            if(!repairs.empty() && !repairReady)++counters.repairDeferred;
            const bool useRepair=repairReady && (outgoing.empty() || repairsSinceOriginal<2) && pacer.ready(wireSize(repairs.front()));
            if(!useRepair && (outgoing.empty() || !pacer.ready(wireSize(outgoing.front()))))break;
            auto &queue=useRepair?repairs:outgoing;auto packet=std::move(queue.front());queue.pop_front();
            if(useRepair)repairsSinceOriginal=std::min(2,repairsSinceOriginal+1);else repairsSinceOriginal=0;
            if(packet.repaired)pendingRepairs.erase(qFromBigEndian<quint16>(packet.payload.constData()));
            if(rtcpNonce==std::numeric_limits<quint32>::max()){fail("Screen-sharing encryption key expired. Share again to reconnect.");return;}
            const auto wire=BroadcastPacket::encrypt(packet.payload,transportKey,packet.repaired?h264RtxPt:h264Pt,packet.marker,
                packet.sequence,packet.timestamp,packet.repaired?sendRtxSsrc:sendVideoSsrc,rtcpNonce++,transportSequence++,true,packet.repaired,rid);
            if(wire.isEmpty() || udp.writeDatagram(wire,address,port)!=wire.size()){++counters.transportErrors;continue;}
            pacer.sent(int(wire.size()));
            counters.txBytes+=wire.size();
            if(packet.repaired){
                ++counters.txRepair;repairBudget.sent(int(wire.size()));
                const auto found=cache.find(qFromBigEndian<quint16>(packet.payload.constData()));
                if(found!=cache.end()){found->second.lastRepairAt=now/1000;counters.maxRepairAge=std::max(counters.maxRepairAge,quint64(now/1000-found->second.firstSentAt));}
            }else{
                repairBudget.originalSent(now,int(wire.size()));
                const auto found=cache.find(packet.sequence);if(found!=cache.end() && found->second.packet.timestamp==packet.timestamp)found->second.firstSentAt=now/1000;
                ++counters.txVideo;++sendPackets;sendOctets+=packet.payload.size();sendTimestamp=packet.timestamp;if(packet.marker)++frames;
            }
        }
    }
    bool queueVideo(const QByteArray &plain,quint32 timestamp,int width,int height){
#ifdef RELAY_HAS_DAVE
        if(!publishing || !encrypted || !encryptor || !sendVideoSsrc || plain.isEmpty() || plain.size()>8*1024*1024)return false;
        if(bandwidthPaused){++counters.congestionGated;return false;}
        if(!outgoing.empty() && clock.elapsed()-outgoing.front().queuedAt>500)discardCongestedVideo();
        if(awaitingPublishKeyframe){
            bool idr=false;
            for(qsizetype i=0;i+3<plain.size();++i)
                if(plain[i]==0 && plain[i+1]==0 && plain[i+2]==1 && (quint8(plain[i+3])&31)==5){idr=true;break;}
            if(!idr){++counters.congestionGated;return false;}
        }
        const auto compatible = H264Bitstream::prepareForDave(plain);
        if(compatible.isEmpty()){++counters.mediaErrors;return false;}
        if(width!=sendWidth || height!=sendHeight){sendWidth=width;sendHeight=height;announceVideo();}
        QByteArray encryptedFrame(daveEncryptorGetMaxCiphertextByteSize(encryptor,DAVE_MEDIA_TYPE_VIDEO,compatible.size()),Qt::Uninitialized);size_t length=0;
        if(daveEncryptorEncrypt(encryptor,DAVE_MEDIA_TYPE_VIDEO,sendVideoSsrc,bytes(compatible),compatible.size(),bytes(encryptedFrame),encryptedFrame.size(),&length)!=DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS){++counters.mediaErrors;return false;}
        encryptedFrame.resize(length);const auto fragments=BroadcastPacket::h264Payloads(encryptedFrame);
        if(fragments.isEmpty() || fragments.size()>4096)return false;
        if(outgoing.size()+repairs.size()+size_t(fragments.size())>4096){++counters.queueDrops;discardCongestedVideo();return false;}
        awaitingPublishKeyframe=false;
        for(qsizetype i=0;i<fragments.size();++i){
            Outgoing packet{fragments[i],timestamp,sendSequence++,i+1==fragments.size(),false,clock.elapsed()};outgoing.push_back(packet);
            if(sendRtxSsrc){cache[packet.sequence]=Cached{packet};cacheOrder.push_back(packet.sequence);while(cacheOrder.size()>4096){cache.erase(cacheOrder.front());cacheOrder.pop_front();}}
        }
        if(videoClockAtUs<0){videoClockBase=timestamp;videoClockAtUs=clock.nsecsElapsed()/1000;}
        ++counters.videoFrames;return true;
#else
        Q_UNUSED(plain);Q_UNUSED(timestamp);Q_UNUSED(width);Q_UNUSED(height);return false;
#endif
    }
    bool sendAudioFrame(const QByteArray &plain,quint32 timestamp){
#ifdef RELAY_HAS_DAVE
        if(!publishing || !encrypted || !encryptor || plain.isEmpty() || plain.size()>1275)return false;
        if(receiverWireBitrate>=0 && receiverWireBitrate<320000)return false;
        QByteArray encryptedFrame(daveEncryptorGetMaxCiphertextByteSize(encryptor,DAVE_MEDIA_TYPE_AUDIO,plain.size()),Qt::Uninitialized);size_t length=0;
        if(daveEncryptorEncrypt(encryptor,DAVE_MEDIA_TYPE_AUDIO,ssrc,bytes(plain),plain.size(),bytes(encryptedFrame),encryptedFrame.size(),&length)!=DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS){++counters.mediaErrors;return false;}
        encryptedFrame.resize(length);
        if(rtcpNonce==std::numeric_limits<quint32>::max()){fail("Screen-sharing encryption key expired. Share again to reconnect.");return false;}
        if(!audioAnnounced){audioAnnounced=true;send(5,{{"speaking",2},{"delay",0},{"ssrc",double(ssrc)}});}
        const auto wire=BroadcastPacket::encrypt(encryptedFrame,transportKey,120,false,audioSequence++,timestamp,ssrc,rtcpNonce++,0,false);
        if(wire.isEmpty() || udp.writeDatagram(wire,address,port)!=wire.size()){++counters.transportErrors;return false;}
        pacer.updateRate(clock.nsecsElapsed()/1000,publishingWireBudget(true));pacer.sent(int(wire.size()));
        counters.txBytes+=wire.size();
        if(audioClockAtUs<0){audioClockBase=timestamp;audioClockAtUs=clock.nsecsElapsed()/1000;}
        ++counters.txAudio;++audioPackets;audioOctets+=encryptedFrame.size();audioTimestamp=timestamp;return true;
#else
        Q_UNUSED(plain);Q_UNUSED(timestamp);return false;
#endif
    }
    void resend(quint16 sequence){
        // Repeated feedback before a queued repair is sent must not amplify
        // loss into an ever-growing priority queue of the same packet.
        if(pendingRepairs.contains(sequence)){++counters.repairRepeated;return;}
        const auto found=cache.find(sequence);if(!sendRtxSsrc || found==cache.end()){++counters.repairMiss;return;}
        const auto now=clock.elapsed();const auto &stored=found->second;
        if(stored.firstSentAt<0){++counters.repairUnsent;return;}
        if(now-stored.firstSentAt>1500){++counters.repairExpired;return;}
        if(stored.lastRepairAt>=0 && now-stored.lastRepairAt<std::clamp(repairRttMs,20,500)){++counters.repairRepeated;return;}
        if(repairs.size()>=512 || outgoing.size()+repairs.size()>=4096){++counters.repairDeferred;return;}
        auto packet=stored.packet;QByteArray repaired(2,'\0');qToBigEndian(sequence,repaired.data());repaired+=packet.payload;
        packet.payload=repaired;packet.sequence=rtxSequence++;packet.repaired=true;repairs.push_back(std::move(packet));pendingRepairs.insert(sequence);
    }
    void senderReport(quint32 source,quint32 timestamp,quint32 packets,quint32 octets){
        if(!packets)return;
        const quint64 now=quint64(QDateTime::currentMSecsSinceEpoch());
        QByteArray report(28,'\0');report[0]=char(0x80);report[1]=char(200);qToBigEndian<quint16>(6,report.data()+2);
        qToBigEndian(source,report.data()+4);qToBigEndian(quint32(now/1000+2208988800ull),report.data()+8);
        qToBigEndian(quint32(((now%1000)<<32)/1000),report.data()+12);qToBigEndian(timestamp,report.data()+16);
        qToBigEndian(packets,report.data()+20);qToBigEndian(octets,report.data()+24);
        const auto before=counters.feedback;sendRtcp(report);
        if(counters.feedback!=before){sentReports.push_back({source,(qFromBigEndian<quint32>(report.constData()+8)<<16)|(qFromBigEndian<quint32>(report.constData()+12)>>16),clock.elapsed()});while(sentReports.size()>16)sentReports.pop_front();}
    }
    void sendRtcp(const QByteArray &plain) {
        if(!connected || transportKey.size()!=32)return;
        if(rtcpNonce==std::numeric_limits<quint32>::max()){fail("Stream transport key expired. Open the stream again.");return;}
        const auto packet=VideoPacket::encryptRtcp(plain,transportKey,rtcpNonce++);
        if(!packet.isEmpty() && udp.writeDatagram(packet,address,port)==packet.size())++counters.feedback;
    }
    void requestKeyframe(quint32 sourceSsrc) {
        auto it=sources.find(sourceSsrc);if(it==sources.end())return;
        auto &source=it->second;const auto now=clock.elapsed();if(now-source.lastPli<800)return;source.lastPli=now;
        QByteArray packet(12,'\0');packet[0]=char(0x81);packet[1]=char(206);qToBigEndian<quint16>(2,packet.data()+2);
        qToBigEndian(ssrc,packet.data()+4);qToBigEndian(sourceSsrc,packet.data()+8);sendRtcp(packet);
    }
    void requestMissing(quint32 sourceSsrc,quint16 sequence) {
        auto &source=sources[sourceSsrc];const auto now=clock.elapsed();if(now-source.lastNack<30)return;source.lastNack=now;
        QByteArray packet(16,'\0');packet[0]=char(0x81);packet[1]=char(205);qToBigEndian<quint16>(3,packet.data()+2);
        qToBigEndian(ssrc,packet.data()+4);qToBigEndian(sourceSsrc,packet.data()+8);qToBigEndian(sequence,packet.data()+12);sendRtcp(packet);
    }
    void receiverReports() {
        if(publishing){
            const auto now=clock.nsecsElapsed()/1000;
            // The first accepted frame supplies each source's random offset.
            // Keep this anchor through packet queues, idle capture and source
            // recovery so reports retain a stable 90/48 kHz clock relationship.
            if(videoClockAtUs>=0)senderReport(sendVideoSsrc,videoClockBase+quint32(std::max<qint64>(0,now-videoClockAtUs)*90/1000),sendPackets,sendOctets);
            if(audioClockAtUs>=0)senderReport(ssrc,audioClockBase+quint32(std::max<qint64>(0,now-audioClockAtUs)*48/1000),audioPackets,audioOctets);
            return;
        }
        for(auto &[sourceSsrc,source]:sources) {
            if(sourceSsrc!=selectedVideo || !source.started)continue;
            const quint32 highest=source.cycles+source.highest,expected=highest-source.base+1;
            const qint64 lost=qint64(expected)-source.received;
            const quint32 intervalExpected=expected-source.lastExpected,intervalReceived=source.received-source.lastReceived;
            const int intervalLost=int(intervalExpected)-int(intervalReceived);
            const int fraction=intervalExpected && intervalLost>0?std::min(255,int((quint64(intervalLost)*256)/intervalExpected)):0;
            source.lastExpected=expected;source.lastReceived=source.received;
            QByteArray packet(32,'\0');packet[0]=char(0x81);packet[1]=char(201);qToBigEndian<quint16>(7,packet.data()+2);
            qToBigEndian(ssrc,packet.data()+4);qToBigEndian(sourceSsrc,packet.data()+8);
            const quint32 loss=(quint32(fraction)<<24)|(quint32(std::clamp<qint64>(lost,0,0x7fffff))&0xffffff);qToBigEndian(loss,packet.data()+12);
            qToBigEndian(highest,packet.data()+16);qToBigEndian(source.lsr,packet.data()+24);
            if(source.lsr)qToBigEndian(quint32((clock.elapsed()-source.lastSr)*65536/1000),packet.data()+28);
            sendRtcp(packet);if(!receiving)requestKeyframe(sourceSsrc);
        }
    }
    void handleRtcp(const QByteArray &packet) {
        const auto plain=VideoPacket::decryptRtcp(packet,transportKey);if(!plain){++counters.rtcpInvalid;return;}
        ++counters.rtcpReceived;
        qsizetype offset=0;
        while(offset+8<=plain->size()) {
            const auto *p=plain->constData()+offset;const qsizetype length=(qFromBigEndian<quint16>(p+2)+1)*4;
            if(length<8 || offset+length>plain->size())break;
            if(publishing && length>=12){
                const quint8 type=quint8(p[1]),format=quint8(p[0])&31;
                const auto media=qFromBigEndian<quint32>(p+8);
                if(type==206 && (format==1 || format==4) && (media==sendVideoSsrc || media==0)){
                    ++counters.pli;
                    if(clock.elapsed()-lastPliAt>=100){lastPliAt=clock.elapsed();emit q->keyFrameRequested();}
                }
                if(type==205 && format==1 && media==sendVideoSsrc){
                    ++counters.nack;
                    int requests=0;for(qsizetype i=12;i+4<=length && requests<64;i+=4){
                        const auto sequence=qFromBigEndian<quint16>(p+i),mask=qFromBigEndian<quint16>(p+i+2);resend(sequence);++requests;
                        for(int bit=0;bit<16 && requests<64;++bit)if(mask&(1<<bit)){resend(quint16(sequence+bit+1));++requests;}
                    }
                }
                if(type==205 && format==15 && length>=20 && (media==sendVideoSsrc || media==0))++counters.twcc;
                if(type==206 && format==15 && length>=20 && std::memcmp(p+12,"REMB",4)==0){
                    const int sourcesCount=quint8(p[16]);bool relevant=false;
                    if(20+sourcesCount*4<=length)for(int i=0;i<sourcesCount;++i)if(qFromBigEndian<quint32>(p+20+i*4)==sendVideoSsrc)relevant=true;
                    const int exponent=quint8(p[17])>>2;const quint64 mantissa=(quint64(quint8(p[17])&3)<<16)|(quint64(quint8(p[18]))<<8)|quint8(p[19]);
                    if(relevant && (media==0 || media==sendVideoSsrc) && exponent<=30 && (mantissa<<exponent)<=1000000000){
                        ++counters.remb;counters.lastRembBitrate=mantissa<<exponent;
                        applyReceiverBudget(counters.lastRembBitrate);
                    }
                }
            }
            if(publishing && (quint8(p[1])==200 || quint8(p[1])==201)){
                const qsizetype first=quint8(p[1])==200?28:8;const int count=quint8(p[0])&31;
                for(int report=0;report<count && first+(report+1)*24<=length;++report){
                    const auto *block=p+first+report*24;if(qFromBigEndian<quint32>(block)!=sendVideoSsrc)continue;
                    ++counters.receiverReports;counters.lastFractionLost=quint8(block[4]);counters.maxFractionLost=std::max(counters.maxFractionLost,counters.lastFractionLost);
                    const auto lsr=qFromBigEndian<quint32>(block+16),delay=qFromBigEndian<quint32>(block+20);
                    if(!lsr)continue;
                    const auto stamp=std::find_if(sentReports.rbegin(),sentReports.rend(),[&](const SentReport &value){return value.source==sendVideoSsrc && value.compact==lsr;});
                    if(stamp==sentReports.rend())continue;
                    const auto elapsed=clock.elapsed()-stamp->at;const qint64 rtt=elapsed-qint64(delay)*1000/65536;
                    if(elapsed>=0 && elapsed<=20000 && rtt>=0 && rtt<=5000){counters.lastRttMs=int(rtt);repairRttMs=std::clamp(int(rtt),20,500);}
                }
            }
            if(quint8(p[1])==200 && length>=28) {
                const auto id=qFromBigEndian<quint32>(p+4);auto it=sources.find(id);
                if(it!=sources.end()){it->second.lsr=(qFromBigEndian<quint32>(p+8)<<16)|(qFromBigEndian<quint32>(p+12)>>16);it->second.lastSr=clock.elapsed();}
            }
            offset+=length;
        }
    }
    void receive() {
        int processed=0;
        while(udp.hasPendingDatagrams() && ++processed<=512) {
            const auto datagram=udp.receiveDatagram();if(datagram.senderAddress()!=address || datagram.senderPort()!=port)continue;
            const auto packet=datagram.data();
            if(discovery){const auto result=VoicePacket::parseDiscovery(packet,ssrc);if(!result)continue;discovery=false;
                send(1,{{"protocol","udp"},{"data",QJsonObject{{"address",result->address},{"port",int(result->port)},{"mode",mode}}},{"codecs",codecs()}});
                setStatus("Negotiating stream encryption…","session_description");continue;}
            ++counters.udp;
            if(const auto pong=VoicePacket::parseNativePong(packet)){
                if(keepAliveCounter && *pong==keepAliveCounter-1)++counters.pongs;
                continue;
            }
            if(!connected || packet.size()<=12)continue;
            const auto type=quint8(packet[1]);if(type>=192 && type<=223){handleRtcp(packet);continue;}
            if(publishing)continue;
            auto frame=VideoPacket::decryptRtp(packet,transportKey);if(!frame){++counters.transportErrors;continue;}
            if(frame->payloadType==120){receiveAudio(*frame);continue;}
            const auto *video=VideoPacket::videoCodec(frame->payloadType);
            if(!video || !StreamDecoder::supports(video->name))continue;
            const bool rtx=frame->payloadType==video->repairType;
            if(rtx){const auto found=rtxSources.find(frame->ssrc);if(found==rtxSources.end() || frame->payload.size()<2)continue;frame->ssrc=found->second;frame->sequence=qFromBigEndian<quint16>(frame->payload.constData());frame->payload.remove(0,2);frame->payloadType=video->payloadType;}
            if(frame->ssrc!=selectedVideo){++counters.unknown;continue;}
            receiveVideo(std::move(*frame));
        }
        if(udp.hasPendingDatagrams())QTimer::singleShot(0,q,[this]{receive();});
    }
    void receiveAudio(const VideoPacket::Rtp &frame) {
        ++counters.audioRtp;
        if(audioMuted){++counters.audioMutedDrops;return;}
        if(deafened){++counters.audioDeafenedDrops;return;}
        if(!encrypted){++counters.audioKeyDrops;return;}
        if(!opus){++counters.audioDecoderDrops;return;}
        const auto retired=retiredAudioSources.find(frame.ssrc);
        if(retired!=retiredAudioSources.end() && (clock.elapsed()-retired->second<2000 ||
            (lastAudioPacketAt>=0 && clock.elapsed()-lastAudioPacketAt<2000))){++counters.audioRetiredDrops;return;}
#ifdef RELAY_HAS_DAVE
        if(!decryptors.contains(ownerId)){++counters.audioKeyDrops;return;}
        // A newly watched sender may not repeat its speaking/SSRC event before
        // audio starts. Authenticate with the selected owner's AUDIO ratchet
        // before allowing an otherwise unknown source to establish its mapping.
        auto plain=decryptMedia(ownerId,frame.payload,DAVE_MEDIA_TYPE_AUDIO);
        if(plain.isEmpty()){if(!audioUsers.contains(frame.ssrc))++counters.audioUnmappedDrops;return;}
        if(!audioUsers.contains(frame.ssrc))mapAudio(ownerId,frame.ssrc,false);
        ++counters.audioPackets;if(!audio.started && audio.queue.empty())audio.next=frame.sequence;
        const auto distance=qint16(frame.sequence-audio.next);if(distance<0){++counters.audioLateSequences;return;}
        if(distance>50 || audio.queue.size()>64){++counters.audioQueueResets;audio={};audio.next=frame.sequence;}
        if(!audio.started && !audio.primingClock.isValid())audio.primingClock.start();
        audio.queue[frame.sequence]=plain;
        lastAudioPacketAt=clock.elapsed();
#else
        Q_UNUSED(frame);
#endif
    }
    void receiveVideo(VideoPacket::Rtp frame) {
        auto it=sources.find(frame.ssrc);if(it==sources.end())return;auto &source=it->second;++counters.videoPackets;
        const auto *video=VideoPacket::videoCodec(frame.payloadType);if(!video)return;
        const QString name=QString::fromLatin1(video->name);
        if(source.codec!=name || !source.depacketizer){source.codec=name;source.depacketizer=std::make_unique<VideoPacket::Depacketizer>(video->depacketizer);source.pending.clear();source.pendingBytes=0;source.started=false;}
        if(!source.started){source.started=true;source.next=frame.sequence;source.highest=frame.sequence;source.base=frame.sequence;}
        const auto distance=qint16(frame.sequence-source.next);if(distance<0)return;
        // A single missing packet in a high-bitrate/4K keyframe can leave many
        // hundreds of otherwise valid packets waiting for its RTX repair.
        if(distance>4096 || source.pending.size()>=4096 || source.pendingBytes+frame.payload.size()>16*1024*1024){
            source.pending.clear();source.pendingBytes=0;source.depacketizer->reset();source.next=frame.sequence;
            if(decoder)decoder->discardUntilKeyFrame();
            requestKeyframe(frame.ssrc);
        }
        if(source.pending.contains(frame.sequence))return;
        if(qint16(frame.sequence-source.highest)>0){if(frame.sequence<source.highest)source.cycles+=65536;source.highest=frame.sequence;}
        ++source.received;const auto id=frame.ssrc;source.pendingBytes+=frame.payload.size();source.pending.emplace(frame.sequence,std::move(frame));drainVideo(id,source);
    }
    void drainVideo(quint32 sourceSsrc,Source &source) {
        while(!source.pending.empty()) {
            auto it=source.pending.find(source.next);
            if(it==source.pending.end()) {
                const auto now=clock.elapsed();if(!source.gapSince)source.gapSince=now;
                requestMissing(sourceSsrc,source.next);
                if(now-source.gapSince<90)return;
                auto best=source.pending.begin();for(auto found=source.pending.begin();found!=source.pending.end();++found)
                    if(quint16(found->first-source.next)<quint16(best->first-source.next))best=found;
                source.next=best->first;source.depacketizer->reset();if(decoder)decoder->discardUntilKeyFrame();requestKeyframe(sourceSsrc);source.gapSince=0;continue;
            }
            auto packet=std::move(it->second);source.pendingBytes-=packet.payload.size();source.pending.erase(it);++source.next;source.gapSince=0;
            auto complete=source.depacketizer->push(packet.payload,packet.marker,packet.sequence,packet.timestamp);if(!complete)continue;
            ++counters.videoFrames;
#ifdef RELAY_HAS_DAVE
            auto plain=decryptMedia(source.user,*complete,DAVE_MEDIA_TYPE_VIDEO);if(plain.isEmpty())continue;
            if(!decoder || codec!=source.codec){codec=source.codec;decoder=std::make_unique<StreamDecoder>(codec);decoder->discardUntilKeyFrame();notify();}
            decoder->submit(plain,packet.timestamp);
#endif
        }
    }
    void processAudio() {
        if(audioMuted || deafened || !opus || (!fixtureMode && !playback))return;
        // Video presentation polls faster than audio arrives. Only the sample
        // clock may advance the jitter buffer, including concealment of losses.
        auto decodeOne=[this](bool conceal) {
            auto it=audio.queue.find(audio.next);QByteArray frame;
            if(it!=audio.queue.end()){frame=it->second;audio.queue.erase(it);audio.misses=0;}
            else {
                // A busy GUI may deliver the timer before a socket notification.
                if(!conceal || udp.hasPendingDatagrams())return false;
                // The target fill runs ahead of playback. Leave that buffered
                // safety available for network jitter before declaring a loss.
                if(audio.primed && audio.clock.isValid() && audio.clock.nsecsElapsed()/1000*48/1000+960<audio.renderedSamples)return false;
                if(++audio.misses>5){++counters.audioUnderruns;audio.started=audio.primed=audio.recoveryPending=false;audio.clock.invalidate();audio.primingClock.invalidate();audio.recoveryClock.invalidate();audio.queue.clear();return false;}
                ++counters.audioConcealed;
            }
            ++audio.next;std::array<opus_int16,5760*2> pcm;
            const int count=opus_decode(opus,frame.isEmpty()?nullptr:bytes(frame),frame.size(),pcm.data(),frame.isEmpty()?audio.frameSamples:5760,0);
            if(count>0){
                audioPending.append(reinterpret_cast<const char*>(pcm.data()),count*4);audio.renderedSamples+=count;
                if(!frame.isEmpty()){++counters.decodedAudio;audio.frameSamples=count;}
            }else{++counters.audioDecodeErrors;audio.renderedSamples+=audio.frameSamples;}
            return true;
        };
        // A service pause longer than the complete normal output budget can
        // leave old PCM and encoded packets permanently behind real time. Only
        // retire that history after a measured stall; ordinary jitter keeps its
        // existing prefill and byte-exact decode path.
        if(audio.started && audio.primed && !audio.recoveryPending && counters.lastPollGapUs>=200000
            && queuedAudioSamples()+audioPending.size()/4>48000/5){audio.recoveryPending=true;audio.recoveryClock.start();}
        if(audio.recoveryPending){
            // A bounded receive turn can stop in the middle of a network burst.
            // Do not mistake that temporary prefix for the newest available tail.
            if(udp.hasPendingDatagrams())return;
            std::vector<quint16> sequences;sequences.reserve(audio.queue.size());
            for(const auto &[sequence,_]:audio.queue)sequences.push_back(sequence);
            std::sort(sequences.begin(),sequences.end(),[this](auto a,auto b){return quint16(a-audio.next)<quint16(b-audio.next);});
            quint16 retain=audio.next;qint64 tailSamples=0;int lastSamples=audio.frameSamples;bool ready=false;
            for(qsizetype at=qsizetype(sequences.size())-1;at>=0;--at){
                if(at+1<qsizetype(sequences.size()) && quint16(sequences[size_t(at)]+1)!=sequences[size_t(at+1)])break;
                retain=sequences[size_t(at)];const auto &packet=audio.queue.at(retain);
                const int samples=opus_packet_get_nb_samples(bytes(packet),packet.size(),48000);
                if(at==qsizetype(sequences.size())-1)lastSamples=samples>0?samples:audio.frameSamples;
                tailSamples+=samples>0?samples:audio.frameSamples;
                if(tailSamples>=2880 && tailSamples-lastSamples>=1920){ready=true;break;}
            }
            // Give an incomplete fresh suffix the same 120ms reorder window as
            // ordinary startup; keep that suffix even if it needs startup PLC.
            if(!ready && audio.recoveryClock.elapsed()<120)return;
            const auto retire=quint16(retain-audio.next);if(retire>64)return;
            counters.audioRetiredSamples+=audioPending.size()/4;audioPending.clear();
            for(int step=0;step<retire;++step){
                auto found=audio.queue.find(audio.next);QByteArray packet;
                if(found!=audio.queue.end()){packet=std::move(found->second);audio.queue.erase(found);}else ++counters.audioConcealed;
                ++audio.next;std::array<opus_int16,5760*2> discarded;
                const int samples=opus_decode(opus,packet.isEmpty()?nullptr:bytes(packet),packet.size(),discarded.data(),packet.isEmpty()?audio.frameSamples:5760,0);
                if(samples>0){counters.audioRetiredSamples+=samples;if(!packet.isEmpty()){++counters.decodedAudio;audio.frameSamples=samples;}}
                else ++counters.audioDecodeErrors;
            }
            // Preserve the predictor through old missing slots, never allowing
            // the ordinary underrun path to clear the retained fresh suffix.
            audio.started=audio.primed=audio.recoveryPending=false;audio.clock.invalidate();audio.renderedSamples=audio.leadSamples=0;
            audio.misses=0;audio.primingClock=audio.recoveryClock;if(audio.queue.empty())audio.primingClock.invalidate();audio.recoveryClock.invalidate();++counters.audioRebases;
        }
        if(!audio.started) {
            int queuedSamples=0,lastSamples=audio.frameSamples;quint16 next=audio.next;
            for(size_t i=0;i<audio.queue.size();++i,++next) {
                const auto found=audio.queue.find(next);if(found==audio.queue.end())break;
                const auto &packet=found->second;
                const int count=opus_packet_get_nb_samples(bytes(packet),packet.size(),48000);
                lastSamples=count>0?count:audio.frameSamples;queuedSamples+=lastSamples;
                if(queuedSamples>=2880 && queuedSamples-lastSamples>=1920)break;
            }
            const bool contiguous=queuedSamples>=2880 && queuedSamples-lastSamples>=1920;
            const bool expired=audio.primingClock.isValid() && audio.primingClock.elapsed()>=120;
            if(contiguous || (expired && !audio.queue.empty())) {
                audio.started=true;audio.primed=false;audio.misses=0;audio.renderedSamples=0;
            }
        }
        if(audio.started && !audio.primed) {
            // Keep the first PCM private until the full initial fill exists.
            // A missing startup packet gets a bounded reorder window before PLC.
            const bool conceal=audio.primingClock.isValid() && audio.primingClock.elapsed()>=120;
            for(int i=0;i<64 && (audioPending.size()<2880*4 || audio.renderedSamples-audio.frameSamples<1920);++i)
                if(!decodeOne(conceal))break;
            if(audio.started && audioPending.size()>=2880*4 && audio.renderedSamples-audio.frameSamples>=1920) {
                audio.leadSamples=std::max<qint64>(0,audio.renderedSamples-audio.frameSamples);
                audio.primed=true;audio.clock.start();
            }else{
                if(!audio.started){audioPending.clear();audio.primingClock.invalidate();}
                return;
            }
        }
        if(audio.started && audio.primed && audio.clock.isValid()) {
            const qint64 dueSamples=audio.clock.nsecsElapsed()/1000*48/1000+audio.leadSamples;
            // Coalesced GUI timers must catch up more than one packet, while
            // work and pending PCM remain bounded when the sink is backpressured.
            for(int i=0;i<64 && audio.started && dueSamples>=audio.renderedSamples && audioPending.size()<3840*6;++i)
                if(!decodeOne(true))break;
        }
        // Include a complete maximum-duration Opus packet beyond the pending
        // target; partial sink writes must not make valid long frames get trimmed.
        if(audioPending.size()>3840*12){const auto dropped=audioPending.size()-3840*12;counters.audioOutputDrops+=dropped/4;audioPending.remove(0,dropped);}
        if(fixtureMode){
            const auto written=fixtureAudioOutput?fixtureAudioOutput->write(audioPending):audioPending.size();
            if(written>0){fixtureAudio+=audioPending.first(written);audioPending.remove(0,written);}
            if(fixtureAudio.size()>3840*50)fixtureAudio.remove(0,fixtureAudio.size()-3840*50);
            return;
        }
        if(!audioPending.isEmpty()){const auto written=playback->write(audioPending.constData(),std::min<qint64>(audioSink->bytesFree(),audioPending.size()));if(written>0){audioPending.remove(0,written);audioSinkHasWritten=true;}}
    }
    qint64 queuedAudioSamples()const{
        qint64 samples=0;
        for(const auto &[_,packet]:audio.queue){const int count=opus_packet_get_nb_samples(bytes(packet),packet.size(),48000);samples+=count>0?count:audio.frameSamples;}
        return samples;
    }
    void poll() {
        if(publishing)return;
        const qint64 entered=clock.nsecsElapsed()/1000;
        ++counters.polls;
        if(lastPollAtUs>=0){
            counters.lastPollGapUs=entered-lastPollAtUs;
            counters.maxPollGapUs=std::max(counters.maxPollGapUs,counters.lastPollGapUs);
            const auto lateUs=counters.lastPollGapUs-qint64(tick.interval())*1000;
            if(lateUs>=10000){
                ++counters.pollLateWakes;
                if(audio.started && audio.primed){++counters.audioLateWakes;counters.maxAudioLateMs=std::max(counters.maxAudioLateMs,lateUs/1000);}
            }
        }
        lastPollAtUs=entered;
        const auto work=qScopeGuard([this,entered]{counters.lastPollWorkUs=clock.nsecsElapsed()/1000-entered;counters.maxPollWorkUs=std::max(counters.maxPollWorkUs,counters.lastPollWorkUs);});
        if(udp.hasPendingDatagrams())receive();
        for(auto &[id,source]:sources)if(source.started && source.depacketizer)drainVideo(id,source);
        processAudio();if(!decoder)return;
        auto snapshot=decoder->take();
        if(!snapshot.error.isEmpty()){error=snapshot.error;notify();}
        if(snapshot.dropped>decoderDrops || snapshot.keyFrameNeeded){decoderDrops=snapshot.dropped;if(selectedVideo)requestKeyframe(selectedVideo);}
        if(snapshot.frame.isValid()) {
            frames=snapshot.decoded;width=snapshot.width;height=snapshot.height;receiving=true;lastFrameAt=clock.elapsed();status="Watching · end-to-end encrypted";stage="receiving";
            if(videoSink){videoSink->setVideoFrame(snapshot.frame);++counters.sinkSubmittedFrames;}
            notify();
        }else if(receiving && clock.elapsed()-lastFrameAt>3000){receiving=false;setStatus("Connected · waiting for video","waiting_for_video");}
    }
};

StreamEngine::StreamEngine(QObject *parent) : QObject(parent),d(std::make_unique<Private>(this)) {
    if(sodium_init()<0)d->error="Could not initialize stream transport encryption.";
#ifdef RELAY_HAS_DAVE
    daveSetLogSinkCallback([](DAVELoggingSeverity,const char*,int,const char*){});
#endif
    d->ws.setMaxAllowedIncomingMessageSize(2*1024*1024);d->ws.setMaxAllowedIncomingFrameSize(2*1024*1024);
    d->deadline.setSingleShot(true);d->tick.setTimerType(Qt::PreciseTimer);d->sendTick.setTimerType(Qt::PreciseTimer);d->diagnosticTimer.setInterval(1000);
    connect(&d->sendTick,&QTimer::timeout,this,[this]{d->sendPacketsNow();});
    connect(&d->heartbeat,&QTimer::timeout,this,[this]{d->heartbeatNow();});
    connect(&d->deadline,&QTimer::timeout,this,[this]{d->fail("The stream connection timed out. Check that the sender is still streaming, then open it again.");});
    connect(&d->tick,&QTimer::timeout,this,[this]{d->poll();});
    connect(&d->feedback,&QTimer::timeout,this,[this]{d->receiverReports();});
    connect(&d->diagnosticTimer,&QTimer::timeout,this,&StreamEngine::diagnosticsChanged);
    connect(&d->keepAlive,&QTimer::timeout,this,[this]{
        // Discord's native media servers recognize this ping magic followed by
        // a little-endian sequence, rather than a zero-padded counter.
        const auto packet=VoicePacket::nativePing(d->keepAliveCounter++);
        if(d->udp.writeDatagram(packet,d->address,d->port)==packet.size())++d->counters.pings;
    });
    connect(&d->ws,&QWebSocket::connected,this,[this]{
        int version=0;
#ifdef RELAY_HAS_DAVE
        d->session=daveSessionCreate(nullptr,nullptr,nullptr,nullptr);version=daveMaxSupportedProtocolVersion();
        if(d->publishing){d->encryptor=daveEncryptorCreate();daveEncryptorSetPassthroughMode(d->encryptor,false);}
#endif
        d->send(0,{{"server_id",d->rtcServerId},{"channel_id",d->rtcChannelId},{"user_id",d->userId},{"session_id",d->sessionId},{"token",d->token},
            {"video",true},{"streams",QJsonArray{QJsonObject{{"type","screen"},{"rid","100"},{"quality",100}}}},{"max_dave_protocol_version",version}});
    });
    connect(&d->ws,&QWebSocket::textMessageReceived,this,[this](const QString &message){d->text(message);});
    connect(&d->ws,&QWebSocket::binaryMessageReceived,this,[this](const QByteArray &message){
#ifdef RELAY_HAS_DAVE
        d->binary(message);
#else
        Q_UNUSED(message);
#endif
    });
    connect(&d->ws,&QWebSocket::disconnected,this,[this]{d->counters.close=int(d->ws.closeCode());if(!d->closing && d->active)d->fail(QString("The stream connection closed (%1). Open it again to reconnect.").arg(int(d->ws.closeCode())));});
    connect(&d->ws,&QWebSocket::errorOccurred,this,[this](QAbstractSocket::SocketError){if(!d->closing && d->active)d->fail("Could not connect to the stream server.");});
    connect(&d->udp,&QUdpSocket::readyRead,this,[this]{d->receive();});
    connect(&d->media,&QMediaDevices::audioOutputsChanged,this,[this]{d->stopAudio();d->startAudio();});
}
StreamEngine::~StreamEngine(){d->reset();}
bool StreamEngine::available()const{
#ifdef RELAY_HAS_DAVE
    return StreamDecoder::supports("AV1") || StreamDecoder::supports("H265") || StreamDecoder::supports("H264") || StreamDecoder::supports("VP8");
#else
    return false;
#endif
}
bool StreamEngine::active()const{return d->active;}bool StreamEngine::waitingForRestart()const{return d->waitingForRestart;}bool StreamEngine::connected()const{return d->connected;}
bool StreamEngine::receiving()const{return d->receiving;}bool StreamEngine::encryptedReady()const{return d->encrypted;}
QString StreamEngine::streamKey()const{return d->key;}QString StreamEngine::title()const{return d->title;}
QString StreamEngine::status()const{return d->status;}QString StreamEngine::error()const{return d->error;}QString StreamEngine::codec()const{return d->codec;}
int StreamEngine::videoWidth()const{return d->width;}int StreamEngine::videoHeight()const{return d->height;}quint64 StreamEngine::frameCount()const{return d->frames;}
QObject *StreamEngine::videoSink()const{return d->videoSink;}void StreamEngine::setVideoSink(QObject *sink){auto video=qobject_cast<QVideoSink*>(sink);if(d->videoSink==video)return;d->videoSink=video;emit videoSinkChanged();}
bool StreamEngine::audioMuted()const{return d->audioMuted;}void StreamEngine::setAudioMuted(bool value){if(d->audioMuted==value)return;d->audioMuted=value;if(value)d->stopAudio();else d->startAudio();d->notify();}
void StreamEngine::setDeafened(bool value){if(d->deafened==value)return;d->deafened=value;if(value)d->stopAudio();else d->startAudio();}
double StreamEngine::volume()const{return d->volume;}void StreamEngine::setVolume(double value){if(!std::isfinite(value))return;d->volume=std::clamp(value,0.0,1.0);if(d->audioSink)d->audioSink->setVolume(float(d->volume));d->notify();}
QString StreamEngine::outputDeviceId()const{return d->outputId;}void StreamEngine::setOutputDeviceId(const QString &id){if(d->outputId==id)return;d->outputId=id;d->stopAudio();d->startAudio();d->notify();}
void StreamEngine::setIdentity(const QString &user,const QString &session){d->userId=user;d->sessionId=session;d->openGateway();}
void StreamEngine::watch(const QString &key,const QString &title){
    if(!available()){d->error="Native stream viewing requires libdave and an H.264 or VP8 decoder.";d->notify();return;}
    static const QRegularExpression valid("^guild:[0-9]{1,22}:[0-9]{1,22}:[0-9]{1,22}$");
    if(!valid.match(key).hasMatch()){d->error="Choose a stream from your current voice channel.";d->notify();return;}
    if(!d->key.isEmpty())stop();d->key=key;d->title=title;d->prepareWatch();emit watchRequested(key);
}
void StreamEngine::stop(){const auto key=d->key;d->reset();d->key.clear();d->title.clear();d->error.clear();d->codec.clear();d->setStatus("No stream selected","idle");if(!key.isEmpty())emit stopRequested(key);}
void StreamEngine::onStreamRestart(const QString &key){
    if(!d->publishing && d->waitingForRestart && d->key==key)d->prepareWatch();
}
void StreamEngine::onStreamCreate(const QJsonObject &event){
    if(!d->active || event.value("stream_key").toString()!=d->key)return;
    const auto server=event.value("rtc_server_id").toString(),channel=event.value("rtc_channel_id").toString();
    if(server.isEmpty() && channel.isEmpty())return; // Availability announcement precedes the requested allocation.
    if(!validId(server) || server.toULongLong()<2 || !validId(channel)){d->fail("Discord did not provide a complete stream media session.");return;}
    if(!d->rtcServerId.isEmpty() && (d->rtcServerId!=server || d->rtcChannelId!=channel)){
        if(d->publishing){d->fail("The stream moved to another server. Open it again.");return;}
        d->prepareWatch();
    }
    d->rtcServerId=server;d->rtcChannelId=channel;d->openGateway();
}
void StreamEngine::onStreamServer(const QJsonObject &event){
    if(!d->active || event.value("stream_key").toString()!=d->key)return;
    const auto endpoint=event.value("endpoint").toString(),token=event.value("token").toString();
    if(endpoint.isEmpty() || token.isEmpty())return;
    if(!d->endpoint.isEmpty() && (d->endpoint!=endpoint || d->token!=token)){
        if(d->publishing){d->fail("The stream transport changed. Open it again.");return;}
        d->prepareWatch();
    }
    d->endpoint=endpoint;d->token=token;d->openGateway();
}
void StreamEngine::onStreamDelete(const QJsonObject &event){
    if(d->key.isEmpty() || event.value("stream_key").toString()!=d->key)return;
    if(event.value("relay_cancelled").toBool() || d->publishing){stop();return;}
    if(d->waitingForRestart)return;
    d->reset();d->error.clear();d->codec.clear();d->waitingForRestart=true;
    d->setStatus("This stream has ended. Waiting for it to restart…","waiting_for_restart");
}
QString StreamEngine::diagnosticSummary()const{
    const auto &c=d->counters;auto summary=QString("Stage %1 · gateway %2/%3 (last %4) · groups %5 · UDP %6 · video packets/frames/decoded %7/%8/%9 · audio/decoded %10/%11 · transport/media errors %12/%13 · feedback %14 · unknown %15 · close %16")
        .arg(d->stage).arg(c.json).arg(c.binary).arg(c.lastOp).arg(c.groups).arg(c.udp).arg(c.videoPackets).arg(c.videoFrames).arg(d->frames).arg(c.audioPackets).arg(c.decodedAudio).arg(c.transportErrors).arg(c.mediaErrors).arg(c.feedback).arg(c.unknown).arg(c.close);
    summary+=QString(" · ping/pong %1/%2 · RTCP received/invalid %3/%4").arg(c.pings).arg(c.pongs).arg(c.rtcpReceived).arg(c.rtcpInvalid);
    summary+=QString(" · audio RTP %1 · audio drops muted/deaf/keys/unmapped/decoder/retired %2/%3/%4/%5/%6/%7 · audio announcements/mappings %8/%9 · audio mapped/output/muted/deaf %10/%11/%12/%13 · audio output state/error %14/%15")
        .arg(c.audioRtp).arg(c.audioMutedDrops).arg(c.audioDeafenedDrops).arg(c.audioKeyDrops).arg(c.audioUnmappedDrops).arg(c.audioDecoderDrops).arg(c.audioRetiredDrops)
        .arg(c.audioAnnouncements).arg(c.audioMappings).arg(!d->audioUsers.empty()).arg(d->playback!=nullptr).arg(d->audioMuted).arg(d->deafened)
        .arg(d->audioSink?int(d->audioSink->state()):-1).arg(d->audioSink?int(d->audioSink->error()):0);
    summary+=QString(" · audio conceal/late-sequence/queue-reset/underrun/service-late-wakes/max-service-late-ms/decoder-errors/output-dropped-samples %1/%2/%3/%4/%5/%6/%7/%8")
        .arg(c.audioConcealed).arg(c.audioLateSequences).arg(c.audioQueueResets).arg(c.audioUnderruns).arg(c.audioLateWakes).arg(c.maxAudioLateMs).arg(c.audioDecodeErrors).arg(c.audioOutputDrops);
    if(!d->publishing){
        const auto timing=playbackDiagnostics();
        summary+=QString(" · poll interval ms %1 · poll current/max gap us %2/%3 · poll current/max work us %4/%5 · poll/active-audio late wakes %6/%7 · sink frame submissions/decoder drops %8/%9")
            .arg(d->tick.interval()).arg(c.lastPollGapUs).arg(c.maxPollGapUs).arg(c.lastPollWorkUs).arg(c.maxPollWorkUs).arg(c.pollLateWakes).arg(c.audioLateWakes).arg(c.sinkSubmittedFrames).arg(d->decoderDrops);
        summary+=QString(" · audio current cursor/encoded/pending/Qt-sink buffer ms %1/%2/%3/%4 · sink idle events %5 · audio recovery rebases/retired samples %6/%7")
            .arg(timing.value("audioCursorLagMs").toLongLong()).arg(timing.value("audioQueuedMs").toLongLong()).arg(timing.value("audioPendingMs").toLongLong()).arg(timing.value("audioSinkBufferedMs").toLongLong()).arg(c.audioSinkIdleEvents).arg(c.audioRebases).arg(c.audioRetiredSamples);
    }
    if(d->publishing)summary+=QString(" · sent video/repair/audio/bytes %1/%2/%3/%4 · PLI/NACK/cache misses %5/%6/%7 · queued/dropped %8/%9")
        .arg(c.txVideo).arg(c.txRepair).arg(c.txAudio).arg(c.txBytes).arg(c.pli).arg(c.nack).arg(c.repairMiss).arg(d->outgoing.size()+d->repairs.size()).arg(c.queueDrops);
    if(d->publishing)summary+=QString(" · RTX skipped repeat/aged/unsent %1/%2/%3 · repair budget deferrals/max age ms %4/%5 · receiver reports/loss last-max/RTT ms %6/%7-%8/%9 · REMB/count bps %10/%11 · TWCC %12 · wants/count quality %13/%14")
        .arg(c.repairRepeated).arg(c.repairExpired).arg(c.repairUnsent).arg(c.repairDeferred).arg(c.maxRepairAge).arg(c.receiverReports).arg(c.lastFractionLost).arg(c.maxFractionLost).arg(c.lastRttMs).arg(c.remb).arg(c.lastRembBitrate).arg(c.twcc).arg(c.wants).arg(c.lastWanted);
    return summary;
}
QVariantMap StreamEngine::playbackDiagnostics()const{
    const auto queuedSamples=d->queuedAudioSamples();
    const qint64 dueSamples=d->audio.started && d->audio.primed && d->audio.clock.isValid()?d->audio.clock.nsecsElapsed()/1000*48/1000+d->audio.leadSamples:0;
    const qint64 sinkBuffered=d->audioSink?std::max<qint64>(0,d->audioSink->bufferSize()-d->audioSink->bytesFree()):0;
    const auto &c=d->counters;
    return {{"polls",qulonglong(c.polls)},{"pollIntervalMs",d->tick.interval()},{"pollLateWakes",qulonglong(c.pollLateWakes)},
        {"lastPollGapUs",c.lastPollGapUs},{"maxPollGapUs",c.maxPollGapUs},{"lastPollWorkUs",c.lastPollWorkUs},{"maxPollWorkUs",c.maxPollWorkUs},
        {"decodedFrames",qulonglong(d->frames)},{"sinkSubmittedFrames",qulonglong(c.sinkSubmittedFrames)},{"decoderDrops",qulonglong(d->decoderDrops)},
        {"audioLateWakes",qulonglong(c.audioLateWakes)},{"maxAudioLateMs",c.maxAudioLateMs},{"audioPrimed",d->audio.primed},
        {"audioRebases",qulonglong(c.audioRebases)},{"audioRetiredSamples",qulonglong(c.audioRetiredSamples)},{"audioRecoveryPending",d->audio.recoveryPending},
        {"audioReceivedPackets",qulonglong(c.audioPackets)},{"audioDecodedPackets",qulonglong(c.decodedAudio)},{"audioConcealedPackets",qulonglong(c.audioConcealed)},
        {"audioCursorLagMs",std::max<qint64>(0,(dueSamples-d->audio.renderedSamples)/48)},
        {"audioQueuedPackets",qulonglong(d->audio.queue.size())},{"audioQueuedMs",queuedSamples/48},{"audioPendingMs",qint64(d->audioPending.size()/192)},
        {"audioSinkBufferedMs",sinkBuffered/192},{"audioSinkIdleEvents",qulonglong(c.audioSinkIdleEvents)},
        {"audioSinkState",d->audioSink?int(d->audioSink->state()):-1},{"audioSinkError",d->audioSink?int(d->audioSink->error()):0}};
}
QVariantMap StreamEngine::publishingDiagnostics()const{
    const auto &c=d->counters;
    return {{"active",d->active},{"connected",d->connected},{"encrypted",d->encrypted},
        {"videoPackets",qulonglong(c.txVideo)},{"repairPackets",qulonglong(c.txRepair)},
        {"audioPackets",qulonglong(c.txAudio)},{"wireBytes",qulonglong(c.txBytes)},
        {"videoFrames",qulonglong(d->frames)},{"offeredFrames",qulonglong(c.videoFrames)},
        {"queuedPackets",qulonglong(d->outgoing.size())},{"queuedRepairs",qulonglong(d->repairs.size())},
        {"queueDrops",qulonglong(c.queueDrops)},{"transportErrors",qulonglong(c.transportErrors)},
        {"mediaErrors",qulonglong(c.mediaErrors)},{"keyframeRequests",qulonglong(c.pli)},
        {"nackRequests",qulonglong(c.nack)},{"repairMisses",qulonglong(c.repairMiss)},
        {"receiverReports",qulonglong(c.receiverReports)},{"fractionLost",c.lastFractionLost},
        {"maxFractionLost",c.maxFractionLost},{"rttMs",c.lastRttMs},
        {"rembReports",qulonglong(c.remb)},{"rembBitrate",qulonglong(c.lastRembBitrate)},
        {"heartbeatPending",d->heartbeatPending},{"gatewayMessages",qulonglong(c.json+c.binary)},
        {"keepalivePings",qulonglong(c.pings)},{"keepalivePongs",qulonglong(c.pongs)},
        {"receiverBitrate",d->receiverWireBitrate},{"selectedBitrate",d->selectedBitrate},
        {"bitrateChanges",qulonglong(c.bitrateChanges)},{"congestionResets",qulonglong(c.congestionResets)},
        {"congestionGated",qulonglong(c.congestionGated)},{"awaitingPublishKeyframe",d->awaitingPublishKeyframe},{"bandwidthPaused",d->bandwidthPaused},
        {"targetBitrate",d->sendBitrate},{"targetFps",d->sendFps},{"width",d->sendWidth},{"height",d->sendHeight}};
}
bool StreamEngine::configureLocalFixture(const QUrl &url){if(d->active || url.scheme()!="ws" || url.host()!="127.0.0.1" || url.port()<=0)return false;d->fixtureMode=true;d->fixtureUrl=url;return true;}
QByteArray StreamEngine::takeFixtureAudio(){if(!d->fixtureMode)return {};QByteArray result;result.swap(d->fixtureAudio);return result;}
void StreamEngine::setFixtureAudioOutput(QIODevice *output){if(d->fixtureMode)d->fixtureAudioOutput=output;}
void StreamEngine::setFixturePollInterval(int milliseconds){if(d->fixtureMode && milliseconds>=1 && milliseconds<=100)d->tick.setInterval(milliseconds);}
void StreamEngine::publish(const QString &key,int width,int height,int fps,int bitrate){
    d->publishing=true;d->sendWidth=width;d->sendHeight=height;d->sendFps=fps;d->selectedBitrate=d->sendBitrate=std::clamp(bitrate,500000,40000000);
    d->sendSequence=quint16(QRandomGenerator::global()->generate());d->rtxSequence=quint16(QRandomGenerator::global()->generate());
    d->audioSequence=quint16(QRandomGenerator::global()->generate());d->transportSequence=quint16(QRandomGenerator::global()->generate());
    watch(key,"Screen sharing");
}
bool StreamEngine::publishVideo(const QByteArray &frame,quint32 timestamp,int width,int height){return d->queueVideo(frame,timestamp,width,height);}
bool StreamEngine::publishAudio(const QByteArray &opus,quint32 timestamp){return d->sendAudioFrame(opus,timestamp);}
