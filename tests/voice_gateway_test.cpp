#include <QtTest>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QScopeGuard>
#include <atomic>
#include <algorithm>
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
template<class Predicate> bool waitNative(Predicate ready,int timeout=1000){
    if(ready())return true;QEventLoop loop;QTimer poll,deadline;poll.setTimerType(Qt::PreciseTimer);poll.setInterval(1);deadline.setSingleShot(true);
    QObject::connect(&poll,&QTimer::timeout,&loop,[&]{if(ready())loop.quit();});QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    poll.start();deadline.start(timeout);loop.exec();return ready();
}
void waitNativeMs(int ms){QEventLoop loop;QTimer::singleShot(ms,&loop,&QEventLoop::quit);loop.exec();}
constexpr auto selfId="3333",peerId="4444";
constexpr quint64 channel=2222;
constexpr quint32 selfSsrc=101,peerSsrc=202;
constexpr auto mode="aead_xchacha20_poly1305_rtpsize";
const unsigned char *data(const QByteArray &b){return reinterpret_cast<const unsigned char*>(b.constData());}
unsigned char *data(QByteArray &b){return reinterpret_cast<unsigned char*>(b.data());}
QByteArray tone(int frameCount,int startSample=0){
    QByteArray pcm(frameCount*3840,Qt::Uninitialized);
    for(int i=0;i<frameCount*960;i++){
        const qint16 sample=qint16(std::sin((i+startSample)*2.0*3.141592653589793*440.0/48000)*6000);
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
        daveEncryptorAssignSsrcToCodec(encryptor,302,DAVE_CODEC_H264);daveEncryptorAssignSsrcToCodec(encryptor,303,DAVE_CODEC_VP8);daveEncryptorAssignSsrcToCodec(encryptor,305,DAVE_CODEC_H265);daveEncryptorAssignSsrcToCodec(encryptor,306,DAVE_CODEC_AV1);
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
    QByteArray encryptVideo(const QByteArray &plain,bool vp8,quint32 source=0){
        QByteArray out(daveEncryptorGetMaxCiphertextByteSize(encryptor,DAVE_MEDIA_TYPE_VIDEO,plain.size()),Qt::Uninitialized);size_t length=0;
        if(daveEncryptorEncrypt(encryptor,DAVE_MEDIA_TYPE_VIDEO,source?source:(vp8?303:302),data(plain),plain.size(),data(out),out.size(),&length)!=DAVE_ENCRYPTOR_RESULT_CODE_SUCCESS)return {};
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
    int rejectResumeCode=0;
    bool createCommit=true,handshakeValid=false,identified=false,protocolSelected=false,groupReady=false,autoAck=true,announcePeer=true;
    QString expectedServerId="1111",expectedChannelId;
    QString failure;
    QList<QJsonObject> clientJson;
    QList<QJsonObject> resumePayloads;
    QList<QByteArray> outgoingAudio;QList<quint32> outgoingStamps;QList<qint64> outgoingTimes;QElapsedTimer wireClock;
#ifdef RELAY_TEST_MLSPP
    std::unique_ptr<MlsFixture> mls;
#endif
    bool listen(){
        wireClock.start();
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
            identified=true;speakingAnnounced=false;handshakeValid=payload.value("server_id").toString()==expectedServerId &&
                (expectedChannelId.isEmpty() || payload.value("channel_id").toString()==expectedChannelId) &&
                payload.value("user_id").toString()==selfId && payload.value("max_dave_protocol_version").toInt()>=1;
            send(2,{{"ssrc",int(allocatedSsrc)},{"ip","127.0.0.1"},{"port",int(udp.localPort())},{"modes",QJsonArray{mode}}});break;
        case 1: {
            protocolSelected=true;
            handshakeValid=handshakeValid && payload.value("data").toObject().value("mode").toString()==mode && payload.value("data").toObject().value("port").toInt()==clientPort;
            QJsonArray key;for(const auto value:transportKey)key.append(int(quint8(value)));
#ifdef RELAY_TEST_MLSPP
            if(initialVersion==1){mls=std::make_unique<MlsFixture>();if(announcePeer)send(11,{{"user_ids",QJsonArray{peerId}}});binary(25,mls->externalSender());}
#endif
            send(4,{{"mode",mode},{"secret_key",key},{"dave_protocol_version",initialVersion}});break;
        }
        case 3: if(autoAck)send(6,{{"t",payload.value("t")}});break;
        case 5: if(quint32(payload.value("ssrc").toDouble())==allocatedSsrc)speakingAnnounced=true;break;
        case 7:
            resumePayloads.append(payload);
            if(rejectResumeCode){if(socket)socket->close(QWebSocketProtocol::CloseCode(rejectResumeCode),"Synthetic resume rejection");}
            else if(socket){
                autoAck=true;
                socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op",9},{"d",QJsonValue::Null}}).toJson(QJsonDocument::Compact)));
            }
            break;
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
    void privateCallAllocationSupportsEncryptedMediaAndResume_data(){
        QTest::addColumn<bool>("serverFirst");
        QTest::newRow("private-state-first")<<false;QTest::newRow("private-server-first")<<true;
    }
    void privateCallAllocationSupportsEncryptedMediaAndResume(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required for encrypted private-call media");
#else
        QFETCH(bool,serverFirst);VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");
        GatewayFixture gateway;gateway.initialVersion=1;
        const auto privateId=QString::number(channel);gateway.expectedServerId=privateId;gateway.expectedChannelId=privateId;
        QVERIFY(gateway.listen());QVERIFY(voice.configureLocalFixture(gateway.url()));
        QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        // DiscordClient maps a private call's null wire guild to its DM channel
        // ID before delivering either gateway event to the native voice engine.
        voice.join(privateId,privateId,"Generated private call",selfId);
        QTRY_COMPARE(requests.size(),1);QCOMPARE(requests.first().at(0).toString(),privateId);QCOMPARE(requests.first().at(1).toString(),privateId);
        const QJsonObject own{{"guild_id",privateId},{"channel_id",privateId},{"user_id",selfId},{"session_id","generated-private-session"}};
        const QJsonObject server{{"guild_id",privateId},{"channel_id",privateId},{"token","generated-private-token"},{"endpoint","generated.discord.media"}};
        if(serverFirst){voice.onVoiceServer(server);voice.onVoiceState(own);}else{voice.onVoiceState(own);voice.onVoiceServer(server);}
        QVERIFY(waitNative([&]{return voice.encryptedReady() || !gateway.failure.isEmpty();},5000));
        QVERIFY2(gateway.failure.isEmpty(),qPrintable(gateway.failure));QVERIFY2(voice.encryptedReady(),qPrintable(voice.diagnosticSummary()));
        QVERIFY(gateway.groupReady);QVERIFY(gateway.handshakeValid);QCOMPARE(voice.currentGuildId(),privateId);QCOMPARE(voice.channelId(),privateId);
        QJsonObject identify;for(const auto &packet:gateway.clientJson)if(packet.value("op").toInt()==0)identify=packet.value("d").toObject();
        QCOMPARE(identify.value("server_id").toString(),privateId);QCOMPARE(identify.value("channel_id").toString(),privateId);
        QCOMPARE(identify.value("user_id").toString(),QString(selfId));QCOMPARE(identify.value("session_id").toString(),QString("generated-private-session"));
        voice.queueFixtureAudio(tone(3));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()>=3;},1500));
        const auto outgoing=gateway.mls->decrypt(gateway.outgoingAudio.first());QVERIFY(!outgoing.isEmpty());
        int opusError=0;auto *decoder=opus_decoder_create(48000,2,&opusError);QCOMPARE(opusError,OPUS_OK);
        std::array<opus_int16,1920> decoded{};const int decodedSamples=opus_decode(decoder,data(outgoing),outgoing.size(),decoded.data(),960,0);opus_decoder_destroy(decoder);
        QCOMPARE(decodedSamples,960);QVERIFY(std::any_of(decoded.begin(),decoded.end(),[](auto sample){return sample!=0;}));
        gateway.send(12,{{"user_id",peerId},{"audio_ssrc",int(peerSsrc)}});gateway.send(5,{{"user_id",peerId},{"ssrc",int(peerSsrc)},{"speaking",1}});waitNativeMs(20);
        auto *encoder=opus_encoder_create(48000,2,OPUS_APPLICATION_VOIP,&opusError);QCOMPARE(opusError,OPUS_OK);
        const auto incoming=tone(8);
        for(int i=0;i<8;++i){
            std::array<opus_int16,1920> samples{};std::memcpy(samples.data(),incoming.constData()+i*3840,3840);
            QByteArray opus(1275,Qt::Uninitialized);const int length=opus_encode(encoder,samples.data(),960,data(opus),opus.size());QVERIFY(length>0);opus.resize(length);
            const auto encrypted=gateway.mls->encrypt(opus);QVERIFY(!encrypted.isEmpty());
            gateway.udp.writeDatagram(VoicePacket::encrypt(encrypted,gateway.transportKey,quint16(100+i),quint32(i*960),peerSsrc,quint32(i+1)),gateway.clientAddress,gateway.clientPort);
        }
        opus_encoder_destroy(encoder);QVERIFY(waitNative([&]{return voice.fixtureCounters().value("decoded").toULongLong()>=3;},1500));
        const auto output=voice.takeFixtureAudio();QVERIFY(output.size()>=3840);QVERIFY(std::any_of(output.begin(),output.end(),[](char sample){return sample!=0;}));
        const auto connectedStage=voice.stage();gateway.autoAck=false;voice.requestFixtureHeartbeat();voice.requestFixtureHeartbeat();
        QVERIFY(waitNative([&]{return gateway.resumePayloads.size()==1 && voice.stage()==connectedStage;},5000));
        const auto resume=gateway.resumePayloads.first();QCOMPARE(resume.value("server_id").toString(),privateId);QCOMPARE(resume.value("channel_id").toString(),privateId);
        QCOMPARE(resume.value("session_id").toString(),QString("generated-private-session"));QCOMPARE(resume.value("token").toString(),QString("generated-private-token"));
        QCOMPARE(requests.size(),1);QVERIFY(voice.encryptedReady());QVERIFY(voice.connected());
        const auto sent=gateway.outgoingAudio.size();voice.queueFixtureAudio(tone(2,2880));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()>=sent+2;},1500));
        QVERIFY(!gateway.mls->decrypt(gateway.outgoingAudio.last()).isEmpty());QCOMPARE(voice.fixtureCounters().value("failures").toULongLong(),quint64(0));
        voice.leave();QTRY_COMPARE(requests.size(),2);QCOMPARE(requests.last().at(0).toString(),privateId);QVERIFY(requests.last().at(1).toString().isEmpty());
