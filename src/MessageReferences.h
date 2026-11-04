#pragma once
#include <QObject>
#include <QPointer>
#include <QJsonObject>
#include <QSet>
#include <QHash>
#include <QVariantList>
class DiscordClient;

// Native message destinations and member-owned Channels & Roles choices.
class MessageReferences final : public QObject {
    Q_OBJECT
    Q_PROPERTY(quint64 revision READ revision NOTIFY referencesChanged)
    Q_PROPERTY(QString guildId READ guildId NOTIFY changed)
    Q_PROPERTY(QString guildName READ guildName NOTIFY changed)
    Q_PROPERTY(QString section READ section NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool saving READ saving NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QVariantList prompts READ prompts NOTIFY changed)
    Q_PROPERTY(QVariantList channels READ channels NOTIFY changed)
public:
    explicit MessageReferences(DiscordClient *client, QObject *parent=nullptr);
    quint64 revision() const { return m_revision; }
    QString guildId() const { return m_guild; }
    QString guildName() const;
    QString section() const { return m_section; }
    QString error() const { return m_error; }
    QString status() const { return m_status; }
    bool loading() const { return m_loading; }
    bool saving() const { return m_saving; }
    bool dirty() const { return m_selected != m_original; }
    QVariantList prompts() const;
    QVariantList channels() const;
    Q_INVOKABLE QVariantMap resolve(const QString &text, const QString &guildId) const;
    Q_INVOKABLE bool activate(const QString &link);
    Q_INVOKABLE bool openGuild(const QString &guildId, const QString &section="customize");
    Q_INVOKABLE void close();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool choose(const QString &promptId, const QString &optionId, bool selected);
    Q_INVOKABLE bool save();
signals:
    void changed();
    void referencesChanged();
    void panelRequested();
    void channelRequested(const QString &channelId);
private:
    friend class MessageReferencesTest;
    bool ready() const;
    bool memberOf(const QString &guild) const;
    void reset();
    void invalidateReferences();
    QJsonObject knownChannel(const QString &id, QString *guild=nullptr) const;
    void acceptOnboarding(const QJsonObject &data);
    void refreshAccess(const QString &guild, quint64 session);
    QPointer<DiscordClient> m_client;
    QHash<QString,QJsonObject> m_channelsById;
    QHash<QString,QString> m_channelGuilds;
    quint64 m_session=0,m_revision=0,m_operation=0,m_channelOperation=0;
    QString m_guild,m_section="customize",m_error,m_status;
    QVariantList m_prompts;
    QSet<QString> m_selected,m_original;
    QJsonObject m_seenPrompts,m_seenOptions;
    bool m_loading=false,m_saving=false,m_loaded=false;
};
