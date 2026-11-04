#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QVideoFrame>
#include <memory>

class BroadcastEncoder {
public:
    struct Frame { QByteArray bytes; quint32 timestamp=0; int width=0,height=0; bool keyframe=false; };
    struct Snapshot { QList<Frame> frames; QString error; quint64 encoded=0,dropped=0; };
    BroadcastEncoder(int maximumHeight,int fps,int bitrate);
    ~BroadcastEncoder();
    static bool available();
    void submit(const QVideoFrame &frame);
    void requestKeyframe();
    void setBitrate(int bitrate);
    Snapshot take();
private:
    struct Private;std::unique_ptr<Private> d;
};
