#pragma once

#include <QByteArray>
#include <QtGlobal>
#include <optional>

namespace VideoPacket {
struct Rtp {
    quint8 payloadType = 0;
    bool marker = false;
    quint16 sequence = 0;
    quint32 timestamp = 0;
    quint32 ssrc = 0;
    quint32 nonce = 0; // Opaque suffix interpreted LE; never use for replay ordering.
    QByteArray payload;
    // Native Discord extension 4 uses 3GPP CVO. Rotation is the clockwise
    // correction for display; the camera-facing bit does not imply mirroring.
    int rotation = 0;
    bool mirrored = false;
    bool hasOrientation = false;
};

std::optional<Rtp> decryptRtp(const QByteArray &packet, const QByteArray &key);
// Use a unique nonce for every packet under a transport key, including RTP.
QByteArray encryptRtcp(const QByteArray &plainRtcp, const QByteArray &key, quint32 nonce);
std::optional<QByteArray> decryptRtcp(const QByteArray &packet, const QByteArray &key);

// One instance per media SSRC. Input is authenticated transport plaintext.
// Output is a complete encoded frame, ready for DAVE decryption then decoding.
class Depacketizer final {
public:
    enum Codec { H264, VP8, H265, AV1 };
    static constexpr qsizetype MaximumFrameBytes = 8 * 1024 * 1024;
    explicit Depacketizer(Codec codec) : m_codec(codec) {}
    std::optional<QByteArray> push(const QByteArray &payload, bool marker,
                                 quint16 sequence, quint32 timestamp);
    void reset();
private:
    void discard();
    bool append(const char *data, qsizetype size);
    bool appendNalu(const char *data, qsizetype size);
    bool h264(const QByteArray &payload);
    bool vp8(const QByteArray &payload);
    bool h265(const QByteArray &payload);
    bool av1(const QByteArray &payload, bool marker);
    bool finishObu(bool omitSize);
    Codec m_codec;
    QByteArray m_frame, m_obu;
    bool m_hasTimestamp = false, m_hasSequence = false, m_discarding = false, m_finished = false;
    bool m_fragmentActive = false;
    quint16 m_fragmentHeader = 0;
    quint8 m_partition = 0;
    quint16 m_lastSequence = 0;
    quint32 m_timestamp = 0;
};

// Keep negotiation, primary payloads and RTX mappings identical in stream and
// voice/camera connections. Lower priorities are preferred by Discord.
struct VideoCodec {
    const char *name;
    quint8 payloadType, repairType;
    int priority;
    Depacketizer::Codec depacketizer;
};
inline constexpr VideoCodec VideoCodecs[] = {
    {"AV1", 101, 102, 1000, Depacketizer::AV1},
    {"H265", 103, 104, 2000, Depacketizer::H265},
    {"H264", 105, 106, 3000, Depacketizer::H264},
    {"VP8", 107, 108, 4000, Depacketizer::VP8},
};
inline const VideoCodec *videoCodec(quint8 payloadType) {
    for (const auto &codec : VideoCodecs)
        if (codec.payloadType == payloadType || codec.repairType == payloadType) return &codec;
    return nullptr;
}
}
