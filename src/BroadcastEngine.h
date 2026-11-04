#pragma once
#include <QObject>
#include <QJsonObject>
#include <QVariantList>
#include <memory>
#include <QVideoFrame>

class BroadcastEngine final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool sharing READ sharing NOTIFY changed)
    Q_PROPERTY(bool audioActive READ audioActive NOTIFY changed)
    Q_PROPERTY(bool audioSuppressed READ audioSuppressed NOTIFY changed)
    Q_PROPERTY(bool usesSystemPicker READ usesSystemPicker CONSTANT)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY changed)
    Q_PROPERTY(QString diagnosticSummary READ diagnosticSummary NOTIFY changed)
    Q_PROPERTY(QVariantList sources READ sources NOTIFY sourcesChanged)
    Q_PROPERTY(quint64 framesSent READ framesSent NOTIFY changed)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY changed)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY changed)
    Q_PROPERTY(bool externalSource READ externalSource NOTIFY changed)
    Q_PROPERTY(QString qualitySummary READ qualitySummary NOTIFY changed)
public:
    explicit BroadcastEngine(QObject *parent=nullptr);
    ~BroadcastEngine() override;
    bool available() const;bool active() const;bool sharing() const;bool audioActive() const;bool usesSystemPicker() const;
    QString status() const;QString error() const;QString sourceName() const;QString diagnosticSummary() const;
    QVariantMap publishingDiagnostics() const;
    QVariantList sources() const;quint64 framesSent() const;int videoWidth() const;int videoHeight() const;
    bool externalSource() const;
    bool audioSuppressed() const;
    // Temporarily exclude local microphone-test feedback from every stream input.
    void setAudioSuppressed(bool suppressed);
    QString qualitySummary() const;
    void setDirectViewersActive(bool active);
    void setIdentity(const QString &userId,const QString &voiceSessionId);
    Q_INVOKABLE void refreshSources();
    Q_INVOKABLE void start(const QString &guildId,const QString &channelId,const QString &sourceId,int maximumHeight=720,int fps=30,bool computerAudio=false);
    Q_INVOKABLE void stop();
    void startExternal(const QString &guildId, const QString &channelId, QObject *source,
                       int maximumHeight = 0, int fps = 0, bool streamAudio = true);
public slots:
    void onStreamCreate(const QJsonObject &event);
    void onStreamServer(const QJsonObject &event);
    void onStreamDelete(const QJsonObject &event);
signals:
    void sourceFrame(const QVideoFrame &frame, int maximumHeight, int fps);
    void sourceVideo(const QByteArray &h264, quint32 timestamp, bool keyframe);
    void sourceAudio(const QByteArray &pcm);
    void changed();void sourcesChanged();
    void startRequested(QString guildId,QString channelId);
    void stopRequested();
private:
    friend class BroadcastGatewayTest;
    bool configureLocalFixture(const QUrl &url);
    struct Private;std::unique_ptr<Private> d;
};
