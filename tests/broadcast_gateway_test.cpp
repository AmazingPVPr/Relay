#include <QtTest>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVideoSink>
#include <QVideoFrame>
#include <QPointer>
#include <QImage>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <cmath>
#include "BroadcastEncoder.h"
#include <opus/opus.h>
#include "StreamEngine.h"
#include "BroadcastEngine.h"
#include "StudioBridge.h"
#include "StudioTestMedia.h"
#include "StreamDecoder.h"
#include <QStandardPaths>
#include "VideoPacket.h"
#include "StreamTestMedia.h"
#include "BroadcastPacket.h"
#ifdef RELAY_TEST_MLSPP
#include "StreamTestDave.h"
#endif

namespace {
constexpr auto streamKey="guild:1111:2222:3333";
template<class Predicate> bool waitNative(Predicate ready,int timeoutMs){
    if(ready())return true;
    // QTest's polling wait services precise timers only about every 10 ms.
    // Exercise the real native timer cadence for sustained media throughput.
    QEventLoop loop;QTimer poll,deadline;poll.setTimerType(Qt::PreciseTimer);poll.setInterval(1);deadline.setSingleShot(true);
    QObject::connect(&poll,&QTimer::timeout,&loop,[&]{if(ready())loop.quit();});
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    poll.start();deadline.start(timeoutMs);loop.exec();return ready();
}
class ObservedSource : public QObject, public RelayBroadcastSourceInterface, public RelayEncodedBroadcastSourceInterface {
    Q_OBJECT
    Q_INTERFACES(RelayBroadcastSourceInterface RelayEncodedBroadcastSourceInterface)
public:
    StudioBridge source;
    QHash<quint32,QByteArray> original;
    void setBroadcastContext(const QVariantMap &context) override {source.setBroadcastContext(context);}
    void setBroadcastCaptureEnabled(bool enabled,int height,int fps,bool audio) override {source.setBroadcastCaptureEnabled(enabled,height,fps,audio);}
    RelayBroadcastMedia takeBroadcastMedia() override {return source.takeBroadcastMedia();}
    RelayEncodedVideo takeEncodedBroadcastVideo() override {
        auto video=source.takeEncodedBroadcastVideo();
        for(const auto &frame:video.frames)original.insert(frame.timestamp,BroadcastPacket::normalizeH264(frame.h264));
        return video;
    }
};
class GeneratedBroadcastSource : public QObject, public RelayBroadcastSourceInterface {
    Q_OBJECT
    Q_INTERFACES(RelayBroadcastSourceInterface)
public:
    bool capturing=false,requestedAudio=false,offerAudio=true,queuedFeedback=false;
    quint64 sample=0,takes=0;double frequency=440;
    void setBroadcastContext(const QVariantMap &) override {}
    void setBroadcastCaptureEnabled(bool enabled,int,int,bool audio) override {capturing=enabled;requestedAudio=audio;}
    RelayBroadcastMedia takeBroadcastMedia() override {
        RelayBroadcastMedia media;if(!capturing)return media;++takes;
        QImage image(320,180,QImage::Format_RGB32);image.fill(QColor(180+int(takes%40),40,60));media.video=QVideoFrame(image);
        if((offerAudio || queuedFeedback) && requestedAudio){
            queuedFeedback=false;
            media.audio.resize(3840);
            for(int i=0;i<960;++i,++sample){const qint16 value=qint16(6000*std::sin(sample*2*3.141592653589793*frequency/48000));
                std::memcpy(media.audio.data()+i*4,&value,2);std::memcpy(media.audio.data()+i*4+2,&value,2);}
        }
        return media;
    }
};
class PacedRawSource : public QObject, public RelayBroadcastSourceInterface {
    Q_OBJECT
    Q_INTERFACES(RelayBroadcastSourceInterface)
public:
    bool enabled=false,audioEnabled=false,noise=false;QElapsedTimer elapsed;int videoCursor=-1,audioCursor=0;quint64 sample=0;
    void setBroadcastContext(const QVariantMap &) override {}
    void setBroadcastCaptureEnabled(bool active,int,int,bool audio) override{
        enabled=active;audioEnabled=audio;if(active){videoCursor=-1;audioCursor=0;sample=0;elapsed.restart();}
    }
    RelayBroadcastMedia takeBroadcastMedia() override{
        RelayBroadcastMedia result;if(!enabled)return result;
        const auto videoNow=int(elapsed.elapsed()*60/1000);
        if(videoNow!=videoCursor){
            videoCursor=videoNow;QImage picture(1280,720,QImage::Format_RGB32);
            // Moving color/texture content, generated without a capture device.
            // This enters the same encoder and transport as a screen/window.
            for(int y=0;y<picture.height();++y){
                auto pixels=reinterpret_cast<QRgb*>(picture.scanLine(y));
                for(int x=0;x<picture.width();++x){
                    quint32 hash=quint32(x+y*picture.width()+videoCursor*999983)*0x9e3779b9u;
                    hash^=hash>>16;hash*=0x85ebca6bu;hash^=hash>>13;
                    pixels[x]=noise?qRgb(hash&255,(hash>>8)&255,(hash>>16)&255)
                        :qRgb((x+videoCursor*5)&255,(y+videoCursor*3)&255,((x^y)+videoCursor*7)&255);
                }
            }
            result.video=QVideoFrame(picture);
        }
        if(audioEnabled)while(audioCursor<=elapsed.elapsed()/20 && result.audio.size()<3840*5){
            const auto offset=result.audio.size();result.audio.resize(offset+3840);
            for(int i=0;i<960;++i,++sample){
                const qint16 value=qint16(6000*std::sin(sample*2*3.141592653589793*440/48000));
                std::memcpy(result.audio.data()+offset+i*4,&value,2);std::memcpy(result.audio.data()+offset+i*4+2,&value,2);
            }
            ++audioCursor;
        }
        return result;
    }
};
// Generated wire-rate source: actual decodable H.264 plus legal filler NALs.
// The filler exercises packet pacing at 24 Mb/s without using personal media
// or turning a transport test into an encoder/CPU benchmark.
class PacedEncodedSource : public QObject, public RelayBroadcastSourceInterface, public RelayEncodedBroadcastSourceInterface {
    Q_OBJECT
    Q_INTERFACES(RelayBroadcastSourceInterface RelayEncodedBroadcastSourceInterface)
public:
    QElapsedTimer elapsed;QByteArray picture;bool enabled=false,audioEnabled=false,provideFallback=false,converted=false;
    int videoCursor=0,audioCursor=0;quint64 sample=0;
    explicit PacedEncodedSource(int bitrate=24000000){
        picture=QByteArray::fromHex("000000010c")+QByteArray(bitrate/8/60-100,char(0xff))+QByteArray(1,char(0x80))
            +StreamTestMedia::encodedFrame("H264");
    }
    void setBroadcastContext(const QVariantMap &) override {}
    void setBroadcastCaptureEnabled(bool active,int height,int fps,bool audio) override{
        const bool wasEnabled=enabled;enabled=active;audioEnabled=audio;converted=height>0 || fps>0;
        if(active && !wasEnabled){videoCursor=audioCursor=0;sample=0;elapsed.restart();}
    }
    RelayEncodedVideo takeEncodedBroadcastVideo() override{
        RelayEncodedVideo result;if(!enabled)return result;
        result.available=true;result.width=320;result.height=180;result.fps=60;
        const auto due=int(elapsed.elapsed()*60/1000);
        while(videoCursor<=due && result.frames.size()<16){
            result.frames.append({picture,quint32(videoCursor*1500),true});++videoCursor;
        }
        return result;
    }
    RelayBroadcastMedia takeBroadcastMedia() override{
        RelayBroadcastMedia result;if(!enabled)return result;
        if(provideFallback && converted){
            QImage image(320,180,QImage::Format_RGB32);image.fill(QColor(180+int(elapsed.elapsed()/20%40),40,60));
            result.video=QVideoFrame(image);
        }
        if(!audioEnabled)return result;
        const auto due=int(elapsed.elapsed()/20);
        while(audioCursor<=due && result.audio.size()<3840*5){
            const auto offset=result.audio.size();result.audio.resize(offset+3840);
            for(int i=0;i<960;++i,++sample){
                const qint16 value=qint16(6000*std::sin(sample*2*3.141592653589793*440/48000));
                std::memcpy(result.audio.data()+offset+i*4,&value,2);std::memcpy(result.audio.data()+offset+i*4+2,&value,2);
            }
            ++audioCursor;
        }
        return result;
    }
};
class BroadcastFixture : public QObject {
public:
    QWebSocketServer server{"Stream fixture",QWebSocketServer::NonSecureMode};QUdpSocket udp;QPointer<QWebSocket> socket;
    QByteArray key=QByteArray(32,'S');QHostAddress clientAddress;quint16 clientPort=0,sequence=0;
    struct SenderClock { quint32 source=0, timestamp=0; quint64 ntp=0; };
    QList<SenderClock> senderClocks;
    QList<VideoPacket::Rtp> media; int senderReports=0; bool announced=false;QJsonObject rendition;
    quint64 wireBytes=0,linkDropped=0;int linkBitrate=0;double linkCredit=12000;QElapsedTimer linkClock;qint64 linkLastUs=0;
    void capLink(int bits){linkBitrate=bits;linkCredit=12000;linkLastUs=0;linkClock.start();}
    QString codec="H264",error;bool createCommit=false,identified=false,selected=false,groupReady=false,addingMember=false,holdTransition=false;
    int pendingTransition=-1;
    int feedbackCount=0,pliCount=0,nackCount=0,keepAlives=0,invalidDatagrams=0;quint32 reportSource=0,reportLsr=0,senderReportLsr=0;
#ifdef RELAY_TEST_MLSPP
    std::unique_ptr<StreamTestDave::Group> group;
#endif
    bool listen(){
        if(!server.listen(QHostAddress::LocalHost,0)||!udp.bind(QHostAddress::LocalHost,0))return false;
        connect(&server,&QWebSocketServer::newConnection,this,[this]{socket=server.nextPendingConnection();socket->setParent(this);
            connect(socket,&QWebSocket::textMessageReceived,this,[this](const QString &text){onText(QJsonDocument::fromJson(text.toUtf8()).object());});
            connect(socket,&QWebSocket::binaryMessageReceived,this,[this](const QByteArray &bytes){onBinary(bytes);});send(8,{{"heartbeat_interval",1000}});});
        connect(&udp,&QUdpSocket::readyRead,this,[this]{while(udp.hasPendingDatagrams()){
            const auto datagram=udp.receiveDatagram();const auto packet=datagram.data();
            if(packet.size()==74 && qFromBigEndian<quint16>(packet.constData())==1){
                clientAddress=datagram.senderAddress();clientPort=datagram.senderPort();QByteArray response(74,'\0');
                qToBigEndian<quint16>(2,response.data());qToBigEndian<quint16>(70,response.data()+2);qToBigEndian<quint32>(101,response.data()+4);
                std::memcpy(response.data()+8,"127.0.0.1",9);qToBigEndian(clientPort,response.data()+72);udp.writeDatagram(response,clientAddress,clientPort);
            }else if(packet.size()==8 && packet.left(4)==QByteArray::fromHex("1337cafe")){
                ++keepAlives;udp.writeDatagram(QByteArray::fromHex("1337f00d")+packet.mid(4),datagram.senderAddress(),datagram.senderPort());
            }else if(auto plain=VideoPacket::decryptRtcp(packet,key)){
                if(quint8((*plain)[1])==200 && plain->size()>=28) senderClocks.append({qFromBigEndian<quint32>(plain->constData()+4),qFromBigEndian<quint32>(plain->constData()+16),qFromBigEndian<quint64>(plain->constData()+8)});
                ++feedbackCount;if(quint8((*plain)[1])==200){++senderReports;if(plain->size()>=28 && qFromBigEndian<quint32>(plain->constData()+4)==202)senderReportLsr=(qFromBigEndian<quint32>(plain->constData()+8)<<16)|(qFromBigEndian<quint32>(plain->constData()+12)>>16);}if(quint8((*plain)[1])==206)++pliCount;if(quint8((*plain)[1])==205)++nackCount;
                if(quint8((*plain)[1])==201 && plain->size()>=32){reportSource=qFromBigEndian<quint32>(plain->constData()+8);reportLsr=qFromBigEndian<quint32>(plain->constData()+24);}
            }else if(auto rtp=VideoPacket::decryptRtp(packet,key)){
                wireBytes+=packet.size();
                if(linkBitrate){
                    const auto now=linkClock.nsecsElapsed()/1000;
                    linkCredit=std::min(12000.0,linkCredit+std::max<qint64>(0,now-linkLastUs)*double(linkBitrate)/8000000);linkLastUs=now;
                    if(linkCredit<packet.size()){++linkDropped;continue;}
                    linkCredit-=packet.size();
                }
                media.append(*rtp);
            }else ++invalidDatagrams;
        }});return true;
    }
    static QByteArray remb(quint64 bits,quint32 source=202){
        QByteArray packet(24,'\0');packet[0]=char(0x8f);packet[1]=char(206);
        qToBigEndian<quint16>(5,packet.data()+2);std::memcpy(packet.data()+12,"REMB",4);packet[16]=1;
        int exponent=0;while(bits>0x3ffff){bits>>=1;++exponent;}
        packet[17]=char((exponent<<2)|((bits>>16)&3));packet[18]=char((bits>>8)&255);packet[19]=char(bits&255);
        qToBigEndian(source,packet.data()+20);return packet;
    }
    quint32 nextFeedbackNonce=250000;
    void feedback(const QByteArray &plain){
        udp.writeDatagram(VideoPacket::encryptRtcp(plain,key,nextFeedbackNonce++),clientAddress,clientPort);
    }
    QUrl url()const{return QUrl(QString("ws://127.0.0.1:%1/?v=9").arg(server.serverPort()));}
    void send(int op,const QJsonObject &data){if(socket)socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op",op},{"d",data},{"seq",int(++sequence)}}).toJson(QJsonDocument::Compact)));}
    void binary(int op,const QByteArray &data){QByteArray out(3,'\0');qToBigEndian(++sequence,out.data());out[2]=char(op);out+=data;if(socket)socket->sendBinaryMessage(out);}
    void transition(int op,const QByteArray &data,quint16 id){QByteArray out(2,'\0');qToBigEndian(id,out.data());out+=data;binary(op,out);}
    void onText(const QJsonObject &root){const auto data=root.value("d").toObject();switch(root.value("op").toInt()){
        case 0:identified=data.value("server_id").toString()=="5556" && data.value("channel_id").toString()=="6666" && data.value("user_id").toString()=="3333" && data.value("video").toBool() && data.value("streams").toArray().first().toObject().value("type").toString()=="screen";
            send(2,{{"ssrc",101},{"ip","127.0.0.1"},{"port",int(udp.localPort())},{"modes",QJsonArray{"aead_xchacha20_poly1305_rtpsize"}},{"streams",QJsonArray{QJsonObject{{"type","video"},{"quality",100},{"rid","100"},{"ssrc",202},{"rtx_ssrc",203}}}}});break;
        case 1:{selected=false;for(const auto &entry:data.value("codecs").toArray()){const auto c=entry.toObject();if(c.value("name").toString()==codec && !c.value("decode").toBool() && c.value("encode").toBool())selected=true;}
            QJsonArray secret;for(int i=0;i<32;++i)secret.append(int('S'));
#ifdef RELAY_TEST_MLSPP
            group=std::make_unique<StreamTestDave::Group>();send(11,{{"user_ids",QJsonArray{"4444"}}});binary(25,group->externalSender());
#endif
            send(4,{{"mode","aead_xchacha20_poly1305_rtpsize"},{"secret_key",secret},{"audio_codec","opus"},{"video_codec",codec},{"dave_protocol_version",1}});break;
        }
        case 3:send(6,{{"t",data.value("t")}});break;
        case 12:rendition=data.value("streams").toArray().first().toObject();announced=data.value("video_ssrc").toInt()==202 && rendition.value("type").toString()=="screen";break;
        case 23:pendingTransition=data.value("transition_id").toInt();if(!holdTransition)send(22,{{"transition_id",pendingTransition}});break;
        default:break;
    }}
    void onBinary(const QByteArray &message){
#ifdef RELAY_TEST_MLSPP
        if(message.isEmpty() || !group)return;
        try{
            if(quint8(message[0])==26){
                if(createCommit)binary(27,group->propose(group->keyPackage()));
                else{const auto combined=group->commitFor(message.mid(1));auto [commit,welcome]=group->split(combined);
                    if(!group->accept(commit,true)||!group->prepare(codec)){error="Fixture could not establish broadcaster group";return;}
                    groupReady=true;transition(30,welcome,0);}
            }else if(quint8(message[0])==28 && addingMember){auto [commit,welcome]=group->split(message.mid(1));Q_UNUSED(welcome);
                if(!group->accept(commit,true)){error="Fixture could not advance the existing broadcaster group";return;}
                addingMember=false;transition(29,commit,9);
            }else if(quint8(message[0])==28){auto [commit,welcome]=group->split(message.mid(1));
                if(!group->accept(welcome,false)||!group->prepare(codec)){error="Fixture could not accept broadcaster welcome";return;}
                groupReady=true;transition(29,commit,7);}
        }catch(const std::exception &exception){error="Fixture MLS parse failed: "+QString::fromUtf8(exception.what());}
#else
        Q_UNUSED(message);
#endif
    }
    bool addMemberWithDelayedExecution(){
#ifdef RELAY_TEST_MLSPP
        auto newcomer=daveSessionCreate(nullptr,nullptr,nullptr,nullptr);if(!newcomer)return false;
        const auto cleanup=qScopeGuard([&]{daveSessionDestroy(newcomer);});
        const auto external=group->externalSender();daveSessionSetExternalSender(newcomer,StreamTestDave::bytes(external),external.size());
        daveSessionInit(newcomer,1,StreamTestDave::groupId,"7777");
        uint8_t *data=nullptr;size_t length=0;daveSessionGetMarshalledKeyPackage(newcomer,&data,&length);
        const QByteArray keyPackage(reinterpret_cast<const char*>(data),length);daveFree(data);if(keyPackage.isEmpty())return false;
        const auto proposal=mlspp::Proposal{mlspp::Add{{mlspp::tls::get<mlspp::KeyPackage>(mlspp::bytes_ns::bytes(StreamTestDave::Group::vector(keyPackage)))}}};
        const auto message=mlspp::external_proposal(group->suite,group->group,1,proposal,0,group->signing);
        mlspp::tls::ostream serialized;serialized<<false;serialized<<std::vector<mlspp::MLSMessage>{message};
        const auto marshalled=StreamTestDave::Group::array(serialized.bytes());const char *ids[]={"3333","4444","7777"};
        uint8_t *candidate=nullptr;size_t candidateLength=0;
        daveSessionProcessProposals(group->session,StreamTestDave::bytes(marshalled),marshalled.size(),ids,3,&candidate,&candidateLength);
        const bool prepared=candidate && candidateLength;daveFree(candidate);if(!prepared)return false;
        holdTransition=true;addingMember=true;pendingTransition=-1;send(11,{{"user_ids",QJsonArray{"7777"}}});
        binary(27,marshalled);return true;
#else
        return false;
#endif
    }
    void announce(){send(12,{{"user_id","4444"},{"audio_ssrc",303},{"video_ssrc",202},{"rtx_ssrc",203},
        {"streams",QJsonArray{QJsonObject{{"type","video"},{"rid","100"},{"quality",100},{"active",true},{"ssrc",202},{"rtx_ssrc",203}}}}});}
};
}

class BroadcastGatewayTest final : public QObject {
    Q_OBJECT
private slots:
    void receiverBudgetUsesOnlyValidCurrentSourceFeedback(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        BroadcastFixture gateway;QVERIFY(gateway.listen());StreamEngine stream;
        QVERIFY(stream.configureLocalFixture(gateway.url()));stream.setIdentity("3333","fixture-session");
        connect(&stream,&StreamEngine::watchRequested,&stream,[&](const QString&){
            stream.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
            stream.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
        });
        QSignalSpy budget(&stream,&StreamEngine::publishingBitrateChanged);
        stream.publish(streamKey,2560,1440,60,40000000);
        QVERIFY(waitNative([&]{return stream.encryptedReady() && gateway.announced;},3000));
        budget.clear();QCOMPARE(gateway.rendition.value("max_bitrate").toInt(),40000000);
        gateway.feedback(BroadcastFixture::remb(1000000,999));
        auto missing=BroadcastFixture::remb(1000000);missing.resize(20);qToBigEndian<quint16>(4,missing.data()+2);
        gateway.feedback(missing);
        auto excessive=BroadcastFixture::remb(1000000);excessive[17]=char(0xff);excessive[18]=char(0xff);excessive[19]=char(0xff);
        gateway.feedback(excessive);QTest::qWait(60);
        QCOMPARE(budget.size(),0);QCOMPARE(stream.publishingDiagnostics().value("rembReports").toULongLong(),quint64(0));
        gateway.feedback(BroadcastFixture::remb(9000000));
        QVERIFY(waitNative([&]{return !budget.isEmpty();},1000));
        QCOMPARE(budget.last().first().toInt(),6944000);
        QTRY_COMPARE(gateway.rendition.value("max_bitrate").toInt(),6944000);
        const int afterReduction=budget.size();
        gateway.feedback(BroadcastFixture::remb(9000000));QTest::qWait(60);QCOMPARE(budget.size(),afterReduction);
        // One report cannot immediately restore the previous excessive rate.
        gateway.feedback(BroadcastFixture::remb(100000000));QTest::qWait(60);
        QCOMPARE(budget.size(),afterReduction);
        QTest::qWait(2050);gateway.feedback(BroadcastFixture::remb(100000000));
        QVERIFY(waitNative([&]{return budget.size()>afterReduction;},1000));
        QVERIFY(budget.last().first().toInt()<=8680000);
        QVERIFY(budget.last().first().toInt()>6944000);
        const auto idr=StreamTestMedia::encodedFrame("H264");QVERIFY(!idr.isEmpty());
        gateway.feedback(BroadcastFixture::remb(0));
        QVERIFY(waitNative([&]{return stream.publishingDiagnostics().value("bandwidthPaused").toBool();},1000));
        QVERIFY(!stream.publishVideo(idr,9000,320,180));
        gateway.feedback(BroadcastFixture::remb(300000));QTest::qWait(40);
        QVERIFY(stream.publishingDiagnostics().value("bandwidthPaused").toBool());
        QVERIFY(!stream.publishVideo(idr,12000,320,180));
        const auto resets=stream.publishingDiagnostics().value("congestionResets").toULongLong();
        for(int i=0;i<10;++i)QVERIFY(!stream.publishVideo(idr,15000+i*3000,320,180));
        QCOMPARE(stream.publishingDiagnostics().value("congestionResets").toULongLong(),resets);
        QSignalSpy resumeKeyframe(&stream,&StreamEngine::keyFrameRequested);
        gateway.feedback(BroadcastFixture::remb(9000000));
        QVERIFY(waitNative([&]{return !stream.publishingDiagnostics().value("bandwidthPaused").toBool();},1000));
        QVERIFY(!resumeKeyframe.isEmpty());QVERIFY(stream.publishVideo(idr,48000,320,180));
        QVERIFY(waitNative([&]{return stream.frameCount()>0;},1000));
        stream.stop();budget.clear();gateway.announced=false;
        stream.publish(streamKey,1280,720,30,6000000);
        QVERIFY(waitNative([&]{return stream.encryptedReady() && gateway.announced;},3000));
        QCOMPARE(gateway.rendition.value("max_bitrate").toInt(),6000000);
        QCOMPARE(stream.publishingDiagnostics().value("rembReports").toULongLong(),quint64(0));
        budget.clear();gateway.feedback(BroadcastFixture::remb(100000000));QTest::qWait(60);
        QCOMPARE(gateway.rendition.value("max_bitrate").toInt(),6000000);
        for(const auto &entry:budget)QVERIFY(entry.first().toInt()<=6000000);
        stream.stop();
#endif
    }

