#pragma once
#include <QObject>
#include <QJsonObject>
#include <QVariantMap>
#include <memory>
class QUrl;

// UI-facing broadcaster transport. Socket work, encryption and pacing belong
// to a dedicated event loop, independently of capture and chat rendering.
class BroadcastTransport final : public QObject {
    Q_OBJECT
public:
    explicit BroadcastTransport(QObject *parent=nullptr);
    ~BroadcastTransport() override;
    bool available() const;
    bool active() const;
    bool connected() const;
    bool encryptedReady() const;
    QString streamKey() const;
    QString status() const;
    QString error() const;
    QString diagnosticSummary() const;
    QVariantMap publishingDiagnostics() const;
    quint64 frameCount() const;
    int publishingBitrate() const;
    void setIdentity(const QString &user,const QString &session);
    void publish(const QString &key,int width,int height,int fps,int bitrate);
    bool publishVideo(const QByteArray &frame,quint32 timestamp,int width,int height);
    bool publishAudio(const QByteArray &opus,quint32 timestamp);
    void stop();
    void onStreamCreate(const QJsonObject &event);
    void onStreamServer(const QJsonObject &event);
    bool configureLocalFixture(const QUrl &url);
signals:
    void changed();
    void stopRequested(QString key);
    void keyFrameRequested();
    void publishingBitrateChanged(int bitrate);
private:
    struct Private;
    std::unique_ptr<Private> d;
};
