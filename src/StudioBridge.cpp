#include "StudioBridge.h"
#include "ObsInputStore.h"

#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QVideoFrameFormat>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QUrl>
#include <QVideoFrame>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <deque>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace {
QString avError(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return QString::fromUtf8(text);
}
enum H264Headers { H264Idr = 1, H264Sps = 2, H264Pps = 4 };
int h264Headers(const uint8_t *data, int size) {
    if (!data || size < 5 || size > 8 * 1024 * 1024) return 0;
    int found = 0;
    for (int at = 0; at + 3 < size;) {
        int prefix = 0;
        if (!data[at] && !data[at + 1]) {
            if (data[at + 2] == 1) prefix = 3;
            else if (at + 4 < size && !data[at + 2] && data[at + 3] == 1) prefix = 4;
        }
        if (!prefix) { ++at; continue; }
        if (at + prefix >= size) break;
        const auto type = data[at + prefix] & 31;
        if (type == 5) found |= H264Idr;
        else if (type == 7) found |= H264Sps;
        else if (type == 8) found |= H264Pps;
        at += prefix + 1;
    }
    return found;
}
}

class StudioWorker final : public QThread {
public:
    struct Snapshot {
        QVideoFrame frame;
        QString error, diagnostic, videoCodec, audioCodec, resolution;
        qint64 packets = 0;
    };
    explicit StudioWorker(QString input, bool listen) : url(std::move(input)), listen(listen) {}
    ~StudioWorker() override { cancel.store(true); wait(); }
    std::atomic_bool cancel{false};
    std::atomic<qint64> videoPackets{0}, decodedVideoFrames{0}, convertedVideoFrames{0};
    std::atomic<qint64> replayedVideoPackets{0}, replayRestores{0}, replayLimitRestores{0}, maxReplayWorkUs{0};
    std::atomic<qint64> backgroundReplayChunks{0}, maxBackgroundReplayWorkUs{0}, backgroundBudgetOverruns{0};
    std::atomic<qint64> replayBytes{0}, replayPackets{0}, maximumReplayBytes{0}, maximumReplayPackets{0};
    QVariantMap processingDiagnostics() const {
        return {{"previewAttached",previewEnabled.load()},{"videoPackets",videoPackets.load()},{"decodedVideoFrames",decodedVideoFrames.load()},
            {"convertedVideoFrames",convertedVideoFrames.load()},{"replayedVideoPackets",replayedVideoPackets.load()},
            {"replayRestores",replayRestores.load()},{"replayLimitRestores",replayLimitRestores.load()},{"maxReplayWorkUs",maxReplayWorkUs.load()},
            {"backgroundReplayChunks",backgroundReplayChunks.load()},{"maxBackgroundReplayWorkUs",maxBackgroundReplayWorkUs.load()},
            {"backgroundBudgetOverruns",backgroundBudgetOverruns.load()},
            {"replayBytes",replayBytes.load()},{"replayPackets",replayPackets.load()},
            {"maximumReplayBytes",maximumReplayBytes.load()},{"maximumReplayPackets",maximumReplayPackets.load()}};
    }
    std::atomic_bool previewEnabled{false};
    void configurePreview(bool enabled) {
        previewEnabled.store(enabled);
        if (!enabled) { QMutexLocker lock(&mutex); latest.frame = {}; }
    }
    void configureBroadcast(bool enabled, int height, int fps, bool audio) {
        QMutexLocker lock(&mutex);
        originalQuality.store(enabled && height == 0 && fps == 0);
        maximumHeight.store(height == 0 ? 2160 : height); frameRate.store(fps == 0 ? 60 : fps); audioEnabled.store(enabled && audio);
        broadcastEnabled.store(enabled);
        ++captureEpoch;
        outgoing = {};
        encoded = {}; encodedBytes = 0;
    }
    void configureParallelOriginal(bool enabled) {
        QMutexLocker lock(&mutex);parallelOriginal.store(enabled);encoded={};encodedBytes=0;
    }
    Snapshot take() {
        QMutexLocker lock(&mutex);
        auto snapshot = latest;
        snapshot.packets = videoPackets.load();
        latest.frame = {}; latest.diagnostic.clear();
        return snapshot;
    }
    RelayBroadcastMedia takeBroadcast() {
        QMutexLocker lock(&mutex);
        auto snapshot = outgoing;
        outgoing.video = {}; outgoing.audio.clear();
        return snapshot;
    }
    RelayEncodedVideo takeEncoded() {
        QMutexLocker lock(&mutex);
        auto snapshot = encoded;
        encoded.frames.clear(); encodedBytes = 0;
        return snapshot;
    }
protected:
    void run() override {
        AVFormatContext *format = avformat_alloc_context();
        if (!format) { fail("Could not allocate the OBS receiver."); return; }
        format->interrupt_callback = {[](void *opaque) -> int { return static_cast<StudioWorker *>(opaque)->cancel.load() ? 1 : 0; }, this};
        format->probesize = 1024 * 1024;
        format->max_analyze_duration = 2 * AV_TIME_BASE;
        AVDictionary *options = nullptr;
        if (listen) av_dict_set(&options, "listen", "1", 0);
        else {
            av_dict_set(&options, "rtmp_live", "live", 0);
            av_dict_set(&options, "rtmp_buffer", "250", 0);
            av_dict_set(&options, "rw_timeout", "5000000", 0);
            av_dict_set(&options, "tls_verify", "1", 0);
        }
        av_dict_set(&options, "tcp_nodelay", "1", 0);
        av_dict_set(&options, "protocol_whitelist", "rtmp,rtmps,tcp,tls", 0);
        av_log_set_level(AV_LOG_QUIET);
        const auto encodedUrl = url.toUtf8();
        int result = avformat_open_input(&format, encodedUrl.constData(), av_find_input_format("flv"), &options);
        av_dict_free(&options);
        if (result < 0) {
            if (!cancel.load()) fail(listen
                ? QString("Could not receive OBS: %1. Check that port 45990 is free, then try again.").arg(avError(result))
                : QString("Could not open the RTMP stream: %1. Check the server, stream key, playback access, and that OBS is streaming.").arg(avError(result)));
            if (format) avformat_close_input(&format);
            return;
        }
        result = avformat_find_stream_info(format, nullptr);
        const int videoIndex = result >= 0 ? av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : result;
        if (videoIndex < 0) { if (!cancel.load()) fail("No video track was found. Use H.264 video in OBS."); avformat_close_input(&format); return; }
        auto openDecoder = [&](int index) -> AVCodecContext * {
            const auto parameters = format->streams[index]->codecpar;
            const auto codec = avcodec_find_decoder(parameters->codec_id);
            auto context = codec ? avcodec_alloc_context3(codec) : nullptr;
            if (!context) return nullptr;
            context->thread_count = 2;
            context->thread_type = FF_THREAD_SLICE;
            context->max_pixels = 8192LL * 4320;
            if (avcodec_parameters_to_context(context, parameters) < 0 || avcodec_open2(context, codec, nullptr) < 0) { avcodec_free_context(&context); return nullptr; }
            return context;
        };
        AVCodecContext *video = openDecoder(videoIndex);
        if (!video) { fail("This OBS video codec cannot be opened. Select H.264 in OBS."); avformat_close_input(&format); return; }
        const int audioIndex = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        AVCodecContext *audio = audioIndex >= 0 ? openDecoder(audioIndex) : nullptr;
        const auto stream = format->streams[videoIndex];
        const auto sourceRate = av_guess_frame_rate(format, stream, nullptr);
        const double sourceFps = av_q2d(sourceRate);
        // Only forward formats native Discord H.264 receivers can decode.
        // Other inputs use the high-quality encoder, at up to 4K/60.
        const int profile = video->profile & ~(AV_PROFILE_H264_CONSTRAINED | AV_PROFILE_H264_INTRA);
        const bool compatible = video->codec_id == AV_CODEC_ID_H264
            && (profile == AV_PROFILE_H264_BASELINE || profile == AV_PROFILE_H264_MAIN || profile == AV_PROFILE_H264_HIGH)
            && video->has_b_frames == 0 && stream->codecpar->video_delay == 0
            && video->width >= 2 && video->width <= 3840 && video->height >= 2 && video->height <= 2160
            && sourceFps > 0 && sourceFps <= 60.01;
        AVBSFContext *annexB = nullptr;
        const auto filter = av_bsf_get_by_name("h264_mp4toannexb");
        if (compatible && filter && av_bsf_alloc(filter, &annexB) >= 0) {
            if (avcodec_parameters_copy(annexB->par_in, stream->codecpar) < 0) av_bsf_free(&annexB);
            else { annexB->time_base_in = stream->time_base; if (av_bsf_init(annexB) < 0) av_bsf_free(&annexB); }
        }
        {
            QMutexLocker lock(&mutex);
            latest.resolution = QString("%1 × %2").arg(video->width).arg(video->height);
            latest.videoCodec = QString::fromUtf8(avcodec_get_name(video->codec_id)).toUpper();
            if (audioIndex >= 0) latest.audioCodec = QString::fromUtf8(avcodec_get_name(format->streams[audioIndex]->codecpar->codec_id)).toUpper();
        }
        AVPacket *packet = av_packet_alloc();
        AVPacket *filtered = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        SwsContext *scaler = nullptr;
        SwrContext *resampler = nullptr;
        AVChannelLayout inputLayout{};
        int inputRate = 0, inputFormat = -1;
        bool wasAudioEnabled = false;
        QElapsedTimer clock; clock.start();
        qint64 previousFrame = -100;
        quint64 encodedEpoch = 0;
        bool waitingForIdr = true;
        qint64 firstDts = AV_NOPTS_VALUE;
        qint64 pacingStamp = AV_NOPTS_VALUE, pacingWall = 0, previousPacketStamp = AV_NOPTS_VALUE;
        const auto pacePacket = [&](const AVPacket *input) {
            const auto stamp = input->dts != AV_NOPTS_VALUE ? input->dts : input->pts;
            if (stamp == AV_NOPTS_VALUE || input->stream_index < 0 || input->stream_index >= int(format->nb_streams)) return;
            const auto mediaUs = av_rescale_q(stamp, format->streams[input->stream_index]->time_base, AVRational{1,1000000});
            auto now = clock.nsecsElapsed() / 1000;
            // RTMP servers and stream probing can hand us many frames at once.
            // Replaying that batch at CPU speed overflows the encoded mailbox
            // and drops a dependent chain, even while the preview looks healthy.
            // Use the media clock for both tracks; discontinuities and network
            // stalls establish a new wall-clock anchor instead of a long wait
            // or a catch-up burst. Cancellation remains bounded to two ms.
            if (pacingStamp == AV_NOPTS_VALUE || (previousPacketStamp != AV_NOPTS_VALUE
                && (mediaUs < previousPacketStamp - 250000 || mediaUs > previousPacketStamp + 1000000))) {
                pacingStamp = mediaUs; pacingWall = now;
            }
            auto due = pacingWall + mediaUs - pacingStamp;
            if (now - due > 200000) { pacingWall += now - due; due = now; }
            while (!cancel.load() && due > now) {
                QThread::usleep(ulong(std::min<qint64>(2000, due - now)));
                now = clock.nsecsElapsed() / 1000;
            }
            previousPacketStamp = mediaUs;
        };
        const auto decodeVideo = [&](const AVPacket *input, bool render, quint64 epoch) {
            if (avcodec_send_packet(video, input) >= 0) {
                while (avcodec_receive_frame(video, frame) >= 0) {
                    ++decodedVideoFrames;
                    if (frame->width < 2 || frame->height < 2 || frame->width > 8192 || frame->height > 8192 || qint64(frame->width) * frame->height > 8192LL * 4320) {
                        fail("The OBS video resolution exceeds the supported limit."); cancel.store(true); break;
                    }
                    { QMutexLocker lock(&mutex); latest.resolution = QString("%1 × %2").arg(frame->width).arg(frame->height); }
                    if (!render) { av_frame_unref(frame); continue; }
                    const bool broadcasting = broadcastEnabled.load();
                    const qint64 now = clock.elapsed();
                    const bool forwarding = broadcasting && originalQuality.load() && annexB;
                    const int fps = broadcasting && !forwarding
                        ? (originalQuality.load() && sourceFps > 0 && sourceFps <= 60.01
                            ? std::clamp(int(std::lround(sourceFps)), 1, 60) : frameRate.load()) : 30;
                    if (now - previousFrame < 1000.0 / fps - 1) { av_frame_unref(frame); continue; }
                    previousFrame = now;
                    const double scale = broadcasting && !forwarding
                        ? std::min({1.0, double(maximumHeight.load()) / frame->height, 3840.0 / frame->width})
                        : std::min({1.0, 540.0 / frame->height, 960.0 / frame->width});
                    const int width = std::max(2, int(frame->width * scale) & ~1);
                    const int height = std::max(2, int(frame->height * scale) & ~1);
                    QVideoFrameFormat videoFormat(QSize(width, height), QVideoFrameFormat::Format_YUV420P);
                    videoFormat.setColorSpace(frame->colorspace==AVCOL_SPC_BT470BG || frame->colorspace==AVCOL_SPC_SMPTE170M
                        ? QVideoFrameFormat::ColorSpace_BT601 : QVideoFrameFormat::ColorSpace_BT709);
                    videoFormat.setColorRange(frame->color_range==AVCOL_RANGE_JPEG?QVideoFrameFormat::ColorRange_Full:QVideoFrameFormat::ColorRange_Video);
                    QVideoFrame output(videoFormat);
                    if (!output.map(QVideoFrame::WriteOnly)) { fail("Could not allocate the OBS video frame."); cancel.store(true); break; }
                    scaler = sws_getCachedContext(scaler, frame->width, frame->height, AVPixelFormat(frame->format), width, height, AV_PIX_FMT_YUV420P, broadcasting && !forwarding ? SWS_LANCZOS : SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    if (!scaler) { output.unmap(); fail("Could not convert the OBS video."); cancel.store(true); break; }
                    uint8_t *planes[] = {output.bits(0), output.bits(1), output.bits(2), nullptr};
                    const int strides[] = {output.bytesPerLine(0), output.bytesPerLine(1), output.bytesPerLine(2), 0};
                    ++convertedVideoFrames;
                    sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
                    output.unmap();
                    {
                        QMutexLocker lock(&mutex);
                        if (previewEnabled.load()) latest.frame = output;
                        latest.resolution = QString("%1 × %2").arg(frame->width).arg(frame->height);
                        if (broadcasting && !forwarding && broadcastEnabled.load() && epoch == captureEpoch.load()) outgoing.video = output;
                    }
                    av_frame_unref(frame);
                }
            }
        };
        struct PacketDeleter { void operator()(AVPacket *packet) const { av_packet_free(&packet); } };
        std::deque<std::unique_ptr<AVPacket, PacketDeleter>> replay;
        qsizetype retainedBytes = 0;
        bool decoderSuspended = false, backgroundReplay = false;
        quint64 replayEpoch = 0;
        constexpr qsizetype maximumReplaySize = 16 * 1024 * 1024;
        constexpr size_t maximumReplayFrames = 180;
        const auto clearReplay = [&] {
            replay.clear(); retainedBytes = 0;
            replayBytes.store(0); replayPackets.store(0);
        };
        const auto retainPacket = [&](const AVPacket *input) {
            // Check before cloning: no transient unbounded GOP or packet copy.
            if (input->size <= 0 || replay.size() >= maximumReplayFrames
                    || input->size > maximumReplaySize - retainedBytes) return false;
            auto copy = std::unique_ptr<AVPacket, PacketDeleter>(av_packet_clone(input));
            if (!copy) return false;
            retainedBytes += input->size; replay.push_back(std::move(copy));
            replayBytes.store(retainedBytes); replayPackets.store(qint64(replay.size()));
            maximumReplayBytes.store(std::max(maximumReplayBytes.load(), qint64(retainedBytes)));
            maximumReplayPackets.store(std::max(maximumReplayPackets.load(), qint64(replay.size())));
            return true;
        };
        const auto replayOne = [&](quint64 epoch) {
            const auto bytes = replay.front()->size;
            decodeVideo(replay.front().get(), false, epoch); ++replayedVideoPackets;
            replay.pop_front(); retainedBytes -= bytes;
            replayBytes.store(retainedBytes); replayPackets.store(qint64(replay.size()));
        };
        const auto replayFinished = [&] {
            if (!replay.empty()) return;
            decoderSuspended = false; backgroundReplay = false;
            previousFrame = -100;
        };
        const auto restoreDecoder = [&](quint64 epoch, bool limitReached) {
            if (!decoderSuspended) return;
            QElapsedTimer replayWork; replayWork.start();
            // A background catch-up already restored the IDR and its prefix.
            // Flushing here would destroy that state and make the suffix invalid.
            if (!backgroundReplay) { avcodec_flush_buffers(video); ++replayRestores; }
            if (limitReached) ++replayLimitRestores;
            while (!replay.empty() && !cancel.load()) replayOne(epoch);
            maxReplayWorkUs.store(std::max(maxReplayWorkUs.load(), replayWork.nsecsElapsed() / 1000));
            clearReplay(); decoderSuspended = false; backgroundReplay = false;
            previousFrame = -100;
        };
        const auto warmDecoder = [&](quint64 epoch, int incomingBytes) {
            if (!decoderSuspended) return;
            if (!backgroundReplay) {
                avcodec_flush_buffers(video); ++replayRestores; ++replayLimitRestores;
                backgroundReplay = true;
            }
            QElapsedTimer work; work.start();
            const auto hasRoom = [&] {
                return replay.size() < maximumReplayFrames && incomingBytes <= maximumReplaySize - retainedBytes;
            };
            bool overBudget = false;
            while (!replay.empty() && !cancel.load()) {
                if (work.nsecsElapsed() >= 8000000) {
                    if (hasRoom()) break;
                    // Exceptional oversized packets cannot be cloned past the
                    // hard cap. Preserve the decoder chain while freeing space.
                    overBudget = true;
                }
                replayOne(epoch);
            }
            if (overBudget) ++backgroundBudgetOverruns;
            ++backgroundReplayChunks;
            maxBackgroundReplayWorkUs.store(std::max(maxBackgroundReplayWorkUs.load(), work.nsecsElapsed() / 1000));
            replayFinished();
        };
        if (!packet || !filtered || !frame) fail("Could not allocate the OBS media buffers.");
        else while (!cancel.load()) {
            result = av_read_frame(format, packet);
            if (result < 0) break;
            pacePacket(packet);
            if (cancel.load()) { av_packet_unref(packet); break; }
            const auto epoch = captureEpoch.load();
            const bool wantsAudio = audioEnabled.load();
            if (wasAudioEnabled && !wantsAudio) {
                if (audio) avcodec_flush_buffers(audio);
                swr_free(&resampler); av_channel_layout_uninit(&inputLayout); inputRate = 0;
            }
            wasAudioEnabled = wantsAudio;
            const bool hiddenOriginal = broadcastEnabled.load() && originalQuality.load() && annexB && !previewEnabled.load();
            if (packet->stream_index == videoIndex) {
                // Restore before examining a new IDR, so changing capture mode
                // cannot replace new resolution metadata with an old GOP.
                if (decoderSuspended && (epoch != replayEpoch || !hiddenOriginal))
                    restoreDecoder(epoch, false);
                ++videoPackets;
                QMutexLocker lock(&mutex);
                if (epoch == captureEpoch.load()) {
                    encoded.available = (originalQuality.load() || parallelOriginal.load()) && broadcastEnabled.load() && annexB;
                    encoded.width = video->width; encoded.height = video->height;
                    encoded.fps = sourceFps > 0 && sourceFps <= 60.01
                        ? std::clamp(int(std::lround(sourceFps)), 1, 60) : 0;
                }
            }
            bool confirmedIdr = false;
            if (packet->stream_index == videoIndex && annexB) {
                const bool original = (originalQuality.load() || parallelOriginal.load()) && broadcastEnabled.load();
                if (encodedEpoch != epoch) { encodedEpoch = epoch; waitingForIdr = true; firstDts = AV_NOPTS_VALUE; }
                if (original && av_packet_ref(filtered, packet) >= 0 && av_bsf_send_packet(annexB, filtered) >= 0) {
                    while (av_bsf_receive_packet(annexB, filtered) >= 0) {
                        // Container key flags also include recovery points and
                        // non-IDR I pictures. A new Discord/WebRTC viewer needs
                        // an actual IDR with its parameter sets in the same AU.
                        const int headers = h264Headers(filtered->data, filtered->size);
                        const bool idr = headers == (H264Idr | H264Sps | H264Pps);
                        if (idr && !confirmedIdr && hiddenOriginal) {
                            // One independent frame per GOP keeps dimensions
                            // current if OBS changes SPS/resolution while hidden.
                            avcodec_flush_buffers(video);
                            decodeVideo(packet, false, epoch);
                        }
                        confirmedIdr = confirmedIdr || idr;
                        if ((headers & H264Idr) && !idr) waitingForIdr = true;
                        const auto pts = filtered->pts, dts = filtered->dts;
                        if (pts != AV_NOPTS_VALUE && dts != AV_NOPTS_VALUE && (!waitingForIdr || idr)) {
                            if (firstDts == AV_NOPTS_VALUE) firstDts = dts;
                            const auto stamp = av_rescale_q(pts - firstDts, stream->time_base, AVRational{1,90000});
                            QMutexLocker lock(&mutex);
                            if (epoch == captureEpoch.load() && broadcastEnabled.load()) {
                                // A stalled consumer must resume on an IDR, never
                                // drop an arbitrary P/B frame then send its dependents.
                                if (encoded.frames.size() >= 16 || encodedBytes + filtered->size > 8 * 1024 * 1024) {
                                    encoded.frames.clear(); encodedBytes = 0; waitingForIdr = true;
                                }
                                if (filtered->size > 0 && filtered->size <= 8 * 1024 * 1024 && (!waitingForIdr || idr)) {
                                    encoded.width = video->width; encoded.height = video->height;
                                    encoded.frames.append({QByteArray(reinterpret_cast<const char *>(filtered->data), filtered->size), quint32(stamp), idr});
                                    encodedBytes += filtered->size; waitingForIdr = false;
                                }
                            }
                        }
                        av_packet_unref(filtered);
                    }
                }
                av_packet_unref(filtered);
            }
            if (packet->stream_index == videoIndex) {
                if (hiddenOriginal && confirmedIdr) {
                    clearReplay();
                    avcodec_flush_buffers(video);
                    decoderSuspended = retainPacket(packet);
                    backgroundReplay = false;
                    replayEpoch = epoch;
                    if (!decoderSuspended) decodeVideo(packet, false, epoch);
                } else if (hiddenOriginal && decoderSuspended) {
                    // Start before the hard cap, leaving room for live packets
                    // while the cached decoder catches up in short batches.
                    // A normal two-second/60fps GOP ends before this threshold.
                    if (backgroundReplay || replay.size() >= 120 || retainedBytes + packet->size > 8 * 1024 * 1024)
                        warmDecoder(epoch, packet->size);
                    if (!decoderSuspended) decodeVideo(packet, false, epoch);
                    else if (!retainPacket(packet)) {
                        // Allocation failure or an individually oversized packet:
                        // do not drop part of the decoder's dependent chain.
                        restoreDecoder(epoch, true); decodeVideo(packet, false, epoch);
                    }
                } else decodeVideo(packet, !hiddenOriginal, epoch);
            } else if (packet->stream_index == audioIndex && wantsAudio) {
                if (!audio) { QMutexLocker lock(&mutex); outgoing.audioError = "Video is sharing, but this stream's audio codec is unavailable."; }
                else if (avcodec_send_packet(audio, packet) >= 0) {
                    while (avcodec_receive_frame(audio, frame) >= 0) {
                        if (frame->sample_rate <= 0 || frame->nb_samples <= 0 || frame->nb_samples > 48000 || !frame->ch_layout.nb_channels || frame->ch_layout.nb_channels > 32) { av_frame_unref(frame); continue; }
                        if (!resampler || inputRate != frame->sample_rate || inputFormat != frame->format || av_channel_layout_compare(&inputLayout, &frame->ch_layout)) {
                            swr_free(&resampler); av_channel_layout_uninit(&inputLayout);
                            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                            if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, 48000, &frame->ch_layout, AVSampleFormat(frame->format), frame->sample_rate, 0, nullptr) < 0 || swr_init(resampler) < 0) {
                                swr_free(&resampler); QMutexLocker lock(&mutex); outgoing.audioError = "Video is sharing, but stream audio could not be prepared."; av_frame_unref(frame); continue;
                            }
                            av_channel_layout_copy(&inputLayout, &frame->ch_layout); inputRate = frame->sample_rate; inputFormat = frame->format;
                        }
                        const int capacity = swr_get_out_samples(resampler, frame->nb_samples);
                        if (capacity > 0 && capacity <= 48000) {
                            QByteArray pcm(capacity * 4, Qt::Uninitialized);
                            uint8_t *target[] = {reinterpret_cast<uint8_t *>(pcm.data())};
                            const int samples = swr_convert(resampler, target, capacity, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
                            if (samples > 0) {
                                pcm.resize(samples * 4);
                                QMutexLocker lock(&mutex);
                                if (audioEnabled.load() && epoch == captureEpoch.load()) {
                                    outgoing.audio += pcm;
                                    constexpr int maximumAudio = 48000 * 4 / 5;
                                    if (outgoing.audio.size() > maximumAudio) outgoing.audio.remove(0, outgoing.audio.size() - maximumAudio);
                                }
                            }
                        }
                        av_frame_unref(frame);
                    }
                }
            }
            // Audio is decoded/published before each bounded background batch.
            // Video batches above also leave source forwarding independent.
            if (backgroundReplay && packet->stream_index != videoIndex)
                warmDecoder(epoch, 0);
            av_packet_unref(packet);
        }
        if (!cancel.load() && result == AVERROR(EPIPE)) {
            QMutexLocker lock(&mutex);
            latest.diagnostic = QStringLiteral("Stream connection closed (broken pipe); the OBS input ended.");
        } else if (!cancel.load() && result != AVERROR_EOF) fail(QString("The stream stopped sending: %1").arg(avError(result)));
        clearReplay();
        av_packet_free(&packet); av_packet_free(&filtered); av_bsf_free(&annexB); av_frame_free(&frame); sws_freeContext(scaler); swr_free(&resampler); av_channel_layout_uninit(&inputLayout);
        avcodec_free_context(&video); avcodec_free_context(&audio); avformat_close_input(&format);
    }
private:
    void fail(const QString &error) { QMutexLocker lock(&mutex); latest.error = error; outgoing.error = error; }
    QString url;
    bool listen;
    std::atomic_bool broadcastEnabled{false}, audioEnabled{false}, originalQuality{false}, parallelOriginal{false};
    std::atomic_int maximumHeight{1080}, frameRate{30};
    std::atomic<quint64> captureEpoch{0};
    QMutex mutex;
    Snapshot latest;
    RelayBroadcastMedia outgoing;
    RelayEncodedVideo encoded;
    qsizetype encodedBytes = 0;
};