    void budgetDownshiftRetiresQueuedTailWithoutRtpSequenceHole(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        BroadcastFixture gateway;QVERIFY(gateway.listen());StreamEngine stream;
        QVERIFY(stream.configureLocalFixture(gateway.url()));stream.setIdentity("3333","fixture-session");
        connect(&stream,&StreamEngine::watchRequested,&stream,[&](const QString&){
            stream.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
            stream.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
        });
        stream.publish(streamKey,320,180,30,40000000);
        QVERIFY(waitNative([&]{return stream.encryptedReady();},3000));
        const auto idr=StreamTestMedia::encodedFrame("H264");QVERIFY(!idr.isEmpty());
        QVERIFY(stream.publishVideo(idr,3000,320,180));
        QVERIFY(waitNative([&]{return stream.frameCount()>=1;},1000));
        const auto oversized=QByteArray::fromHex("000000010c")+QByteArray(200000,char(0xff))+QByteArray(1,char(0x80))+idr;
        for(int i=0;i<6;++i)QVERIFY(stream.publishVideo(oversized,6000+i*3000,320,180));
        QSignalSpy requested(&stream,&StreamEngine::keyFrameRequested);
        gateway.feedback(BroadcastFixture::remb(1000000));
        QVERIFY(waitNative([&]{return stream.publishingDiagnostics().value("congestionResets").toULongLong()>0;},1000));
        QVERIFY(!requested.isEmpty());QVERIFY(stream.publishingDiagnostics().value("awaitingPublishKeyframe").toBool());
        QCOMPARE(stream.publishingDiagnostics().value("queuedPackets").toInt(),0);
        QByteArray interframe;
        for(auto nalu:StreamTestMedia::nalus(idr)){
            if((quint8(nalu[0])&31)==5)nalu[0]=char((quint8(nalu[0])&0xe0)|1);
            interframe+=QByteArray::fromHex("00000001");interframe+=nalu;
        }
        QVERIFY(!stream.publishVideo(interframe,27000,320,180));
        QVERIFY(stream.publishingDiagnostics().value("congestionGated").toULongLong()>0);
        QVERIFY(stream.publishVideo(idr,30000,320,180));
        QVERIFY(!stream.publishingDiagnostics().value("awaitingPublishKeyframe").toBool());
        QVERIFY(waitNative([&]{return !gateway.media.isEmpty() && gateway.media.last().timestamp==30000 && gateway.media.last().marker;},2000));
        bool have=false;quint16 next=0;QSet<quint32> nonces;
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);QByteArray resumed;
        for(const auto &packet:gateway.media)if(packet.payloadType==105){
            if(have)QCOMPARE(packet.sequence,next);have=true;next=quint16(packet.sequence+1);
            QVERIFY(!nonces.contains(packet.nonce));nonces.insert(packet.nonce);
            if(auto complete=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp))
                if(packet.timestamp==30000)resumed=*complete;
        }
        QVERIFY(!resumed.isEmpty());
        auto recipient=daveDecryptorCreate();QVERIFY(recipient);
        const auto cleanup=qScopeGuard([&]{daveDecryptorDestroy(recipient);});
        auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(ratchet);
        daveDecryptorTransitionToKeyRatchet(recipient,ratchet);daveKeyRatchetDestroy(ratchet);
        QByteArray clear(resumed.size(),Qt::Uninitialized);size_t length=0;
        QCOMPARE(daveDecryptorDecrypt(recipient,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(resumed),resumed.size(),StreamTestDave::bytes(clear),clear.size(),&length),DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);
        clear.resize(length);QCOMPARE(clear,idr);
        StreamDecoder decoder("H264");QVERIFY(decoder.submit(clear,30000));
        StreamDecoder::Snapshot picture;
        QVERIFY(waitNative([&]{if(!picture.frame.isValid())picture=decoder.take();return picture.frame.isValid();},2000));
        QCOMPARE(picture.frame.size(),QSize(320,180));stream.stop();
#endif
    }

    void originalVideoAdaptsOnlyWhenSourceExceedsReceiverBudget_data(){
        QTest::addColumn<int>("bits");QTest::addColumn<bool>("converted");
        QTest::newRow("fits-preserves-original")<<1000000<<false;
        QTest::newRow("over-budget-converts-once")<<24000000<<true;
    }
    void originalVideoAdaptsOnlyWhenSourceExceedsReceiverBudget(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        QFETCH(int,bits);QFETCH(bool,converted);
        BroadcastFixture gateway;QVERIFY(gateway.listen());PacedEncodedSource source(bits);source.provideFallback=true;BroadcastEngine engine;
        QVERIFY(engine.configureLocalFixture(gateway.url()));engine.setIdentity("3333","fixture-session");
        connect(&engine,&BroadcastEngine::startRequested,&engine,[&](const QString&,const QString&){
            QTimer::singleShot(0,&engine,[&]{
                engine.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
                engine.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
            });
        });
        engine.startExternal("1111","2222",&source,0,0,false);
        QVERIFY2(waitNative([&]{return engine.framesSent()>=4;},3000),qPrintable(engine.error()+" "+gateway.error));
        QCOMPARE(engine.publishingDiagnostics().value("sourcePath").toInt(),3);
        gateway.feedback(BroadcastFixture::remb(9000000));
        QVERIFY(waitNative([&]{return engine.publishingDiagnostics().value("targetBitrate").toInt()==6944000;},1000));
        QElapsedTimer duration;duration.start();
        QVERIFY(waitNative([&]{gateway.media.clear();return duration.elapsed()>=4500;},5000));
        const auto state=engine.publishingDiagnostics();
        QCOMPARE(state.value("sourcePath").toInt(),converted?2:3);
        QCOMPARE(state.value("originalRecoveries").toULongLong(),quint64(converted?1:0));
        const auto frames=engine.framesSent();
        QVERIFY2(waitNative([&]{gateway.media.clear();return engine.framesSent()>frames+30;},1500),qPrintable(engine.diagnosticSummary()));
        QCOMPARE(engine.publishingDiagnostics().value("originalRecoveries").toULongLong(),quint64(converted?1:0));
        QVERIFY(engine.sharing());QVERIFY2(engine.error().isEmpty(),qPrintable(engine.error()));
        engine.stop();
#endif
    }

    void changingReceiverBudgetKeepsActualEncodedVideoPlaying(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        BroadcastFixture gateway;QVERIFY(gateway.listen());PacedRawSource source;source.noise=true;BroadcastEngine engine;
        QVERIFY(engine.configureLocalFixture(gateway.url()));engine.setIdentity("3333","fixture-session");
        connect(&engine,&BroadcastEngine::startRequested,&engine,[&](const QString&,const QString&){
            QTimer::singleShot(0,&engine,[&]{
                engine.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
                engine.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
            });
        });
        engine.startExternal("1111","2222",&source,720,30,false);
        QVERIFY2(waitNative([&]{return engine.framesSent()>=4;},5000),qPrintable(engine.error()+" "+gateway.error));
        auto recipient=daveDecryptorCreate();QVERIFY(recipient);
        const auto cleanup=qScopeGuard([&]{daveDecryptorDestroy(recipient);});
        auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(ratchet);
        daveDecryptorTransitionToKeyRatchet(recipient,ratchet);daveKeyRatchetDestroy(ratchet);
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);StreamDecoder decoder("H264");
        QElapsedTimer duration;duration.start();QEventLoop loop;QTimer consume;consume.setInterval(1);consume.setTimerType(Qt::PreciseTimer);
        bool reduced=false,capped=false,haveStamp=false,haveSequence=false;quint32 lastStamp=0;quint16 nextSequence=0;
        int phase[3]={},gated=0,lossGaps=0;quint64 bytesBefore=0,bytesAfter=0,decoded=0,wireStart=0,wireEnd=0,dropsAtMeasurement=0;
        qint64 wireStartAt=-1,wireEndAt=-1,lastPli=-1000;
        qint64 maxGap=0,lastPicture=0;QString failure;
        connect(&consume,&QTimer::timeout,&loop,[&]{
            const auto now=duration.elapsed();
            if(now>=30000 || !failure.isEmpty()){loop.quit();return;}
            // A two-Mb/s receiver starts dropping the formerly six-Mb/s stream
            // before its bandwidth report arrives, matching a real bottleneck.
            if(!capped && now>=3000){capped=true;gateway.capLink(2000000);}
            if(!reduced && now>=3500){reduced=true;gateway.feedback(BroadcastFixture::remb(2000000));}
            if(wireStartAt<0 && now>=6000){wireStartAt=now;wireStart=gateway.wireBytes;dropsAtMeasurement=gateway.linkDropped;}
            if(wireEndAt<0 && now>=10000){wireEndAt=now;wireEnd=gateway.wireBytes;}
            auto packets=std::move(gateway.media);gateway.media.clear();
            for(const auto &packet:packets){
                if(packet.payloadType!=105)continue;
                if(haveSequence && packet.sequence!=nextSequence){
                    ++lossGaps;decoder.discardUntilKeyFrame();
                    if(now-lastPli>=200){
                        QByteArray pli(12,'\0');pli[0]=char(0x81);pli[1]=char(206);qToBigEndian<quint16>(2,pli.data()+2);qToBigEndian<quint32>(202,pli.data()+8);
                        gateway.feedback(pli);lastPli=now;
                    }
                }
                haveSequence=true;nextSequence=quint16(packet.sequence+1);
                if(auto complete=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp)){
                    QByteArray clear(complete->size(),Qt::Uninitialized);size_t length=0;
                    if(daveDecryptorDecrypt(recipient,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(*complete),complete->size(),StreamTestDave::bytes(clear),clear.size(),&length)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS){
                        if(now<6000 && gateway.linkDropped){decoder.discardUntilKeyFrame();++gated;continue;}
                        failure="Video decryption failed after bitrate recovery";break;
                    }
                    clear.resize(length);
                    if(haveStamp && qint32(packet.timestamp-lastStamp)<=0){failure="Video timestamp moved backwards during bitrate change";break;}
                    haveStamp=true;lastStamp=packet.timestamp;
                    if(!decoder.submit(clear,packet.timestamp)){++gated;continue;}
                    ++phase[now<3000?0:now<10000?1:2];
                    if(now>=1000 && now<3000)bytesBefore+=clear.size();
                    if(now>=6000 && now<10000)bytesAfter+=clear.size();
                }
            }
            const auto picture=decoder.take();decoded=picture.decoded;
            if(!picture.error.isEmpty())failure=picture.error;
            if(picture.frame.isValid()){
                if(lastPicture)maxGap=std::max(maxGap,now-lastPicture);
                lastPicture=now;
                if(picture.frame.size()!=QSize(1280,720))failure="Receiver budget changed the chosen resolution";
            }
            if(!engine.error().isEmpty())failure=engine.error();if(!gateway.error.isEmpty())failure=gateway.error;
        });
        consume.start();loop.exec();consume.stop();
        const auto measuredWireRate=wireEndAt>wireStartAt?(wireEnd-wireStart)*8000/quint64(wireEndAt-wireStartAt):0;
        qInfo("ReceiverBudgetNoise720p30 beforeBitsPerSecond=%llu afterBitsPerSecond=%llu wireBitsPerSecond=%llu dropped=%llu lossGaps=%d gated=%d decoded=%llu gapMs=%lld phases=%d/%d/%d diagnostics=%s",
            qulonglong(bytesBefore*8/2),qulonglong(bytesAfter*8/4),qulonglong(measuredWireRate),qulonglong(gateway.linkDropped),lossGaps,gated,qulonglong(decoded),qlonglong(maxGap),phase[0],phase[1],phase[2],qPrintable(engine.diagnosticSummary()));
        QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY(reduced);QVERIFY(duration.elapsed()>=30000);
        QCOMPARE(engine.publishingDiagnostics().value("targetBitrate").toInt(),1344000);
        QVERIFY2(bytesBefore>500000,"Generated noise must fill enough of the initial rate to exercise an actual downshift");
        QVERIFY2(bytesAfter*8/4<1900000,"Actual encoded payload must fit the smaller receiver budget");
        QVERIFY2(bytesAfter<bytesBefore,"Four seconds after downshift must use less than two seconds before it");
        QVERIFY(phase[0]>45);QVERIFY(phase[1]>100);QVERIFY(phase[2]>450);QVERIFY(decoded>650);
        QVERIFY(gateway.linkDropped>0);QVERIFY(lossGaps>0);QVERIFY(engine.publishingDiagnostics().value("keyframeRequests").toULongLong()>0);
        QVERIFY2(measuredWireRate>0 && measuredWireRate<=2000000,"Actual encrypted RTP wire rate must fit the receiver grant");
        QCOMPARE(gateway.linkDropped,dropsAtMeasurement);
        QVERIFY2(maxGap<1800,"Receiver must not freeze through encoder rate changes");
        QVERIFY(engine.sharing());QCOMPARE(gateway.invalidDatagrams,0);engine.stop();
