#pragma once
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <memory>
class VoiceEngine;
class StreamDecoder;
class QVideoSink;

// An explicitly selected remote camera uses the existing encrypted voice
// connection. This receiver never opens or transmits a local camera.
class CameraEngine final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool receiving READ receiving NOTIFY changed)
    Q_PROPERTY(QString streamKey READ streamKey NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString codec READ codec NOTIFY changed)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY changed)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY changed)
    Q_PROPERTY(quint64 frameCount READ frameCount NOTIFY changed)
    Q_PROPERTY(QObject* videoSink READ videoSink WRITE setVideoSink NOTIFY videoSinkChanged)
public:
    explicit CameraEngine(VoiceEngine *voice,QObject *parent=nullptr);
    ~CameraEngine() override;
    bool available() const;bool active()const{return m_active;}
    bool connected()const;bool receiving()const{return m_receiving;}
    QString streamKey()const{return m_user;}QString title()const{return m_title;}
    QString status()const;QString error()const{return m_error;}QString codec()const{return m_codec;}
    int videoWidth()const{return m_width;}int videoHeight()const{return m_height;}
    quint64 frameCount()const{return m_frames;}
    QObject *videoSink()const;void setVideoSink(QObject *sink);
    Q_INVOKABLE void watch(const QString &userId,const QString &title);
    Q_INVOKABLE void stop();
signals:
    void changed();void videoSinkChanged();
    void watchRequested();void stopRequested();
private:
    void reset();void poll();
    VoiceEngine *m_voice;
    QPointer<QVideoSink> m_sink;
    QTimer m_poll;
    std::unique_ptr<StreamDecoder> m_decoder;
    QString m_user,m_title,m_error,m_codec;
    bool m_active=false,m_receiving=false,m_announced=false;
    int m_width=0,m_height=0;
    int m_rotation=0;bool m_mirrored=false;
    quint64 m_frames=0;
    qint64 m_lastFrame=0;
};
