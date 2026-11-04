#include "VideoPacket.h"
#include "Av1Bitstream.h"

#include <QtEndian>
#include <sodium.h>
#include <array>
#include <cstring>
#include <utility>

// H.264 and VP8 packet formats: RFC 6184 sections 5.6–5.8 and RFC 7741 section 4.
// Discord framing and reassembly were cross-checked against discord-native-voice:
// https://github.com/dolfies/discord-native-voice (transport/crypto.rs, rtp/mod.rs,
// video/h264.rs, video/vp8.rs). This implementation adds strict fragment-state,
// sequence, size, and compound-packet validation.
//
// The MIT License (MIT)
// Copyright (c) 2026-present Dolfies
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

namespace VideoPacket {
namespace {
constexpr qsizetype MaximumDatagram = 65535;
constexpr qsizetype TagBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;
constexpr qsizetype SuffixBytes = 4;
bool cryptoReady() { static const bool ready = sodium_init() >= 0; return ready; }
bool isRtcpType(quint8 type) { return type >= 192 && type <= 223; }

bool orientation(const QByteArray &extensions, quint16 profile, Rtp &rtp) {
    // RFC 8285 one-byte/two-byte headers. Values are already authenticated and
    // decrypted, and parsing never reads into the following media payload.
    // 3GPP TS 26.114 section 7.4.5 defines CVO as |0000 C F R1 R0|.
    // WebRTC's rtp_cvo.h maps R1/R0 to a clockwise display correction.
    // https://webrtc.googlesource.com/src/+/refs/heads/main/modules/rtp_rtcp/include/rtp_cvo.h
    const bool oneByte = profile == 0xbede;
    if (!oneByte && (profile & 0xfff0) != 0x1000) return true;
    qsizetype offset = 0;
    while (offset < extensions.size()) {
        const auto header = quint8(extensions[offset++]);
        if (!header) continue;
        const quint8 id = oneByte ? header >> 4 : header;
        if (oneByte && id == 0) continue;
        if (oneByte && id == 15) break;
        if (!oneByte && offset == extensions.size()) return false;
        const qsizetype length = oneByte ? (header & 15) + 1 : quint8(extensions[offset++]);
        if (length > extensions.size() - offset) return false;
        if (id == 4 && length == 1) {
            const auto value = quint8(extensions[offset]);
            if (!(value & 0xf0)) {
                rtp.rotation = (value & 3) * 90;
                rtp.mirrored = (value & 4) != 0;
                rtp.hasOrientation = true;
            }
        }
        offset += length;
    }
    return true;
}

std::optional<QByteArray> decrypt(const QByteArray &packet, const QByteArray &key, qsizetype headerSize) {
    if (!cryptoReady() || key.size() != 32 || packet.size() > MaximumDatagram ||
        packet.size() < headerSize + TagBytes + SuffixBytes) return {};
    std::array<unsigned char, 24> nonce{};
    // The wire suffix is a byte string. Discord clients use different counter
    // endianness; authentication must not reinterpret or reorder these bytes.
    std::memcpy(nonce.data(), packet.constData() + packet.size() - SuffixBytes, SuffixBytes);
    QByteArray plaintext(packet.size() - headerSize - TagBytes - SuffixBytes, Qt::Uninitialized);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            reinterpret_cast<unsigned char *>(plaintext.data()), &length, nullptr,
            reinterpret_cast<const unsigned char *>(packet.constData() + headerSize),
            packet.size() - headerSize - SuffixBytes,
            reinterpret_cast<const unsigned char *>(packet.constData()), headerSize,
            nonce.data(), reinterpret_cast<const unsigned char *>(key.constData())) != 0) return {};
    plaintext.resize(qsizetype(length));
    return plaintext;
}

bool validRtcp(const QByteArray &plain) {
    if (plain.size() < 8 || plain.size() > MaximumDatagram - TagBytes - SuffixBytes || plain.size() % 4) return false;
    qsizetype offset = 0;
    while (offset < plain.size()) {
        if (plain.size() - offset < 4 || (quint8(plain[offset]) >> 6) != 2 ||
            !isRtcpType(quint8(plain[offset + 1]))) return false;
        const qsizetype size = (qsizetype(qFromBigEndian<quint16>(plain.constData() + offset + 2)) + 1) * 4;
        if (size < (offset == 0 ? 8 : 4) || size > plain.size() - offset) return false;
        if (quint8(plain[offset]) & 0x20) {
            const auto padding = quint8(plain[offset + size - 1]);
            if (offset + size != plain.size() || padding == 0 || padding > size - 4) return false;
        }
        offset += size;
    }
    return true;
}
}

