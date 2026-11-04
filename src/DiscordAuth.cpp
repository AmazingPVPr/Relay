#include "DiscordAuth.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QRegularExpression>
#include <QUrl>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QSettings>
#include <QFileInfo>
#include <QDir>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <QDateTime>
#include <QUrlQuery>
#include <qrencode.h>
#include <algorithm>

namespace {
constexpr int MaximumMessage = 64 * 1024;
const QByteArray userAgent("Relay/0.1 (native Qt client)");
bool validFingerprint(const QByteArray &fingerprint) {
    static const QRegularExpression pattern("^[A-Za-z0-9_-]{43}$");
    return pattern.match(QString::fromLatin1(fingerprint)).hasMatch();
}
}

DiscordAuth::DiscordAuth(QObject *parent) : QObject(parent), m_network(this) {
    m_deadline.setSingleShot(true);
    m_countdown.setInterval(1000);
    connect(&m_heartbeat, &QTimer::timeout, this, &DiscordAuth::heartbeat);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        fail("This sign-in code expired. Create a new code and scan it again.");
    });
    connect(&m_countdown, &QTimer::timeout, this, [this] {
        const int seconds = m_elapsed.isValid() ? int(std::max<qint64>(0, m_timeoutMs - m_elapsed.elapsed() + 999) / 1000) : 0;
        if (seconds != m_expiresIn) { m_expiresIn = seconds; emit changed(); }
    });
}
DiscordAuth::~DiscordAuth() { stopTransport(); }

void DiscordAuth::stopTransport() {
    ++m_generation;
    if (m_bwProcess) {
        auto *process = m_bwProcess.data(); m_bwProcess = nullptr;
        disconnect(process, nullptr, this, nullptr);
        process->setParent(nullptr);
        if (process->state() == QProcess::NotRunning) process->deleteLater();
        else {
            connect(process, &QProcess::finished, process, &QObject::deleteLater);
            process->kill();
        }
    }
    AuthCrypto::erase(m_rememberedLogin);
    m_savedTotp.fill(QChar(0)); m_savedTotp.clear();
    m_savedTotpAttempted = false;
    m_mfaTicket.clear();
    m_bwItem.clear();
    AuthCrypto::erase(m_bwSession);
    m_bwTotpAttempted = false;
    m_bwConnecting = false;
    m_closing = true;
    m_heartbeat.stop();
    m_deadline.stop();
    m_countdown.stop();
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if (m_socket) {
        disconnect(m_socket.get(), nullptr, this, nullptr);
        m_socket->abort();
        m_socket.release()->deleteLater();
    }
    m_crypto.reset();
    AuthCrypto::erase(m_fingerprint);
    m_qrImage.clear();
    m_qrReady = false;
    m_waitingHeartbeat = false;
    m_expiresIn = 0;
    m_active = false;
    m_stage = Stage::Idle;
    m_closing = false;
}

void DiscordAuth::cancel() {
    stopTransport();
    m_completed = false;
    m_scannedUserName.clear();
    m_error.clear();
    m_status = "Sign-in canceled";
    emit changed();
}
void DiscordAuth::retry() { start(); }
void DiscordAuth::fail(const QString &reason) {
    stopTransport();
    m_completed = false;
    m_error = reason;
    m_status = "Sign-in needs attention";
    emit changed();
}

void DiscordAuth::start() {
    stopTransport();
    m_completed = false;
    m_error.clear();
    m_scannedUserName.clear();
    m_active = true;
    m_status = "Preparing a secure sign-in code…";
    emit changed();
    const auto generation = m_generation;
    QTimer::singleShot(0, this, [this, generation] {
        if (generation != m_generation || !m_active) return;
        m_crypto = std::make_unique<AuthCrypto>();
        if (!m_crypto->generate()) { fail("Could not prepare the encryption key. Try signing in again."); return; }
        m_socket = std::make_unique<QWebSocket>(QStringLiteral("https://discord.com"), QWebSocketProtocol::VersionLatest, this);
        m_socket->setMaxAllowedIncomingMessageSize(MaximumMessage);
        m_socket->setMaxAllowedIncomingFrameSize(MaximumMessage);
        connect(m_socket.get(), &QWebSocket::textMessageReceived, this, [this, generation](const QString &message) {
            if (generation == m_generation) onText(message);
        });
        connect(m_socket.get(), &QWebSocket::errorOccurred, this, [this, generation](QAbstractSocket::SocketError) {
            if (generation == m_generation && m_active && !m_closing && m_stage != Stage::Exchange)
                fail("Could not reach Discord's sign-in service. Check your connection, then try again.");
        });
        connect(m_socket.get(), &QWebSocket::disconnected, this, [this, generation] {
            if (generation != m_generation || !m_active || m_closing || m_stage == Stage::Exchange) return;
            const auto code = int(m_socket->closeCode());
            fail(code == 4003 ? QStringLiteral("This sign-in code expired. Create a new code.") :
                 QStringLiteral("Discord closed the sign-in session (code %1). Create a new code and try again.").arg(code));
        });
        m_stage = Stage::Hello;
        m_status = "Connecting to Discord sign-in…";
        m_deadline.start(20000);
        emit changed();
        QNetworkRequest request{QUrl(m_gatewayUrl)};
        request.setRawHeader("User-Agent", userAgent);
        m_socket->open(request);
    });
}

