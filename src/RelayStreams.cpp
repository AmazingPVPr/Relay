#include "RelayStreams.h"
#include "RelayPeers.h"
#include "BroadcastEngine.h"
#include "BroadcastEncoder.h"
#include "StreamDecoder.h"
#include "BroadcastPacket.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QDataStream>
#include <QElapsedTimer>
#include <QMediaDevices>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QVideoSink>
#include <opus/opus.h>
#include <array>
#include <cstring>

namespace {
constexpr int Header = 38, Chunk = 12000, MaxFrame = 4 * 1024 * 1024;
bool keyframe(const QByteArray &data) {
    // Annex B IDR, verified independently of the remote metadata.
    for (qsizetype i=0;i+4<data.size();++i) {
        if (data[i]==0 && data[i+1]==0 && data[i+2]==1 && (quint8(data[i+3])&31)==5) return true;
    }
    return false;
}
}
struct RelayStreams::Private {
    RelayStreams *q; RelayPeers *peers; BroadcastEngine *broadcast;
    QTimer tick; QElapsedTimer clock;
    bool enabled=true, muted=false, deafened=false;
    int mbps=20, height=2160, fps=60;
    QString published, watching, watchingId, status="No Relay stream selected", outputId;
    struct Offer { QString id, name; };
    QHash<QString,Offer> offers;
    QHash<QString,bool> viewers; // true means an IDR is required after joining/congestion.
    QHash<QString,qint64> lastKeyRequests;
    QPointer<QVideoSink> sink;
    std::unique_ptr<BroadcastEncoder> encoder;
    std::unique_ptr<StreamDecoder> decoder;
    quint64 sequence=0, receivingSequence=0, decoded=0;
    quint32 receivingStamp=0, receivingTotal=0;
    QByteArray assembly, pcm;
    bool receivingKey=false, needKey=true;
    qint64 originalAt=-10000, assemblyAt=0, lastPacketAt=0, lastKeyAt=-10000, watchAt=0;
    std::unique_ptr<QAudioSink> audio;
    QIODevice *audioDevice=nullptr;
    OpusEncoder *opus=nullptr; OpusDecoder *opusDecoder=nullptr;
    explicit Private(RelayStreams *owner,RelayPeers *p,BroadcastEngine *b):q(owner),peers(p),broadcast(b){clock.start();}
    ~Private(){if(opus)opus_encoder_destroy(opus);if(opusDecoder)opus_decoder_destroy(opusDecoder);}
    void notify(){emit q->changed();}
    QJsonObject control(const QString &type,const QString &id) { return {{"v",1},{"type",type},{"id",id}}; }
    void announce(const QString &peer) {
        auto frame=control("available",published);
        frame.insert("name",published.isEmpty()?QString():broadcast->sourceName().left(200));
        peers->sendStreamControl(peer,frame);
    }
    void updatePublication() {
        const bool active=enabled && broadcast->active();
        if(active==!published.isEmpty())return;
        published=active?QUuid::createUuid().toString(QUuid::WithoutBraces):QString();
        viewers.clear();encoder.reset();pcm.clear();originalAt=-10000;
        if(opus){opus_encoder_destroy(opus);opus=nullptr;}
        for(const auto &row:peers->contacts())announce(row.toMap().value("key").toString());
        notify();
    }
    void requestKey() {
        if(watching.isEmpty() || clock.elapsed()-lastKeyAt<1000)return;
        lastKeyAt=clock.elapsed();peers->sendStreamControl(watching,control("key",watchingId));
    }
    void controlReceived(const QString &peer,const QJsonObject &frame) {
        if(frame.value("v").toInt()!=1)return;
        const auto type=frame.value("type").toString(), id=frame.value("id").toString();
        if(!id.isEmpty() && (QUuid(id).isNull() || id.size()!=36))return;
        if(type=="available") {
            const auto name=frame.value("name").toString().left(200);
            if(watching==peer && watchingId!=id)q->stopWatching();
            if(id.isEmpty())offers.remove(peer);else offers[peer]={id,name};
            notify();return;
        }
        if(type=="watch") {
            if(id.isEmpty() || id!=published || viewers.size()>=8)return;
            if(!peers->sendStreamControl(peer,control("start",id)))return;
            viewers[peer]=true;
            if(encoder)encoder->requestKeyframe();
            notify();
        } else if(type=="leave" && id==published) {
            viewers.remove(peer);
            if(viewers.isEmpty()){encoder.reset();pcm.clear();}
            notify();
        } else if(type=="key" && id==published && viewers.contains(peer)) {
            if(clock.elapsed()-lastKeyRequests.value(peer,-10000)<1000)return;
            lastKeyRequests[peer]=clock.elapsed();viewers[peer]=true;if(encoder)encoder->requestKeyframe();
        } else if(type=="start" && peer==watching && id==watchingId) {
            status="Waiting for the source keyframe…";notify();
        }
    }
    void send(const QByteArray &data,quint32 stamp,bool key,quint8 kind) {
        if(published.isEmpty() || data.isEmpty() || data.size()>MaxFrame || viewers.isEmpty())return;
        const auto id=QUuid(published).toRfc4122();const auto seq=++sequence;
        for(const auto &peer:viewers.keys()) {
            if(!peers->streamConnected(peer)){viewers.remove(peer);notify();continue;}
            if(peers->streamBufferedAmount(peer)>512*1024) {
                viewers[peer]=true;if(encoder)encoder->requestKeyframe();continue;
            }
            if(kind==1 && viewers[peer] && !key)continue;
            bool sent=true;
            for(qsizetype offset=0;offset<data.size();offset+=Chunk) {
                QByteArray packet;QDataStream out(&packet,QIODevice::WriteOnly);out.setVersion(QDataStream::Qt_6_0);
                out.writeRawData(id.constData(),16);
                out<<kind<<seq<<stamp<<quint32(data.size())<<quint32(offset)<<quint8(key);
                packet+=data.mid(offset,Chunk);
                if(!peers->sendStreamPacket(peer,packet)){sent=false;break;}
            }
            if(kind==1)viewers[peer]=!sent;
        }
    }
    void rawFrame(const QVideoFrame &frame,int maximumHeight,int frameRate) {
        if(viewers.isEmpty() || !frame.isValid() || clock.elapsed()-originalAt<1000)return;
        if(!encoder || height!=maximumHeight || fps!=frameRate) {
            height=maximumHeight;fps=frameRate;
            encoder=std::make_unique<BroadcastEncoder>(height,fps,mbps*1000000);
            for(auto it=viewers.begin();it!=viewers.end();++it)it.value()=true;
        }
        encoder->submit(frame);
    }
    void originalVideo(const QByteArray &bytes,quint32 stamp,bool key) {
        if(viewers.isEmpty())return;
        const auto normalized=BroadcastPacket::normalizeH264(bytes);
        if(normalized.isEmpty())return;
        originalAt=clock.elapsed();encoder.reset();
        send(normalized,stamp,key && keyframe(normalized),1);
    }
    void sourceAudio(const QByteArray &bytes) {
        if(viewers.isEmpty() || bytes.isEmpty() || bytes.size()>48000*4/5 || bytes.size()%4)return;
        if(!opus){int error=0;opus=opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&error);if(!opus || error!=OPUS_OK)return;opus_encoder_ctl(opus,OPUS_SET_BITRATE(256000));}
        pcm+=bytes;
        if(pcm.size()>3840*10)pcm=pcm.right(3840*5);
        while(pcm.size()>=3840) {
            std::array<opus_int16,1920> samples;std::memcpy(samples.data(),pcm.constData(),3840);pcm.remove(0,3840);
            QByteArray encoded(1275,Qt::Uninitialized);const int count=opus_encode(opus,samples.data(),960,reinterpret_cast<unsigned char *>(encoded.data()),encoded.size());
            if(count>0){encoded.resize(count);send(encoded,quint32(clock.elapsed()*48),false,2);}
        }
    }
    void resetAudio(){audio.reset();audioDevice=nullptr;}
    void play(const QByteArray &bytes) {
        if(muted || deafened || bytes.size()>1275)return;
        if(!opusDecoder){int error=0;opusDecoder=opus_decoder_create(48000,2,&error);if(!opusDecoder || error!=OPUS_OK)return;}
        std::array<opus_int16,1920> samples;
        const auto count=opus_decode(opusDecoder,reinterpret_cast<const unsigned char *>(bytes.constData()),bytes.size(),samples.data(),960,0);
        if(count<=0)return;
        if(!audio) {
            QAudioDevice device=QMediaDevices::defaultAudioOutput();
            for(const auto &candidate:QMediaDevices::audioOutputs())if(QString::fromLatin1(candidate.id().toBase64())==outputId)device=candidate;
            QAudioFormat format;format.setSampleRate(48000);format.setChannelCount(2);format.setSampleFormat(QAudioFormat::Int16);
            if(device.isNull() || !device.isFormatSupported(format))return;
            audio=std::make_unique<QAudioSink>(device,format);audio->setBufferSize(19200);audioDevice=audio->start();
        }
        if(audioDevice && audio->bytesFree()>=count*4)audioDevice->write(reinterpret_cast<const char *>(samples.data()),count*4);
    }
    void receive(const QString &peer,const QByteArray &packet) {
        if(peer!=watching || packet.size()<=Header || packet.size()>Header+Chunk || watchingId.isEmpty())return;
        QDataStream in(packet);in.setVersion(QDataStream::Qt_6_0);
        QByteArray id(16,Qt::Uninitialized);in.readRawData(id.data(),16);
        quint8 kind=0,key=0;quint64 seq=0;quint32 stamp=0,total=0,offset=0;
        in>>kind>>seq>>stamp>>total>>offset>>key;
        if(in.status()!=QDataStream::Ok || id!=QUuid(watchingId).toRfc4122() || total==0 || total>MaxFrame || offset>=total || key>1)return;
        const auto data=packet.mid(Header);
        if(quint64(offset)+quint64(data.size())>total)return;
        if(kind==2){if(offset==0 && total==quint32(data.size()) && total<=1275)play(data);return;}
        if(kind!=1)return;
        if(offset==0){
            if(!assembly.isEmpty()){needKey=true;requestKey();}
            receivingSequence=seq;receivingStamp=stamp;receivingTotal=total;receivingKey=key;assembly.clear();assemblyAt=clock.elapsed();
        }
        if(seq!=receivingSequence || offset!=quint32(assembly.size()) || total!=receivingTotal || stamp!=receivingStamp || bool(key)!=receivingKey){assembly.clear();needKey=true;requestKey();return;}
        assembly+=data;
        if(quint32(assembly.size())!=total)return;
        auto frame=std::move(assembly);assembly.clear();
        const bool actualKey=receivingKey && keyframe(frame);
        if(needKey && !actualKey){requestKey();return;}
        if(actualKey)needKey=false;
        if(!decoder)decoder=std::make_unique<StreamDecoder>("H264");
        if(!decoder->submit(frame,stamp)){needKey=true;requestKey();}
        lastPacketAt=clock.elapsed();
    }
    void poll() {
        if(encoder){const auto snapshot=encoder->take();for(const auto &frame:snapshot.frames)send(frame.bytes,frame.timestamp,frame.keyframe,1);}
        if(!watching.isEmpty()) {
            if(!peers->streamConnected(watching)){q->stopWatching();return;}
            if(!assembly.isEmpty() && clock.elapsed()-assemblyAt>1500){assembly.clear();needKey=true;requestKey();}
            if(clock.elapsed()-lastPacketAt>3000) {
                requestKey();
                const auto next=clock.elapsed()-watchAt>10000?QString("No video received. Check the sender and direct connection."):QString("Waiting for the source keyframe…");
                if(status!=next){status=next;notify();}
            }
        }
        if(decoder) {
            const auto snapshot=decoder->take();
            if(snapshot.frame.isValid() && sink)sink->setVideoFrame(snapshot.frame);
            if(snapshot.keyFrameNeeded){needKey=true;decoder->discardUntilKeyFrame();requestKey();}
            if(!snapshot.error.isEmpty() && status!=snapshot.error){status=snapshot.error;notify();}
            else if(snapshot.decoded!=decoded){decoded=snapshot.decoded;status=QString("Relay direct · %1 × %2 · encrypted").arg(snapshot.width).arg(snapshot.height);notify();}
        }
    }
};
RelayStreams::RelayStreams(RelayPeers *peers,BroadcastEngine *broadcast,QObject *parent):QObject(parent),d(std::make_unique<Private>(this,peers,broadcast)) {
    connect(broadcast,&BroadcastEngine::changed,this,[this]{d->updatePublication();});
    connect(broadcast,&BroadcastEngine::sourceFrame,this,[this](const QVideoFrame &frame,int height,int fps){d->rawFrame(frame,height,fps);});
    connect(broadcast,&BroadcastEngine::sourceVideo,this,[this](const QByteArray &frame,quint32 stamp,bool key){d->originalVideo(frame,stamp,key);});
    connect(broadcast,&BroadcastEngine::sourceAudio,this,[this](const QByteArray &pcm){d->sourceAudio(pcm);});
    connect(peers,&RelayPeers::streamReady,this,[this](const QString &peer){d->announce(peer);});
    connect(peers,&RelayPeers::streamControlReceived,this,[this](const QString &peer,const QJsonObject &frame){d->controlReceived(peer,frame);});
    connect(peers,&RelayPeers::streamPacketReceived,this,[this](const QString &peer,const QByteArray &packet){d->receive(peer,packet);});
    connect(peers,&RelayPeers::streamDisconnected,this,[this](const QString &peer){d->offers.remove(peer);d->viewers.remove(peer);if(d->viewers.isEmpty()){d->encoder.reset();d->pcm.clear();}d->lastKeyRequests.remove(peer);if(d->watching==peer)stopWatching();emit changed();});
    d->tick.setInterval(10);connect(&d->tick,&QTimer::timeout,this,[this]{d->poll();});
    connect(this,&RelayStreams::changed,this,[this]{
        d->broadcast->setDirectViewersActive(!d->viewers.isEmpty());
        const bool needed=!d->viewers.isEmpty() || !d->watching.isEmpty();
        if(needed && !d->tick.isActive())d->tick.start();else if(!needed)d->tick.stop();
    });
}
RelayStreams::~RelayStreams(){stopWatching();}
bool RelayStreams::publishEnabled() const{return d->enabled;}
void RelayStreams::setPublishEnabled(bool value){if(value==d->enabled)return;d->enabled=value;d->updatePublication();emit changed();}
int RelayStreams::bitrateMbps()const{return d->mbps;}
void RelayStreams::setBitrateMbps(int value){value=qBound(1,value,40);if(value==d->mbps)return;d->mbps=value;if(d->encoder)d->encoder->setBitrate(value*1000000);emit changed();}
int RelayStreams::viewerCount()const{return d->viewers.size();}
QVariantList RelayStreams::streams()const{QVariantList rows;for(auto it=d->offers.cbegin();it!=d->offers.cend();++it)rows.append(QVariantMap{{"peer",it.key()},{"name",it->name}});return rows;}
QString RelayStreams::watchingPeer()const{return d->watching;}
QString RelayStreams::status()const{return d->status;}
QObject *RelayStreams::videoSink()const{return d->sink;}
void RelayStreams::setVideoSink(QObject *sink){auto *video=qobject_cast<QVideoSink *>(sink);if(d->sink==video)return;d->sink=video;emit changed();}
bool RelayStreams::audioMuted()const{return d->muted;}
void RelayStreams::setAudioMuted(bool muted){d->muted=muted;d->resetAudio();emit changed();}
void RelayStreams::setDeafened(bool deafened){if(d->deafened==deafened)return;d->deafened=deafened;d->resetAudio();}
void RelayStreams::setOutputDeviceId(const QString &id){if(d->outputId==id)return;d->outputId=id;d->resetAudio();}
void RelayStreams::watch(const QString &peer){
    if(!d->offers.contains(peer) || !d->peers->streamConnected(peer))return;
    stopWatching();d->watching=peer;d->watchingId=d->offers[peer].id;d->watchAt=d->clock.elapsed();d->lastPacketAt=d->watchAt;d->lastKeyAt=-10000;
    d->status="Connecting directly…";
    if(!d->peers->sendStreamControl(peer,d->control("watch",d->watchingId))){stopWatching();return;}
    emit changed();
}
void RelayStreams::stopWatching(){
    if(!d->watching.isEmpty())d->peers->sendStreamControl(d->watching,d->control("leave",d->watchingId));
    d->watching.clear();d->watchingId.clear();d->assembly.clear();d->needKey=true;d->decoder.reset();d->decoded=0;d->resetAudio();
    if(d->opusDecoder){opus_decoder_destroy(d->opusDecoder);d->opusDecoder=nullptr;}
    if(d->sink)d->sink->setVideoFrame({});
    d->status="No Relay stream selected";emit changed();
}
