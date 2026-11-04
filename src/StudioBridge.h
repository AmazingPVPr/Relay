#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVideoSink>
#include <memory>
#include "addons/BroadcastSourceInterface.h"
#include "addons/EncodedBroadcastSourceInterface.h"

class StudioWorker;
class ObsInputStore;

// OBS preview from a local publisher or an existing RTMP server.
// It deliberately has no Discord credentials or Discord transport.
// A latest-frame mailbox bounds memory even if the UI is hidden or rendering slowly.
class StudioBridge final : public QObject, public RelayBroadcastSourceInterface, public RelayEncodedBroadcastSourceInterface {
    Q_OBJECT
    Q_INTERFACES(RelayBroadcastSourceInterface)
    Q_INTERFACES(RelayEncodedBroadcastSourceInterface)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool receiving READ receiving NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString videoCodec READ videoCodec NOTIFY changed)
    Q_PROPERTY(QString audioCodec READ audioCodec NOTIFY changed)
    Q_PROPERTY(QString resolution READ resolution NOTIFY changed)
    Q_PROPERTY(qint64 frameCount READ frameCount NOTIFY changed)
    Q_PROPERTY(QVariantMap processingDiagnostics READ processingDiagnostics NOTIFY changed)
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY changed)
    Q_PROPERTY(QString obsServer READ obsServer CONSTANT)
    Q_PROPERTY(QString streamKey READ streamKey CONSTANT)
    Q_PROPERTY(bool broadcastAvailable READ broadcastAvailable NOTIFY broadcastChanged)
    Q_PROPERTY(QVariantMap broadcastContext READ broadcastContext NOTIFY broadcastChanged)
    Q_PROPERTY(InputMode inputMode READ inputMode WRITE setInputMode NOTIFY configurationChanged)
    Q_PROPERTY(QString remoteServer READ remoteServer WRITE setRemoteServer NOTIFY configurationChanged)
    Q_PROPERTY(QString remoteStreamKey READ remoteStreamKey WRITE setRemoteStreamKey NOTIFY configurationChanged)
    Q_PROPERTY(bool rememberInput READ rememberInput WRITE setRememberInput NOTIFY inputStorageChanged)
    Q_PROPERTY(bool inputSaved READ inputSaved NOTIFY inputStorageChanged)
    Q_PROPERTY(bool inputStorageBusy READ inputStorageBusy NOTIFY inputStorageChanged)
    Q_PROPERTY(QString inputStorageStatus READ inputStorageStatus NOTIFY inputStorageChanged)
    Q_PROPERTY(QString inputStorageError READ inputStorageError NOTIFY inputStorageChanged)
    Q_PROPERTY(QObject* videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
public:
    enum InputMode { DirectFromObs, ExistingRtmpStream };
    Q_ENUM(InputMode)
    explicit StudioBridge(QObject* parent = nullptr);
    ~StudioBridge() override;
    bool running() const { return m_running; }
    bool receiving() const { return m_receiving; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    QString videoCodec() const { return m_videoCodec; }
    QString audioCodec() const { return m_audioCodec; }
    QString resolution() const { return m_resolution; }
    qint64 frameCount() const { return m_frameCount; }
    QVariantMap processingDiagnostics() const;
    bool hasAudio() const { return !m_audioCodec.isEmpty(); }
    QString obsServer() const { return QStringLiteral("rtmp://127.0.0.1:45990/live"); }
    QString streamKey() const { return QStringLiteral("relay"); }
    bool broadcastAvailable() const { return m_broadcastContext.value("available").toBool(); }
    QVariantMap broadcastContext() const { return m_broadcastContext; }
    void setBroadcastContext(const QVariantMap &context) override;
    void setBroadcastCaptureEnabled(bool enabled, int maximumHeight, int fps, bool audio) override;
    RelayBroadcastMedia takeBroadcastMedia() override;
    RelayEncodedVideo takeEncodedBroadcastVideo() override;
    Q_INVOKABLE void setRelayOriginalVideoEnabled(bool enabled);
    InputMode inputMode() const { return m_inputMode; }
    void setInputMode(InputMode mode);
    QString remoteServer() const { return m_remoteServer; }
    void setRemoteServer(const QString &server);
    QString remoteStreamKey() const { return m_remoteStreamKey; }
    void setRemoteStreamKey(const QString &key);
    // Opt-in by the real OBS add-on host only. Default instances perform no
    // settings or wallet access (including all media fixtures).
    void enableInputPersistence();
    bool rememberInput() const;
    void setRememberInput(bool remember);
    bool inputSaved() const;
    bool inputStorageBusy() const;
    QString inputStorageStatus() const;
    QString inputStorageError() const;
    Q_INVOKABLE void forgetSavedInput();
    Q_INVOKABLE void retryInputSave();
    QObject* videoSink() const { return m_sink; }
    void setVideoSink(QObject* sink);
    Q_INVOKABLE void releasePreview(QObject* sink);
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void copyObsServer();
    Q_INVOKABLE void copyStreamKey();
    Q_INVOKABLE void requestBroadcast(int maximumHeight = 0, int fps = 0, bool audio = true);
    Q_INVOKABLE void stopBroadcast();
signals:
    void changed();
    void diagnostic(const QString &message);
    void videoSinkChanged();
    void configurationChanged();
    void broadcastChanged();
    void broadcastRequested(int maximumHeight, int fps, bool audio);
    void broadcastStopRequested();
    void inputStorageChanged();
private:
    void inputEdited();
    QString remoteInputUrl() const;
    void pollFrame();
    std::unique_ptr<StudioWorker> m_worker;
    QPointer<QVideoSink> m_sink;
    QMetaObject::Connection m_sinkDestroyed;
    QTimer m_previewTimer;
    QTimer m_inputSaveTimer;
    std::unique_ptr<ObsInputStore> m_inputStore;
    bool m_running = false;
    bool m_receiving = false;
    QString m_status = QStringLiteral("OBS input is off");
    QString m_error, m_videoCodec, m_audioCodec, m_resolution;
    qint64 m_frameCount = 0;
    quint64 m_generation = 0;
    InputMode m_inputMode = DirectFromObs;
    // Connection details are optional dedicated-wallet data; never logs or
    // plaintext preferences, and never related to Discord account credentials.
    QString m_remoteServer, m_remoteStreamKey;
    QVariantMap m_broadcastContext;
    bool m_broadcastCapture = false;
    bool m_stopWhenBroadcastEnds = false;
};
