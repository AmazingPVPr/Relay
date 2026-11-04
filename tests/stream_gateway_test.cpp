#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
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
#include <opus/opus.h>
#include <cmath>
#include "StreamEngine.h"
#include "VideoPacket.h"
#include "StreamTestMedia.h"
#ifdef RELAY_TEST_MLSPP
#include "StreamTestDave.h"
#endif

namespace {
constexpr auto streamKey="guild:1111:2222:4444";
// A generated push sink accepts at most 80 ms of PCM and drains at 48 kHz,
// including while the GUI thread is deliberately blocked. No device is opened.
class PacedAudioOutput final : public QIODevice {
    QElapsedTimer clock;
    qint64 updatedUs=0,pending=0;
    void drain(){const auto now=clock.nsecsElapsed()/1000;const auto consumed=(now-updatedUs)*192/1000/4*4;pending=std::max<qint64>(0,pending-consumed);updatedUs=pending?updatedUs+consumed*1000/192:now;}
    qint64 readData(char*,qint64) override{return -1;}
    qint64 writeData(const char*,qint64 length) override{drain();const auto accepted=std::min<qint64>(length,3840*4-pending);pending+=accepted;return accepted;}
public:
    PacedAudioOutput(){clock.start();open(QIODevice::WriteOnly|QIODevice::Unbuffered);}
};
class Gateway : public QObject {
public:
    QWebSocketServer server{"Stream fixture",QWebSocketServer::NonSecureMode};QUdpSocket udp;QPointer<QWebSocket> socket;
    QByteArray key=QByteArray(32,'S');QHostAddress clientAddress;quint16 clientPort=0,sequence=0;
    QString codec="H264",error,ownerId="4444",rtcServerId="5556",rtcChannelId="6666";bool createCommit=false,identified=false,selected=false,groupReady=false;
    int feedbackCount=0,pliCount=0,nackCount=0;quint32 reportSource=0,reportLsr=0;
    QJsonObject wants;QJsonArray offeredCodecs;
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
            }else if(auto plain=VideoPacket::decryptRtcp(packet,key)){
                ++feedbackCount;if(quint8((*plain)[1])==206)++pliCount;if(quint8((*plain)[1])==205)++nackCount;
                if(quint8((*plain)[1])==201 && plain->size()>=32){reportSource=qFromBigEndian<quint32>(plain->constData()+8);reportLsr=qFromBigEndian<quint32>(plain->constData()+24);}
            }
        }});return true;
    }
    QUrl url()const{return QUrl(QString("ws://127.0.0.1:%1/?v=9").arg(server.serverPort()));}
    void send(int op,const QJsonObject &data){if(socket)socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op",op},{"d",data},{"seq",int(++sequence)}}).toJson(QJsonDocument::Compact)));}
    void binary(int op,const QByteArray &data){QByteArray out(3,'\0');qToBigEndian(++sequence,out.data());out[2]=char(op);out+=data;if(socket)socket->sendBinaryMessage(out);}
    void transition(int op,const QByteArray &data,quint16 id){QByteArray out(2,'\0');qToBigEndian(id,out.data());out+=data;binary(op,out);}
    void onText(const QJsonObject &root){const auto data=root.value("d").toObject();switch(root.value("op").toInt()){
        case 0:identified=data.value("server_id").toString()==rtcServerId && data.value("channel_id").toString()==rtcChannelId && data.value("user_id").toString()=="3333" && data.value("video").toBool();
            send(2,{{"ssrc",101},{"ip","127.0.0.1"},{"port",int(udp.localPort())},{"modes",QJsonArray{"aead_xchacha20_poly1305_rtpsize"}}});break;
        case 1:{offeredCodecs=data.value("codecs").toArray();selected=false;for(const auto &entry:data.value("codecs").toArray()){const auto c=entry.toObject();if(c.value("name").toString()==codec && c.value("decode").toBool() && !c.value("encode").toBool())selected=true;}
            QJsonArray secret;for(int i=0;i<32;++i)secret.append(int('S'));
#ifdef RELAY_TEST_MLSPP
            group=std::make_unique<StreamTestDave::Group>(ownerId,rtcServerId.toULongLong()-1);send(11,{{"user_ids",QJsonArray{ownerId}}});binary(25,group->externalSender());
#endif
            send(4,{{"mode","aead_xchacha20_poly1305_rtpsize"},{"secret_key",secret},{"audio_codec","opus"},{"video_codec",codec},{"dave_protocol_version",1}});break;
        }
        case 3:send(6,{{"t",data.value("t")}});break;
        case 15:wants=data;break;
        case 23:send(22,{{"transition_id",data.value("transition_id")}});break;
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
            }else if(quint8(message[0])==28){auto [commit,welcome]=group->split(message.mid(1));
                if(!group->accept(welcome,false)||!group->prepare(codec)){error="Fixture could not accept broadcaster welcome";return;}
                groupReady=true;transition(29,commit,7);}
        }catch(const std::exception &){error="Fixture MLS parse failed";}
#else
        Q_UNUSED(message);
#endif
    }
    void announce(){send(12,{{"user_id",ownerId},{"audio_ssrc",303},{"video_ssrc",202},{"rtx_ssrc",203},
        {"streams",QJsonArray{QJsonObject{{"type","screen"},{"rid","100"},{"quality",100},{"active",true},{"ssrc",202},{"rtx_ssrc",203}}}}});}
};
}

