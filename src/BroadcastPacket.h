#pragma once
#include <QByteArray>
#include <QList>
#include <QtGlobal>

namespace BroadcastPacket {
QList<QByteArray> h264Payloads(const QByteArray &annexB, int maximum=1100);
QByteArray normalizeH264(const QByteArray &annexB);
QByteArray encrypt(const QByteArray &payload,const QByteArray &key,quint8 payloadType,bool marker,
                   quint16 sequence,quint32 timestamp,quint32 ssrc,quint32 nonce,
                   quint16 transportSequence=0,bool video=true,bool repaired=false,const QByteArray &rid="100");
}
