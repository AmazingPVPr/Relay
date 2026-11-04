#pragma once
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QWebSocket>
#include <QLocalServer>
#include <QLockFile>
#include <memory>
class RelayPeerTransport;

// Intentionally has no DiscordClient, Discord token, upload, URL-fetch or action dependency.
class RelayPeers final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(bool mcpEnabled READ mcpEnabled WRITE setMcpEnabled NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString identity READ identity NOTIFY changed)
    Q_PROPERTY(QString rendezvousUrl READ rendezvousUrl WRITE setRendezvousUrl NOTIFY changed)
    Q_PROPERTY(QString contactCard READ contactCard NOTIFY changed)
    Q_PROPERTY(QVariantList contacts READ contacts NOTIFY changed)
    Q_PROPERTY(QString selectedPeer READ selectedPeer WRITE setSelectedPeer NOTIFY changed)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY changed)
    Q_PROPERTY(QString storagePath READ storagePath CONSTANT)
    Q_PROPERTY(QString mcpEndpoint READ mcpEndpoint NOTIFY changed)
    Q_PROPERTY(QString mcpConfiguration READ mcpConfiguration NOTIFY changed)
public:
    explicit RelayPeers(const QString &directory, const QStringList &stunServers = {"stun:stun.l.google.com:19302", "stun:stun.cloudflare.com:3478"}, QObject *parent = nullptr);
    ~RelayPeers() override;
    bool enabled() const { return m_enabled; }
    bool mcpEnabled() const { return m_mcp.isListening(); }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    QString identity() const { return QString::fromLatin1(m_public.toHex()); }
    QString rendezvousUrl() const { return m_url; }
    QString contactCard() const;
    QVariantList contacts() const;
    QString selectedPeer() const { return m_selected; }
    QVariantList messages() const;
    QString storagePath() const { return m_directory; }
    QString mcpEndpoint() const;
    QString mcpConfiguration() const;
    Q_INVOKABLE QVariantList messagesForPeer(const QString &peer);
    void setEnabled(bool enabled);
    void setMcpEnabled(bool enabled);
    void setRendezvousUrl(const QString &url);
    void setSelectedPeer(const QString &peer);
    Q_INVOKABLE QString addContact(const QString &card, const QString &name, const QString &binding = {}, bool externalSetup = false);
    Q_INVOKABLE QString peerForBinding(const QString &binding) const;
    Q_INVOKABLE bool bindContact(const QString &peer, const QString &binding);
    Q_INVOKABLE void setAiAccess(const QString &peer, bool allowed);
    Q_INVOKABLE bool removeContact(const QString &peer);
    Q_INVOKABLE bool clearHistory(const QString &peer);
    Q_INVOKABLE bool sendText(const QString &peer, const QString &text);
    Q_INVOKABLE void reconnect(const QString &peer);
    bool streamConnected(const QString &peer) const;
    bool sendStreamControl(const QString &peer, const QJsonObject &frame);
    bool sendStreamPacket(const QString &peer, const QByteArray &packet);
    qsizetype streamBufferedAmount(const QString &peer) const;
    // Only called by the private Relay MCP pipe, with the same grant checks as UI state.
    QJsonObject mcpRequest(const QJsonObject &request);
    static int runMcp(const QString &endpoint);
signals:
    void changed();
    void streamReady(const QString &peer);
    void streamDisconnected(const QString &peer);
    void streamControlReceived(const QString &peer, const QJsonObject &frame);
    void streamPacketReceived(const QString &peer, const QByteArray &packet);
    void externalSignal(const QString &peer, const QString &payload);
    void externalReconnect(const QString &peer);
private:
    friend class RelayPeersTest;
    friend class RelayDiscordBridge;
    friend class RelayDiscordBridgeTest;
    struct Contact { QString name, binding; bool ai = false, active = true, external = false; QJsonArray history; bool historyLoaded = false, damaged = false; QString state = "Offline"; };
    bool initialize();
    bool fail(const QString &error);
    bool saveSettings();
    bool loadHistory(const QString &peer);
    bool saveHistory(const QString &peer);
    QByteArray seal(const QByteArray &plain) const;
    QByteArray unseal(const QByteArray &cipher) const;
    bool writePrivate(const QString &path, const QByteArray &bytes);
    bool queueText(const QString &peer, const QString &text, QString *id = nullptr);
    void processFrame(const QString &peer, const QJsonObject &frame);
    void flush(const QString &peer);
    void pollPresence();
    bool needsSetupService() const;
    void startExternalConnection(const QString &peer);
    void setContactActive(const QString &peer, bool active);
    void signalReceived(const QJsonObject &message, bool external = false);
    void sendDescription(const QString &peer, const QString &session, const QString &sdp, const QString &type);
    RelayPeerTransport *makeTransport(const QString &peer, const QString &session);
    void closeTransport(const QString &peer);
    void acceptMcp();
    QString m_directory, m_url, m_selected, m_error, m_status = "Relay messaging is off";
    QByteArray m_public, m_secret, m_diskKey;
    QHash<QString, Contact> m_contacts;
    QJsonObject m_pairings;
    QHash<QString, RelayPeerTransport *> m_transports;
    QHash<QString, QString> m_sessions;
    QHash<QString, qint64> m_lastAttempts;
    QHash<QString, QHash<QString, qint64>> m_inFlight;
    QHash<QString, qint64> m_seenSignals;
    QWebSocket m_signal;
    QTimer m_poll;
    QLocalServer m_mcp;
    std::unique_ptr<QLockFile> m_lock, m_mcpLock;
    QStringList m_stun;
    bool m_enabled = false, m_ready = false, m_initialized = false, m_externalOnly = false;
};
