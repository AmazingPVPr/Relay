#pragma once
#include <QObject>
#include <QJsonObject>
#include <QHash>
#include <QPointer>
#include <QQueue>
#include <QTimer>
class DiscordClient;
class RelayPeers;

// Discord is used only for consent, public identity exchange, and encrypted ICE
// setup. RelayPeers and its MCP have no route to send chat text through this bridge.
class RelayDiscordBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList invitations READ invitations NOTIFY changed)
public:
    RelayDiscordBridge(DiscordClient *client, RelayPeers *peers, QObject *parent = nullptr);
    QVariantList invitations() const;
    Q_INVOKABLE QVariantMap conversation(const QString &binding) const;
    Q_INVOKABLE void setConversationEnabled(const QString &binding, const QString &channel, bool enabled);
    Q_INVOKABLE void reconnect(const QString &binding);
    Q_INVOKABLE void dismiss(const QString &binding);
    Q_INVOKABLE void resetIdentity(const QString &binding);
    static QString setupSummary(const QString &content);
signals:
    void changed();
    void invitationReceived(const QString &channel, const QString &name);
private:
    friend class RelayDiscordBridgeTest;
    struct Outbound { QString binding, channel, content, kind, offer; quint64 generation = 0; };
    struct Assembly { int count = 0; qint64 created = 0; QHash<int,QString> parts; };
    QString bindingFor(const QString &channel, const QString &author = {}) const;
    QString nameFor(const QString &channel) const;
    bool persist(const QString &binding, const QJsonObject &row);
    void accountChanged();
    void scan();
    void receive(const QJsonObject &message);
    void connectPeer(const QString &binding);
    void sendInvite(const QString &binding, const QString &replyTo = {});
    void queue(const QString &binding, QJsonObject body);
    void sendNext();
    void sendSignal(const QString &peer, const QString &payload);
    void status(const QString &binding, const QString &text);
    static QJsonObject decode(const QString &content);
    static QString encode(const QJsonObject &body);
    QPointer<DiscordClient> m_client;
    QPointer<RelayPeers> m_peers;
    QString m_account;
    quint64 m_generation = 0;
    bool m_sending = false, m_connected = false;
    QQueue<Outbound> m_outbound;
    QHash<QString, QString> m_scanned;
    QHash<QString, Assembly> m_assemblies;
    QTimer m_scanTimer;
};
