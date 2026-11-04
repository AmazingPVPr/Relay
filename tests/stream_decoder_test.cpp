#include <QtTest>
#include <QImage>
#include "StreamDecoder.h"
#include "StreamTestMedia.h"

namespace {
QList<QByteArray> predictedSequence(bool fullRange = false) {
    const auto codec=avcodec_find_encoder_by_name("libx264");if(!codec)return {};
    auto encoder=avcodec_alloc_context3(codec);if(!encoder)return {};
    encoder->width=1920;encoder->height=1080;encoder->pix_fmt=AV_PIX_FMT_YUV420P;
    encoder->time_base={1,60};encoder->framerate={60,1};encoder->bit_rate=12000000;
    encoder->gop_size=120;encoder->max_b_frames=0;encoder->thread_count=2;
    encoder->colorspace=AVCOL_SPC_BT709;encoder->color_trc=AVCOL_TRC_BT709;
    encoder->color_primaries=AVCOL_PRI_BT709;encoder->color_range=fullRange?AVCOL_RANGE_JPEG:AVCOL_RANGE_MPEG;
    av_opt_set(encoder->priv_data,"preset","ultrafast",0);av_opt_set(encoder->priv_data,"tune","zerolatency",0);
    av_opt_set(encoder->priv_data,"x264-params","scenecut=0",0);
    QList<QByteArray> result;auto frame=av_frame_alloc();auto packet=av_packet_alloc();
    if(frame && packet && avcodec_open2(encoder,codec,nullptr)>=0){
        frame->format=encoder->pix_fmt;frame->width=encoder->width;frame->height=encoder->height;
        frame->colorspace=encoder->colorspace;frame->color_trc=encoder->color_trc;
        frame->color_primaries=encoder->color_primaries;frame->color_range=encoder->color_range;
        if(av_frame_get_buffer(frame,32)>=0)for(int n=0;n<10;++n){
            av_frame_make_writable(frame);frame->pts=n;
            for(int y=0;y<frame->height;++y)std::memset(frame->data[0]+y*frame->linesize[0],fullRange?0:96+n,frame->width);
            for(int y=0;y<frame->height/2;++y){
                std::memset(frame->data[1]+y*frame->linesize[1],128,frame->width/2);
                std::memset(frame->data[2]+y*frame->linesize[2],128,frame->width/2);
            }
            if(avcodec_send_frame(encoder,frame)<0)break;
            while(avcodec_receive_packet(encoder,packet)>=0){
                result.append(QByteArray(reinterpret_cast<char*>(packet->data),packet->size));av_packet_unref(packet);
            }
        }
    }
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&encoder);return result;
}
}