void DiscordAuth::send(const QJsonObject &payload) {
    if (m_socket && m_socket->state() == QAbstractSocket::ConnectedState)
        m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
}
void DiscordAuth::heartbeat() {
    if (!m_active || m_stage == Stage::Exchange) return;
    if (m_waitingHeartbeat) { fail("Discord sign-in stopped responding. Create a new code."); return; }
    m_waitingHeartbeat = true;
    send({{"op", "heartbeat"}});
}

QString DiscordAuth::renderQr(const QByteArray &url) {
    QRcode *code = QRcode_encodeString(url.constData(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!code || code->width < 1 || code->width > 177) { QRcode_free(code); return {}; }
    constexpr int quietZone = 4;
    constexpr int scale = 6;
    const int size = (code->width + quietZone * 2) * scale;
    QImage image(size, size, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int y = 0; y < code->width; ++y)
        for (int x = 0; x < code->width; ++x)
            if (code->data[y * code->width + x] & 1)
                painter.drawRect((x + quietZone) * scale, (y + quietZone) * scale, scale, scale);
    painter.end();
    QRcode_free(code);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

void DiscordAuth::onText(const QString &message) {
    if (!m_active || !m_crypto || message.size() > MaximumMessage) return;
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(message.toUtf8(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        fail("Discord sent an unreadable sign-in response. Try a new code."); return;
    }
    const auto payload = document.object();
    const auto op = payload.value("op").toString();
    if (op == "heartbeat_ack") { m_waitingHeartbeat = false; return; }
    if (op == "cancel") { fail("Sign-in was canceled on your phone. Create a new code when you're ready."); return; }
    if (op == "hello") {
        if (m_stage != Stage::Hello) { fail("Discord restarted the sign-in handshake. Try a new code."); return; }
        const int interval = payload.value("heartbeat_interval").toInt();
        const int timeout = payload.value("timeout_ms").toInt();
        if (interval < 1000 || interval > 300000 || timeout < 1000 || timeout > 3600000) {
            fail(QStringLiteral("Discord returned unsupported sign-in timing (heartbeat %1 ms, expiry %2 ms). Try again.").arg(interval).arg(timeout)); return;
        }
        // The service's lease length varies. Keep our QR valid for at most three
        // minutes even if Discord grants a longer lease, and honor shorter leases.
        m_timeoutMs = std::min(timeout, 180000);
        m_elapsed.start();
        m_expiresIn = (m_timeoutMs + 999) / 1000;
        m_deadline.start(m_timeoutMs);
        m_countdown.start();
        m_heartbeat.start(interval);
        m_stage = Stage::Nonce;
        send({{"op", "init"}, {"encoded_public_key", QString::fromLatin1(m_crypto->publicKeyBase64())}});
        emit changed();
    } else if (op == "nonce_proof") {
        if (m_stage != Stage::Nonce) { fail("Discord returned an unexpected sign-in challenge."); return; }
        auto nonce = m_crypto->decrypt(payload.value("encrypted_nonce").toString().toLatin1());
        if (nonce.isEmpty()) { fail("Could not verify Discord's encrypted sign-in challenge."); return; }
        // Current Remote Auth v2 sends the decrypted OAEP nonce. Older clients
        // used a SHA-256 `proof` field; current servers expect `nonce` instead.
        send({{"op", "nonce_proof"}, {"nonce", QString::fromLatin1(AuthCrypto::urlBase64(nonce))}});
        AuthCrypto::erase(nonce);
        m_stage = Stage::Fingerprint;
    } else if (op == "pending_remote_init") {
        if (m_stage != Stage::Fingerprint) { fail("Discord returned an unexpected sign-in code."); return; }
        const auto fingerprint = payload.value("fingerprint").toString().toLatin1();
        const auto expected = m_crypto->fingerprint();
        if (!validFingerprint(fingerprint) || fingerprint.size() != expected.size() ||
            CRYPTO_memcmp(fingerprint.constData(), expected.constData(), expected.size()) != 0) {
            fail("The sign-in code did not match this app's encryption key. Create a new code."); return;
        }
        m_fingerprint = fingerprint;
        m_qrImage = renderQr("https://discord.com/ra/" + fingerprint);
        if (m_qrImage.isEmpty()) { fail("Could not draw the sign-in code. Try again."); return; }
        m_qrReady = true;
        m_stage = Stage::Scan;
        m_status = "Scan this code with the Discord app on your phone";
        emit changed();
    } else if (op == "pending_ticket") {
        if (m_stage != Stage::Scan) { fail("Discord returned an unexpected account confirmation."); return; }
        auto user = m_crypto->decrypt(payload.value("encrypted_user_payload").toString().toLatin1());
        const auto parts = user.split(':');
        const bool valid = parts.size() == 4 && QRegularExpression("^[0-9]{1,22}$").match(QString::fromLatin1(parts.value(0))).hasMatch();
        if (!valid) { AuthCrypto::erase(user); fail("Could not read the account confirmation. Try a new code."); return; }
        m_scannedUserName = QString::fromUtf8(parts.at(3)).left(128);
        m_scannedUserName.remove(QRegularExpression("[\\x00-\\x1F\\x7F]"));
        AuthCrypto::erase(user);
        m_qrReady = false;
        m_qrImage.clear();
        m_stage = Stage::Confirmation;
        m_status = "Confirm the sign-in request on your phone";
        emit changed();
    } else if (op == "pending_login") {
        if (m_stage != Stage::Confirmation) { fail("Discord returned an unexpected sign-in ticket. Try a new code."); return; }
        const auto ticket = payload.value("ticket").toString();
        if (ticket.size() < 10 || ticket.size() > 2048 || ticket.contains(QRegularExpression("[\\x00-\\x20\\x7F]"))) {
            fail("Discord returned an invalid sign-in ticket."); return;
        }
        m_stage = Stage::Exchange;
        m_qrReady = false;
        m_qrImage.clear();
        m_heartbeat.stop();
        m_status = "Completing sign-in…";
        emit changed();
        exchangeTicket(ticket);
    }
}

void DiscordAuth::exchangeTicket(const QString &ticket) {
    const auto generation = m_generation;
    QNetworkRequest request{QUrl(m_loginUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", userAgent);
    request.setRawHeader("Origin", "https://discord.com");
    request.setRawHeader("Referer", "https://discord.com/login");
    request.setRawHeader("X-Fingerprint", m_fingerprint);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(20000);
    auto *reply = m_network.post(request, QJsonDocument(QJsonObject{{"ticket", ticket}}).toJson(QJsonDocument::Compact));
    m_reply = reply;
    reply->setReadBufferSize(MaximumMessage + 1);
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
        if (received > MaximumMessage || total > MaximumMessage) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        if (generation != m_generation || m_stage != Stage::Exchange || !m_crypto) return;
        m_reply = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto bytes = reply->read(MaximumMessage + 1);
        if (bytes.size() > MaximumMessage) { fail("Discord returned an oversized sign-in response."); return; }
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(bytes, &parse);
        const auto response = document.object();
        if (response.contains("captcha_key") || response.contains("captcha_sitekey")) {
            fail("Discord requested a verification challenge this native login cannot display. Resolve account checks in the official app or browser, then try signing in again.");
            return;
        }
        if (status == 429) { fail("Discord asked you to wait before signing in again. Wait a moment, then create a new code."); return; }
        if (status < 200 || status >= 300 || reply->error() != QNetworkReply::NoError ||
            parse.error != QJsonParseError::NoError || !document.isObject()) {
            fail("Discord could not finish this sign-in. Create a new code and confirm it on your phone again."); return;
        }
        auto tokenBytes = m_crypto->decrypt(response.value("encrypted_token").toString().toLatin1());
        if (tokenBytes.size() < 20 || tokenBytes.size() > 512 ||
            tokenBytes.contains('\0') || QString::fromUtf8(tokenBytes).contains(QRegularExpression("\\s"))) {
            AuthCrypto::erase(tokenBytes);
            fail("Could not decrypt Discord's sign-in confirmation. Try a new code."); return;
        }
        QString token = QString::fromUtf8(tokenBytes);
        AuthCrypto::erase(tokenBytes);
        stopTransport();
        m_stage = Stage::Complete;
        m_completed = true;
        m_status = "Signed in to Discord";
        m_error.clear();
        emit changed();
        emit authenticated(token);
        token.fill(QChar(0));
        token.clear();
    });
}


bool DiscordAuth::rememberLogin() const { return QSettings().value("signIn/automaticLogin", false).toBool(); }
void DiscordAuth::setRememberLogin(bool enabled) {
    QSettings().setValue("signIn/automaticLogin", enabled);
    if (!enabled) QSettings().remove("signIn/automaticAccount");
    emit rememberLoginChanged();
}

QString DiscordAuth::totpCode(const QString &input, qint64 seconds) {
    QString secret = input.trimmed();
    QString algorithm = "SHA1"; int digits = 6; int period = 30;
    if (secret.startsWith("otpauth://")) {
        QUrl url(secret); QUrlQuery query(url);
        if (url.host() != "totp") return {};
        secret = query.queryItemValue("secret");
        if (query.hasQueryItem("algorithm")) algorithm = query.queryItemValue("algorithm").toUpper();
        if (query.hasQueryItem("digits")) digits = query.queryItemValue("digits").toInt();
        if (query.hasQueryItem("period")) period = query.queryItemValue("period").toInt();
    }
    if ((digits != 6 && digits != 8) || period < 1 || period > 300 || seconds < 0) return {};
    const EVP_MD *digest = algorithm == "SHA1" ? EVP_sha1() : algorithm == "SHA256" ? EVP_sha256() : algorithm == "SHA512" ? EVP_sha512() : nullptr;
    if (!digest) return {};
    QByteArray key; quint32 buffer = 0; int bits = 0;
    for (auto c : secret.toUpper()) {
        if (c.isSpace() || c == '=') continue;
        int value = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ234567").indexOf(c);
        if (value < 0) { AuthCrypto::erase(key); return {}; }
        buffer = (buffer << 5) | quint32(value); bits += 5;
        if (bits >= 8) { bits -= 8; key.append(char((buffer >> bits) & 255)); }
    }
    secret.fill(QChar(0));
    if (key.isEmpty()) return {};
    quint64 counter = quint64(seconds / period); unsigned char message[8];
    for (int i = 7; i >= 0; --i) { message[i] = counter & 255; counter >>= 8; }
    unsigned char hash[EVP_MAX_MD_SIZE]; unsigned int size = 0;
    auto *result = HMAC(digest, key.constData(), int(key.size()), message, 8, hash, &size);
    AuthCrypto::erase(key);
    if (!result || size < 20) return {};
    const auto offset = hash[size - 1] & 15;
    const quint32 value = ((quint32(hash[offset]) & 127) << 24) | (quint32(hash[offset+1]) << 16) | (quint32(hash[offset+2]) << 8) | hash[offset+3];
    OPENSSL_cleanse(hash, sizeof(hash));
    return QString::number(value % (digits == 8 ? 100000000 : 1000000)).rightJustified(digits, '0');
}

void DiscordAuth::signInSavedCredential(const QString &encoded) {
    cancel();
    auto bytes = QByteArray::fromBase64(encoded.toLatin1());
    const auto login = QJsonDocument::fromJson(bytes).object();
    AuthCrypto::erase(bytes);
    const auto email = login.value("username").toString();
    auto password = login.value("password").toString();
    if (email.trimmed().isEmpty() || password.isEmpty()) { fail("The remembered login is incomplete. Reconnect your Discord entry."); return; }
    m_savedTotp = login.value("totp").toString();
    m_rememberedLogin = encoded.toLatin1();
    m_deadline.start(180000);
    passwordRequest("/auth/login", {{"login", email}, {"password", password}, {"undelete", false}});
    password.fill(QChar(0));
}

void DiscordAuth::signInPassword(const QString &email, const QString &password) {
    cancel();
    if (email.trimmed().isEmpty() || password.isEmpty()) { fail("Enter your email and password."); return; }
    m_deadline.start(180000);
    passwordRequest("/auth/login", {{"login", email.trimmed()}, {"password", password}, {"undelete", false}});
}

void DiscordAuth::submitCode(const QString &code, bool backup) {
    if (m_mfaTicket.isEmpty() || m_reply || m_bwProcess) return;
    if (code.trimmed().isEmpty()) { m_error = "Enter your authenticator or backup code."; emit changed(); return; }
    passwordRequest("/auth/mfa/totp", {{"ticket", m_mfaTicket}, {"code", code.trimmed()}, {"backup", backup}});
}

void DiscordAuth::passwordRequest(const QString &path, const QJsonObject &body) {
    m_active = true; m_completed = false; m_error.clear();
    m_status = "Signing in to Discord…"; emit changed();
    QNetworkRequest request{QUrl(m_apiBase + path)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("User-Agent", userAgent);
    request.setRawHeader("Origin", "https://discord.com");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(20000);
    auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto *reply = m_network.post(request, bytes);
    AuthCrypto::erase(bytes);
    m_reply = reply;
    const auto generation = m_generation;
    reply->setReadBufferSize(MaximumMessage + 1);
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
        if (received > MaximumMessage || total > MaximumMessage) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        if (generation != m_generation) return;
        m_reply = nullptr;
        auto data = reply->read(MaximumMessage + 1);
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(data, &parse);
        AuthCrypto::erase(data);
        const auto response = doc.object();
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (response.contains("captcha_key") || response.contains("captcha_sitekey")) {
            fail("Discord requires a verification challenge. Use QR sign-in, or complete account checks in the official Discord app."); return;
        }
        if (status == 429) { fail("Discord asked you to wait. Try signing in again later."); return; }
        if (status < 200 || status >= 300 || reply->error() != QNetworkReply::NoError || parse.error != QJsonParseError::NoError || !doc.isObject()) {
            m_active = false;
            m_error = m_mfaTicket.isEmpty() ? "Sign-in failed. Check your credentials and any account verification emails, or use QR sign-in." : "Code was not accepted. Try a fresh authenticator code or a backup code.";
            if (m_mfaTicket.isEmpty()) { fail(m_error); return; }
            m_status = "Enter your two-factor code"; emit changed(); return;
        }
        auto token = response.value("token").toString();
        if (token.size() >= 20 && token.size() <= 8192 && !token.contains(QRegularExpression("[\\s\\x00-\\x1f\\x7f]"))) {
            if (!m_rememberedLogin.isEmpty()) emit rememberedLoginReady(QString::fromLatin1(m_rememberedLogin));
            stopTransport(); m_completed = true; m_status = "Signed in to Discord"; m_error.clear();
            emit changed(); emit authenticated(token); token.fill(QChar(0)); return;
        }
        if (response.value("mfa").toBool() && !response.value("ticket").toString().isEmpty()) {
            m_mfaTicket = response.value("ticket").toString(); m_active = false;
            m_status = "Enter your authenticator or backup code"; emit changed();
            if (!m_savedTotp.isEmpty() && !m_savedTotpAttempted) {
                m_savedTotpAttempted = true;
                const auto code = totpCode(m_savedTotp, QDateTime::currentSecsSinceEpoch());
                if (!code.isEmpty()) submitCode(code);
            } else if (!m_bwSession.isEmpty() && !m_bwTotpAttempted) fetchBitwardenCode();
            return;
        }
        fail("Discord requires another sign-in method. Use QR sign-in or the official Discord app.");
    });
}

void DiscordAuth::runBitwarden(const QStringList &arguments, const QString &password, std::function<void(QByteArray)> done) {
    auto program = m_bwProgram.isEmpty() ? QStandardPaths::findExecutable("bw") : m_bwProgram;
    if (program.isEmpty()) {
        const auto userCli = QDir::homePath() + "/.local/bin/bw";
        if (QFileInfo(userCli).isExecutable()) program = userCli;
    }
    if (program.isEmpty()) { fail("Install the official Bitwarden CLI (bw), then sign in to it before connecting Relay."); return; }
    auto *process = new QProcess(this); m_bwProcess = process;
    auto environment = QProcessEnvironment::systemEnvironment();
    // Relay owns a separate encrypted CLI profile; never reconfigure another client.
    const auto profile = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/bitwarden";
    QDir().mkpath(profile);
    environment.insert("BITWARDENCLI_APPDATA_DIR", profile);
    environment.remove("BW_SESSION");
    if (!environment.contains("NODE_EXTRA_CA_CERTS")) {
        for (const auto &bundle : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt"})
            if (QFileInfo::exists(QString::fromLatin1(bundle))) { environment.insert("NODE_EXTRA_CA_CERTS", QString::fromLatin1(bundle)); break; }
    }
    // A session key or master password is never passed in process arguments or persisted.
    if (!m_bwSession.isEmpty()) environment.insert("BW_SESSION", QString::fromUtf8(m_bwSession));
    if (!password.isEmpty()) environment.insert("RELAY_BW_PASSWORD", password);
    process->setProcessEnvironment(environment);
    const auto generation = m_generation;
    auto *timeout = new QTimer(process); timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, this, [this, generation] { if (generation == m_generation) fail("Bitwarden timed out. Unlock your vault and try again."); });
    connect(process, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError error) {
        if (generation == m_generation && error == QProcess::FailedToStart) fail("Could not start Bitwarden CLI.");
    });
    connect(process, &QProcess::readyReadStandardError, process, [process] {
        auto error = process->readAllStandardError().toLower();
        // Store only a category; never retain or display arbitrary CLI output.
        if (error.contains("certificate") || error.contains("local issuer")) process->setProperty("tlsError", true);
        if (error.contains("two-step") || error.contains("two factor") || error.contains("two-factor")) process->setProperty("twoFactorError", true);
        AuthCrypto::erase(error);
    });
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, generation] {
        if (generation == m_generation && process->bytesAvailable() > 65536) fail("Bitwarden returned an oversized response.");
    });
    connect(process, &QProcess::finished, this, [this, process, generation, done = std::move(done)](int code, QProcess::ExitStatus exit) mutable {
        process->deleteLater();
        if (generation != m_generation) return;
        m_bwProcess = nullptr;
        auto value = process->readAllStandardOutput();
        if (code != 0 || exit != QProcess::NormalExit || value.size() > 65536) {
            AuthCrypto::erase(value);
            if (!m_mfaTicket.isEmpty()) { m_active = false; m_error = "Bitwarden could not supply a code. Enter your two-factor code below."; emit changed(); }
            else if (process->property("tlsError").toBool()) fail("The Bitwarden server certificate could not be verified. Add its issuing certificate to this computer's trusted certificates, then reconnect.");
            else if (m_bwConnecting) fail("Bitwarden could not connect. Check the server, vault email and master password; include a fresh vault two-factor code if enabled. Additional account checks may require the Bitwarden app.");
            else fail("Bitwarden could not read this entry. Connect your vault below first, then check the master password and item ID.");
            return;
        }
        done(value); AuthCrypto::erase(value);
    });
    timeout->start(30000);
    process->start(program, arguments);
    process->closeWriteChannel();
}