#endif
    }

    void micTestAudioGateKeepsVideoAndDropsFeedbackTail(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        BroadcastFixture gateway;QVERIFY(gateway.listen());BroadcastEngine engine;GeneratedBroadcastSource source;
        QVERIFY(engine.available());QVERIFY(engine.configureLocalFixture(gateway.url()));engine.setIdentity("3333","fixture-session");
        connect(&engine,&BroadcastEngine::startRequested,&engine,[&](const QString&,const QString&){
            QTimer::singleShot(0,&engine,[&]{
                engine.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
                engine.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
            });
        });
        auto audioCount=[&]{return std::count_if(gateway.media.cbegin(),gateway.media.cend(),[](const auto &packet){return packet.payloadType==120;});};
        engine.startExternal("1111","2222",&source,720,30,true);
        QVERIFY2(waitNative([&]{return engine.framesSent()>=4 && audioCount()>=4;},5000),qPrintable(engine.diagnosticSummary()+" "+engine.error()+" "+gateway.error));
        QVERIFY(engine.sharing());QVERIFY(engine.audioActive());
        engine.setAudioSuppressed(true);QVERIFY(engine.audioSuppressed());QVERIFY(!engine.audioActive());
        source.frequency=880;
        // Allow packets already on the wire before the gate to reach the local receiver.
        QVERIFY(waitNative([&]{return !gateway.udp.hasPendingDatagrams();},100));QTest::qWait(30);
        const auto beforeAudio=audioCount();const auto beforeVideo=engine.framesSent(),beforeTakes=source.takes;
        QTest::qWait(200);QCOMPARE(audioCount(),beforeAudio);QVERIFY(engine.framesSent()>beforeVideo);QVERIFY(source.takes>beforeTakes);
        QVERIFY(source.capturing);QVERIFY(source.requestedAudio);QVERIFY(engine.active());
        engine.setAudioSuppressed(false);QVERIFY(engine.audioSuppressed());
        QTest::qWait(220);QVERIFY(engine.audioSuppressed());QCOMPARE(audioCount(),beforeAudio);
        // Keep one feedback batch queued while the GUI passes the tail deadline.
        // The first resumed poll must discard it before opening the audio gate.
        source.queuedFeedback=true;source.offerAudio=false;QThread::msleep(120);
        QTRY_VERIFY_WITH_TIMEOUT(!engine.audioSuppressed(),1000);QTest::qWait(120);
        QCOMPARE(audioCount(),beforeAudio);QVERIFY(!engine.audioActive());QVERIFY(engine.framesSent()>beforeVideo+4);
        source.frequency=1320;source.offerAudio=true;
        QVERIFY(waitNative([&]{return audioCount()>=beforeAudio+4;},1500));QVERIFY(engine.audioActive());
        auto decryptor=daveDecryptorCreate();QVERIFY(decryptor);auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(ratchet);
        daveDecryptorTransitionToKeyRatchet(decryptor,ratchet);daveKeyRatchetDestroy(ratchet);
        const auto destroyDecryptor=qScopeGuard([&]{daveDecryptorDestroy(decryptor);});
        int decoderError=0;auto decoder=opus_decoder_create(48000,2,&decoderError);QCOMPARE(decoderError,OPUS_OK);
        const auto destroyDecoder=qScopeGuard([&]{opus_decoder_destroy(decoder);});
        qsizetype audioIndex=0;int samplesSeen=0;double freshSin=0,freshCos=0,feedbackSin=0,feedbackCos=0;
        for(const auto &packet:gateway.media)if(packet.payloadType==120){
            if(audioIndex++<beforeAudio)continue;
            QByteArray opus(packet.payload.size(),Qt::Uninitialized);size_t length=0;
            QCOMPARE(daveDecryptorDecrypt(decryptor,DAVE_MEDIA_TYPE_AUDIO,StreamTestDave::bytes(packet.payload),packet.payload.size(),StreamTestDave::bytes(opus),opus.size(),&length),DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);
            opus.resize(length);std::array<opus_int16,1920> pcm{};QCOMPARE(opus_decode(decoder,StreamTestDave::bytes(opus),opus.size(),pcm.data(),960,0),960);
            for(int i=0;i<960;++i,++samplesSeen){const auto value=pcm[i*2];
                const auto phase=samplesSeen*2*3.141592653589793/48000;
                freshSin+=value*std::sin(1320*phase);freshCos+=value*std::cos(1320*phase);
                feedbackSin+=value*std::sin(880*phase);feedbackCos+=value*std::cos(880*phase);
            }
        }
        QVERIFY(samplesSeen>=3840);QVERIFY(freshSin*freshSin+freshCos*freshCos>10*(feedbackSin*feedbackSin+feedbackCos*feedbackCos));
        QVERIFY(engine.sharing());QVERIFY(source.capturing);QVERIFY(source.requestedAudio);
        QVERIFY2(gateway.error.isEmpty(),qPrintable(gateway.error));QVERIFY2(engine.error().isEmpty(),qPrintable(engine.error()));
        engine.stop();QVERIFY(!source.capturing);
#endif
    }
    void rtmpInputToEncryptedBroadcast_data(){
        QTest::addColumn<bool>("includeAudio");QTest::addColumn<bool>("autoQuality");QTest::addColumn<bool>("original");QTest::addColumn<QSize>("size");QTest::addColumn<int>("fps");QTest::addColumn<int>("bFrames");QTest::addColumn<QString>("pixelFormat");
        QTest::newRow("video-and-audio")<<true<<false<<false<<QSize(1920,1080)<<15<<0<<QString("yuv420p");
        QTest::newRow("video-only")<<false<<false<<false<<QSize(1920,1080)<<15<<0<<QString("yuv420p");
        QTest::newRow("original-1080p60")<<true<<true<<true<<QSize(1920,1080)<<60<<0<<QString("yuv420p");
        QTest::newRow("original-4k60")<<false<<true<<true<<QSize(3840,2160)<<60<<0<<QString("yuv420p");
        QTest::newRow("b-frame-compatible-reencode")<<true<<true<<false<<QSize(1280,720)<<30<<2<<QString("yuv420p");
        QTest::newRow("source-format-conversion")<<true<<true<<false<<QSize(1280,720)<<15<<0<<QString("yuv444p");
    }
    void rtmpInputToEncryptedBroadcast(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        if(QStandardPaths::findExecutable("ffmpeg").isEmpty())QSKIP("FFmpeg generates the RTMP source");
        QFETCH(bool,includeAudio);QFETCH(bool,autoQuality);QFETCH(bool,original);QFETCH(QSize,size);QFETCH(int,fps);QFETCH(int,bFrames);QFETCH(QString,pixelFormat);
        RtmpPlaybackFixture feed;feed.size=size;feed.fps=fps;feed.bFrames=bFrames;feed.pixelFormat=pixelFormat;feed.motion=original&&size.width()==1920;feed.start();
        QVERIFY(feed.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(feed.listening(),3000);
        ObservedSource observed;auto &source=observed.source;source.setInputMode(StudioBridge::ExistingRtmpStream);source.setRemoteServer(feed.server());source.setRemoteStreamKey("fixture-key");source.start();
        QTRY_VERIFY_WITH_TIMEOUT(source.receiving() || !source.error().isEmpty(),10000);QVERIFY2(source.receiving(),qPrintable(source.error()));
        BroadcastFixture gateway;QVERIFY(gateway.listen());BroadcastEngine engine;QVERIFY(engine.available());
        QVERIFY(engine.configureLocalFixture(gateway.url()));engine.setIdentity("3333","fixture-session");
        connect(&engine,&BroadcastEngine::startRequested,&engine,[&](const QString &guild,const QString &channel){
            QCOMPARE(guild,QString("1111"));QCOMPARE(channel,QString("2222"));
            QTimer::singleShot(0,&engine,[&]{engine.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});engine.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});});
        });
        engine.startExternal("1111","2222",&observed,autoQuality?0:1080,autoQuality?0:30,includeAudio);
        QTRY_VERIFY_WITH_TIMEOUT(engine.framesSent()>=2 || !engine.error().isEmpty() || !gateway.error.isEmpty(),10000);
        QVERIFY2(engine.error().isEmpty(),qPrintable(engine.error()));QVERIFY2(gateway.error.isEmpty(),qPrintable(gateway.error));QVERIFY(engine.framesSent()>=2);
        QVERIFY(engine.sharing());QVERIFY(engine.externalSource());QCOMPARE(QSize(engine.videoWidth(),engine.videoHeight()),size);
        QCOMPARE(engine.qualitySummary().contains("Original video"),original);
        if(autoQuality)QCOMPARE(gateway.rendition.value("max_framerate").toInt(),fps);
        if(original){QCOMPARE(gateway.rendition.value("max_framerate").toInt(),fps);QCOMPARE(gateway.rendition.value("max_resolution").toObject().value("height").toInt(),size.height());}
        if(original && fps==60){
            QVERIFY2(waitNative([&]{return engine.framesSent()>=65;},4000),qPrintable(engine.diagnosticSummary()));
            QList<quint32> stamps;for(const auto &packet:gateway.media)if(packet.payloadType==105 && packet.marker)stamps.append(packet.timestamp);
            QVERIFY(stamps.size()>=60);
            for(qsizetype i=1;i<stamps.size();++i)QVERIFY2(qAbs(int(stamps[i]-stamps[i-1])-1500)<=90,"Original 60 fps must retain every source presentation timestamp");
        }
        auto decryptor=daveDecryptorCreate();QVERIFY(decryptor);auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(ratchet);
        daveDecryptorTransitionToKeyRatchet(decryptor,ratchet);daveKeyRatchetDestroy(ratchet);
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);QByteArray video;quint32 videoTimestamp=0;
        for(const auto &packet:gateway.media)if(packet.payloadType==105){if(auto complete=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp)){video=*complete;videoTimestamp=packet.timestamp;break;}}
        QVERIFY(!video.isEmpty());QByteArray decoded(video.size(),Qt::Uninitialized);size_t length=0;
        QCOMPARE(daveDecryptorDecrypt(decryptor,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(video),video.size(),StreamTestDave::bytes(decoded),decoded.size(),&length),DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);decoded.resize(length);
        if(original){QVERIFY(observed.original.contains(videoTimestamp));QCOMPARE(decoded,observed.original.value(videoTimestamp));}
        StreamDecoder decoder("H264");QVERIFY(decoder.submit(decoded,videoTimestamp));StreamDecoder::Snapshot picture;
        // Every transmitted path is now realtime-compatible: even a B-frame
        // source produces a picture immediately from its first encoded IDR.
        QTRY_VERIFY_WITH_TIMEOUT(picture.frame.isValid() || (picture=decoder.take(),picture.frame.isValid()),5000);QCOMPARE(picture.frame.size(),size);QVERIFY(picture.frame.toImage().pixelColor(size.width()/2,size.height()/2).red()>150);
        auto audioPacket=[&]() -> QByteArray {for(const auto &packet:gateway.media)if(packet.payloadType==120)return packet.payload;return {};};
        if(includeAudio){
            QTRY_VERIFY_WITH_TIMEOUT(!audioPacket().isEmpty(),3000);const auto audio=audioPacket();decoded.resize(audio.size());length=0;
            QCOMPARE(daveDecryptorDecrypt(decryptor,DAVE_MEDIA_TYPE_AUDIO,StreamTestDave::bytes(audio),audio.size(),StreamTestDave::bytes(decoded),decoded.size(),&length),DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);decoded.resize(length);
            int error=0;auto opus=opus_decoder_create(48000,2,&error);QCOMPARE(error,OPUS_OK);std::array<opus_int16,1920> samples{};
            QCOMPARE(opus_decode(opus,reinterpret_cast<const unsigned char *>(decoded.constData()),decoded.size(),samples.data(),960,0),960);opus_decoder_destroy(opus);
            QVERIFY(std::any_of(samples.cbegin(),samples.cend(),[](auto sample){return qAbs(int(sample))>100;}));QVERIFY(engine.audioActive());
        }else{QTest::qWait(300);QVERIFY(audioPacket().isEmpty());QVERIFY(!engine.audioActive());}
        daveDecryptorDestroy(decryptor);QVERIFY(!engine.diagnosticSummary().contains("fixture-key"));QVERIFY(!engine.diagnosticSummary().contains(feed.server()));
        QSignalSpy stopped(&engine,&BroadcastEngine::stopRequested);engine.stop();QCOMPARE(stopped.size(),1);QVERIFY(!engine.active());QVERIFY(source.running());
        QTest::qWait(100);QVERIFY(source.takeBroadcastMedia().audio.isEmpty());source.stop();
