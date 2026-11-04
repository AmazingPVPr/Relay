#include <QtTest>
#include <QScopeGuard>
#include <QProcess>
#include <QStandardPaths>
#include "H264Bitstream.h"
#ifdef RELAY_TEST_MLSPP
#include "StreamTestDave.h"
#endif

namespace {
// Literal RBSP vectors from WebRTC's GenerateFakeSps test generator (640x480,
// max_num_ref_frames=1), with its emulation prevention byte. The receiver's
// SpsVuiRewriter expects the first three cases to be rewritten, and the last
// to be retained. These are independent of Relay's parser/writer.
// https://webrtc.googlesource.com/src/+/refs/heads/main/common_video/h264/sps_vui_rewriter_unittest.cc
const QByteArray noVui = QByteArray::fromHex("00000303f40503c7c0");
const QByteArray noRestriction = QByteArray::fromHex("00000303f40503c7e000");
const QByteArray suboptimal = QByteArray::fromHex("00000303f40503c7e01b41108908");
const QByteArray optimal = QByteArray::fromHex("00000303f40503c7e01b41108d00");
QByteArray frame(const QByteArray &sps) { return QByteArray::fromHex("0000000167") + sps + QByteArray::fromHex("0000000168ce06e20000000165b840112233445566778899"); }
}
class H264BitstreamTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
#ifdef RELAY_TEST_MLSPP
        daveSetLogSinkCallback([](DAVELoggingSeverity, const char *, int, const char *) {});
