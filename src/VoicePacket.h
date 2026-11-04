#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

namespace VoicePacket {
struct Frame { quint16 sequence{}; quint32 timestamp{}; quint32 ssrc{}; QByteArray payload; quint32 nonce{}; };
QByteArray discoveryRequest(quint32 ssrc);
struct Discovery { QString address; quint16 port{}; };
std::optional<Discovery> parseDiscovery(const QByteArray &packet, quint32 expectedSsrc);
QByteArray nativePing(quint32 sequence);
std::optional<quint32> parseNativePong(const QByteArray &packet);
QByteArray encrypt(const QByteArray &payload, const QByteArray &key, quint16 sequence,
                   quint32 timestamp, quint32 ssrc, quint32 nonce);
std::optional<Frame> decrypt(const QByteArray &packet, const QByteArray &key);
}