class StreamGatewayTest final : public QObject {
    Q_OBJECT
private:
    void startGeneratedStream(StreamEngine &stream,Gateway &gateway,const QString &key) {
        QVERIFY(stream.configureLocalFixture(gateway.url()));
        stream.watch(key,"Generated lifecycle stream");stream.setIdentity("3333","generated-session");
        stream.onStreamServer({{"stream_key",key},{"endpoint","fixture.discord.media"},{"token","generated-token"}});
        stream.onStreamCreate({{"stream_key",key},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
        QTRY_VERIFY_WITH_TIMEOUT(stream.encryptedReady() || !gateway.error.isEmpty(),5000);
        QVERIFY2(gateway.error.isEmpty(),qPrintable(gateway.error));QVERIFY2(stream.encryptedReady(),qPrintable(stream.diagnosticSummary()));
    }
#ifdef RELAY_TEST_MLSPP
    QByteArray generatedOpus() {
        int error=0;auto *encoder=opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&error);
        if(error!=OPUS_OK || !encoder)return {};
        std::array<opus_int16,1920> pcm{};
        for(size_t i=0;i<pcm.size();++i)pcm[i]=opus_int16((i%40)*100);
        QByteArray result(1275,Qt::Uninitialized);const auto size=opus_encode(encoder,pcm.data(),960,reinterpret_cast<unsigned char*>(result.data()),result.size());
        opus_encoder_destroy(encoder);if(size<=0)return {};result.resize(size);return result;
    }
    void sendGeneratedAudio(Gateway &gateway,quint32 source,quint16 first=0,int count=6) {
        const auto opus=generatedOpus();QVERIFY(!opus.isEmpty());
        for(int i=0;i<count;++i) {
            const auto protectedAudio=gateway.group->encrypt(opus,DAVE_MEDIA_TYPE_AUDIO);QVERIFY(!protectedAudio.isEmpty());
            gateway.udp.writeDatagram(StreamTestMedia::encryptRtp(protectedAudio,gateway.key,120,false,quint16(first+i),quint32(first+i)*960,source,1000+first+i),gateway.clientAddress,gateway.clientPort);
        }
    }
#endif
private slots:
    void playbackTimingSeparatesPollCadenceFromAudioCursorLag_data(){
        QTest::addColumn<int>("frameMs");QTest::addColumn<int>("lostOffset");
        for(int duration:{20,60,120})QTest::newRow(qPrintable(QString::number(duration)+"ms"))<<duration<<-1;
        QTest::newRow("20ms-oldest-stalled-packet-lost")<<20<<0;
        QTest::newRow("20ms-interior-old-prefix-packet-lost")<<20<<3;
        QTest::newRow("120ms-oldest-stalled-packet-lost")<<120<<0;
    }
    void playbackTimingSeparatesPollCadenceFromAudioCursorLag(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the encrypted stream fixture");
#else
        QFETCH(int,frameMs);QFETCH(int,lostOffset);Gateway gateway;QVERIFY(gateway.listen());PacedAudioOutput output;StreamEngine stream;
        startGeneratedStream(stream,gateway,streamKey);gateway.announce();stream.setFixtureAudioOutput(&output);QTest::qWait(30);
        const int frameSamples=frameMs*48,frameCount=3600/frameMs;int error=0;
        auto *encoder=opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&error);QCOMPARE(error,OPUS_OK);
        QList<QByteArray> wire,packets;QByteArray reference;std::vector<opus_int16> input(frameSamples*2);std::array<opus_int16,5760*2> decoded;
        for(int i=0;i<frameCount;++i){
            for(int at=0;at<frameSamples;++at){const auto sample=opus_int16(std::sin(2*3.141592653589793*(277+i%19*29)*(i*frameSamples+at)/48000)*(4000+i%20*200));input[at*2]=input[at*2+1]=sample;}
            QByteArray packet(1275,Qt::Uninitialized);const int count=opus_encode(encoder,input.data(),frameSamples,reinterpret_cast<unsigned char*>(packet.data()),packet.size());QVERIFY(count>0);packet.resize(count);
            packets.append(packet);
            const auto encrypted=gateway.group->encrypt(packet,DAVE_MEDIA_TYPE_AUDIO);QVERIFY(!encrypted.isEmpty());wire.append(StreamTestMedia::encryptRtp(encrypted,gateway.key,120,false,quint16(i),quint32(i)*frameSamples,303,4000+i));
        }
        opus_encoder_destroy(encoder);
        QElapsedTimer sourceClock;sourceClock.start();int sent=0,lost=-1;
        const auto sendDue=[&]{const auto due=std::min<qint64>(wire.size(),sourceClock.elapsed()/frameMs+1);while(sent<due){if(sent!=lost)gateway.udp.writeDatagram(wire[sent],gateway.clientAddress,gateway.clientPort);++sent;}};
        QTimer source;source.setTimerType(Qt::PreciseTimer);source.setInterval(5);connect(&source,&QTimer::timeout,&stream,sendDue);sendDue();source.start();
        QTRY_VERIFY_WITH_TIMEOUT(stream.playbackDiagnostics().value("audioPrimed").toBool(),1000);QTest::qWait(200);
        const auto before=stream.takeFixtureAudio();QVERIFY(!before.isEmpty());lost=lostOffset>=0?sent+lostOffset:-1;
        auto *decoder=opus_decoder_create(48000,2,&error);QCOMPARE(error,OPUS_OK);
        for(int i=0;i<packets.size();++i){const auto &packet=packets[i];const bool missing=i==lost;
            const int samples=opus_decode(decoder,missing?nullptr:reinterpret_cast<const unsigned char*>(packet.constData()),missing?0:packet.size(),decoded.data(),missing?frameSamples:5760,0);QCOMPARE(samples,frameSamples);reference.append(reinterpret_cast<const char*>(decoded.data()),samples*4);}
        opus_decoder_destroy(decoder);QCOMPARE(before,reference.first(before.size()));
        // Emulate a GUI stall while the remote source continues. Deliver the
        // packets that accumulated on the network before servicing timers.
        QTest::qSleep(400);sendDue();QTest::qWait(500);
        const auto after=stream.playbackDiagnostics();const auto late=after.value("audioLateWakes").toULongLong();auto recovered=stream.takeFixtureAudio();
        qInfo()<<"After generated stall"<<after;
        QTest::qWait(500);const auto settled=stream.playbackDiagnostics();recovered+=stream.takeFixtureAudio();qInfo()<<"Settled generated playback"<<settled;
        QVERIFY2(after.value("maxPollGapUs").toLongLong()>=350000,"The generated GUI stall was not observed");
        QVERIFY2(settled.value("audioLateWakes").toULongLong()<=late+3,"A historical decode cursor deficit counted every regular poll as a new late wake");
        QCOMPARE(settled.value("audioRebases").toULongLong(),quint64(1));QVERIFY(settled.value("audioRetiredSamples").toULongLong()>0);QVERIFY(!settled.value("audioRecoveryPending").toBool());
        QCOMPARE(settled.value("audioConcealedPackets").toULongLong(),quint64(lost>=0?1:0));
        QVERIFY2(settled.value("audioQueuedMs").toLongLong()<=std::max(40,frameMs*2),"Stale encoded audio persisted after cadence recovered");
        QVERIFY2(settled.value("audioCursorLagMs").toLongLong()<std::max(30,frameMs),"The recovered sample clock retained its historical deficit");
        const auto retired=settled.value("audioRetiredSamples").toLongLong()*4;QVERIFY(!recovered.isEmpty());
        QVERIFY2(recovered==reference.mid(before.size()+retired,recovered.size()),"Retiring stale audio corrupted or reordered the retained Opus samples");
        source.stop();stream.stop();
#endif
    }

