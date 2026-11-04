#include "AuthCrypto.h"
#include "DiscordAuth.h"
#include <QCryptographicHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocketServer>
#include <QtTest>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace {
QByteArray encryptWithPublicKey(const QByteArray &der, const QByteArray &plaintext, bool sha1 = false) {
    const auto *cursor = reinterpret_cast<const unsigned char *>(der.constData());
    EVP_PKEY *key = d2i_PUBKEY(nullptr, &cursor, der.size());
    if (!key) return {};
    EVP_PKEY_CTX *context = EVP_PKEY_CTX_new(key, nullptr);
    QByteArray result;
    size_t size = 0;
    const EVP_MD *hash = sha1 ? EVP_sha1() : EVP_sha256();
    if (context && EVP_PKEY_encrypt_init(context) > 0 &&
        EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_OAEP_PADDING) > 0 &&
        EVP_PKEY_CTX_set_rsa_oaep_md(context, hash) > 0 &&
        EVP_PKEY_CTX_set_rsa_mgf1_md(context, hash) > 0 &&
        EVP_PKEY_encrypt(context, nullptr, &size, reinterpret_cast<const unsigned char *>(plaintext.constData()), plaintext.size()) > 0) {
        result.resize(size);
        if (EVP_PKEY_encrypt(context, reinterpret_cast<unsigned char *>(result.data()), &size,
                reinterpret_cast<const unsigned char *>(plaintext.constData()), plaintext.size()) > 0) result.resize(size);
        else result.clear();
    }
    EVP_PKEY_CTX_free(context);
    EVP_PKEY_free(key);
    return result.toBase64();
}
}

// Complete protocol peer and ticket endpoint on loopback, without real accounts.
class AuthFixture final : public QObject {
public:
    QWebSocketServer server{"Relay auth fixture", QWebSocketServer::NonSecureMode};
    QTcpServer http;
    QPointer<QWebSocket> peer;
    QByteArray publicDer;
    QByteArray nonce = "fixture random nonce with \0 binary bytes";
    QByteArray token = "fixture.account.token.for.local.tests.only";
    QByteArray postBody;
    QByteArray postHeaders;
    bool proofValid = false;
    bool badFingerprint = false;
    bool acknowledgeHeartbeat = true;
    bool captcha = false;
    int timeoutMs = 12000;
    int responseDelayMs = 0;
    int heartbeatCount = 0;
    int exchanges = 0;
    AuthFixture() {
        nonce = QByteArray("fixture-nonce\0binary", 20);
        connect(&server, &QWebSocketServer::newConnection, this, [this] {
            peer = server.nextPendingConnection();
            connect(peer, &QWebSocket::disconnected, peer, &QObject::deleteLater);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString &text) {
                const auto payload = QJsonDocument::fromJson(text.toUtf8()).object();
                const auto op = payload.value("op").toString();
                if (op == "init") {
                    publicDer = QByteArray::fromBase64(payload.value("encoded_public_key").toString().toLatin1());
                    send({{"op", "nonce_proof"}, {"encrypted_nonce", QString::fromLatin1(encryptWithPublicKey(publicDer, nonce))}});
                } else if (op == "nonce_proof") {
                    proofValid = QByteArray::fromBase64(payload.value("nonce").toString().toLatin1(), QByteArray::Base64UrlEncoding) == nonce;
                    auto fingerprint = AuthCrypto::urlBase64(QCryptographicHash::hash(publicDer, QCryptographicHash::Sha256));
                    if (badFingerprint) fingerprint[0] = fingerprint[0] == 'A' ? 'B' : 'A';
                    send({{"op", "pending_remote_init"}, {"fingerprint", QString::fromLatin1(fingerprint)}});
                } else if (op == "heartbeat") {
                    ++heartbeatCount;
                    if (acknowledgeHeartbeat) send({{"op", "heartbeat_ack"}});
                }
            });
            send({{"op", "hello"}, {"heartbeat_interval", 1000}, {"timeout_ms", timeoutMs}});
        });
        connect(&http, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = http.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    const int end = bytes.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    int size = 0;
                    for (const auto &line : bytes.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) size = line.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + size) return;
                    socket->setProperty("handled", true);
                    postHeaders = bytes.left(end);
                    postBody = bytes.mid(end + 4, size);
                    ++exchanges;
                    const auto response = captcha ? QJsonObject{{"captcha_key", QJsonArray{"fixture challenge"}}} :
                        QJsonObject{{"encrypted_token", QString::fromLatin1(encryptWithPublicKey(publicDer, token))}};
                    const auto body = QJsonDocument(response).toJson(QJsonDocument::Compact);
                    const auto status = captcha ? QByteArray("400 Bad Request") : QByteArray("200 OK");
                    QTimer::singleShot(responseDelayMs, socket, [socket, body, status] {
                        if (socket->state() != QAbstractSocket::ConnectedState) return;
                        socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\nContent-Length: " +
                            QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
    bool listen() { return server.listen(QHostAddress::LocalHost) && http.listen(QHostAddress::LocalHost); }
    QString websocketUrl() const { return "ws://127.0.0.1:" + QString::number(server.serverPort()); }
    QString loginUrl() const { return "http://127.0.0.1:" + QString::number(http.serverPort()) + "/login"; }
    void send(const QJsonObject &data) {
        if (peer) peer->sendTextMessage(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)));
    }
    void scan() {
        send({{"op", "pending_ticket"}, {"encrypted_user_payload", QString::fromLatin1(
            encryptWithPublicKey(publicDer, "123456789:0:0:Fixture Person"))}});
    }
    void confirm() {
        send({{"op", "pending_login"}, {"ticket", "fixture-ticket-from-confirmed-phone"}});
        if (peer) peer->close();
    }
};

