# Stream transport pacing

Relay preserves the selected video resolution, frame rate and encoded target.
The outgoing RTP sender spreads large keyframes over time instead of sending
32 datagrams at once. Its byte budget is the configured encoded bitrate plus
25% for RTP, encryption and repairs. Active 256 kb/s Opus audio adds a 320 kb/s
transport allowance; audio has immediate priority and charges the same budget.

A precise 1 ms timer sends at most eight video/repair datagrams per wakeup.
Stored credit is limited to 9.6 KB and each elapsed refill to 20 ms, so an event
loop stall cannot trigger an unlimited catch-up burst. Repeated NACKs share one
pending repair. After transmission, repeats wait for the measured report RTT
(20–500 ms, initially 100 ms). Recovery only considers originals actually sent
successfully, and stops retrying them after 1500 ms.

Repairs have a separate bounded queue, a two-packet burst allowance, and a byte
budget equal to the lesser of 15% of the configured encoded bitrate and 25% of
original wire bytes sent in the previous second. That history carries a large
keyframe's allowance through tiny following frames or a brief source pause.
At most two repairs precede an available original, so repeated feedback cannot
starve new video. Session and encryption-disable resets clear both queues,
packet history and budgets. Audio continues to have immediate priority.

This follows the elapsed-time and sent-byte accounting principle described in
the primary [WebRTC pacing controller implementation](https://webrtc.googlesource.com/src/+/refs/heads/main/modules/pacing/pacing_controller.cc).
Relay's constants and implementation are local policy. This is transport
scheduling, not an adaptive congestion controller; a sustained source above
the available path capacity can still require a lower source bitrate.

The retry policy follows the RTT suppression used by WebRTC's
[packet history](https://webrtc.googlesource.com/src/+/97feff2d9eed6515ad696d48b94856fcc9cdde1c/modules/rtp_rtcp/source/rtp_packet_history.cc).
[RFC 8834 §6.1](https://www.rfc-editor.org/rfc/rfc8834.html#section-6.1)
also describes how blind retransmission can amplify congestion. The specific
budgets and age limits above are Relay policy; they preserve the source's
resolution, frame rate and encoded target.

`broadcast_pacer_test` uses a deterministic 40 Mb/s link with an eight-datagram
queue to test a large keyframe, bounded catch-up, audio fairness and low-rate
progress. `broadcast_gateway_test` verifies queued NACK coalescing and later
repair, repeat suppression, expiry and receiver-report handling through a
generated local encrypted gateway; it never uses an account or private stream.
Diagnostics retain only aggregate feedback counts, loss fractions, RTT and
bitrate hints. Quality wants, REMB and TWCC are observed for diagnosis; they do
not currently change source encoding automatically.