#endif
    }
    void webRtcReceiverVectors_data() {
        QTest::addColumn<QByteArray>("sps");
        QTest::newRow("missing-vui") << noVui;
        QTest::newRow("missing-restriction") << noRestriction;
        QTest::newRow("excessive-buffering") << suboptimal;
    }
    void webRtcReceiverVectors() {
        QFETCH(QByteArray, sps);
        const auto input = frame(sps), output = H264Bitstream::prepareForDave(input);
        QVERIFY(!output.isEmpty()); QVERIFY(output != input);
        // Only the SPS changes; PPS, coded picture and existing NAL boundaries
        // remain byte-for-byte intact. A receiver no longer rewrites this SPS.
        QVERIFY(output.endsWith(input.mid(input.indexOf(QByteArray::fromHex("0000000168")))));
        QCOMPARE(H264Bitstream::prepareForDave(output), output);
        QCOMPARE(output, frame(optimal));
    }
    void alreadySafePicturesAndMetadataAreUnchanged() {
        const auto input = QByteArray::fromHex("000000010910") + frame(optimal) + QByteArray::fromHex("00000001060a112233000000010cffff80");
        QCOMPARE(H264Bitstream::prepareForDave(input), input);
        QCOMPARE(H264Bitstream::prepareForDave(QByteArray::fromHex("00000141b840112233")), QByteArray::fromHex("00000141b840112233"));
    }
    void generatedProfilesRetainColorTimingAndPictures_data() {
        QTest::addColumn<QString>("profile"); QTest::addColumn<QString>("format");
        QTest::newRow("baseline-420") << QString("baseline") << QString("yuv420p");
        QTest::newRow("main-420") << QString("main") << QString("yuv420p");
        QTest::newRow("high-420") << QString("high") << QString("yuv420p");
        QTest::newRow("high10") << QString("high10") << QString("yuv420p10le");
        QTest::newRow("high422") << QString("high422") << QString("yuv422p");
        QTest::newRow("high444") << QString("high444") << QString("yuv444p");
    }
    void generatedProfilesRetainColorTimingAndPictures() {
        QFETCH(QString, profile); QFETCH(QString, format);
        const auto executable = QStandardPaths::findExecutable("ffmpeg"); if (executable.isEmpty()) QSKIP("FFmpeg is needed only for generated H264 profile coverage");
        QProcess generator; generator.start(executable, {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=c=red:s=320x180:r=15", "-frames:v", "1", "-pix_fmt", format, "-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency", "-profile:v", profile, "-level:v", "5.1", "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709", "-color_range", "tv", "-x264-params", "repeat-headers=1:annexb=1:aud=1:scenecut=0", "-f", "h264", "pipe:1"});
        QVERIFY(generator.waitForStarted(3000)); QVERIFY(generator.waitForFinished(10000));
        const auto errors = generator.readAllStandardError(); QVERIFY2(generator.exitCode() == 0, errors.constData());
        const auto coded = generator.readAllStandardOutput(); QVERIFY(coded.size() > 100); QVERIFY(coded.size() < 65536);
        // These encoder streams already carry the receiver's optimal buffering
        // limits. Entire-frame equality proves profile/level, BT.709 color,
        // timestamps, parameter sets and coded pictures are all retained.
        QCOMPARE(H264Bitstream::prepareForDave(coded), coded);
    }
    void rejectsMalformedInputWithoutUnboundedWork() {
        for (const auto &input : {QByteArray{}, QByteArray("no Annex B"), QByteArray::fromHex("00000001"), QByteArray::fromHex("0000000167ff"), QByteArray::fromHex("00000001e7000000"), QByteArray::fromHex("000000011c112233")}) QVERIFY(H264Bitstream::prepareForDave(input).isEmpty());
        QVERIFY(H264Bitstream::prepareForDave(QByteArray::fromHex("0000000167") + QByteArray(65537, char(0xff))).isEmpty());
        QVERIFY(H264Bitstream::prepareForDave(QByteArray(8 * 1024 * 1024 + 1, 'x')).isEmpty());
        for (int length = 0; length < noVui.size() - 1; ++length) QVERIFY(H264Bitstream::prepareForDave(frame(noVui.left(length))).isEmpty());
    }
    void recipientSpsRewriteInvalidatesUnpreparedDaveFrame() {
#ifndef RELAY_TEST_MLSPP
        QSKIP("MLS++ headers are required for independent encrypted media validation");
#else
        StreamTestDave::Group group;
        auto receiver = daveSessionCreate(nullptr, nullptr, nullptr, nullptr); QVERIFY(receiver);
        const auto cleanReceiver = qScopeGuard([&] { daveSessionDestroy(receiver); });
        const auto external = group.externalSender(); daveSessionSetExternalSender(receiver, StreamTestDave::bytes(external), external.size());
        daveSessionInit(receiver, 1, StreamTestDave::groupId, StreamTestDave::selfId);
        uint8_t *key = nullptr; size_t keyLength = 0; daveSessionGetMarshalledKeyPackage(receiver, &key, &keyLength);
        const QByteArray keyPackage(reinterpret_cast<const char *>(key), keyLength); daveFree(key);
        const auto combined = group.commitFor(keyPackage); auto [commit, welcome] = group.split(combined); QVERIFY(group.accept(commit, true));
        const char *ids[] = {StreamTestDave::selfId, StreamTestDave::ownerId};
        auto accepted = daveSessionProcessWelcome(receiver, StreamTestDave::bytes(welcome), welcome.size(), ids, 2); QVERIFY(accepted); daveWelcomeResultDestroy(accepted);
        QVERIFY(group.prepare("H264"));
        auto decryptor = daveDecryptorCreate(); QVERIFY(decryptor); const auto cleanDecryptor = qScopeGuard([&] { daveDecryptorDestroy(decryptor); });
        auto ratchet = daveSessionGetKeyRatchet(receiver, StreamTestDave::ownerId); QVERIFY(ratchet); daveDecryptorTransitionToKeyRatchet(decryptor, ratchet); daveKeyRatchetDestroy(ratchet);
        const auto unsafeFrame = frame(noVui), safeFrame = H264Bitstream::prepareForDave(unsafeFrame); QVERIFY(!safeFrame.isEmpty());
        const auto originalCipher = group.encrypt(unsafeFrame); QVERIFY(!originalCipher.isEmpty());
        QByteArray clear(originalCipher.size() + 128, Qt::Uninitialized); size_t clearLength = 0;
        // Model the primary WebRTC recipient mutation using the independent
        // golden SPS from its own test generator, not Relay's receiver/parser.
        auto rewrittenCipher = originalCipher; const auto originalSps = QByteArray::fromHex("67") + noVui;
        const auto before = rewrittenCipher.indexOf(originalSps); QVERIFY(before >= 0);
        rewrittenCipher.replace(before, originalSps.size(), QByteArray::fromHex("67") + optimal);
        QCOMPARE(daveDecryptorDecrypt(decryptor, DAVE_MEDIA_TYPE_VIDEO, StreamTestDave::bytes(rewrittenCipher), rewrittenCipher.size(), StreamTestDave::bytes(clear), clear.size(), &clearLength), DAVE_DECRYPTOR_RESULT_CODE_DECRYPTION_FAILURE);
        // A fresh, otherwise identical frame succeeds on that same receiver;
        // the preceding failure is authentication, not an already-used nonce.
        QCOMPARE(daveDecryptorDecrypt(decryptor, DAVE_MEDIA_TYPE_VIDEO, StreamTestDave::bytes(originalCipher), originalCipher.size(), StreamTestDave::bytes(clear), clear.size(), &clearLength), DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);
        // When preparation happens before authentication, the actual frame
        // transform survives transport and produces the intended clear frame.
        const auto preparedCipher = group.encrypt(safeFrame); QVERIFY(!preparedCipher.isEmpty());
        QCOMPARE(daveDecryptorDecrypt(decryptor, DAVE_MEDIA_TYPE_VIDEO, StreamTestDave::bytes(preparedCipher), preparedCipher.size(), StreamTestDave::bytes(clear), clear.size(), &clearLength), DAVE_DECRYPTOR_RESULT_CODE_SUCCESS);
        clear.resize(clearLength); QCOMPARE(clear, safeFrame);
#endif
    }
};
QTEST_GUILESS_MAIN(H264BitstreamTest)
#include "h264_bitstream_test.moc"
