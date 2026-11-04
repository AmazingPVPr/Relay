#pragma once
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <QJsonObject>
#include <QSet>
#include <memory>
class DiscordClient;
class QSettings;

// Explicit menu actions use the existing account client's bounded REST queue.
// Local organization is scoped to the signed-in account; it contains IDs only.
class ContextActions final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    explicit ContextActions(DiscordClient *client, QObject *parent = nullptr);
    ContextActions(DiscordClient *client, const QString &settingsFile, QObject *parent = nullptr);
    ~ContextActions() override;
    bool busy() const { return m_busy; }
    QString error() const { return m_error; }
    int revision() const { return m_revision; }
    Q_INVOKABLE void clearError();
    Q_INVOKABLE QVariantMap dmInfo(const QString &channelId) const;
    Q_INVOKABLE QVariantMap guildInfo(const QString &guildId) const;
    Q_INVOKABLE QString dmChannelForUser(const QString &userId) const;
    Q_INVOKABLE bool markDmRead(const QString &channelId);
    Q_INVOKABLE bool closeDm(const QString &channelId);
    Q_INVOKABLE bool setDmPinned(const QString &channelId, bool enabled);
    Q_INVOKABLE bool setDmFavorite(const QString &channelId, bool enabled);
    Q_INVOKABLE bool muteDm(const QString &channelId, int seconds);
    Q_INVOKABLE bool markGuildRead(const QString &guildId);
    Q_INVOKABLE bool muteGuild(const QString &guildId, int seconds);
    Q_INVOKABLE bool setGuildNotificationLevel(const QString &guildId, int level);
    Q_INVOKABLE bool setGuildHideMuted(const QString &guildId, bool enabled);
    Q_INVOKABLE bool setGuildShowAll(const QString &guildId, bool enabled);
    Q_INVOKABLE bool setGuildNickname(const QString &guildId, const QString &nickname);
signals:
    void changed();
private:
    friend class ContextActionsTest;
    void synchronizeAccount();
    void notify();
    bool fail(const QString &error);
    bool ready();
    bool knownDm(const QString &id) const;
    bool knownGuild(const QString &id) const;
    bool saveLocal(const QString &key, const QSet<QString> &values);
    bool organizeDm(const QString &id, bool enabled, bool pinned);
    bool patchNotification(const QString &guildId, const QJsonObject &body);
    static bool muteBody(int seconds, QJsonObject &body);
    static bool muted(const QJsonObject &settings);
    QPointer<DiscordClient> m_client;
    std::unique_ptr<QSettings> m_settings;
    QString m_account, m_error;
    quint64 m_session = 0, m_operation = 0;
    int m_revision = 0;
    bool m_busy = false, m_synchronizing = false;
};