class StreamDecoderTest final : public QObject {
    Q_OBJECT
private slots:
    void decode_data() {
        QTest::addColumn<QString>("codec");QTest::addColumn<int>("width");QTest::addColumn<int>("height");
        QTest::newRow("H264")<<QString("H264")<<320<<180;
        QTest::newRow("VP8")<<QString("VP8")<<320<<180;
        QTest::newRow("H265")<<QString("H265")<<320<<180;
        QTest::newRow("AV1")<<QString("AV1")<<320<<180;
        QTest::newRow("ultrawide-native-resolution")<<QString("H264")<<5120<<1440;
    }
    void decode() {
        QFETCH(QString,codec);QFETCH(int,width);QFETCH(int,height);
        const auto encoded=StreamTestMedia::encodedFrame(codec,width,height);QVERIFY2(!encoded.isEmpty(),"Test encoder unavailable");
        StreamDecoder decoder(codec);QVERIFY(decoder.submit(encoded,0));
        StreamDecoder::Snapshot snapshot;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{auto current=decoder.take();if(current.frame.isValid())snapshot=std::move(current);return snapshot.frame.isValid();})(),4000);
        QVERIFY(snapshot.error.isEmpty());QCOMPARE(snapshot.width,width);QCOMPARE(snapshot.height,height);QCOMPARE(snapshot.decoded,quint64(1));
        const auto image=snapshot.frame.toImage();QVERIFY(!image.isNull());QCOMPARE(image.size(),QSize(width,height));
        const auto pixel=image.pixelColor(width/2,height/2);QVERIFY(pixel.red()>pixel.blue());
    }
    void modernCodecRecovery_data() {
        QTest::addColumn<QString>("codec");
        QTest::newRow("hevc-idr-recovery")<<QString("H265");
        QTest::newRow("av1-keyframe-recovery")<<QString("AV1");
    }
    void modernCodecRecovery() {
        QFETCH(QString,codec);QVERIFY(StreamDecoder::supports(codec));
        const auto frames=StreamTestMedia::encodedFrames(codec,4,320,180,30);QCOMPARE(frames.size(),4);
        StreamDecoder decoder(codec);decoder.discardUntilKeyFrame();QVERIFY(decoder.submit(frames[0],0));
        QTRY_COMPARE_WITH_TIMEOUT(decoder.take().decoded,quint64(1),4000);
        for(int i=1;i<4;++i)QVERIFY(decoder.submit(frames[i],quint32(i*3000)));
        QTRY_COMPARE_WITH_TIMEOUT(decoder.take().decoded,quint64(4),4000);
        decoder.discardUntilKeyFrame();QVERIFY(!decoder.submit(frames[2],12000));QVERIFY(decoder.take().keyFrameNeeded);
        QVERIFY(decoder.submit(frames[0],15000));QTRY_COMPARE_WITH_TIMEOUT(decoder.take().decoded,quint64(5),4000);
        QVERIFY(!decoder.take().keyFrameNeeded);
        QVERIFY(!StreamDecoder::supports("UNKNOWN"));
    }
    void rejectsMalformedAv1Framing() {
        StreamDecoder decoder("AV1");
        for(const auto &frame:{"00","32ff","32ffffffffffffffff01","3401","32ff00aa"})
            QVERIFY(!decoder.submit(QByteArray::fromHex(frame),0));
    }
    void rejectsOversizedFrame() {
        StreamDecoder decoder("VP8");QVERIFY(!decoder.submit({},0));QVERIFY(!decoder.submit(QByteArray(8*1024*1024+1,'x'),0));
    }
    void queuedPredictedFramesKeepReferences() {
        const auto frames=predictedSequence();QCOMPARE(frames.size(),10);
        StreamDecoder decoder("H264");for(qsizetype i=0;i<frames.size();++i)QVERIFY(decoder.submit(frames[i],quint32(i*1500)));
        StreamDecoder::Snapshot result;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{auto current=decoder.take();if(current.frame.isValid())result=current;return current.decoded==10;})(),5000);
        QCOMPARE(result.decoded,quint64(10));QCOMPARE(result.dropped,quint64(0));QVERIFY(!result.keyFrameNeeded);
        QCOMPARE(result.frame.size(),QSize(1920,1080));
        QVERIFY(result.frame.map(QVideoFrame::ReadOnly));QCOMPARE(result.frame.bits(0)[540*result.frame.bytesPerLine(0)+960],quint8(105));result.frame.unmap();
    }
    void missingReferencesWaitForKeyFrame() {
        const auto frames=predictedSequence();QCOMPARE(frames.size(),10);
        StreamDecoder decoder("H264");QVERIFY(decoder.submit(frames[0],0));
        QTRY_COMPARE_WITH_TIMEOUT(decoder.take().decoded,quint64(1),4000);
        decoder.discardUntilKeyFrame();QVERIFY(decoder.take().keyFrameNeeded);
        QVERIFY(!decoder.submit(frames[1],1500));QVERIFY(decoder.take().keyFrameNeeded);
        QVERIFY(decoder.submit(frames[0],9000));StreamDecoder::Snapshot result;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{auto current=decoder.take();if(current.frame.isValid())result=current;return current.decoded==2;})(),4000);
        QVERIFY(!result.keyFrameNeeded);QCOMPARE(result.width,1920);QVERIFY(result.error.isEmpty());
    }
    void queueOverflowRecoversAtKeyFrame() {
        const auto frames=predictedSequence();QCOMPARE(frames.size(),10);StreamDecoder decoder("H264");
        QVERIFY(decoder.submit(frames[0],0));
        for(int i=0;i<2000;++i)decoder.submit(frames[1],quint32((i+1)*1500));
        const auto overflow=decoder.take();QVERIFY(overflow.dropped>0);QVERIFY(overflow.keyFrameNeeded);
        QVERIFY(!decoder.submit(frames[2],3000001));
        // Let the one already-running decode finish before supplying a new IDR.
        QTest::qWait(100);const auto before=decoder.take().decoded;
        QVERIFY(decoder.submit(frames[0],3001500));StreamDecoder::Snapshot result;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{auto current=decoder.take();if(current.frame.isValid())result=current;return current.decoded>before;})(),4000);
        QVERIFY(result.frame.isValid());QVERIFY(!result.keyFrameNeeded);QVERIFY(result.error.isEmpty());
    }
    void preservesColorMetadata_data() {
        QTest::addColumn<bool>("fullRange");QTest::newRow("limited-bt709")<<false;QTest::newRow("full-bt709")<<true;
    }
    void preservesColorMetadata() {
        QFETCH(bool,fullRange);const auto frames=predictedSequence(fullRange);QCOMPARE(frames.size(),10);
        StreamDecoder decoder("H264");QVERIFY(decoder.submit(frames[0],0));StreamDecoder::Snapshot result;
        QTRY_VERIFY_WITH_TIMEOUT((result.frame.isValid() || (result=decoder.take(),result.frame.isValid())),4000);
        QCOMPARE(result.frame.surfaceFormat().colorSpace(),QVideoFrameFormat::ColorSpace_BT709);
        QCOMPARE(result.frame.surfaceFormat().colorTransfer(),QVideoFrameFormat::ColorTransfer_BT709);
        QCOMPARE(result.frame.surfaceFormat().colorRange(),fullRange?QVideoFrameFormat::ColorRange_Full:QVideoFrameFormat::ColorRange_Video);
        QVERIFY(result.frame.map(QVideoFrame::ReadOnly));QCOMPARE(result.frame.bits(0)[540*result.frame.bytesPerLine(0)+960],quint8(fullRange?0:96));result.frame.unmap();
    }
};
QTEST_GUILESS_MAIN(StreamDecoderTest)
#include "stream_decoder_test.moc"
