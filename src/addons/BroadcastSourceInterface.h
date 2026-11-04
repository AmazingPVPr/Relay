#pragma once
#include <QByteArray>
#include <QVariantMap>
#include <QVideoFrame>
#include <QtPlugin>

struct RelayBroadcastMedia {
    QVideoFrame video;
    QByteArray audio; // 48 kHz stereo signed-16 PCM, at most 200 ms.
    QString error, audioError;
    bool ended = false;
};

// Optional native capability. The host polls bounded media mailboxes, owns the
// Discord transport, and supplies display state without credentials or IDs.
class RelayBroadcastSourceInterface {
public:
    virtual ~RelayBroadcastSourceInterface() = default;
    virtual void setBroadcastContext(const QVariantMap &context) = 0;
    virtual void setBroadcastCaptureEnabled(bool enabled, int maximumHeight, int fps, bool audio) = 0;
    virtual RelayBroadcastMedia takeBroadcastMedia() = 0;
};
#define RelayBroadcastSourceInterface_iid "org.relayclient.BroadcastSource/1.0"
Q_DECLARE_INTERFACE(RelayBroadcastSourceInterface, RelayBroadcastSourceInterface_iid)
