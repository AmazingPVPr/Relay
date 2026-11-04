#pragma once
#include <QByteArray>
#include <QList>
#include <QtPlugin>

struct RelayEncodedVideoFrame {
    QByteArray h264; // A complete Annex B access unit, including headers on IDRs.
    quint32 timestamp = 0; // Original presentation time in 90 kHz RTP units.
    bool keyframe = false;
};

struct RelayEncodedVideo {
    bool available = false;
    int width = 0, height = 0, fps = 0;
    QList<RelayEncodedVideoFrame> frames;
};

// Optional companion to BroadcastSource/1.0. Source mode uses height=0/fps=0
// in setBroadcastCaptureEnabled. Older providers retain the decoded-frame path.
class RelayEncodedBroadcastSourceInterface {
public:
    virtual ~RelayEncodedBroadcastSourceInterface() = default;
    virtual RelayEncodedVideo takeEncodedBroadcastVideo() = 0;
};
#define RelayEncodedBroadcastSourceInterface_iid "org.relayclient.EncodedBroadcastSource/1.0"
Q_DECLARE_INTERFACE(RelayEncodedBroadcastSourceInterface, RelayEncodedBroadcastSourceInterface_iid)