#endif
    }
    void duplicateAllocationAndHealthySameChannelJoinAreIdempotent(){
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.connected();},3000));QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        const auto stage=voice.stage();const auto before=gateway.clientJson.size();
        voice.onVoiceServer({{"guild_id","1111"},{"token","fixture-token-never-real"},{"endpoint","fixture.discord.media"}});
        voice.join("1111",QString::number(channel),"Local fixture",selfId);waitNativeMs(80);
        QVERIFY(voice.connected());QCOMPARE(voice.stage(),stage);QCOMPARE(requests.size(),0);
        int identifies=0;for(const auto &packet:gateway.clientJson)if(packet.value("op").toInt()==0)++identifies;
        QCOMPARE(identifies,1);QVERIFY(gateway.clientJson.size()>=before);voice.leave();
    }
    void changedAllocationReconnectsWithoutGatewayLeave_data(){
        QTest::addColumn<bool>("nullFirst");QTest::addColumn<bool>("sameEndpoint");
        QTest::newRow("direct-replacement")<<false<<false;
        QTest::newRow("null-then-replacement")<<true<<false;
        QTest::newRow("new-token-same-endpoint")<<false<<true;
    }
    void changedAllocationReconnectsWithoutGatewayLeave(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        QFETCH(bool,nullFirst);QFETCH(bool,sameEndpoint);VoiceEngine voice;GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.encryptedReady();},5000));QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        if(nullFirst){
            voice.onVoiceServer({{"guild_id","1111"},{"token","fixture-reallocating-token"},{"endpoint",QJsonValue::Null}});
            QVERIFY(!voice.connected());QVERIFY(!voice.encryptedReady());QCOMPARE(voice.stage(),QString("awaiting_voice_server"));
            QCOMPARE(voice.channelId(),QString::number(channel));waitNativeMs(50);QCOMPARE(requests.size(),0);
        }
        gateway.transportKey=QByteArray(32,'L');
        voice.onVoiceServer({{"guild_id","1111"},{"token","fixture-new-token"},{"endpoint",sameEndpoint?"fixture.discord.media":"replacement.discord.media"}});
        QVERIFY(waitNative([&]{return voice.encryptedReady() || !gateway.failure.isEmpty();},5000));QVERIFY2(voice.encryptedReady(),qPrintable(gateway.failure));
        QCOMPARE(voice.channelId(),QString::number(channel));QCOMPARE(requests.size(),0);
        int identifies=0;for(const auto &packet:gateway.clientJson)if(packet.value("op").toInt()==0)++identifies;QCOMPARE(identifies,2);
        voice.queueFixtureAudio(tone(2));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==2;}));
        for(const auto &cipher:gateway.outgoingAudio)QVERIFY(!gateway.mls->decrypt(cipher).isEmpty());voice.leave();
#endif
    }
    void delayedLeaveAcknowledgementSequencesFreshJoin_data(){QTest::addColumn<bool>("serverFirst");QTest::newRow("state-first")<<false;QTest::newRow("server-first")<<true;}
    void delayedLeaveAcknowledgementSequencesFreshJoin(){
        QFETCH(bool,serverFirst);VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.connected();},3000));QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        voice.leave();voice.join("1111",QString::number(channel),"Local fixture",selfId);
        QTRY_COMPARE(requests.size(),1);QVERIFY(requests.first()[1].toString().isEmpty());QCOMPARE(voice.stage(),QString("awaiting_leave"));
        const QJsonObject oldState{{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"session_id","fixture-session"}};
        const QJsonObject oldServer{{"guild_id","1111"},{"token","fixture-token-never-real"},{"endpoint","fixture.discord.media"}};
        voice.onVoiceState(oldState);voice.onVoiceServer(oldServer);waitNativeMs(40);QVERIFY(!voice.connected());QCOMPARE(requests.size(),1);
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QJsonValue::Null},{"user_id",selfId},{"session_id","fixture-session"}});
        QTRY_COMPARE(requests.size(),2);QCOMPARE(requests.last()[1].toString(),QString::number(channel));
        const QJsonObject freshState{{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"session_id","new-fixture-session"}};
        const QJsonObject freshServer{{"guild_id","1111"},{"token","new-fixture-token"},{"endpoint","fixture.discord.media"}};
        if(serverFirst){voice.onVoiceServer(freshServer);voice.onVoiceState(freshState);}else{voice.onVoiceState(freshState);voice.onVoiceServer(freshServer);}
        QVERIFY(waitNative([&]{return voice.connected();},3000));QCOMPARE(requests.size(),2);voice.leave();
    }
    void recoverableSocketCloseResumesEncryptedMedia_data(){QTest::addColumn<int>("closeCode");QTest::newRow("abnormal-tcp-close")<<-1;QTest::newRow("voice-server-crashed")<<4015;}
    void recoverableSocketCloseResumesEncryptedMedia(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        QFETCH(int,closeCode);VoiceEngine voice;GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.encryptedReady() && voice.latencyMs()>=0;},5000));voice.configureAudio({false,false,false,1.0,1.0,-70});
        voice.queueFixtureAudio(tone(2));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==2;}));
        const auto port=gateway.clientPort;const auto stage=voice.stage();QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        if(closeCode<0)gateway.socket->abort();else gateway.socket->close(QWebSocketProtocol::CloseCode(closeCode),"Synthetic recoverable close");
        QVERIFY(waitNative([&]{return voice.stage()=="reconnecting_websocket";},1500));QVERIFY(voice.connected());QVERIFY(voice.encryptedReady());
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"session_id","fixture-session"}});
        waitNativeMs(40);QCOMPARE(gateway.resumePayloads.size(),0);QCOMPARE(voice.stage(),QString("reconnecting_websocket"));
        voice.queueFixtureAudio(tone(2,1920));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==4;}));
        QVERIFY(waitNative([&]{return gateway.resumePayloads.size()==1 && voice.stage()==stage;},5000));
        QCOMPARE(gateway.resumePayloads.first().value("server_id").toString(),QString("1111"));
        QCOMPARE(gateway.resumePayloads.first().value("session_id").toString(),QString("fixture-session"));
        QCOMPARE(gateway.resumePayloads.first().value("token").toString(),QString("fixture-token-never-real"));
        QVERIFY(gateway.resumePayloads.first().contains("seq_ack"));QVERIFY(gateway.resumePayloads.first().value("seq_ack").toInt(-1)>=0);
        QCOMPARE(gateway.clientPort,port);QCOMPARE(requests.size(),0);
        int identifies=0,selects=0;for(const auto &packet:gateway.clientJson){identifies+=packet.value("op").toInt()==0;selects+=packet.value("op").toInt()==1;}QCOMPARE(identifies,1);QCOMPARE(selects,1);
        voice.queueFixtureAudio(tone(2,3840));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==6;}));
        for(const auto &cipher:gateway.outgoingAudio)QVERIFY(!gateway.mls->decrypt(cipher).isEmpty());
        QCOMPARE(voice.fixtureCounters().value("sent").toInt(),6);QCOMPARE(voice.fixtureCounters().value("failures").toInt(),0);voice.leave();
