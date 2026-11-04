#pragma once
#include <cstdint>
#include <memory>

// Processes fixed 20 ms, 48 kHz stereo frames on the audio owner's thread.
class AudioProcessor final {
public:
    struct Options {
        bool noiseSuppression = false, echoCancellation = false, automaticGainControl = false;
        double inputVolume = 1.0, outputVolume = 1.0, sensitivityDb = -48.0;
        bool voiceLimiterEnabled = false;
        double voiceLimiterCeiling = 0.5;
        bool operator==(const Options &) const = default;
    };
    AudioProcessor();
    ~AudioProcessor();
    void configure(const Options &options);
    void reset();
    // Capture returns whether the frame passes the sensitivity gate (with hangover).
    bool capture(std::int16_t *stereo, int delayMs = 60);
    // Local microphone monitoring applies speaker gain without telling the echo
    // canceller that the user's own voice is a remote playback reference.
    void monitor(std::int16_t *stereo);
    void playback(std::int16_t *stereo);
    double peak() const;
    bool healthy() const;
private:
    struct Private;
    std::unique_ptr<Private> d;
};
