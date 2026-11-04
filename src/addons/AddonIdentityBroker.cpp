#include "AddonIdentityBroker.h"
#include "DiscordClient.h"
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QRegularExpression>

AddonIdentityBroker::AddonIdentityBroker(DiscordClient *client, const QString &clientId, const QUrl &redirectUri, QObject *parent)
    : QObject(parent), m_client(client), m_clientId(clientId), m_redirect(redirectUri) {
    m_network.setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
    connect(client, &DiscordClient::stateChanged, this, &AddonIdentityBroker::changed);
}
AddonIdentityBroker::~AddonIdentityBroker() { cancel(); }
QString AddonIdentityBroker::accountId() const { return available() ? m_client->currentUserId() : QString{}; }
bool AddonIdentityBroker::available() const {
    return m_client && !m_client->m_demoMode && !m_client->m_token.isEmpty() && !m_client->currentUserId().isEmpty()
        && QRegularExpression("^[0-9]{15,22}$").match(m_clientId).hasMatch()
        && m_redirect.scheme() == "https" && !m_redirect.host().isEmpty() && m_redirect.userInfo().isEmpty()
        && !m_redirect.hasQuery() && !m_redirect.hasFragment() && m_redirect.port(443) == 443;
}
QString AddonIdentityBroker::accountName() const { return available() ? m_client->currentUserName() : QString{}; }
bool AddonIdentityBroker::validate(const QUrl &url, QString *state) const {
    if (!available() || url.scheme() != "https" || url.host() != "discord.com" || url.port(443) != 443 ||
        !url.userInfo().isEmpty() || url.hasFragment() || (url.path() != "/oauth2/authorize" && url.path() != "/api/oauth2/authorize")) return false;
    const QUrlQuery query(url);
    const QStringList allowed{"client_id", "redirect_uri", "response_type", "scope", "state", "prompt"};
    for (const auto &item : query.queryItems(QUrl::FullyDecoded)) {
        if (!allowed.contains(item.first) || query.allQueryItemValues(item.first).size() != 1) return false;
    }
    *state = query.queryItemValue("state", QUrl::FullyDecoded);
    return query.queryItemValue("client_id") == m_clientId && query.queryItemValue("scope") == "identify"
        && query.queryItemValue("response_type") == "code" && query.queryItemValue("redirect_uri", QUrl::FullyDecoded) == m_redirect.toString()
        && QRegularExpression("^[A-Za-z0-9_-]{20,200}$").match(*state).hasMatch()
        && (!query.hasQueryItem("prompt") || QStringList{"none", "consent"}.contains(query.queryItemValue("prompt")));
}
void AddonIdentityBroker::cancel() {
    ++m_request;
    if (m_pending) { auto *reply = m_pending.data(); m_pending.clear(); reply->abort(); }
}
void AddonIdentityBroker::authorize(const QUrl &url) {
    QString state;
    if (!validate(url, &state)) { emit authorizationFinished({}, "Relay could not validate this identity-only authorization request."); return; }
    cancel();
    const auto ticket = m_request;
    const auto generation = m_client->m_generation;
    const auto account = m_client->currentUserId();
    // The endpoint and Authorization header are controlled exclusively by the host.
    QUrl endpoint(m_client->m_apiBase + "/oauth2/authorize"); endpoint.setQuery(url.query());
    QNetworkRequest request(endpoint);
    request.setRawHeader("Authorization", m_client->m_token);
    request.setRawHeader("User-Agent", "Relay/0.1 QtNetwork");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(20000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *reply = m_network.post(request, QByteArrayLiteral("{\"authorize\":true,\"permissions\":\"0\"}"));
    m_pending = reply; reply->setReadBufferSize(32769);
    connect(reply, &QIODevice::readyRead, this, [reply] { if (reply->bytesAvailable() > 32768) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, ticket, generation, account, state] {
        reply->deleteLater();
        if (ticket != m_request) return;
        m_pending.clear();
        if (!available() || m_client->m_generation != generation || m_client->currentUserId() != account) {
            emit authorizationFinished({}, "The Relay account changed. Connect again with the intended account."); return;
        }
        if (reply->error() != QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || reply->bytesAvailable() > 32768) {
            emit authorizationFinished({}, "Discord needs an additional approval or could not authorize this add-on. Browser connection is available below."); return;
        }
        const QUrl callback(QJsonDocument::fromJson(reply->readAll()).object().value("location").toString());
        auto base = callback; base.setQuery(QString{});
        const QUrlQuery query(callback);
        bool valid = base == m_redirect && !callback.hasFragment() && query.queryItemValue("state", QUrl::FullyDecoded) == state
            && query.allQueryItemValues("state").size() == 1 && query.allQueryItemValues("code").size() == 1
            && !query.queryItemValue("code").isEmpty() && query.queryItemValue("code").size() <= 4096;
        // Discord includes RFC 9207 issuer identification in current code grants.
        // Accept its exact issuer once, while retaining all destination/state checks.
        if (query.hasQueryItem("iss") && (query.allQueryItemValues("iss").size() != 1
            || query.queryItemValue("iss", QUrl::FullyDecoded) != "https://discord.com")) valid = false;
        for (const auto &item : query.queryItems()) if (item.first != "state" && item.first != "code" && item.first != "iss") valid = false;
        if (!valid) { emit authorizationFinished({}, "Discord did not return the expected one-time authorization."); return; }
        emit authorizationFinished(callback, {});
    });
}
