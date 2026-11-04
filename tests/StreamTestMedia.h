#pragma once
#include <QByteArray>
#include "Av1Bitstream.h"
#include <QList>
#include <QString>
#include <QtEndian>
#include <sodium.h>
#include <array>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace StreamTestMedia {
inline QList<QByteArray> nalus(const QByteArray &bytes) {
    QList<QByteArray> result;
    qsizetype start=-1;
    for(qsizetype i=0;i+3<=bytes.size();) {
        const bool four=i+4<=bytes.size() && bytes.mid(i,4)==QByteArray::fromHex("00000001");
        const bool three=bytes.mid(i,3)==QByteArray::fromHex("000001");
        if(four || three) { if(start>=0 && i>start)result.append(bytes.mid(start,i-start));i+=four?4:3;start=i; }
        else ++i;
    }
    if(start>=0 && start<bytes.size())result.append(bytes.mid(start));return result;
}
inline QList<QByteArray> encodedFrames(const QString &name,int count=1,int width=320,int height=180,int gop=1) {
    av_log_set_level(AV_LOG_ERROR);
    const char *encoderName=name=="H264"?"libx264":name=="H265"?"libx265":name=="AV1"?"libaom-av1":"libvpx";
    const auto codec=avcodec_find_encoder_by_name(encoderName);
    if(!codec)return {};
    AVCodecContext *context=avcodec_alloc_context3(codec);if(!context)return {};
    context->width=width;context->height=height;context->pix_fmt=AV_PIX_FMT_YUV420P;context->time_base={1,30};context->framerate={30,1};
    context->bit_rate=1000000;context->gop_size=gop;context->max_b_frames=0;context->thread_count=2;
    if(name=="H264" || name=="H265"){
        av_opt_set(context->priv_data,"preset","ultrafast",0);av_opt_set(context->priv_data,"tune","zerolatency",0);
        if(name=="H265")av_opt_set(context->priv_data,"x265-params","log-level=error:pools=none:frame-threads=1:repeat-headers=1:scenecut=0",0);
        else av_opt_set(context->priv_data,"x264-params","scenecut=0",0);
    } else {
        av_opt_set(context->priv_data,"cpu-used","8",0);av_opt_set(context->priv_data,"lag-in-frames","0",0);
        if(name=="AV1")av_opt_set(context->priv_data,"usage","realtime",0);
        else av_opt_set(context->priv_data,"deadline","realtime",0);
    }
    QList<QByteArray> result;
    if(avcodec_open2(context,codec,nullptr)>=0) {
        auto frame=av_frame_alloc();auto packet=av_packet_alloc();
        if(frame && packet) {
            frame->format=context->pix_fmt;frame->width=width;frame->height=height;
            const auto drain=[&] {
                while(avcodec_receive_packet(context,packet)>=0){
                    QByteArray encoded(reinterpret_cast<char*>(packet->data),packet->size);
                    if(name=="H264" || name=="H265"){
                        QByteArray normalised;for(const auto &nalu:nalus(encoded)){normalised+=QByteArray::fromHex("00000001");normalised+=nalu;}encoded=normalised;
                    }
                    result.append(encoded);av_packet_unref(packet);
                }
            };
            if(av_frame_get_buffer(frame,32)>=0)for(int n=0;n<count;++n) {
                av_frame_make_writable(frame);frame->pts=n;
                for(int y=0;y<height;++y)std::memset(frame->data[0]+y*frame->linesize[0],96+n,width);
                for(int y=0;y<height/2;++y){std::memset(frame->data[1]+y*frame->linesize[1],90,width/2);std::memset(frame->data[2]+y*frame->linesize[2],220,width/2);}
                if(avcodec_send_frame(context,frame)<0)break;drain();
            }
            avcodec_send_frame(context,nullptr);drain();
        }
        av_packet_free(&packet);av_frame_free(&frame);
    }
    avcodec_free_context(&context);return result;
}
inline QByteArray encodedFrame(const QString &name,int width=320,int height=180) {
    const auto frames=encodedFrames(name,1,width,height);return frames.isEmpty()?QByteArray{}:frames.first();
}
inline QList<QByteArray> packetize(const QString &codec,const QByteArray &frame,int maximum=900) {
    QList<QByteArray> result;
    if(codec=="VP8") {
        for(qsizetype offset=0;offset<frame.size();offset+=maximum){QByteArray packet(1,offset?char(0):char(0x10));packet+=frame.mid(offset,maximum);result.append(packet);}
        return result;
    }
    if(codec=="AV1") {
        qsizetype cursor=0;Av1Bitstream::Obu obu;bool first=true;
        while(cursor<frame.size()) {
            if(!Av1Bitstream::readObu(frame,cursor,obu))return {};
            if(obu.type==2 || obu.type==8 || obu.type==15)continue;
            QByteArray bytes;bytes+=char(quint8(frame[obu.start])&~2);
            if(obu.headerBytes==2)bytes+=frame[obu.start+1];bytes+=frame.mid(obu.payload,obu.size);
            for(qsizetype offset=0;offset<bytes.size();offset+=maximum-1) {
                const bool end=offset+maximum-1>=bytes.size();
                QByteArray packet(1,char(0x10|(offset?0x80:0)|(end?0:0x40)|(first && !offset && obu.type==1?8:0)));
                packet+=bytes.mid(offset,maximum-1);result.append(packet);
            }
            first=false;
        }
        return result;
    }
    for(const auto &nalu:nalus(frame)) {
        if(nalu.size()<=maximum){result.append(nalu);continue;}
        if(codec=="H265"){
            for(qsizetype offset=2;offset<nalu.size();offset+=maximum-3){
                QByteArray packet;packet+=char((quint8(nalu[0])&0x81)|(49<<1));packet+=nalu[1];
                packet+=char(((quint8(nalu[0])>>1)&63)|(offset==2?0x80:0)|(offset+maximum-3>=nalu.size()?0x40:0));
                packet+=nalu.mid(offset,maximum-3);result.append(packet);
            }
            continue;
        }
        for(qsizetype offset=1;offset<nalu.size();offset+=maximum-2){
            QByteArray packet;packet+=char((quint8(nalu[0])&0xe0)|28);
            packet+=char((quint8(nalu[0])&0x1f)|(offset==1?0x80:0)|(offset+maximum-2>=nalu.size()?0x40:0));
            packet+=nalu.mid(offset,maximum-2);result.append(packet);
        }
    }
    return result;
}
inline QByteArray encryptRtp(const QByteArray &payload,const QByteArray &key,quint8 pt,bool marker,quint16 sequence,quint32 timestamp,quint32 ssrc,quint32 counter) {
    QByteArray packet(12+payload.size()+16+4,Qt::Uninitialized);packet[0]=char(0x80);packet[1]=char(pt|(marker?0x80:0));
    qToBigEndian(sequence,packet.data()+2);qToBigEndian(timestamp,packet.data()+4);qToBigEndian(ssrc,packet.data()+8);
    std::array<unsigned char,24> nonce{};qToLittleEndian(counter,nonce.data());unsigned long long length=0;
    if(crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(packet.data()+12),&length,
        reinterpret_cast<const unsigned char*>(payload.constData()),payload.size(),reinterpret_cast<const unsigned char*>(packet.constData()),12,nullptr,
        nonce.data(),reinterpret_cast<const unsigned char*>(key.constData()))!=0)return {};
    std::memcpy(packet.data()+12+length,nonce.data(),4);return packet;
}
}
