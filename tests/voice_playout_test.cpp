// Generated media only. The independent UDP receiver and 20 ms playout clock
// keep running while GUI stalls, so late audio cannot pass by decoding afterward.
#include <QtTest>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <QScopeGuard>
#include <QJsonDocument>
#include <QJsonArray>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QtEndian>
#include <QPointer>
#include <opus/opus.h>
#include <array>
#include <atomic>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "VoiceEngine.h"
#include "VoicePacket.h"
#include "VideoPacket.h"
#include "BroadcastPacket.h"
#include "CameraEngine.h"
#include "StreamTestMedia.h"
#include <QVideoSink>
#include <QVideoFrame>
#ifdef RELAY_TEST_HAS_DAVE
#include <dave/dave.h>
#endif
#ifdef RELAY_TEST_MLSPP
#include <mls/messages.h>
#endif

namespace {
constexpr auto selfId="3333",peerId="4444";
constexpr quint64 channel=2222;
constexpr quint32 selfSsrc=101,peerSsrc=202;
constexpr auto mode="aead_xchacha20_poly1305_rtpsize";
const unsigned char *data(const QByteArray &b){return reinterpret_cast<const unsigned char*>(b.constData());}
unsigned char *data(QByteArray &b){return reinterpret_cast<unsigned char*>(b.data());}
QByteArray tone(int frameCount){
    QByteArray pcm(frameCount*3840,Qt::Uninitialized);
    for(int i=0;i<frameCount*960;i++){
        const qint16 sample=qint16(std::sin(i*2.0*3.141592653589793*440.0/48000)*6000);
        std::memcpy(pcm.data()+i*4,&sample,2);std::memcpy(pcm.data()+i*4+2,&sample,2);
    }
    return pcm;
}
#ifdef RELAY_TEST_MLSPP
// The fixture follows the MLS external-sender construction in Discord's MIT-licensed
// libdave tests. Its signing key and all media are generated locally for this test.
class MlsFixture {
public:
    mlspp::CipherSuite suite{mlspp::CipherSuite::ID::P256_AES128GCM_SHA256_P256};
    mlspp::SignaturePrivateKey signing=mlspp::SignaturePrivateKey::generate(suite);
    mlspp::ExternalSender sender;
    mlspp::bytes_ns::bytes group;
    DAVESessionHandle session=nullptr;
    DAVEEncryptorHandle encryptor=nullptr;
    DAVEDecryptorHandle decryptor=nullptr;
    QString failure;
    MlsFixture(){
        group.resize(8);qToBigEndian<quint64>(channel,group.data());
        sender.signature_key=signing.public_key;
        sender.credential=mlspp::Credential::basic({0x00,0x01,0x01,0x00});
        session=daveSessionCreate(nullptr,nullptr,nullptr,nullptr);
        const auto external=externalSender();daveSessionSetExternalSender(session,data(external),external.size());
        daveSessionInit(session,1,channel,peerId);
    }
    ~MlsFixture(){if(encryptor)daveEncryptorDestroy(encryptor);if(decryptor)daveDecryptorDestroy(decryptor);if(session)daveSessionDestroy(session);}
    static QByteArray byteArray(const std::vector<uint8_t> &bytes){return QByteArray(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
    static std::vector<uint8_t> vector(const QByteArray &b){return std::vector<uint8_t>(data(b),data(b)+b.size());}
    QByteArray externalSender(){return byteArray(mlspp::tls::marshal(sender));}
    QByteArray keyPackage(){uint8_t *bytes=nullptr;size_t length=0;daveSessionGetMarshalledKeyPackage(session,&bytes,&length);QByteArray out(reinterpret_cast<char*>(bytes),length);daveFree(bytes);return out;}
    QByteArray propose(const QByteArray &keyPackage){
        auto proposal=mlspp::Proposal{mlspp::Add{{mlspp::tls::get<mlspp::KeyPackage>(mlspp::bytes_ns::bytes(vector(keyPackage)))}}};
        auto message=mlspp::external_proposal(suite,group,0,proposal,0,signing);
        mlspp::tls::ostream stream;stream<<false;stream<<std::vector<mlspp::MLSMessage>{message};return byteArray(stream.bytes());
    }
    std::pair<QByteArray,QByteArray> split(const QByteArray &combined){
        mlspp::tls::istream stream(mlspp::bytes_ns::bytes(vector(combined)));mlspp::MLSMessage commit;mlspp::Welcome welcome;stream>>commit;stream>>welcome;
        return {byteArray(mlspp::tls::marshal(commit)),byteArray(mlspp::tls::marshal(welcome))};
    }
    QByteArray commitFor(const QByteArray &remoteKey){
        const auto proposal=propose(remoteKey);const char *ids[]={selfId,peerId};uint8_t *bytes=nullptr;size_t length=0;
        daveSessionProcessProposals(session,data(proposal),proposal.size(),ids,2,&bytes,&length);
        QByteArray out(reinterpret_cast<char*>(bytes),length);daveFree(bytes);return out;
    }
    bool accept(const QByteArray &message,bool commit){
        if(commit){auto result=daveSessionProcessCommit(session,data(message),message.size());const bool valid=result && !daveCommitResultIsFailed(result) && !daveCommitResultIsIgnored(result);if(result)daveCommitResultDestroy(result);return valid;}
        const char *ids[]={selfId,peerId};auto result=daveSessionProcessWelcome(session,data(message),message.size(),ids,2);if(result)daveWelcomeResultDestroy(result);return result!=nullptr;
    }
    bool prepareMedia(){
        auto local=daveSessionGetKeyRatchet(session,peerId),remote=daveSessionGetKeyRatchet(session,selfId);
        if(!local || !remote){if(local)daveKeyRatchetDestroy(local);if(remote)daveKeyRatchetDestroy(remote);return false;}
        encryptor=daveEncryptorCreate();decryptor=daveDecryptorCreate();
        daveEncryptorAssignSsrcToCodec(encryptor,peerSsrc,DAVE_CODEC_OPUS);daveEncryptorSetPassthroughMode(encryptor,false);
        daveEncryptorAssignSsrcToCodec(encryptor,302,DAVE_CODEC_H264);daveEncryptorAssignSsrcToCodec(encryptor,303,DAVE_CODEC_VP8);
        daveEncryptorSetKeyRatchet(encryptor,local);daveDecryptorTransitionToKeyRatchet(decryptor,remote);
        daveKeyRatchetDestroy(local);daveKeyRatchetDestroy(remote);return true;
    }
    QByteArray encrypt(const QByteArray &plain){
        QByteArray out(daveEncryptorGetMaxCiphertextByteSize(encryptor,DAVE_MEDIA_TYPE_AUDIO,plain.size()),Qt::Uninitialized);size_t length=0;
        if(daveEncryptorEncrypt(encryptor,DAVE_MEDIA_TYPE_AUDIO,peerSsrc,data(plain),plain.size(),data(out),out.size(),&length)!=DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS)return {};
        out.resize(length);return out;
    }
    QByteArray decrypt(const QByteArray &encrypted){
        QByteArray out(encrypted.size(),Qt::Uninitialized);size_t length=0;
        if(daveDecryptorDecrypt(decryptor,DAVE_MEDIA_TYPE_AUDIO,data(encrypted),encrypted.size(),data(out),out.size(),&length)!=DAVE_DECRYPTOR_RESULT_CODE_SUCCESS)return {};
        out.resize(length);return out;
    }
    QByteArray encryptVideo(const QByteArray &plain,bool vp8){
        QByteArray out(daveEncryptorGetMaxCiphertextByteSize(encryptor,DAVE_MEDIA_TYPE_VIDEO,plain.size()),Qt::Uninitialized);size_t length=0;
        if(daveEncryptorEncrypt(encryptor,DAVE_MEDIA_TYPE_VIDEO,vp8?303:302,data(plain),plain.size(),data(out),out.size(),&length)!=DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS)return {};
        out.resize(length);return out;
    }
};
#endif
class GatewayFixture:public QObject {
public:
    QWebSocketServer server{"Voice fixture",QWebSocketServer::NonSecureMode};
    QUdpSocket udp;
    QPointer<QWebSocket> socket;
    QByteArray transportKey=QByteArray(32,'K');
    QHostAddress clientAddress;
    quint16 clientPort=0,sequence=0;
    quint32 allocatedSsrc=selfSsrc;
    bool speakingAnnounced=false;
    int unannouncedAudio=0;
    int initialVersion=0;
    bool createCommit=true,handshakeValid=false,identified=false,protocolSelected=false,groupReady=false,autoAck=true,announcePeer=true;
    QString failure;
    QList<QJsonObject> clientJson;
    QList<QByteArray> outgoingAudio;QList<quint32> outgoingStamps;QList<qint64> outgoingTimes;QElapsedTimer wireClock;
#ifdef RELAY_TEST_MLSPP
    std::unique_ptr<MlsFixture> mls;
#endif
    bool listen(){wireClock.start();
        if(!server.listen(QHostAddress::LocalHost,0) || !udp.bind(QHostAddress::LocalHost,0))return false;
        connect(&server,&QWebSocketServer::newConnection,this,[this]{
            socket=server.nextPendingConnection();socket->setParent(this);
            connect(socket,&QWebSocket::textMessageReceived,this,[this](const QString &text){onText(QJsonDocument::fromJson(text.toUtf8()).object());});
            connect(socket,&QWebSocket::binaryMessageReceived,this,[this](const QByteArray &message){onBinary(message);});
            send(8,{{"heartbeat_interval",1000}});
        });
        connect(&udp,&QUdpSocket::readyRead,this,[this]{
            while(udp.hasPendingDatagrams()){
                const auto packet=udp.receiveDatagram();const auto bytes=packet.data();
                if(bytes.size()==74 && qFromBigEndian<quint16>(bytes.constData())==1){
                    clientAddress=packet.senderAddress();clientPort=packet.senderPort();
                    QByteArray response(74,'\0');qToBigEndian<quint16>(2,response.data());qToBigEndian<quint16>(70,response.data()+2);qToBigEndian<quint32>(allocatedSsrc,response.data()+4);
                    const QByteArray address="127.0.0.1";std::copy(address.begin(),address.end(),response.begin()+8);qToBigEndian(clientPort,response.data()+72);
                    udp.writeDatagram(response,clientAddress,clientPort);
                }else if(bytes.size()>12){const auto frame=VoicePacket::decrypt(bytes,transportKey);if(frame){
                    if(!speakingAnnounced){++unannouncedAudio;if(socket)socket->close(QWebSocketProtocol::CloseCode(4006),"Fixture: audio SSRC was not announced");}
                    else {outgoingAudio.append(frame->payload);outgoingStamps.append(frame->timestamp);outgoingTimes.append(wireClock.elapsed());}
                }}
            }
        });return true;
    }
    QUrl url()const{return QUrl(QString("ws://127.0.0.1:%1/?v=8").arg(server.serverPort()));}
    void send(int opcode,const QJsonObject &payload){if(socket)socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op",opcode},{"d",payload},{"seq",int(++sequence)}}).toJson(QJsonDocument::Compact)));}
    void binary(int opcode,const QByteArray &payload){QByteArray out(3,'\0');qToBigEndian(++sequence,out.data());out[2]=char(opcode);out+=payload;if(socket)socket->sendBinaryMessage(out);}
    void transition(int opcode,const QByteArray &payload,quint16 id=7){QByteArray out(2,'\0');qToBigEndian(id,out.data());out+=payload;binary(opcode,out);}
    void onText(const QJsonObject &root){
        clientJson.append(root);const auto payload=root.value("d").toObject();switch(root.value("op").toInt()){
        case 0:
            identified=true;speakingAnnounced=false;handshakeValid=payload.value("server_id").toString()=="1111" && payload.value("user_id").toString()==selfId && payload.value("max_dave_protocol_version").toInt()>=1;
            send(2,{{"ssrc",int(allocatedSsrc)},{"ip","127.0.0.1"},{"port",int(udp.localPort())},{"modes",QJsonArray{mode}}});break;
        case 1: {
            protocolSelected=true;
            handshakeValid=handshakeValid && payload.value("data").toObject().value("mode").toString()==mode && payload.value("data").toObject().value("port").toInt()==clientPort;
            QJsonArray key;for(int i=0;i<32;i++)key.append(int('K'));
#ifdef RELAY_TEST_MLSPP
            if(initialVersion==1){mls=std::make_unique<MlsFixture>();if(announcePeer)send(11,{{"user_ids",QJsonArray{peerId}}});binary(25,mls->externalSender());}
#endif
            send(4,{{"mode",mode},{"secret_key",key},{"dave_protocol_version",initialVersion}});break;
        }
        case 3: if(autoAck)send(6,{{"t",payload.value("t")}});break;
        case 5: if(quint32(payload.value("ssrc").toDouble())==allocatedSsrc)speakingAnnounced=true;break;
        case 23: send(22,{{"transition_id",payload.value("transition_id")}});break;
        default:break;
        }
    }
    void onBinary(const QByteArray &message){
#ifdef RELAY_TEST_MLSPP
        if(message.isEmpty() || !mls || !announcePeer)return;
        try{
            if(quint8(message[0])==26){
                if(createCommit){const auto peerPackage=mls->keyPackage();binary(27,mls->propose(peerPackage));}
                else{auto combined=mls->commitFor(message.mid(1));if(combined.isEmpty()){failure="Fixture failed to make a group commit";return;}
                    auto [commit,welcome]=mls->split(combined);if(!mls->accept(commit,true)){failure="Fixture commit rejected";return;}
                    if(!mls->prepareMedia()){failure="Fixture media ratchet missing";return;}
                    groupReady=true;transition(30,welcome,0);
                }
            }else if(quint8(message[0])==28){
                auto [commit,welcome]=mls->split(message.mid(1));if(!mls->accept(welcome,false)){failure="Fixture welcome rejected";return;}
                if(!mls->prepareMedia()){failure="Fixture media ratchet missing";return;}
                groupReady=true;transition(29,commit);
            }
        }catch(const std::exception &){failure="Fixture could not parse an MLS message";}
#else
        Q_UNUSED(message);
#endif
    }
};
}

class VoiceGatewayTest:public QObject {
    Q_OBJECT
    void prepare(VoiceEngine &voice,GatewayFixture &gateway,bool stateFirst=true){
        QVERIFY(voice.configureLocalFixture(gateway.url()));
        voice.join("1111",QString::number(channel),"Local fixture",selfId);
        const QJsonObject state{{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"session_id","fixture-session"}};
        const QJsonObject server{{"guild_id","1111"},{"token","fixture-token-never-real"},{"endpoint","fixture.discord.media"}};
        if(stateFirst){voice.onVoiceState(state);voice.onVoiceServer(server);}else{voice.onVoiceServer(server);voice.onVoiceState(state);}
    }

private slots:
    void realTimePlayout_data(){
        QTest::addColumn<int>("stallMs");QTest::addColumn<int>("bufferMs");QTest::addColumn<int>("captureBlockFrames");
        QTest::newRow("unloaded-40ms")<<0<<40<<960;
        QTest::newRow("ui-stalls-40ms")<<70<<40<<960;
        QTest::newRow("ui-stalls-60ms")<<70<<60<<960;
        QTest::newRow("ui-stalls-100ms")<<70<<100<<960;
        QTest::newRow("native-1024-20ms")<<0<<20<<1024;
        QTest::newRow("native-1024-ui-stalls-40ms")<<70<<40<<1024;
    }
    void realTimePlayout(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("Local MLS fixture dependency unavailable");
#else
        QFETCH(int,stallMs);QFETCH(int,bufferMs);QFETCH(int,captureBlockFrames);VoiceEngine voice;GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QTRY_VERIFY_WITH_TIMEOUT(voice.encryptedReady() && gateway.groupReady,5000);voice.configureAudio({false,false,false,1.0,1.0,-70});
        std::atomic<int> offered=0;
        struct Stats {int packets=0,late=0,played=0,plc=0,stale=0,codecErrors=0,concealmentBursts=0; qint64 maxWire=0,lastWire=0,firstWire=-1;quint32 firstStamp=0;int maxConsecutivePlc=0,consecutivePlc=0;};Stats stats;
        QThread receiverThread;QObject receiver;receiver.moveToThread(&receiverThread);gateway.udp.disconnect(&gateway);gateway.udp.moveToThread(&receiverThread);receiverThread.start();
        QTimer *playout=nullptr;OpusDecoder *decoder=nullptr;QElapsedTimer receiverClock;std::map<int,QByteArray> packets;
        QMetaObject::invokeMethod(&receiver,[&]{receiverClock.start();int error=0;decoder=opus_decoder_create(48000,2,&error);if(error!=OPUS_OK)++stats.codecErrors;
            connect(&gateway.udp,&QUdpSocket::readyRead,&receiver,[&]{while(gateway.udp.hasPendingDatagrams()){const auto frame=VoicePacket::decrypt(gateway.udp.receiveDatagram().data(),gateway.transportKey);if(!frame)continue;const auto now=receiverClock.elapsed();if(stats.firstWire<0){stats.firstWire=now;stats.firstStamp=frame->timestamp;}else stats.maxWire=std::max(stats.maxWire,now-stats.lastWire);stats.lastWire=now;++stats.packets;const int index=quint32(frame->timestamp-stats.firstStamp)/960;if(now>stats.firstWire+index*20+bufferMs)++stats.late;if(index<stats.played){++stats.stale;continue;}const auto plain=gateway.mls->decrypt(frame->payload);if(plain.isEmpty()){++stats.codecErrors;continue;}packets[index]=plain;}});
            playout=new QTimer(&receiver);playout->setTimerType(Qt::PreciseTimer);playout->setInterval(1);connect(playout,&QTimer::timeout,&receiver,[&]{if(stats.firstWire<0)return;int &consecutive=stats.consecutivePlc;while(receiverClock.elapsed()>=stats.firstWire+stats.played*20+bufferMs && stats.played<offered.load()){const auto it=packets.find(stats.played);QByteArray plain;if(it!=packets.end()){plain=it->second;packets.erase(it);consecutive=0;}else{if(consecutive++==0)++stats.concealmentBursts;++stats.plc;stats.maxConsecutivePlc=std::max(stats.maxConsecutivePlc,consecutive);}std::array<opus_int16,1920> decoded;const auto result=opus_decode(decoder,plain.isEmpty()?nullptr:data(plain),plain.size(),decoded.data(),960,0);if(result!=960)++stats.codecErrors;++stats.played;}});playout->start();
        },Qt::BlockingQueuedConnection);
        QThread producerThread;QObject producer;producer.moveToThread(&producerThread);QTimer *captureTimer=nullptr;QElapsedTimer captureClock;int producedSamples=0;
        connect(&producerThread,&QThread::started,&producer,[&]{
            captureClock.start();captureTimer=new QTimer(&producer);captureTimer->setTimerType(Qt::PreciseTimer);captureTimer->setInterval(captureBlockFrames==960?20:1);
            connect(captureTimer,&QTimer::timeout,&producer,[&]{
                auto supply=[&]{
                    QByteArray generated(captureBlockFrames*4,Qt::Uninitialized);
                    for(int i=0;i<captureBlockFrames;++i){const qint16 value=qint16(std::sin((producedSamples+i)*2.0*3.141592653589793*440.0/48000)*6000);std::memcpy(generated.data()+i*4,&value,2);std::memcpy(generated.data()+i*4+2,&value,2);}
                    producedSamples+=captureBlockFrames;offered=producedSamples/960;voice.queueFixtureAudio(generated);
                };
                if(captureBlockFrames==960)supply();
                // Native capture blocks do not align with Opus's 20 ms frame.
                // An independent sample clock exposes packet bursts that an
                // exact 960-frame timer cannot exercise. Receiver buffer sizes
                // are controlled fixture deadlines, not assumptions about a
                // live Discord receiver's adaptive jitter buffer.
                else while(captureClock.nsecsElapsed()>=qint64(producedSamples+captureBlockFrames)*1000000000/48000)supply();
            });captureTimer->start();
        });producerThread.start();
        QEventLoop loop;QTimer finish,load;finish.setSingleShot(true);finish.setInterval(6000);load.setInterval(125);load.setTimerType(Qt::PreciseTimer);connect(&finish,&QTimer::timeout,&loop,&QEventLoop::quit);connect(&load,&QTimer::timeout,&loop,[&]{if(stallMs)QThread::msleep(stallMs);});finish.start();if(stallMs)load.start();loop.exec();load.stop();
        QMetaObject::invokeMethod(&producer,[&]{captureTimer->stop();delete captureTimer;},Qt::BlockingQueuedConnection);producerThread.quit();producerThread.wait();
        QEventLoop drain;QTimer::singleShot(300,&drain,&QEventLoop::quit);drain.exec();
        auto *mainThread=QThread::currentThread();QMetaObject::invokeMethod(&receiver,[&]{playout->stop();delete playout;gateway.udp.disconnect(&receiver);gateway.udp.moveToThread(mainThread);opus_decoder_destroy(decoder);},Qt::BlockingQueuedConnection);receiverThread.quit();receiverThread.wait();
        const auto counts=voice.fixtureCounters();qInfo("realtime playout: captureBlock=%d stall=%d buffer=%d offered=%d captured=%llu dropped=%llu wire=%d maxGap=%lld played=%d plc=%d late=%d stale=%d concealmentBursts=%d longestPlc=%d codecErrors=%d",captureBlockFrames,stallMs,bufferMs,offered.load(),counts.value("captureFrames").toULongLong(),counts.value("captureDropped").toULongLong(),stats.packets,qlonglong(stats.maxWire),stats.played,stats.plc,stats.late,stats.stale,stats.concealmentBursts,stats.maxConsecutivePlc,stats.codecErrors);QCOMPARE(stats.codecErrors,0);QVERIFY(stats.played>250);QCOMPARE(stats.plc,0);QCOMPARE(stats.late,0);QCOMPARE(stats.stale,0);QVERIFY(stats.maxWire<40);voice.leave();
#endif
    }
};
QTEST_GUILESS_MAIN(VoiceGatewayTest)
#include "voice_playout_test.moc"