#endif
    }
    void rejectedResumeObtainsOneFreshAllocation_data(){
        QTest::addColumn<int>("code");QTest::addColumn<int>("outages");QTest::addColumn<int>("expectedResumes");
        QTest::newRow("invalid-session")<<4006<<1<<1;QTest::newRow("expired-session")<<4009<<1<<1;
        QTest::newRow("fresh-budget-for-independent-outage")<<4006<<2<<1;
        QTest::newRow("bounded-resume-exhaustion")<<4000<<1<<3;
    }
    void rejectedResumeObtainsOneFreshAllocation(){
        QFETCH(int,code);QFETCH(int,outages);QFETCH(int,expectedResumes);VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.connected();},3000));QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        for(int outage=0;outage<outages;++outage){
            const auto resumes=gateway.resumePayloads.size();gateway.rejectResumeCode=code;gateway.socket->abort();
            QVERIFY(waitNative([&]{return gateway.resumePayloads.size()==resumes+expectedResumes && voice.stage()=="awaiting_leave";},5000));
            QTRY_COMPARE(requests.size(),outage*2+1);QVERIFY(requests.last()[1].toString().isEmpty());gateway.rejectResumeCode=0;
            voice.onVoiceState({{"guild_id","1111"},{"channel_id",QJsonValue::Null},{"user_id",selfId}});QTRY_COMPARE(requests.size(),outage*2+2);
            voice.onVoiceState({{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"session_id",QString("fresh-fixture-session-%1").arg(outage)}});
            voice.onVoiceServer({{"guild_id","1111"},{"token",QString("fresh-fixture-token-%1").arg(outage)},{"endpoint","fixture.discord.media"}});
            QVERIFY(waitNative([&]{return voice.connected();},3000));QCOMPARE(requests.size(),outage*2+2);
        }
        voice.leave();
    }
    void terminalCloseCodesDoNotRetryOrRequestLeave_data(){QTest::addColumn<int>("code");for(const int code:{4014,4021,4022})QTest::newRow(qPrintable(QString::number(code)))<<code;}
    void terminalCloseCodesDoNotRetryOrRequestLeave(){
        QFETCH(int,code);VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.connected();},3000));QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        gateway.socket->close(QWebSocketProtocol::CloseCode(code),"Synthetic terminal close");QVERIFY(waitNative([&]{return voice.stage()=="failed";}));
        QVERIFY(!voice.connected());waitNativeMs(650);QCOMPARE(gateway.resumePayloads.size(),0);QCOMPARE(requests.size(),0);
        const auto failures=voice.diagnosticHistory().value("failures").toArray();QVERIFY(!failures.isEmpty());QCOMPARE(failures.last().toObject().value("closeCode").toInt(),code);
        voice.join("1111",QString::number(channel),"Local fixture",selfId);waitNativeMs(40);QCOMPARE(voice.stage(),QString("awaiting_gateway_state"));
        QCOMPARE(voice.diagnosticHistory().value("failures").toArray().last().toObject().value("closeCode").toInt(),code);voice.leave();
    }
    void rapidCrossGuildRoundTripTracksBothPendingLeaves_data(){QTest::addColumn<bool>("ackAFirst");QTest::newRow("A-ack-first")<<true;QTest::newRow("B-ack-first")<<false;}
    void rapidCrossGuildRoundTripTracksBothPendingLeaves(){
        QFETCH(bool,ackAFirst);VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.connected();},3000));QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        voice.join("6666","7777","Other fixture guild",selfId);voice.join("1111",QString::number(channel),"Local fixture",selfId);
        QTRY_COMPARE(requests.size(),3);QCOMPARE(voice.stage(),QString("awaiting_leave"));
        QCOMPARE(requests[0][0].toString(),QString("1111"));QVERIFY(requests[0][1].toString().isEmpty());
        QCOMPARE(requests[1][0].toString(),QString("6666"));QCOMPARE(requests[1][1].toString(),QString("7777"));
        QCOMPARE(requests[2][0].toString(),QString("6666"));QVERIFY(requests[2][1].toString().isEmpty());
        const QJsonObject ackA{{"guild_id","1111"},{"channel_id",QJsonValue::Null},{"user_id",selfId}};
        const QJsonObject ackB{{"guild_id","6666"},{"channel_id",QJsonValue::Null},{"user_id",selfId}};
        if(!ackAFirst){voice.onVoiceState(ackB);waitNativeMs(30);QCOMPARE(requests.size(),3);QCOMPARE(voice.stage(),QString("awaiting_leave"));}
        voice.onVoiceState(ackA);QTRY_COMPARE(requests.size(),4);QCOMPARE(requests.last()[0].toString(),QString("1111"));QCOMPARE(requests.last()[1].toString(),QString::number(channel));
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"session_id","roundtrip-fixture-session"}});
        voice.onVoiceServer({{"guild_id","1111"},{"token","roundtrip-fixture-token"},{"endpoint","fixture.discord.media"}});
        QVERIFY(waitNative([&]{return voice.connected();},3000));if(ackAFirst)voice.onVoiceState(ackB);waitNativeMs(30);
        QVERIFY(voice.connected());QCOMPARE(voice.channelId(),QString::number(channel));QCOMPARE(requests.size(),4);voice.leave();
    }
    void deafenRestoresIndependentMuteState(){
        VoiceEngine voice;
        QVERIFY(!voice.muted());
        voice.setDeafened(true);QVERIFY(voice.deafened());QVERIFY(voice.muted());
        voice.setDeafened(true); // Repeated requests must not change the saved mute preference.
        voice.setDeafened(false);QVERIFY(!voice.deafened());QVERIFY(!voice.muted());
        voice.setMuted(true);voice.setDeafened(true);voice.setDeafened(false);
        QVERIFY(voice.muted());QVERIFY(!voice.deafened());
        voice.setMuted(false);voice.setDeafened(true);voice.setMuted(true);voice.setDeafened(false);
        QVERIFY(voice.muted()); // A separate mute while deafened also persists.
        voice.setDeafened(true);voice.setMuted(false);
        QVERIFY(!voice.muted());QVERIFY(!voice.deafened());
    }
    void localControlsDoNotRejoinBeforeLeaveAcknowledgement(){
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.connected();},3000));QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        voice.leave();voice.join("1111",QString::number(channel),"Local fixture",selfId);QTRY_COMPARE(requests.size(),1);QCOMPARE(voice.stage(),QString("awaiting_leave"));
        voice.setMuted(true);voice.setDeafened(true);voice.startMicTest();QVERIFY(voice.micTesting());voice.stopMicTest();voice.setMuted(false);voice.setDeafened(false);
        waitNativeMs(40);QCOMPARE(requests.size(),1);QCOMPARE(voice.stage(),QString("awaiting_leave"));
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QJsonValue::Null},{"user_id",selfId}});QTRY_COMPARE(requests.size(),2);
        QCOMPARE(requests.last()[1].toString(),QString::number(channel));QVERIFY(!requests.last()[2].toBool());QVERIFY(!requests.last()[3].toBool());voice.leave();
    }
    void deferredOldSocketErrorDoesNotRefreshNewChannelAttempt(){
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.connected();},3000));QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);bool emitted=false;
        QVERIFY(QMetaObject::invokeMethod(voice.m_runtimeContext,[&]{
            auto *socket=voice.m_runtimeContext->findChild<QWebSocket*>();
            if(socket)emitted=QMetaObject::invokeMethod(socket,"errorOccurred",Qt::DirectConnection,Q_ARG(QAbstractSocket::SocketError,QAbstractSocket::RemoteHostClosedError));
            voice.join("1111","5555","New fixture channel",selfId);
        },Qt::BlockingQueuedConnection));
        QVERIFY(emitted);waitNativeMs(40);QCOMPARE(voice.stage(),QString("awaiting_gateway_state"));QCOMPARE(requests.size(),1);QCOMPARE(requests.first()[1].toString(),QString("5555"));
        voice.onVoiceState({{"guild_id","1111"},{"channel_id","5555"},{"user_id",selfId},{"session_id","fresh-channel-session"}});
        voice.onVoiceServer({{"guild_id","1111"},{"token","fresh-channel-token"},{"endpoint","fixture.discord.media"}});
        QVERIFY(waitNative([&]{return voice.connected();},3000));QCOMPARE(requests.size(),1);voice.leave();
    }
    void accountChangeDropsQueuedLeaveAndStartsCleanIdentity(){
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.connected();},3000));QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        QVERIFY(QMetaObject::invokeMethod(voice.m_runtimeContext,[&]{voice.leave();voice.onAccountState(QString());},Qt::BlockingQueuedConnection));
        waitNativeMs(40);QCOMPARE(requests.size(),0);QVERIFY(!voice.connected());QCOMPARE(voice.stage(),QString("idle"));
        voice.onAccountState(selfId);voice.join("1111",QString::number(channel),"Local fixture",selfId);QTRY_COMPARE(requests.size(),1);
        QCOMPARE(requests.last()[1].toString(),QString::number(channel));QCOMPARE(voice.stage(),QString("awaiting_gateway_state"));
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"session_id","account-fixture-session"}});
        voice.onVoiceServer({{"guild_id","1111"},{"token","account-fixture-token"},{"endpoint","fixture.discord.media"}});
        QVERIFY(waitNative([&]{return voice.connected();},3000));
        // Even an identical target cannot reuse another account's transport.
        voice.join("1111",QString::number(channel),"Local fixture","8888");QVERIFY(!voice.connected());QVERIFY(voice.matchesAccount("8888"));
        QTRY_COMPARE(requests.size(),2);QCOMPARE(voice.stage(),QString("awaiting_gateway_state"));voice.onAccountState(QString());
    }
    void missingServerAllocationRetriesOnceAndRetainsFailureEvidence(){
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());QVERIFY(voice.configureLocalFixture(gateway.url()));
        QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);voice.join("1111",QString::number(channel),"Local fixture",selfId);
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"session_id","fixture-session"}});
        QTRY_COMPARE(requests.size(),1);voice.expireFixtureDeadline();QTRY_COMPARE(requests.size(),2);QCOMPARE(voice.stage(),QString("awaiting_leave"));
        const auto failures=voice.diagnosticHistory().value("failures").toArray();QVERIFY(!failures.isEmpty());
        QCOMPARE(failures.last().toObject().value("reason").toString(),QString("awaiting_state_timeout"));
        QCOMPARE(failures.last().toObject().value("gateway").toInt(),0);QVERIFY(failures.last().toObject().value("selfState").toBool());QVERIFY(!failures.last().toObject().value("serverUpdate").toBool());
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QJsonValue::Null},{"user_id",selfId}});QTRY_COMPARE(requests.size(),3);
        // Withhold both fresh allocation events as well: the retry is bounded.
        voice.expireFixtureDeadline();QVERIFY(waitNative([&]{return voice.stage()=="failed";}));
        const auto history=QJsonDocument(voice.diagnosticHistory()).toJson();QVERIFY(history.contains("awaiting_state_timeout"));QVERIFY(!history.contains("fixture-session"));QVERIFY(!history.contains("fixture-token"));
        voice.join("1111",QString::number(channel),"Local fixture",selfId);QVERIFY(QJsonDocument(voice.diagnosticHistory()).toJson().contains("awaiting_state_timeout"));voice.leave();
    }
    void leaveAcknowledgementTimeoutDoesNotLoop(){
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());QVERIFY(voice.configureLocalFixture(gateway.url()));
        QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);voice.join("1111",QString::number(channel),"Local fixture",selfId);QTRY_COMPARE(requests.size(),1);
        voice.expireFixtureDeadline();QTRY_COMPARE(requests.size(),2);QCOMPARE(voice.stage(),QString("awaiting_leave"));
        voice.expireFixtureDeadline();QVERIFY(waitNative([&]{return voice.stage()=="failed";}));
        const auto failures=voice.diagnosticHistory().value("failures").toArray();QVERIFY(!failures.isEmpty());QCOMPARE(failures.last().toObject().value("reason").toString(),QString("leave_ack_timeout"));
        waitNativeMs(80);QCOMPARE(requests.size(),2);QVERIFY(gateway.clientJson.isEmpty());
        voice.join("1111",QString::number(channel),"Local fixture",selfId);QTRY_COMPARE(requests.size(),3);
        QCOMPARE(requests.last()[1].toString(),QString::number(channel));QCOMPARE(voice.stage(),QString("awaiting_gateway_state"));
        // A late acknowledgement from the expired leave must not cancel this
        // explicit retry. A later real removal from an established call must.
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QJsonValue::Null},{"user_id",selfId}});
        QCOMPARE(voice.stage(),QString("awaiting_gateway_state"));
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"session_id","retry-fixture-session"}});
        voice.onVoiceServer({{"guild_id","1111"},{"token","retry-fixture-token"},{"endpoint","fixture.discord.media"}});
        QVERIFY(waitNative([&]{return voice.connected();},3000));
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QJsonValue::Null},{"user_id",selfId}});
        QCOMPARE(voice.stage(),QString("failed"));QVERIFY(!voice.connected());QCOMPARE(requests.size(),3);voice.leave();
    }

    void localSpeakingSurvivesNativeCaptureCadenceAndReleases(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        VoiceEngine voice;GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.encryptedReady();},5000));voice.configureAudio({false,false,false,1.0,1.0,-70});
        QSignalSpy speaking(&voice,&VoiceEngine::speakingChanged);
        auto changes=[&](bool active){int count=0;for(const auto &event:speaking)if(event[0].toString()==selfId && event[1].toBool()==active)++count;return count;};
        // Native devices commonly supply 1024 samples every 21.33 ms. The
        // independent 20 ms voice timer will sometimes have only a partial
        // frame; that is capture cadence, not a transition to silent speech.
        QThread producerThread;QObject producer;producer.moveToThread(&producerThread);QTimer *captureTimer=nullptr;
        QElapsedTimer captureClock;std::atomic<int> offeredBlocks=0;std::atomic<bool> silence=false;
        connect(&producerThread,&QThread::started,&producer,[&]{
            captureClock.start();captureTimer=new QTimer(&producer);captureTimer->setTimerType(Qt::PreciseTimer);captureTimer->setInterval(1);
            connect(captureTimer,&QTimer::timeout,&producer,[&]{
                while(captureClock.nsecsElapsed()>=qint64(offeredBlocks.load()+1)*1024*1000000000/48000){
                    const int block=offeredBlocks++;const auto pcm=silence.load()?QByteArray(1024*4,'\0'):tone(2,block*1024).left(1024*4);
                    voice.queueFixtureAudio(pcm);
                }
            });captureTimer->start();
        });
        auto stopProducer=[&]{if(!producerThread.isRunning())return;QMetaObject::invokeMethod(&producer,[&]{delete captureTimer;captureTimer=nullptr;},Qt::BlockingQueuedConnection);producerThread.quit();producerThread.wait();};
        const auto cleanup=qScopeGuard(stopProducer);producerThread.start();
        QVERIFY(waitNative([&]{return changes(true)>0;}));QVERIFY(waitNative([&]{return offeredBlocks.load()>=96;},2500));
        qInfo("Native 1024-frame capture: blocks=%d, speaking-on=%d, speaking-off-during-tone=%d",offeredBlocks.load(),changes(true),changes(false));
        QVERIFY(offeredBlocks>=96);QCOMPARE(changes(true),1);QCOMPARE(changes(false),0);
        QVERIFY(gateway.outgoingAudio.size()>=90);QCOMPARE(voice.fixtureCounters().value("captureDropped").toInt(),0);
        silence=true;QElapsedTimer release;release.start();QVERIFY(waitNative([&]{return changes(false)==1;},350));
        QVERIFY(release.elapsed()<=350);waitNativeMs(160);QCOMPARE(changes(true),1);QCOMPARE(changes(false),1);
        // Genuine silence uses the existing processed-audio release. A device
        // that stops supplying samples must also lose its indicator promptly.
        silence=false;QVERIFY(waitNative([&]{return changes(true)==2;}));stopProducer();
        QVERIFY(waitNative([&]{return changes(false)==2;},200));waitNativeMs(80);QCOMPARE(changes(true),2);
        QCOMPARE(voice.fixtureCounters().value("failures").toInt(),0);voice.leave();
