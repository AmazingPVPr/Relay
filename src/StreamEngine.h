#pragma once
#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>
#include <memory>
class QUrl;
class QIODevice;

class StreamEngine final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool waitingForRestart READ waitingForRestart NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool receiving READ receiving NOTIFY changed)
    Q_PROPERTY(bool encryptedReady READ encryptedReady NOTIFY changed)
    Q_PROPERTY(QString streamKey READ streamKey NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString codec READ codec NOTIFY changed)
    Q_PROPERTY(QString diagnosticSummary READ diagnosticSummary NOTIFY diagnosticsChanged)
    Q_PROPERTY(QVariantMap playbackDiagnostics READ playbackDiagnostics NOTIFY diagnosticsChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY changed)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY changed)
    Q_PROPERTY(quint64 frameCount READ frameCount NOTIFY changed)
    Q_PROPERTY(QObject* videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
    Q_PROPERTY(bool audioMuted READ audioMuted WRITE setAudioMuted NOTIFY changed)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY changed)
    Q_PROPERTY(QString outputDeviceId READ outputDeviceId WRITE setOutputDeviceId NOTIFY changed)
public:
    explicit StreamEngine(QObject *parent = nullptr);
    ~StreamEngine() override;
    bool available() const; bool active() const; bool waitingForRestart() const; bool connected() const; bool receiving() const; bool encryptedReady() const;
    QString streamKey() const; QString title() const; QString status() const; QString error() const; QString codec() const;
    QString diagnosticSummary() const; QVariantMap playbackDiagnostics() const;
    QVariantMap publishingDiagnostics() const; int videoWidth() const; int videoHeight() const; quint64 frameCount() const;
    QObject *videoSink() const; void setVideoSink(QObject *sink);
    bool audioMuted() const; void setAudioMuted(bool muted);
    void setDeafened(bool deafened);
    double volume() const; void setVolume(double volume);
    QString outputDeviceId() const; void setOutputDeviceId(const QString &id);
    void setIdentity(const QString &userId, const QString &voiceSessionId);
    Q_INVOKABLE void watch(const QString &streamKey, const QString &title);
    Q_INVOKABLE void stop();
public slots:
    void onStreamRestart(const QString &streamKey);
    void onStreamCreate(const QJsonObject &event);
    void onStreamServer(const QJsonObject &event);
    void onStreamDelete(const QJsonObject &event);
signals:
    void changed(); void diagnosticsChanged(); void videoSinkChanged();
    void watchRequested(QString streamKey);
    void stopRequested(QString streamKey);
    void keyFrameRequested();
    void publishingBitrateChanged(int bitrate);
private:
    friend class StreamGatewayTest;
    friend class BroadcastEngine;
    friend class BroadcastTransport;
    friend class BroadcastGatewayTest;
    void publish(const QString &streamKey,int width,int height,int fps,int bitrate);
    bool publishVideo(const QByteArray &frame,quint32 timestamp,int width,int height);
    bool publishAudio(const QByteArray &opus,quint32 timestamp);
    bool configureLocalFixture(const QUrl &url);
    QByteArray takeFixtureAudio();
    void setFixturePollInterval(int milliseconds);
    void setFixtureAudioOutput(QIODevice *output);
    struct Private;
    std::unique_ptr<Private> d;
};