StudioBridge::StudioBridge(QObject* parent) : QObject(parent) {
    m_previewTimer.setInterval(33);
    connect(&m_previewTimer, &QTimer::timeout, this, &StudioBridge::pollFrame);
    m_inputSaveTimer.setSingleShot(true); m_inputSaveTimer.setInterval(500);
    connect(&m_inputSaveTimer, &QTimer::timeout, this, &StudioBridge::retryInputSave);
}

StudioBridge::~StudioBridge() { m_inputSaveTimer.stop(); stop(); }

void StudioBridge::enableInputPersistence() {
    if (m_inputStore) return;
    m_inputStore = std::make_unique<ObsInputStore>();
    connect(m_inputStore.get(), &ObsInputStore::changed, this, &StudioBridge::inputStorageChanged);
    connect(m_inputStore.get(), &ObsInputStore::restored, this, [this](int mode, const QString &server, const QString &key) {
        // A restored address is prefill only. In particular, no receiver,
        // external connection or Discord broadcast is started here.
        if (m_running) return;
        m_inputMode = InputMode(mode); m_remoteServer = server; m_remoteStreamKey = key;
        emit configurationChanged(); emit changed();
    });
    m_inputStore->restore(); emit inputStorageChanged();
}
bool StudioBridge::rememberInput() const { return m_inputStore && m_inputStore->remember(); }
bool StudioBridge::inputSaved() const { return m_inputStore && m_inputStore->saved(); }
bool StudioBridge::inputStorageBusy() const { return m_inputStore && m_inputStore->busy(); }
QString StudioBridge::inputStorageStatus() const { return m_inputStore ? m_inputStore->status() : "Input is kept only for this session"; }
QString StudioBridge::inputStorageError() const { return m_inputStore ? m_inputStore->error() : QString{}; }
void StudioBridge::setRememberInput(bool remember) {
    if (!m_inputStore) return;
    m_inputStore->setRemember(remember);
    if (remember) retryInputSave(); else m_inputSaveTimer.stop();
}
void StudioBridge::forgetSavedInput() { m_inputSaveTimer.stop(); if (m_inputStore) m_inputStore->clear(); }
void StudioBridge::retryInputSave() {
    m_inputSaveTimer.stop();
    if (m_inputStore && m_inputStore->remember()) m_inputStore->save(int(m_inputMode), m_remoteServer, m_remoteStreamKey);
}
void StudioBridge::inputEdited() {
    if (!m_inputStore) return;
    m_inputStore->cancelRestore();
    if (m_inputStore->remember()) m_inputSaveTimer.start();
}

