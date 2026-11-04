#pragma once

#include <QLocalServer>
#include <QObject>
#include <QSet>
#include <QVariantMap>
#include <functional>
#include <memory>

class QLocalSocket;
class QLockFile;

// A private, same-user command channel for desktop shortcuts and stream decks.
// It never exposes a token, message, account identifier, or network listener.
class ControlServer final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit ControlServer(QObject *parent = nullptr);
    ~ControlServer() override;
    bool running() const { return m_server.isListening(); }
    QString error() const { return m_error; }
    bool start(const QString &serverName = {});
    void stop();
    QString serverName() const { return m_server.fullServerName(); }
    void setStatusProvider(std::function<QVariantMap()> provider);
    void setDiagnosticsProvider(std::function<QVariantMap()> provider);
    Q_INVOKABLE bool execute(const QString &action);
    Q_INVOKABLE QVariantMap status() const;
    QVariantMap diagnostics() const;

    static QStringList supportedActions();
    static QString defaultServerName();
    // Blocking CLI helper; call before constructing the application's window.
    static QVariantMap sendCommand(const QString &action, int timeoutMs = 1500,
                                   const QString &serverName = {});
signals:
    void actionRequested(const QString &action);
    void changed();
private:
    void acceptConnections();
    void handleRequest(QLocalSocket *socket);
    void finish(QLocalSocket *socket, const QVariantMap &response);
    static bool sameUser(QLocalSocket *socket);
    QLocalServer m_server;
    QSet<QLocalSocket *> m_clients;
    QString m_error;
    std::function<QVariantMap()> m_statusProvider;
    std::function<QVariantMap()> m_diagnosticsProvider;
    std::unique_ptr<QLockFile> m_lock;
};
