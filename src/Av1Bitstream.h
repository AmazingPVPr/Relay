#pragma once
#include <QByteArray>
#include <QtGlobal>

// AV1 low-overhead OBU framing. RTP omits OBU lengths; DAVE additionally
// requires the final OBU to retain its no-size header until authentication.
namespace Av1Bitstream {
inline bool readSize(const QByteArray &data, qsizetype &offset, quint64 &value) {
    value = 0;
    for (int i = 0; i < 8 && offset < data.size(); ++i) {
        const auto byte = quint8(data[offset++]);
        value |= quint64(byte & 0x7f) << (i * 7);
        if (!(byte & 0x80)) return true;
    }
    return false;
}
inline void writeSize(QByteArray &out, quint64 value) {
    do {
        const auto byte = quint8(value & 0x7f); value >>= 7;
        out += char(byte | (value ? 0x80 : 0));
    } while (value);
}
struct Obu {
    qsizetype start = 0, headerBytes = 0, payload = 0, size = 0;
    int type = 0;
};
inline bool readObu(const QByteArray &data, qsizetype &offset, Obu &obu) {
    if (offset >= data.size()) return false;
    obu.start = offset;
    const auto header = quint8(data[offset++]);
    if (header & 0x81) return false;
    obu.type = (header >> 3) & 15;
    if (!obu.type) return false;
    if (header & 4) {
        if (offset >= data.size() || (quint8(data[offset]) & 7)) return false;
        ++offset;
    }
    obu.headerBytes = offset - obu.start;
    quint64 size = data.size() - offset;
    if ((header & 2) && !readSize(data, offset, size)) return false;
    if (size > quint64(data.size() - offset)) return false;
    obu.payload = offset; obu.size = qsizetype(size); offset += obu.size;
    return true;
}
inline QByteArray withSizes(const QByteArray &frame) {
    QByteArray out; qsizetype offset = 0; Obu obu;
    while (offset < frame.size()) {
        if (!readObu(frame, offset, obu)) return {};
        out += char(quint8(frame[obu.start]) | 2);
        if (obu.headerBytes == 2) out += frame[obu.start + 1];
        writeSize(out, quint64(obu.size));
        out += frame.mid(obu.payload, obu.size);
    }
    return out;
}
}