std::optional<Rtp> decryptRtp(const QByteArray &packet, const QByteArray &key) {
    if (packet.size() < 12 + TagBytes + SuffixBytes || (quint8(packet[0]) >> 6) != 2 ||
        isRtcpType(quint8(packet[1]))) return {};
    const bool extended = (quint8(packet[0]) & 0x10) != 0;
    qsizetype headerSize = 12 + 4 * (quint8(packet[0]) & 0x0f);
    if (packet.size() < headerSize + TagBytes + SuffixBytes) return {};
    qsizetype extensionBytes = 0;
    quint16 extensionProfile = 0;
    if (extended) {
        if (packet.size() < headerSize + 4 + TagBytes + SuffixBytes) return {};
        extensionProfile = qFromBigEndian<quint16>(packet.constData() + headerSize);
        extensionBytes = qsizetype(qFromBigEndian<quint16>(packet.constData() + headerSize + 2)) * 4;
        headerSize += 4;
    }
    auto plaintext = decrypt(packet, key, headerSize);
    if (!plaintext || extensionBytes > plaintext->size()) return {};
    Rtp rtp;
    if (extensionBytes) {
        if (!orientation(plaintext->left(extensionBytes), extensionProfile, rtp)) return {};
        plaintext->remove(0, extensionBytes);
    }
    if (quint8(packet[0]) & 0x20) {
        if (plaintext->isEmpty()) return {};
        const auto padding = quint8(plaintext->back());
        if (padding == 0 || padding > plaintext->size()) return {};
        plaintext->chop(padding);
    }
    if (plaintext->isEmpty()) return {};
    rtp.payloadType = quint8(quint8(packet[1]) & 0x7f); rtp.marker = (quint8(packet[1]) & 0x80) != 0;
    rtp.sequence = qFromBigEndian<quint16>(packet.constData() + 2); rtp.timestamp = qFromBigEndian<quint32>(packet.constData() + 4);
    rtp.ssrc = qFromBigEndian<quint32>(packet.constData() + 8);
    rtp.nonce = qFromLittleEndian<quint32>(packet.constData() + packet.size() - SuffixBytes); rtp.payload = std::move(*plaintext);
    return rtp;
}

QByteArray encryptRtcp(const QByteArray &plainRtcp, const QByteArray &key, quint32 counter) {
    if (!cryptoReady() || key.size() != 32 || !validRtcp(plainRtcp)) return {};
    QByteArray packet(plainRtcp.size() + TagBytes + SuffixBytes, Qt::Uninitialized);
    std::memcpy(packet.data(), plainRtcp.constData(), 8);
    std::array<unsigned char, 24> nonce{};
    qToLittleEndian(counter, nonce.data());
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            reinterpret_cast<unsigned char *>(packet.data() + 8), &length,
            reinterpret_cast<const unsigned char *>(plainRtcp.constData() + 8), plainRtcp.size() - 8,
            reinterpret_cast<const unsigned char *>(packet.constData()), 8, nullptr,
            nonce.data(), reinterpret_cast<const unsigned char *>(key.constData())) != 0) return {};
    std::memcpy(packet.data() + 8 + qsizetype(length), nonce.data(), SuffixBytes);
    return packet;
}

std::optional<QByteArray> decryptRtcp(const QByteArray &packet, const QByteArray &key) {
    if (packet.size() < 8 + TagBytes + SuffixBytes || (quint8(packet[0]) >> 6) != 2 ||
        !isRtcpType(quint8(packet[1]))) return {};
    auto plaintext = decrypt(packet, key, 8);
    if (!plaintext) return {};
    plaintext->prepend(packet.constData(), 8);
    if (!validRtcp(*plaintext)) return {};
    return plaintext;
}

void Depacketizer::reset() {
    m_frame.clear(); m_obu.clear();
    m_hasTimestamp = m_hasSequence = m_discarding = m_finished = m_fragmentActive = false;
    m_fragmentHeader = m_partition = 0;
    m_lastSequence = 0;
    m_timestamp = 0;
}

void Depacketizer::discard() {
    m_discarding = true;
    m_fragmentActive = false;
    m_frame.clear(); m_obu.clear();
}

