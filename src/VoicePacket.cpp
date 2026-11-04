#include "VoicePacket.h"
#include <QHostAddress>
#include <QtEndian>
#include <sodium.h>
#include <array>
#include <cstring>

namespace VoicePacket {
QByteArray nativePing(quint32 sequence) {
    QByteArray packet(8, '\0');
    qToBigEndian<quint32>(0x1337cafe, packet.data());
    qToLittleEndian(sequence, packet.data()+4);
    return packet;
}
std::optional<quint32> parseNativePong(const QByteArray &packet) {
    if(packet.size()<8 || qFromBigEndian<quint32>(packet.constData())!=0x1337f00d)return {};
    return qFromLittleEndian<quint32>(packet.constData()+4);
}
QByteArray discoveryRequest(quint32 ssrc) {
    QByteArray b(74, '\0');
    qToBigEndian<quint16>(1, b.data()); qToBigEndian<quint16>(70, b.data()+2);
    qToBigEndian(ssrc,b.data()+4); return b;
}
std::optional<Discovery> parseDiscovery(const QByteArray &b, quint32 ssrc) {
    if(b.size()!=74 || qFromBigEndian<quint16>(b.constData())!=2 ||
       qFromBigEndian<quint16>(b.constData()+2)!=70 || qFromBigEndian<quint32>(b.constData()+4)!=ssrc) return {};
    QByteArray ip=b.mid(8,64); const auto zero=ip.indexOf('\0');
    if(zero<0) return {};
    ip.truncate(zero);
    QHostAddress address(QString::fromLatin1(ip));
    const auto port=qFromBigEndian<quint16>(b.constData()+72);
    if(address.isNull() || port==0) return {};
    return Discovery{address.toString(),port};
}
QByteArray encrypt(const QByteArray &payload,const QByteArray &key,quint16 sequence,quint32 timestamp,quint32 ssrc,quint32 nonce) {
    if(key.size()!=crypto_aead_xchacha20poly1305_ietf_KEYBYTES || payload.isEmpty() || payload.size()>65000) return {};
    QByteArray out(12+payload.size()+crypto_aead_xchacha20poly1305_ietf_ABYTES+4,Qt::Uninitialized);
    out[0]=char(0x80);out[1]=char(0x78);
    qToBigEndian(sequence,out.data()+2);qToBigEndian(timestamp,out.data()+4);qToBigEndian(ssrc,out.data()+8);
    std::array<unsigned char,24> iv{};qToBigEndian(nonce,iv.data());
    unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(out.data()+12),&size,
        reinterpret_cast<const unsigned char*>(payload.constData()),payload.size(),reinterpret_cast<const unsigned char*>(out.constData()),12,nullptr,iv.data(),reinterpret_cast<const unsigned char*>(key.constData()))!=0) return {};
    qToBigEndian(nonce,out.data()+12+size);return out;
}
std::optional<Frame> decrypt(const QByteArray &packet,const QByteArray &key) {
    if(key.size()!=32 || packet.size()<32 || (quint8(packet[0])>>6)!=2 || (quint8(packet[1])&0x7f)!=120) return {};
    const bool extension=(quint8(packet[0])&0x10)!=0;
    int header=12+4*(quint8(packet[0])&0x0f);
    if(extension) header+=4;
    if(header+20>packet.size()) return {};
    const auto nonce=qFromBigEndian<quint32>(packet.constData()+packet.size()-4);
    std::array<unsigned char,24> iv{};qToBigEndian(nonce,iv.data());
    QByteArray payload(packet.size()-header-20,Qt::Uninitialized);unsigned long long size=0;
    if(crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(payload.data()),&size,nullptr,
        reinterpret_cast<const unsigned char*>(packet.constData()+header),packet.size()-header-4,
        reinterpret_cast<const unsigned char*>(packet.constData()),header,iv.data(),reinterpret_cast<const unsigned char*>(key.constData()))!=0) return {};
    payload.resize(size);
    if(extension) {
        const int extensionBytes=4*qFromBigEndian<quint16>(packet.constData()+header-2);
        if(extensionBytes>payload.size())return {};
        payload.remove(0,extensionBytes);
    }
    if(quint8(packet[0])&0x20) {
        if(payload.isEmpty())return {};
        const auto padding=quint8(payload.back());
        if(padding==0 || padding>payload.size()) return {};
        payload.chop(padding);
    }
    if(payload.isEmpty())return {};
    return Frame{qFromBigEndian<quint16>(packet.constData()+2),qFromBigEndian<quint32>(packet.constData()+4),qFromBigEndian<quint32>(packet.constData()+8),payload,nonce};
}
}