#endif
    }
    void localSpeakingClearsWhenTransmissionIsBlocked_data(){
        QTest::addColumn<QString>("gate");
        for(const auto *gate:{"mute","deafen","server-mute","server-deafen","microphone-test","leave"})QTest::newRow(gate)<<QString::fromLatin1(gate);
    }
    void localSpeakingClearsWhenTransmissionIsBlocked(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        QFETCH(QString,gate);VoiceEngine voice;GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.encryptedReady();},5000));voice.configureAudio({false,false,false,1.0,1.0,-70});
        QSignalSpy speaking(&voice,&VoiceEngine::speakingChanged);
        auto localActive=[&]{for(auto it=speaking.crbegin();it!=speaking.crend();++it)if((*it)[0].toString()==selfId)return (*it)[1].toBool();return false;};
        voice.queueFixtureAudio(tone(3));QVERIFY(waitNative(localActive));
        if(gate=="mute")voice.setMuted(true);
        else if(gate=="deafen")voice.setDeafened(true);
        else if(gate.startsWith("server-"))voice.onVoiceState({{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"mute",gate=="server-mute"},{"deaf",gate=="server-deafen"}});
        else if(gate=="microphone-test")voice.startMicTest();else voice.leave();
        QVERIFY2(waitNative([&]{return !localActive();},50),"Closing a transmission gate must clear the ring without waiting for the cadence hold");
        const auto clearedAt=speaking.size();voice.queueFixtureAudio(tone(3));waitNativeMs(80);
        QVERIFY(!localActive());for(int i=clearedAt;i<speaking.size();++i)if(speaking[i][0].toString()==selfId)QVERIFY(!speaking[i][1].toBool());
        if(voice.micTesting())voice.stopMicTest();voice.leave();
#endif
    }
    void microphoneTestStaysConnectedAndIsolatesCallAudio(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        VoiceEngine voice;GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.encryptedReady();},5000));voice.configureAudio({false,false,false,1.0,1.0,-70});
        voice.queueFixtureAudio(tone(3));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==3;}));
        gateway.send(5,{{"user_id",peerId},{"ssrc",int(peerSsrc)},{"speaking",1}});waitNativeMs(20);
        int encoderError=0;auto *encoder=opus_encoder_create(48000,2,OPUS_APPLICATION_VOIP,&encoderError);QCOMPARE(encoderError,OPUS_OK);
        const auto destroyEncoder=qScopeGuard([&]{opus_encoder_destroy(encoder);});
        auto incomingTone=[&](quint16 start){
            const auto pcm=tone(6);
            for(int frame=0;frame<6;++frame){
                std::array<opus_int16,1920> samples;std::memcpy(samples.data(),pcm.constData()+frame*3840,3840);
                QByteArray opus(1275,Qt::Uninitialized);const auto count=opus_encode(encoder,samples.data(),960,data(opus),opus.size());
                QVERIFY(count>0);opus.resize(count);const auto encrypted=gateway.mls->encrypt(opus);QVERIFY(!encrypted.isEmpty());
                const auto sequence=quint16(start+frame);
                const auto wire=VoicePacket::encrypt(encrypted,gateway.transportKey,sequence,quint32(sequence)*960,peerSsrc,sequence);
                QCOMPARE(gateway.udp.writeDatagram(wire,gateway.clientAddress,gateway.clientPort),qint64(wire.size()));
            }
        };
        incomingTone(100);QVERIFY(waitNative([&]{return voice.fixtureCounters().value("decoded").toInt()>=3;}));
        voice.takeFixtureAudio();QSignalSpy requested(&voice,&VoiceEngine::voiceStateRequested);
        voice.startMicTest();QVERIFY(voice.micTesting());QVERIFY(voice.muted());QVERIFY(voice.deafened());
        QVERIFY(voice.connected());QVERIFY(voice.encryptedReady());QCOMPARE(voice.channelId(),QString::number(channel));
        QTRY_COMPARE(requested.count(),1);QCOMPARE(requested.last(),QVariantList({"1111",QString::number(channel),true,true}));
        const auto sent=voice.fixtureCounters().value("sent").toULongLong(),decoded=voice.fixtureCounters().value("decoded").toULongLong();
        const auto received=voice.fixtureCounters().value("received").toULongLong();
        const auto local=tone(6);voice.queueFixtureAudio(local);incomingTone(106);
        QVERIFY(!voice.playSoundboardPcm(tone(1)));
        QVERIFY(waitNative([&]{return voice.fixtureCounters().value("micFrames").toInt()==6;}));waitNativeMs(140);
        QCOMPARE(voice.takeFixtureAudio(),local);QCOMPARE(voice.fixtureCounters().value("sent").toULongLong(),sent);
        QCOMPARE(voice.fixtureCounters().value("decoded").toULongLong(),decoded);QCOMPARE(voice.fixtureCounters().value("received").toULongLong(),received);
        QCOMPARE(gateway.outgoingAudio.size(),3);QVERIFY(voice.connected());QVERIFY(voice.encryptedReady());
        voice.queueFixtureAudio(tone(6));voice.stopMicTest();
        QVERIFY(!voice.micTesting());QVERIFY(!voice.muted());QVERIFY(!voice.deafened());
        QTRY_COMPARE(requested.count(),2);QCOMPARE(requested.last(),QVariantList({"1111",QString::number(channel),false,false}));
        QCOMPARE(voice.fixtureCounters().value("micQueuedBytes").toInt(),0);QVERIFY(voice.takeFixtureAudio().isEmpty());
        // Local test speech is discarded before the call is unmuted.
        waitNativeMs(80);QCOMPARE(gateway.outgoingAudio.size(),3);
        voice.queueFixtureAudio(tone(3));incomingTone(112);
        QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==6 && voice.fixtureCounters().value("decoded").toULongLong()>decoded;}));
        QVERIFY(!voice.takeFixtureAudio().isEmpty());QVERIFY(voice.connected());QVERIFY(voice.encryptedReady());
        int identifies=0;for(const auto &packet:gateway.clientJson)if(packet.value("op").toInt()==0)++identifies;
        QCOMPARE(identifies,1);QCOMPARE(voice.fixtureCounters().value("failures").toInt(),0);voice.leave();