class AuthTests final : public QObject {
    Q_OBJECT
private:
    static void attach(DiscordAuth &auth, const AuthFixture &fixture) {
        auth.m_gatewayUrl = fixture.websocketUrl();
        auth.m_loginUrl = fixture.loginUrl();
    }
private slots:
    void cryptoUsesSpkiAndOaepSha256() {
        AuthCrypto key;
        QVERIFY(key.generate());
        const auto der = key.publicKeyDer();
        QVERIFY(der.size() > 256);
        QCOMPARE(QByteArray::fromBase64(key.publicKeyBase64()), der);
        const auto *cursor = reinterpret_cast<const unsigned char *>(der.constData());
        EVP_PKEY *publicKey = d2i_PUBKEY(nullptr, &cursor, der.size());
        QVERIFY(publicKey);
        QCOMPARE(EVP_PKEY_get_bits(publicKey), 2048);
        EVP_PKEY_free(publicKey);
        const auto plaintext = QByteArray("test nonce with embedded\0null", 29);
        QCOMPARE(key.decrypt(encryptWithPublicKey(der, plaintext)), plaintext);
        QVERIFY(key.decrypt(encryptWithPublicKey(der, plaintext, true)).isEmpty());
        QVERIFY(key.decrypt("not base64@@@").isEmpty());
        auto damaged = encryptWithPublicKey(der, plaintext);
        damaged[20] = damaged[20] == 'A' ? 'B' : 'A';
        QVERIFY(key.decrypt(damaged).isEmpty());
        AuthCrypto wrongKey;
        QVERIFY(wrongKey.generate());
        QVERIFY(wrongKey.decrypt(encryptWithPublicKey(der, plaintext)).isEmpty());
        QCOMPARE(key.fingerprint(), AuthCrypto::urlBase64(QCryptographicHash::hash(der, QCryptographicHash::Sha256)));
        QByteArray secret("fixture secret");
        AuthCrypto::erase(secret);
        QVERIFY(secret.isEmpty());
    }