void StudioBridge::setVideoSink(QObject* sink) {
    auto* videoSink = qobject_cast<QVideoSink*>(sink);
    if (m_sink == videoSink) return;
    QObject::disconnect(m_sinkDestroyed);
    m_sink = videoSink;
    if (m_worker) m_worker->configurePreview(videoSink != nullptr);
    if (videoSink) {
        m_stopWhenBroadcastEnds = false;
        m_sinkDestroyed = connect(videoSink, &QObject::destroyed, this, [this] {
            m_sink = nullptr;
            if (m_worker) m_worker->configurePreview(false);
            m_stopWhenBroadcastEnds = m_broadcastCapture || m_broadcastContext.value("active").toBool();
            emit videoSinkChanged();
            if (!m_stopWhenBroadcastEnds) stop();
        });
    }
    emit videoSinkChanged();
}

void StudioBridge::releasePreview(QObject* sink) {
    // Only the editor that owns the currently attached preview can release it.
    // A closed editor may retain input for its running share, but it must not
    // leave a hidden RTMP decoder running when that share later ends.
    if (!sink || m_sink != qobject_cast<QVideoSink*>(sink)) return;
    m_stopWhenBroadcastEnds = m_broadcastCapture || m_broadcastContext.value("active").toBool();
    setVideoSink(nullptr);
    if (!m_stopWhenBroadcastEnds) stop();
}

