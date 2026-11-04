#include "RelayRendezvous.h"
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QDateTime>
#include <QRegularExpression>
#include <sodium.h>
namespace {
void send(QWebSocket *socket, const QJsonObject &message) {
    if (socket->bytesToWrite() > 262144) { socket->close(); return; }
    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}
const QRegularExpression KeyPattern("^[0-9a-f]{64}$");
}
RelayRendezvous::RelayRendezvous(QObject *parent) : QObject(parent), m_server("Relay setup", QWebSocketServer::NonSecureMode, this) {
    m_server.setMaxPendingConnections(32);
    m_server.setHandshakeTimeout(5000);
    connect(&m_server, &QWebSocketServer::newConnection, this, &RelayRendezvous::accept);
}
RelayRendezvous::~RelayRendezvous() {
    m_server.close();
    const auto sockets = m_server.findChildren<QWebSocket *>();
    for (auto *socket : sockets) { socket->disconnect(this); socket->close(); }
    m_online.clear();
}
bool RelayRendezvous::listen(const QHostAddress &address, quint16 port) { return sodium_init() >= 0 && m_server.listen(address, port); }
void RelayRendezvous::accept() {
    while (m_server.hasPendingConnections()) {
        auto *socket = m_server.nextPendingConnection();
        if (m_server.findChildren<QWebSocket *>().size() > 512) { socket->close(); socket->deleteLater(); continue; }
        socket->setMaxAllowedIncomingFrameSize(100000); socket->setMaxAllowedIncomingMessageSize(100000);
        QByteArray challenge(32, Qt::Uninitialized); randombytes_buf(challenge.data(), 32);
        socket->setProperty("challenge", challenge);
        socket->setProperty("budget", 60);
        socket->setProperty("minute", QDateTime::currentSecsSinceEpoch() / 60);
        send(socket, {{"challenge", QString::fromLatin1(challenge.toBase64())}});
        QTimer::singleShot(5000, socket, [socket] { if (socket->property("identity").toString().isEmpty()) socket->close(); });
        auto *heartbeat = new QTimer(socket); heartbeat->setInterval(20000);
        connect(heartbeat, &QTimer::timeout, socket, [socket] {
            if (QDateTime::currentSecsSinceEpoch() - socket->property("lastPong").toLongLong() > 60) { socket->close(); return; }
            socket->ping();
        });
        socket->setProperty("lastPong", QDateTime::currentSecsSinceEpoch()); heartbeat->start();
        connect(socket, &QWebSocket::pong, socket, [socket] { socket->setProperty("lastPong", QDateTime::currentSecsSinceEpoch()); });
        connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &text) { receive(socket, text); });
        connect(socket, &QWebSocket::binaryMessageReceived, socket, [socket] { socket->close(); });
        connect(socket, &QWebSocket::disconnected, this, [this, socket] {
            const auto key = socket->property("identity").toString();
            if (m_online.value(key) == socket) m_online.remove(key);
            socket->deleteLater();
        });
    }
}
void RelayRendezvous::receive(QWebSocket *socket, const QString &text) {
    if (text.toUtf8().size() > 100000) { socket->close(); return; }
    const auto minute = QDateTime::currentSecsSinceEpoch() / 60;
    if (socket->property("minute").toLongLong() != minute) { socket->setProperty("minute", minute); socket->setProperty("budget", 60); }
    const auto budget = socket->property("budget").toInt();
    if (budget <= 0) { socket->close(); return; }
    socket->setProperty("budget", budget - 1);
    const auto message = QJsonDocument::fromJson(text.toUtf8()).object();
    auto identity = socket->property("identity").toString();
    if (identity.isEmpty()) {
        const auto key = message.value("key").toString();
        const auto signature = QByteArray::fromBase64(message.value("signature").toString().toLatin1());
        const auto publicKey = QByteArray::fromHex(key.toLatin1());
        const auto challenge = socket->property("challenge").toByteArray(); socket->setProperty("challenge", {});
        const auto data = QByteArray("relay-rendezvous-v1:") + challenge;
        if (!KeyPattern.match(key).hasMatch() || challenge.size() != 32 || signature.size() != crypto_sign_BYTES ||
            crypto_sign_verify_detached(reinterpret_cast<const unsigned char *>(signature.constData()),
                reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size()), reinterpret_cast<const unsigned char *>(publicKey.constData())) != 0 ||
            m_online.contains(key)) { socket->close(); return; }
        socket->setProperty("identity", key); m_online.insert(key, socket); send(socket, {{"ready", true}}); return;
    }
    if (message.keys() == QStringList{"lookup"} && message.value("lookup").isArray()) {
        const auto requested = message.value("lookup").toArray();
        if (requested.size() > 128) { socket->close(); return; }
        QJsonArray online;
        for (const auto &peer : requested) if (m_online.contains(peer.toString())) online.append(peer);
        send(socket, {{"online", online}}); return;
    }
    if (message.keys() != QStringList{"payload", "to"}) { socket->close(); return; }
    const auto peer = message.value("to").toString();
    const auto payload = message.value("payload").toString();
    if (!KeyPattern.match(peer).hasMatch() || payload.size() > 90000 || payload.isEmpty() || QByteArray::fromBase64(payload.toLatin1()).size() > 65536) { socket->close(); return; }
    if (auto target = m_online.value(peer)) send(target, {{"from", identity}, {"payload", payload}});
    // Offline packets are dropped. No database, queue, history or disk writes.
}
