#include "RelayPeers.h"
#include "RelayPeerTransport.h"
#include "control/ControlServer.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QLocalSocket>
#include <QUrl>
#include <QUuid>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <sodium.h>
#include <cmath>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {
constexpr qint64 MaxSave = 64 * 1024 * 1024;
constexpr int MaxHistory = 10000;
const QRegularExpression PeerPattern("^[0-9a-f]{64}$");
const QRegularExpression IdPattern("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
QByteArray json(const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
bool validText(const QString &text) {
    if (text.trimmed().isEmpty() || text.toUtf8().size() > 4000 || text.contains(QChar(0))) return false;
    for (const auto ch : text) if (ch.unicode() < 32 && ch != '\n' && ch != '\r' && ch != '\t') return false;
    // A strict UTF-8 round trip also rejects unpaired UTF-16 surrogates from UI/MCP.
    return QString::fromUtf8(text.toUtf8()) == text;
}
bool privateEntry(const QString &path, bool directory) {
    const QFileInfo info(path);
    if (info.isSymLink() || (directory ? !info.isDir() : !info.isFile())) return false;
#ifdef Q_OS_UNIX
    if (info.ownerId() != uint(geteuid())) return false;
    const auto unsafe = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                        QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    if (info.permissions() & unsafe) return false;
#endif
    return true;
}
QByteArray readBounded(const QString &path, qint64 limit) {
    if (!privateEntry(path, false)) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit) return {};
    return file.readAll();
}

}