void StudioBridge::setInputMode(InputMode mode) {
    if (m_running || (mode != DirectFromObs && mode != ExistingRtmpStream)) return;
    if (mode == m_inputMode) { inputEdited(); return; }
    m_inputMode = mode;
    inputEdited();
    m_error.clear();
    emit configurationChanged();
    emit changed();
}

void StudioBridge::setRemoteServer(const QString &server) {
    if (m_running) return;
    if (server == m_remoteServer) { inputEdited(); return; }
    m_remoteServer = server;
    inputEdited();
    m_error.clear();
    emit configurationChanged();
    emit changed();
}

void StudioBridge::setRemoteStreamKey(const QString &key) {
    if (m_running) return;
    if (key == m_remoteStreamKey) { inputEdited(); return; }
    m_remoteStreamKey = key;
    inputEdited();
    m_error.clear();
    emit configurationChanged();
    emit changed();
}

QString StudioBridge::remoteInputUrl() const {
    const auto server = m_remoteServer.trimmed();
    const auto key = m_remoteStreamKey.trimmed();
    if (server.isEmpty() || server.size() > 4096 || key.size() > 2048) return {};
    for (const auto &part : {server, key})
        for (const auto c : part) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return {};
    QUrl url(server, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "rtmp" && url.scheme() != "rtmps") || url.host().isEmpty() ||
        url.hasFragment() || url.port() == 0 || url.port() > 65535) return {};
    if (!key.isEmpty()) {
        auto path = url.path(QUrl::FullyEncoded);
        while (path.endsWith('/')) path.chop(1);
        const int queryStart = key.indexOf('?');
        const auto playpath = queryStart < 0 ? key : key.left(queryStart);
        if (playpath.isEmpty() || playpath.startsWith('/')) return {};
        url.setPath(path + '/' + playpath, QUrl::TolerantMode);
        if (queryStart >= 0) {
            const auto query = key.mid(queryStart + 1);
            url.setQuery(url.hasQuery() ? url.query(QUrl::FullyEncoded) + '&' + query : query, QUrl::TolerantMode);
        }
    }
    if (url.path().isEmpty() || url.path() == "/") return {};
    return url.toString(QUrl::FullyEncoded);
}