#endif
    }
    void generatedCaptureSurvivesGuiLoad_data(){
        QTest::addColumn<int>("stallMs");QTest::newRow("unloaded")<<0;QTest::newRow("streaming-like-gui-load")<<70;
    }
    void generatedCaptureSurvivesGuiLoad(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        QFETCH(int,stallMs);VoiceEngine voice;GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.encryptedReady();},5000));voice.configureAudio({false,false,false,1.0,1.0,-70});
        // A separate clock supplies generated microphone PCM even while the UI
        // is busy. Feed the audio owner directly, just like its QAudioSource,
        // rather than delaying generated capture through a GUI event first.
        // Production ingress, DSP, Opus, DAVE and RTP are all exercised.
        QThread producerThread;QObject producer;producer.moveToThread(&producerThread);QTimer *captureTimer=nullptr;std::atomic<int> offered=0;
        connect(&producerThread,&QThread::started,&producer,[&]{
            captureTimer=new QTimer(&producer);captureTimer->setTimerType(Qt::PreciseTimer);captureTimer->setInterval(20);
            connect(captureTimer,&QTimer::timeout,&producer,[&]{const int frame=offered++;const auto generated=tone(1,frame*960);
                voice.queueFixtureAudio(generated);});captureTimer->start();
        });
        auto stopProducer=[&]{if(!producerThread.isRunning())return;QMetaObject::invokeMethod(&producer,[&]{delete captureTimer;captureTimer=nullptr;},Qt::BlockingQueuedConnection);producerThread.quit();producerThread.wait();};
        const auto cleanup=qScopeGuard(stopProducer);producerThread.start();
        QEventLoop loop;QTimer finish,load;finish.setSingleShot(true);finish.setTimerType(Qt::PreciseTimer);finish.setInterval(6000);load.setInterval(125);load.setTimerType(Qt::PreciseTimer);
        connect(&finish,&QTimer::timeout,&loop,&QEventLoop::quit);connect(&load,&QTimer::timeout,&loop,[&]{if(stallMs)QThread::msleep(stallMs);});finish.start();if(stallMs)load.start();loop.exec();load.stop();stopProducer();
        QVERIFY(waitNative([&]{return voice.fixtureCounters().value("captureFrames").toInt()==offered && !voice.fixtureCounters().value("captureQueued").toInt() && gateway.outgoingAudio.size()==offered;},1000));
        int decoded=0,opusResult=0;auto *decoder=opus_decoder_create(48000,2,&opusResult);QCOMPARE(opusResult,OPUS_OK);
        const auto destroy=qScopeGuard([&]{opus_decoder_destroy(decoder);});double sumSin=0,sumCos=0,power=0;quint64 compared=0;
        for(const auto &cipher:gateway.outgoingAudio){const auto plain=gateway.mls->decrypt(cipher);QVERIFY(!plain.isEmpty());std::array<opus_int16,1920> pcm;
            QCOMPARE(opus_decode(decoder,data(plain),plain.size(),pcm.data(),960,0),960);
            for(int i=0;i<960;++i){const int position=decoded*960+i;if(position<4800)continue;const double value=pcm[i*2],phase=position*2.0*3.141592653589793*440.0/48000;
                sumSin+=value*std::sin(phase);sumCos+=value*std::cos(phase);power+=value*value;++compared;}++decoded;
        }
        const double coherence=2*(sumSin*sumSin+sumCos*sumCos)/(compared*power);qint64 maxWireGap=0;
        for(int i=1;i<gateway.outgoingStamps.size();++i){QCOMPARE(gateway.outgoingStamps[i]-gateway.outgoingStamps[i-1],quint32(960));maxWireGap=std::max(maxWireGap,gateway.outgoingTimes[i]-gateway.outgoingTimes[i-1]);}
        const auto counters=voice.fixtureCounters();
        qInfo("Generated mic: UI stall=%d ms, offered=%d, decoded=%d, waveform coherence=%.6f, max wire gap=%lld ms, max queued=%d frames, catch-up=%llu frames",stallMs,offered.load(),decoded,coherence,qlonglong(maxWireGap),counters.value("captureMaxQueued").toInt(),counters.value("captureCatchups").toULongLong());
        QVERIFY(offered>=290);QCOMPARE(decoded,offered.load());QVERIFY2(coherence>.98,"Retained microphone samples must preserve continuous phase through callback batching");
        QCOMPARE(counters.value("captureDropped").toULongLong(),quint64(0));QVERIFY(counters.value("captureMaxQueued").toInt()<=12);
        // The GUI-owned receiver's callback timestamps can still bunch during
        // this test. The independent voice_playout_test checks wire deadlines.
        // Here the audio owner must avoid the UI pause, preserve samples and
        // avoid building the previous five-frame microphone backlog.
        QVERIFY(counters.value("captureMaxQueued").toInt()<=2);
        QVERIFY(counters.value("maxAudioLateMs").toLongLong()<40);
        if(!stallMs)QVERIFY(maxWireGap<60);
        QCOMPARE(counters.value("failures").toULongLong(),quint64(0));voice.leave();
#endif
    }
    void soundboardSharesTheMicrophoneClock_data(){
        QTest::addColumn<QString>("source");QTest::newRow("no-microphone")<<QString("none");QTest::newRow("batched-microphone")<<QString("batched");QTest::newRow("stalled-microphone")<<QString("stalled");
    }
    void soundboardSharesTheMicrophoneClock(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        QFETCH(QString,source);VoiceEngine voice;GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.encryptedReady();},5000));voice.configureAudio({false,false,false,1.0,1.0,-70});
        if(source!="none"){voice.queueFixtureAudio(tone(2));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==2;}));}
        QVERIFY(voice.playSoundboardPcm(tone(30)));QTimer capture;capture.setTimerType(Qt::PreciseTimer);capture.setInterval(40);int captured=2;
        if(source=="batched"){
            connect(&capture,&QTimer::timeout,&voice,[&]{voice.queueFixtureAudio(tone(2,captured*960));captured+=2;});capture.start();waitNativeMs(1000);capture.stop();
            QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==captured && !voice.fixtureCounters().value("captureQueued").toInt();}));
            QCOMPARE(voice.fixtureCounters().value("soundboardQueued").toInt(),0);QCOMPARE(gateway.outgoingAudio.size(),captured);
            // Empty ticks between two-frame microphone batches must not insert
            // extra board-only packets or make an effect play faster.
            for(int i=1;i<gateway.outgoingStamps.size();++i)QCOMPARE(gateway.outgoingStamps[i]-gateway.outgoingStamps[i-1],quint32(960));
        }else{
            const int expected=source=="none"?30:32;QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==expected;},1800));
            QCOMPARE(voice.fixtureCounters().value("soundboardQueued").toInt(),0);waitNativeMs(100);QCOMPARE(gateway.outgoingAudio.size(),expected);
            if(source=="stalled")QVERIFY(gateway.outgoingStamps[2]-gateway.outgoingStamps[1]>=quint32(10*960));
        }
        QCOMPARE(voice.fixtureCounters().value("captureDropped").toInt(),0);voice.leave();
#endif
    }
    void captureOverrunPreservesSourceClock_data(){
        QTest::addColumn<int>("pauseMs");QTest::addColumn<int>("frames");QTest::addColumn<int>("drops");
        QTest::newRow("twelve-retained-across-two-wakes")<<260<<12<<0;
        QTest::newRow("trim-and-wall-reanchor-count-loss-once")<<400<<20<<10;
    }
    void captureOverrunPreservesSourceClock(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        QFETCH(int,pauseMs);QFETCH(int,frames);QFETCH(int,drops);VoiceEngine voice;GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.encryptedReady();},5000));voice.configureAudio({false,false,false,1.0,1.0,-70});
        voice.queueFixtureAudio(tone(2));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==2;}));const auto previous=gateway.outgoingStamps.last();const auto beforeCatchup=voice.fixtureCounters().value("captureCatchups").toInt();
        // Withhold source PCM, then supply a bounded capture batch. The
        // independent owner keeps running while this caller is paused.
        QThread::msleep(pauseMs);voice.queueFixtureAudio(tone(frames,1920));
        QCOMPARE(voice.fixtureCounters().value("captureDropped").toInt(),drops);QVERIFY(voice.fixtureCounters().value("captureQueued").toInt()<=12);
        QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==2+frames-drops && !voice.fixtureCounters().value("captureQueued").toInt();}));
        const auto firstStep=gateway.outgoingStamps[2]-previous;
        QVERIFY2(firstStep>=quint32((drops+1)*960),"The first retained sample must follow the discarded source interval");
        QVERIFY2(firstStep<=quint32((drops+2)*960),"Reanchoring must not count trimmed samples twice or cap the twelve-frame source at ten");
        for(int i=3;i<gateway.outgoingStamps.size();++i)QCOMPARE(gateway.outgoingStamps[i]-gateway.outgoingStamps[i-1],quint32(960));
        QCOMPARE(voice.fixtureCounters().value("captureCatchups").toInt()-beforeCatchup,frames-drops-(frames-drops>10?2:1));voice.leave();
