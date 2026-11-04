#include <QtTest>
#include <QtEndian>
#include <sodium.h>
#include "VoicePacket.h"
#include "VoiceEngine.h"
#include <array>

class VoicePacketTest: public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){QVERIFY(sodium_init()>=0);}
    void nativeUdpKeepalive(){
        QCOMPARE(VoicePacket::nativePing(0x01020304).toHex(),QByteArray("1337cafe04030201"));
        const auto pong=VoicePacket::parseNativePong(QByteArray::fromHex("1337f00d04030201"));
        QVERIFY(pong);QCOMPARE(*pong,quint32(0x01020304));
        QVERIFY(!VoicePacket::parseNativePong(VoicePacket::nativePing(1)));
        QVERIFY(!VoicePacket::parseNativePong(QByteArray::fromHex("1337f00d040302")));
        QVERIFY(!VoicePacket::parseNativePong(QByteArray(8,'\0')));
    }
    void discovery(){
        auto request=VoicePacket::discoveryRequest(0x10203040);
        QCOMPARE(request.size(),74);QCOMPARE(request.left(8).toHex(),QByteArray("0001004610203040"));
        auto response=request;response[1]=2;const QByteArray ip="203.0.113.7";
        std::copy(ip.begin(),ip.end(),response.begin()+8);qToBigEndian<quint16>(50050,response.data()+72);
        auto found=VoicePacket::parseDiscovery(response,0x10203040);QVERIFY(found);QCOMPARE(found->address,QString("203.0.113.7"));QCOMPARE(found->port,quint16(50050));
        QVERIFY(!VoicePacket::parseDiscovery(response,1));QVERIFY(!VoicePacket::parseDiscovery(response.left(73),0x10203040));
        response.replace(8,64,QByteArray(64,'x'));QVERIFY(!VoicePacket::parseDiscovery(response,0x10203040));
    }
    void authenticatedRoundTrip(){
        QByteArray key(32,'k'),payload=QByteArray::fromHex("f8fffe00112233445566778899");
        auto packet=VoicePacket::encrypt(payload,key,65535,0xfffffffe,0x12345678,0x01020304);
        QCOMPARE(packet.left(12).toHex(),QByteArray("8078fffffffffffe12345678"));QCOMPARE(packet.right(4).toHex(),QByteArray("01020304"));
        auto decoded=VoicePacket::decrypt(packet,key);QVERIFY(decoded);QCOMPARE(decoded->payload,payload);QCOMPARE(decoded->sequence,quint16(65535));QCOMPARE(decoded->ssrc,quint32(0x12345678));QCOMPARE(decoded->nonce,quint32(0x01020304));
        for(int i=0;i<packet.size();++i){auto tampered=packet;tampered[i]=char(quint8(tampered[i])^0x01);QVERIFY2(!VoicePacket::decrypt(tampered,key),qPrintable(QString("Unauthenticated byte %1 accepted").arg(i)));}
        QVERIFY(!VoicePacket::decrypt(packet,QByteArray(32,'q')));
        QVERIFY(VoicePacket::encrypt(payload,QByteArray(31,'k'),0,0,0,0).isEmpty());
    }
    void stripsAuthenticatedExtension(){
        QByteArray key(32,'z');QByteArray header=QByteArray::fromHex("91780002000003c00000006300000100bede0001");
        QByteArray plain=QByteArray::fromHex("10000000aabbccdd");
        QByteArray packet=header;packet.resize(header.size()+plain.size()+16+4);
        std::array<unsigned char,24> nonce{};qToBigEndian<quint32>(42,nonce.data());unsigned long long size=0;
        QCOMPARE(crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(packet.data()+header.size()),&size,
            reinterpret_cast<const unsigned char*>(plain.constData()),plain.size(),reinterpret_cast<const unsigned char*>(header.constData()),header.size(),nullptr,nonce.data(),reinterpret_cast<const unsigned char*>(key.constData())),0);
        qToBigEndian<quint32>(42,packet.data()+packet.size()-4);
        const auto decoded=VoicePacket::decrypt(packet,key);QVERIFY(decoded);QCOMPARE(decoded->payload,QByteArray::fromHex("aabbccdd"));
        // Extension preamble participates in AEAD associated data.
        packet[19]=2;QVERIFY(!VoicePacket::decrypt(packet,key));
    }
    void malformedPackets(){
        const QByteArray key(32,'x');for(int n=0;n<64;n++){QByteArray p(n,'\0');QVERIFY(!VoicePacket::decrypt(p,key));}
        auto p=VoicePacket::encrypt("frame",key,0,0,1,0);p[0]=char(0x9f);QVERIFY(!VoicePacket::decrypt(p,key));
    }
    void rejectsForeignVoiceEndpoint(){
        VoiceEngine voice;
        if(!voice.available())QSKIP("DAVE not compiled into this build");
        QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        voice.join("111","222","Fixture channel","333");
        QTRY_COMPARE(requests.count(),1);
        voice.onVoiceState({{"guild_id","111"},{"channel_id","222"},{"user_id","333"},{"session_id","fixture-session"}});
        voice.onVoiceServer({{"guild_id","111"},{"token","fixture-only"},{"endpoint","voice.attacker.invalid"}});
        QVERIFY(!voice.connected());QVERIFY(voice.error().contains("invalid voice endpoint"));
        QTRY_COMPARE(requests.count(),2);QVERIFY(requests.last().at(1).toString().isEmpty());
        QVERIFY(!voice.micTesting());QCOMPARE(voice.inputLevel(),0.0);
    }
    void noCaptureUntilRequested(){
        VoiceEngine voice;QVERIFY(!voice.connected());QVERIFY(!voice.micTesting());QCOMPARE(voice.inputLevel(),0.0);
        QSignalSpy requests(&voice,&VoiceEngine::voiceStateRequested);
        voice.join(QString(),QString(),QString(),QString());QCOMPARE(requests.count(),0);QVERIFY(!voice.error().isEmpty());
        voice.leave();QVERIFY(voice.error().isEmpty());QVERIFY(!voice.micTesting());
        voice.setMuted(true);voice.setDeafened(true);QVERIFY(voice.muted());QVERIFY(voice.deafened());QCOMPARE(requests.count(),0);
    }
};
QTEST_GUILESS_MAIN(VoicePacketTest)
#include "voice_packet_test.moc"
