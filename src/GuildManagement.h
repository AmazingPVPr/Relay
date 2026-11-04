#pragma once
#include <QObject>
#include <QPointer>
#include <QJsonObject>
#include <QVariantMap>
#include <QVariantList>
#include <QSet>
class DiscordClient;

// Native management over the signed-in client's existing request queue. Every
// mutation is an explicit UI action; the service never stores an account secret.
class GuildManagement final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString guildId READ guildId NOTIFY changed)
    Q_PROPERTY(QVariantMap guild READ guild NOTIFY changed)
    Q_PROPERTY(QVariantMap capabilities READ capabilities NOTIFY changed)
    Q_PROPERTY(QVariantMap permissions READ capabilities NOTIFY changed)
    Q_PROPERTY(QVariantList roles READ roles NOTIFY changed)
    Q_PROPERTY(QVariantList channels READ channels NOTIFY changed)
    Q_PROPERTY(QString section READ section NOTIFY changed)
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool mutating READ mutating NOTIFY changed)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit GuildManagement(DiscordClient *client,QObject *parent=nullptr);
    QString guildId() const { return m_guildId; }
    QVariantMap guild() const;
    QVariantMap capabilities() const;
    QVariantList roles() const;
    QVariantList channels() const;
    QString section() const { return m_section; }
    QVariantList items() const { return m_items; }
    bool busy() const { return m_busy; }
    bool mutating() const { return m_mutating; }
    bool hasMore() const { return m_hasMore; }
    QString error() const { return m_error; }
    QString status() const { return m_status; }
    Q_INVOKABLE void selectGuild(const QString &id);
    Q_INVOKABLE void loadSection(const QString &section);
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void clearError() { m_error.clear(); emit changed(); }
    Q_INVOKABLE QVariantMap userPermissions(const QString &guildId,const QString &userId) const;
    Q_INVOKABLE QVariantMap channelPermissions(const QString &guildId,const QString &channelId) const;
    Q_INVOKABLE bool execute(const QString &action,const QString &targetId,const QVariantMap &values={});
signals:
    void changed();
    void actionCompleted(const QString &action,const QString &id,const QVariantMap &result);
private:
    friend class GuildManagementTest;
    friend class Soundboard;
    friend class SoundboardTest;
    void reset();
    bool ready() const;
    bool fail(const QString &reason);
    QJsonObject snapshot(const QString &guildId) const;
    QJsonObject member(const QString &guildId,const QString &userId) const;
    QJsonObject channel(const QString &guildId,const QString &channelId) const;
    QJsonObject role(const QString &roleId) const;
    quint64 permissionBits(const QString &guildId,const QString &channelId={}) const;
    bool roleEditable(const QJsonObject &role) const;
    QVariantMap itemRow(const QString &section,const QJsonObject &object) const;
    void fetchOverview();
    void fetchItems(bool more);
    void acceptGuild(const QJsonObject &data);
    bool prepare(const QString &action,const QString &targetId,const QVariantMap &values,QByteArray &method,QString &path,QJsonObject &body);
    bool imageData(const QVariant &source,QString &data);
    bool leaveGuild(const QString &target);
    QPointer<DiscordClient> m_client;
    QString m_guildId,m_section="overview",m_error,m_status,m_cursor;
    QJsonObject m_guild;
    QHash<QString,QJsonObject> m_members,m_events;
    QVariantList m_items;
    quint64 m_session=0,m_generation=0,m_readOperation=0,m_writeOperation=0;
    bool m_busy=false,m_mutating=false,m_hasMore=false,m_refreshPending=false;
    QSet<QString> m_pendingLeaves;
};
