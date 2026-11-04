#pragma once
#include <QByteArray>
#include <QString>
#include <QVideoFrame>
#include <memory>

// Compressed frames enter a bounded worker queue; only the newest decoded frame
// is retained for the UI. No network or account access belongs in this worker.
class StreamDecoder final {
public:
    struct Snapshot {
        QVideoFrame frame;
        QString error;
        quint64 decoded = 0, dropped = 0;
        int width = 0, height = 0;
        bool keyFrameNeeded = false;
    };
    explicit StreamDecoder(const QString &codec);
    ~StreamDecoder();
    StreamDecoder(const StreamDecoder &) = delete;
    StreamDecoder &operator=(const StreamDecoder &) = delete;
    bool submit(const QByteArray &frame, quint32 timestamp);
    // Missing reference frames cannot be repaired by decoding later interframes.
    // Keep the last displayed picture and resume only from a fresh keyframe.
    void discardUntilKeyFrame();
    Snapshot take();
    static bool supports(const QString &codec);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