bool Depacketizer::append(const char *data, qsizetype size) {
    if (size < 0 || size > MaximumFrameBytes - m_frame.size()) return false;
    m_frame.append(data, size);
    return true;
}

bool Depacketizer::appendNalu(const char *data, qsizetype size) {
    static constexpr char StartCode[]{0, 0, 0, 1};
    return size > 0 && append(StartCode, 4) && append(data, size);
}

bool Depacketizer::h264(const QByteArray &payload) {
    if (payload.isEmpty() || (quint8(payload[0]) & 0x80)) return false;
    const auto type = quint8(payload[0]) & 0x1f;
    if (type >= 1 && type <= 23) return !m_fragmentActive && appendNalu(payload.constData(), payload.size());
    if (type == 24) { // STAP-A: each member has its own network-order length.
        if (m_fragmentActive || payload.size() < 4) return false;
        qsizetype offset = 1;
        while (offset < payload.size()) {
            if (payload.size() - offset < 2) return false;
            const auto size = qFromBigEndian<quint16>(payload.constData() + offset);
            offset += 2;
            if (size == 0 || size > payload.size() - offset) return false;
            const auto header = quint8(payload[offset]);
            if ((header & 0x80) || (header & 0x1f) == 0 || (header & 0x1f) > 23 ||
                !appendNalu(payload.constData() + offset, size)) return false;
            offset += size;
        }
        return true;
    }
    if (type != 28 || payload.size() < 3) return false; // FU-A only; interleaved mode is not negotiated.
    const auto fu = quint8(payload[1]);
    const bool start = (fu & 0x80) != 0, end = (fu & 0x40) != 0;
    const auto nalType = fu & 0x1f;
    if ((fu & 0x20) || (start && end) || nalType == 0 || nalType > 23) return false;
    const auto header = quint8((quint8(payload[0]) & 0xe0) | nalType);
    if (start) {
        if (m_fragmentActive) return false;
        const char byte = char(header);
        if (!appendNalu(&byte, 1)) return false;
        m_fragmentActive = true;
        m_fragmentHeader = header;
    } else if (!m_fragmentActive || m_fragmentHeader != header) return false;
    if (!append(payload.constData() + 2, payload.size() - 2)) return false;
    if (end) m_fragmentActive = false;
    return true;
}

bool Depacketizer::vp8(const QByteArray &payload) {
    if (payload.isEmpty()) return false;
    const auto descriptor = quint8(payload[0]);
    const bool start = (descriptor & 0x10) != 0;
    const auto partition = quint8(descriptor & 0x0f);
    if (partition > 8) return false;
    qsizetype offset = 1;
    if (descriptor & 0x80) {
        if (offset >= payload.size()) return false;
        const auto extensions = quint8(payload[offset++]);
        if (extensions & 0x80) {
            if (offset >= payload.size()) return false;
            offset += (quint8(payload[offset]) & 0x80) ? 2 : 1;
        }
        if (extensions & 0x40) ++offset;
        if (extensions & 0x30) ++offset;
    }
    if (offset >= payload.size()) return false;
    if (m_frame.isEmpty()) {
        if (!start || partition != 0) return false;
    } else if ((start && partition == 0) || (!start && partition != m_partition) || partition < m_partition) return false;
    m_partition = partition;
    return append(payload.constData() + offset, payload.size() - offset);
}

// RFC 7798 single NAL, AP and FU; decoding-order fields are not negotiated.
bool Depacketizer::h265(const QByteArray &payload) {
    if (payload.size() < 2 || (quint8(payload[0]) & 0x80) || !(quint8(payload[1]) & 7)) return false;
    const auto type = (quint8(payload[0]) >> 1) & 63;
    if (type < 48) return !m_fragmentActive && appendNalu(payload.constData(), payload.size());
    if (type == 48) {
        if (m_fragmentActive) return false;
        qsizetype offset = 2; int members = 0;
        while (offset < payload.size()) {
            if (payload.size() - offset < 2) return false;
            const auto size = qFromBigEndian<quint16>(payload.constData() + offset); offset += 2;
            if (size < 2 || size > payload.size() - offset) return false;
            const auto header = quint8(payload[offset]), second = quint8(payload[offset + 1]);
            if ((header & 0x80) || !(second & 7) || ((header >> 1) & 63) >= 48 ||
                !appendNalu(payload.constData() + offset, size)) return false;
            offset += size; ++members;
        }
        return members >= 2;
    }
    if (type != 49 || payload.size() < 4) return false;
    const auto fu = quint8(payload[2]);
    const bool start = fu & 0x80, end = fu & 0x40;
    const auto nalType = fu & 63;
    if (nalType >= 48 || (start && end)) return false;
    const quint16 header = (quint16((quint8(payload[0]) & 0x81) | (nalType << 1)) << 8) | quint8(payload[1]);
    if (start) {
        if (m_fragmentActive) return false;
        char bytes[2]{char(header >> 8), char(header & 255)};
        if (!appendNalu(bytes, 2)) return false;
        m_fragmentActive = true; m_fragmentHeader = header;
    } else if (!m_fragmentActive || m_fragmentHeader != header) return false;
    if (!append(payload.constData() + 3, payload.size() - 3)) return false;
    if (end) m_fragmentActive = false;
    return true;
}