void StudioBridge::start() {
    if (m_running) return;
    if (m_inputStore) { m_inputStore->cancelRestore(); if (m_inputSaveTimer.isActive()) retryInputSave(); }
    stop();
    const bool remote = m_inputMode == ExistingRtmpStream;
    const auto input = remote ? remoteInputUrl() : obsServer() + '/' + streamKey();
    if (input.isEmpty()) {
        m_error = QStringLiteral("Enter an rtmp:// or rtmps:// server and stream key, or paste the complete stream URL.");
        m_status = QStringLiteral("Check the stream address");
        emit changed();
        return;
    }
    ++m_generation;
    const quint64 generation = m_generation;
    m_error.clear(); m_videoCodec.clear(); m_audioCodec.clear(); m_resolution.clear();
    m_frameCount = 0; m_receiving = false; m_running = true;
    m_status = remote ? QStringLiteral("Connecting to existing RTMP stream") : QStringLiteral("Waiting for OBS on this computer");
    m_worker = std::make_unique<StudioWorker>(input, !remote);
    m_worker->configurePreview(m_sink != nullptr);
    connect(m_worker.get(), &QThread::finished, this, [this, generation] {
        if (generation != m_generation) return;
        pollFrame();
        m_previewTimer.stop();
        m_running = false; m_receiving = false;
        m_status = m_error.isEmpty() ? QStringLiteral("Stream ended") : QStringLiteral("OBS input needs attention");
        emit changed();
    });
    m_worker->start();
    m_previewTimer.start();
    emit changed();
}