    void completesExplicitPhoneFlowAndDecryptsTicket() {
        AuthFixture fixture;
        QVERIFY(fixture.listen());
        DiscordAuth auth;
        attach(auth, fixture);
        QSignalSpy authenticated(&auth, &DiscordAuth::authenticated);
        auth.start();
        QTRY_VERIFY_WITH_TIMEOUT(auth.qrReady(), 3000);
        QVERIFY(fixture.proofValid);
        QVERIFY(auth.active());
        QVERIFY(auth.expiresIn() > 0);
        QVERIFY(auth.qrImage().startsWith("data:image/png;base64,"));
        const auto png = QByteArray::fromBase64(auth.qrImage().section(',', 1).toLatin1());
        QImage image;
        QVERIFY(image.loadFromData(png, "PNG"));
        QVERIFY(image.width() >= 200);
        QCOMPARE(image.pixelColor(0, 0), QColor(Qt::white));
        fixture.scan();
        QTRY_COMPARE_WITH_TIMEOUT(auth.scannedUserName(), "Fixture Person", 1000);
        QVERIFY(!auth.qrReady());
        QVERIFY(auth.qrImage().isEmpty());
        fixture.confirm();
        QTRY_COMPARE_WITH_TIMEOUT(authenticated.count(), 1, 2000);
        QCOMPARE(authenticated.at(0).at(0).toString(), QString::fromLatin1(fixture.token));
        QVERIFY(auth.completed());
        QVERIFY(!auth.active());
        QVERIFY(!auth.m_crypto);
        QCOMPARE(fixture.exchanges, 1);
        QCOMPARE(QJsonDocument::fromJson(fixture.postBody).object().value("ticket").toString(), "fixture-ticket-from-confirmed-phone");
        QVERIFY(!fixture.postHeaders.toLower().contains("authorization:"));
    }

    void wrongFingerprintNeverBecomesScannable() {
        AuthFixture fixture;
        fixture.badFingerprint = true;
        QVERIFY(fixture.listen());
        DiscordAuth auth;
        attach(auth, fixture);
        auth.start();
        QTRY_VERIFY_WITH_TIMEOUT(!auth.active(), 3000);
        QVERIFY(auth.error().contains("encryption key"));
        QVERIFY(!auth.qrReady());
        QVERIFY(auth.qrImage().isEmpty());
    }

    void cancellationDropsKeyAndIgnoresLateCallbacks() {
        AuthFixture fixture;
        QVERIFY(fixture.listen());
        fixture.responseDelayMs = 250;
        DiscordAuth auth;
        attach(auth, fixture);
        QSignalSpy authenticated(&auth, &DiscordAuth::authenticated);
        auth.start();
        QTRY_VERIFY_WITH_TIMEOUT(auth.qrReady(), 3000);
        fixture.scan();
        QTRY_VERIFY_WITH_TIMEOUT(!auth.scannedUserName().isEmpty(), 1000);
        fixture.confirm();
        QTRY_COMPARE_WITH_TIMEOUT(fixture.exchanges, 1, 1000);
        auth.cancel();
        QTest::qWait(400);
        QCOMPARE(authenticated.count(), 0);
        QVERIFY(!auth.active());
        QVERIFY(auth.qrImage().isEmpty());
        QVERIFY(!auth.m_crypto);
        QVERIFY(auth.m_fingerprint.isEmpty());
    }

    void challengeIsReportedWithoutBypassOrRetry() {
        AuthFixture fixture;
        fixture.captcha = true;
        QVERIFY(fixture.listen());
        DiscordAuth auth;
        attach(auth, fixture);
        QSignalSpy authenticated(&auth, &DiscordAuth::authenticated);
        auth.start();
        QTRY_VERIFY_WITH_TIMEOUT(auth.qrReady(), 3000);
        fixture.scan();
        QTRY_VERIFY_WITH_TIMEOUT(!auth.scannedUserName().isEmpty(), 1000);
        fixture.confirm();
        QTRY_VERIFY_WITH_TIMEOUT(!auth.active(), 2000);
        QVERIFY(auth.error().contains("verification challenge"));
        QCOMPARE(authenticated.count(), 0);
        QCOMPARE(fixture.exchanges, 1);
        QVERIFY(auth.qrImage().isEmpty());
    }