#endif
    }

    void rawMotionEncoderKeepsPlayingBeyondTenSeconds(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        BroadcastFixture gateway;QVERIFY(gateway.listen());PacedRawSource source;BroadcastEngine engine;
        QVERIFY(engine.configureLocalFixture(gateway.url()));engine.setIdentity("3333","fixture-session");
        connect(&engine,&BroadcastEngine::startRequested,&engine,[&](const QString&,const QString&){
            QTimer::singleShot(0,&engine,[&]{
                engine.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
                engine.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
            });
        });
        engine.startExternal("1111","2222",&source,720,60,true);
        QVERIFY2(waitNative([&]{return engine.framesSent()>=4;},5000),qPrintable(engine.error()+" "+gateway.error));
        auto recipient=daveDecryptorCreate();QVERIFY(recipient);
        const auto cleanup=qScopeGuard([&]{daveDecryptorDestroy(recipient);});
        auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(ratchet);
        daveDecryptorTransitionToKeyRatchet(recipient,ratchet);daveKeyRatchetDestroy(ratchet);
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);StreamDecoder decoder("H264");
        QEventLoop loop;QTimer consume,busy;consume.setInterval(1);consume.setTimerType(Qt::PreciseTimer);
        busy.setInterval(10);busy.setTimerType(Qt::PreciseTimer);QElapsedTimer duration;duration.start();
        int delivered=0,shown=0,audioPackets=0,phase[3]={};quint64 decoded=0;QString failure;
        connect(&busy,&QTimer::timeout,&loop,[]{QThread::usleep(9000);});
        connect(&consume,&QTimer::timeout,&loop,[&]{
            if(duration.elapsed()>=16000 || !failure.isEmpty()){loop.quit();return;}
            auto packets=std::move(gateway.media);gateway.media.clear();
            for(const auto &packet:packets){
                if(packet.payloadType==120){++audioPackets;continue;}
                if(packet.payloadType!=105)continue;
                if(auto complete=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp)){
                    QByteArray clear(complete->size(),Qt::Uninitialized);size_t length=0;
                    if(daveDecryptorDecrypt(recipient,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(*complete),complete->size(),StreamTestDave::bytes(clear),clear.size(),&length)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS){failure="Video decryption failed";break;}
                    clear.resize(length);if(!decoder.submit(clear,packet.timestamp)){failure="Decoder rejected frame";break;}
                    ++delivered;++phase[std::min(2,int(duration.elapsed()/6000))];
                }
            }
            const auto picture=decoder.take();decoded=picture.decoded;
            if(picture.frame.isValid()){++shown;if(picture.frame.size()!=QSize(1280,720))failure="Encoded resolution changed";}
            if(!engine.error().isEmpty())failure=engine.error();if(!gateway.error.isEmpty())failure=gateway.error;
        });
        consume.start();busy.start();loop.exec();consume.stop();busy.stop();
        qInfo("Generated720p60RawGuiBusy delivered=%d decoded=%llu observed=%d audio=%d phases=%d/%d/%d diagnostics=%s",delivered,qulonglong(decoded),shown,audioPackets,phase[0],phase[1],phase[2],qPrintable(engine.diagnosticSummary()));
        QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY(duration.elapsed()>=16000);
        // The isolated six-CPU runner also generates and decodes this source.
        // Each phase must keep producing real decoded motion, including >10s.
        QVERIFY(phase[0]>180);QVERIFY(phase[1]>180);QVERIFY(phase[2]>120);
        QVERIFY(decoded>480);QVERIFY(shown>400);QVERIFY(audioPackets>650);
        QVERIFY(engine.sharing());QVERIFY(engine.qualitySummary().contains("High quality encoding"));
        engine.stop();
