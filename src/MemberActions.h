#pragma once

#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QSet>
#include <QTimer>
#include <QVariantMap>
#include <memory>

class DiscordClient;
class VoiceEngine;

class MemberActions final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(qulonglong revision READ revision NOTIFY changed)
public:
    explicit MemberActions(DiscordClient *client, VoiceEngine *voice, QObject *parent = nullptr);
    MemberActions(DiscordClient *client, VoiceEngine *voice, const QString &settingsFile, QObject *parent = nullptr);
    ~MemberActions() override;
    bool busy() const { return m_busy; }
    QString error() const { return m_error; }
    qulonglong revision() const { return m_revision; }
    Q_INVOKABLE QVariantMap state(const QString &userId) const;
    Q_INVOKABLE bool setLocalMuted(const QString &userId, bool muted);
    Q_INVOKABLE bool setLocalVolume(const QString &userId, double volume);
    Q_INVOKABLE bool setLocalSoundboardVolume(const QString &userId, double volume);
    Q_INVOKABLE bool setBlocked(const QString &userId, bool blocked);
    Q_INVOKABLE bool copyId(const QString &id);
    Q_INVOKABLE bool copyMessageLink(const QString &guildId, const QString &channelId, const QString &messageId);
    Q_INVOKABLE void clearError();
signals:
    void changed();
    void blockedChanged(const QString &userId, bool blocked);
private:
    friend class MemberActionsTest;
    struct LocalAudio { bool muted = false; double volume = 1; double soundboardVolume = 1; };
    void synchronizeAccount();
    void rememberAudio(const QString &userId);
    void savePending();
    void notify();
    bool validateLocalTarget(const QString &userId);
    static QString messageLink(const QString &guildId, const QString &channelId, const QString &messageId);
    static bool copyText(const QString &text);
    QPointer<DiscordClient> m_client;
    QPointer<VoiceEngine> m_voice;
    std::unique_ptr<QSettings> m_settings;
    QTimer m_saveTimer;
    QHash<QString, LocalAudio> m_local;
    QSet<QString> m_pending;
    QString m_account, m_error;
    quint64 m_session = 0, m_operation = 0, m_revision = 0;
    bool m_busy = false, m_loading = false;
};
