#include "BroadcastPacket.h"
#include <QDateTime>
#include <QtEndian>
#include <sodium.h>
#include <array>
#include <cstring>

namespace {
QList<QByteArray> nalus(const QByteArray &frame) {
    QList<QByteArray> result;
    if(frame.isEmpty() || frame.size()>8*1024*1024)return result;
    qsizetype start=-1;
    for(qsizetype i=0;i+3<=frame.size();) {
        const bool three=frame[i]==0 && frame[i+1]==0 && frame[i+2]==1;
        const bool four=i+4<=frame.size() && frame[i]==0 && frame[i+1]==0 && frame[i+2]==0 && frame[i+3]==1;
        if(three || four) {if(start>=0 && i>start)result.append(frame.mid(start,i-start));i+=four?4:3;start=i;}
        else ++i;
    }
    if(start>=0 && start<frame.size())result.append(frame.mid(start));
    return result;
}
void extension(QByteArray &out,quint8 id,const QByteArray &data) {out+=char((id<<4)|(data.size()-1));out+=data;}
}
namespace BroadcastPacket {
QByteArray normalizeH264(const QByteArray &annexB) {
    QByteArray result;for(const auto &nalu:nalus(annexB)){result+=QByteArray::fromHex("00000001");result+=nalu;}return result;
}
QList<QByteArray> h264Payloads(const QByteArray &frame,int maximum) {
    QList<QByteArray> result;if(maximum<64 || maximum>1400)return result;
    for(const auto &nalu:nalus(frame)) {
        const auto type=quint8(nalu[0])&31;if(type<1 || type>23)return {};
        if(nalu.size()<=maximum){result.append(nalu);continue;}
        for(qsizetype offset=1;offset<nalu.size();offset+=maximum-2) {
            QByteArray payload;payload+=char((quint8(nalu[0])&0xe0)|28);
            payload+=char(type|(offset==1?0x80:0)|(offset+maximum-2>=nalu.size()?0x40:0));
            payload+=nalu.mid(offset,maximum-2);result.append(payload);
        }
    }
    return result;
}
QByteArray encrypt(const QByteArray &payload,const QByteArray &key,quint8 pt,bool marker,
                   quint16 sequence,quint32 timestamp,quint32 ssrc,quint32 nonce,
                   quint16 transportSequence,bool video,bool repaired,const QByteArray &rid) {
    if(key.size()!=32 || payload.isEmpty() || payload.size()>1400 || pt>127)return {};
    QByteArray header(video?16:12,'\0');header[0]=char(video?0x90:0x80);header[1]=char(pt|(marker?0x80:0));
    qToBigEndian(sequence,header.data()+2);qToBigEndian(timestamp,header.data()+4);qToBigEndian(ssrc,header.data()+8);
    QByteArray protectedPayload;
    if(video) {
        // Discord's RTP extension mapping, as implemented by discord-native-voice
        // (MIT, Dolfies, 2026). The extension preamble is authenticated; values
        // are encrypted along with the media in rtpsize mode.
        extension(protectedPayload,2,QByteArray(3,'\0'));
        const quint32 sendTime=quint32((quint64(QDateTime::currentMSecsSinceEpoch())*262144/1000)&0xffffff);
        QByteArray time(3,'\0');time[0]=char(sendTime>>16);time[1]=char(sendTime>>8);time[2]=char(sendTime);extension(protectedPayload,3,time);
        extension(protectedPayload,4,QByteArray(1,'\0'));
        QByteArray transport(2,'\0');qToBigEndian(transportSequence,transport.data());extension(protectedPayload,5,transport);
        extension(protectedPayload,6,QByteArray(3,'\0'));extension(protectedPayload,7,QByteArray(1,1));
        extension(protectedPayload,8,QByteArray(13,'\0'));extension(protectedPayload,10,"screen");
        extension(protectedPayload,repaired?12:11,rid.isEmpty()?QByteArray("100"):rid.left(16));
        while(protectedPayload.size()%4)protectedPayload+=char(0);
        qToBigEndian<quint16>(0xbede,header.data()+12);qToBigEndian<quint16>(protectedPayload.size()/4,header.data()+14);
    }
    protectedPayload+=payload;
    std::array<unsigned char,24> iv{};qToLittleEndian(nonce,iv.data());
    QByteArray output(header.size()+protectedPayload.size()+20,Qt::Uninitialized);std::memcpy(output.data(),header.constData(),header.size());
    unsigned long long length=0;
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(output.data()+header.size()),&length,
        reinterpret_cast<const unsigned char*>(protectedPayload.constData()),protectedPayload.size(),
        reinterpret_cast<const unsigned char*>(header.constData()),header.size(),nullptr,iv.data(),reinterpret_cast<const unsigned char*>(key.constData()))!=0)return {};
    std::memcpy(output.data()+header.size()+length,iv.data(),4);return output;
}
}