#endif
    }

    void workerStopAndRestartRetirePreviousSessionCallbacks(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        BroadcastFixture gateway;QVERIFY(gateway.listen());PacedEncodedSource source;BroadcastEngine engine;
        QVERIFY(engine.configureLocalFixture(gateway.url()));engine.setIdentity("3333","fixture-session");
        connect(&engine,&BroadcastEngine::startRequested,&engine,[&](const QString&,const QString&){
            QTimer::singleShot(0,&engine,[&]{
                engine.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
                engine.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
            });
        });
        QSignalSpy stopped(&engine,&BroadcastEngine::stopRequested);
        for(int attempt=0;attempt<3;++attempt){
            gateway.media.clear();gateway.error.clear();
            engine.startExternal("1111","2222",&source,0,0,true);
            QVERIFY2(waitNative([&]{return engine.framesSent()>=4;},3000),qPrintable(engine.error()+" "+gateway.error));
            QVERIFY(engine.sharing());QVERIFY(engine.active());QVERIFY(source.enabled);
            // Queue a worker-to-UI keyframe notification immediately before
            // stopping, then leave it pending until a new session starts.
            QByteArray pli(12,'\0');pli[0]=char(0x81);pli[1]=char(206);qToBigEndian<quint16>(2,pli.data()+2);
            qToBigEndian<quint32>(202,pli.data()+8);
            gateway.udp.writeDatagram(VideoPacket::encryptRtcp(pli,gateway.key,quint32(220000+attempt)),gateway.clientAddress,gateway.clientPort);
            QThread::msleep(20);engine.stop();
            QVERIFY(!engine.active());QVERIFY(!source.enabled);QCOMPARE(stopped.size(),attempt+1);
            if(attempt<2){
                // Do not process GUI events between Stop and the replacement:
                // stale queued worker signals now meet a live new generation.
                engine.startExternal("1111","2222",&source,0,0,true);
                QVERIFY(engine.active());QCOMPARE(stopped.size(),attempt+1);
            }else{
                // Allow packets sent before Stop to arrive; thereafter the old
                // worker must neither send media nor reactivate the source.
                QTest::qWait(30);const auto afterStop=gateway.media.size();QTest::qWait(80);
                QCOMPARE(gateway.media.size(),afterStop);QVERIFY(!engine.active());QCOMPARE(stopped.size(),attempt+1);
            }
        }
        // Destruction while actively sharing must also join its worker cleanly.
        auto active=std::make_unique<BroadcastEngine>();QVERIFY(active->configureLocalFixture(gateway.url()));
        active->setIdentity("3333","fixture-session");
        connect(active.get(),&BroadcastEngine::startRequested,active.get(),[&](const QString&,const QString&){
            QTimer::singleShot(0,active.get(),[&]{
                active->onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
                active->onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
            });
        });
        active->startExternal("1111","2222",&source,0,0,false);
        QVERIFY(waitNative([&]{return active->framesSent()>=4;},3000));active.reset();
        QVERIFY(!source.enabled);QTest::qWait(30);const auto afterDestroy=gateway.media.size();QTest::qWait(80);
        QCOMPARE(gateway.media.size(),afterDestroy);
#endif
    }

    void sustainedHighBitrateKeepsPacingWhileGuiIsBusy_data(){
        QTest::addColumn<int>("bitrate");
        QTest::newRow("24Mbps")<<24000000;QTest::newRow("40Mbps")<<40000000;
    }
    void sustainedHighBitrateKeepsPacingWhileGuiIsBusy(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        QFETCH(int,bitrate);
        const int requestedSoak=qEnvironmentVariableIntValue("RELAY_BROADCAST_SOAK_MS");
        const int soakMs=requestedSoak>0?std::clamp(requestedSoak,18000,180000):18000;
        const int soakSeconds=soakMs/1000;
        BroadcastFixture gateway;QVERIFY(gateway.listen());PacedEncodedSource source(bitrate);BroadcastEngine engine;
        QVERIFY(engine.configureLocalFixture(gateway.url()));engine.setIdentity("3333","fixture-session");
        connect(&engine,&BroadcastEngine::startRequested,&engine,[&](const QString&,const QString&){
            QTimer::singleShot(0,&engine,[&]{
                engine.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
                engine.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
            });
        });
        engine.startExternal("1111","2222",&source,0,0,true);
        QVERIFY2(waitNative([&]{return engine.framesSent()>=4;},5000),qPrintable(engine.diagnosticSummary()+" "+engine.error()));
        auto recipient=daveDecryptorCreate();QVERIFY(recipient);
        const auto cleanup=qScopeGuard([&]{daveDecryptorDestroy(recipient);});
        auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(ratchet);
        daveDecryptorTransitionToKeyRatchet(recipient,ratchet);daveKeyRatchetDestroy(ratchet);
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);StreamDecoder decoder("H264");
        int opusError=0;auto opus=opus_decoder_create(48000,2,&opusError);QCOMPARE(opusError,OPUS_OK);
        const auto audioCleanup=qScopeGuard([&]{opus_decoder_destroy(opus);});
        QElapsedTimer duration;duration.start();QEventLoop loop;QTimer consume,busy;
        consume.setTimerType(Qt::PreciseTimer);consume.setInterval(1);
        busy.setTimerType(Qt::PreciseTimer);busy.setInterval(10);
        int delivered=0,played=0,audioPackets=0,latePictures=0;quint64 decoded=0,bytes=0;int phases[4]={};
        qint64 maxBusyAge=0;bool loaded=false,haveVideoSequence=false;quint16 nextVideoSequence=0;QString failure;
        connect(&busy,&QTimer::timeout,&loop,[&]{
            // Model a busy UI doing 9ms of rendering/layout work every10ms.
            // The 1ms sender timer must not share this GUI scheduling budget.
            if(duration.elapsed()>=2000 && duration.elapsed()<soakMs-3000){loaded=true;QThread::usleep(9000);}
        });
        connect(&consume,&QTimer::timeout,&loop,[&]{
            if(duration.elapsed()>=soakMs || !failure.isEmpty()){loop.quit();return;}
            auto packets=std::move(gateway.media);gateway.media.clear();
            for(const auto &packet:packets){
                if(packet.payloadType==120){
                    QByteArray clear(packet.payload.size(),Qt::Uninitialized);size_t length=0;
                    if(daveDecryptorDecrypt(recipient,DAVE_MEDIA_TYPE_AUDIO,StreamTestDave::bytes(packet.payload),packet.payload.size(),StreamTestDave::bytes(clear),clear.size(),&length)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS){failure="Audio decryption failed";break;}
                    clear.resize(length);std::array<opus_int16,1920> pcm;
                    if(opus_decode(opus,StreamTestDave::bytes(clear),clear.size(),pcm.data(),960,0)!=960){failure="Audio decoding failed";break;}
                    ++audioPackets;
                }else if(packet.payloadType==105){
                    if(haveVideoSequence && packet.sequence!=nextVideoSequence){failure="Generated path lost/reordered an original RTP packet";break;}
                    haveVideoSequence=true;nextVideoSequence=quint16(packet.sequence+1);
                    bytes+=packet.payload.size();
                    if(auto complete=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp)){
                        QByteArray clear(complete->size(),Qt::Uninitialized);size_t length=0;
                        if(daveDecryptorDecrypt(recipient,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(*complete),complete->size(),StreamTestDave::bytes(clear),clear.size(),&length)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS){failure="Video decryption failed";break;}
                        clear.resize(length);if(!decoder.submit(clear,packet.timestamp)){failure="Video decoding rejected input";break;}
                        ++delivered;++phases[std::min(3,int(duration.elapsed()/5000))];
                        const qint64 age=source.elapsed.elapsed()-packet.timestamp/90;
                        if(duration.elapsed()>5000 && duration.elapsed()<soakMs-3000){maxBusyAge=std::max(maxBusyAge,age);if(age>300)++latePictures;}
                    }
                }
            }
            const auto picture=decoder.take();decoded=picture.decoded;if(picture.frame.isValid()){++played;if(picture.frame.size()!=QSize(320,180))failure="Decoded dimensions changed";}
            if(!engine.error().isEmpty())failure=engine.error();if(!gateway.error.isEmpty())failure=gateway.error;
        });
        consume.start();busy.start();loop.exec();consume.stop();busy.stop();
        qInfo("Generated%dMbpsGuiBusy delivered=%d decoded=%llu observed=%d audio=%d bytes=%llu maxBusyAgeMs=%lld late=%d phases=%d/%d/%d/%d diagnostics=%s",
            bitrate/1000000,delivered,qulonglong(decoded),played,audioPackets,qulonglong(bytes),qlonglong(maxBusyAge),latePictures,phases[0],phases[1],phases[2],phases[3],qPrintable(engine.diagnosticSummary()));
        QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY(loaded);QVERIFY(duration.elapsed()>=soakMs);
        QVERIFY2(maxBusyAge<300,"Outgoing video must not accumulate seconds of stale pictures behind GUI work");
        QVERIFY2(phases[1]>240 && phases[2]>240,"Both busy five-second intervals must sustain at least48fps");
        QVERIFY(phases[3]>48*(soakSeconds-15));
        QVERIFY(decoded>quint64(48*soakSeconds));QVERIFY(audioPackets>42*soakSeconds);
        QVERIFY(bytes>quint64(bitrate/8)*quint64(soakSeconds-3));QVERIFY(engine.sharing());
        QCOMPARE(gateway.invalidDatagrams,0);
        const auto diagnostics=engine.publishingDiagnostics();
        QCOMPARE(diagnostics.value("queueDrops").toULongLong(),quint64(0));
        QCOMPARE(diagnostics.value("transportErrors").toULongLong(),quint64(0));
        QCOMPARE(diagnostics.value("mediaErrors").toULongLong(),quint64(0));
        QCOMPARE(diagnostics.value("publishRejected").toULongLong(),quint64(0));
        engine.stop();