QString DiscordAuth::bitwardenServer() const {
    return QSettings().value("signIn/bitwardenServer", "https://vault.bitwarden.com").toString();
}

void DiscordAuth::connectBitwarden(const QString &server, const QString &email, const QString &password, const QString &code, int method) {
    cancel();
    QUrl url(server.trimmed());
    if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
        fail("Enter an HTTPS Bitwarden server URL, without account details, query or fragment."); return;
    }
    if (email.trimmed().isEmpty() || password.isEmpty() || (method != 0 && method != 1 && method != 3)) {
        fail("Enter your vault email and Bitwarden master password."); return;
    }
    auto endpoint = url.toString();
    while (endpoint.endsWith('/')) endpoint.chop(1);
    // Wipe our retained password when callbacks complete or are canceled.
    auto secret = std::shared_ptr<QString>(new QString(password), [](QString *value) { value->fill(QChar(0)); delete value; });
    m_bwConnecting = true; m_active = true; m_deadline.start(180000);
    m_status = "Connecting your Bitwarden vault…"; emit changed();
    runBitwarden({"status"}, {}, [this, endpoint, email, secret, code, method](QByteArray bytes) {
        QJsonParseError parse;
        const auto state = QJsonDocument::fromJson(bytes, &parse).object();
        if (parse.error != QJsonParseError::NoError || !state.contains("status")) { fail("Bitwarden returned an unreadable connection status."); return; }
        auto complete = [this, endpoint](QByteArray value) {
            auto session = value.trimmed();
            if (session.isEmpty() || session.contains('\n')) { AuthCrypto::erase(session); fail("Bitwarden did not complete the connection."); return; }
            AuthCrypto::erase(session);
            QSettings().setValue("signIn/bitwardenServer", endpoint);
            m_active = false; m_bwConnecting = false; m_deadline.stop(); m_error.clear();
            m_status = "Bitwarden connected. Enter your Discord login item ID, then sign in from Bitwarden.";
            emit changed();
        };
        if (state.value("status").toString() != "unauthenticated") {
            const auto current = state.value("serverUrl").toString();
            auto normalized = current.isEmpty() ? QString("https://vault.bitwarden.com") : current;
            while (normalized.endsWith('/')) normalized.chop(1);
            if (normalized != endpoint || state.value("userEmail").toString().compare(email.trimmed(), Qt::CaseInsensitive) != 0) {
                fail("A different vault is connected to Relay. Disconnect it below before changing server or account."); return;
            }
            runBitwarden({"unlock", "--passwordenv", "RELAY_BW_PASSWORD", "--raw"}, *secret, complete);
            return;
        }
        runBitwarden({"config", "server", endpoint}, {}, [this, endpoint, email, secret, code, method, complete](QByteArray) {
            QStringList args{"login", email.trimmed(), "--passwordenv", "RELAY_BW_PASSWORD", "--raw", "--nointeraction"};
            if (!code.trimmed().isEmpty()) args << "--method" << QString::number(method) << "--code" << code.trimmed();
            runBitwarden(args, *secret, complete);
        });
    });
}

