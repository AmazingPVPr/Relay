#include "BroadcastEncoder.h"
#include "BroadcastPacket.h"
#include <QImage>
#include <QScopeGuard>
#include <QThread>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

struct BroadcastEncoder::Private {
    int maximumHeight,fps,bitrate;
    quint64 bitrateGeneration=0;
    std::mutex mutex;std::condition_variable ready;bool stop=false,force=true;
    QVideoFrame pending;Snapshot result;std::thread worker;
    Private(int height,int rate,int bits):maximumHeight(std::clamp(height,360,2160)),fps(std::clamp(rate,15,60)),bitrate(std::clamp(bits,500000,40000000)),worker([this]{run();}){}
    ~Private(){{std::lock_guard lock(mutex);stop=true;pending={};}ready.notify_all();if(worker.joinable())worker.join();}
    void fail(const QString &error){std::lock_guard lock(mutex);result.error=error;}
    void run(){
        int configuredBitrate=0;
        AVCodecContext *encoder=nullptr;AVFrame *frame=av_frame_alloc();AVPacket *packet=av_packet_alloc();SwsContext *scaler=nullptr;
        QVideoFrame latest;QSize size;auto started=std::chrono::steady_clock::now(),last=started-std::chrono::seconds(1);
        if(!frame || !packet){fail("Could not allocate the screen encoder.");av_frame_free(&frame);av_packet_free(&packet);return;}
        while(true){
            bool keyframe=false;int requestedBitrate=0;quint64 requestedGeneration=0;
            {
                std::unique_lock lock(mutex);
                ready.wait_for(lock,std::chrono::milliseconds(1000),[this]{return stop || pending.isValid() || force;});
                if(stop)break;
                const auto next=last+std::chrono::microseconds(1000000/fps);
                ready.wait_until(lock,next,[this]{return stop;});if(stop)break;
                if(pending.isValid()){latest=std::move(pending);pending={};}
                keyframe=force;force=false;
                requestedBitrate=bitrate;requestedGeneration=bitrateGeneration;
            }
            if(!latest.isValid())continue;
            last=std::chrono::steady_clock::now();
            if(latest.width()>16384 || latest.height()>16384 || qint64(latest.width())*latest.height()>67108864){fail("The source video is too large to encode.");break;}
            auto input=latest;
            const bool planar=input.pixelFormat()==QVideoFrameFormat::Format_YUV420P && input.map(QVideoFrame::ReadOnly);
            const auto mapping=qScopeGuard([&]{if(planar)input.unmap();});
            QImage image;
            const uint8_t *pixels[4]={};int strides[4]={};
            if(planar){for(int plane=0;plane<3;++plane){pixels[plane]=input.bits(plane);strides[plane]=input.bytesPerLine(plane);}}
            else{
                image=latest.toImage().convertToFormat(QImage::Format_RGBA8888);
                if(image.isNull()){fail("Could not read the source video frame.");break;}
                pixels[0]=image.constBits();strides[0]=int(image.bytesPerLine());
            }
            const QSize source=planar?input.size():image.size();
            const double scale=std::min({1.0,double(maximumHeight)/source.height(),3840.0/source.width()});
            const QSize wanted(std::max(2,int(source.width()*scale)&~1),std::max(2,int(source.height()*scale)&~1));
            if(!encoder || wanted!=size || configuredBitrate!=requestedBitrate){
                avcodec_free_context(&encoder);av_frame_unref(frame);sws_freeContext(scaler);scaler=nullptr;
                const auto codec=avcodec_find_encoder_by_name("libx264");encoder=codec?avcodec_alloc_context3(codec):nullptr;
                if(!encoder){fail("The H.264 screen encoder is unavailable.");break;}
                size=wanted;encoder->width=size.width();encoder->height=size.height();encoder->pix_fmt=AV_PIX_FMT_YUV420P;
                encoder->time_base={1,90000};encoder->framerate={fps,1};encoder->bit_rate=requestedBitrate;encoder->rc_max_rate=requestedBitrate;encoder->rc_buffer_size=std::max(125000,requestedBitrate/4);
                encoder->gop_size=fps*2;encoder->max_b_frames=0;encoder->thread_count=std::clamp(QThread::idealThreadCount()/2,2,8);encoder->thread_type=FF_THREAD_SLICE;
                encoder->flags|=AV_CODEC_FLAG_LOW_DELAY;
                const auto format=latest.surfaceFormat();
                encoder->color_range=planar && format.colorRange()==QVideoFrameFormat::ColorRange_Full?AVCOL_RANGE_JPEG:AVCOL_RANGE_MPEG;
                encoder->colorspace=planar && format.colorSpace()==QVideoFrameFormat::ColorSpace_BT601?AVCOL_SPC_SMPTE170M:AVCOL_SPC_BT709;
                encoder->color_primaries=encoder->colorspace==AVCOL_SPC_SMPTE170M?AVCOL_PRI_SMPTE170M:AVCOL_PRI_BT709;
                encoder->color_trc=AVCOL_TRC_BT709;
                av_opt_set(encoder->priv_data,"preset","veryfast",0);av_opt_set(encoder->priv_data,"tune","zerolatency",0);
                av_opt_set(encoder->priv_data,"crf","18",0);
                av_opt_set(encoder->priv_data,"profile","high",0);av_opt_set(encoder->priv_data,"forced-idr","1",0);
                av_opt_set(encoder->priv_data,"x264-params","repeat-headers=1:annexb=1:aud=1:scenecut=0",0);
                if(avcodec_open2(encoder,codec,nullptr)<0){fail("Could not start the H.264 screen encoder.");break;}
                frame->format=encoder->pix_fmt;frame->width=encoder->width;frame->height=encoder->height;
                if(av_frame_get_buffer(frame,32)<0){fail("Could not allocate a screen encoding frame.");break;}
                configuredBitrate=requestedBitrate;
                keyframe=true;
            }
            if(av_frame_make_writable(frame)<0){fail("Screen encoder allocation failed.");break;}
            scaler=sws_getCachedContext(scaler,source.width(),source.height(),planar?AV_PIX_FMT_YUV420P:AV_PIX_FMT_RGBA,size.width(),size.height(),AV_PIX_FMT_YUV420P,SWS_LANCZOS,nullptr,nullptr,nullptr);
            if(!scaler){fail("Could not convert the screen image.");break;}
            const auto coefficients=sws_getCoefficients(encoder->colorspace==AVCOL_SPC_SMPTE170M?SWS_CS_SMPTE170M:SWS_CS_ITU709);
            sws_setColorspaceDetails(scaler,coefficients,planar?encoder->color_range==AVCOL_RANGE_JPEG:1,
                coefficients,encoder->color_range==AVCOL_RANGE_JPEG,0,1<<16,1<<16);
            sws_scale(scaler,pixels,strides,0,source.height(),frame->data,frame->linesize);
            frame->color_range=encoder->color_range;frame->colorspace=encoder->colorspace;
            frame->color_primaries=encoder->color_primaries;frame->color_trc=encoder->color_trc;
            const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(last-started).count();
            frame->pts=elapsed*90/1000;frame->pict_type=keyframe?AV_PICTURE_TYPE_I:AV_PICTURE_TYPE_NONE;
            if(avcodec_send_frame(encoder,frame)<0){fail("Screen encoder rejected a frame.");break;}
            while(avcodec_receive_packet(encoder,packet)>=0){
                const auto bytes=BroadcastPacket::normalizeH264(QByteArray(reinterpret_cast<const char*>(packet->data),packet->size));
                const Frame encoded{bytes,quint32(packet->pts),size.width(),size.height(),bool(packet->flags&AV_PKT_FLAG_KEY)};
                av_packet_unref(packet);
                if(bytes.isEmpty() || bytes.size()>8*1024*1024){fail("The screen encoder produced an oversized frame.");continue;}
                std::lock_guard lock(mutex);
                // A rate change can arrive during a costly frame. Its old
                // bitstream must not escape after the transport accepted a
                // lower receiver budget. The new context starts with an IDR.
                if(requestedGeneration!=bitrateGeneration){force=true;continue;}
                if(result.frames.size()>=3){result.dropped+=result.frames.size();result.frames.clear();force=true;}
                result.frames.append(encoded);++result.encoded;
            }
        }
        sws_freeContext(scaler);av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&encoder);
    }
};
BroadcastEncoder::BroadcastEncoder(int height,int fps,int bitrate):d(std::make_unique<Private>(height,fps,bitrate)){}
BroadcastEncoder::~BroadcastEncoder()=default;
bool BroadcastEncoder::available(){return avcodec_find_encoder_by_name("libx264")!=nullptr;}
void BroadcastEncoder::submit(const QVideoFrame &frame){if(!frame.isValid())return;{std::lock_guard lock(d->mutex);if(d->stop)return;if(d->pending.isValid())++d->result.dropped;d->pending=frame;}d->ready.notify_one();}
void BroadcastEncoder::setBitrate(int bitrate){
    bitrate=std::clamp(bitrate,500000,40000000);
    {
        std::lock_guard lock(d->mutex);
        // Keep a little headroom below a changed upper limit so harmless
        // receiver-estimate jitter does not repeatedly rebuild the codec.
        // Ignoring a change is safe only while the current encoder remains
        // below the newest limit; every actual reduction is honored.
        if(bitrate>=d->bitrate && qint64(bitrate)*100<qint64(d->bitrate)*105)return;
        const int target=int(std::max<qint64>(500000,qint64(bitrate)*98/100));
        if(d->bitrate==target)return;
        d->bitrate=target;++d->bitrateGeneration;
        d->result.dropped+=d->result.frames.size();d->result.frames.clear();
        d->force=true;
    }
    d->ready.notify_one();
}
void BroadcastEncoder::requestKeyframe(){{std::lock_guard lock(d->mutex);d->force=true;}d->ready.notify_one();}
BroadcastEncoder::Snapshot BroadcastEncoder::take(){std::lock_guard lock(d->mutex);auto snapshot=d->result;d->result.frames.clear();return snapshot;}
