# Private Linux audio backend regression

This manual test checks the actual Qt 6.12 audio backend. The regular unit tests validate cue samples, gain, settings, and lifecycle through an injected output; they cannot detect OS notification-role muting.

Requirements: Linux, Qt 6.12 development files, CMake, Ninja, a C++ compiler, Python 3.9+, PipeWire with its Pulse protocol module, and `pactl`, `parec`, and `pw-link`.

From the Relay source directory:

```sh
python3 tests/audio_backend/run.py --work-dir ../../work/audio-backend-regression
```

Omit `--work-dir` to use a fresh directory under `/tmp`. The standalone CMake project builds only the probe and production `SoundEffects`/`AppSettings` sources. It does not change the main build. Generated binaries and logs are rejected inside `outputs/`.

The runner creates a private PipeWire daemon, Pulse socket, virtual sink, settings directories, and explicit graph links. Its configuration loads no hardware discovery, ALSA, Bluetooth, session manager, or process launcher. All commands target its private server. It never loads a sink into the host server, changes host notification volume, opens a microphone, or records live/user audio. The only monitored samples are Relay's bundled synthesized cues. The probe refuses playback unless exactly its uniquely named virtual sink is available. There is no physical-device fallback.

The three required results are:

1. `QSoundEffect`, event role unmuted: nonzero generated PCM.
2. `QSoundEffect`, event role muted: exactly zero PCM, despite successful sample loading/playback.
3. Production `SoundEffects`, with the same event-only mute policy: ordinary output role, nonzero PCM, four completed cues, zero errors, and positive consumed-byte counters.

The private event mute simulates a muted notification role; it does not read or modify the user's notification settings. Private stream gain is normalized to 100%, while the probe sets Relay's own cue volume to 50%. Each case must capture at least 48,000 real backend samples. Float samples are analyzed in memory and are never saved as audio files.

`results.json` contains roles, sample counts, peak/RMS levels, and production counters. Per-case logs and `server.log` aid diagnosis. All child processes and the private runtime directory are removed in a `finally` block, including on failure or Ctrl+C. The selected work directory retains build files and logs for review. Missing tools, an unavailable private endpoint, an unexpected device/role, silent production playback, or failed completion causes a failing exit status.
