#pragma once
#include <QObject>
#include <QVariantList>
#include <QPointer>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QBuffer>
#include <QCache>
#include <QNetworkAccessManager>
#include <QMap>
class QNetworkReply;
class QJsonArray;
class DiscordClient;
class VoiceEngine;
class GuildManagement;
class AppSettings;
class Soundboard final:public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList sounds READ sounds NOTIFY changed)
    Q_PROPERTY(QVariantList favorites READ favorites NOTIFY changed)
    Q_PROPERTY(QVariantList frequent READ frequent NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool sending READ sending NOTIFY changed)
    Q_PROPERTY(bool canPlay READ canPlay NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString guildId READ guildId NOTIFY changed)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY changed)
public:
    Soundboard(DiscordClient*,VoiceEngine*,GuildManagement*,AppSettings*,QObject *parent=nullptr);
    ~Soundboard() override;
    QVariantList sounds()const{return m_sounds;}
    QVariantList favorites() const;
    QVariantList frequent() const;
    bool busy()const{return m_pending>0;}
    bool sending()const{return m_sending;}
    bool canPlay()const;
    QString error()const{return m_error;}
    QString guildId()const{return m_guild;}
    bool previewLoading()const{return m_previewLoading;}
    Q_INVOKABLE void load(const QString &guildId);
    Q_INVOKABLE bool play(const QString &soundId);
    Q_INVOKABLE bool preview(const QString &soundId);
    Q_INVOKABLE bool activate(const QString &soundId,bool previewOnly=false);
    Q_INVOKABLE bool setFavorite(const QString &soundId,bool favorite);
    Q_INVOKABLE bool isFavorite(const QString &soundId) const;
    Q_INVOKABLE void stopPreview();
signals:void changed();
private:
    friend class SoundboardTest;
    void reset();
    void syncLibrary();
    void saveLibrary();
    void rememberUse(const QString &id);
    void finishActivation();
    QVariantMap libraryRow(const QString &id) const;
    QVariantMap find(const QString&)const;
    void add(const QJsonArray&,const QString&);
    struct ReceivedClip;
    void receiveEffect(const QJsonObject &event);
    void startReceivedClip(ReceivedClip *clip,const QByteArray &bytes);
    void finishReceivedClip(ReceivedClip *clip,const QString &error = {});
    void stopReceivedClips();
    bool receivedClipCurrent(const ReceivedClip *clip) const;
    void startPreview(const QString&,double);
    void playDownloaded(const QByteArray&);
    QPointer<DiscordClient> m_client;QPointer<VoiceEngine> m_voice;GuildManagement *m_permissions;AppSettings *m_settings;double m_sourceVolume=1;
    QVariantList m_sounds;QString m_error,m_guild;quint64 m_generation=0,m_operation=0;int m_pending=0;bool m_sending=false;
    QMediaPlayer m_preview;QAudioOutput m_output;QElapsedTimer m_cooldown;
    QNetworkAccessManager m_mediaNetwork;QPointer<QNetworkReply> m_download;
    QBuffer m_previewData;QCache<QString,QByteArray> m_audioCache{8192};
    QString m_cdnBase="https://cdn.discordapp.com/soundboard-sounds/";
    QString m_libraryAccount,m_libraryFile,m_pendingActivation,m_activationVoiceChannel,m_activationVoiceGuild;
    QMap<QString,QVariantMap> m_library;
    bool m_activationPreview=false;
    QString m_previewId;
    quint64 m_previewGeneration=0;bool m_previewLoading=false;
    QList<ReceivedClip*> m_received;
    QString m_receivedGuild,m_receivedChannel;
};
