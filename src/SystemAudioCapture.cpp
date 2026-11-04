#include "SystemAudioCapture.h"
#include <pulse/pulseaudio.h>
#include <mutex>
#include <algorithm>

struct SystemAudioCapture::Private {
    pa_threaded_mainloop *loop=nullptr;pa_context *context=nullptr;pa_stream *stream=nullptr;
    std::mutex mutex;Snapshot snapshot;
    void fail(const QString &reason){std::lock_guard lock(mutex);snapshot.error=reason;snapshot.active=false;}
    static void streamState(pa_stream *stream,void *userdata){
        auto *self=static_cast<Private*>(userdata);const auto state=pa_stream_get_state(stream);
        if(state==PA_STREAM_READY){std::lock_guard lock(self->mutex);self->snapshot.active=true;}
        else if(state==PA_STREAM_FAILED || state==PA_STREAM_TERMINATED)self->fail("Computer audio capture stopped. Share again to reconnect it.");
    }
    static void audioReady(pa_stream *stream,size_t,void *userdata){
        auto *self=static_cast<Private*>(userdata);const void *data=nullptr;size_t size=0;
        if(pa_stream_peek(stream,&data,&size)<0){self->fail("Could not read computer audio.");return;}
        if(size){
            constexpr size_t maximum=48000*4/2;const auto keep=std::min(size,maximum)&~size_t(3);
            std::lock_guard lock(self->mutex);
            if(data)self->snapshot.pcm.append(static_cast<const char*>(data)+size-keep,qsizetype(keep));
            else self->snapshot.pcm.append(QByteArray(qsizetype(keep),'\0'));
            if(self->snapshot.pcm.size()>qsizetype(maximum))self->snapshot.pcm.remove(0,self->snapshot.pcm.size()-qsizetype(maximum));
        }
        pa_stream_drop(stream);
    }
    static void sinkInfo(pa_context *context,const pa_sink_info *info,int end,void *userdata){
        auto *self=static_cast<Private*>(userdata);if(self->stream)return;
        if(end || !info || !info->monitor_source_name){if(end)self->fail("The current output has no computer-audio monitor.");return;}
        const pa_sample_spec format{PA_SAMPLE_S16LE,48000,2};
        self->stream=pa_stream_new(context,"Relay screen-share audio",&format,nullptr);
        if(!self->stream){self->fail("Could not initialize computer audio.");return;}
        pa_stream_set_state_callback(self->stream,streamState,self);pa_stream_set_read_callback(self->stream,audioReady,self);
        pa_buffer_attr buffer;buffer.maxlength=48000*4/2;buffer.tlength=uint32_t(-1);buffer.prebuf=uint32_t(-1);buffer.minreq=uint32_t(-1);buffer.fragsize=3840;
        if(pa_stream_connect_record(self->stream,info->monitor_source_name,&buffer,PA_STREAM_ADJUST_LATENCY)<0)self->fail("Could not capture the current output's computer audio.");
    }
    static void serverInfo(pa_context *context,const pa_server_info *info,void *userdata){
        auto *self=static_cast<Private*>(userdata);
        if(!info || !info->default_sink_name){self->fail("No output is available for computer audio.");return;}
        auto *operation=pa_context_get_sink_info_by_name(context,info->default_sink_name,sinkInfo,self);
        if(operation)pa_operation_unref(operation);else self->fail("Could not locate the computer-audio monitor.");
    }
    static void contextState(pa_context *context,void *userdata){
        auto *self=static_cast<Private*>(userdata);const auto state=pa_context_get_state(context);
        if(state==PA_CONTEXT_READY){auto *operation=pa_context_get_server_info(context,serverInfo,self);if(operation)pa_operation_unref(operation);}
        else if(state==PA_CONTEXT_FAILED || state==PA_CONTEXT_TERMINATED)self->fail("Computer audio requires a running PipeWire-Pulse or PulseAudio service.");
    }
};
SystemAudioCapture::SystemAudioCapture():d(std::make_unique<Private>()){}
SystemAudioCapture::~SystemAudioCapture(){stop();}
bool SystemAudioCapture::start(){
    if(d->loop)return true;
    {std::lock_guard lock(d->mutex);d->snapshot={};}
    d->loop=pa_threaded_mainloop_new();if(!d->loop){d->fail("Could not initialize computer audio.");return false;}
    d->context=pa_context_new(pa_threaded_mainloop_get_api(d->loop),"Relay screen sharing");
    if(!d->context){stop();d->fail("Could not initialize computer audio.");return false;}
    pa_context_set_state_callback(d->context,Private::contextState,d.get());
    if(pa_context_connect(d->context,nullptr,PA_CONTEXT_NOAUTOSPAWN,nullptr)<0 || pa_threaded_mainloop_start(d->loop)<0){stop();d->fail("Computer audio requires a running PipeWire-Pulse or PulseAudio service.");return false;}
    return true;
}
void SystemAudioCapture::stop(){
    if(!d->loop)return;
    pa_threaded_mainloop_lock(d->loop);
    if(d->stream){pa_stream_set_read_callback(d->stream,nullptr,nullptr);pa_stream_set_state_callback(d->stream,nullptr,nullptr);pa_stream_disconnect(d->stream);pa_stream_unref(d->stream);d->stream=nullptr;}
    if(d->context){pa_context_set_state_callback(d->context,nullptr,nullptr);pa_context_disconnect(d->context);pa_context_unref(d->context);d->context=nullptr;}
    pa_threaded_mainloop_unlock(d->loop);pa_threaded_mainloop_stop(d->loop);pa_threaded_mainloop_free(d->loop);d->loop=nullptr;
    std::lock_guard lock(d->mutex);d->snapshot.pcm.clear();d->snapshot.active=false;
}
SystemAudioCapture::Snapshot SystemAudioCapture::take(){std::lock_guard lock(d->mutex);auto snapshot=d->snapshot;d->snapshot.pcm.clear();return snapshot;}