void StudioBridge::stop() {
    if (m_broadcastCapture) emit broadcastStopRequested();
    m_broadcastCapture = false;
    m_stopWhenBroadcastEnds = false;
    ++m_generation;
    m_previewTimer.stop();
    if (m_worker) {
        disconnect(m_worker.get(), nullptr, this, nullptr);
        m_worker->cancel.store(true);
        m_worker->wait();
        m_worker.reset();
    }
    m_running = false; m_receiving = false;
    m_status = QStringLiteral("OBS input is off");
    if (m_sink) m_sink->setVideoFrame(QVideoFrame());
    emit changed();
}

void StudioBridge::pollFrame() {
    if (!m_worker) return;
    auto snapshot = m_worker->take();
    if (!snapshot.diagnostic.isEmpty()) emit diagnostic(snapshot.diagnostic);
    const bool metadataChanged = (!snapshot.error.isEmpty() && snapshot.error != m_error)
        || m_videoCodec != snapshot.videoCodec || m_audioCodec != snapshot.audioCodec
        || m_resolution != snapshot.resolution;
    if (!snapshot.error.isEmpty()) m_error = snapshot.error;
    m_videoCodec = snapshot.videoCodec; m_audioCodec = snapshot.audioCodec;
    m_resolution = snapshot.resolution;
    const bool receivedMore = m_frameCount != snapshot.packets;
    m_frameCount = snapshot.packets;
    if (snapshot.packets > 0) {
        m_receiving = true;
        m_status = m_inputMode == ExistingRtmpStream ? QStringLiteral("Receiving RTMP") : QStringLiteral("Receiving OBS");
        if (m_sink) m_status += QStringLiteral(" · local preview");
    }
    if (snapshot.frame.isValid() && m_sink) m_sink->setVideoFrame(snapshot.frame);
    if (metadataChanged || receivedMore || snapshot.frame.isValid()) emit changed();
}

