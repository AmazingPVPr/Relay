#include "AccountSettingsService.h"
#include "DiscordClient.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace {
QJsonObject self() { return {{"id", "42"}, {"username", "fixture"}, {"global_name", "Generated User"}, {"email", "fixture@example.invalid"}, {"phone", "+15555550123"}, {"mfa_enabled", true}, {"verified", true}, {"premium_type", 2}, {"token", "fixture-private-response-token"}}; }
QByteArray json(const QJsonValue &value) { return value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact) : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact); }
QByteArray vint(quint64 value) { QByteArray result; do { quint8 byte = value & 127; value >>= 7; result += char(byte | (value ? 128 : 0)); } while (value); return result; }
QByteArray proto(quint64 field, const QByteArray &payload) { return vint(field * 8 + 2) + vint(payload.size()) + payload; }
QByteArray wrapped(quint64 field, quint64 value) { return proto(field, QByteArray(1, 8) + vint(value)); }
class AccountHttp final : public QObject {
public:
    struct Reply {
        int status; QByteArray body, headers; int delay;
        Reply(int code = 200, QByteArray response = "{}", QByteArray extra = {}, int wait = 0) : status(code), body(std::move(response)), headers(std::move(extra)), delay(wait) {}
    };
    struct Request { QByteArray method; QString path; QJsonObject body; QByteArray authorization, mfaAuthorization; };
    QTcpServer server; QList<Request> requests; std::function<Reply(const Request &)> handler;
    bool listen() {
        if (!server.listen(QHostAddress::LocalHost, 0)) return false;
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto bytes = socket->property("bytes").toByteArray() + socket->readAll(); socket->setProperty("bytes", bytes); if (socket->property("handled").toBool()) return;
                    const int end = bytes.indexOf("\r\n\r\n"); if (end < 0) return; int length = 0; QByteArray auth, mfa;
                    for (const auto &line : bytes.left(end).split('\n')) { if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt(); if (line.toLower().startsWith("authorization:")) auth = line.mid(14).trimmed(); if (line.toLower().startsWith("x-discord-mfa-authorization:")) mfa = line.mid(28).trimmed(); }
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("handled", true); const auto first = bytes.left(bytes.indexOf("\r\n")).split(' ');
                    Request request{first.value(0), QString::fromLatin1(first.value(1)), QJsonDocument::fromJson(bytes.mid(end + 4, length)).object(), auth, mfa}; requests.append(request);
                    const auto reply = handler ? handler(request) : Reply{}; QPointer<QTcpSocket> guarded(socket);
                    QTimer::singleShot(reply.delay, socket, [guarded, reply] { if (!guarded || guarded->state() != QAbstractSocket::ConnectedState) return;
                        guarded->write("HTTP/1.1 " + QByteArray::number(reply.status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(reply.body.size()) + "\r\nConnection: close\r\n" + reply.headers + "\r\n" + reply.body); guarded->disconnectFromHost(); });
                }); connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        }); return true;
    }
    QString base() const { return "http://127.0.0.1:" + QString::number(server.serverPort()); }
};
}

