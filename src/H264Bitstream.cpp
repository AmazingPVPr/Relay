#include "H264Bitstream.h"
#include <QList>
#include <algorithm>
#include <limits>

namespace {
constexpr qsizetype MaxFrame = 8 * 1024 * 1024, MaxSps = 64 * 1024;
class Bits {
public:
    explicit Bits(const QByteArray &data) : bytes(data) {}
    const QByteArray &bytes;
    qsizetype at = 0;
    bool ok = true;
    quint32 read(int count = 1) {
        if (!ok || count < 0 || count > 32 || qsizetype(count) > bytes.size() * 8 - at) { ok = false; return 0; }
        quint32 result = 0;
        for (int n = 0; n < count; ++n, ++at) result = (result << 1) | ((quint8(bytes[at / 8]) >> (7 - at % 8)) & 1);
        return result;
    }
    quint32 ue() {
        int zeros = 0;
        while (ok && !read()) if (++zeros > 31) { ok = false; return 0; }
        const auto suffix = read(zeros);
        return ok ? quint32(((quint64(1) << zeros) - 1) + suffix) : 0;
    }
    qint64 se() { const auto value = ue(); return value & 1 ? qint64(value / 2) + 1 : -qint64(value / 2); }
};
class Writer {
public:
    QByteArray bytes;
    qsizetype at = 0;
    void bit(bool value) { if (at % 8 == 0) bytes += char(0); if (value) bytes[at / 8] = char(quint8(bytes[at / 8]) | (1 << (7 - at % 8))); ++at; }
    void ue(quint32 value) { const quint64 code = quint64(value) + 1; int size = 0; for (auto v = code; v; v >>= 1) ++size; for (int i = 1; i < size; ++i) bit(false); for (int i = size - 1; i >= 0; --i) bit((code >> i) & 1); }
    void copy(const QByteArray &from, qsizetype start, qsizetype end) { for (auto i = start; i < end; ++i) bit((quint8(from[i / 8]) >> (7 - i % 8)) & 1); }
};
QByteArray unescape(const QByteArray &data) {
    QByteArray result; result.reserve(data.size()); int zeros = 0;
    for (auto byte : data) {
        if (zeros >= 2 && quint8(byte) == 3) { zeros = 0; continue; }
        result += byte; zeros = byte == 0 ? zeros + 1 : 0;
    }
    return result;
}
QByteArray escape(const QByteArray &data) {
    QByteArray result; result.reserve(data.size() + 16); int zeros = 0;
    for (auto byte : data) {
        if (zeros >= 2 && quint8(byte) <= 3) { result += char(3); zeros = 0; }
        result += byte; zeros = byte == 0 ? zeros + 1 : 0;
    }
    return result;
}
bool skipSps(Bits &bits, quint32 &refs) {
    const auto profile = bits.read(8); bits.read(16); if (bits.ue() > 31) return false;
    const bool high = profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135 || profile == 144;
    if (high) {
        const auto chroma = bits.ue(); if (chroma > 3) return false; if (chroma == 3) bits.read();
        if (bits.ue() > 6 || bits.ue() > 6) return false;
        bits.read();
        if (bits.read()) for (int i = 0; i < (chroma == 3 ? 12 : 8); ++i) if (bits.read()) {
            int last = 8, next = 8;
            for (int j = 0; j < (i < 6 ? 16 : 64); ++j) {
                if (next != 0) { const auto delta = bits.se(); if (!bits.ok || delta < -128 || delta > 127) return false; next = (last + int(delta) + 256) % 256; }
                if (next != 0) last = next;
            }
        }
    }
    if (bits.ue() > 12) return false;
    const auto poc = bits.ue(); if (poc > 2) return false;
    if (poc == 0) { if (bits.ue() > 12) return false; }
    else if (poc == 1) { bits.read(); bits.se(); bits.se(); const auto count = bits.ue(); if (count > 255) return false; for (quint32 i = 0; i < count; ++i) bits.se(); }
    refs = bits.ue(); if (refs > 16) return false;
    bits.read(); if (bits.ue() > 65535 || bits.ue() > 65535) return false;
    if (!bits.read()) bits.read();
    bits.read();
    if (bits.read()) for (int n = 0; n < 4; ++n) bits.ue();
    return bits.ok;
}
bool skipHrd(Bits &bits) {
    const auto count = bits.ue(); if (count > 31) return false; bits.read(8);
    for (quint32 n = 0; n <= count; ++n) { bits.ue(); bits.ue(); bits.read(); }
    bits.read(20); return bits.ok;
}
void restrictions(Writer &out, quint32 refs) { out.bit(true); out.ue(2); out.ue(1); out.ue(16); out.ue(16); out.ue(0); out.ue(refs); }
QByteArray prepareSps(const QByteArray &nalu) {
    if (nalu.size() < 5 || nalu.size() > MaxSps) return {};
    const auto rbsp = unescape(nalu.mid(1)); Bits bits(rbsp); quint32 refs = 0;
    if (!skipSps(bits, refs)) return {};
    const auto vuiFlag = bits.at; const bool vui = bits.read();
    qsizetype replaceStart = vuiFlag, replaceEnd = bits.at; Writer replacement;
    if (!vui) {
        replacement.bit(true); for (int i = 0; i < 8; ++i) replacement.bit(false); replacement.bit(true); restrictions(replacement, refs);
    } else {
        if (bits.read()) { if (bits.read(8) == 255) bits.read(32); } // aspect ratio
        if (bits.read()) bits.read(); // overscan
        if (bits.read()) { bits.read(4); if (bits.read()) { bits.read(24); } } // signal and color
        if (bits.read()) { bits.ue(); bits.ue(); } // chroma location
        if (bits.read()) { bits.read(32); bits.read(32); bits.read(); } // timing
        const bool nalHrd = bits.read(); if (nalHrd && !skipHrd(bits)) return {};
        const bool vclHrd = bits.read(); if (vclHrd && !skipHrd(bits)) return {};
        if (nalHrd || vclHrd) bits.read();
        bits.read(); // picture structure
        replaceStart = bits.at;
        if (!bits.read()) { replaceEnd = bits.at; replacement.bit(true); restrictions(replacement, refs); }
        else {
            bits.read(); for (int i = 0; i < 4; ++i) bits.ue();
            replaceStart = bits.at; const auto reorder = bits.ue(), buffering = bits.ue(); replaceEnd = bits.at;
            if (!bits.ok) return {};
            if (reorder == 0 && buffering <= refs) return nalu;
            replacement.ue(0); replacement.ue(refs);
        }
    }
    if (!bits.ok || replaceEnd >= rbsp.size() * 8) return {};
    Writer out; out.copy(rbsp, 0, replaceStart); out.copy(replacement.bytes, 0, replacement.at); out.copy(rbsp, replaceEnd, rbsp.size() * 8);
    return nalu.left(1) + escape(out.bytes);
}
struct Nalu { qsizetype start, payload, end; };
}
QByteArray H264Bitstream::prepareForDave(const QByteArray &frame) {
    if (frame.isEmpty() || frame.size() > MaxFrame) return {};
    QList<Nalu> nalus;
    for (qsizetype at = 0; at + 3 <= frame.size();) {
        const bool four = at + 4 <= frame.size() && frame[at] == 0 && frame[at + 1] == 0 && frame[at + 2] == 0 && frame[at + 3] == 1;
        const bool three = frame[at] == 0 && frame[at + 1] == 0 && frame[at + 2] == 1;
        if (four || three) { if (!nalus.isEmpty()) nalus.last().end = at; if (nalus.size() >= 4096) return {}; nalus.append({at, at + (four ? 4 : 3), frame.size()}); at += four ? 4 : 3; }
        else ++at;
    }
    if (nalus.isEmpty() || nalus.first().start != 0) return {};
    QByteArray output; output.reserve(frame.size() + 128);
    for (const auto &nalu : nalus) {
        if (nalu.payload >= nalu.end) return {};
        const auto type = quint8(frame[nalu.payload]) & 31; if (type < 1 || type > 23 || (quint8(frame[nalu.payload]) & 128)) return {};
        auto bytes = frame.mid(nalu.payload, nalu.end - nalu.payload);
        if (type == 7) { bytes = prepareSps(bytes); if (bytes.isEmpty()) return {}; }
        output += frame.mid(nalu.start, nalu.payload - nalu.start); output += bytes;
    }
    return output;
}