void StudioBridge::copyObsServer() { QGuiApplication::clipboard()->setText(obsServer()); }
void StudioBridge::copyStreamKey() { QGuiApplication::clipboard()->setText(streamKey()); }

void StudioBridge::setBroadcastContext(const QVariantMap &context) {
    if (m_broadcastContext == context) return;
    m_broadcastContext = context;
    emit broadcastChanged();
    if (m_stopWhenBroadcastEnds && !m_sink && !m_broadcastCapture && !broadcastContext().value("active").toBool()) stop();
}

void StudioBridge::setBroadcastCaptureEnabled(bool enabled, int height, int fps, bool audio) {
    m_broadcastCapture = enabled;
    if (m_worker) m_worker->configureBroadcast(enabled, height == 0 ? 0 : std::clamp(height, 360, 2160), fps == 0 ? 0 : std::clamp(fps, 15, 60), audio);
    if (m_stopWhenBroadcastEnds && !m_sink && !enabled && !broadcastContext().value("active").toBool()) stop();
}

QVariantMap StudioBridge::processingDiagnostics() const {
    return m_worker ? m_worker->processingDiagnostics() : QVariantMap{};
}

RelayEncodedVideo StudioBridge::takeEncodedBroadcastVideo() { return m_worker ? m_worker->takeEncoded() : RelayEncodedVideo{}; }

RelayBroadcastMedia StudioBridge::takeBroadcastMedia() {
    auto media = m_worker ? m_worker->takeBroadcast() : RelayBroadcastMedia{};
    media.ended = !m_worker || !m_running || m_worker->isFinished();
    return media;
}

void StudioBridge::requestBroadcast(int height, int fps, bool audio) {
    emit broadcastRequested(height, fps, audio);
}

void StudioBridge::stopBroadcast() { emit broadcastStopRequested(); }

void StudioBridge::setRelayOriginalVideoEnabled(bool enabled){if(m_worker)m_worker->configureParallelOriginal(enabled);}
