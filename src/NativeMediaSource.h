#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QVariantList>
#include <QUrl>
class QMediaPlayer;
class QNetworkReply;
class QBuffer;

// Feed one HLS video variant plus its original audio group to Qt. This avoids
// probing every resolution/codec before playback while retaining quality choices.
class NativeMediaSource : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(QVariantList qualityOptions READ qualityOptions NOTIFY changed)
    Q_PROPERTY(int activeQuality READ activeQuality NOTIFY changed)
public:
    explicit NativeMediaSource(QObject *parent=nullptr);
    ~NativeMediaSource() override;
    bool active() const {return m_active;}
    QVariantList qualityOptions() const;
    int activeQuality() const {return m_selected;}
    Q_INVOKABLE bool open(QObject *player,const QString &source);
    Q_INVOKABLE bool selectQuality(int index);
    Q_INVOKABLE void cancel();
signals:
    void changed();
private:
    friend class NativeMediaSourceTest;
    struct Variant {QString stream,uri,codec;int width=0,height=0;double fps=0;};
    bool parse(const QByteArray &bytes);
    QByteArray manifestFor(int index) const;
    void fallback(quint64 generation);
    bool trusted(const QUrl &url) const;
    static QHash<QString,QString> attributes(const QString &text);
    QNetworkAccessManager m_network;
    QPointer<QMediaPlayer> m_player;
    QPointer<QNetworkReply> m_reply;
    QUrl m_source;
    QStringList m_common;
    QList<Variant> m_variants;
    QHash<int,QPointer<QBuffer>> m_buffers;
    quint64 m_generation=0;
    int m_selected=-1;
    bool m_active=false;
    QUrl m_fixtureOrigin; // Generated HTTP fixture only, not exposed to QML.
};
