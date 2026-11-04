#include <QtTest>
#include <QImage>
#include "BroadcastEncoder.h"
#include "BroadcastPacket.h"
#include "StreamDecoder.h"
#include "VideoPacket.h"
#include <sodium.h>

class BroadcastEncoderTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase(){QVERIFY(sodium_init()>=0);}
    void generatedScreen_data(){QTest::addColumn<QSize>("source");QTest::addColumn<QSize>("expected");QTest::addColumn<int>("height");QTest::newRow("normal")<<QSize(320,180)<<QSize(320,180)<<720;QTest::newRow("ultrawide")<<QSize(5120,1440)<<QSize(2560,720)<<720;QTest::newRow("4k")<<QSize(3840,2160)<<QSize(3840,2160)<<2160;}
    void generatedScreen(){
        if(!BroadcastEncoder::available())QSKIP("libx264 is unavailable");
        QFETCH(QSize,source);QFETCH(QSize,expected);QFETCH(int,height);
        BroadcastEncoder encoder(height,30,30000000);QImage image(source,QImage::Format_RGB32);image.fill(QColor(210,40,30));
        encoder.submit(QVideoFrame(image));BroadcastEncoder::Snapshot snapshot;
        QTRY_VERIFY_WITH_TIMEOUT((!snapshot.frames.isEmpty() || !snapshot.error.isEmpty() || (snapshot=encoder.take(),!snapshot.frames.isEmpty() || !snapshot.error.isEmpty())),5000);
        QVERIFY2(snapshot.error.isEmpty(),qPrintable(snapshot.error));const auto frame=snapshot.frames.first();QCOMPARE(QSize(frame.width,frame.height),expected);QVERIFY(frame.keyframe);
        VideoPacket::Depacketizer depacketizer(VideoPacket::Depacketizer::H264);const auto payloads=BroadcastPacket::h264Payloads(frame.bytes);QVERIFY(!payloads.isEmpty());
        QByteArray rebuilt;quint16 sequence=65530;quint32 nonce=0x80000000;const QByteArray key(32,'K');
        for(qsizetype i=0;i<payloads.size();++i){
            const auto wire=BroadcastPacket::encrypt(payloads[i],key,105,i+1==payloads.size(),sequence++,frame.timestamp,202,nonce++);
            QVERIFY(wire.size()<=1200);const auto decoded=VideoPacket::decryptRtp(wire,key);QVERIFY(decoded);QCOMPARE(decoded->payload,payloads[i]);
            if(auto whole=depacketizer.push(decoded->payload,decoded->marker,decoded->sequence,decoded->timestamp))rebuilt=*whole;
        }
        QCOMPARE(rebuilt,frame.bytes);StreamDecoder decoder("H264");QVERIFY(decoder.submit(rebuilt,frame.timestamp));StreamDecoder::Snapshot result;
        QTRY_VERIFY_WITH_TIMEOUT((result.frame.isValid() || (result=decoder.take(),result.frame.isValid())),5000);QCOMPARE(QSize(result.width,result.height),expected);
        const auto color=result.frame.toImage().pixelColor(expected.width()/2,expected.height()/2);QVERIFY(color.red()>150);QVERIFY(color.blue()<80);
        snapshot={};encoder.requestKeyframe();QTRY_VERIFY_WITH_TIMEOUT((!snapshot.frames.isEmpty() || (snapshot=encoder.take(),!snapshot.frames.isEmpty())),3000);QVERIFY(snapshot.frames.last().keyframe);
    }
    void smallBandwidthUpdatesPreservePredictiveFrames(){
        if(!BroadcastEncoder::available())QSKIP("libx264 is unavailable");
        BroadcastEncoder encoder(720,30,2000000);StreamDecoder decoder("H264");
        QImage image(QSize(320,180),QImage::Format_RGB32);
        quint32 previousStamp=0;bool haveStamp=false;quint64 offered=0;
        int jitterKeyframes=0,predictive=0;
        StreamDecoder::Snapshot decoded;
        for(int step=0;step<34;++step){
            if(step==1)encoder.setBitrate(1000000);
            else if(step>=2 && step<32)encoder.setBitrate(1000000-(step%10)*1000);
            else if(step==32)encoder.setBitrate(800000);
            else if(step==33)encoder.setBitrate(801000);
            image.fill(QColor(110+step*2,50+step,80));
            encoder.submit(QVideoFrame(image));
            BroadcastEncoder::Snapshot output;
            QTRY_VERIFY_WITH_TIMEOUT((!output.frames.isEmpty() || !output.error.isEmpty()
                || (output=encoder.take(),!output.frames.isEmpty() || !output.error.isEmpty())),2000);
            QVERIFY2(output.error.isEmpty(),qPrintable(output.error));QVERIFY(!output.frames.isEmpty());
            if(step==1 || step==32)QVERIFY(output.frames.first().keyframe);
            for(const auto &frame:output.frames){
                if(haveStamp)QVERIFY(qint32(frame.timestamp-previousStamp)>0);
                previousStamp=frame.timestamp;haveStamp=true;
                if(step>=1 && step<32){if(frame.keyframe)++jitterKeyframes;else ++predictive;}
                QVERIFY(decoder.submit(frame.bytes,frame.timestamp));++offered;
            }
            QTRY_VERIFY_WITH_TIMEOUT((decoded.decoded>=offered
                || (decoded=decoder.take(),decoded.decoded>=offered)),2000);
            QVERIFY2(decoded.error.isEmpty(),qPrintable(decoded.error));
            QCOMPARE(decoded.dropped,quint64(0));
        }
        QCOMPARE(jitterKeyframes,1);QVERIFY(predictive>=30);QCOMPARE(decoded.decoded,offered);
        qInfo("BandwidthJitter frames=%llu predictive=%d keyframes=%d",qulonglong(offered),predictive,jitterKeyframes);
    }
    void malformedPacketInput(){QVERIFY(BroadcastPacket::h264Payloads("not h264").isEmpty());QVERIFY(BroadcastPacket::encrypt("test",QByteArray(31,'K'),105,true,1,1,1,1).isEmpty());}
};
QTEST_GUILESS_MAIN(BroadcastEncoderTest)
#include "broadcast_encoder_test.moc"