void DiscordAuth::disconnectBitwarden() {
    setRememberLogin(false);
    cancel(); m_active = true; m_bwConnecting = true;
    m_status = "Disconnecting Bitwarden from Relay…"; emit changed();
    runBitwarden({"logout"}, {}, [this](QByteArray) {
        QSettings().remove("signIn/bitwardenItem");
        m_active = false; m_bwConnecting = false;
        m_status = "Bitwarden disconnected from Relay"; emit changed();
    });
}

QString DiscordAuth::bitwardenItem() const { return QSettings().value("signIn/bitwardenItem").toString(); }

void DiscordAuth::signInBitwarden(const QString &itemId, const QString &masterPassword) {
    cancel();
    static const QRegularExpression id("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    if (!id.match(itemId.trimmed()).hasMatch()) { fail("Enter the item ID of your Discord login from Bitwarden."); return; }
    QSettings().setValue("signIn/bitwardenItem", itemId.trimmed());
    m_bwItem = itemId.trimmed(); m_active = true; m_deadline.start(180000);
    m_status = "Reading your Discord login from Bitwarden…"; emit changed();
    auto fetch = [this] {
        runBitwarden({"get", "item", m_bwItem, "--raw"}, {}, [this](QByteArray data) {
            const auto item = QJsonDocument::fromJson(data).object();
            const auto login = item.value("login").toObject();
            const auto email = login.value("username").toString();
            auto password = login.value("password").toString();
            if (item.value("type").toInt() != 1 || email.isEmpty() || password.isEmpty()) { fail("Select a Bitwarden login entry containing your Discord email and password."); return; }
            m_rememberedLogin = QJsonDocument(login).toJson(QJsonDocument::Compact).toBase64();
            passwordRequest("/auth/login", {{"login", email}, {"password", password}, {"undelete", false}});
            password.fill(QChar(0));
        });
    };
    if (masterPassword.isEmpty()) {
        fetch();
    } else {
        runBitwarden({"unlock", "--passwordenv", "RELAY_BW_PASSWORD", "--raw"}, masterPassword, [this, fetch](QByteArray value) {
            m_bwSession = value.trimmed();
            if (m_bwSession.isEmpty()) { fail("Bitwarden did not unlock. Try again."); return; }
            fetch();
        });
    }
}

void DiscordAuth::fetchBitwardenCode() {
    m_bwTotpAttempted = true; m_active = true;
    m_status = "Getting your two-factor code from Bitwarden…"; emit changed();
    runBitwarden({"get", "totp", m_bwItem, "--raw"}, {}, [this](QByteArray value) {
        const auto code = QString::fromUtf8(value).trimmed();
        if (!QRegularExpression("^[0-9]{6}$").match(code).hasMatch()) {
            m_active = false; m_error = "No authenticator code available in this entry. Enter your code below."; emit changed(); return;
        }
        submitCode(code);
    });
}
