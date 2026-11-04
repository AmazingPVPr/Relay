#include "VideoPacket.h"

#include <QtEndian>
#include <QtTest>
#include <sodium.h>
#include <array>
#include <cstring>

class VideoPacketTest final : public QObject {
    Q_OBJECT
    static QByteArray key() {
        QByteArray result(32, Qt::Uninitialized);
        for (int i = 0; i < 32; ++i) result[i] = char(i);
        return result;
    }
    // Independent transport fixture builder. No production serialization helper.
    static QByteArray seal(const QByteArray &header, const QByteArray &body, const QByteArray &suffix = QByteArray::fromHex("01020304")) {
        std::array<unsigned char, 24> nonce{};
        std::memcpy(nonce.data(), suffix.constData(), 4);
        QByteArray ciphertext(body.size() + 16, Qt::Uninitialized);
        unsigned long long size = 0;
        crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char *>(ciphertext.data()), &size,
            reinterpret_cast<const unsigned char *>(body.constData()), body.size(),
            reinterpret_cast<const unsigned char *>(header.constData()), header.size(), nullptr,
            nonce.data(), reinterpret_cast<const unsigned char *>(key().constData()));
        ciphertext.resize(qsizetype(size));
        return header + ciphertext + suffix;
    }
private slots:
    void initTestCase() { QVERIFY(sodium_init() >= 0); }
    void fixedRtpVectorAuthenticatesAndStripsMetadata() {
        // Produced independently with Python ctypes/libsodium, key bytes 00..1f.
        const auto packet = QByteArray::fromHex("b1e50009000023280000002a00000066bede00014cda06dcb1f03516dbf1fd8ccaa494e28d7303f766c18f96a834fa00010000");
        const auto parsed = VideoPacket::decryptRtp(packet, key());
        QVERIFY(parsed);
        QCOMPARE(parsed->payloadType, quint8(101));
        QVERIFY(parsed->marker);
        QCOMPARE(parsed->sequence, quint16(9));
        QCOMPARE(parsed->timestamp, quint32(9000));
        QCOMPARE(parsed->ssrc, quint32(42));
        QCOMPARE(parsed->payload, QByteArray::fromHex("65aabb"));
        QCOMPARE(parsed->nonce, quint32(256));
        for (int i = 0; i < packet.size(); ++i) {
            auto changed = packet;
            changed[i] = char(quint8(changed[i]) ^ 1);
            QVERIFY2(!VideoPacket::decryptRtp(changed, key()), qPrintable(QString("Tampered byte %1 accepted").arg(i)));
        }
        QVERIFY(!VideoPacket::decryptRtp(packet, QByteArray(32, 'x')));
    }
    void arbitraryNonceByteOrderDoesNotChangeAuthentication() {
        const auto header = QByteArray::fromHex("80e50009000023280000002a");
        const auto body = QByteArray::fromHex("65aabb");
        for (const auto &suffix : {QByteArray::fromHex("000000ff"), QByteArray::fromHex("00000100"),
                                   QByteArray::fromHex("ff000000"), QByteArray::fromHex("00010000")}) {
            const auto parsed = VideoPacket::decryptRtp(seal(header, body, suffix), key());
            QVERIFY(parsed);
            QCOMPARE(parsed->payload, body);
        }
    }
    void authenticatedCameraOrientation_data() {
        QTest::addColumn<int>("profile");QTest::addColumn<int>("value");QTest::addColumn<int>("rotation");QTest::addColumn<bool>("mirror");
        QTest::newRow("landscape")<<0xbede<<0<<0<<false;
        QTest::newRow("portrait-clockwise")<<0xbede<<1<<90<<false;
        QTest::newRow("upside-down-front")<<0xbede<<10<<180<<false;
        QTest::newRow("portrait-mirrored")<<0xbede<<7<<270<<true;
        QTest::newRow("two-byte-portrait-front")<<0x1000<<9<<90<<false;
        QTest::newRow("two-byte-app-bits-mirrored")<<0x100a<<5<<90<<true;
    }
    void authenticatedCameraOrientation() {
        QFETCH(int,profile);QFETCH(int,value);QFETCH(int,rotation);QFETCH(bool,mirror);
        QByteArray header=QByteArray::fromHex("90e50009000023280000002a00000001");qToBigEndian<quint16>(quint16(profile),header.data()+12);
        QByteArray extension;
        if(profile==0xbede)extension=QByteArray::fromHex("40000000");
        else extension=QByteArray::fromHex("04010000");
        extension[profile==0xbede?1:2]=char(value);
        const auto parsed=VideoPacket::decryptRtp(seal(header,extension+QByteArray::fromHex("65aabb")),key());QVERIFY(parsed);
        QVERIFY(parsed->hasOrientation);QCOMPARE(parsed->rotation,rotation);QCOMPARE(parsed->mirrored,mirror);QCOMPARE(parsed->payload,QByteArray::fromHex("65aabb"));
        auto changed=seal(header,extension+QByteArray::fromHex("65aabb"));changed[17]^=1;QVERIFY(!VideoPacket::decryptRtp(changed,key()));
    }
    void cameraOrientationBoundsAndUnknownProfiles() {
        const auto header=QByteArray::fromHex("90e50009000023280000002abede0001");
        // The CVO value is inside the extension area, never in media bytes.
        QVERIFY(!VideoPacket::decryptRtp(seal(header,QByteArray::fromHex("43aabbcc65aabb")),key()));
        const auto reserved=VideoPacket::decryptRtp(seal(header,QByteArray::fromHex("40f1000065aabb")),key());QVERIFY(reserved);QVERIFY(!reserved->hasOrientation);
        auto twoByte=header;qToBigEndian<quint16>(0x1000,twoByte.data()+12);
        QVERIFY(!VideoPacket::decryptRtp(seal(twoByte,QByteArray::fromHex("0404000065aabb")),key()));
        auto unknown=header;qToBigEndian<quint16>(0xabcd,unknown.data()+12);
        const auto parsed=VideoPacket::decryptRtp(seal(unknown,QByteArray::fromHex("4001000065aabb")),key());QVERIFY(parsed);QVERIFY(!parsed->hasOrientation);QCOMPARE(parsed->payload,QByteArray::fromHex("65aabb"));
    }
    void malformedAuthenticatedRtpIsRejected() {
        const auto extension = QByteArray::fromHex("90e50009000023280000002abede0002");
        QVERIFY(!VideoPacket::decryptRtp(seal(extension, QByteArray::fromHex("1000000065aa")), key()));
        const auto padded = QByteArray::fromHex("a0e50009000023280000002a");
        QVERIFY(!VideoPacket::decryptRtp(seal(padded, QByteArray::fromHex("65aa00")), key()));
        QVERIFY(!VideoPacket::decryptRtp(seal(padded, QByteArray::fromHex("65aa05")), key()));
        for (int n = 0; n < 32; ++n) QVERIFY(!VideoPacket::decryptRtp(QByteArray(n, '\0'), key()));
        QVERIFY(!VideoPacket::decryptRtp(seal(QByteArray::fromHex("80e50009000023280000002a"), {}), key()));
    }
    void rtcpMatchesIndependentFixedVectorAndAuthenticates() {
        const auto clear = QByteArray::fromHex("81ce00020000002a00000063");
        const auto expected = QByteArray::fromHex("81ce00020000002aa43f8372f1f665ac70a9e58509b51ce7d151d93204030201");
        QCOMPARE(VideoPacket::encryptRtcp(clear, key(), 0x01020304), expected);
        const auto plain = VideoPacket::decryptRtcp(expected, key());
        QVERIFY(plain);
        QCOMPARE(*plain, clear);
        for (int i = 0; i < expected.size(); ++i) {
            auto changed = expected;
            changed[i] = char(quint8(changed[i]) ^ 1);
            QVERIFY(!VideoPacket::decryptRtcp(changed, key()));
        }
        QVERIFY(!VideoPacket::decryptRtp(expected, key()));
    }
    void compoundRtcpAndInvalidLengths() {
        const auto clear = QByteArray::fromHex("80c900010000002a81ce00020000002a00000063");
        const auto packet = VideoPacket::encryptRtcp(clear, key(), 3);
        QVERIFY(!packet.isEmpty());
        const auto decoded = VideoPacket::decryptRtcp(packet, key());
        QVERIFY(decoded);
        QCOMPARE(*decoded, clear);
        auto invalid = clear;
        invalid[11] = char(10); // Second RTCP block claims more bytes than available.
        QVERIFY(VideoPacket::encryptRtcp(invalid, key(), 4).isEmpty());
        QVERIFY(!VideoPacket::decryptRtcp(seal(invalid.left(8), invalid.mid(8)), key()));
        QVERIFY(VideoPacket::encryptRtcp(clear, QByteArray(31, 'a'), 5).isEmpty());
    }
    void h264SingleAndAggregationProduceAnnexB() {
        VideoPacket::Depacketizer parser(VideoPacket::Depacketizer::H264);
        auto result = parser.push(QByteArray::fromHex("65aabb"), true, 1, 9000);
        QVERIFY(result);
        QCOMPARE(*result, QByteArray::fromHex("0000000165aabb"));
        result = parser.push(QByteArray::fromHex("780003674200000268cc"), true, 2, 12000);
        QVERIFY(result);
        QCOMPARE(*result, QByteArray::fromHex("000000016742000000000168cc"));
    }
    void h264FragmentsAndMultipleNalusAssembleOneFrame() {
        VideoPacket::Depacketizer parser(VideoPacket::Depacketizer::H264);
        QVERIFY(!parser.push(QByteArray::fromHex("674200"), false, 65534, 9000));
        QVERIFY(!parser.push(QByteArray::fromHex("7c85aabb"), false, 65535, 9000));
        QVERIFY(!parser.push(QByteArray::fromHex("7c05ccdd"), false, 0, 9000));
        const auto result = parser.push(QByteArray::fromHex("7c45eeff"), true, 1, 9000);
        QVERIFY(result);
        QCOMPARE(*result, QByteArray::fromHex("000000016742000000000165aabbccddeeff"));
    }
    void missingFragmentDropsFrameAndNextTimestampRecovers() {
        VideoPacket::Depacketizer parser(VideoPacket::Depacketizer::H264);
        QVERIFY(!parser.push(QByteArray::fromHex("7c85aabb"), false, 1, 9000));
        QVERIFY(!parser.push(QByteArray::fromHex("7c45eeff"), true, 3, 9000));
        const auto result = parser.push(QByteArray::fromHex("65aabb"), true, 4, 12000);
        QVERIFY(result);
        QCOMPARE(*result, QByteArray::fromHex("0000000165aabb"));
    }
    void duplicateAndLatePacketsDoNotDuplicateOrPoisonFrames() {
        VideoPacket::Depacketizer parser(VideoPacket::Depacketizer::H264);
        QVERIFY(!parser.push(QByteArray::fromHex("7c85aa"), false, 10, 9000));
        QVERIFY(!parser.push(QByteArray::fromHex("7c85aa"), false, 10, 9000));
        QVERIFY(!parser.push(QByteArray::fromHex("65ff"), true, 9, 6000));
        const auto result = parser.push(QByteArray::fromHex("7c45bb"), true, 11, 9000);
        QVERIFY(result);
        QCOMPARE(*result, QByteArray::fromHex("0000000165aabb"));
        QVERIFY(!parser.push(QByteArray::fromHex("7c45bb"), true, 11, 9000));
    }
    void h264MalformedFragmentsAndAggregatesAreDropped() {
        for (const auto &invalid : {"7c45aa", "7cc5aa", "7ca5aa", "78000567", "780000", "f8aabb", "790000"}) {
            VideoPacket::Depacketizer parser(VideoPacket::Depacketizer::H264);
            QVERIFY2(!parser.push(QByteArray::fromHex(invalid), true, 1, 9000), invalid);
        }
        VideoPacket::Depacketizer parser(VideoPacket::Depacketizer::H264);
        QVERIFY(!parser.push(QByteArray::fromHex("7c85aa"), false, 1, 9000));
        QVERIFY(!parser.push(QByteArray::fromHex("7c41bb"), true, 2, 9000)); // NAL type changed mid-fragment.
        parser.reset();
        QVERIFY(!parser.push(QByteArray::fromHex("7c85aa"), true, 1, 9000)); // Marker before FU end.
    }
    void vp8AllDescriptorExtensionsAreRemoved() {
        VideoPacket::Depacketizer parser(VideoPacket::Depacketizer::VP8);
        // X,S,PID0; I,L,T,K; 15-bit PictureID; TL0PICIDX; TID/KEYIDX.
        QVERIFY(!parser.push(QByteArray::fromHex("90f081230740aabb"), false, 1, 9000));
        const auto result = parser.push(QByteArray::fromHex("0088ccdd"), true, 2, 9000);
        QVERIFY(result);
        QCOMPARE(*result, QByteArray::fromHex("aabb88ccdd"));
    }
    void vp8PartitionsAndStartRequirements() {
        VideoPacket::Depacketizer parser(VideoPacket::Depacketizer::VP8);
        QVERIFY(!parser.push(QByteArray::fromHex("10aabb"), false, 1, 9000));
        const auto result = parser.push(QByteArray::fromHex("11ccdd"), true, 2, 9000);
        QVERIFY(result);
        QCOMPARE(*result, QByteArray::fromHex("aabbccdd"));
        for (const auto &invalid : {"00aabb", "11aabb", "90", "9080", "908080", "90f0812307", "1faabb"}) {
            parser.reset();
            QVERIFY2(!parser.push(QByteArray::fromHex(invalid), true, 1, 9000), invalid);
        }
    }
    void hevcSinglesAggregatesAndFragments() {
        using D=VideoPacket::Depacketizer;D parser(D::H265);
        auto result=parser.push(QByteArray::fromHex("2601aabb"),true,65534,9000);QVERIFY(result);
        QCOMPARE(*result,QByteArray::fromHex("000000012601aabb"));
        result=parser.push(QByteArray::fromHex("600100034001aa00034201bb"),true,65535,12000);QVERIFY(result);
        QCOMPARE(*result,QByteArray::fromHex("000000014001aa000000014201bb"));
        QVERIFY(!parser.push(QByteArray::fromHex("620193aabb"),false,0,15000));
        QVERIFY(!parser.push(QByteArray::fromHex("620113cc"),false,1,15000));
        result=parser.push(QByteArray::fromHex("620153dd"),true,2,15000);QVERIFY(result);
        QCOMPARE(*result,QByteArray::fromHex("000000012601aabbccdd"));
    }
    void hevcMalformedPayloadsAndLossAreRejected() {
        using D=VideoPacket::Depacketizer;
        for(const auto &invalid:{"26","a601aa","2600aa","6001","6001000140","600100034001aa",
                                "600100054001aa00034201bb","620153aa","6201d3aa","6201b0aa","6401aa"}){
            D parser(D::H265);QVERIFY2(!parser.push(QByteArray::fromHex(invalid),true,1,9000),invalid);
        }
        D parser(D::H265);QVERIFY(!parser.push(QByteArray::fromHex("620193aa"),false,1,9000));
        QVERIFY(!parser.push(QByteArray::fromHex("620253bb"),true,2,9000)); // Header layer/TID changed.
        QVERIFY(parser.push(QByteArray::fromHex("2601cc"),true,3,12000));
        parser.reset();QVERIFY(!parser.push(QByteArray::fromHex("620193aa"),false,1,9000));
        QVERIFY(!parser.push(QByteArray::fromHex("620153bb"),true,3,9000));
        QVERIFY(parser.push(QByteArray::fromHex("2601cc"),true,4,12000));
    }
    void av1AggregationModesRestoreDaveFraming_data() {
        QTest::addColumn<QByteArray>("packet");QTest::addColumn<QByteArray>("frame");
        // Final OBU has no size field: libdave authenticates this exact header.
        QTest::newRow("w1-single")<<QByteArray::fromHex("1830aabb")<<QByteArray::fromHex("30aabb");
        QTest::newRow("w2-sequence-and-frame")<<QByteArray::fromHex("280208aa30bbcc")<<QByteArray::fromHex("0a01aa30bbcc");
        QTest::newRow("w3-with-extension")<<QByteArray::fromHex("380208aa031420bb30cc")<<QByteArray::fromHex("0a01aa162001bb30cc");
        QTest::newRow("w0-all-explicit")<<QByteArray::fromHex("000208aa0330bbcc")<<QByteArray::fromHex("0a01aa30bbcc");
        QTest::newRow("padded-obu-length")<<QByteArray::fromHex("100a8100aa")<<QByteArray::fromHex("08aa");
    }
    void av1AggregationModesRestoreDaveFraming() {
        QFETCH(QByteArray,packet);QFETCH(QByteArray,frame);VideoPacket::Depacketizer parser(VideoPacket::Depacketizer::AV1);
        const auto result=parser.push(packet,true,1,9000);QVERIFY(result);QCOMPARE(*result,frame);
    }
    void av1FragmentsCrossPacketsAndRecoverAfterLoss() {
        using D=VideoPacket::Depacketizer;D parser(D::AV1);
        QVERIFY(!parser.push(QByteArray::fromHex("5030aa"),false,65535,9000));
        QVERIFY(!parser.push(QByteArray::fromHex("d0bbcc"),false,0,9000));
        const auto result=parser.push(QByteArray::fromHex("90dd"),true,1,9000);QVERIFY(result);
        QCOMPARE(*result,QByteArray::fromHex("30aabbccdd"));
        parser.reset();QVERIFY(!parser.push(QByteArray::fromHex("5030aa"),false,1,9000));
        QVERIFY(!parser.push(QByteArray::fromHex("90bb"),true,3,9000));
        QVERIFY(parser.push(QByteArray::fromHex("1030cc"),true,4,12000));
        // A temporal unit can contain a complete OBU, a fragment, and another OBU.
        parser.reset();QVERIFY(!parser.push(QByteArray::fromHex("680208aa30bb"),false,1,9000));
        const auto mixed=parser.push(QByteArray::fromHex("a001cc28dd"),true,2,9000);QVERIFY(mixed);
        QCOMPARE(*mixed,QByteArray::fromHex("0a01aa3202bbcc28dd"));
    }
    void av1MalformedPayloadsAreRejected() {
        using D=VideoPacket::Depacketizer;
        for(const auto &invalid:{"","10","1034","10b0aa","1000aa","183520aa","9030aa","9830aa","5030aa",
                                "000030aa","00ff","00ffffffffffffffff0130aa","200330aa","100a05aa"}){
            D parser(D::AV1);QVERIFY2(!parser.push(QByteArray::fromHex(invalid),true,1,9000),invalid);
        }
        D parser(D::AV1);QVERIFY(!parser.push(QByteArray::fromHex("5030aa"),false,1,9000));
        QVERIFY(!parser.push(QByteArray::fromHex("1030bb"),true,2,9000)); // Missing continuation bit.
        QVERIFY(parser.push(QByteArray::fromHex("1030cc"),true,3,12000));
    }
    void av1AssemblyBoundAndTimestampReset() {
        using D=VideoPacket::Depacketizer;D parser(D::AV1);QByteArray chunk(65000,'a');
        chunk[0]=0x50;chunk[1]=0x30;QVERIFY(!parser.push(chunk,false,1,9000));chunk[0]=char(0xd0);
        for(quint16 sequence=2;sequence<140;++sequence)QVERIFY(!parser.push(chunk,false,sequence,9000));
        chunk[0]=char(0x90);QVERIFY(!parser.push(chunk,true,140,9000));
        QVERIFY(parser.push(QByteArray::fromHex("1030cc"),true,141,12000));
    }
    void boundedAssemblyRejectsOversizeAndRecoversAfterReset() {
        VideoPacket::Depacketizer parser(VideoPacket::Depacketizer::H264);
        QByteArray chunk(65000, 'a');
        chunk[0] = char(0x7c);
        chunk[1] = char(0x85);
        QVERIFY(!parser.push(chunk, false, 1, 9000));
        chunk[1] = 5;
        for (quint16 sequence = 2; sequence < 140; ++sequence)
            QVERIFY(!parser.push(chunk, false, sequence, 9000));
        chunk[1] = char(0x45);
        QVERIFY(!parser.push(chunk, true, 140, 9000));
        parser.reset();
        QVERIFY(parser.push(QByteArray::fromHex("65aabb"), true, 1, 12000));
    }
};

QTEST_GUILESS_MAIN(VideoPacketTest)
#include "video_packet_test.moc"
