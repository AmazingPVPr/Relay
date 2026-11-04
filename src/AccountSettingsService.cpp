#include "AccountSettingsService.h"
#include "DiscordClient.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>
#include <QtEndian>
#include <qrencode.h>
#include <algorithm>
#include <cmath>

namespace {
constexpr qsizetype MaximumResponse = 2 * 1024 * 1024, MaximumProto = 256 * 1024;
const QStringList emailCategories{"communication", "social", "tips", "recommendations_and_events", "updates_and_announcements", "family_center_digest"};
bool idValid(const QString &id) { static const QRegularExpression re("^[0-9]{1,22}$"); return re.match(id).hasMatch(); }
bool safeSecret(const QString &value, int maximum = 512) { return !value.isEmpty() && value.size() <= maximum && !value.contains(QRegularExpression("[\\x00-\\x20\\x7f]")); }
QString randomTotpSecret() {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    // 160 bits from the operating system RNG, as a standard RFC 4648 base32 key.
    QString result; result.reserve(32);
    for (int n = 0; n < 4; ++n) { const quint64 bits = QRandomGenerator::system()->generate64(); for (int j = 0; j < 8; ++j) result += QLatin1Char(alphabet[(bits >> (j * 5)) & 31]); }
    return result;
}
QString totpQr(const QString &uri) {
    const auto encoded = uri.toUtf8(); auto *qr = QRcode_encodeString(encoded.constData(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!qr || qr->width < 1 || qr->width > 177) { QRcode_free(qr); return {}; }
    const int size = qr->width + 8; QByteArray svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " + QByteArray::number(size) + " " + QByteArray::number(size) + "\"><path fill=\"white\" d=\"M0 0h" + QByteArray::number(size) + "v" + QByteArray::number(size) + "H0z\"/><path fill=\"black\" d=\"";
    for (int y = 0; y < qr->width; ++y) for (int x = 0; x < qr->width; ++x) if (qr->data[y * qr->width + x] & 1) svg += "M" + QByteArray::number(x + 4) + " " + QByteArray::number(y + 4) + "h1v1h-1z";
    svg += "\"/></svg>"; QRcode_free(qr); return "data:image/svg+xml;base64," + QString::fromLatin1(svg.toBase64());
}
QString clean(QString text, int limit = 200) { text.remove(QChar(0)); return text.left(limit); }
QVariantMap field(const QString &label, const QString &value) { return {{"label", label}, {"value", clean(value)}}; }
QString avatar(const QJsonObject &user) {
    const auto id = user.value("id").toString(), hash = user.value("avatar").toString();
    if (!idValid(id) || !QRegularExpression("^[A-Za-z0-9_]{1,128}$").match(hash).hasMatch()) return {};
    return QString("https://cdn.discordapp.com/avatars/%1/%2.png?size=128").arg(id, hash);
}
QString friendlyError(int status, const QJsonObject &body) {
    if (status == 401) return "This account session expired. Sign in again.";
    if (status == 403) return "Discord did not allow this account operation.";
    if (status == 404) return "This account feature or item is not available.";
    if (status == 429) return "Discord asked you to wait. Try again later; this action was not retried.";
    if (body.contains("captcha_key") || body.contains("captcha_sitekey")) return "Discord requires an account challenge. Complete it in the official client before trying again.";
    const int code = body.value("code").toInt();
    if (code == 60003 || body.contains("mfa")) return "Discord requires additional authentication for this operation.";
    if (code == 60008 || code == 60005) return "The authenticator code or setup key was not accepted.";
    if (status == 400) return "Discord did not accept these details. Check your entries and current password.";
    // Never echo an error body: validation responses can contain sensitive input.
    return status ? QString("The account request failed (%1). No automatic retry was made.").arg(status)
                  : QString("The request could not be confirmed. Check your connection before trying again.");
}
struct WireField { quint32 number = 0; int wire = 0; quint64 value = 0; QByteArray raw, payload; };
bool varint(const QByteArray &bytes, qsizetype &at, quint64 &value) {
    value = 0;
    for (int shift = 0; shift < 70 && at < bytes.size(); shift += 7) {
        const quint8 byte = quint8(bytes[at++]); if (shift == 63 && (byte & 0xfe)) return false;
        value |= quint64(byte & 0x7f) << shift; if (!(byte & 0x80)) return true;
    }
    return false;
}
QByteArray encodeVarint(quint64 value) { QByteArray result; do { quint8 byte = value & 0x7f; value >>= 7; result += char(byte | (value ? 0x80 : 0)); } while (value); return result; }
bool fields(const QByteArray &bytes, QList<WireField> &result) {
    if (bytes.size() > MaximumProto) return false;
    qsizetype at = 0;
    while (at < bytes.size()) {
        const auto start = at; quint64 key = 0; if (!varint(bytes, at, key) || key < 8 || key >> 3 > 0x1fffffff || result.size() >= 10000) return false;
        WireField field; field.number = quint32(key >> 3); field.wire = key & 7;
        if (field.wire == 0) { if (!varint(bytes, at, field.value)) return false; }
        else if (field.wire == 1 || field.wire == 5) { const int count = field.wire == 1 ? 8 : 4; if (bytes.size() - at < count) return false; field.payload = bytes.mid(at, count); at += count; }
        else if (field.wire == 2) { quint64 count = 0; if (!varint(bytes, at, count) || count > quint64(bytes.size() - at)) return false; field.payload = bytes.mid(at, qsizetype(count)); at += qsizetype(count); }
        else return false;
        field.raw = bytes.mid(start, at - start); result.append(field);
    }
    return true;
}
QByteArray message(const QByteArray &bytes, quint32 number, bool *present = nullptr) {
    QList<WireField> parsed; QByteArray result; if (present) *present = false; if (!fields(bytes, parsed)) return {};
    for (const auto &f : parsed) if (f.number == number && f.wire == 2) { result += f.payload; if (present) *present = true; }
    return result;
}
quint64 scalar(const QByteArray &bytes, quint32 number, quint64 fallback = 0) {
    QList<WireField> parsed; if (!fields(bytes, parsed)) return fallback;
    for (const auto &f : parsed) if (f.number == number && f.wire == 0) fallback = f.value;
    return fallback;
}
quint64 wrapped(const QByteArray &bytes, quint32 number, quint64 fallback = 0, bool *present = nullptr) {
    bool exists = false; const auto inner = message(bytes, number, &exists); if (present) *present = exists; return exists ? scalar(inner, 1) : fallback;
}
QByteArray lengthField(quint32 number, const QByteArray &data) { return encodeVarint((quint64(number) << 3) | 2) + encodeVarint(data.size()) + data; }
QByteArray replaceField(const QByteArray &bytes, quint32 number, const QByteArray &replacement) {
    QList<WireField> parsed; if (!fields(bytes, parsed)) return {};
    QByteArray result; for (const auto &f : parsed) if (f.number != number) result += f.raw;
    return result + replacement;
}
QByteArray wrapperField(quint32 number, quint64 value) { return lengthField(number, QByteArray(1, char(8)) + encodeVarint(value)); }
}

AccountSettingsService::AccountSettingsService(DiscordClient *client, QObject *parent) : QObject(parent), m_client(client) {
    m_sensitiveTimer.setSingleShot(true); m_sensitiveTimer.setInterval(10 * 60 * 1000);
    connect(&m_sensitiveTimer, &QTimer::timeout, this, [this] {
        // A submitted write can already have rotated the session remotely.
        // Keep its bounded response handler alive so the replacement is saved.
        if (!committed() && (m_pendingMfa || m_mfaRetrying || m_activeAction.startsWith("mfa-"))) cancelRequests();
        clearSensitive(); emit changed();
    });
    if (!client) return;
    m_session = client->m_generation; m_userId = client->currentUserId();
    connect(client, &DiscordClient::stateChanged, this, [this] {
        if (m_client && (m_session != m_client->m_generation || m_userId != m_client->currentUserId())) {
            reset(); m_session = m_client->m_generation; m_userId = m_client->currentUserId();
        }
    });
    connect(client, &QObject::destroyed, this, [this] { m_client = nullptr; reset(); });
}
AccountSettingsService::~AccountSettingsService() { cancelRequests(); clearSensitive(); }
bool AccountSettingsService::ready() const { return m_client && !m_client->demoMode() && !m_client->m_token.isEmpty() && idValid(m_client->currentUserId()); }
bool AccountSettingsService::fail(const QString &reason) { m_error = reason; emit changed(); return false; }
void AccountSettingsService::clearError() { m_error.clear(); emit changed(); }
void AccountSettingsService::cancelRequests() {
    ++m_operation; const auto replies = m_replies; m_replies.clear();
    for (auto *reply : replies) { reply->disconnect(this); reply->abort(); reply->deleteLater(); }
    m_reads = 0; m_mutating = false; m_mfaRetrying = false; m_activeAction.clear();
}
void AccountSettingsService::clearSensitive() {
    m_sensitiveTimer.stop();
    if (m_pendingMfa) { m_pendingMfa->body = {}; m_pendingMfa->ticket.fill(QChar(0)); m_pendingMfa.reset(); } m_challengeMethods.clear(); m_mfaRetrying = false;
    for (auto *value : {&m_mfaSecret, &m_backupNonce, &m_regenerateNonce}) { value->fill(QChar(0)); value->clear(); }
    for (const auto &key : {"mfaSecret", "mfaUri", "mfaQrImage", "backupCodes", "backupStage"}) m_details.remove(key);
}
void AccountSettingsService::reset() {
    cancelRequests(); clearSensitive(); m_account.clear(); m_details.clear(); m_items.clear(); m_targets.clear();
    m_preferences.fill('\0'); m_preferences.clear(); m_preferencesReady = false; m_error.clear(); m_status.clear(); emit changed();
}
void AccountSettingsService::close() { if (committed()) { fail("Wait for the current account change to finish before closing settings."); return; } reset(); }
QVariantMap AccountSettingsService::capabilities() const {
    const bool available = ready();
    const bool accountLoaded = available && m_account.value("id").toString() == m_userId;
    return {{"updateAccount", available}, {"phone", false}, {"mfaEnroll", accountLoaded && !m_account.value("mfaEnabled").toBool()}, {"mfaDisable", accountLoaded && m_account.value("mfaEnabled").toBool()}, {"backupCodes", accountLoaded && m_account.value("mfaEnabled").toBool()},
        {"preferences", available && m_preferencesReady}, {"emailPreferences", available && m_page == "email" && m_details.contains("categories")},
        {"revokeApps", available}, {"disconnectConnections", available}, {"unblockUsers", available}, {"purchases", false}};
}
void AccountSettingsService::send(const QByteArray &method, const QString &path, QJsonObject body, bool mutation, Handler callback, const QByteArray &mfaAuthorization, bool allowChallenge) {
    if (!ready()) { callback({}, "Sign in before opening account settings."); return; }
    if (m_replies.size() >= 4 || !path.startsWith('/') || path.startsWith("//") || path.contains("..")) { callback({}, "This account request is not available."); return; }
    const QUrl base(m_client->m_apiBase), url(m_client->m_apiBase + path);
    const bool official = base.scheme() == "https" && base.host() == "discord.com" && base.port() == -1;
    const bool fixture = m_fixtureMode && base.scheme() == "http" && base.host() == "127.0.0.1" && base.port() > 0;
    if ((!official && !fixture) || !url.userInfo().isEmpty() || url.hasFragment()) { callback({}, "The account API endpoint is not trusted."); return; }
    QNetworkRequest request(url); request.setRawHeader("Authorization", m_client->m_token);
    if (!mfaAuthorization.isEmpty()) request.setRawHeader("X-Discord-MFA-Authorization", mfaAuthorization);
    request.setRawHeader("User-Agent", "Relay/0.1 (native Qt client)"); request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy); request.setTransferTimeout(20000);
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto *reply = method == "GET" ? m_network.get(request) : m_network.sendCustomRequest(request, method, payload);
    payload.fill('\0'); payload.clear(); m_replies.insert(reply); if (mutation) m_mutating = true; else ++m_reads;
    const auto operation = m_operation, session = m_session; const auto user = m_userId, action = m_activeAction;
    reply->setReadBufferSize(MaximumResponse + 1);
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) { if (received > MaximumResponse || total > MaximumResponse) { reply->setProperty("tooLarge", true); reply->abort(); } });
    connect(reply, &QNetworkReply::finished, this, [this, reply, operation, session, user, mutation, method, path, body = std::move(body), action, allowChallenge, callback = std::move(callback)]() mutable {
        m_replies.remove(reply); reply->deleteLater();
        if (operation != m_operation || !m_client || session != m_client->m_generation || user != m_client->currentUserId()) return;
        if (mutation) { m_mutating = false; m_activeAction.clear(); } else m_reads = std::max(0, m_reads - 1);
        QByteArray raw = reply->read(MaximumResponse + 1); const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (raw.size() > MaximumResponse || reply->property("tooLarge").toBool()) { raw.fill('\0'); callback({}, "Discord returned too much account data to load safely."); emit changed(); return; }
        QJsonParseError parse; const auto document = QJsonDocument::fromJson(raw, &parse); raw.fill('\0'); raw.clear();
        const auto response = document.object();
        if (allowChallenge && mutation && status == 401 && response.value("code").toInt() == 60003 && response.value("mfa").isObject()) {
            const auto challenge = response.value("mfa").toObject(); const auto ticket = challenge.value("ticket").toString(); QStringList methods;
            for (const auto &entry : challenge.value("methods").toArray()) { const auto type = entry.toObject().value("type").toString(); if ((type == "totp" || type == "backup" || type == "password") && !methods.contains(type)) methods.append(type); }
            if (safeSecret(ticket, 4096) && !methods.isEmpty()) {
                m_pendingMfa = std::make_unique<PendingMfa>(PendingMfa{method, path, ticket, action, std::move(body), std::move(callback)});
                m_challengeMethods = methods; m_error.clear(); m_status = "Additional authentication is required."; m_sensitiveTimer.start(); emit changed(); return;
            }
        }
        body = {};
        if (status >= 200 && status < 300 && reply->error() == QNetworkReply::NoError && (status == 204 || parse.error == QJsonParseError::NoError))
            callback(document.isArray() ? QJsonValue(document.array()) : QJsonValue(document.object()), {});
        else callback({}, friendlyError(status, document.object()));
        emit changed();
    });
    emit changed();
}
void AccountSettingsService::cancelMfa() {
    if (committed()) { fail("Wait for the current account change to finish before closing authentication."); return; }
    if (m_pendingMfa || m_mfaRetrying) cancelRequests();
    clearSensitive(); m_error.clear(); m_status.clear(); emit changed();
}
bool AccountSettingsService::confirmMfa(const QString &method, const QString &input) {
    if (!ready() || !m_pendingMfa || !m_challengeMethods.contains(method) || m_mutating || busy()) return fail("This authentication challenge is no longer available.");
    QString data = input; if (method == "totp") data.remove(' '); else if (method == "backup") data.remove('-');
    if (data.isEmpty() || data.size() > 1024 || data.contains(QChar(0)) || (method == "totp" && !QRegularExpression("^[0-9]{6}$").match(data).hasMatch())) return fail("Enter a valid authentication code or password.");
    m_error.clear(); m_status = "Verifying…"; m_activeAction = "mfa-finish";
    send("POST", "/mfa/finish", {{"ticket", m_pendingMfa->ticket}, {"mfa_type", method}, {"data", data}}, true, [this](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) { m_error = error; m_status.clear(); return; }
        const auto authorization = value.toObject().value("token").toString();
        if (!m_pendingMfa || !safeSecret(authorization, 4096)) { fail("Discord did not return a readable authentication approval. Try the operation again."); m_status.clear(); return; }
        auto pending = std::move(m_pendingMfa); m_mfaRetrying = true; m_activeAction = pending->action; m_error.clear(); m_status = "Saving…"; m_sensitiveTimer.start();
        send(pending->method, pending->path, std::move(pending->body), true, [this, callback = std::move(pending->callback)](const QJsonValue &result, const QString &retryError) {
            m_mfaRetrying = false; m_challengeMethods.clear(); callback(result, retryError);
        }, authorization.toUtf8(), false);
    }, {}, false);
    return true;
}
bool AccountSettingsService::acceptAccount(const QJsonObject &object) {
    if (!m_client || object.value("id").toString() != m_userId) return fail("Discord returned account details for a different user.");
    QVariantMap result{{"id", m_userId}, {"username", clean(object.value("username").toString(), 80)}, {"globalName", clean(object.value("global_name").toString(), 80)},
        {"email", clean(object.value("email").toString(), 320)}, {"phone", clean(object.value("phone").toString(), 80)}, {"mfaEnabled", object.value("mfa_enabled").toBool()},
        {"smsEnabled", object.value("sms_enabled").toBool()}, {"verified", object.value("verified").toBool()}, {"premiumType", object.value("premium_type").toInt()}, {"avatarUrl", avatar(object)}};
    m_account = result;
    auto user = m_client->m_users.value(m_userId); for (const auto &key : {"username", "global_name", "avatar", "discriminator"}) if (object.contains(key)) user.insert(key, object.value(key));
    user.insert("id", m_userId); m_client->updateUser(user); return true;
}
bool AccountSettingsService::rotateCredential(const QJsonObject &response, bool required) {
    const auto token = response.value("token").toString();
    if (token.isEmpty() && !required) return true;
    if (token.size() < 20 || !safeSecret(token)) return fail("The account changed, but Discord returned an invalid replacement session. Sign in again.");
    m_client->m_token.fill('\0'); m_client->m_token = token.toUtf8();
    // Until the gateway identifies the replacement session, logging out a
    // device from a cached old session hash could log out this device itself.
    m_client->m_authSessionIdHash.clear(); emit credentialRotated(token); return true;
}
bool AccountSettingsService::acceptBackupCodes(const QJsonObject &response) {
    if (!response.value("backup_codes").isArray() || response.value("backup_codes").toArray().size() > 20) return fail("Discord did not return readable recovery codes. Request them again.");
    QVariantList codes;
    for (const auto &entry : response.value("backup_codes").toArray()) {
        const auto object = entry.toObject(); const auto code = object.value("code");
        if (!code.isString() || !safeSecret(code.toString(), 32) || !object.value("consumed").isBool()) return fail("Discord did not return readable recovery codes. Request them again.");
        codes.append(QVariantMap{{"code", code.toString()}, {"consumed", object.value("consumed").toBool()}});
    }
    m_details.insert("backupCodes", codes); m_details.insert("backupStage", "codes"); m_sensitiveTimer.start(); return true;
}
bool AccountSettingsService::acceptPreferences(const QJsonObject &response) {
    if (!response.value("settings").isString() || response.value("settings").toString().size() > MaximumProto * 2) return fail("Discord returned unreadable messaging preferences.");
    const auto decoded = QByteArray::fromBase64Encoding(response.value("settings").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    QList<WireField> parsed; if (!decoded || !fields(decoded.decoded, parsed)) return fail("Discord returned unreadable messaging preferences.");
    const auto privacy = message(decoded.decoded, 8); QList<WireField> privacyFields; if (!fields(privacy, privacyFields)) return fail("Discord returned unreadable privacy preferences.");
    m_preferences = privacy; m_preferencesReady = true; const auto flags = wrapped(privacy, 11, 14);
    QVariantMap prefs{{"defaultDmsAllowed", !wrapped(privacy, 27, scalar(privacy, 4))}, {"friendRequestsEveryone", bool(flags & 8)}, {"friendRequestsMutualFriends", bool(flags & 2)},
        {"friendRequestsMutualServers", bool(flags & 4)}, {"activityJoinFriends", bool(wrapped(privacy, 1, 1))}, {"activityJoinVoice", bool(wrapped(privacy, 2, 1))}, {"contactSyncEnabled", bool(wrapped(privacy, 10))}};
    bool known = false; const auto requests = wrapped(privacy, 17, 0, &known); if (known) prefs.insert("messageRequestsAllowed", !requests);
    const auto training = wrapped(privacy, 19, 0, &known); if (known) prefs.insert("nonSpamRetrainingOptIn", bool(training));
    m_details.insert("preferences", prefs); return true;
}
bool AccountSettingsService::preparePreferences(const QVariantMap &values, QJsonObject &body) {
    if (!m_preferencesReady || values.isEmpty() || values.size() > 10) return fail("Load messaging preferences before saving changes.");
    static const QHash<QString, int> booleans{{"defaultDmsAllowed", 27}, {"messageRequestsAllowed", 17}, {"activityJoinFriends", 1}, {"activityJoinVoice", 2}, {"contactSyncEnabled", 10}, {"nonSpamRetrainingOptIn", 19}};
    static const QHash<QString, int> bits{{"friendRequestsEveryone", 8}, {"friendRequestsMutualFriends", 2}, {"friendRequestsMutualServers", 4}};
    auto privacy = m_preferences; quint64 flags = wrapped(privacy, 11, 14); bool flagsChanged = false;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.value().metaType().id() != QMetaType::Bool || (!booleans.contains(it.key()) && !bits.contains(it.key()))) return fail("This messaging preference cannot be changed here.");
        if (bits.contains(it.key())) { const quint64 bit = bits.value(it.key()); flags = it.value().toBool() ? flags | bit : flags & ~bit; flagsChanged = true; continue; }
        const bool invert = it.key() == "defaultDmsAllowed" || it.key() == "messageRequestsAllowed";
        const bool value = it.value().toBool() != invert; const auto number = booleans.value(it.key());
        privacy = replaceField(privacy, number, wrapperField(number, value));
        if (number == 27) privacy = replaceField(privacy, 4, encodeVarint(4 << 3) + encodeVarint(value));
    }
    if (flagsChanged) privacy = replaceField(privacy, 11, wrapperField(11, flags));
    // Send only the privacy subtree. Retain unknown fields inside that subtree;
    // other settings categories are never serialized or overwritten.
    body = {{"settings", QString::fromLatin1(lengthField(8, privacy).toBase64())}}; return true;
}
bool AccountSettingsService::acceptEmailPreferences(const QJsonObject &response) {
    if (!response.value("categories").isObject() || !response.value("initialized").isBool()) return fail("Discord returned unreadable email preferences.");
    QVariantMap categories;
    for (const auto &name : emailCategories) if (response.value("categories").toObject().value(name).isBool()) categories.insert(name, response.value("categories").toObject().value(name).toBool());
    m_details.insert("categories", categories); m_details.insert("initialized", response.value("initialized").toBool()); return true;
}
void AccountSettingsService::acceptItems(const QString &page, const QJsonValue &value) {
    if ((page == "family" && !value.toObject().value("linked_users").isArray()) ||
        (page == "sessions" && !value.toObject().value("user_sessions").isArray()) ||
        (page != "family" && page != "sessions" && !value.isArray())) {
        fail("Discord returned an unreadable account list."); return;
    }
    QJsonArray array = value.toArray();
    if (page == "family") array = value.toObject().value("linked_users").toArray();
    if (page == "sessions") array = value.toObject().value("user_sessions").toArray();
    m_items.clear(); m_targets.clear();
    QHash<QString, QJsonObject> familyUsers;
    if (page == "family") for (const auto &entry : value.toObject().value("users").toArray()) { const auto user = entry.toObject(); if (familyUsers.size() < 200) familyUsers.insert(user.value("id").toString(), user); }
    for (const auto &entry : array) {
        if (m_items.size() >= 250) break;
        if (!entry.isObject()) continue;
        const auto object = entry.toObject(); QString id = object.value("id").toString(), title, subtitle, status, action, actionLabel, icon; QVariantList extra;
        QJsonObject target;
        if (page == "apps") {
            const auto app = object.value("application").toObject(); if (!idValid(id) || !idValid(app.value("id").toString())) continue;
            title = app.value("name").toString(); subtitle = app.value("description").toString(); action = "revoke-app"; actionLabel = "Deauthorize"; target = {{"id", id}};
            QStringList scopes; for (const auto &scope : object.value("scopes").toArray()) if (scopes.size() < 30) scopes.append(clean(scope.toString(), 80)); extra.append(field("Permissions", scopes.join(", ")));
        } else if (page == "connections") {
            const auto type = object.value("type").toString(); if (id.isEmpty() || id.size() > 128 || !QRegularExpression("^[A-Za-z0-9_-]{1,40}$").match(type).hasMatch()) continue;
            target = {{"id", id}, {"type", type}}; id = type + ":" + id; title = object.value("name").toString(); subtitle = type;
            status = object.value("verified").toBool() ? "Verified" : QString{}; action = "disconnect-connection"; actionLabel = "Disconnect";
        } else if (page == "messaging" || page == "privacy") {
            const bool blocked = object.value("type").toInt() == 2, ignored = object.value("user_ignored").toBool(); if (!blocked && !ignored) continue;
            auto user = object.value("user").toObject(); if (id.isEmpty()) id = user.value("id").toString(); if (!idValid(id)) continue;
            title = user.value("global_name").toString(user.value("username").toString()); icon = avatar(user); status = blocked ? "Blocked" : "Ignored";
            action = blocked ? "unblock-user" : "unignore-user"; actionLabel = blocked ? "Unblock" : "Stop ignoring"; target = {{"id", id}};
        } else if (page == "family") {
            id = object.value("user_id").toString(); if (!idValid(id)) continue; const auto user = familyUsers.value(id);
            title = user.value("global_name").toString(user.value("username").toString()); if (title.isEmpty()) title = "Linked account"; icon = avatar(user);
            extra.append(field("Link status", QString::number(object.value("link_status").toInt())));
        } else if (page == "sessions") {
            id = object.value("id_hash").toString(); if (id.isEmpty() || id.size() > 128) continue; const auto client = object.value("client_info").toObject();
            title = client.value("os").toString(); subtitle = client.value("platform").toString(); extra.append(field("Location", client.value("location").toString())); extra.append(field("Last used", object.value("approx_last_used_time").toString()));
            if (m_client && !m_client->m_authSessionIdHash.isEmpty()) {
                if (id == m_client->m_authSessionIdHash) status = "Current device";
                else if (safeSecret(id, 128)) { action = "logout-session"; actionLabel = "Log out device"; target = {{"id", id}}; }
            }
        } else if (page == "billing") {
            if (!idValid(id)) continue;
            title = object.value("brand").toString(); if (title.isEmpty()) title = "Saved payment method";
            const auto last4 = object.value("last_4").toString(); if (QRegularExpression("^[0-9]{4}$").match(last4).hasMatch()) subtitle = "Ending in " + last4;
            status = object.value("default").toBool() ? "Default" : QString{};
        } else if (page == "subscriptions" || page == "nitro") {
            if (!idValid(id)) continue;
            title = "Subscription"; status = object.value("status").toVariant().toString();
            extra.append(field("Current period ends", object.value("current_period_end").toString()));
            if (object.value("cancel_at_period_end").isBool()) extra.append(field("Renewal", object.value("cancel_at_period_end").toBool() ? "Ends this period" : "Automatic"));
        } else if (page == "boosts") {
            if (!idValid(id)) continue;
            title = "Server Boost"; const auto applied = object.value("premium_guild_subscription").toObject();
            const auto guild = applied.value("guild_id").toString(); status = guild.isEmpty() ? "Available" : "Applied"; if (!guild.isEmpty()) extra.append(field("Server ID", guild));
            extra.append(field("Cooldown ends", object.value("cooldown_ends_at").toString()));
        } else if (page == "gifts") {
            if (!idValid(id)) continue;
            title = "Gift entitlement"; subtitle = object.value("sku_id").toString(); status = object.value("consumed").toBool() ? "Used" : "Available"; extra.append(field("Expires", object.value("ends_at").toString()));
        }
        if (id.isEmpty()) continue;
        m_items.append(QVariantMap{{"id", clean(id, 180)}, {"title", clean(title.isEmpty() ? "Account item" : title, 120)}, {"subtitle", clean(subtitle, 300)}, {"status", clean(status, 80)},
            {"iconUrl", icon}, {"fields", extra}, {"action", action}, {"actionLabel", actionLabel}});
        if (!action.isEmpty()) m_targets.insert(id, target);
    }
}
void AccountSettingsService::loadPage(const QString &page) {
    static const QHash<QString, QString> paths{{"account", "/users/@me"}, {"billing", "/users/@me/billing/payment-sources"}, {"subscriptions", "/users/@me/billing/subscriptions"},
        {"nitro", "/users/@me/billing/subscriptions"}, {"boosts", "/users/@me/guilds/premium/subscription-slots"}, {"gifts", "/users/@me/entitlements/gifts"},
        {"apps", "/oauth2/tokens"}, {"connections", "/users/@me/connections"}, {"family", "/users/@me/linked-users"}, {"sessions", "/auth/sessions"}, {"email", "/users/@me/email-settings"}};
    if (m_mutating) { fail("Wait for the current account change to finish."); return; }
    if (!paths.contains(page) && page != "messaging" && page != "privacy") { fail("This account settings page is not available."); return; }
    cancelRequests(); clearSensitive(); m_page = page; m_items.clear(); m_targets.clear(); m_details.clear(); m_error.clear(); m_status = "Loading…"; m_preferencesReady = false;
    if (!ready()) { fail("Sign in to load your Discord account settings."); m_status.clear(); return; }
    m_session = m_client->m_generation; m_userId = m_client->currentUserId();
    if (page == "messaging" || page == "privacy") {
        send("GET", "/users/@me/settings-proto/1", {}, false, [this](const QJsonValue &value, const QString &error) { if (!error.isEmpty()) m_error = error; else acceptPreferences(value.toObject()); m_status.clear(); });
        send("GET", "/users/@me/relationships", {}, false, [this, page](const QJsonValue &value, const QString &error) { if (!error.isEmpty()) m_error = error; else acceptItems(page, value); m_status.clear(); });
    } else send("GET", paths.value(page), {}, false, [this, page](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) m_error = error; else if (page == "account") acceptAccount(value.toObject()); else if (page == "email") acceptEmailPreferences(value.toObject()); else acceptItems(page, value);
        if (page == "nitro") m_details.insert("premiumType", m_account.value("premiumType"));
        m_status.clear();
    });
}
void AccountSettingsService::refresh() { loadPage(m_page); }
bool AccountSettingsService::execute(const QString &action, const QString &targetId, const QVariantMap &values) {
    if (action == "mfa-cancel") {
        if (committed()) return fail("Wait for the current account change to finish before closing authentication.");
        if (m_pendingMfa || m_mfaRetrying || m_activeAction.startsWith("mfa-")) cancelRequests();
        clearSensitive(); m_status.clear(); m_error.clear(); emit changed(); return true;
    }
    if (!ready()) return fail("Sign in before changing account settings.");
    if (busy() || m_mutating) return fail("Wait for the current account request to finish.");
    if (m_pendingMfa) return fail("Complete or cancel the current authentication challenge first.");
    QJsonObject body; QByteArray method = "POST"; QString path;
    if (action == "mfa-begin") {
        if (!capabilities().value("mfaEnroll").toBool() || !targetId.isEmpty() || !values.isEmpty()) return fail("Load your account before setting up an authenticator.");
        clearSensitive(); m_mfaSecret = randomTotpSecret(); QUrl uri; uri.setScheme("otpauth"); uri.setHost("totp"); uri.setPath("/Discord:" + m_account.value("email").toString());
        QUrlQuery query; query.addQueryItem("secret", m_mfaSecret); query.addQueryItem("issuer", "Discord"); uri.setQuery(query);
        const auto encoded = uri.toString(QUrl::FullyEncoded); m_details.insert("mfaSecret", m_mfaSecret); m_details.insert("mfaUri", encoded); m_details.insert("mfaQrImage", totpQr(encoded));
        m_error.clear(); m_status.clear(); m_sensitiveTimer.start(); emit changed(); return true;
    } else if (action == "mfa-confirm") {
        if (!capabilities().value("mfaEnroll").toBool() || m_mfaSecret.isEmpty() || !targetId.isEmpty() || values.size() != 1 || !values.contains("code") || values.value("code").metaType().id() != QMetaType::QString) return fail("Begin authenticator setup before confirming it.");
        auto code = values.value("code").toString(); code.remove(' ');
        if (!QRegularExpression("^[0-9]{6}$").match(code).hasMatch()) return fail("Enter the six-digit code from your authenticator.");
        path = "/users/@me/mfa/totp/enable"; body = {{"code", code}, {"secret", m_mfaSecret}};
    } else if (action == "mfa-disable") {
        if (!capabilities().value("mfaDisable").toBool() || !targetId.isEmpty() || !values.isEmpty()) return fail("Load your account before disabling its authenticator.");
        path = "/users/@me/mfa/totp/disable";
    } else if (action == "mfa-backup-send") {
        if (!capabilities().value("backupCodes").toBool() || !targetId.isEmpty() || values.size() != 1 || values.value("currentPassword").metaType().id() != QMetaType::QString || values.value("currentPassword").toString().isEmpty() || values.value("currentPassword").toString().size() > 1024) return fail("Enter your current password to request recovery codes.");
        clearSensitive(); path = "/auth/verify/view-backup-codes-challenge"; body = {{"password", values.value("currentPassword").toString()}};
    } else if (action == "mfa-backup-confirm") {
        if (!capabilities().value("backupCodes").toBool() || !targetId.isEmpty() || values.size() != 2 || values.value("key").metaType().id() != QMetaType::QString || values.value("regenerate").metaType().id() != QMetaType::Bool || !safeSecret(values.value("key").toString(), 256)) return fail("Enter the verification key from your recovery-code email.");
        const bool regenerate = values.value("regenerate").toBool(); const auto nonce = regenerate ? m_regenerateNonce : m_backupNonce;
        if (nonce.isEmpty()) return fail("Request a fresh recovery-code verification email first.");
        path = "/users/@me/mfa/codes-verification"; body = {{"key", values.value("key").toString()}, {"nonce", nonce}, {"regenerate", regenerate}};
    } else if (action == "update-account") {
        static const QStringList allowed{"username", "globalName", "email", "currentPassword", "newPassword"};
        for (auto it = values.cbegin(); it != values.cend(); ++it) if (!allowed.contains(it.key()) || it.value().metaType().id() != QMetaType::QString || it.value().toString().size() > 1024) return fail("This account field cannot be changed here.");
        if (values.contains("username")) { const auto name = values.value("username").toString().trimmed(); if (name.size() < 2 || name.size() > 32 || name.contains(QChar(0))) return fail("Enter a username between 2 and 32 characters."); body.insert("username", name); }
        if (values.contains("globalName")) { const auto name = values.value("globalName").toString().trimmed(); if (name.size() > 32 || name.contains(QChar(0))) return fail("Display names can contain at most 32 characters."); body.insert("global_name", name.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(name)); }
        if (values.contains("email")) { const auto email = values.value("email").toString().trimmed(); if (email.size() > 320 || !QRegularExpression("^[^\\s@]+@[^\\s@]+\\.[^\\s@]+$").match(email).hasMatch()) return fail("Enter a valid email address."); body.insert("email", email); }
        if (values.contains("newPassword")) { const auto password = values.value("newPassword").toString(); if (password.size() < 8 || password.size() > 256) return fail("New passwords must contain between 8 and 256 characters."); body.insert("new_password", password); }
        if (body.isEmpty()) return fail("Choose an account detail to update.");
        const auto password = values.value("currentPassword").toString(); if ((body.contains("username") || body.contains("email") || body.contains("new_password")) && password.isEmpty()) return fail("Enter your current password to change these details.");
        if (!password.isEmpty()) body.insert("password", password);
        method = "PATCH"; path = "/users/@me";
    } else if (action == "update-preferences") {
        if (!preparePreferences(values, body)) return false;
        method = "PATCH"; path = "/users/@me/settings-proto/1";
    } else if (action == "update-email-preferences") {
        if (m_page != "email" || !m_details.contains("categories") || values.isEmpty() || values.size() > emailCategories.size()) return fail("Load email preferences before changing them.");
        QJsonObject categories;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            if (!emailCategories.contains(it.key()) || it.value().metaType().id() != QMetaType::Bool) return fail("This email category cannot be changed here.");
            categories.insert(it.key(), it.value().toBool());
        }
        method = "PATCH"; path = "/users/@me/email-settings"; body = {{"settings", QJsonObject{{"categories", categories}}}};
    } else {
        const auto target = m_targets.value(targetId); if (target.isEmpty() || !values.isEmpty()) return fail("Refresh this list before changing the selected item."); method = "DELETE";
        if (action == "logout-session" && m_page == "sessions") {
            const auto id = target.value("id").toString();
            if (m_client->m_authSessionIdHash.isEmpty() || id == m_client->m_authSessionIdHash) return fail("The current device cannot be logged out from this list.");
            method = "POST"; path = "/auth/sessions/logout"; body = {{"session_id_hashes", QJsonArray{id}}};
        } else if (action == "revoke-app" && m_page == "apps") path = "/oauth2/tokens/" + target.value("id").toString();
        else if (action == "disconnect-connection" && m_page == "connections") path = "/users/@me/connections/" + target.value("type").toString() + "/" + QString::fromLatin1(QUrl::toPercentEncoding(target.value("id").toString()));
        else if ((action == "unblock-user" || action == "unignore-user") && (m_page == "messaging" || m_page == "privacy")) {
            const auto row = std::find_if(m_items.cbegin(), m_items.cend(), [&targetId](const QVariant &row) { return row.toMap().value("id").toString() == targetId; });
            if (row == m_items.cend() || row->toMap().value("action").toString() != action) return fail("This relationship action is no longer available.");
            path = "/users/@me/relationships/" + targetId + (action == "unignore-user" ? "/ignore" : QString{});
        } else return fail("This account action is not available.");
    }
    m_error.clear(); m_status = "Saving…"; m_activeAction = action;
    if (action.startsWith("mfa-")) m_sensitiveTimer.start();
    send(method, path, std::move(body), true, [this, action, targetId](const QJsonValue &value, const QString &error) {
        m_activeAction.clear();
        if (!error.isEmpty()) { m_error = error; m_status.clear(); return; }
        if (action == "mfa-confirm") {
            clearSensitive(); m_account.insert("mfaEnabled", true);
            if (!rotateCredential(value.toObject(), true) || !acceptBackupCodes(value.toObject())) { m_status.clear(); return; }
        } else if (action == "mfa-disable") {
            clearSensitive(); m_account.insert("mfaEnabled", false); m_account.insert("smsEnabled", false);
            if (!rotateCredential(value.toObject(), true)) { m_status.clear(); return; }
        } else if (action == "mfa-backup-send") {
            const auto object = value.toObject(); const auto nonce = object.value("nonce").toString(), regenerate = object.value("regenerate_nonce").toString();
            if (!safeSecret(nonce) || !safeSecret(regenerate)) { fail("Discord did not return a readable recovery-code challenge. Request a new email."); m_status.clear(); return; }
            m_backupNonce = nonce; m_regenerateNonce = regenerate; m_details.insert("backupStage", "email"); m_sensitiveTimer.start();
        } else if (action == "mfa-backup-confirm") {
            if (!acceptBackupCodes(value.toObject())) { m_status.clear(); return; }
            m_backupNonce.fill(QChar(0)); m_backupNonce.clear(); m_regenerateNonce.fill(QChar(0)); m_regenerateNonce.clear();
        } else if (action == "update-account") {
            auto object = value.toObject(); if (!acceptAccount(object)) { m_status.clear(); return; }
            if (!rotateCredential(object)) { m_status.clear(); return; }
        } else if (action == "update-preferences") {
            const auto response = value.toObject(); if (response.value("out_of_date").toBool()) { m_preferencesReady = false; fail("Preferences changed elsewhere. Refresh before saving again."); m_status.clear(); return; }
            if (!acceptPreferences(response)) { m_status.clear(); return; }
        } else if (action == "update-email-preferences") {
            if (!acceptEmailPreferences(value.toObject())) { m_status.clear(); return; }
        } else {
            for (int at = m_items.size() - 1; at >= 0; --at) if (m_items[at].toMap().value("id").toString() == targetId) m_items.removeAt(at);
            m_targets.remove(targetId);
        }
        m_status = "Saved."; emit actionCompleted(action);
    });
    return true;
}