bool Depacketizer::finishObu(bool omitSize) {
    qsizetype offset = 0; Av1Bitstream::Obu obu;
    if (!Av1Bitstream::readObu(m_obu, offset, obu) || offset != m_obu.size()) return false;
    QByteArray header;
    header += char((quint8(m_obu[0]) & ~2) | (omitSize ? 0 : 2));
    if (obu.headerBytes == 2) header += m_obu[1];
    if (!omitSize) Av1Bitstream::writeSize(header, quint64(obu.size));
    if (!append(header.constData(), header.size()) || !append(m_obu.constData() + obu.payload, obu.size)) return false;
    m_obu.clear(); m_fragmentActive = false; return true;
}

// AOMedia AV1 RTP aggregation header (Z/Y/W/N), length-delimited OBU elements,
// and OBU fragments. Authenticate DAVE before restoring the final OBU length.
bool Depacketizer::av1(const QByteArray &payload, bool marker) {
    if (payload.size() < 2) return false;
    const auto header = quint8(payload[0]);
    const bool z = header & 0x80, y = header & 0x40;
    const int w = (header >> 4) & 3;
    if ((z && (header & 8)) || (marker && y) || z != !m_obu.isEmpty()) return false;
    qsizetype offset = 1; int elements = 0;
    while (offset < payload.size()) {
        ++elements;
        quint64 size = payload.size() - offset;
        if ((!w || elements != w) && !Av1Bitstream::readSize(payload, offset, size)) return false;
        if (!size || size > quint64(payload.size() - offset)) return false;
        const bool last = offset + qsizetype(size) == payload.size();
        if (elements > 1 && !m_obu.isEmpty()) return false;
        if (size > quint64(MaximumFrameBytes - m_frame.size() - m_obu.size())) return false;
        m_obu.append(payload.constData() + offset, qsizetype(size)); offset += qsizetype(size);
        if (last && y) m_fragmentActive = true;
        else if (!finishObu(marker && last)) return false;
    }
    return elements > 0 && (!w || elements == w);
}

std::optional<QByteArray> Depacketizer::push(const QByteArray &payload, bool marker,
                                           quint16 sequence, quint32 timestamp) {
    const quint16 distance = quint16(sequence - m_lastSequence);
    // Ignore duplicates/late packets without poisoning the frame being assembled.
    if (m_hasSequence && (distance == 0 || distance >= 0x8000)) return {};
    const bool gap = m_hasSequence && distance != 1;
    m_hasSequence = true;
    m_lastSequence = sequence;
    if (!m_hasTimestamp || m_timestamp != timestamp) {
        m_hasTimestamp = true;
        m_timestamp = timestamp;
        m_frame.clear(); m_obu.clear();
        m_discarding = m_finished = m_fragmentActive = false;
        m_partition = 0;
    }
    if (m_finished) return {};
    if (gap) discard();
    if (!m_discarding) {
        bool valid = false;
        if (payload.size() <= MaximumDatagram) switch (m_codec) {
        case H264: valid = h264(payload); break;
        case VP8: valid = vp8(payload); break;
        case H265: valid = h265(payload); break;
        case AV1: valid = av1(payload, marker); break;
        }
        if (!valid) discard();
    }
    if (!marker) return {};
    m_finished = true;
    if (m_fragmentActive || m_discarding || m_frame.isEmpty()) { discard(); return {}; }
    return std::exchange(m_frame, {});
}
}