    void expiryInvalidatesQrAndMissingHeartbeatStopsSession() {
        AuthFixture fixture;
        fixture.timeoutMs = 1000;
        QVERIFY(fixture.listen());
        DiscordAuth auth;
        attach(auth, fixture);
        auth.start();
        QTRY_VERIFY_WITH_TIMEOUT(auth.qrReady(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!auth.active(), 2500);
        QVERIFY(auth.error().contains("expired"));
        QVERIFY(auth.qrImage().isEmpty());
        fixture.timeoutMs = 10000;
        fixture.acknowledgeHeartbeat = false;
        auth.retry();
        QTRY_VERIFY_WITH_TIMEOUT(auth.qrReady(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!auth.active(), 3500);
        QVERIFY(auth.error().contains("stopped responding"));
    }

    void immediateCancelPreventsConnection() {
        DiscordAuth auth;
        auth.start();
        auth.cancel();
        QTest::qWait(30);
        QVERIFY(!auth.active());
        QVERIFY(!auth.m_socket);
        QVERIFY(!auth.m_crypto);
    }

    void longerServiceLeaseHasLocalExpiry() {
        AuthFixture fixture;
        fixture.timeoutMs = 600000;
        QVERIFY(fixture.listen());
        DiscordAuth auth;
        attach(auth, fixture);
        auth.start();
        QTRY_VERIFY_WITH_TIMEOUT(auth.qrReady(), 3000);
        QVERIFY(auth.expiresIn() > 0);
        QVERIFY(auth.expiresIn() <= 180);
        auth.cancel();
    }


    void passwordAndMfaFlow() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QJsonObject> bodies;
        QList<QByteArray> paths;
        int requests = 0;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto *socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                auto data = socket->property("data").toByteArray() + socket->readAll();
                socket->setProperty("data", data);
                int end = data.indexOf("\r\n\r\n");
                if (end < 0 || socket->property("handled").toBool()) return;
                int length = 0;
                for (const auto &line : data.left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                if (data.size() < end + 4 + length) return;
                socket->setProperty("handled", true);
                paths << data.split(' ').value(1);
                bodies << QJsonDocument::fromJson(data.mid(end + 4, length)).object();
                ++requests;
                const auto body = requests == 1 ? QByteArray(R"({"mfa":true,"ticket":"generated-fixture-ticket"})") :
                    requests == 2 ? QByteArray(R"({"message":"invalid code"})") : QByteArray(R"({"token":"generated.fixture.session.token"})");
                socket->write(QByteArray("HTTP/1.1 ") + (requests == 2 ? "400 Bad Request" : "200 OK") + "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        DiscordAuth auth;
        auth.m_apiBase = QString("http://127.0.0.1:%1").arg(server.serverPort());
        QSignalSpy signedIn(&auth, &DiscordAuth::authenticated);
        auth.signInPassword("fixture@example.invalid", "generated-password");
        QTRY_VERIFY(auth.mfaRequired() && !auth.active());
        QCOMPARE(bodies[0].value("password").toString(), QString("generated-password"));
        QCOMPARE(paths[0], QByteArray("/auth/login"));
        auth.submitCode("000000");
        QTRY_VERIFY(!auth.active() && !auth.error().isEmpty());
        QVERIFY(auth.mfaRequired());
        auth.submitCode("generated-backup", true);
        QTRY_COMPARE(signedIn.size(), 1);
        QCOMPARE(paths[2], QByteArray("/auth/mfa/totp"));
        QVERIFY(bodies[2].value("backup").toBool());
        QVERIFY(!auth.mfaRequired());
        QVERIFY(auth.completed());

        requests = 0;
        QSignalSpy remembered(&auth, &DiscordAuth::rememberedLoginReady);
        const auto encoded = QJsonDocument(QJsonObject{{"username", "fixture@example.invalid"}, {"password", "generated-password"}, {"totp", "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"}}).toJson(QJsonDocument::Compact).toBase64();
        auth.signInSavedCredential(QString::fromLatin1(encoded));
        QTRY_VERIFY(requests == 2 && !auth.active());
        QCOMPARE(bodies[0].value("password").toString(), QString("generated-password"));
        QCOMPARE(bodies[1].value("code").toString().size(), 6);
        QVERIFY(auth.mfaRequired());
        QCOMPARE(remembered.size(), 0);
        auth.submitCode("generated-backup", true);
        QTRY_COMPARE(remembered.size(), 1);
        QCOMPARE(remembered[0][0].toString(), QString::fromLatin1(encoded));
        QVERIFY(auth.m_savedTotp.isEmpty());
        QVERIFY(auth.m_rememberedLogin.isEmpty());
        signedIn.clear();

        #ifndef Q_OS_WIN
        // Exercise the real asynchronous subprocess chain with generated vault data.
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile cli(directory.filePath("bw-fixture"));
        QVERIFY(cli.open(QIODevice::WriteOnly));
        cli.write(R"(#!/bin/sh
case "$1:$2" in
unlock:--passwordenv)
  test "$RELAY_BW_PASSWORD" = generated-master || exit 1
  printf 'generated-session'
  ;;
get:item)
  test "$BW_SESSION" = generated-session || exit 1
  printf '{"type":1,"login":{"username":"fixture@example.invalid","password":"generated-password"}}'
  ;;
get:totp)
  test "$BW_SESSION" = generated-session || exit 1
  printf '123456'
  ;;
*) exit 1;;
esac
)");
        cli.close();
        QVERIFY(cli.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        auth.m_bwProgram = cli.fileName();
        requests = 0;
        // Second request deliberately rejects the automatic TOTP, preserving manual retry.
        auth.signInBitwarden("12345678-1234-1234-1234-123456789abc", "generated-master");
        QTRY_VERIFY(requests == 2 && !auth.active());
        QCOMPARE(bodies.last().value("code").toString(), QString("123456"));
        QVERIFY(auth.mfaRequired());
        auth.submitCode("generated-backup", true);
        QTRY_COMPARE(signedIn.size(), 1);
        QVERIFY(auth.m_bwSession.isEmpty());
        QVERIFY(!auth.m_bwProcess);
        #endif
    }


