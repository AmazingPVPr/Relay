#pragma once
#include <QObject>
#include <QWebSocketServer>
#include <QHash>
#include <QPointer>
class QWebSocket;
// Memory-only setup routing. It neither accepts nor stores conversation messages.
class RelayRendezvous final : public QObject {
    Q_OBJECT
public:
    explicit RelayRendezvous(QObject *parent = nullptr);
    ~RelayRendezvous() override;
    bool listen(const QHostAddress &address = QHostAddress::LocalHost, quint16 port = 39872);
    quint16 port() const { return m_server.serverPort(); }
    int onlineCount() const { return m_online.size(); }
private:
    void accept();
    void receive(QWebSocket *socket, const QString &text);
    QWebSocketServer m_server;
    QHash<QString, QPointer<QWebSocket>> m_online;
};