RelayPeers::RelayPeers(const QString &directory, const QStringList &stun, QObject *parent)
    : QObject(parent), m_directory(directory), m_stun(stun) {
    m_initialized = initialize();
    m_signal.setMaxAllowedIncomingFrameSize(100000);
    m_signal.setMaxAllowedIncomingMessageSize(100000);
    connect(&m_signal, &QWebSocket::textMessageReceived, this, [this](const QString &message) {
        signalReceived(QJsonDocument::fromJson(message.toUtf8()).object());
    });
    connect(&m_signal, &QWebSocket::binaryMessageReceived, this, [this] { m_signal.close(); });
    connect(&m_signal, &QWebSocket::disconnected, this, [this] {
        m_ready = false;
        m_status = m_enabled ? "Setup service disconnected · existing direct chats stay connected" : "Relay messaging is off";
        emit changed();
    });
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (!m_enabled) return;
        if (needsSetupService() && m_signal.state() == QAbstractSocket::UnconnectedState) m_signal.open(QUrl(m_url));
        if (m_ready) pollPresence();
        for (const auto &peer : m_transports.keys()) if (m_transports[peer]->connected()) flush(peer);
    });
    m_poll.setInterval(5000);
    m_mcp.setSocketOptions(QLocalServer::UserAccessOption);
    m_mcp.setMaxPendingConnections(8);
    connect(&m_mcp, &QLocalServer::newConnection, this, &RelayPeers::acceptMcp);
}
RelayPeers::~RelayPeers() {
    setMcpEnabled(false);
    setEnabled(false);
    sodium_memzero(m_secret.data(), size_t(m_secret.size()));
    sodium_memzero(m_diskKey.data(), size_t(m_diskKey.size()));
}
bool RelayPeers::fail(const QString &error) { m_error = error; emit changed(); return false; }
bool RelayPeers::writePrivate(const QString &path, const QByteArray &bytes) {
    if (QFileInfo::exists(path) && !privateEntry(path, false)) return fail("Relay save file permissions are unsafe.");
    if (QFileInfo(path).isSymLink()) return fail("Relay does not write through linked save files.");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit()) return fail("Could not save Relay data. Check free space and permissions.");
    return true;
}
bool RelayPeers::initialize() {
    if (sodium_init() < 0) return fail("Relay encryption is unavailable.");
    if (!QFileInfo::exists(m_directory)) {
        if (!QDir().mkpath(m_directory) || !QFile::setPermissions(m_directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            return fail("Could not create a private Relay save directory.");
    }
    if (!privateEntry(m_directory, true)) return fail("Relay requires a private save directory owned by this user.");
    m_lock = std::make_unique<QLockFile>(m_directory + "/instance.lock");
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0)) return fail("Another Relay instance is using this messaging profile.");
    const auto identityFile = m_directory + "/identity.key";
    QByteArray seed;
    if (QFileInfo::exists(identityFile) || QFileInfo(identityFile).isSymLink()) {
        const auto bytes = readBounded(identityFile, 64);
        if (bytes.size() != 64) return fail("Relay identity file is unreadable or damaged. It has not been replaced.");
        seed = bytes.left(32); m_diskKey = bytes.mid(32);
    } else {
        if (QDir(m_directory).entryList({"*.relay"}, QDir::Files).size())
            return fail("Relay identity is missing beside existing encrypted saves. Restore the original identity.key.");
        seed.resize(32); m_diskKey.resize(32);
        randombytes_buf(seed.data(), 32); randombytes_buf(m_diskKey.data(), 32);
        if (!writePrivate(identityFile, seed + m_diskKey)) return false;
    }
    m_public.resize(crypto_sign_PUBLICKEYBYTES); m_secret.resize(crypto_sign_SECRETKEYBYTES);
    crypto_sign_seed_keypair(reinterpret_cast<unsigned char *>(m_public.data()), reinterpret_cast<unsigned char *>(m_secret.data()), reinterpret_cast<const unsigned char *>(seed.constData()));
    sodium_memzero(seed.data(), size_t(seed.size()));
    // There are no network requests until the user enables Relay messaging.
    m_url = QStringLiteral("wss://media.amazingpvper.com/relay-rendezvous");
    const auto settingsFile = m_directory + "/contacts.relay";
    if (QFileInfo::exists(settingsFile) || QFileInfo(settingsFile).isSymLink()) {
        const auto plain = unseal(readBounded(settingsFile, MaxSave));
        const auto settings = QJsonDocument::fromJson(plain).object();
        if (settings.value("owner").toString() != identity() || settings.value("kind").toString() != "contacts-v1")
            return fail("Relay contact save is damaged or cannot be decrypted. It has not been replaced.");
        m_url = settings.value("rendezvous").toString();
        m_pairings = settings.value("pairings").toObject();
        for (const auto &value : settings.value("contacts").toArray()) {
            const auto row = value.toObject();
            const auto peer = row.value("key").toString();
            if (!PeerPattern.match(peer).hasMatch() || peer == identity() || m_contacts.size() >= 128) return fail("Invalid Relay contacts save.");
            Contact contact;
            contact.name = row.value("name").toString().left(80);
            contact.binding = row.value("binding").toString().left(256);
            contact.ai = row.value("ai").toBool();
            contact.active = row.value("active").toBool(true);
            contact.external = row.value("external").toBool();
            m_contacts.insert(peer, contact);
        }
    }
    return true;
}
QByteArray RelayPeers::seal(const QByteArray &plain) const {
    QByteArray out(crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES + plain.size(), Qt::Uninitialized);
    randombytes_buf(out.data(), crypto_secretbox_NONCEBYTES);
    crypto_secretbox_easy(reinterpret_cast<unsigned char *>(out.data() + crypto_secretbox_NONCEBYTES),
        reinterpret_cast<const unsigned char *>(plain.constData()), size_t(plain.size()),
        reinterpret_cast<const unsigned char *>(out.constData()), reinterpret_cast<const unsigned char *>(m_diskKey.constData()));
    return out;
}
QByteArray RelayPeers::unseal(const QByteArray &cipher) const {
    if (m_diskKey.size() != crypto_secretbox_KEYBYTES || cipher.size() < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) return {};
    QByteArray out(cipher.size() - crypto_secretbox_NONCEBYTES - crypto_secretbox_MACBYTES, Qt::Uninitialized);
    if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char *>(out.data()),
        reinterpret_cast<const unsigned char *>(cipher.constData() + crypto_secretbox_NONCEBYTES), size_t(cipher.size() - crypto_secretbox_NONCEBYTES),
        reinterpret_cast<const unsigned char *>(cipher.constData()), reinterpret_cast<const unsigned char *>(m_diskKey.constData())) != 0) return {};
    return out;
}
bool RelayPeers::saveSettings() {
    if (!m_initialized) return false;
    QJsonArray rows;
    for (auto it = m_contacts.cbegin(); it != m_contacts.cend(); ++it)
        rows.append(QJsonObject{{"key", it.key()}, {"name", it->name}, {"binding", it->binding}, {"ai", it->ai}, {"active", it->active}, {"external", it->external}});
    return writePrivate(m_directory + "/contacts.relay", seal(json({{"kind", "contacts-v1"}, {"owner", identity()}, {"rendezvous", m_url}, {"contacts", rows}, {"pairings", m_pairings}})));
}
bool RelayPeers::loadHistory(const QString &peer) {
    if (!m_contacts.contains(peer)) return false;
    auto &contact = m_contacts[peer];
    if (contact.historyLoaded) return !contact.damaged;
    contact.historyLoaded = true;
    const auto path = m_directory + "/" + peer + ".relay";
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) return true;
    const auto document = QJsonDocument::fromJson(unseal(readBounded(path, MaxSave))).object();
    if (document.value("kind") != "history-v1" || document.value("owner") != identity() || document.value("peer") != peer ||
        !document.value("messages").isArray() || document.value("messages").toArray().size() > MaxHistory) {
        contact.damaged = true;
        return fail("This conversation save is damaged or cannot be decrypted. Sending is blocked to preserve it.");
    }
    const auto rows = document.value("messages").toArray();
    QSet<QString> ids;
    for (const auto &value : rows) {
        const auto row = value.toObject();
        const auto id = row.value("id").toString();
        if (!IdPattern.match(id).hasMatch() || ids.contains(id) || !validText(row.value("text").toString()) ||
            !row.value("own").isBool() || !row.value("pending").isBool()) {
            contact.damaged = true; return fail("This conversation save contains invalid records. It has not been changed.");
        }
        ids.insert(id);
    }
    contact.history = rows;
    return true;
}
bool RelayPeers::saveHistory(const QString &peer) {
    auto &contact = m_contacts[peer];
    if (contact.damaged) return false;
    return writePrivate(m_directory + "/" + peer + ".relay", seal(json({{"kind", "history-v1"}, {"owner", identity()}, {"peer", peer}, {"messages", contact.history}})));
}
QString RelayPeers::contactCard() const {
    if (!m_initialized) return {};
    return "relay-contact-v1:" + QString::fromLatin1(json({{"key", identity()}, {"rendezvous", m_url}}).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
QVariantList RelayPeers::contacts() const {
    QVariantList rows;
    auto peers = m_contacts.keys(); peers.sort();
    for (const auto &peer : peers) {
        const auto &contact = m_contacts[peer];
        rows.append(QVariantMap{{"key", peer}, {"name", contact.name}, {"binding", contact.binding}, {"ai", contact.ai}, {"active", contact.active}, {"external", contact.external}, {"connected", m_transports.contains(peer) && m_transports[peer]->connected()}, {"state", contact.state}});
    }
    return rows;
}
QVariantList RelayPeers::messages() const {
    QVariantList rows;
    if (!m_contacts.contains(m_selected)) return rows;
    const auto &history = m_contacts[m_selected].history;
    for (int i = qMax(0, int(history.size()) - 500); i < history.size(); ++i) rows.append(history[i].toObject().toVariantMap());
    return rows;
}
void RelayPeers::setSelectedPeer(const QString &peer) {
    if (!m_initialized || (!peer.isEmpty() && !m_contacts.contains(peer))) return;
    m_selected = peer;
    if (!peer.isEmpty()) loadHistory(peer);
    emit changed();
}
QString RelayPeers::addContact(const QString &card, const QString &name, const QString &binding, bool externalSetup) {
    if (!m_initialized || !card.startsWith("relay-contact-v1:") || card.size() > 4096 || binding.size() > 256) { fail("Invalid Relay contact card."); return {}; }
    const auto row = QJsonDocument::fromJson(QByteArray::fromBase64(card.mid(17).toLatin1(), QByteArray::Base64UrlEncoding)).object();
    const auto peer = row.value("key").toString();
    if (!PeerPattern.match(peer).hasMatch() || peer == identity() || row.value("rendezvous").toString() != m_url || m_contacts.size() >= 128) {
        fail("Use another person's contact card from the same setup service. Verify the full identity with them."); return {};
    }
    // Ed25519 public keys must also be valid for authenticated box encryption.
    unsigned char curve[32];
    const auto bytes = QByteArray::fromHex(peer.toLatin1());
    if (crypto_sign_ed25519_pk_to_curve25519(curve, reinterpret_cast<const unsigned char *>(bytes.constData())) != 0) { fail("Invalid contact identity."); return {}; }
    const auto previous = m_contacts;
    if (!m_contacts.contains(peer)) { Contact contact; contact.name = name.trimmed().left(80); if (contact.name.isEmpty()) contact.name = peer.left(12); m_contacts.insert(peer, contact); }
    if (externalSetup) m_contacts[peer].external = true;
    if (!binding.isEmpty()) {
        for (auto &contact : m_contacts) if (contact.binding == binding) contact.binding.clear();
        m_contacts[peer].binding = binding;
    }
    if (!saveSettings()) { m_contacts = previous; return {}; }
    m_error.clear(); setSelectedPeer(peer);
    if (m_ready) pollPresence();
    return peer;
}
QString RelayPeers::peerForBinding(const QString &binding) const {
    if (binding.isEmpty()) return {};
    for (auto it = m_contacts.cbegin(); it != m_contacts.cend(); ++it) if (it->binding == binding) return it.key();
    return {};
}
bool RelayPeers::bindContact(const QString &peer, const QString &binding) {
    if (!m_initialized || !m_contacts.contains(peer) || binding.isEmpty() || binding.size() > 256) return false;
    const auto previous = m_contacts;
    for (auto &contact : m_contacts) if (contact.binding == binding) contact.binding.clear();
    m_contacts[peer].binding = binding;
    if (!saveSettings()) { m_contacts = previous; return false; }
    emit changed(); return true;
}
void RelayPeers::setAiAccess(const QString &peer, bool allowed) {
    if (!m_contacts.contains(peer) || !m_contacts[peer].active) return;
    const auto previous = m_contacts[peer].ai;
    m_contacts[peer].ai = allowed;
    if (!saveSettings()) m_contacts[peer].ai = previous;
    emit changed();
}
bool RelayPeers::removeContact(const QString &peer) {
    if (!m_contacts.contains(peer)) return false;
    const auto previous = m_contacts.take(peer);
    if (!saveSettings()) { m_contacts.insert(peer, previous); return false; }
    closeTransport(peer);
    if (m_selected == peer) m_selected.clear();
    emit changed(); return true;
}
bool RelayPeers::clearHistory(const QString &peer) {
    if (!loadHistory(peer)) return false;
    const auto previous = m_contacts[peer].history;
    m_contacts[peer].history = {};
    if (!saveHistory(peer)) { m_contacts[peer].history = previous; return false; }
    emit changed(); return true;
}
void RelayPeers::setRendezvousUrl(const QString &value) {
    const QUrl url(value.trimmed());
    if (m_enabled || !m_initialized) return;
    if (url.scheme() != "wss" || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
        fail("The setup service requires a secure wss:// address."); return;
    }
    const auto previous = m_url;
    m_url = url.toString();
    if (!saveSettings()) m_url = previous;
    emit changed();
}
void RelayPeers::setEnabled(bool value) {
    if (value && (!m_initialized || !QUrl(m_url).isValid())) return;
    m_enabled = value;
    if (value) {
        m_status = needsSetupService() ? "Connecting to setup service" : "Ready for direct Relay chat"; m_error.clear();
        if (needsSetupService()) m_signal.open(QUrl(m_url));
        m_poll.start();
    } else {
        m_poll.stop(); m_ready = false; m_signal.close();
        for (const auto &peer : m_transports.keys()) closeTransport(peer);
        m_status = "Relay messaging is off";
    }
    emit changed();
}
bool RelayPeers::needsSetupService() const {
    for (const auto &contact : m_contacts) if (contact.active && !contact.external) return true;
    return !m_externalOnly;
}
void RelayPeers::setContactActive(const QString &peer, bool active) {
    if (!m_contacts.contains(peer)) return;
    auto &contact = m_contacts[peer]; const auto previous = contact;
    contact.active = active;
    if (!active) contact.ai = false;
    if (!saveSettings()) { contact = previous; return; }
    if (!active) closeTransport(peer);
    if (!needsSetupService()) m_signal.close();
    emit changed();
}
void RelayPeers::startExternalConnection(const QString &peer) {
    if (!m_enabled || !m_contacts.contains(peer) || !m_contacts[peer].active || !m_contacts[peer].external || identity() >= peer) return;
    if (m_transports.contains(peer) && (m_transports[peer]->connected() || QDateTime::currentMSecsSinceEpoch() - m_lastAttempts.value(peer) < 30000)) return;
    try { makeTransport(peer, QUuid::createUuid().toString(QUuid::WithoutBraces))->offer(); }
    catch (...) { closeTransport(peer); fail("Could not start a direct WebRTC session."); }
}
void RelayPeers::pollPresence() {
    if (!m_ready) return;
    QJsonArray peers;
    for (const auto &peer : m_contacts.keys()) if (m_contacts[peer].active && !m_contacts[peer].external) peers.append(peer);
    m_signal.sendTextMessage(QString::fromUtf8(json({{"lookup", peers}})));
}
void RelayPeers::closeTransport(const QString &peer) {
    if (auto *transport = m_transports.take(peer)) { transport->disconnect(this); transport->deleteLater(); }
    emit streamDisconnected(peer);
    m_sessions.remove(peer); m_inFlight.remove(peer);
    if (m_contacts.contains(peer)) m_contacts[peer].state = "Offline";
}
RelayPeerTransport *RelayPeers::makeTransport(const QString &peer, const QString &session) {
    closeTransport(peer);
    auto *transport = new RelayPeerTransport(m_stun, this);
    m_transports.insert(peer, transport); m_sessions.insert(peer, session);
    m_lastAttempts[peer] = QDateTime::currentMSecsSinceEpoch();
    m_contacts[peer].state = "Finding a direct route…";
    connect(transport, &RelayPeerTransport::streamReady, this, [this, peer] { emit streamReady(peer); });
    connect(transport, &RelayPeerTransport::streamControlReceived, this, [this, peer](const QJsonObject &frame) { if (streamConnected(peer)) emit streamControlReceived(peer, frame); });
    connect(transport, &RelayPeerTransport::streamPacketReceived, this, [this, peer](const QByteArray &packet) { if (streamConnected(peer)) emit streamPacketReceived(peer, packet); });
    connect(transport, &RelayPeerTransport::descriptionReady, this, [this, peer, session](const QString &sdp, const QString &type) { sendDescription(peer, session, sdp, type); });
    connect(transport, &RelayPeerTransport::frameReceived, this, [this, peer](const QJsonObject &frame) { processFrame(peer, frame); });
    connect(transport, &RelayPeerTransport::stateChanged, this, [this, peer](const QString &state) { m_contacts[peer].state = state; if (!streamConnected(peer)) emit streamDisconnected(peer); emit changed(); flush(peer); });
    emit changed();
    return transport;
}
void RelayPeers::reconnect(const QString &peer) {
    if (!m_enabled || !m_contacts.contains(peer) || !m_contacts[peer].active) return;
    closeTransport(peer); m_lastAttempts.remove(peer);
    if (m_contacts[peer].external) emit externalReconnect(peer); else if (m_ready) pollPresence();
}
void RelayPeers::sendDescription(const QString &peer, const QString &session, const QString &sdp, const QString &type) {
    if (!m_enabled || sdp.toUtf8().size() > 48000 || !m_contacts.contains(peer) || !m_contacts[peer].active ||
        (!m_contacts[peer].external && !m_ready) || m_sessions.value(peer) != session) return;
    unsigned char remoteCurve[32], localCurve[32];
    const auto publicKey = QByteArray::fromHex(peer.toLatin1());
    if (crypto_sign_ed25519_pk_to_curve25519(remoteCurve, reinterpret_cast<const unsigned char *>(publicKey.constData())) != 0 ||
        crypto_sign_ed25519_sk_to_curve25519(localCurve, reinterpret_cast<const unsigned char *>(m_secret.constData())) != 0) return;
    const auto plain = json({{"v", 1}, {"session", session}, {"type", type}, {"sdp", sdp}, {"time", double(QDateTime::currentSecsSinceEpoch())}, {"nonce", QUuid::createUuid().toString(QUuid::WithoutBraces)}});
    QByteArray payload(crypto_box_NONCEBYTES + crypto_box_MACBYTES + plain.size(), Qt::Uninitialized);
    randombytes_buf(payload.data(), crypto_box_NONCEBYTES);
    const auto result = crypto_box_easy(reinterpret_cast<unsigned char *>(payload.data() + crypto_box_NONCEBYTES),
        reinterpret_cast<const unsigned char *>(plain.constData()), size_t(plain.size()),
        reinterpret_cast<const unsigned char *>(payload.constData()), remoteCurve, localCurve);
    sodium_memzero(localCurve, sizeof localCurve);
    if (result == 0) {
        const auto encoded = QString::fromLatin1(payload.toBase64());
        if (m_contacts[peer].external) emit externalSignal(peer, encoded);
        else m_signal.sendTextMessage(QString::fromUtf8(json({{"to", peer}, {"payload", encoded}})));
    }
}
void RelayPeers::signalReceived(const QJsonObject &message, bool external) {
    if (!m_enabled) return;
    if (!external && message.contains("challenge")) {
        const auto challenge = QByteArray::fromBase64(message.value("challenge").toString().toLatin1());
        if (challenge.size() != 32) { m_signal.close(); return; }
        const auto data = QByteArray("relay-rendezvous-v1:") + challenge;
        QByteArray signature(crypto_sign_BYTES, Qt::Uninitialized);
        crypto_sign_detached(reinterpret_cast<unsigned char *>(signature.data()), nullptr,
            reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size()), reinterpret_cast<const unsigned char *>(m_secret.constData()));
        m_signal.sendTextMessage(QString::fromUtf8(json({{"key", identity()}, {"signature", QString::fromLatin1(signature.toBase64())}})));
        return;
    }
    if (!external && message.value("ready").toBool()) { m_ready = true; m_status = "Ready · setup service carries no chat messages"; emit changed(); pollPresence(); return; }
    if (!external && !m_ready) return;
    if (!external && message.value("online").isArray()) {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        for (const auto &value : message.value("online").toArray()) {
            const auto peer = value.toString();
            if (!m_contacts.contains(peer) || !m_contacts[peer].active || m_contacts[peer].external || identity() >= peer || (m_transports.contains(peer) && m_transports[peer]->connected()) || now - m_lastAttempts.value(peer) < 45000) continue;
            try { makeTransport(peer, QUuid::createUuid().toString(QUuid::WithoutBraces))->offer(); }
            catch (...) { closeTransport(peer); fail("Could not start a direct WebRTC session."); }
        }
        return;
    }
    const auto peer = message.value("from").toString();
    if (!m_contacts.contains(peer) || !m_contacts[peer].active || m_contacts[peer].external != external) return;
    const auto cipher = QByteArray::fromBase64(message.value("payload").toString().toLatin1());
    if (cipher.size() < crypto_box_NONCEBYTES + crypto_box_MACBYTES || cipher.size() > 65536) return;
    unsigned char remoteCurve[32], localCurve[32];
    const auto publicKey = QByteArray::fromHex(peer.toLatin1());
    if (crypto_sign_ed25519_pk_to_curve25519(remoteCurve, reinterpret_cast<const unsigned char *>(publicKey.constData())) != 0 ||
        crypto_sign_ed25519_sk_to_curve25519(localCurve, reinterpret_cast<const unsigned char *>(m_secret.constData())) != 0) return;
    QByteArray plain(cipher.size() - crypto_box_NONCEBYTES - crypto_box_MACBYTES, Qt::Uninitialized);
    const auto result = crypto_box_open_easy(reinterpret_cast<unsigned char *>(plain.data()),
        reinterpret_cast<const unsigned char *>(cipher.constData() + crypto_box_NONCEBYTES), size_t(cipher.size() - crypto_box_NONCEBYTES),
        reinterpret_cast<const unsigned char *>(cipher.constData()), remoteCurve, localCurve);
    sodium_memzero(localCurve, sizeof localCurve);
    if (result != 0) return;
    const auto signal = QJsonDocument::fromJson(plain).object();
    const auto type = signal.value("type").toString(), session = signal.value("session").toString(), nonce = signal.value("nonce").toString();
    const auto signalTime = signal.value("time").toDouble(-1);
    if (signal.value("v").toInt() != 1 || !IdPattern.match(session).hasMatch() || !IdPattern.match(nonce).hasMatch() ||
        !std::isfinite(signalTime) || std::abs(double(QDateTime::currentSecsSinceEpoch()) - signalTime) > 90 || m_seenSignals.contains(nonce)) return;
    // One offerer per identity pair eliminates glare. Answers must match this attempt.
    if ((type == "offer" && peer >= identity()) || (type == "answer" && (!m_transports.contains(peer) || m_sessions.value(peer) != session)) ||
        (type != "offer" && type != "answer")) return;
    const auto now = QDateTime::currentSecsSinceEpoch();
    for (auto it = m_seenSignals.begin(); it != m_seenSignals.end();) {
        if (now - it.value() > 180) it = m_seenSignals.erase(it); else ++it;
    }
    if (m_seenSignals.size() >= 4096) return;
    m_seenSignals.insert(nonce, now);
    try {
        auto *transport = type == "offer" ? makeTransport(peer, session) : m_transports.value(peer);
        transport->acceptDescription(signal.value("sdp").toString(), type);
    } catch (...) { closeTransport(peer); fail("The peer's connection setup was invalid."); }
}
bool RelayPeers::queueText(const QString &peer, const QString &text, QString *outputId) {
    if (!m_initialized || !m_contacts.contains(peer) || !m_contacts[peer].active || !validText(text)) return fail("Relay accepts text only, up to 4,000 UTF-8 bytes.");
    if (!loadHistory(peer)) return false;
    auto &history = m_contacts[peer].history;
    if (history.size() >= MaxHistory) return fail("Conversation storage is full. Clear its local history before sending more.");
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    history.append(QJsonObject{{"id", id}, {"text", text}, {"own", true}, {"pending", true}, {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    if (!saveHistory(peer)) { history.removeLast(); return false; }
    if (outputId) *outputId = id;
    m_error.clear(); emit changed(); flush(peer); return true;
}
bool RelayPeers::sendText(const QString &peer, const QString &text) { return queueText(peer, text); }
void RelayPeers::flush(const QString &peer) {
    auto *transport = m_transports.value(peer);
    if (!transport || !transport->connected() || !loadHistory(peer)) return;
    int sent = 0;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto &value : m_contacts[peer].history) {
        const auto row = value.toObject();
        if (!row.value("own").toBool() || !row.value("pending").toBool()) continue;
        const auto id = row.value("id").toString();
        if (now - m_inFlight[peer].value(id) < 15000) continue;
        if (++sent > 32 || !transport->send({{"v", 1}, {"type", "text"}, {"id", id}, {"text", row.value("text")}})) break;
        m_inFlight[peer].insert(id, now);
    }
}
void RelayPeers::processFrame(const QString &peer, const QJsonObject &frame) {
    if (!m_enabled || !m_contacts.contains(peer) || !m_contacts[peer].active || !loadHistory(peer)) return;
    auto *transport = m_transports.value(peer);
    if (!transport || !transport->connected()) return;
    const auto type = frame.value("type").toString(), id = frame.value("id").toString();
    const auto expectedKeys = type == "text" ? QStringList{"id", "text", "type", "v"} : QStringList{"id", "type", "v"};
    if (frame.keys() != expectedKeys || frame.value("v").toInt() != 1 || !IdPattern.match(id).hasMatch() || (type != "text" && type != "ack")) {
        closeTransport(peer); fail("Relay rejected a non-text or invalid protocol frame."); return;
    }
    auto &history = m_contacts[peer].history;
    for (int i = 0; i < history.size(); ++i) {
        auto row = history[i].toObject();
        if (row.value("id") != id) continue;
        if (type == "ack" && row.value("own").toBool() && row.value("pending").toBool()) {
            const auto old = row;
            row.insert("pending", false); history[i] = row;
            if (!saveHistory(peer)) history[i] = old;
            else m_inFlight[peer].remove(id);
            emit changed();
            QTimer::singleShot(0, this, [this, peer] { flush(peer); });
        } else if (type == "text" && !row.value("own").toBool() && row.value("text") == frame.value("text")) {
            transport->send({{"v", 1}, {"type", "ack"}, {"id", id}});
        }
        return;
    }
    if (type != "text") return;
    if (!frame.value("text").isString() || !validText(frame.value("text").toString()) || history.size() >= MaxHistory) {
        fail("An incoming message was rejected: invalid text or conversation storage full."); return;
    }
    history.append(QJsonObject{{"id", id}, {"text", frame.value("text")}, {"own", false}, {"pending", false}, {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
    if (!saveHistory(peer)) { history.removeLast(); return; }
    // Acknowledgement means saved by the peer, never a claim of human/AI reading.
    transport->send({{"v", 1}, {"type", "ack"}, {"id", id}});
    emit changed();
}

QVariantList RelayPeers::messagesForPeer(const QString &peer) {
    QVariantList rows;
    if (!loadHistory(peer)) return rows;
    const auto &history = m_contacts[peer].history;
    for (int i = qMax(0, int(history.size()) - 500); i < history.size(); ++i) rows.append(history[i].toObject().toVariantMap());
    return rows;
}

bool RelayPeers::streamConnected(const QString &peer) const {
    auto *transport = m_transports.value(peer);
    return m_enabled && m_contacts.contains(peer) && m_contacts[peer].active && transport && transport->streamConnected();
}
bool RelayPeers::sendStreamControl(const QString &peer, const QJsonObject &frame) { return streamConnected(peer) && m_transports[peer]->sendStreamControl(frame); }
bool RelayPeers::sendStreamPacket(const QString &peer, const QByteArray &packet) { return streamConnected(peer) && m_transports[peer]->sendStreamPacket(packet); }
qsizetype RelayPeers::streamBufferedAmount(const QString &peer) const { return streamConnected(peer) ? m_transports[peer]->streamBufferedAmount() : 0; }
