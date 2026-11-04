#pragma once
#include "BroadcastPacer.h"
#include <deque>

// RTX is useful recovery traffic, but cannot consume the complete media path.
// Measure original wire bytes, so a quiet high-quality source does not gain an
// oversized repair allowance simply from its configured maximum bitrate.
class BroadcastRepairBudget final {
public:
    void reset() { m_samples.clear(); m_bytes = 0; m_pacer.reset(); }
    void originalSent(std::int64_t now, int bytes) {
        expire(now);
        if (bytes <= 0) return;
        m_samples.push_back({now, bytes}); m_bytes += bytes;
    }
    void update(std::int64_t now, int configuredBitrate) {
        expire(now);
        // One second of history carries an IDR's allowance into its tiny P
        // frames and brief source pauses. Two packets remain available after
        // idle; there is no unlimited refill when no originals were sent.
        const auto rate = std::min<std::int64_t>(std::max(0, configuredBitrate) * 15LL / 100, m_bytes * 2);
        m_pacer.updateRate(now, rate);
    }
    bool ready(int bytes) const { return m_pacer.ready(bytes); }
    void sent(int bytes) { m_pacer.sent(bytes); }
private:
    void expire(std::int64_t now) {
        while (!m_samples.empty() && (now - m_samples.front().at >= 1000000 || m_samples.size() > 16384)) {
            m_bytes -= m_samples.front().bytes; m_samples.pop_front();
        }
    }
    struct Sample { std::int64_t at; int bytes; };
    std::deque<Sample> m_samples;
    std::int64_t m_bytes = 0;
    BroadcastPacer m_pacer{2400, 2400};
};