    void rememberedCancellationWipesSecrets() {
        DiscordAuth auth;
        auth.m_rememberedLogin = "generated-fixture-record";
        auth.m_savedTotp = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
        auth.cancel();
        QVERIFY(auth.m_rememberedLogin.isEmpty());
        QVERIFY(auth.m_savedTotp.isEmpty());
        QVERIFY(!auth.m_savedTotpAttempted);
    }

    void rememberedTotpVectors() {
        const QString secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
        QCOMPARE(DiscordAuth::totpCode(secret, 59), QString("287082"));
        QCOMPARE(DiscordAuth::totpCode("otpauth://totp/test?secret=" + secret + "&digits=8", 59), QString("94287082"));
        QCOMPARE(DiscordAuth::totpCode("otpauth://totp/test?secret=" + secret + "&period=0", 59), QString());
        QCOMPARE(DiscordAuth::totpCode("invalid!", 59), QString());
    }

    void optionalBitwardenConnection() {
#ifdef Q_OS_WIN
        QSKIP("This generated CLI fixture is a Unix shell program; native Windows Bitwarden requires a manual check.");
#endif
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile cli(directory.filePath("bw-connect-fixture"));
        QVERIFY(cli.open(QIODevice::WriteOnly));
        cli.write("#!/bin/sh\nset -eu\nbase=\"$(dirname \"$0\")\"\ntest -n \"$BITWARDENCLI_APPDATA_DIR\"\nfor arg in \"$@\"; do test \"$arg\" != generated-master; done\ncase \"$1\" in\nstatus)\n  if test -f \"$base/account\"; then\n    printf '{\"status\":\"locked\",\"serverUrl\":\"https://vault.example.invalid\",\"userEmail\":\"fixture@example.invalid\"}'\n  else\n    printf '{\"status\":\"unauthenticated\"}'\n  fi;;\nconfig)\n  test \"$2\" = server\n  printf '%s' \"$3\" > \"$base/server\"\n  printf 'Saved setting config';;\nlogin)\n  test \"$2\" = fixture@example.invalid\n  test \"$RELAY_BW_PASSWORD\" = generated-master\n  test \"$(cat \"$base/server\")\" = https://vault.example.invalid\n  touch \"$base/account\"\n  printf 'generated-vault-session';;\nunlock)\n  test \"$RELAY_BW_PASSWORD\" = generated-master\n  printf 'generated-vault-session';;\nlogout)\n  rm -f \"$base/account\"\n  printf 'Logged out';;\n*) exit 1;;\nesac\n");
        cli.close();
        QVERIFY(cli.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        DiscordAuth auth;
        auth.m_bwProgram = cli.fileName();
        QSignalSpy signedIn(&auth, &DiscordAuth::authenticated);
        auth.connectBitwarden("http://vault.example.invalid", "fixture@example.invalid", "generated-master", "", 0);
        QVERIFY(!auth.active());
        QVERIFY(auth.error().contains("HTTPS"));
        auth.connectBitwarden("https://vault.example.invalid/", "fixture@example.invalid", "generated-master", "123456", 0);
        QTRY_VERIFY(!auth.active());
        QVERIFY2(auth.error().isEmpty(), qPrintable(auth.error()));
        QCOMPARE(auth.bitwardenServer(), QString("https://vault.example.invalid"));
        QVERIFY(auth.status().startsWith("Bitwarden connected"));
        QVERIFY(auth.m_bwSession.isEmpty());
        QCOMPARE(signedIn.size(), 0);
        auth.connectBitwarden("https://other.example.invalid", "fixture@example.invalid", "generated-master", "", 0);
        QTRY_VERIFY(!auth.active());
        QVERIFY(auth.error().contains("different vault"));
        QCOMPARE(auth.bitwardenServer(), QString("https://vault.example.invalid"));
        auth.disconnectBitwarden();
        QTRY_VERIFY(!auth.active());
        QVERIFY(auth.error().isEmpty());
        QVERIFY(!QFile::exists(directory.filePath("account")));
        auth.connectBitwarden("https://vault.example.invalid", "fixture@example.invalid", "generated-master", "", 0);
        auth.cancel();
        QVERIFY(!auth.m_bwProcess);
        QVERIFY(!auth.active());
    }

    void passwordCancellationAndValidation() {
        DiscordAuth auth;
        auth.signInPassword("", "");
        QVERIFY(!auth.active());
        QVERIFY(!auth.error().isEmpty());
        auth.signInBitwarden("--invalid-option", "generated-password");
        QVERIFY(!auth.active());
        QVERIFY(!auth.m_bwProcess);
        auth.m_mfaTicket = "generated-ticket";
        auth.m_bwSession = "generated-session";
        auth.cancel();
        QVERIFY(!auth.mfaRequired());
        QVERIFY(auth.m_bwSession.isEmpty());
    }

    void liveRemoteAuthHandshake() {
        if (!qEnvironmentVariableIsSet("RELAY_TEST_LIVE_QR_HANDSHAKE"))
            QSKIP("Explicit opt-in required for unauthenticated Discord handshake; no account or QR is exposed.");
        DiscordAuth auth;
        auth.start();
        QTRY_VERIFY_WITH_TIMEOUT(auth.qrReady() || !auth.active(), 25000);
        QVERIFY2(auth.qrReady(), qPrintable(auth.error()));
        QVERIFY(auth.expiresIn() > 0);
        QVERIFY(!auth.completed());
        auth.cancel();
        QVERIFY(!auth.qrReady());
    }
};
QTEST_GUILESS_MAIN(AuthTests)
#include "auth_test.moc"