    void audioQueueResetRestartsItsBoundedReorderWindow() {
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the encrypted stream fixture");
#else
        Gateway gateway;QVERIFY(gateway.listen());StreamEngine stream;startGeneratedStream(stream,gateway,streamKey);gateway.announce();QTest::qWait(30);
        sendGeneratedAudio(gateway,303,0,1);sendGeneratedAudio(gateway,303,100,1);sendGeneratedAudio(gateway,303,102,2);
        QByteArray rendered;QTRY_VERIFY_WITH_TIMEOUT((rendered+=stream.takeFixtureAudio()).size()>=3840*3,1000);
        QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 4/3"),1000);
        stream.stop();
#endif
    }
    void audioAfterARealPauseGetsANewStartupWindow() {
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the encrypted stream fixture");
#else
        Gateway gateway;QVERIFY(gateway.listen());StreamEngine stream;startGeneratedStream(stream,gateway,streamKey);gateway.announce();QTest::qWait(30);
        const auto packet=generatedOpus();QVERIFY(!packet.isEmpty());int error=0;auto *decoder=opus_decoder_create(48000,2,&error);QCOMPARE(error,OPUS_OK);
        QByteArray reference;std::array<opus_int16,5760*2> pcm;
        for(int i=0;i<12;++i) {
            if(i==6)for(int lost=0;lost<5;++lost){const int samples=opus_decode(decoder,nullptr,0,pcm.data(),960,0);QCOMPARE(samples,960);reference.append(reinterpret_cast<const char*>(pcm.data()),samples*4);}
            const int samples=opus_decode(decoder,reinterpret_cast<const unsigned char*>(packet.constData()),packet.size(),pcm.data(),5760,0);QCOMPARE(samples,960);reference.append(reinterpret_cast<const char*>(pcm.data()),samples*4);
        }
        opus_decoder_destroy(decoder);
        int sent=0,target=6;QTimer producer;producer.setTimerType(Qt::PreciseTimer);producer.setInterval(20);
        auto send=[&] {
            if(sent>=target){producer.stop();return;}
            const auto protectedAudio=gateway.group->encrypt(packet,DAVE_MEDIA_TYPE_AUDIO);QVERIFY(!protectedAudio.isEmpty());
            gateway.udp.writeDatagram(StreamTestMedia::encryptRtp(protectedAudio,gateway.key,120,false,quint16(sent),quint32(sent)*960,303,3000+sent),gateway.clientAddress,gateway.clientPort);++sent;
        };
        connect(&producer,&QTimer::timeout,&stream,send);send();producer.start();QTRY_COMPARE_WITH_TIMEOUT(sent,6,1000);producer.stop();
        QTest::qWait(240);QVERIFY(stream.diagnosticSummary().contains("audio/decoded 6/6"));
        auto rendered=stream.takeFixtureAudio();QCOMPARE(rendered.size(),11*3840);
        target=12;send();producer.start();QTRY_COMPARE_WITH_TIMEOUT(sent,12,1000);producer.stop();QTest::qWait(60);rendered+=stream.takeFixtureAudio();
        QVERIFY2(rendered.left(reference.size())==reference,"A resumed source was concealed or skipped before its fresh prefill completed");
        QVERIFY2(stream.diagnosticSummary().contains("audio/decoded 12/12"),qPrintable(stream.diagnosticSummary()));stream.stop();
#endif
    }
    void generatedContinuousAudioFollowsItsSampleClock_data() {
        QTest::addColumn<int>("frameMs");QTest::addColumn<int>("pollMs");QTest::addColumn<bool>("deferSocketNotification");QTest::addColumn<int>("startupGap");
        QTest::newRow("20ms-audio-10ms-video")<<20<<10<<false<<0;
        QTest::newRow("20ms-audio-17ms-video")<<20<<17<<false<<0;
        QTest::newRow("20ms-audio-30ms-video")<<20<<30<<false<<0;
        QTest::newRow("20ms-audio-70ms-video")<<20<<70<<false<<0;
        QTest::newRow("20ms-audio-pending-udp-30ms-video")<<20<<30<<true<<0;
        QTest::newRow("20ms-startup-reordered")<<20<<10<<false<<1;
        QTest::newRow("20ms-startup-lost")<<20<<10<<false<<2;
        QTest::newRow("20ms-middle-packet-jitter")<<20<<10<<false<<3;
        QTest::newRow("10ms-audio")<<10<<10<<false<<0;
        QTest::newRow("40ms-audio")<<40<<10<<false<<0;
        QTest::newRow("60ms-audio")<<60<<10<<false<<0;
    }
    void generatedContinuousAudioFollowsItsSampleClock() {
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the encrypted stream fixture");
#else
        QFETCH(int,frameMs);QFETCH(int,pollMs);QFETCH(bool,deferSocketNotification);QFETCH(int,startupGap);
        Gateway gateway;QVERIFY(gateway.listen());StreamEngine stream;startGeneratedStream(stream,gateway,streamKey);gateway.announce();
        stream.setFixturePollInterval(pollMs);QTest::qWait(30);
        if(deferSocketNotification){auto *udp=stream.findChild<QUdpSocket*>();QVERIFY(udp);QObject::disconnect(udp,&QUdpSocket::readyRead,&stream,nullptr);}
        constexpr int durationMs=1200;const int frameSamples=frameMs*48,frameCount=durationMs/frameMs;
        int error=0;auto *encoder=opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&error);QCOMPARE(error,OPUS_OK);
        auto *referenceDecoder=opus_decoder_create(48000,2,&error);QCOMPARE(error,OPUS_OK);
        QList<QByteArray> packets;QByteArray reference;std::vector<opus_int16> input(frameSamples*2);std::array<opus_int16,5760*2> referencePcm;
        for(int frame=0;frame<frameCount;++frame) {
            for(int sample=0;sample<frameSamples;++sample) {
                const auto value=opus_int16(std::sin(2*3.141592653589793*440*(frame*frameSamples+sample)/48000)*12000);
                input[sample*2]=input[sample*2+1]=value;
            }
            QByteArray packet(1275,Qt::Uninitialized);const int encoded=opus_encode(encoder,input.data(),frameSamples,reinterpret_cast<unsigned char*>(packet.data()),packet.size());
            QVERIFY(encoded>0);packet.resize(encoded);packets.append(packet);
            const bool lost=startupGap==2 && frame==1;
            const int decoded=opus_decode(referenceDecoder,lost?nullptr:reinterpret_cast<const unsigned char*>(packet.constData()),lost?0:packet.size(),referencePcm.data(),lost?frameSamples:5760,0);
            QCOMPARE(decoded,frameSamples);reference.append(reinterpret_cast<const char*>(referencePcm.data()),decoded*4);
        }
        opus_encoder_destroy(encoder);opus_decoder_destroy(referenceDecoder);
        QByteArray rendered;qsizetype initialBytes=0;int sent=0;
        QTimer drain;drain.setTimerType(Qt::PreciseTimer);drain.setInterval(2);
        connect(&drain,&QTimer::timeout,&stream,[&] {const auto chunk=stream.takeFixtureAudio();if(!chunk.isEmpty() && !initialBytes)initialBytes=chunk.size();rendered+=chunk;});drain.start();
        QTimer producer;producer.setTimerType(Qt::PreciseTimer);producer.setInterval(frameMs);
        auto sendNext=[&] {
            if(sent>=packets.size()){producer.stop();return;}
            if(startupGap==2 && sent==1){++sent;return;}
            const auto protectedAudio=gateway.group->encrypt(packets[sent],DAVE_MEDIA_TYPE_AUDIO);QVERIFY(!protectedAudio.isEmpty());
            const auto datagram=StreamTestMedia::encryptRtp(protectedAudio,gateway.key,120,false,quint16(sent),quint32(sent*frameSamples),303,2000+sent);
            if((startupGap==1 && sent==1) || (startupGap==3 && sent==frameCount/2))QTimer::singleShot(startupGap==1?50:10,&stream,[&,datagram] {gateway.udp.writeDatagram(datagram,gateway.clientAddress,gateway.clientPort);});
            else gateway.udp.writeDatagram(datagram,gateway.clientAddress,gateway.clientPort);
            ++sent;
        };
        connect(&producer,&QTimer::timeout,&stream,sendNext);sendNext();producer.start();
        QTRY_COMPARE_WITH_TIMEOUT(sent,frameCount,durationMs+2000);producer.stop();
        QTRY_VERIFY_WITH_TIMEOUT(rendered.size()>=reference.size(),1000);drain.stop();rendered+=stream.takeFixtureAudio();
        qInfo().noquote()<<QString("Generated audio %1ms packet / %2ms poll: firstPCM=%3B, expected=%4B, rendered=%5B, exactReference=%6")
            .arg(frameMs).arg(pollMs).arg(initialBytes).arg(reference.size()).arg(rendered.size()).arg(rendered.left(reference.size())==reference);
        if(rendered.left(reference.size())!=reference)qInfo().noquote()<<stream.diagnosticSummary();
        QVERIFY2(initialBytes>=48000*4*60/1000,"Audio output began before its 60 ms PCM prefill");
        QVERIFY2(rendered.left(reference.size())==reference,"Clean generated audio was skipped, concealed or reordered by the presentation poll");
        QVERIFY2(stream.diagnosticSummary().contains(QString("audio/decoded %1/%1").arg(frameCount-(startupGap==2?1:0))),qPrintable(stream.diagnosticSummary()));
        stream.stop();
#endif
    }
    void endedStreamDoesNotPreventNewOwnerAudioWithoutPriorSsrcAnnouncement() {
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the encrypted stream fixture");
#else
        Gateway first,second;second.ownerId="5555";QVERIFY(first.listen());QVERIFY(second.listen());
        StreamEngine stream;startGeneratedStream(stream,first,streamKey);first.announce();
        QTest::qWait(30);sendGeneratedAudio(first,303);
        QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 6/6"),1000);
        const auto staleAudio=first.group->encrypt(generatedOpus(),DAVE_MEDIA_TYPE_AUDIO);QVERIFY(!staleAudio.isEmpty());
        stream.onStreamDelete({{"stream_key",streamKey}});QVERIFY(!stream.active());
        const QString nextKey="guild:1111:2222:5555";startGeneratedStream(stream,second,nextKey);
        // Existing senders can deliver authenticated audio before repeating a
        // speaking/audio-SSRC announcement to a newly joined viewer.
        second.send(12,{{"user_id",second.ownerId},{"video_ssrc",202},{"streams",QJsonArray{QJsonObject{{"type","screen"},{"ssrc",202},{"active",true},{"quality",100}}}}});
        // Even a valid outer transport packet from the previous stream cannot
        // establish B's mapping with A's stale owner key.
        second.udp.writeDatagram(StreamTestMedia::encryptRtp(staleAudio,second.key,120,false,0,0,703,900),second.clientAddress,second.clientPort);
        QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio RTP 1"),1000);
        QVERIFY(stream.diagnosticSummary().contains("audio announcements/mappings 0/0"));
        QTest::qWait(30);sendGeneratedAudio(second,503);
        QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 6/6"),1000);
        QVERIFY(stream.diagnosticSummary().contains("audio announcements/mappings 0/1"));
        // A delayed matching announcement must retain the queue and decoder.
        second.send(5,{{"user_id",second.ownerId},{"ssrc",503},{"speaking",2}});
        QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio announcements/mappings 1/1"),1000);
        sendGeneratedAudio(second,503,1000);QTRY_VERIFY2_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 12/12"),qPrintable(stream.diagnosticSummary()),1000);
        // Another user's claimed source is ignored; a changed SSRC is adopted
        // only after audio authenticates with the selected owner's current key.
        second.send(5,{{"user_id","6666"},{"ssrc",903},{"speaking",2}});
        sendGeneratedAudio(second,803);QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 18/18"),1000);
        QVERIFY(stream.diagnosticSummary().contains("audio announcements/mappings 1/2"));
        sendGeneratedAudio(second,503,2000,1);QTest::qWait(30);
        QVERIFY(stream.diagnosticSummary().contains("audio announcements/mappings 1/2"));
        sendGeneratedAudio(second,803,1000);QTRY_VERIFY2_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 24/24"),qPrintable(stream.diagnosticSummary()),1000);
        // A sender can later reuse an old SSRC without repeating metadata. It
        // may authenticate again once the current source has fallen silent.
        QTest::qWait(2100);sendGeneratedAudio(second,503,3000);
        QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 30/30"),1000);
        QVERIFY(stream.diagnosticSummary().contains("audio announcements/mappings 1/3"));
        QVERIFY2(stream.error().isEmpty(),qPrintable(stream.error()));stream.stop();
#endif
    }
    void subscriptionsPreserveUnspecifiedAudioWhileDisablingUnselectedVideo() {
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the encrypted stream fixture");
#else
        Gateway gateway;QVERIFY(gateway.listen());StreamEngine stream;startGeneratedStream(stream,gateway,streamKey);
        const auto layer=[](int source,int quality,bool active=true){return QJsonObject{{"type","screen"},{"ssrc",source},{"active",active},{"quality",quality}};};
        gateway.send(12,{{"user_id",gateway.ownerId},{"audio_ssrc",303},{"streams",QJsonArray{layer(202,50),layer(404,100)}}});
        QTRY_COMPARE_WITH_TIMEOUT(gateway.wants.value("404").toInt(),100,1000);
        QCOMPARE(gateway.wants.value("any").toInt(),100);QCOMPARE(gateway.wants.value("202").toInt(),0);QVERIFY(!gateway.wants.contains("303"));
        // A replacement list must retire the old selected layer as well as
        // disabling unwanted layers in the new list, while leaving audio on.
        gateway.send(12,{{"user_id",gateway.ownerId},{"streams",QJsonArray{layer(606,100),layer(808,50)}}});
        QTRY_COMPARE_WITH_TIMEOUT(gateway.wants.value("606").toInt(),100,1000);
        QCOMPARE(gateway.wants.value("any").toInt(),100);QCOMPARE(gateway.wants.value("404").toInt(),0);QCOMPARE(gateway.wants.value("808").toInt(),0);
        gateway.send(12,{{"user_id",gateway.ownerId},{"streams",QJsonArray{layer(606,100,false)}}});
        QTRY_COMPARE_WITH_TIMEOUT(gateway.wants.value("606").toInt(),0,1000);QCOMPARE(gateway.wants.value("any").toInt(),100);
        sendGeneratedAudio(gateway,303);QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 6/6"),1000);stream.stop();
#endif
    }
    void ownerResumeAndAudioControlsKeepOutputLifecycleIndependentOfNewEncryption() {
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the encrypted stream fixture");
#else
        Gateway gateway;QVERIFY(gateway.listen());StreamEngine stream;stream.setVolume(.73);startGeneratedStream(stream,gateway,streamKey);
        gateway.announce();QTest::qWait(30);sendGeneratedAudio(gateway,303);QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 6/6"),1000);
        gateway.send(13,{{"user_id",gateway.ownerId}});QTRY_COMPARE_WITH_TIMEOUT(stream.status(),QString("Waiting for the stream to resume…"),1000);QVERIFY(stream.encryptedReady());
        gateway.send(5,{{"user_id",gateway.ownerId},{"ssrc",503},{"speaking",2}});
        QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio announcements/mappings 2/2"),1000);
        sendGeneratedAudio(gateway,503);QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 12/12"),1000);QCOMPARE(stream.volume(),.73);
        stream.setAudioMuted(true);sendGeneratedAudio(gateway,803,0,3);QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio RTP 15"),1000);
        QVERIFY(stream.diagnosticSummary().contains("audio announcements/mappings 2/2"));QVERIFY(stream.audioMuted());
        stream.setAudioMuted(false);stream.setDeafened(true);sendGeneratedAudio(gateway,803,3,3);QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio RTP 18"),1000);
        QVERIFY(stream.diagnosticSummary().contains("audio announcements/mappings 2/2"));
        stream.setDeafened(false);sendGeneratedAudio(gateway,503,6);QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 18/18"),1000);
        QVERIFY(stream.diagnosticSummary().contains("audio drops muted/deaf/keys/unmapped/decoder/retired 3/3/0/0/0/0"));stream.stop();
#endif
    }
    void remoteEndPreservesSelectionUntilExplicitStop(){
        StreamEngine stream;if(!stream.available())QSKIP("Native stream viewing is unavailable");
        QSignalSpy stopped(&stream,&StreamEngine::stopRequested);
        stream.watch(streamKey,"Generated stream");QVERIFY(stream.active());
        stream.onStreamDelete({{"stream_key","guild:1111:2222:5555"}});
        QVERIFY(stream.active());QCOMPARE(stopped.size(),0);
        stream.onStreamDelete({{"stream_key",streamKey}});
        QCOMPARE(stopped.size(),0);QVERIFY(!stream.active());QVERIFY(stream.waitingForRestart());
        QCOMPARE(stream.streamKey(),QString(streamKey));QCOMPARE(stream.title(),"Generated stream");
        QVERIFY(stream.status().contains("Waiting"));QVERIFY(!stream.receiving());
        stream.onStreamDelete({{"stream_key",streamKey}});QCOMPARE(stopped.size(),0);
        stream.onStreamRestart("guild:1111:2222:5555");QVERIFY(!stream.active());
        stream.onStreamRestart(streamKey);QVERIFY(stream.active());QVERIFY(!stream.waitingForRestart());
        stream.onStreamDelete({{"stream_key",streamKey}});QVERIFY(stream.waitingForRestart());
        stream.stop();QCOMPARE(stopped.size(),1);QVERIFY(stream.streamKey().isEmpty());
        stream.onStreamRestart(streamKey);QVERIFY(!stream.active());QVERIFY(!stream.waitingForRestart());
    }
    void explicitCancellationClearsAWaitingSelection(){
        StreamEngine stream;if(!stream.available())QSKIP("Native stream viewing is unavailable");
        QSignalSpy stopped(&stream,&StreamEngine::stopRequested);
        stream.watch(streamKey,"Generated stream");stream.onStreamDelete({{"stream_key",streamKey}});
        stream.onStreamDelete({{"stream_key",streamKey},{"relay_cancelled",true}});
        QCOMPARE(stopped.size(),1);QVERIFY(stream.streamKey().isEmpty());QVERIFY(!stream.waitingForRestart());
        stream.onStreamRestart(streamKey);QVERIFY(!stream.active());
    }
    void restartedOwnerReusesTheSinkWithFreshAudio_data(){
        QTest::addColumn<bool>("deleted");QTest::addColumn<bool>("serverFirst");
        QTest::newRow("ended-create-first")<<true<<false;
        QTest::newRow("ended-server-first")<<true<<true;
        QTest::newRow("allocation-change-create-first")<<false<<false;
        QTest::newRow("allocation-change-server-first")<<false<<true;
    }
    void restartedOwnerReusesTheSinkWithFreshAudio(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the encrypted stream fixture");
#else
        QFETCH(bool,deleted);QFETCH(bool,serverFirst);
        Gateway gateway;QVERIFY(gateway.listen());StreamEngine stream;QVideoSink sink;
        stream.setVideoSink(&sink);stream.setVolume(0.37);
        QSignalSpy stopped(&stream,&StreamEngine::stopRequested),requested(&stream,&StreamEngine::watchRequested);
        startGeneratedStream(stream,gateway,streamKey);gateway.announce();QTest::qWait(30);
        sendGeneratedAudio(gateway,303);QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 6/6"),1000);
        QByteArray rendered;QTRY_VERIFY_WITH_TIMEOUT(!(rendered+=stream.takeFixtureAudio()).isEmpty(),1000);
        stream.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});
        QVERIFY(stream.encryptedReady());QVERIFY(stream.diagnosticSummary().contains("audio/decoded 6/6"));
        if(deleted){stream.onStreamDelete({{"stream_key",streamKey}});QVERIFY(stream.waitingForRestart());stream.onStreamRestart(streamKey);}
        gateway.rtcServerId="7778";gateway.rtcChannelId="8888";gateway.groupReady=false;gateway.identified=false;
        const QJsonObject create{{"stream_key",streamKey},{"rtc_server_id",gateway.rtcServerId},{"rtc_channel_id",gateway.rtcChannelId}};
        const QJsonObject server{{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","fresh-generated-token"}};
        if(serverFirst){stream.onStreamServer(server);stream.onStreamCreate(create);}else{stream.onStreamCreate(create);stream.onStreamServer(server);}
        QTRY_VERIFY_WITH_TIMEOUT(stream.encryptedReady() || !gateway.error.isEmpty(),5000);
        QVERIFY2(stream.encryptedReady(),qPrintable(stream.error()));QVERIFY(gateway.identified);
        QCOMPARE(stream.videoSink(),static_cast<QObject*>(&sink));QCOMPARE(stream.title(),"Generated lifecycle stream");
        QCOMPARE(stream.volume(),0.37);QCOMPARE(stopped.count(),0);QCOMPARE(requested.count(),1);
        gateway.announce();QTest::qWait(30);sendGeneratedAudio(gateway,303);
        QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains("audio/decoded 6/6"),1000);
        rendered.clear();QTRY_VERIFY_WITH_TIMEOUT(!(rendered+=stream.takeFixtureAudio()).isEmpty(),1000);
        stream.stop();
#endif
    }
    void media_data(){QTest::addColumn<QString>("codec");QTest::addColumn<bool>("createCommit");QTest::newRow("H264-welcome")<<QString("H264")<<false;QTest::newRow("VP8-commit")<<QString("VP8")<<true;QTest::newRow("HEVC-welcome")<<QString("H265")<<false;QTest::newRow("AV1-welcome")<<QString("AV1")<<false;QTest::newRow("AV1-commit")<<QString("AV1")<<true;}
    void media(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the encrypted stream fixture");
#else
        QFETCH(QString,codec);QFETCH(bool,createCommit);Gateway gateway;gateway.codec=codec;gateway.createCommit=createCommit;QVERIFY(gateway.listen());
        StreamEngine stream;QVERIFY(stream.available());QVERIFY(stream.configureLocalFixture(gateway.url()));QVideoSink sink;stream.setVideoSink(&sink);
        connect(&stream,&StreamEngine::watchRequested,&stream,[&](const QString&){stream.setIdentity("3333","local-fixture-session");
            stream.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","local-token"}});
            stream.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});});
        stream.watch(streamKey,"Generated stream");QTRY_VERIFY_WITH_TIMEOUT(stream.encryptedReady() || !gateway.error.isEmpty(),5000);
        QVERIFY2(gateway.error.isEmpty(),qPrintable(gateway.error));QVERIFY2(stream.encryptedReady(),qPrintable(stream.diagnosticSummary()));QVERIFY(gateway.identified);QVERIFY(gateway.selected);
        QStringList offered;QSet<int> payloadTypes;int lastPriority=0;
        const QMap<QString,QPair<int,int>> expected{{"AV1",{101,102}},{"H265",{103,104}},{"H264",{105,106}},{"VP8",{107,108}}};
        for(const auto &entry:gateway.offeredCodecs){
            const auto video=entry.toObject();if(video.value("type").toString()!="video")continue;
            const auto name=video.value("name").toString();QVERIFY(expected.contains(name));offered.append(name);
            QCOMPARE(video.value("payload_type").toInt(),expected[name].first);QCOMPARE(video.value("rtx_payload_type").toInt(),expected[name].second);
            QVERIFY(video.value("decode").toBool());QVERIFY(!video.value("encode").toBool());
            QVERIFY(video.value("priority").toInt()>lastPriority);lastPriority=video.value("priority").toInt();
            QVERIFY(!payloadTypes.contains(expected[name].first));payloadTypes.insert(expected[name].first);
            QVERIFY(!payloadTypes.contains(expected[name].second));payloadTypes.insert(expected[name].second);
        }
        QCOMPARE(offered,QStringList({"AV1","H265","H264","VP8"}));
        gateway.announce();QTRY_VERIFY_WITH_TIMEOUT(gateway.pliCount>0,1000);
        const auto encoded=StreamTestMedia::encodedFrame(codec);QVERIFY(!encoded.isEmpty());const auto encrypted=gateway.group->encrypt(encoded);QVERIFY(!encrypted.isEmpty());
        const auto fragments=StreamTestMedia::packetize(codec,encrypted,codec=="AV1"?8:80);QVERIFY(fragments.size()>=3);
        const int payloadType=codec=="AV1"?101:codec=="H265"?103:codec=="H264"?105:107;
        QList<QByteArray> packets;quint16 sequence=65530;quint32 counter=250;
        for(qsizetype i=0;i<fragments.size();++i)packets.append(StreamTestMedia::encryptRtp(fragments[i],gateway.key,payloadType,i+1==fragments.size(),sequence++,9000,202,counter++));
        // Real transport ciphertext, reordered packets, and sequence wrap.
        gateway.udp.writeDatagram(packets[0],gateway.clientAddress,gateway.clientPort);
        gateway.udp.writeDatagram(packets[2],gateway.clientAddress,gateway.clientPort);
        QTest::qWait(35);
        if(createCommit)gateway.udp.writeDatagram(packets[1],gateway.clientAddress,gateway.clientPort);
        else {
            QByteArray repair(2,'\0');qToBigEndian<quint16>(65531,repair.data());repair+=fragments[1];
            const auto rtx=StreamTestMedia::encryptRtp(repair,gateway.key,payloadType+1,false,12,9000,203,counter++);
            gateway.udp.writeDatagram(rtx,gateway.clientAddress,gateway.clientPort);
        }
        for(qsizetype i=3;i<packets.size();++i)gateway.udp.writeDatagram(packets[i],gateway.clientAddress,gateway.clientPort);
        QTRY_VERIFY_WITH_TIMEOUT(stream.receiving(),3000);QCOMPARE(stream.videoWidth(),320);QCOMPARE(stream.videoHeight(),180);QCOMPARE(stream.frameCount(),quint64(1));QCOMPARE(stream.playbackDiagnostics().value("sinkSubmittedFrames").toULongLong(),quint64(1));
        const auto image=sink.videoFrame().toImage();QVERIFY(!image.isNull());QVERIFY(image.pixelColor(100,90).red()>image.pixelColor(100,90).blue());
        QVERIFY(gateway.nackCount>0);QVERIFY(stream.diagnosticSummary().contains("transport/media errors 0/0"));
        int opusError=0;auto encoder=opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&opusError);QCOMPARE(opusError,OPUS_OK);
        std::array<opus_int16,1920> pcm{};for(size_t i=0;i<pcm.size();++i)pcm[i]=opus_int16((i%40)*100);
        for(quint16 i=0;i<6;++i){
            QByteArray encodedAudio(1275,Qt::Uninitialized);const int size=opus_encode(encoder,pcm.data(),960,reinterpret_cast<unsigned char*>(encodedAudio.data()),encodedAudio.size());QVERIFY(size>0);encodedAudio.resize(size);
            const auto protectedAudio=gateway.group->encrypt(encodedAudio,DAVE_MEDIA_TYPE_AUDIO);QVERIFY(!protectedAudio.isEmpty());
            gateway.udp.writeDatagram(StreamTestMedia::encryptRtp(protectedAudio,gateway.key,120,false,i,i*960,303,counter++),gateway.clientAddress,gateway.clientPort);
        }
        opus_encoder_destroy(encoder);
        QTRY_VERIFY_WITH_TIMEOUT(stream.diagnosticSummary().contains(QRegularExpression("audio/decoded 6/[1-9]")),1000);
        QByteArray senderReport(28,'\0');senderReport[0]=char(0x80);senderReport[1]=char(200);qToBigEndian<quint16>(6,senderReport.data()+2);
        qToBigEndian<quint32>(202,senderReport.data()+4);qToBigEndian<quint32>(0x01020304,senderReport.data()+8);qToBigEndian<quint32>(0x05060708,senderReport.data()+12);
        gateway.udp.writeDatagram(VideoPacket::encryptRtcp(senderReport,gateway.key,counter++),gateway.clientAddress,gateway.clientPort);
        QTRY_COMPARE_WITH_TIMEOUT(gateway.reportSource,quint32(202),1500);QCOMPARE(gateway.reportLsr,quint32(0x03040506));
        gateway.udp.writeDatagram(packets.last(),gateway.clientAddress,gateway.clientPort);QTest::qWait(60);QCOMPARE(stream.frameCount(),quint64(1));
        stream.setAudioMuted(true);stream.setDeafened(true);stream.setDeafened(false);QVERIFY(stream.audioMuted());
        const auto summary=stream.diagnosticSummary();QVERIFY(!summary.contains("local-token"));QVERIFY(!summary.contains(streamKey));QVERIFY(!summary.contains("Generated stream"));
        QSignalSpy stopped(&stream,&StreamEngine::stopRequested);stream.stop();QCOMPARE(stopped.size(),1);QVERIFY(!stream.active());QVERIFY(!sink.videoFrame().isValid());