#endif
    }
    void captureAbsenceAndMuteDoNotReplayStaleAudio_data(){
        QTest::addColumn<QString>("gate");for(const auto *gate:{"absent","mute","deafen","server-mute","server-deafen"})QTest::newRow(gate)<<QString::fromLatin1(gate);
    }
    void captureAbsenceAndMuteDoNotReplayStaleAudio(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        QFETCH(QString,gate);VoiceEngine voice;GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.encryptedReady();},5000));voice.queueFixtureAudio(tone(2));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==2;}));
        const auto initiallySent=gateway.outgoingAudio.size();const auto initialPrevious=gateway.outgoingStamps.last();
        auto setGate=[&](bool enabled){if(gate=="mute")voice.setMuted(enabled);else if(gate=="deafen")voice.setDeafened(enabled);
            else if(gate.startsWith("server-"))voice.onVoiceState({{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"mute",enabled && gate=="server-mute"},{"deaf",enabled && gate=="server-deafen"}});};
        if(gate!="absent"){
            // Capture-ready dispatch may send this speech before the gate
            // closes, including within this owner action. Once closed, no
            // queued or newly supplied microphone audio may be transmitted.
            QVERIFY(QMetaObject::invokeMethod(voice.m_runtimeContext,[&]{voice.queueFixtureAudio(tone(6));setGate(true);},Qt::BlockingQueuedConnection));
            QCOMPARE(voice.fixtureCounters().value("captureQueued").toInt(),0);
            const auto before=voice.fixtureCounters().value("captureFrames");voice.queueFixtureAudio(tone(6));QCOMPARE(voice.fixtureCounters().value("captureFrames"),before);QCOMPARE(voice.fixtureCounters().value("captureQueued").toInt(),0);}
        const auto sentBeforePause=voice.fixtureCounters().value("sent").toInt();
        QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==sentBeforePause;}));
        const auto previous=gateway.outgoingStamps.last();
        const int additionalSourceFrames=int(quint32(previous-initialPrevious)/960);
        QCOMPARE(additionalSourceFrames,sentBeforePause-initiallySent);
        waitNativeMs(380);QCOMPARE(voice.fixtureCounters().value("sent").toInt(),sentBeforePause);QCOMPARE(gateway.outgoingAudio.size(),sentBeforePause);
        setGate(false);voice.queueFixtureAudio(tone(2));QVERIFY(waitNative([&]{return gateway.outgoingAudio.size()==sentBeforePause+2;}));waitNativeMs(70);QCOMPARE(gateway.outgoingAudio.size(),sentBeforePause+2);
        // The pre-gate batch can already occupy part of the source timeline;
        // measure from its last actual RTP packet rather than from the first
        // two fixture packets or from an assumed timer-only dispatch.
        const auto elapsedSource=gateway.outgoingStamps[sentBeforePause]-previous;
        QVERIFY(elapsedSource>=quint32((16-additionalSourceFrames)*960));QVERIFY(elapsedSource<=quint32((25-additionalSourceFrames)*960));voice.leave();
