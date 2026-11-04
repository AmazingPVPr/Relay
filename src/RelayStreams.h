#pragma once
#include <QObject>
#include <QVariantList>
#include <memory>
class RelayPeers;
class BroadcastEngine;

// Streaming is ephemeral, separate from saved Relay text and its MCP tools.
class RelayStreams final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool publishEnabled READ publishEnabled WRITE setPublishEnabled NOTIFY changed)
    Q_PROPERTY(int bitrateMbps READ bitrateMbps WRITE setBitrateMbps NOTIFY changed)
    Q_PROPERTY(int viewerCount READ viewerCount NOTIFY changed)
    Q_PROPERTY(QVariantList streams READ streams NOTIFY changed)
    Q_PROPERTY(QString watchingPeer READ watchingPeer NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QObject* videoSink READ videoSink WRITE setVideoSink NOTIFY changed)
    Q_PROPERTY(bool audioMuted READ audioMuted WRITE setAudioMuted NOTIFY changed)
public:
    RelayStreams(RelayPeers *peers, BroadcastEngine *broadcast, QObject *parent = nullptr);
    ~RelayStreams() override;
    bool publishEnabled() const;
    void setPublishEnabled(bool value);
    int bitrateMbps() const;
    void setBitrateMbps(int value);
    int viewerCount() const;
    QVariantList streams() const;
    QString watchingPeer() const;
    QString status() const;
    QObject *videoSink() const;
    void setVideoSink(QObject *sink);
    bool audioMuted() const;
    void setAudioMuted(bool muted);
    void setDeafened(bool deafened);
    void setOutputDeviceId(const QString &id);
    Q_INVOKABLE void watch(const QString &peer);
    Q_INVOKABLE void stopWatching();
signals:
    void changed();
private:
    friend class RelayPeersTest;
    struct Private;
    std::unique_ptr<Private> d;
};