#endif
    }
    void modernCodecSwitchRetainsConnectionAndSink() {
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers required");
#else
        Gateway gateway;gateway.codec="AV1";QVERIFY(gateway.listen());StreamEngine stream;QVideoSink sink;stream.setVideoSink(&sink);
        startGeneratedStream(stream,gateway,streamKey);gateway.announce();QTRY_VERIFY_WITH_TIMEOUT(gateway.pliCount>0,1000);
        quint16 sequence=100;quint32 nonce=9000,stamp=9000;quint64 submitted=0;
        for(const auto &codec:QStringList{"AV1","H265","H264","VP8","AV1"}) {
            gateway.send(14,{{"video_codec",codec}});QTest::qWait(20);
            const auto daveCodec=codec=="AV1"?DAVE_CODEC_AV1:codec=="H265"?DAVE_CODEC_H265:codec=="H264"?DAVE_CODEC_H264:DAVE_CODEC_VP8;
            daveEncryptorAssignSsrcToCodec(gateway.group->encryptor,202,daveCodec);
            const auto plain=StreamTestMedia::encodedFrame(codec);QVERIFY(!plain.isEmpty());const auto encrypted=gateway.group->encrypt(plain);QVERIFY(!encrypted.isEmpty());
            const auto packets=StreamTestMedia::packetize(codec,encrypted,900);QVERIFY(!packets.isEmpty());
            const int pt=codec=="AV1"?101:codec=="H265"?103:codec=="H264"?105:107;
            for(qsizetype i=0;i<packets.size();++i)
                gateway.udp.writeDatagram(StreamTestMedia::encryptRtp(packets[i],gateway.key,pt,i+1==packets.size(),sequence++,stamp,202,nonce++),gateway.clientAddress,gateway.clientPort);
            ++submitted;stamp+=3000;
            QTRY_COMPARE_WITH_TIMEOUT(stream.playbackDiagnostics().value("sinkSubmittedFrames").toULongLong(),submitted,3000);
            QCOMPARE(stream.codec(),codec);QVERIFY(stream.connected());QVERIFY(stream.encryptedReady());QVERIFY(stream.error().isEmpty());
            QCOMPARE(sink.videoFrame().size(),QSize(320,180));
        }
        // Opus still decrypts through the same MLS group after all video switches.
        sendGeneratedAudio(gateway,303,0,6);
        QTRY_VERIFY_WITH_TIMEOUT(stream.playbackDiagnostics().value("audioDecodedPackets").toULongLong()>0,1000);
        QVERIFY(stream.diagnosticSummary().contains("transport/media errors 0/0"));stream.stop();
#endif
    }
    void highestRenditionAndLargeRepair() {
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for the encrypted stream fixture");
#else
        Gateway gateway;QVERIFY(gateway.listen());StreamEngine stream;QVERIFY(stream.configureLocalFixture(gateway.url()));
        QVideoSink sink;stream.setVideoSink(&sink);
        connect(&stream,&StreamEngine::watchRequested,&stream,[&](const QString&){stream.setIdentity("3333","local-fixture-session");
            stream.onStreamServer({{"stream_key",streamKey},{"endpoint","fixture.discord.media"},{"token","local-token"}});
            stream.onStreamCreate({{"stream_key",streamKey},{"rtc_server_id","5556"},{"rtc_channel_id","6666"}});});
        stream.watch(streamKey,"Generated high resolution stream");QTRY_VERIFY_WITH_TIMEOUT(stream.encryptedReady() || !gateway.error.isEmpty(),5000);
        QVERIFY2(gateway.error.isEmpty(),qPrintable(gateway.error));QVERIFY(stream.encryptedReady());
        const auto rendition=[](int source,int quality,int height,int fps,bool active=true){return QJsonObject{
            {"type","screen"},{"ssrc",source},{"rtx_ssrc",source+1},{"quality",quality},{"active",active},{"max_framerate",fps},
            {"max_resolution",QJsonObject{{"type","fixed"},{"width",height*16/9},{"height",height}}}};};
        gateway.send(12,{{"user_id","4444"},{"audio_ssrc",303},{"video_ssrc",202},{"streams",QJsonArray{
            rendition(202,50,360,30),rendition(302,100,720,30),rendition(404,100,1440,60),rendition(502,100,2160,60,false)}}});
        QTRY_COMPARE_WITH_TIMEOUT(gateway.wants.value("404").toInt(),100,1500);
        QCOMPARE(gateway.wants.value("202").toInt(),0);QCOMPARE(gateway.wants.value("302").toInt(),0);QCOMPARE(gateway.wants.value("502").toInt(),0);
        const auto encoded=StreamTestMedia::encodedFrame("H264",2560,1440);QVERIFY(!encoded.isEmpty());
        const auto encrypted=gateway.group->encrypt(encoded);QVERIFY(!encrypted.isEmpty());
        const auto fragments=StreamTestMedia::packetize("H264",encrypted,16);QVERIFY(fragments.size()>256);QVERIFY(fragments.size()<4096);
        QList<QByteArray> packets;quint16 sequence=65000;quint32 counter=200;
        for(qsizetype i=0;i<fragments.size();++i)packets.append(StreamTestMedia::encryptRtp(fragments[i],gateway.key,105,i+1==fragments.size(),sequence++,9000,404,counter++));
        // Repair an early missing packet only after the whole large keyframe
        // reaches the application's reorder queue. The old 256-packet queue
        // discarded this valid high layer. Yield between small send batches so
        // a host's UDP receive ceiling cannot add unrelated kernel packet loss;
        // the missing sequence still keeps every later fragment pending.
        QElapsedTimer burst;burst.start();
        QCOMPARE(gateway.udp.writeDatagram(packets[0],gateway.clientAddress,gateway.clientPort),qint64(packets[0].size()));
        for(qsizetype i=2;i<packets.size();++i) {
            QCOMPARE(gateway.udp.writeDatagram(packets[i],gateway.clientAddress,gateway.clientPort),qint64(packets[i].size()));
            if((i-1)%32==0)QCoreApplication::processEvents(QEventLoop::AllEvents,1);
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents,1);
        QCoreApplication::processEvents(QEventLoop::AllEvents,1);
        const auto beforeRepair=stream.diagnosticSummary();
        qInfo().nospace()<<"Generated large repair: encoded="<<encoded.size()<<"B, fragments="<<fragments.size()
                         <<", delivered-before-repair="<<packets.size()-1<<", burst="<<burst.elapsed()<<"ms";
        QVERIFY2(beforeRepair.contains(QString("video packets/frames/decoded %1/0/0").arg(packets.size()-1)),qPrintable(beforeRepair));
        QCOMPARE(stream.frameCount(),quint64(0));QVERIFY(!stream.receiving());
        QVERIFY2(burst.elapsed()<90,"The large keyframe must reach the reorder queue before its existing 90ms repair deadline");
        QVERIFY2(gateway.nackCount>0,qPrintable(beforeRepair));
        QByteArray repair(2,'\0');qToBigEndian<quint16>(65001,repair.data());repair+=fragments[1];
        const auto repairedPacket=StreamTestMedia::encryptRtp(repair,gateway.key,106,false,12,9000,405,counter++);
        QCOMPARE(gateway.udp.writeDatagram(repairedPacket,gateway.clientAddress,gateway.clientPort),qint64(repairedPacket.size()));
        QTRY_VERIFY2_WITH_TIMEOUT(stream.receiving(),qPrintable(stream.diagnosticSummary()),4000);QCOMPARE(stream.videoWidth(),2560);QCOMPARE(stream.videoHeight(),1440);
        QVERIFY2(stream.diagnosticSummary().contains(QString("video packets/frames/decoded %1/1/1").arg(packets.size())),qPrintable(stream.diagnosticSummary()));
        QVERIFY(stream.diagnosticSummary().contains("transport/media errors 0/0"));
        QCOMPARE(sink.videoFrame().size(),QSize(2560,1440));QCOMPARE(stream.frameCount(),quint64(1));
        // Explicitly inactive state must clear the view instead of resurrecting
        // the old top-level SSRC through the legacy fallback.
        gateway.send(12,{{"user_id","4444"},{"video_ssrc",404},{"streams",QJsonArray{rendition(404,100,1440,60,false)}}});
        QTRY_VERIFY_WITH_TIMEOUT(!stream.receiving(),1000);QVERIFY(!sink.videoFrame().isValid());QCOMPARE(gateway.wants.value("404").toInt(),0);
        stream.stop();
#endif
    }
    void invalidTargetNeverConnects(){StreamEngine stream;QSignalSpy requested(&stream,&StreamEngine::watchRequested);stream.watch("https://example.com","invalid");QCOMPARE(requested.size(),0);QVERIFY(!stream.active());QVERIFY(!stream.error().isEmpty());}
};
QTEST_GUILESS_MAIN(StreamGatewayTest)
#include "stream_gateway_test.moc"