#endif
    }

    void senderReportsKeepCurrentMediaClockWhileCaptureIsQuiet_data(){
        QTest::addColumn<quint32>("initialVideo");QTest::addColumn<quint32>("initialAudio");
        QTest::newRow("normal")<<quint32(90000)<<quint32(48000);
        QTest::newRow("wrap")<<quint32(0xffff0000)<<quint32(0xffff8000);
    }
    void senderReportsKeepCurrentMediaClockWhileCaptureIsQuiet(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        QFETCH(quint32,initialVideo);QFETCH(quint32,initialAudio);
        BroadcastFixture gateway;QVERIFY(gateway.listen());StreamEngine stream;
        QVERIFY(stream.configureLocalFixture(gateway.url()));stream.setIdentity("3333","fixture-session");
        connect(&stream,&StreamEngine::watchRequested,&stream,[&](const QString&){
            stream.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
            stream.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
        });
        stream.publish(streamKey,320,180,30,2000000);
        QVERIFY(waitNative([&]{return stream.encryptedReady();},3000));
        const auto video=StreamTestMedia::encodedFrame("H264");
        QVERIFY(!video.isEmpty());QVERIFY(stream.publishVideo(video,initialVideo,320,180));
        int error=0;auto opus=opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&error);QCOMPARE(error,OPUS_OK);
        const auto cleanup=qScopeGuard([&]{opus_encoder_destroy(opus);});
        std::array<opus_int16,1920> silence{};QByteArray audio(1275,Qt::Uninitialized);
        const auto bytes=opus_encode(opus,silence.data(),960,reinterpret_cast<unsigned char*>(audio.data()),audio.size());
        QVERIFY(bytes>0);audio.resize(bytes);QVERIFY(stream.publishAudio(audio,initialAudio));
        // A static/temporarily stalled source still has a running RTP clock.
        // Real receivers correlate reports for playout; simply decoding packets
        // as soon as they arrive cannot detect stale timestamp correlations.
        QVERIFY(waitNative([&]{return gateway.senderClocks.size()>=6;},4500));
        for(const auto source:{quint32(202),quint32(101)}){
            QList<BroadcastFixture::SenderClock> clocks;
            for(const auto &item:gateway.senderClocks)if(item.source==source)clocks.append(item);
            QVERIFY(clocks.size()>=3);
            for(int i=1;i<clocks.size();++i){
                const auto wall=double(clocks[i].ntp-clocks[i-1].ntp)/4294967296.;
                const auto media=double(quint32(clocks[i].timestamp-clocks[i-1].timestamp))/(source==202?90000:48000);
                QVERIFY(wall>.5);
                qInfo("GeneratedSenderClock ssrc=%u wall=%.6f media=%.6f",source,wall,media);
                QVERIFY2(std::abs(wall-media)<.025,"Sender report RTP time must describe the NTP instant, not the last packet");
            }
        }
        QVERIFY(stream.encryptedReady());stream.stop();
#endif
    }

    void sustainedVideoSurvivesEpochChangesAndLateRecipient(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        if(!BroadcastEncoder::available())QSKIP("libx264 is unavailable");
        BroadcastFixture gateway;QVERIFY(gateway.listen());StreamEngine stream;
        QVERIFY(stream.configureLocalFixture(gateway.url()));stream.setIdentity("3333","fixture-session");
        connect(&stream,&StreamEngine::watchRequested,&stream,[&](const QString&){
            stream.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
            stream.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
        });
        BroadcastEncoder encoder(720,15,2000000);
        connect(&stream,&StreamEngine::keyFrameRequested,&stream,[&]{encoder.requestKeyframe();});
        stream.publish(streamKey,320,180,15,2000000);
        QTRY_VERIFY_WITH_TIMEOUT(stream.encryptedReady() || !gateway.error.isEmpty(),5000);
        QVERIFY2(gateway.error.isEmpty(),qPrintable(gateway.error));QVERIFY(stream.encryptedReady());
        QVERIFY(gateway.identified);QVERIFY(gateway.announced);

        auto recipient=daveDecryptorCreate();auto lateRecipient=daveDecryptorCreate();QVERIFY(recipient);QVERIFY(lateRecipient);
        const auto cleanup=qScopeGuard([&]{daveDecryptorDestroy(recipient);daveDecryptorDestroy(lateRecipient);});
        auto updateRatchet=[&](DAVEDecryptorHandle decryptor){
            auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");
            if(!ratchet)return false;
            daveDecryptorTransitionToKeyRatchet(decryptor,ratchet);daveKeyRatchetDestroy(ratchet);return true;
        };
        QVERIFY(updateRatchet(recipient));
        StreamDecoder decoder("H264"),lateDecoder("H264");
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);
        QImage image(320,180,QImage::Format_RGB32);image.fill(QColor(210,40,30));
        QElapsedTimer elapsed;elapsed.start();
        qsizetype consumed=0;quint32 lastTimestamp=0;bool haveTimestamp=false,lateWatching=false,ratchetPending=false;
        int resets=0,plies=0,encodedKeyframes=0,decodedPictures=0,latePictures=0;
        int phaseFrames[3]={0,0,0};int phasePictures[3]={0,0,0};int lastPli=-2000;
        quint32 nonce=900;QString failure;
        auto sendPli=[&]{
            QByteArray pli(12,'\0');pli[0]=char(0x81);pli[1]=char(206);qToBigEndian<quint16>(2,pli.data()+2);
            qToBigEndian<quint32>(202,pli.data()+8);
            gateway.udp.writeDatagram(VideoPacket::encryptRtcp(pli,gateway.key,nonce++),gateway.clientAddress,gateway.clientPort);++plies;
        };
        while(elapsed.elapsed()<16500 && failure.isEmpty()){
            const int now=int(elapsed.elapsed());
            if(resets<2 && now>=(resets+1)*5000){
                // A real new MLS group/key, rather than a repeated notification
                // using the existing ratchet. Keep receiving old queued packets.
                auto nextGroup=std::make_unique<StreamTestDave::Group>();
                // A media-session reset keeps the gateway's external sender
                // identity, while creating new group state and media secrets.
                nextGroup->signing=gateway.group->signing;nextGroup->sender=gateway.group->sender;
                const auto external=nextGroup->externalSender();
                daveSessionSetExternalSender(nextGroup->session,StreamTestDave::bytes(external),external.size());
                daveSessionInit(nextGroup->session,1,StreamTestDave::groupId,StreamTestDave::ownerId);
                gateway.group=std::move(nextGroup);gateway.groupReady=false;
                gateway.send(24,{{"epoch",1},{"protocol_version",1}});ratchetPending=true;++resets;
            }
            if(ratchetPending && gateway.groupReady && stream.encryptedReady()){
                if(!updateRatchet(recipient) || (lateWatching && !updateRatchet(lateRecipient))){failure="New epoch did not supply the recipient ratchet";break;}
                ratchetPending=false;
            }
            if(!lateWatching && now>=8000 && !ratchetPending){
                if(!updateRatchet(lateRecipient)){failure="Late recipient did not receive the current group key";break;}
                lateWatching=true;sendPli();
            }
            if(now-lastPli>=2000){sendPli();lastPli=now;}
            encoder.submit(QVideoFrame(image));
            const auto snapshot=encoder.take();
            if(!snapshot.error.isEmpty()){failure=snapshot.error;break;}
            for(const auto &frame:snapshot.frames){
                if(frame.keyframe)++encodedKeyframes;
                if(stream.encryptedReady() && !stream.publishVideo(frame.bytes,frame.timestamp,frame.width,frame.height))failure="Publisher rejected a generated frame";
            }
            while(consumed<gateway.media.size() && failure.isEmpty()){
                const auto packet=gateway.media[consumed++];if(packet.payloadType!=105)continue;
                const auto protectedFrame=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp);if(!protectedFrame)continue;
                if(haveTimestamp && qint32(packet.timestamp-lastTimestamp)<=0){failure="Outgoing presentation timestamps did not advance";break;}
                lastTimestamp=packet.timestamp;haveTimestamp=true;
                QByteArray plain(protectedFrame->size(),Qt::Uninitialized);size_t length=0;
                if(daveDecryptorDecrypt(recipient,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(*protectedFrame),protectedFrame->size(),StreamTestDave::bytes(plain),plain.size(),&length)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS){failure="Recipient could not decrypt a frame across the group transition";break;}
                plain.resize(length);++phaseFrames[resets];
                if(!decoder.submit(plain,packet.timestamp)){failure="Native decoder rejected a generated frame";break;}
                if(lateWatching){
                    QByteArray latePlain(protectedFrame->size(),Qt::Uninitialized);size_t lateLength=0;
                    if(daveDecryptorDecrypt(lateRecipient,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(*protectedFrame),protectedFrame->size(),StreamTestDave::bytes(latePlain),latePlain.size(),&lateLength)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS){failure="Late recipient could not decrypt a live frame";break;}
                    latePlain.resize(lateLength);lateDecoder.submit(latePlain,packet.timestamp);
                }
            }
            const auto picture=decoder.take();
            if(picture.frame.isValid()){++decodedPictures;++phasePictures[resets];if(picture.frame.size()!=QSize(320,180))failure="Decoded video size changed";}
            const auto latePicture=lateDecoder.take();if(latePicture.frame.isValid())++latePictures;
            if(!stream.error().isEmpty())failure=stream.error();if(!gateway.error.isEmpty())failure=gateway.error;
            QTest::qWait(10);
        }
        QVERIFY2(failure.isEmpty(),qPrintable(failure));QVERIFY(elapsed.elapsed()>=16000);QCOMPARE(resets,2);
        QVERIFY(plies>=8);QVERIFY(encodedKeyframes>=8);QVERIFY(decodedPictures>=100);QVERIFY(latePictures>=50);
        for(int phase=0;phase<3;++phase){QVERIFY2(phaseFrames[phase]>=30,"Each MLS group must continue carrying video");QVERIFY2(phasePictures[phase]>=20,"Each MLS group must continue displaying decoded video");}
        QVERIFY(gateway.senderReports>=14);QVERIFY(gateway.keepAlives>=3);
        const auto pongCounts=QRegularExpression("ping/pong ([0-9]+)/([0-9]+)").match(stream.diagnosticSummary());
        QVERIFY(pongCounts.hasMatch());QVERIFY(pongCounts.captured(1).toInt()>=3);QVERIFY(pongCounts.captured(2).toInt()>=3);
        QVERIFY(stream.active());QVERIFY(stream.encryptedReady());stream.stop();
