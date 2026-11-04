#include "StreamDecoder.h"
#include "Av1Bitstream.h"
#include <QVideoFrameFormat>
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace {
AVCodecID codecId(const QString &name) {
    if (name == "H264") return AV_CODEC_ID_H264;
    if (name == "VP8") return AV_CODEC_ID_VP8;
    if (name == "H265") return AV_CODEC_ID_HEVC;
    if (name == "AV1") return AV_CODEC_ID_AV1;
    return AV_CODEC_ID_NONE;
}
const AVCodec *decoderFor(const QString &name) {
    // FFmpeg codec registration is immutable. Cache discovery so per-packet
    // capability checks never traverse the decoder registry.
    static const AVCodec *av1 = []() -> const AVCodec * {
        if (auto *codec = avcodec_find_decoder_by_name("libdav1d")) return codec;
        if (auto *codec = avcodec_find_decoder_by_name("libaom-av1")) return codec;
        // The native AV1 wrapper may require a GPU context, which this worker
        // does not create. Do not advertise hardware-only decoder support.
        return nullptr;
    }();
    static const AVCodec *hevc = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    static const AVCodec *h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
    static const AVCodec *vp8 = avcodec_find_decoder(AV_CODEC_ID_VP8);
    if (name == "AV1") return av1;
    if (name == "H265") return hevc;
    if (name == "H264") return h264;
    if (name == "VP8") return vp8;
    return nullptr;
}
constexpr qsizetype maxFrameBytes = 8 * 1024 * 1024;
constexpr qsizetype maxQueuedBytes = 16 * 1024 * 1024;
constexpr size_t maxQueuedFrames = 12;
bool keyFrame(const QString &codec, const QByteArray &frame) {
    if (codec == "VP8")
        return frame.size() >= 10 && !(quint8(frame[0]) & 1)
            && frame.mid(3, 3) == QByteArray::fromHex("9d012a");
    if (codec == "AV1") {
        // A fresh parser needs the sequence header. Requesting an intra frame
        // repeats it in Discord's AV1 stream; reject intervening delta frames
        // quietly instead of parsing/logging each one without configuration.
        qsizetype offset = 0; Av1Bitstream::Obu obu; bool sequenceHeader = false;
        while (offset < frame.size()) {
            if (!Av1Bitstream::readObu(frame, offset, obu)) return false;
            sequenceHeader |= obu.type == 1;
        }
        if (!sequenceHeader) return false;
        auto *parser = av_parser_init(AV_CODEC_ID_AV1);
        auto *context = avcodec_alloc_context3(nullptr);
        if (!parser || !context) {
            if (parser) av_parser_close(parser);
            avcodec_free_context(&context); return false;
        }
        parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
        uint8_t *out = nullptr; int size = 0;
        // FFmpeg parsers require a zero-padded input buffer.
        auto padded = frame; padded.append(AV_INPUT_BUFFER_PADDING_SIZE, '\0');
        const auto consumed = av_parser_parse2(parser, context, &out, &size,
            reinterpret_cast<const uint8_t *>(padded.constData()), int(frame.size()),
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        const bool key = consumed >= 0 && parser->key_frame == 1;
        av_parser_close(parser); avcodec_free_context(&context); return key;
    }
    if (codec != "H264" && codec != "H265") return false;
    // The RTP depacketizer produces Annex B access units, including their SPS
    // and PPS. An IDR resets all prediction references in this receive path.
    for (qsizetype i = 0; i + 3 < frame.size(); ++i) {
        if (frame[i] || frame[i + 1]) continue;
        qsizetype header = -1;
        if (frame[i + 2] == 1) header = i + 3;
        else if (!frame[i + 2] && i + 4 < frame.size() && frame[i + 3] == 1) header = i + 4;
        if (header < 0) continue;
        if (codec == "H264" && (quint8(frame[header]) & 0x1f) == 5) return true;
        const auto type = (quint8(frame[header]) >> 1) & 63;
        if (codec == "H265" && header + 1 < frame.size() && type >= 16 && type <= 21) return true;
    }
    return false;
}
QVideoFrameFormat displayFormat(const AVFrame *frame) {
    QVideoFrameFormat format(QSize(frame->width, frame->height), QVideoFrameFormat::Format_YUV420P);
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709: format.setColorSpace(QVideoFrameFormat::ColorSpace_BT709); break;
    case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: format.setColorSpace(QVideoFrameFormat::ColorSpace_BT2020); break;
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_FCC:
        format.setColorSpace(QVideoFrameFormat::ColorSpace_BT601); break;
    default:
        format.setColorSpace(frame->height > 576 ? QVideoFrameFormat::ColorSpace_BT709 : QVideoFrameFormat::ColorSpace_BT601); break;
    }
    const bool full = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P;
    format.setColorRange(full ? QVideoFrameFormat::ColorRange_Full : QVideoFrameFormat::ColorRange_Video);
    switch (frame->color_trc) {
    case AVCOL_TRC_BT709: format.setColorTransfer(QVideoFrameFormat::ColorTransfer_BT709); break;
    case AVCOL_TRC_SMPTE170M: format.setColorTransfer(QVideoFrameFormat::ColorTransfer_BT601); break;
    case AVCOL_TRC_LINEAR: format.setColorTransfer(QVideoFrameFormat::ColorTransfer_Linear); break;
    case AVCOL_TRC_GAMMA22: format.setColorTransfer(QVideoFrameFormat::ColorTransfer_Gamma22); break;
    case AVCOL_TRC_GAMMA28: format.setColorTransfer(QVideoFrameFormat::ColorTransfer_Gamma28); break;
    case AVCOL_TRC_SMPTE2084: format.setColorTransfer(QVideoFrameFormat::ColorTransfer_ST2084); break;
    case AVCOL_TRC_ARIB_STD_B67: format.setColorTransfer(QVideoFrameFormat::ColorTransfer_STD_B67); break;
    default: break;
    }
    return format;
}
}
struct StreamDecoder::Private {
    struct Frame { QByteArray bytes; quint32 timestamp; bool reset = false; };
    QString codecName;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Frame> queue;
    qsizetype queuedBytes = 0;
    bool stop = false;
    bool resetOnKeyFrame = false;
    Snapshot latest;
    std::thread worker;
    explicit Private(QString codec) : codecName(std::move(codec)), worker([this] { run(); }) {}
    ~Private() {
        { std::lock_guard lock(mutex); stop = true; queue.clear(); queuedBytes = 0; }
        ready.notify_all();
        if (worker.joinable()) worker.join();
    }
    void fail(const QString &message) { std::lock_guard lock(mutex); latest.error = message; }
    void recoverLocked() {
        latest.dropped += queue.size(); queue.clear(); queuedBytes = 0;
        latest.keyFrameNeeded = true; resetOnKeyFrame = true;
    }
    void recover() {
        std::lock_guard lock(mutex); ++latest.dropped; recoverLocked();
    }
    void run() {
        const AVCodec *codec = decoderFor(codecName);
        AVCodecContext *decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!decoder) { fail("This stream video codec is unavailable."); return; }
        decoder->thread_count = 2;
        decoder->thread_type = FF_THREAD_SLICE;
        decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
        decoder->max_pixels = 8192LL * 4320;
        if (avcodec_open2(decoder, codec, nullptr) < 0) {
            avcodec_free_context(&decoder); fail("Could not open the stream video decoder."); return;
        }
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        SwsContext *scaler = nullptr;
        if (!packet || !frame) { fail("Could not allocate the stream video decoder."); }
        else for (;;) {
            Frame input;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [this] { return stop || !queue.empty(); });
                if (stop) break;
                input = std::move(queue.front()); queue.pop_front(); queuedBytes -= input.bytes.size();
            }
            if (input.reset) avcodec_flush_buffers(decoder);
            av_packet_unref(packet);
            if (av_new_packet(packet, int(input.bytes.size())) < 0) { fail("Could not allocate a stream frame."); break; }
            std::memcpy(packet->data, input.bytes.constData(), size_t(input.bytes.size()));
            packet->pts = input.timestamp;
            if (avcodec_send_packet(decoder, packet) < 0) { recover(); continue; }
            while (avcodec_receive_frame(decoder, frame) >= 0) {
                if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags) {
                    av_frame_unref(frame); recover(); break;
                }
                if (frame->width < 2 || frame->height < 2 || frame->width > 8192 || frame->height > 8192
                    || qint64(frame->width) * frame->height > 8192LL * 4320) {
                    fail("Stream resolution exceeds the supported decode limit."); av_frame_unref(frame); continue;
                }
                // Keep video in planar YUV instead of copying full-sized RGBA for
                // every frame. QVideoSink performs presentation and local cropping.
                const auto format = displayFormat(frame);
                QVideoFrame video(format);
                if (!video.map(QVideoFrame::WriteOnly)) { fail("Could not allocate a stream display frame."); av_frame_unref(frame); continue; }
                uint8_t *planes[4] = {video.bits(0), video.bits(1), video.bits(2), nullptr};
                int strides[4] = {video.bytesPerLine(0), video.bytesPerLine(1), video.bytesPerLine(2), 0};
                if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
                    // Preserve the decoded samples and range exactly; these
                    // common formats need no scaling or second conversion.
                    for (int plane = 0; plane < 3; ++plane) {
                        const int rows = plane ? (frame->height + 1) / 2 : frame->height;
                        const int columns = plane ? (frame->width + 1) / 2 : frame->width;
                        for (int row = 0; row < rows; ++row)
                            std::memcpy(planes[plane] + row * strides[plane], frame->data[plane] + row * frame->linesize[plane], size_t(columns));
                    }
                } else {
                    scaler = sws_getCachedContext(scaler, frame->width, frame->height, AVPixelFormat(frame->format),
                        frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
                    if (!scaler) { video.unmap(); fail("Could not convert the stream video format."); av_frame_unref(frame); continue; }
                    const int color = format.colorSpace() == QVideoFrameFormat::ColorSpace_BT709 ? SWS_CS_ITU709
                        : format.colorSpace() == QVideoFrameFormat::ColorSpace_BT2020 ? SWS_CS_BT2020 : SWS_CS_ITU601;
                    const bool full = format.colorRange() == QVideoFrameFormat::ColorRange_Full;
                    sws_setColorspaceDetails(scaler, sws_getCoefficients(color), full, sws_getCoefficients(color), full, 0, 1 << 16, 1 << 16);
                    sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
                }
                video.unmap();
                {
                    std::lock_guard lock(mutex);
                    latest.frame = std::move(video); latest.width = frame->width; latest.height = frame->height;
                    ++latest.decoded;
                }
                av_frame_unref(frame);
            }
        }
        sws_freeContext(scaler); av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&decoder);
    }
};
StreamDecoder::StreamDecoder(const QString &codec) : d(std::make_unique<Private>(codec)) {}
StreamDecoder::~StreamDecoder() = default;
bool StreamDecoder::supports(const QString &codec) { return codecId(codec) != AV_CODEC_ID_NONE && decoderFor(codec); }
bool StreamDecoder::submit(const QByteArray &frame, quint32 timestamp) {
    if (frame.isEmpty() || frame.size() > maxFrameBytes) return false;
    // Restore low-overhead OBU lengths only after the DAVE payload was
    // authenticated and stripped. Altering encrypted framing breaks its tag.
    const auto input = d->codecName == "AV1" ? Av1Bitstream::withSizes(frame) : frame;
    if (input.isEmpty() || input.size() > maxFrameBytes) return false;
    {
        std::lock_guard lock(d->mutex);
        if (d->stop || !d->latest.error.isEmpty()) return false;
        if (d->queue.size() >= maxQueuedFrames || d->queuedBytes + input.size() > maxQueuedBytes)
            d->recoverLocked();
        if (d->latest.keyFrameNeeded && !keyFrame(d->codecName, input)) { ++d->latest.dropped; return false; }
        const bool reset = d->resetOnKeyFrame;
        d->latest.keyFrameNeeded = false; d->resetOnKeyFrame = false;
        d->queue.push_back({input, timestamp, reset}); d->queuedBytes += input.size();
    }
    d->ready.notify_one(); return true;
}
void StreamDecoder::discardUntilKeyFrame() {
    std::lock_guard lock(d->mutex); d->recoverLocked();
}
StreamDecoder::Snapshot StreamDecoder::take() {
    std::lock_guard lock(d->mutex);
    auto snapshot = d->latest; d->latest.frame = {}; return snapshot;
}
