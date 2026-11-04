#pragma once
#include <QByteArray>
#include <QString>
#include <memory>

// Records only output audio: WASAPI loopback on Windows, an output monitor
// through PipeWire-Pulse/PulseAudio on Linux. Never substitutes a microphone.
class SystemAudioCapture {
public:
    struct Snapshot { QByteArray pcm; QString error; bool active=false; };
    SystemAudioCapture();
    ~SystemAudioCapture();
    bool start();
    void stop();
    Snapshot take();
private:
    struct Private;std::unique_ptr<Private> d;
};
