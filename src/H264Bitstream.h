#pragma once
#include <QByteArray>

namespace H264Bitstream {
// Prepare the clear SPS before DAVE authenticates it. WebRTC receivers rewrite
// absent/excessive buffering restrictions before their frame decryptor runs.
// Keeping those restrictions canonical here prevents that rewrite invalidating
// the authenticated frame. Coded picture, timing and color bytes are retained.
// Returns empty for a malformed/oversized Annex B frame or SPS.
QByteArray prepareForDave(const QByteArray &annexB);
}