class AccountSettingsServiceTest final : public QObject {
    Q_OBJECT
    static void clientFixture(DiscordClient &client, const AccountHttp &server) { client.m_apiBase = server.base(); client.m_token = "fixture-token-0000000000000000"; client.m_currentUserId = "42"; client.m_users.insert("42", {{"id", "42"}, {"username", "fixture"}}); }
private slots:
    void accountReadsAreExplicitAndExposeOnlySafeFields() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &) { return AccountHttp::Reply{200, json(self())}; };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true; QCOMPARE(server.requests.size(), 0); QVERIFY(service.account().isEmpty());
        service.loadPage("account"); QVERIFY(service.busy()); QTRY_VERIFY(!service.busy()); QCOMPARE(server.requests.size(), 1); QCOMPARE(server.requests.first().path, QString("/users/@me"));
        QCOMPARE(service.account().value("username").toString(), QString("fixture")); QCOMPARE(service.account().value("mfaEnabled").toBool(), true);
        QVERIFY(!service.account().contains("token")); QVERIFY(!json(QJsonObject::fromVariantMap(service.account())).contains("private-response-token")); QVERIFY(!client.m_users.value("42").contains("email"));
        service.close(); QVERIFY(service.account().isEmpty()); QVERIFY(service.items().isEmpty()); QVERIFY(service.details().isEmpty());
    }
    void accountSaveUsesOneExplicitWriteAndSafelyRotatesSession() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) { auto object = self(); object.insert("global_name", request.body.value("global_name")); object.insert("token", "fixture-replacement-session-00000"); return AccountHttp::Reply{200, json(object)}; };
        DiscordClient client; clientFixture(client, server); client.m_authSessionIdHash = "old-session-hash"; AccountSettingsService service(&client); service.m_fixtureMode = true; QSignalSpy rotated(&service, &AccountSettingsService::credentialRotated), completed(&service, &AccountSettingsService::actionCompleted);
        QVERIFY(service.execute("update-account", {}, {{"globalName", "A new display"}, {"currentPassword", "fixture-current-secret"}, {"newPassword", "fixture-new-secret"}})); QVERIFY(service.mutating());
        QTRY_VERIFY(!service.mutating()); QCOMPARE(server.requests.size(), 1); const auto request = server.requests.first(); QCOMPARE(request.method, QByteArray("PATCH")); QCOMPARE(request.body.value("new_password").toString(), QString("fixture-new-secret")); QCOMPARE(request.body.value("password").toString(), QString("fixture-current-secret"));
        QCOMPARE(client.m_token, QByteArray("fixture-replacement-session-00000")); QVERIFY(client.m_authSessionIdHash.isEmpty()); QCOMPARE(rotated.size(), 1); QCOMPARE(completed.size(), 1); QCOMPARE(service.account().value("globalName").toString(), QString("A new display"));
        const auto exposed = json(QJsonObject::fromVariantMap(service.account())) + json(QJsonObject::fromVariantMap(service.details())) + service.error().toUtf8() + service.status().toUtf8(); QVERIFY(!exposed.contains("secret")); QVERIFY(!exposed.contains("replacement-session"));
    }
    void rejectsUnsupportedFieldsAndNeverRetriesSensitiveFailures() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &) { return AccountHttp::Reply{429, "{\"message\":\"echo fixture-current-secret\",\"retry_after\":0.01}"}; };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true;
        QVERIFY(!service.execute("purchase", {}, {})); QVERIFY(!service.execute("update-account", {}, {{"phone", "+1"}})); QVERIFY(!service.execute("update-account", {}, {{"username", "validname"}})); QVERIFY(!service.execute("update-account", {}, {{"email", "bad"}, {"currentPassword", "secret"}})); QCOMPARE(server.requests.size(), 0);
        QVERIFY(service.execute("update-account", {}, {{"username", "validname"}, {"currentPassword", "fixture-current-secret"}})); QTRY_VERIFY(!service.mutating()); QVERIFY(!service.error().isEmpty()); QVERIFY(!service.error().contains("fixture-current-secret")); QTest::qWait(200); QCOMPARE(server.requests.size(), 1);
    }
    void staleAccountCloseAndPageCallbacksNeverRepopulatePrivateState() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) { return AccountHttp::Reply{200, request.path == "/users/@me" ? json(self()) : QByteArray("[]"), {}, request.path == "/users/@me" ? 150 : 0}; };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true;
        service.loadPage("account"); QTRY_COMPARE(server.requests.size(), 1); service.loadPage("connections"); QTRY_VERIFY(!service.busy()); QTest::qWait(180); QCOMPARE(service.page(), QString("connections")); QVERIFY(service.account().isEmpty());
        service.loadPage("account"); QTRY_COMPARE(server.requests.size(), 3); client.m_currentUserId = "99"; emit client.stateChanged(); QVERIFY(!service.busy()); QTest::qWait(180); QVERIFY(service.account().isEmpty());
        client.m_currentUserId = "42"; emit client.stateChanged(); service.loadPage("account"); QTRY_COMPARE(server.requests.size(), 4); service.close(); QTest::qWait(180); QVERIFY(!service.busy()); QVERIFY(service.account().isEmpty());
    }
    void nativeListsStripTokensAndRestrictActionsToTheirLoadedItems() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) {
            if (request.method == "DELETE") return AccountHttp::Reply{204, {}};
            if (request.path == "/oauth2/tokens") return AccountHttp::Reply{200, "[{\"id\":\"111\",\"access_token\":\"private-access\",\"application\":{\"id\":\"222\",\"name\":\"Generated app\",\"secret\":\"never-show\"},\"scopes\":[\"identify\"]}]"};
            return AccountHttp::Reply{200, "[{\"id\":\"steam-user\",\"type\":\"steam\",\"name\":\"Generated connection\",\"access_token\":\"private-access\"}]"};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true;
        service.loadPage("apps"); QTRY_VERIFY(!service.busy()); QCOMPARE(service.items().size(), 1); QVERIFY(!json(QJsonArray::fromVariantList(service.items())).contains("private-access")); QVERIFY(!json(QJsonArray::fromVariantList(service.items())).contains("never-show"));
        QVERIFY(!service.execute("revoke-app", "not-loaded")); QVERIFY(service.execute("revoke-app", "111")); QTRY_VERIFY(!service.mutating()); QVERIFY(service.items().isEmpty()); QCOMPARE(server.requests.last().path, QString("/oauth2/tokens/111"));
        service.loadPage("connections"); QTRY_VERIFY(!service.busy()); QVERIFY(!service.execute("revoke-app", "steam:steam-user")); QVERIFY(service.execute("disconnect-connection", "steam:steam-user")); QTRY_VERIFY(!service.mutating()); QCOMPARE(server.requests.last().path, QString("/users/@me/connections/steam/steam-user"));
    }
    void messagingPreservesUnknownProtoFieldsAndUsesVerifiedRelationshipActions() {
        const auto unknown = proto(90, "future-preference"); const auto initial = proto(8, unknown + wrapped(11, 14) + wrapped(17, 1));
        AccountHttp server; QVERIFY(server.listen()); server.handler = [initial](const auto &request) {
            if (request.path == "/users/@me/relationships") return AccountHttp::Reply{200, "[{\"id\":\"111\",\"type\":2,\"user\":{\"id\":\"111\",\"username\":\"Blocked fixture\"}},{\"id\":\"222\",\"type\":1,\"user_ignored\":true,\"user\":{\"id\":\"222\",\"username\":\"Ignored fixture\"}},{\"id\":\"333\",\"type\":1}]"};
            if (request.method == "DELETE") return AccountHttp::Reply{204, {}};
            const auto settings = request.method == "PATCH" ? request.body.value("settings").toString() : QString::fromLatin1(initial.toBase64()); return AccountHttp::Reply{200, json(QJsonObject{{"settings", settings}})};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true; service.loadPage("messaging"); QTRY_VERIFY(!service.busy()); QVERIFY(service.capabilities().value("preferences").toBool()); QCOMPARE(service.items().size(), 2);
        const auto prefs = service.details().value("preferences").toMap(); QVERIFY(prefs.value("defaultDmsAllowed").toBool()); QVERIFY(!prefs.value("messageRequestsAllowed").toBool()); QVERIFY(prefs.value("friendRequestsEveryone").toBool());
        QVERIFY(!service.execute("update-preferences", {}, {{"unverified-setting", true}})); QVERIFY(service.execute("update-preferences", {}, {{"defaultDmsAllowed", false}, {"friendRequestsEveryone", false}})); QTRY_VERIFY(!service.mutating());
        const auto sent = QByteArray::fromBase64(server.requests.last().body.value("settings").toString().toLatin1()); QVERIFY(sent.contains(unknown)); QVERIFY(sent.contains(wrapped(11, 6))); QVERIFY(sent.contains(wrapped(27, 1))); QVERIFY(!service.details().value("preferences").toMap().value("defaultDmsAllowed").toBool());
        QVERIFY(service.execute("unignore-user", "222")); QTRY_VERIFY(!service.mutating()); QCOMPARE(server.requests.last().path, QString("/users/@me/relationships/222/ignore")); QVERIFY(!service.execute("unignore-user", "111")); QVERIFY(service.execute("unblock-user", "111")); QTRY_VERIFY(!service.mutating()); QVERIFY(service.items().isEmpty());
    }
    void malformedPreferencesAndForeignAccountNeverBecomeEditable() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) { if (request.path == "/users/@me") { auto foreign = self(); foreign.insert("id", "99"); return AccountHttp::Reply{200, json(foreign)}; } return AccountHttp::Reply{200, request.path.contains("settings-proto") ? QByteArray("{\"settings\":\"%%%invalid%%%\"}") : QByteArray("[]")}; };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true; service.loadPage("privacy"); QTRY_VERIFY(!service.busy()); QVERIFY(!service.capabilities().value("preferences").toBool()); QVERIFY(!service.error().isEmpty()); QVERIFY(!service.execute("update-preferences", {}, {{"defaultDmsAllowed", true}}));
        service.loadPage("account"); QTRY_VERIFY(!service.busy()); QVERIFY(service.account().isEmpty()); QVERIFY(!service.error().isEmpty());
    }
    void redirectsCannotSendCredentialsToAnotherEndpoint() {
        AccountHttp source, destination; QVERIFY(source.listen()); QVERIFY(destination.listen()); source.handler = [&destination](const auto &) { return AccountHttp::Reply{302, "{}", "Location: " + destination.base().toUtf8() + "/capture\r\n"}; };
        DiscordClient client; clientFixture(client, source); AccountSettingsService service(&client); service.m_fixtureMode = true; service.loadPage("account"); QTRY_VERIFY(!service.busy()); QCOMPARE(source.requests.size(), 1); QCOMPARE(destination.requests.size(), 0); QVERIFY(!service.error().isEmpty());
    }
    void emailPreferencesUseOnlyVerifiedBooleanCategories() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) {
            const auto categories = request.method == "PATCH" ? request.body.value("settings").toObject().value("categories").toObject() : QJsonObject{{"communication", true}, {"social", false}, {"unverified-key", "private"}};
            return AccountHttp::Reply{200, json(QJsonObject{{"categories", categories}, {"initialized", true}})};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true;
        QVERIFY(!service.execute("update-email-preferences", {}, {{"communication", false}})); service.loadPage("email"); QTRY_VERIFY(!service.busy());
        QCOMPARE(service.details().value("categories").toMap().size(), 2); QVERIFY(service.capabilities().value("emailPreferences").toBool());
        QVERIFY(!service.execute("update-email-preferences", {}, {{"unverified-key", true}})); QVERIFY(!service.execute("update-email-preferences", {}, {{"social", "false"}}));
        QVERIFY(service.execute("update-email-preferences", {}, {{"communication", false}, {"family_center_digest", true}})); QTRY_VERIFY(!service.mutating());
        QCOMPARE(server.requests.last().method, QByteArray("PATCH")); QCOMPARE(server.requests.last().path, QString("/users/@me/email-settings"));
        QCOMPARE(server.requests.last().body, (QJsonObject{{"settings", QJsonObject{{"categories", QJsonObject{{"communication", false}, {"family_center_digest", true}}}}}}));
        QVERIFY(!service.details().value("categories").toMap().value("communication").toBool());
    }
    void authenticatorSetupIsLocalRandomAndClearsSecretsAfterActivation() {
        QString submittedSecret;
        AccountHttp server; QVERIFY(server.listen()); server.handler = [&submittedSecret](const auto &request) {
            if (request.method == "GET") { auto account = self(); account.insert("mfa_enabled", false); return AccountHttp::Reply{200, json(account)}; }
            submittedSecret = request.body.value("secret").toString(); return AccountHttp::Reply{200, "{\"token\":\"fixture-new-mfa-session-000000\",\"backup_codes\":[{\"code\":\"1234-5678\",\"consumed\":false}]}"};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true; service.loadPage("account"); QTRY_VERIFY(!service.busy());
        QVERIFY(service.capabilities().value("mfaEnroll").toBool()); QVERIFY(!service.execute("mfa-confirm", {}, {{"code", "123456"}}));
        const auto reads = server.requests.size(); QVERIFY(service.execute("mfa-begin")); QCOMPARE(server.requests.size(), reads);
        const auto first = service.details().value("mfaSecret").toString(); QVERIFY(QRegularExpression("^[A-Z2-7]{32}$").match(first).hasMatch());
        QVERIFY(service.details().value("mfaUri").toString().startsWith("otpauth://totp/Discord:")); QVERIFY(service.details().value("mfaUri").toString().contains(first)); QVERIFY(service.details().value("mfaQrImage").toString().startsWith("data:image/svg+xml;base64,"));
        QVERIFY(service.execute("mfa-cancel")); QVERIFY(service.details().isEmpty()); QVERIFY(service.m_mfaSecret.isEmpty());
        QVERIFY(service.execute("mfa-begin")); const auto second = service.details().value("mfaSecret").toString(); QVERIFY(first != second); QVERIFY(!service.execute("mfa-confirm", {}, {{"code", "123"}}));
        QSignalSpy rotated(&service, &AccountSettingsService::credentialRotated); QVERIFY(service.execute("mfa-confirm", {}, {{"code", "123 456"}})); QTRY_VERIFY(!service.mutating());
        QCOMPARE(server.requests.last().path, QString("/users/@me/mfa/totp/enable")); QCOMPARE(server.requests.last().body.value("code").toString(), QString("123456")); QCOMPARE(submittedSecret, second);
        QVERIFY(service.m_mfaSecret.isEmpty()); QVERIFY(!service.details().contains("mfaSecret")); QVERIFY(!service.details().contains("mfaQrImage")); QVERIFY(service.account().value("mfaEnabled").toBool()); QCOMPARE(rotated.size(), 1);
        QCOMPARE(service.details().value("backupCodes").toList().size(), 1); QVERIFY(!service.capabilities().value("mfaEnroll").toBool()); QVERIFY(service.capabilities().value("mfaDisable").toBool());
        QVERIFY(service.execute("mfa-cancel")); QVERIFY(!service.details().contains("backupCodes"));
    }
    void recoveryCodesRequirePasswordThenEmailNonceAndNeverExposeNonce() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) {
            if (request.method == "GET") return AccountHttp::Reply{200, json(self())};
            if (request.path == "/auth/verify/view-backup-codes-challenge") return AccountHttp::Reply{200, "{\"nonce\":\"fixture-view-nonce\",\"regenerate_nonce\":\"fixture-regenerate-nonce\"}"};
            return AccountHttp::Reply{200, "{\"backup_codes\":[{\"code\":\"1111-2222\",\"consumed\":false},{\"code\":\"3333-4444\",\"consumed\":true}]}"};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true; service.loadPage("account"); QTRY_VERIFY(!service.busy());
        QVERIFY(!service.execute("mfa-backup-confirm", {}, {{"key", "123456"}, {"regenerate", false}})); QVERIFY(!service.execute("mfa-backup-send", {}, {{"currentPassword", ""}}));
        QVERIFY(service.execute("mfa-backup-send", {}, {{"currentPassword", "fixture-current-secret"}})); QTRY_VERIFY(!service.mutating()); QCOMPARE(service.details().value("backupStage").toString(), QString("email"));
        const auto exposed = json(QJsonObject::fromVariantMap(service.details())); QVERIFY(!exposed.contains("nonce")); QVERIFY(!exposed.contains("secret"));
        QVERIFY(service.execute("mfa-backup-confirm", {}, {{"key", "fixture-email-key"}, {"regenerate", false}})); QTRY_VERIFY(!service.mutating());
        QCOMPARE(server.requests.last().path, QString("/users/@me/mfa/codes-verification")); QCOMPARE(server.requests.last().body.value("nonce").toString(), QString("fixture-view-nonce")); QCOMPARE(service.details().value("backupCodes").toList().size(), 2); QVERIFY(service.m_backupNonce.isEmpty()); QVERIFY(service.m_regenerateNonce.isEmpty());
        QVERIFY(!service.execute("mfa-backup-confirm", {}, {{"key", "fixture-email-key"}, {"regenerate", false}}));
        service.close(); QVERIFY(service.details().isEmpty()); QVERIFY(service.m_backupNonce.isEmpty());
    }
    void genericMfaChallengeRetriesOnlyAfterExplicitVerificationAndOnlyOnce() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) {
            if (request.method == "GET") return AccountHttp::Reply{200, json(self())};
            if (request.path == "/mfa/finish") return AccountHttp::Reply{200, "{\"token\":\"fixture-short-lived-mfa-approval\"}"};
            if (request.mfaAuthorization.isEmpty()) return AccountHttp::Reply{401, "{\"code\":60003,\"mfa\":{\"ticket\":\"fixture-ticket\",\"methods\":[{\"type\":\"webauthn\"},{\"type\":\"totp\"},{\"type\":\"backup\"},{\"type\":\"password\"}]}}"};
            return AccountHttp::Reply{200, "{\"token\":\"fixture-mfa-disabled-session-000\"}"};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true; service.loadPage("account"); QTRY_VERIFY(!service.busy());
        QVERIFY(service.execute("mfa-disable")); QTRY_VERIFY(!service.mutating()); QCOMPARE(service.challengeMethods(), (QStringList{"totp", "backup", "password"})); QCOMPARE(server.requests.size(), 2);
        QTest::qWait(50); QCOMPARE(server.requests.size(), 2); QVERIFY(!service.execute("update-account", {}, {{"globalName", "during challenge"}})); QVERIFY(!service.confirmMfa("webauthn", "anything")); QVERIFY(!service.confirmMfa("totp", "short"));
        QVERIFY(service.confirmMfa("totp", "123 456")); QTRY_VERIFY(!service.mutating() && service.challengeMethods().isEmpty()); QCOMPARE(server.requests.size(), 4);
        QCOMPARE(server.requests[2].path, QString("/mfa/finish")); QCOMPARE(server.requests[2].body, (QJsonObject{{"ticket", "fixture-ticket"}, {"mfa_type", "totp"}, {"data", "123456"}})); QVERIFY(server.requests[2].mfaAuthorization.isEmpty());
        QCOMPARE(server.requests[3].path, QString("/users/@me/mfa/totp/disable")); QCOMPARE(server.requests[3].mfaAuthorization, QByteArray("fixture-short-lived-mfa-approval")); QVERIFY(!service.account().value("mfaEnabled").toBool()); QVERIFY(!service.m_pendingMfa);
        service.loadPage("account"); QTRY_VERIFY(!service.busy()); QVERIFY(server.requests.last().mfaAuthorization.isEmpty());
    }
    void challengeFailureCancelExpiryAndAccountChangesCannotReplaySensitiveBodies() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) {
            if (request.path == "/mfa/finish") return AccountHttp::Reply{400, "{\"code\":60008,\"message\":\"never echo fixture-password\"}"};
            return AccountHttp::Reply{401, "{\"code\":60003,\"mfa\":{\"ticket\":\"fixture-ticket\",\"methods\":[{\"type\":\"password\"}]}}"};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true;
        const auto start = [&service] { return service.execute("update-account", {}, {{"username", "new-fixture"}, {"currentPassword", "fixture-password"}}); };
        QVERIFY(start()); QTRY_VERIFY(!service.challengeMethods().isEmpty()); QVERIFY(service.m_pendingMfa); QCOMPARE(service.m_pendingMfa->body.value("password").toString(), QString("fixture-password"));
        QVERIFY(service.confirmMfa("password", "fixture-password")); QTRY_VERIFY(!service.mutating()); QVERIFY(service.m_pendingMfa); QVERIFY(!service.error().contains("fixture-password")); QCOMPARE(server.requests.size(), 2);
        service.cancelMfa(); QVERIFY(!service.m_pendingMfa); QVERIFY(service.challengeMethods().isEmpty()); QVERIFY(!service.confirmMfa("password", "fixture-password"));
        QVERIFY(start()); QTRY_VERIFY(!service.challengeMethods().isEmpty()); service.m_sensitiveTimer.setInterval(1); service.m_sensitiveTimer.start(); QTRY_VERIFY(service.challengeMethods().isEmpty()); QVERIFY(!service.m_pendingMfa);
        service.m_sensitiveTimer.setInterval(10 * 60 * 1000);
        QVERIFY(start()); QTRY_VERIFY(!service.challengeMethods().isEmpty()); client.m_currentUserId = "99"; emit client.stateChanged(); QVERIFY(!service.m_pendingMfa); QVERIFY(service.challengeMethods().isEmpty()); QVERIFY(!service.confirmMfa("password", "fixture-password"));
    }
    void malformedApprovalAndRepeatedChallengesCannotLoopOrReplayWithoutConsent() {
        AccountHttp server; QVERIFY(server.listen()); bool validApproval = false;
        server.handler = [&validApproval](const auto &request) {
            if (request.path == "/mfa/finish") return AccountHttp::Reply{200, validApproval ? QByteArray("{\"token\":\"fixture-one-use-approval\"}") : QByteArray("{\"token\":\"invalid\\nheader\"}")};
            return AccountHttp::Reply{401, "{\"code\":60003,\"mfa\":{\"ticket\":\"fixture-ticket\",\"methods\":[{\"type\":\"totp\"}]}}"};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true;
        QVERIFY(service.execute("update-account", {}, {{"globalName", "Generated value"}})); QTRY_VERIFY(!service.challengeMethods().isEmpty());
        QVERIFY(service.confirmMfa("totp", "123456")); QTRY_VERIFY(!service.mutating()); QCOMPARE(server.requests.size(), 2); QVERIFY(service.m_pendingMfa); QVERIFY(!service.error().isEmpty());
        validApproval = true; QVERIFY(service.confirmMfa("totp", "123456")); QTRY_VERIFY(!service.mutating()); QCOMPARE(server.requests.size(), 4); QVERIFY(!service.m_pendingMfa); QVERIFY(service.challengeMethods().isEmpty()); QVERIFY(!service.error().isEmpty());
        QTest::qWait(50); QCOMPARE(server.requests.size(), 4); QVERIFY(!service.confirmMfa("totp", "123456"));
    }
    void closingCanCancelApprovalButPreservesCommittedOriginalResponse() {
        AccountHttp server; QVERIFY(server.listen());
        server.handler = [](const auto &request) {
            if (request.method == "GET") return AccountHttp::Reply{200, "[]"};
            if (request.path == "/mfa/finish") return AccountHttp::Reply{200, "{\"token\":\"fixture-one-use-approval\"}", {}, 100};
            if (!request.mfaAuthorization.isEmpty()) return AccountHttp::Reply{200, json(self()), {}, 100};
            return AccountHttp::Reply{401, "{\"code\":60003,\"mfa\":{\"ticket\":\"fixture-ticket\",\"methods\":[{\"type\":\"password\"}]}}"};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true; QSignalSpy complete(&service, &AccountSettingsService::actionCompleted);
        QVERIFY(service.execute("update-account", {}, {{"globalName", "Generated value"}})); QTRY_VERIFY(!service.challengeMethods().isEmpty()); QVERIFY(service.confirmMfa("password", "fixture-password")); QTRY_COMPARE(server.requests.size(), 2); service.close(); QTest::qWait(150); QCOMPARE(server.requests.size(), 2); QVERIFY(service.account().isEmpty()); QVERIFY(service.challengeMethods().isEmpty()); QCOMPARE(complete.size(), 0);
        QVERIFY(service.execute("update-account", {}, {{"globalName", "Generated value"}})); QTRY_VERIFY(!service.challengeMethods().isEmpty()); QVERIFY(service.confirmMfa("password", "fixture-password")); QTRY_COMPARE(server.requests.size(), 5); QVERIFY(service.committed()); service.cancelMfa(); service.close(); QVERIFY(service.committed()); QVERIFY(!service.execute("mfa-cancel")); QTest::qWait(150); QVERIFY(!service.account().isEmpty()); QCOMPARE(complete.size(), 1); QCOMPARE(client.m_token, QByteArray("fixture-private-response-token"));
        QVERIFY(service.execute("update-account", {}, {{"globalName", "Generated value"}})); QTRY_VERIFY(!service.challengeMethods().isEmpty()); service.loadPage("connections"); QTRY_VERIFY(!service.busy()); QVERIFY(!service.confirmMfa("password", "fixture-password")); QVERIFY(service.challengeMethods().isEmpty());
    }
    void enrollmentCannotDiscardCommittedReplacementCredential() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) {
            if (request.method == "GET") { auto account = self(); account.insert("mfa_enabled", false); return AccountHttp::Reply{200, json(account)}; }
            return AccountHttp::Reply{200, "{\"token\":\"fixture-committed-mfa-session-000\",\"backup_codes\":[{\"code\":\"1234-5678\",\"consumed\":false}]}", {}, 100};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true; QSignalSpy rotated(&service, &AccountSettingsService::credentialRotated);
        service.loadPage("account"); QTRY_VERIFY(!service.busy()); QVERIFY(service.execute("mfa-begin")); QVERIFY(service.execute("mfa-confirm", {}, {{"code", "123456"}})); QTRY_COMPARE(server.requests.size(), 2); QVERIFY(service.committed());
        service.close(); QVERIFY(!service.execute("mfa-cancel")); service.cancelMfa(); QVERIFY(service.committed()); QVERIFY(service.mutating()); QTRY_VERIFY(!service.mutating()); QCOMPARE(rotated.size(), 1); QCOMPARE(client.m_token, QByteArray("fixture-committed-mfa-session-000")); QVERIFY(service.account().value("mfaEnabled").toBool()); QVERIFY(service.m_mfaSecret.isEmpty());
        service.close(); QVERIFY(service.account().isEmpty()); QVERIFY(service.details().isEmpty());
    }
    void backupChallengeNormalizesHyphensAndPasswordRemainsLiteral() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) {
            if (request.path == "/mfa/finish") return AccountHttp::Reply{400, "{\"code\":60008}"};
            return AccountHttp::Reply{401, "{\"code\":60003,\"mfa\":{\"ticket\":\"fixture-ticket\",\"methods\":[{\"type\":\"backup\"},{\"type\":\"password\"}]}}"};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true;
        QVERIFY(service.execute("update-account", {}, {{"globalName", "Generated"}})); QTRY_VERIFY(!service.challengeMethods().isEmpty());
        QVERIFY(service.confirmMfa("backup", "1234-5678")); QTRY_VERIFY(!service.mutating()); QCOMPARE(server.requests.last().body.value("data").toString(), QString("12345678"));
        QVERIFY(service.confirmMfa("password", " fixture-password - ")); QTRY_VERIFY(!service.mutating()); QCOMPARE(server.requests.last().body.value("data").toString(), QString(" fixture-password - ")); service.cancelMfa();
    }
    void deviceLogoutRequiresKnownCurrentSessionAndExcludesCurrentDevice() {
        AccountHttp server; QVERIFY(server.listen()); server.handler = [](const auto &request) {
            if (request.method == "POST") return AccountHttp::Reply{204, {}};
            return AccountHttp::Reply{200, "{\"user_sessions\":[{\"id_hash\":\"current-hash\",\"client_info\":{\"os\":\"Linux\",\"platform\":\"desktop\"}},{\"id_hash\":\"other-hash\",\"client_info\":{\"os\":\"Generated OS\",\"location\":\"Generated place\"}}]}"};
        };
        DiscordClient client; clientFixture(client, server); AccountSettingsService service(&client); service.m_fixtureMode = true; service.loadPage("sessions"); QTRY_VERIFY(!service.busy()); QCOMPARE(service.items().size(), 2); QVERIFY(!service.execute("logout-session", "other-hash"));
        client.m_authSessionIdHash = "current-hash"; service.refresh(); QTRY_VERIFY(!service.busy()); QCOMPARE(service.items()[0].toMap().value("status").toString(), QString("Current device")); QVERIFY(!service.execute("logout-session", "current-hash")); QVERIFY(!service.execute("logout-session", "not-loaded"));
        QVERIFY(service.execute("logout-session", "other-hash")); QTRY_VERIFY(!service.mutating()); QCOMPARE(server.requests.last().path, QString("/auth/sessions/logout")); QCOMPARE(server.requests.last().body.value("session_id_hashes").toArray(), QJsonArray{"other-hash"}); QCOMPARE(service.items().size(), 1);
    }
};
QTEST_GUILESS_MAIN(AccountSettingsServiceTest)
#include "account_settings_test.moc"
