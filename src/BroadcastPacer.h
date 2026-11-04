#pragma once
#include <algorithm>
#include <cstdint>

// Byte credit, rather than a packet count alone, bounds both long-term rate
// and the burst after a stalled event loop. Times are monotonic microseconds.
class BroadcastPacer final {
public:
    static constexpr int packetsPerWakeup = 8;
    static constexpr int maximumCredit = packetsPerWakeup * 1200;
    explicit BroadcastPacer(int capacity = maximumCredit, int initial = 0)
        : m_capacity(std::clamp(capacity, 1, maximumCredit)), m_initial(std::clamp(initial, 0, m_capacity)), m_credit(m_initial) {}
    void reset() { m_last = -1; m_credit = m_initial; }
    void update(std::int64_t now, int videoBitrate, bool audioActive) {
        updateRate(now, std::int64_t(std::max(1, videoBitrate) * 1.25) + (audioActive ? 320000 : 0));
    }
    void updateRate(std::int64_t now, std::int64_t bitsPerSecond) {
        if (m_last < 0) { m_last = now; return; }
        const auto elapsed = std::clamp<std::int64_t>(now - m_last, 0, 20000);
        m_last = now;
        // 25% covers RTP/AEAD overhead and repairs. The 256 kb/s audio
        // encoder gets its own transport allowance, charged to this budget.
        const double bytesPerSecond = std::max<std::int64_t>(0, bitsPerSecond) / 8.;
        m_credit = std::min(double(m_capacity), m_credit + elapsed * bytesPerSecond / 1000000);
    }
    bool ready(int bytes) const { return bytes > 0 && bytes <= m_capacity && m_credit >= bytes; }
    void sent(int bytes) {
        // Priority audio may borrow credit; bound debt across source stalls.
        m_credit = std::max(-double(m_capacity), m_credit - std::max(0, bytes));
    }
private:
    std::int64_t m_last = -1;
    int m_capacity, m_initial;
    double m_credit = 0;
};