#endif
    }

    void encryptedCamera_data(){QTest::addColumn<QString>("codec");QTest::newRow("h264-camera")<<QString("H264");QTest::newRow("vp8-mobile-camera")<<QString("VP8");QTest::newRow("hevc-camera")<<QString("H265");QTest::newRow("av1-camera")<<QString("AV1");}
    void encryptedCamera(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        QFETCH(QString,codec);const bool vp8=codec=="VP8";VoiceEngine voice;CameraEngine camera(&voice);QVideoSink sink;camera.setVideoSink(&sink);
        QVERIFY(!voice.watchCamera(peerId));QVERIFY(!camera.active());
        GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QTRY_VERIFY_WITH_TIMEOUT(voice.encryptedReady() || !gateway.failure.isEmpty(),5000);QVERIFY2(voice.encryptedReady(),qPrintable(gateway.failure));
        bool offered=false;for(const auto &message:gateway.clientJson)if(message.value("op").toInt()==1)for(const auto &entry:message.value("d").toObject().value("codecs").toArray()){
            const auto c=entry.toObject();if(c.value("name").toString()==codec)offered=c.value("decode").toBool() && !c.value("encode").toBool();
        }QVERIFY(offered);
        auto audioWanted=[&]{for(auto it=gateway.clientJson.crbegin();it!=gateway.clientJson.crend();++it)if(it->value("op").toInt()==15){
            const auto wants=it->value("d").toObject();return wants.value("any").toInt()==100 && wants.value(QString::number(peerSsrc)).toInt(100)==100;}return false;};
        QTRY_VERIFY(audioWanted());
        const quint32 videoSsrc=codec=="AV1"?306:codec=="H265"?305:vp8?303:302;
        const int payloadType=codec=="AV1"?101:codec=="H265"?103:vp8?107:105;
        gateway.send(12,{{"user_id",peerId},{"audio_ssrc",int(peerSsrc)},{"streams",QJsonArray{
            QJsonObject{{"type","video"},{"ssrc",301},{"quality",50},{"active",true}},
            QJsonObject{{"type","video"},{"ssrc",int(videoSsrc)},{"rtx_ssrc",304},{"quality",100},{"active",true}}
        }}});QTest::qWait(30);
        auto wanted=[&](int value){for(auto it=gateway.clientJson.crbegin();it!=gateway.clientJson.crend();++it)if(it->value("op").toInt()==15)return it->value("d").toObject().value(QString::number(videoSsrc)).toInt(-1)==value;return false;};
        QVERIFY(!camera.active());QVERIFY(wanted(0));QVERIFY(audioWanted());QSignalSpy starts(&camera,&CameraEngine::watchRequested),stops(&camera,&CameraEngine::stopRequested);
        camera.watch(peerId,"Fixture camera");QVERIFY(camera.active());QCOMPARE(starts.size(),1);QTRY_VERIFY(wanted(100));QVERIFY(audioWanted());
        for(auto it=gateway.clientJson.crbegin();it!=gateway.clientJson.crend();++it)if(it->value("op").toInt()==15){QCOMPARE(it->value("d").toObject().value("301").toInt(-1),0);break;}
        quint16 seq=65530;quint32 nonce=10000,stamp=9000;
        const QSize inputSize=vp8?QSize(180,320):QSize(320,180);
        auto sendFrame=[&](bool damaged){
            auto frame=gateway.mls->encryptVideo(StreamTestMedia::encodedFrame(codec,inputSize.width(),inputSize.height()),vp8,videoSsrc);QVERIFY(!frame.isEmpty());
            const auto payloads=StreamTestMedia::packetize(codec,frame,1000);QVERIFY(!payloads.isEmpty());
            for(qsizetype i=0;i<payloads.size();++i){auto wire=BroadcastPacket::encrypt(payloads[i],gateway.transportKey,payloadType,i+1==payloads.size(),seq++,stamp,videoSsrc,nonce++);if(damaged)wire[wire.size()-5]^=1;gateway.udp.writeDatagram(wire,gateway.clientAddress,gateway.clientPort);}stamp+=3000;
        };
        sendFrame(true);QTest::qWait(100);QCOMPARE(camera.frameCount(),quint64(0));
        sendFrame(false);QTRY_VERIFY_WITH_TIMEOUT(camera.frameCount()>0 || !camera.error().isEmpty(),3000);QVERIFY2(camera.receiving(),qPrintable(camera.error()));
        QCOMPARE(QSize(camera.videoWidth(),camera.videoHeight()),inputSize);QVERIFY(sink.videoFrame().isValid());QVERIFY(voice.connected());
        voice.setDeafened(true);sendFrame(false);QTest::qWait(100);QVERIFY(camera.active());voice.setDeafened(false);
        camera.stop();QVERIFY(!camera.active());QVERIFY(voice.connected());QVERIFY(!sink.videoFrame().isValid());QCOMPARE(stops.size(),1);QTRY_VERIFY(wanted(0));QVERIFY(audioWanted());
        camera.watch(peerId,"Fixture camera");QVERIFY(camera.active());
        voice.onVoiceState({{"guild_id","1111"},{"user_id",peerId},{"channel_id","2222"},{"self_video",false}});QTRY_VERIFY(!camera.active());QVERIFY(voice.connected());
        camera.watch(peerId,"Waiting camera");QVERIFY(camera.active());voice.leave();QTRY_VERIFY(!camera.active());QVERIFY(!sink.videoFrame().isValid());
#endif
    }
    void heartbeatLatencyUsesMatchingAckAndBoundedHistory(){
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;gateway.autoAck=false;QVERIFY(gateway.listen());
        QCOMPARE(voice.latencyMs(),-1.0);QVERIFY(voice.latencyHistory().isEmpty());prepare(voice,gateway);
        QTRY_VERIFY_WITH_TIMEOUT(voice.connected(),2000);QCOMPARE(voice.voiceServerLabel(),QString("fixture.discord.media"));
        QJsonValue token;for(const auto &message:gateway.clientJson)if(message.value("op").toInt()==3)token=message.value("d").toObject().value("t");
        QVERIFY(token.isDouble());gateway.send(6,{{"t",token.toDouble()-1}});QTest::qWait(25);
        QCOMPARE(voice.latencyMs(),-1.0);QVERIFY(voice.latencyHistory().isEmpty());
        QSignalSpy stats(&voice,&VoiceEngine::statsChanged),changed(&voice,&VoiceEngine::changed);
        gateway.send(6,{{"t",token}});QTRY_COMPARE(voice.latencyHistory().size(),1);
        QVERIFY(voice.latencyMs()>=20);QCOMPARE(voice.averageLatencyMs(),voice.latencyMs());QTRY_VERIFY(stats.size()>0);QCOMPARE(changed.size(),0);
        gateway.send(6,{{"t",token}});QTest::qWait(15);QCOMPARE(voice.latencyHistory().size(),1);
        gateway.autoAck=true;
        for(int i=0;i<62;++i){const auto before=stats.size();gateway.send(8,{{"heartbeat_interval",1000}});QTRY_VERIFY(stats.size()>before);}
        const auto history=voice.latencyHistory();QCOMPARE(history.size(),60);double sum=0;
        for(const auto &entry:history){const auto point=entry.toMap();QVERIFY(point.value("ms").toDouble()>=0);QVERIFY(point.value("timestamp").toLongLong()>0);sum+=point.value("ms").toDouble();}
        QCOMPARE(voice.averageLatencyMs(),sum/60);voice.leave();QCOMPARE(voice.latencyMs(),-1.0);QCOMPARE(voice.averageLatencyMs(),-1.0);
        QVERIFY(voice.latencyHistory().isEmpty());QVERIFY(voice.voiceServerLabel().isEmpty());
    }
    void localHandshakeAndSolitaryChannel(){
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());
        prepare(voice,gateway,false);QTRY_VERIFY_WITH_TIMEOUT(voice.connected(),3000);QVERIFY(gateway.handshakeValid);QVERIFY(gateway.protocolSelected);
        QCOMPARE(voice.stage(),QString("waiting_for_participants"));QVERIFY(!voice.encryptedReady());QVERIFY(!voice.micTesting());
        QCOMPARE(voice.channelId(),QString("2222"));QCOMPARE(voice.currentGuildId(),QString("1111"));
        const auto summary=voice.diagnosticSummary();QVERIFY(!summary.contains("fixture-token"));QVERIFY(!summary.contains(selfId));QVERIFY(!summary.contains("Local fixture"));
        QSignalSpy speaking(&voice,&VoiceEngine::speakingChanged);
        gateway.send(11,{{"user_ids",QJsonArray{peerId}}});gateway.send(5,{{"user_id",peerId},{"ssrc",int(peerSsrc)},{"speaking",1}});
        QTest::qWait(40);QCOMPARE(speaking.count(),0);
        gateway.send(5,{{"user_id",peerId},{"ssrc",int(peerSsrc)},{"speaking",0}});
        QTest::qWait(40);QCOMPARE(speaking.count(),0);
        voice.leave();QVERIFY(!voice.connected());QCOMPARE(voice.stage(),QString("idle"));
    }
    void everyNewSessionAnnouncesItsAudioSsrcEvenWhenListeningOnly_data(){
        QTest::addColumn<bool>("muted");QTest::addColumn<bool>("deafened");
        QTest::newRow("silent-join")<<false<<false;
        QTest::newRow("muted-listener")<<true<<false;
        QTest::newRow("deafened-join")<<false<<true;
    }
    void everyNewSessionAnnouncesItsAudioSsrcEvenWhenListeningOnly(){
        QFETCH(bool,muted);QFETCH(bool,deafened);
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");
        voice.setMuted(muted);voice.setDeafened(deafened);
        GatewayFixture first;QVERIFY(first.listen());prepare(voice,first);
        QTRY_VERIFY_WITH_TIMEOUT(voice.connected() && first.speakingAnnounced,2000);
        auto announcements=[](const GatewayFixture &gateway){QList<QJsonObject> result;
            for(const auto &packet:gateway.clientJson)if(packet.value("op").toInt()==5)result.append(packet.value("d").toObject());return result;};
        auto initial=announcements(first);QCOMPARE(initial.size(),1);QCOMPARE(initial.first().value("speaking").toInt(),0);
        QCOMPARE(initial.first().value("delay").toInt(),0);QCOMPARE(quint32(initial.first().value("ssrc").toDouble()),first.allocatedSsrc);
        QCOMPARE(first.unannouncedAudio,0);QVERIFY(first.outgoingAudio.isEmpty());QCOMPARE(voice.muted(),muted||deafened);QCOMPARE(voice.deafened(),deafened);
        voice.leave();
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QJsonValue::Null},{"user_id",selfId}});
        GatewayFixture second;second.allocatedSsrc=30303;QVERIFY(second.listen());prepare(voice,second,false);
        QTRY_VERIFY_WITH_TIMEOUT(voice.connected() && second.speakingAnnounced,2000);
        initial=announcements(second);QCOMPARE(initial.size(),1);QCOMPARE(quint32(initial.first().value("ssrc").toDouble()),second.allocatedSsrc);
        QCOMPARE(initial.first().value("speaking").toInt(),0);QCOMPARE(second.unannouncedAudio,0);voice.leave();
    }
    void encryptedReceiveWorksBeforeLocalSpeechAndAcrossCameraChanges_data(){
        QTest::addColumn<bool>("videoAnnouncement");QTest::addColumn<bool>("muted");
        QTest::newRow("speaking-ssrc-silent-local")<<false<<false;
        QTest::newRow("video-ssrc-silent-local")<<true<<false;
        QTest::newRow("speaking-ssrc-muted-listener")<<false<<true;
        QTest::newRow("video-ssrc-muted-listener")<<true<<true;
    }
    void encryptedReceiveWorksBeforeLocalSpeechAndAcrossCameraChanges(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ fixture headers required");
#else
        QFETCH(bool,videoAnnouncement);QFETCH(bool,muted);
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");voice.setMuted(muted);
        GatewayFixture gateway;gateway.initialVersion=1;QVERIFY(gateway.listen());prepare(voice,gateway);
        QTRY_VERIFY_WITH_TIMEOUT(voice.encryptedReady() || !gateway.failure.isEmpty(),5000);QVERIFY2(voice.encryptedReady(),qPrintable(gateway.failure));
        QTRY_VERIFY(gateway.speakingAnnounced);QCOMPARE(gateway.unannouncedAudio,0);
        auto activeLocalSpeech=[&]{for(const auto &packet:gateway.clientJson)if(packet.value("op").toInt()==5 && packet.value("d").toObject().value("speaking").toInt()!=0)return true;return false;};
        auto audioWanted=[&]{for(auto it=gateway.clientJson.crbegin();it!=gateway.clientJson.crend();++it)if(it->value("op").toInt()==15){
            const auto wants=it->value("d").toObject();return wants.value("any").toInt()==100 && wants.value(QString::number(peerSsrc)).toInt(100)==100;}return false;};
        QTRY_VERIFY(audioWanted());
        QVERIFY(!activeLocalSpeech());
        // A real muted listener emits no audio; a silent microphone must still
        // announce Speaking before its first encrypted comfort-noise packet.
        voice.queueFixtureAudio(QByteArray(6*3840,'\0'));
        if(muted){QTest::qWait(140);QVERIFY(gateway.outgoingAudio.isEmpty());}
        else QTRY_VERIFY_WITH_TIMEOUT(gateway.outgoingAudio.size()>=3,1500);
        QVERIFY(!activeLocalSpeech());QCOMPARE(gateway.unannouncedAudio,0);QVERIFY(voice.connected());
        if(videoAnnouncement)gateway.send(12,{{"user_id",peerId},{"audio_ssrc",int(peerSsrc)}});
        else gateway.send(5,{{"user_id",peerId},{"ssrc",int(peerSsrc)},{"speaking",1}});
        QTest::qWait(30);
        auto incomingTone=[&](quint16 startSequence,quint32 startNonce){
            int result=0;auto *encoder=opus_encoder_create(48000,2,OPUS_APPLICATION_VOIP,&result);QCOMPARE(result,OPUS_OK);
            const auto pcm=tone(8);
            for(int i=0;i<8;++i){
                std::array<opus_int16,1920> samples;std::memcpy(samples.data(),pcm.constData()+i*3840,3840);QByteArray opus(1275,Qt::Uninitialized);
                const int count=opus_encode(encoder,samples.data(),960,data(opus),opus.size());QVERIFY(count>0);opus.resize(count);
                const auto encrypted=gateway.mls->encrypt(opus);QVERIFY(!encrypted.isEmpty());
                const auto wire=VoicePacket::encrypt(encrypted,gateway.transportKey,quint16(startSequence+i),quint32((startSequence+i)*960),peerSsrc,startNonce+i);
                gateway.udp.writeDatagram(wire,gateway.clientAddress,gateway.clientPort);
            }opus_encoder_destroy(encoder);
        };
        auto expectIncoming=[&](quint16 sequence,quint32 nonce){
            const auto before=voice.fixtureCounters().value("decoded").toULongLong();incomingTone(sequence,nonce);
            QTRY_VERIFY_WITH_TIMEOUT(voice.fixtureCounters().value("decoded").toULongLong()>=before+8,1500);
            const auto output=voice.takeFixtureAudio();QVERIFY(output.size()>=3840);bool audible=false;for(const auto sample:output)if(sample){audible=true;break;}QVERIFY(audible);
        };
        expectIncoming(100,1000);QVERIFY(!activeLocalSpeech());
        gateway.send(12,{{"user_id",peerId},{"audio_ssrc",int(peerSsrc)},{"streams",QJsonArray{
            QJsonObject{{"type","video"},{"ssrc",302},{"rtx_ssrc",304},{"quality",100},{"active",true}}}}});
        QTest::qWait(30);QVERIFY(voice.watchCamera(peerId));QCOMPARE(voice.cameraUserId(),QString(peerId));QTRY_VERIFY(audioWanted());
        expectIncoming(200,2000);voice.stopCamera();QVERIFY(voice.cameraUserId().isEmpty());QTRY_VERIFY(audioWanted());
        expectIncoming(300,3000);QVERIFY(voice.connected());QCOMPARE(gateway.unannouncedAudio,0);
        voice.setMuted(false);voice.queueFixtureAudio(tone(6));QTRY_VERIFY_WITH_TIMEOUT(activeLocalSpeech(),1500);
        QTRY_VERIFY_WITH_TIMEOUT([&]{for(auto it=gateway.clientJson.crbegin();it!=gateway.clientJson.crend();++it)if(it->value("op").toInt()==5)return it->value("d").toObject().value("speaking").toInt()==0;return false;}(),1500);
        QCOMPARE(voice.fixtureCounters().value("failures").toULongLong(),quint64(0));voice.leave();
#endif
    }
    void missedHeartbeatResumesAndRetainsFailureEvidence(){
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());prepare(voice,gateway);
        QVERIFY(waitNative([&]{return voice.connected() && voice.latencyMs()>=0;},3000));const auto stage=voice.stage();QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        gateway.autoAck=false;voice.requestFixtureHeartbeat();voice.requestFixtureHeartbeat();
        QVERIFY(voice.connected());QVERIFY(waitNative([&]{return gateway.resumePayloads.size()==1 && voice.stage()==stage;},5000));QCOMPARE(requests.size(),0);
        const auto failures=voice.diagnosticHistory().value("failures").toArray();QVERIFY(!failures.isEmpty());
        bool recorded=false;for(const auto &failure:failures)if(failure.toObject().value("reason").toString()=="heartbeat_timeout")recorded=true;QVERIFY(recorded);
        const auto history=QJsonDocument(voice.diagnosticHistory()).toJson();QVERIFY(!history.contains("fixture-token"));QVERIFY(!history.contains("fixture-session"));voice.leave();
    }
    void solitaryDaveSessionWaitsForParticipant(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are needed for the external-sender fixture");
#else
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;
        gateway.initialVersion=1;gateway.announcePeer=false;QVERIFY(gateway.listen());prepare(voice,gateway);
        QTRY_VERIFY_WITH_TIMEOUT(voice.connected(),3000);QCOMPARE(voice.stage(),QString("waiting_for_participants"));
        QVERIFY(voice.status().contains("waiting for someone to join"));QVERIFY(!voice.encryptedReady());QVERIFY(voice.error().isEmpty());
        gateway.send(11,{{"user_ids",QJsonArray{peerId}}});
        QTRY_COMPARE_WITH_TIMEOUT(voice.stage(),QString("mls_exchange"),1000);voice.leave();
#endif
    }
    void sameGuildSwitchDoesNotSendLeave(){
        VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");GatewayFixture gateway;QVERIFY(gateway.listen());
        prepare(voice,gateway);QTRY_VERIFY_WITH_TIMEOUT(voice.connected(),3000);
        QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        voice.join("1111","5555","Second fixture",selfId);
        QTRY_COMPARE(requests.size(),1);QCOMPARE(requests.at(0).at(0).toString(),QString("1111"));
        QCOMPARE(requests.at(0).at(1).toString(),QString("5555"));
        voice.join("1111","6666","Third fixture",selfId);
        QTRY_COMPARE(requests.size(),2);QCOMPARE(requests.at(1).at(0).toString(),QString("1111"));
        QCOMPARE(requests.at(1).at(1).toString(),QString("6666"));
        // Both earlier targets can still have queued own-state updates while the
        // newest same-guild join waits for its first acknowledgement.
        voice.onVoiceState({{"guild_id","1111"},{"channel_id",QString::number(channel)},{"user_id",selfId},{"session_id","fixture-session"}});
        voice.onVoiceState({{"guild_id","1111"},{"channel_id","5555"},{"user_id",selfId},{"session_id","second-fixture-session"}});
        waitNativeMs(40);QCOMPARE(voice.stage(),QString("awaiting_gateway_state"));QCOMPARE(requests.size(),2);
        voice.onVoiceState({{"guild_id","1111"},{"channel_id","6666"},{"user_id",selfId},{"session_id","third-fixture-session"}});
        voice.onVoiceServer({{"guild_id","1111"},{"token","third-fixture-token"},{"endpoint","fixture.discord.media"}});
        QTRY_VERIFY_WITH_TIMEOUT(voice.connected(),3000);QCOMPARE(voice.channelId(),QString("6666"));QVERIFY(voice.error().isEmpty());
        voice.leave();QTRY_COMPARE(requests.size(),3);QVERIFY(requests.last().at(1).toString().isEmpty());
    }
    void encryptedMedia_data(){
        QTest::addColumn<bool>("createCommit"); QTest::addColumn<bool>("littleEndianNonce");
        QTest::newRow("create-commit-be")<<true<<false; QTest::newRow("receive-welcome-be")<<false<<false;
        QTest::newRow("create-commit-le")<<true<<true; QTest::newRow("receive-welcome-le")<<false<<true;
    }
    void encryptedMedia(){
#ifndef RELAY_TEST_MLSPP
        QSKIP("Build with RELAY_TEST_MLSPP and MLS++ headers for the full MLS integration fixture");
#else
        QFETCH(bool,createCommit);QFETCH(bool,littleEndianNonce);VoiceEngine voice;if(!voice.available())QSKIP("DAVE unavailable");
        GatewayFixture gateway;gateway.initialVersion=1;gateway.createCommit=createCommit;QVERIFY(gateway.listen());prepare(voice,gateway);
        QTRY_VERIFY_WITH_TIMEOUT(voice.encryptedReady() || !gateway.failure.isEmpty(),5000);QVERIFY2(gateway.failure.isEmpty(),qPrintable(gateway.failure));
        QVERIFY2(voice.encryptedReady(),qPrintable(voice.diagnosticSummary()));QVERIFY(gateway.groupReady);QVERIFY(gateway.handshakeValid);
        // Exercise both SSRC notification forms. No real device is opened by fixture mode.
        gateway.send(12,{{"user_id",peerId},{"audio_ssrc",int(peerSsrc)}});
        gateway.send(5,{{"user_id",peerId},{"ssrc",int(peerSsrc)},{"speaking",1}});
        QTest::qWait(30);
        voice.setMuted(true);QVERIFY(!voice.playSoundboardPcm(tone(2)));voice.setMuted(false);
        QVERIFY(voice.playSoundboardPcm(tone(6)));QTRY_VERIFY_WITH_TIMEOUT(gateway.outgoingAudio.size()>=3,1500);
        voice.stopSoundboard();gateway.outgoingAudio.clear();
        queueFixtureAudio(voice,tone(6));
        QTRY_VERIFY_WITH_TIMEOUT(gateway.outgoingAudio.size()>=3,1500);
        int opusResult=0;OpusDecoder *decoder=opus_decoder_create(48000,2,&opusResult);QCOMPARE(opusResult,OPUS_OK);
        auto plain=gateway.mls->decrypt(gateway.outgoingAudio.first());QVERIFY(!plain.isEmpty());
        std::array<opus_int16,1920> decoded{};QCOMPARE(opus_decode(decoder,data(plain),plain.size(),decoded.data(),960,0),960);opus_decoder_destroy(decoder);
        bool nonzero=false;for(auto sample:decoded)if(sample){nonzero=true;break;}QVERIFY(nonzero);
        OpusEncoder *encoder=opus_encoder_create(48000,2,OPUS_APPLICATION_VOIP,&opusResult);QCOMPARE(opusResult,OPUS_OK);
        const auto pcm=tone(8);QByteArray lastPacket;
        for(int i=0;i<8;i++){
            std::array<opus_int16,1920> samples;std::memcpy(samples.data(),pcm.constData()+i*3840,3840);QByteArray opus(1275,Qt::Uninitialized);
            const int size=opus_encode(encoder,samples.data(),960,data(opus),opus.size());QVERIFY(size>0);opus.resize(size);
            const auto encrypted=gateway.mls->encrypt(opus);QVERIFY(!encrypted.isEmpty());
            // Exercise RTP wrap and nonce byte carry independently. Both raw nonce
            // representations authenticate, so replay logic must use RTP sequence.
            quint32 nonce = quint32(252+i); if(littleEndianNonce) nonce=qbswap(nonce);
            lastPacket=VoicePacket::encrypt(encrypted,gateway.transportKey,quint16(65532+i),quint32(i*960),peerSsrc,nonce);
            gateway.udp.writeDatagram(lastPacket,gateway.clientAddress,gateway.clientPort);
        }
        opus_encoder_destroy(encoder);
        QTRY_VERIFY_WITH_TIMEOUT(voice.fixtureCounters().value("decoded").toULongLong()>=3,1500);
        const auto output=voice.takeFixtureAudio();QVERIFY(output.size()>=3840);nonzero=false;for(auto b:output)if(b){nonzero=true;break;}QVERIFY(nonzero);
        QCOMPARE(voice.fixtureCounters().value("failures").toULongLong(),quint64(0));
        const auto accepted=voice.fixtureCounters().value("received").toULongLong();QCOMPARE(accepted,quint64(8));gateway.udp.writeDatagram(lastPacket,gateway.clientAddress,gateway.clientPort);QTest::qWait(30);
        QCOMPARE(voice.fixtureCounters().value("received").toULongLong(),accepted);
        // DAVE transition zero is executed without requiring an op23 acknowledgement.
        gateway.send(21,{{"transition_id",9},{"protocol_version",0}});QTRY_VERIFY_WITH_TIMEOUT(!voice.encryptedReady(),1000);
        QCOMPARE(voice.stage(),QString("waiting_for_participants"));voice.leave();
#endif
    }
    static void queueFixtureAudio(VoiceEngine &voice,const QByteArray &pcm){voice.queueFixtureAudio(pcm);}
};
QTEST_GUILESS_MAIN(VoiceGatewayTest)
#include "voice_gateway_test.moc"