#endif
    }

    void publishingChangesSendingKeyOnlyOnExecute(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        BroadcastFixture gateway;QVERIFY(gateway.listen());StreamEngine stream;
        QVERIFY(stream.configureLocalFixture(gateway.url()));stream.setIdentity("3333","fixture-session");
        connect(&stream,&StreamEngine::watchRequested,&stream,[&](const QString&){
            stream.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
            stream.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
        });
        stream.publish(streamKey,320,180,30,2000000);QTRY_VERIFY_WITH_TIMEOUT(stream.encryptedReady() || !gateway.error.isEmpty(),5000);
        QVERIFY2(gateway.error.isEmpty(),qPrintable(gateway.error));QVERIFY(stream.encryptedReady());
        auto recipient=daveDecryptorCreate();QVERIFY(recipient);const auto cleanup=qScopeGuard([&]{daveDecryptorDestroy(recipient);});
        auto oldRatchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(oldRatchet);
        daveDecryptorTransitionToKeyRatchet(recipient,oldRatchet);daveKeyRatchetDestroy(oldRatchet);
        const auto encoded=StreamTestMedia::encodedFrame("H264");QVERIFY(!encoded.isEmpty());
        QVERIFY(gateway.addMemberWithDelayedExecution());
        QTRY_VERIFY_WITH_TIMEOUT(gateway.pendingTransition==9 || !gateway.error.isEmpty(),5000);
        QVERIFY2(gateway.error.isEmpty(),qPrintable(gateway.error));QCOMPARE(gateway.pendingTransition,9);QVERIFY(stream.encryptedReady());
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);qsizetype consumed=0;
        auto receivePlain=[&]() -> QByteArray {
            while(consumed<gateway.media.size()){
                const auto packet=gateway.media[consumed++];if(packet.payloadType!=105)continue;
                const auto protectedFrame=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp);if(!protectedFrame)continue;
                QByteArray plain(protectedFrame->size(),Qt::Uninitialized);size_t length=0;
                if(daveDecryptorDecrypt(recipient,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(*protectedFrame),protectedFrame->size(),StreamTestDave::bytes(plain),plain.size(),&length)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS)return QByteArray("wrong sending key");
                plain.resize(length);return plain;
            }
            return {};
        };
        for(int frame=0;frame<5;++frame){
            QVERIFY(stream.publishVideo(encoded,quint32(9000+frame*3000),320,180));QByteArray plain;
            QTRY_VERIFY_WITH_TIMEOUT(!plain.isEmpty() || (plain=receivePlain(),!plain.isEmpty()),1000);QCOMPARE(plain,encoded);QTest::qWait(100);
        }
        auto newRatchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(newRatchet);
        daveDecryptorTransitionToKeyRatchet(recipient,newRatchet);daveKeyRatchetDestroy(newRatchet);
        QSignalSpy executed(&stream,&StreamEngine::keyFrameRequested);gateway.send(22,{{"transition_id",9}});
        QTRY_VERIFY_WITH_TIMEOUT(!executed.isEmpty(),1000);QVERIFY(stream.publishVideo(encoded,27000,320,180));QByteArray after;
        QTRY_VERIFY_WITH_TIMEOUT(!after.isEmpty() || (after=receivePlain(),!after.isEmpty()),1000);QCOMPARE(after,encoded);stream.stop();
#endif
    }

    void originalLongGopStartsWithoutWaitingForTheNextObsIdr(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        if(QStandardPaths::findExecutable("ffmpeg").isEmpty())QSKIP("FFmpeg generates the local long-GOP source");
        RtmpPlaybackFixture feed;feed.size=QSize(640,360);feed.fps=30;feed.durationSeconds=40;feed.gopSeconds=30;feed.motion=true;feed.start();
        QVERIFY(feed.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(feed.listening(),3000);
        ObservedSource observed;auto &source=observed.source;source.setInputMode(StudioBridge::ExistingRtmpStream);source.setRemoteServer(feed.server());source.setRemoteStreamKey("fixture-key");source.start();
        QTRY_VERIFY_WITH_TIMEOUT(source.receiving() || !source.error().isEmpty(),10000);QVERIFY2(source.receiving(),qPrintable(source.error()));
        BroadcastFixture gateway;QVERIFY(gateway.listen());BroadcastEngine engine;QVERIFY(engine.configureLocalFixture(gateway.url()));engine.setIdentity("3333","fixture-session");
        connect(&engine,&BroadcastEngine::startRequested,&engine,[&](const QString&,const QString&){QTimer::singleShot(0,&engine,[&]{engine.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});engine.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});});});
        engine.startExternal("1111","2222",&observed,0,0,false);
        // No source IDR is due for 30 seconds. Allocate encryption from its
        // validated metadata now, so a normal source's first IDR is usable.
        QVERIFY2(waitNative([&]{return gateway.groupReady && engine.sharing();},1000),qPrintable(engine.diagnosticSummary()));
        QCOMPARE(engine.framesSent(),quint64(0));
        QVERIFY2(waitNative([&]{return engine.framesSent()>=4 || !engine.error().isEmpty();},6000),qPrintable(engine.diagnosticSummary()+" "+engine.error()));
        QVERIFY2(engine.error().isEmpty(),qPrintable(engine.error()));QVERIFY(engine.framesSent()>=4);
        QCOMPARE(engine.videoWidth(),feed.size.width());QCOMPARE(engine.videoHeight(),feed.size.height());
        QCOMPARE(gateway.rendition.value("max_framerate").toInt(),feed.fps);
        QVERIFY(engine.qualitySummary().contains("High quality encoding"));
        QVERIFY(engine.diagnosticSummary().contains("recoveries=1,recovery=initial-keyframe"));
        auto decryptor=daveDecryptorCreate();QVERIFY(decryptor);const auto cleanup=qScopeGuard([&]{daveDecryptorDestroy(decryptor);});
        auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(ratchet);daveDecryptorTransitionToKeyRatchet(decryptor,ratchet);daveKeyRatchetDestroy(ratchet);
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);StreamDecoder decoder("H264");
        for(const auto &packet:gateway.media)if(packet.payloadType==105)if(auto complete=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp)){
            QByteArray clear(complete->size(),Qt::Uninitialized);size_t length=0;
            QCOMPARE(daveDecryptorDecrypt(decryptor,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(*complete),complete->size(),StreamTestDave::bytes(clear),clear.size(),&length),DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);clear.resize(length);
            QSet<int> types;for(const auto &nalu:StreamTestMedia::nalus(clear))types.insert(quint8(nalu[0])&31);QVERIFY(types.contains(5));QVERIFY(types.contains(7));QVERIFY(types.contains(8));
            QVERIFY(decoder.submit(clear,packet.timestamp));break; // Decode the first random-access picture before offering its dependents.
        }
        StreamDecoder::Snapshot picture;QVERIFY(waitNative([&]{return picture.frame.isValid() || (picture=decoder.take(),picture.frame.isValid());},2000));QCOMPARE(picture.frame.size(),feed.size);
        engine.stop();source.stop();
#endif
    }

    void originalLongGopRecoversAfterMailboxLossWithContinuousTimestamps(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        if(QStandardPaths::findExecutable("ffmpeg").isEmpty())QSKIP("FFmpeg generates the local long-GOP source");
        RtmpPlaybackFixture feed;feed.size=QSize(640,360);feed.fps=60;feed.durationSeconds=40;feed.gopSeconds=30;feed.earlyKeyframesSeconds=4;feed.motion=true;feed.start();
        QVERIFY(feed.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(feed.listening(),3000);
        ObservedSource observed;auto &source=observed.source;source.setInputMode(StudioBridge::ExistingRtmpStream);source.setRemoteServer(feed.server());source.setRemoteStreamKey("fixture-key");source.start();
        QTRY_VERIFY_WITH_TIMEOUT(source.receiving() || !source.error().isEmpty(),10000);QVERIFY2(source.receiving(),qPrintable(source.error()));
        BroadcastFixture gateway;QVERIFY(gateway.listen());BroadcastEngine engine;QVERIFY(engine.configureLocalFixture(gateway.url()));engine.setIdentity("3333","fixture-session");
        connect(&engine,&BroadcastEngine::startRequested,&engine,[&](const QString&,const QString&){QTimer::singleShot(0,&engine,[&]{engine.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});engine.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});});});
        engine.startExternal("1111","2222",&observed,0,0,false);
        QVERIFY2(waitNative([&]{return engine.framesSent()>=4 || !engine.error().isEmpty();},6000),qPrintable(engine.diagnosticSummary()));
        QVERIFY(engine.qualitySummary().contains("Original video"));
        auto decryptor=daveDecryptorCreate();QVERIFY(decryptor);const auto cleanup=qScopeGuard([&]{daveDecryptorDestroy(decryptor);});
        auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(ratchet);daveDecryptorTransitionToKeyRatchet(decryptor,ratchet);daveKeyRatchetDestroy(ratchet);
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);StreamDecoder decoder("H264");
        int pictures=0;bool haveStamp=false;quint32 lastStamp=0,maxGap=0;
        const auto drain=[&]{
            auto packets=std::move(gateway.media);gateway.media.clear();
            for(const auto &packet:packets)if(packet.payloadType==105)if(auto complete=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp)){
                QByteArray clear(complete->size(),Qt::Uninitialized);size_t length=0;
                QCOMPARE(daveDecryptorDecrypt(decryptor,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(*complete),complete->size(),StreamTestDave::bytes(clear),clear.size(),&length),DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);clear.resize(length);
                if(haveStamp){QVERIFY2(qint32(packet.timestamp-lastStamp)>0,"Recovery must not restart the RTP presentation clock");maxGap=std::max(maxGap,packet.timestamp-lastStamp);}
                lastStamp=packet.timestamp;haveStamp=true;QVERIFY(decoder.submit(clear,packet.timestamp));
            }
            auto picture=decoder.take();if(picture.frame.isValid()){++pictures;QCOMPARE(picture.frame.size(),feed.size);}
        };
        QVERIFY(waitNative([&]{drain();return pictures>=4;},2000));
        QElapsedTimer healthy;healthy.start();
        QVERIFY(waitNative([&]{drain();return healthy.elapsed()>=4500;},5000));
        QVERIFY(engine.qualitySummary().contains("Original video"));
        // A busy GUI overruns the same 16-picture mailbox that receives RTMP
        // batches. The source has no natural IDR for another 30 seconds.
        const auto before=pictures;QThread::msleep(650);QElapsedTimer recovery;recovery.start();
        QVERIFY2(waitNative([&]{drain();return pictures>before+2 && engine.qualitySummary().contains("High quality encoding");},4000),qPrintable(engine.diagnosticSummary()+" "+engine.error()));
        QVERIFY(recovery.elapsed()<4000);QVERIFY(maxGap<4000*90);
        QVERIFY(engine.diagnosticSummary().contains("recoveries=1,recovery=source-keyframe"));
        const auto recovered=pictures;QVERIFY(waitNative([&]{drain();return pictures>=recovered+25;},1800));
        QCOMPARE(gateway.rendition.value("max_framerate").toInt(),feed.fps);QVERIFY(engine.error().isEmpty());QVERIFY(source.error().isEmpty());
        qInfo("GeneratedLongGopRecovery pictures=%d recoveryMs=%lld maxRtpGapMs=%u",pictures,qlonglong(recovery.elapsed()),maxGap/90);
        engine.stop();source.stop();
