#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
// Independent per-speaker limiter: immediate attack, gradual release, no boost.
class VoiceLimiter {
    double gain = 1.0;
public:
    double frameGain(const char *pcm, int samples, double userGain, bool enabled, double ceiling) {
        if (!enabled || userGain <= 0) { gain = 1.0; return userGain; }
        double peak = 0;
        for (int i = 0; i < samples; ++i) {
            std::int16_t sample; std::memcpy(&sample, pcm + i * 2, 2);
            peak = std::max(peak, std::abs(double(sample)) * userGain);
        }
        const double target = peak > 0 ? std::min(1.0, ceiling * 32767.0 / peak) : 1.0;
        gain = target < gain ? target : std::min(target, gain + (1.0 - gain) * 0.05);
        return userGain * gain;
    }
};
