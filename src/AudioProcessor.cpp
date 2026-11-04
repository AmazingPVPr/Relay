#include "AudioProcessor.h"
#include <rnnoise.h>
#ifdef RELAY_USE_SPEEX
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <deque>
#else
#include <modules/audio_processing/include/audio_processing.h>
#endif
#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr int frames = 960, channels = 2, chunk = 480;
std::int16_t sample(double value) { return std::int16_t(std::clamp(value, -32768.0, 32767.0)); }
}
struct AudioProcessor::Private {
    Options options;
    std::array<DenoiseState *, 2> denoisers{};
#ifdef RELAY_USE_SPEEX
    SpeexEchoState *echo=nullptr;
    std::deque<std::array<std::int16_t,chunk*channels>> echoReferences;
    bool hasPlaybackReference=false;
    std::array<SpeexPreprocessState*,2> gain{};
#else
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
#endif
    int hangover = 0;
    double peak = 0;
    bool healthy = true;
    ~Private() { clear(); }
    void clear() {
        for (auto &state : denoisers) { if (state) rnnoise_destroy(state); state = nullptr; }
#ifdef RELAY_USE_SPEEX
        if(echo)speex_echo_state_destroy(echo);
        echo=nullptr;echoReferences.clear();hasPlaybackReference=false;
        for(auto &state:gain){if(state)speex_preprocess_state_destroy(state);state=nullptr;}
#else
        apm = nullptr;
#endif
        hangover = 0; peak = 0;
    }
    void initialize() {
        clear(); healthy = true;
        if (options.noiseSuppression) {
            if (rnnoise_get_frame_size() != chunk) { healthy = false; return; }
            for (auto &state : denoisers) { state = rnnoise_create(nullptr); if (!state) healthy = false; }
        }
#ifdef RELAY_USE_SPEEX
        if(options.echoCancellation){
            echo=speex_echo_state_init_mc(chunk,24000,channels,channels);
            int rate=48000;
            if(!echo || speex_echo_ctl(echo,SPEEX_ECHO_SET_SAMPLING_RATE,&rate)!=0)healthy=false;
        }
        if(options.automaticGainControl)for(auto &state:gain){
            state=speex_preprocess_state_init(chunk,48000);
            if(!state){healthy=false;continue;}
            int enabled=1,disabled=0;float level=10000;
            healthy=(speex_preprocess_ctl(state,SPEEX_PREPROCESS_SET_DENOISE,&disabled)==0) && healthy;
            healthy=(speex_preprocess_ctl(state,SPEEX_PREPROCESS_SET_AGC,&enabled)==0) && healthy;
            healthy=(speex_preprocess_ctl(state,SPEEX_PREPROCESS_SET_AGC_LEVEL,&level)==0) && healthy;
        }
#else
        if (options.echoCancellation || options.automaticGainControl) {
            apm = webrtc::AudioProcessingBuilder().Create();
            if (!apm) { healthy = false; return; }
            webrtc::AudioProcessing::Config config;
            config.echo_canceller.enabled = options.echoCancellation;
            config.high_pass_filter.enabled = options.echoCancellation;
            config.gain_controller2.enabled = options.automaticGainControl;
            config.gain_controller2.adaptive_digital.enabled = options.automaticGainControl;
            apm->ApplyConfig(config);
            healthy = apm->Initialize() == 0 && healthy;
        }
#endif
    }
};
AudioProcessor::AudioProcessor() : d(std::make_unique<Private>()) {}
AudioProcessor::~AudioProcessor() = default;
void AudioProcessor::configure(const Options &value) {
    auto options = value;
    options.inputVolume = std::isfinite(value.inputVolume) ? std::clamp(value.inputVolume, 0.0, 2.0) : 1.0;
    options.outputVolume = std::isfinite(value.outputVolume) ? std::clamp(value.outputVolume, 0.0, 2.0) : 1.0;
    options.sensitivityDb = std::isfinite(value.sensitivityDb) ? std::clamp(value.sensitivityDb, -70.0, -10.0) : -48.0;
    const bool rebuild = options.noiseSuppression != d->options.noiseSuppression ||
        options.echoCancellation != d->options.echoCancellation || options.automaticGainControl != d->options.automaticGainControl;
    d->options = options;
    if (rebuild) d->initialize();
}
void AudioProcessor::reset() { d->initialize(); }
bool AudioProcessor::capture(std::int16_t *stereo, int delayMs) {
    if (!stereo) return false;
#ifndef RELAY_USE_SPEEX
    const webrtc::StreamConfig format(48000, channels);
#else
    (void)delayMs; // The 500 ms adaptive filter covers the output/capture delay.
#endif
    for (int offset = 0; offset < frames; offset += chunk) {
        auto *pcm = stereo + offset * channels;
#ifdef RELAY_USE_SPEEX
        if(d->echo && d->hasPlaybackReference){
            // Feed each rendered reference once. The asynchronous Speex API
            // assumes a device-callback cadence that batched Qt capture does not
            // guarantee. A microphone monitor never supplies this reference.
            std::array<std::int16_t,chunk*channels> reference{},output;
            if(!d->echoReferences.empty()){
                reference=d->echoReferences.front();d->echoReferences.pop_front();
            }
            speex_echo_cancellation(d->echo,pcm,reference.data(),output.data());
            std::copy(output.begin(),output.end(),pcm);
        }
        for(int channel=0;channel<channels;++channel)if(d->gain[channel]){
            std::array<std::int16_t,chunk> mono;
            for(int i=0;i<chunk;++i)mono[i]=pcm[i*channels+channel];
            speex_preprocess_run(d->gain[channel],mono.data());
            for(int i=0;i<chunk;++i)pcm[i*channels+channel]=mono[i];
        }
#else
        if (d->apm) {
            d->apm->set_stream_delay_ms(std::clamp(delayMs, 0, 500));
            if (d->apm->ProcessStream(pcm, format, format, pcm) != 0) d->healthy = false;
        }
#endif
        if (d->options.noiseSuppression) {
            for (int channel = 0; channel < channels; ++channel) {
                if (!d->denoisers[channel]) continue;
                std::array<float, chunk> mono;
                for (int i = 0; i < chunk; ++i) mono[i] = pcm[i * channels + channel];
                rnnoise_process_frame(d->denoisers[channel], mono.data(), mono.data());
                for (int i = 0; i < chunk; ++i) pcm[i * channels + channel] = sample(mono[i]);
            }
        }
    }
    double energy = 0;
    d->peak = 0;
    for (int i = 0; i < frames * channels; ++i) {
        stereo[i] = sample(stereo[i] * d->options.inputVolume);
        const double normalized = double(stereo[i]) / 32768.0;
        energy += normalized * normalized;
        d->peak = std::max(d->peak, std::abs(normalized));
    }
    const bool audible = energy / (frames * channels) >= std::pow(10.0, d->options.sensitivityDb / 10.0);
    if (audible) d->hangover = 10; // Keep word endings through 200 ms of quiet.
    else if (d->hangover > 0) --d->hangover;
    const bool active = audible || d->hangover > 0;
    if (!active) std::fill_n(stereo, frames * channels, 0);
    return active;
}
void AudioProcessor::monitor(std::int16_t *stereo) {
    if (!stereo) return;
    for (int i = 0; i < frames * channels; ++i) stereo[i] = sample(stereo[i] * d->options.outputVolume);
}
void AudioProcessor::playback(std::int16_t *stereo) {
    if (!stereo) return;
    monitor(stereo);
#ifdef RELAY_USE_SPEEX
    if(d->echo)for(int offset=0;offset<frames;offset+=chunk){
        std::array<std::int16_t,chunk*channels> reference;
        std::copy_n(stereo+offset*channels,reference.size(),reference.data());
        // Bound storage even while microphone capture is paused.
        if(d->echoReferences.size()>=50)d->echoReferences.pop_front();
        d->echoReferences.push_back(reference);d->hasPlaybackReference=true;
    }
#else
    if (d->apm && d->options.echoCancellation) {
        const webrtc::StreamConfig format(48000, channels);
        // Keep the reference separate: processing it must not alter speaker samples.
        std::array<std::int16_t, chunk * channels> reference;
        for (int offset = 0; offset < frames; offset += chunk) {
            std::copy_n(stereo + offset * channels, reference.size(), reference.data());
            if (d->apm->ProcessReverseStream(reference.data(), format, format, reference.data()) != 0) d->healthy = false;
        }
    }
#endif
}
double AudioProcessor::peak() const { return d->peak; }
bool AudioProcessor::healthy() const { return d->healthy; }