#endif
    }

    void originalVideoKeepsFlowingWhileViewersRequestKeyframes(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        if(QStandardPaths::findExecutable("ffmpeg").isEmpty())QSKIP("FFmpeg generates the local high-motion source");
        RtmpPlaybackFixture feed;feed.size=QSize(1920,1080);feed.fps=60;feed.motion=true;feed.durationSeconds=40;feed.gopSeconds=2;feed.bitrate=6000000;feed.start();
        QVERIFY(feed.process.waitForStarted(2000));QTRY_VERIFY_WITH_TIMEOUT(feed.listening(),3000);
        ObservedSource observed;auto &source=observed.source;source.setInputMode(StudioBridge::ExistingRtmpStream);source.setRemoteServer(feed.server());source.setRemoteStreamKey("fixture-key");source.start();
        QTRY_VERIFY_WITH_TIMEOUT(source.receiving() || !source.error().isEmpty(),10000);QVERIFY2(source.receiving(),qPrintable(source.error()));
        BroadcastFixture gateway;QVERIFY(gateway.listen());BroadcastEngine engine;QVERIFY(engine.configureLocalFixture(gateway.url()));engine.setIdentity("3333","fixture-session");
        connect(&engine,&BroadcastEngine::startRequested,&engine,[&](const QString&,const QString&){QTimer::singleShot(0,&engine,[&]{engine.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});engine.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});});});
        engine.startExternal("1111","2222",&observed,0,0,false);
        QTRY_VERIFY_WITH_TIMEOUT(engine.framesSent()>=4 || !engine.error().isEmpty() || !gateway.error.isEmpty(),10000);
        QVERIFY2(engine.error().isEmpty(),qPrintable(engine.error()));QVERIFY2(gateway.error.isEmpty(),qPrintable(gateway.error));QVERIFY(engine.framesSent()>=4);QVERIFY2(engine.qualitySummary().contains("Original video"),qPrintable(engine.diagnosticSummary()));
        auto decryptor=daveDecryptorCreate();QVERIFY(decryptor);const auto cleanup=qScopeGuard([&]{daveDecryptorDestroy(decryptor);});
        auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(ratchet);daveDecryptorTransitionToKeyRatchet(decryptor,ratchet);daveKeyRatchetDestroy(ratchet);
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);StreamDecoder decoder("H264");
        QElapsedTimer elapsed;elapsed.start();observed.original.clear();
        int delivered=0,pictures=0,requests=0,phase[4]={0,0,0,0};qint64 nextPli=3000;bool stalled=false;quint32 nonce=120000,lastStamp=0,maxSteadyGap=0;bool haveStamp=false;
        // Keep native sender timers at their normal cadence. A polling qWait
        // would coalesce them and create artificial high-bitrate packet loss.
        QEventLoop samplingLoop;QTimer sample;sample.setInterval(1);sample.setTimerType(Qt::PreciseTimer);
        connect(&sample,&QTimer::timeout,&samplingLoop,[&]{
            const auto tickGuard=qScopeGuard([&]{if(QTest::currentTestFailed()){sample.stop();samplingLoop.quit();}});
            if(elapsed.elapsed()>=31000){sample.stop();samplingLoop.quit();return;}
            if(!stalled && elapsed.elapsed()>=15000){stalled=true;QThread::msleep(350);} // Actual source mailbox overflow/recovery.
            if(elapsed.elapsed()>=nextPli){
                QByteArray pli(12,'\0');pli[0]=char(0x81);pli[1]=char(206);qToBigEndian<quint16>(2,pli.data()+2);qToBigEndian<quint32>(202,pli.data()+8);
                gateway.udp.writeDatagram(VideoPacket::encryptRtcp(pli,gateway.key,nonce++),gateway.clientAddress,gateway.clientPort);nextPli+=350;++requests;
            }
            auto packets=std::move(gateway.media);gateway.media.clear();
            for(const auto &packet:packets)if(packet.payloadType==105){
                if(auto complete=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp)){
                    QByteArray clear(complete->size(),Qt::Uninitialized);size_t length=0;
                    QCOMPARE(daveDecryptorDecrypt(decryptor,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(*complete),complete->size(),StreamTestDave::bytes(clear),clear.size(),&length),DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);clear.resize(length);
                    if(haveStamp){
                        const auto step=packet.timestamp-lastStamp;QVERIFY(qint32(step)>0);
                        if(elapsed.elapsed()>3500 && (elapsed.elapsed()<14000 || elapsed.elapsed()>19000))maxSteadyGap=std::max(maxSteadyGap,step);
                    }
                    lastStamp=packet.timestamp;haveStamp=true;
                    QVERIFY(decoder.submit(clear,packet.timestamp));++delivered;++phase[std::min(3,int(elapsed.elapsed()/10000))];
                }
            }
            auto picture=decoder.take();if(picture.frame.isValid()){++pictures;QCOMPARE(picture.frame.size(),feed.size);}
            QVERIFY2(engine.error().isEmpty(),qPrintable(engine.error()));QVERIFY2(source.error().isEmpty(),qPrintable(source.error()));
        });sample.start();samplingLoop.exec();QVERIFY(!QTest::currentTestFailed());
        int pPictures=0,idrPictures=0;for(const auto &bytes:observed.original)for(const auto &nalu:StreamTestMedia::nalus(bytes)){const auto type=quint8(nalu[0])&31;if(type==1){++pPictures;break;}if(type==5){++idrPictures;break;}}
        qInfo("Generated1080p60 source=%lld delivered=%d decoded=%d PLI=%d P=%d IDR=%d maxSteadyGap=%u phases=%d/%d/%d/%d",qlonglong(observed.original.size()),delivered,pictures,requests,pPictures,idrPictures,maxSteadyGap,phase[0],phase[1],phase[2],phase[3]);
        QVERIFY(requests>=70);QVERIFY(stalled);QVERIFY(observed.original.size()>1400);
        // A PLI requests a future random-access picture. It does not invalidate
        // a sender's existing chain or authorize dropping every intervening P
        // picture globally. Only the deliberate mailbox overflow may gate it.
        QVERIFY2(delivered>=observed.original.size()*85/100,"Keyframe requests must not globally pause a valid OBS source chain");
        QVERIFY(pPictures>1200);QVERIFY(idrPictures<30);QVERIFY(pictures>1200);QVERIFY(phase[1]>400);QVERIFY(phase[2]>400);QVERIFY(maxSteadyGap<=4500);
        engine.stop();const auto retained=engine.diagnosticSummary();QVERIFY(retained.contains("source(path=obs-original,size=1920x1080,sourceFps=60,outputFps=60"));QVERIFY(!retained.contains("fixture-key"));QVERIFY(!retained.contains(feed.server()));source.stop();
#endif
    }

    void encryptedVideoAudioAndFeedback(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the outgoing stream fixture");
#else
        BroadcastFixture gateway;QVERIFY(gateway.listen());StreamEngine stream;
        QVERIFY(stream.configureLocalFixture(gateway.url()));stream.setIdentity("3333","fixture-session");
        connect(&stream,&StreamEngine::watchRequested,&stream,[&](const QString&){
            stream.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
            stream.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fixture-token"}});
        });
        stream.publish(streamKey,320,180,30,2000000);
        QVERIFY(!stream.publishVideo(StreamTestMedia::encodedFrame("H264"),9000,320,180));
        QTRY_VERIFY_WITH_TIMEOUT(stream.encryptedReady() || !gateway.error.isEmpty(),5000);
        QVERIFY2(gateway.error.isEmpty(),qPrintable(gateway.error));QVERIFY2(stream.encryptedReady(),qPrintable(stream.diagnosticSummary()));
        QVERIFY(gateway.identified);QVERIFY(gateway.selected);QVERIFY(gateway.announced);
        auto decryptor=daveDecryptorCreate();QVERIFY(decryptor);
        auto ratchet=daveSessionGetKeyRatchet(gateway.group->session,"3333");QVERIFY(ratchet);
        daveDecryptorTransitionToKeyRatchet(decryptor,ratchet);daveKeyRatchetDestroy(ratchet);
        const auto encoded=StreamTestMedia::encodedFrame("H264");QVERIFY(!encoded.isEmpty());
        QVERIFY(stream.publishVideo(encoded,9000,320,180));
        QTRY_VERIFY_WITH_TIMEOUT(stream.frameCount()>0,2000);QTRY_VERIFY_WITH_TIMEOUT(!gateway.media.isEmpty() && gateway.media.last().marker,2000);
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);QByteArray protectedFrame;QSet<quint32> nonces;
        for(const auto &packet:gateway.media){
            QCOMPARE(packet.payloadType,quint8(105));QCOMPARE(packet.ssrc,quint32(202));QVERIFY(!nonces.contains(packet.nonce));nonces.insert(packet.nonce);
            if(auto frame=depacketizer.push(packet.payload,packet.marker,packet.sequence,packet.timestamp))protectedFrame=*frame;
        }
        QVERIFY(!protectedFrame.isEmpty());QByteArray plain(protectedFrame.size(),Qt::Uninitialized);size_t length=0;
        QCOMPARE(daveDecryptorDecrypt(decryptor,DAVE_MEDIA_TYPE_VIDEO,StreamTestDave::bytes(protectedFrame),protectedFrame.size(),StreamTestDave::bytes(plain),plain.size(),&length),DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);
        plain.resize(length);QCOMPARE(plain,encoded);
        const auto original=gateway.media.first();const int previous=gateway.media.size();
        QByteArray nack(16,'\0');nack[0]=char(0x81);nack[1]=char(205);qToBigEndian<quint16>(3,nack.data()+2);qToBigEndian<quint32>(202,nack.data()+8);qToBigEndian(original.sequence,nack.data()+12);
        for(int request=0;request<50;++request)
            gateway.udp.writeDatagram(VideoPacket::encryptRtcp(nack,gateway.key,700+request),gateway.clientAddress,gateway.clientPort);
        QVERIFY(waitNative([&]{return gateway.media.size()>previous;},1000));const auto repair=gateway.media.last();QCOMPARE(repair.payloadType,quint8(106));QCOMPARE(repair.ssrc,quint32(203));
        QCOMPARE(qFromBigEndian<quint16>(repair.payload.constData()),original.sequence);QCOMPARE(repair.payload.mid(2),original.payload);QVERIFY(!nonces.contains(repair.nonce));nonces.insert(repair.nonce);
        gateway.udp.writeDatagram(VideoPacket::encryptRtcp(nack,gateway.key,750),gateway.clientAddress,gateway.clientPort);
        QTest::qWait(25);QCOMPARE(gateway.media.size(),previous+1);
        // A sent repair stays protected while its response is in flight.
        // A genuinely later request can still recover a dropped repair.
        QTest::qWait(110);
        gateway.udp.writeDatagram(VideoPacket::encryptRtcp(nack,gateway.key,751),gateway.clientAddress,gateway.clientPort);
        QTRY_COMPARE_WITH_TIMEOUT(gateway.media.size(),previous+2,1000);
        QCOMPARE(gateway.media.last().payload,repair.payload);
        QVERIFY(gateway.media.last().sequence!=repair.sequence);
        QVERIFY(!nonces.contains(gateway.media.last().nonce));nonces.insert(gateway.media.last().nonce);
        QSignalSpy requested(&stream,&StreamEngine::keyFrameRequested);QByteArray pli(12,'\0');pli[0]=char(0x81);pli[1]=char(206);qToBigEndian<quint16>(2,pli.data()+2);qToBigEndian<quint32>(202,pli.data()+8);
        gateway.udp.writeDatagram(VideoPacket::encryptRtcp(pli,gateway.key,752),gateway.clientAddress,gateway.clientPort);QTRY_VERIFY(requested.size()>0);
        int opusError=0;auto opus=opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&opusError);QCOMPARE(opusError,OPUS_OK);
        std::array<opus_int16,1920> tone{};for(size_t i=0;i<tone.size();++i)tone[i]=opus_int16((i%48)*100);
        QByteArray audio(1275,Qt::Uninitialized);const int count=opus_encode(opus,tone.data(),960,reinterpret_cast<unsigned char*>(audio.data()),audio.size());opus_encoder_destroy(opus);QVERIFY(count>0);audio.resize(count);
        const int beforeAudio=gateway.media.size();QVERIFY(stream.publishAudio(audio,4800));QTRY_VERIFY(gateway.media.size()>beforeAudio);
        const auto audioPacket=gateway.media.last();QCOMPARE(audioPacket.payloadType,quint8(120));QCOMPARE(audioPacket.ssrc,quint32(101));QVERIFY(!nonces.contains(audioPacket.nonce));
        plain.resize(audioPacket.payload.size());length=0;
        QCOMPARE(daveDecryptorDecrypt(decryptor,DAVE_MEDIA_TYPE_AUDIO,StreamTestDave::bytes(audioPacket.payload),audioPacket.payload.size(),StreamTestDave::bytes(plain),plain.size(),&length),DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);
        plain.resize(length);QCOMPARE(plain,audio);QTRY_VERIFY_WITH_TIMEOUT(gateway.senderReports>0,2000);
        QVERIFY(gateway.senderReportLsr);const auto measuredLsr=gateway.senderReportLsr;QTest::qWait(25);
        QByteArray rr(32,'\0');rr[0]=char(0x81);rr[1]=char(201);qToBigEndian<quint16>(7,rr.data()+2);qToBigEndian<quint32>(404,rr.data()+4);qToBigEndian<quint32>(202,rr.data()+8);rr[12]=64;qToBigEndian(measuredLsr,rr.data()+24);
        gateway.udp.writeDatagram(VideoPacket::encryptRtcp(rr,gateway.key,753),gateway.clientAddress,gateway.clientPort);
        QTRY_VERIFY(stream.diagnosticSummary().contains("receiver reports/loss last-max/RTT ms 1/64-64/"));
        const auto measured=QRegularExpression("RTT ms 1/64-64/(\\d+)").match(stream.diagnosticSummary());QVERIFY(measured.hasMatch());QVERIFY(measured.captured(1).toInt()>=20);
        // Old reports cannot drive replay after their useful recovery window.
        QTest::qWait(1550);const auto beforeOld=gateway.media.size();
        gateway.udp.writeDatagram(VideoPacket::encryptRtcp(nack,gateway.key,754),gateway.clientAddress,gateway.clientPort);QTest::qWait(30);QCOMPARE(gateway.media.size(),beforeOld);
        daveDecryptorDestroy(decryptor);const auto diagnostics=stream.diagnosticSummary();QVERIFY(!diagnostics.contains("fixture-token"));QVERIFY(!diagnostics.contains(streamKey));
        stream.stop();QVERIFY(!stream.active());QVERIFY(!stream.publishVideo(encoded,12000,320,180));
#endif
    }
};
QTEST_MAIN(BroadcastGatewayTest)
#include "broadcast_gateway_test.moc"
