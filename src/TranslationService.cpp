#include "TranslationService.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

namespace {
struct Language { const char *code; const char *name; };
constexpr Language SupportedLanguages[] = {
    {"ar", "Arabic"}, {"bg", "Bulgarian"}, {"ca", "Catalan"}, {"zh-CN", "Chinese (Simplified)"},
    {"zh-TW", "Chinese (Traditional)"}, {"hr", "Croatian"}, {"cs", "Czech"}, {"da", "Danish"},
    {"nl", "Dutch"}, {"en", "English"}, {"et", "Estonian"}, {"fi", "Finnish"}, {"fr", "French"},
    {"de", "German"}, {"el", "Greek"}, {"he", "Hebrew"}, {"hi", "Hindi"}, {"hu", "Hungarian"},
    {"id", "Indonesian"}, {"it", "Italian"}, {"ja", "Japanese"}, {"ko", "Korean"}, {"lv", "Latvian"},
    {"lt", "Lithuanian"}, {"ms", "Malay"}, {"nb", "Norwegian"}, {"fa", "Persian"}, {"pl", "Polish"},
    {"pt", "Portuguese"}, {"ro", "Romanian"}, {"ru", "Russian"}, {"sr", "Serbian"}, {"sk", "Slovak"},
    {"sl", "Slovenian"}, {"es", "Spanish"}, {"sv", "Swedish"}, {"th", "Thai"}, {"tr", "Turkish"},
    {"uk", "Ukrainian"}, {"ur", "Urdu"}, {"vi", "Vietnamese"}
};
bool supported(const QString &code) {
    for (const auto &language : SupportedLanguages)
        if (code == QLatin1String(language.code)) return true;
    return false;
}
}

TranslationService::TranslationService(QObject *parent) : QObject(parent), m_network(this) {
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { fail(QStringLiteral("Google took too long to respond. Try again.")); });
}

TranslationService::~TranslationService() { abortRequest(); }

QVariantList TranslationService::languages() const {
    QVariantList result;
    result.reserve(std::size(SupportedLanguages));
    for (const auto &language : SupportedLanguages)
        result.append(QVariantMap{{QStringLiteral("code"), QString::fromLatin1(language.code)},
                                  {QStringLiteral("name"), QString::fromLatin1(language.name)}});
    return result;
}

bool TranslationService::translate(const QString &text, const QString &targetLanguage) {
    abortRequest();
    m_original.clear();
    m_translated.clear();
    m_detected.clear();
    m_error.clear();
    if (text.trimmed().isEmpty()) {
        fail(QStringLiteral("Enter some text to translate."));
        return false;
    }
    if (text.size() > MaximumInput) {
        fail(QStringLiteral("Choose up to 4,000 characters to translate at a time."));
        return false;
    }
    if (!supported(targetLanguage)) {
        fail(QStringLiteral("Choose a supported language."));
        return false;
    }
    m_original = text;
    m_target = targetLanguage;
    QUrl url = m_endpoint;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("gtx"));
    query.addQueryItem(QStringLiteral("dt"), QStringLiteral("t"));
    query.addQueryItem(QStringLiteral("sl"), QStringLiteral("auto"));
    query.addQueryItem(QStringLiteral("tl"), targetLanguage);
    // QUrlQuery accepts percent-encoded input. Encode before adding so literal
    // percent sequences and '+' survive the server's form-style decoding.
    query.addQueryItem(QStringLiteral("q"), QString::fromLatin1(QUrl::toPercentEncoding(text)));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", "Relay/0.1");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(m_timeoutMs);
    auto *reply = m_network.get(request);
    m_reply = reply;
    reply->setReadBufferSize(MaximumResponse + 1);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { consume(reply); });
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        if (m_reply == reply && reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > MaximumResponse)
            fail(QStringLiteral("Google returned a response that is too large. Try shorter text."));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finish(reply); });
    m_deadline.start(m_timeoutMs);
    emit changed();
    return true;
}

void TranslationService::abortRequest() {
    m_deadline.stop();
    if (auto *reply = m_reply.data()) {
        m_reply.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    m_response.clear();
}

void TranslationService::cancel() {
    if (!busy()) return;
    abortRequest();
    emit changed();
}

void TranslationService::clear() {
    abortRequest();
    m_original.clear();
    m_translated.clear();
    m_detected.clear();
    m_error.clear();
    emit changed();
}

void TranslationService::fail(const QString &message) {
    abortRequest();
    m_translated.clear();
    m_detected.clear();
    m_error = message;
    emit changed();
}

void TranslationService::consume(QNetworkReply *reply) {
    if (m_reply != reply) return;
    const auto bytes = reply->read(MaximumResponse + 1 - m_response.size());
    m_response.append(bytes);
    if (m_response.size() > MaximumResponse)
        fail(QStringLiteral("Google returned a response that is too large. Try shorter text."));
}

void TranslationService::finish(QNetworkReply *reply) {
    if (m_reply != reply) return;
    consume(reply);
    if (m_reply != reply) return;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 429) {
        fail(QStringLiteral("Google is limiting translation requests right now. Try again later."));
        return;
    }
    if (status != 200 || reply->error() != QNetworkReply::NoError) {
        fail(QStringLiteral("Couldn’t reach Google Translate. Check your connection and try again."));
        return;
    }
    QString translated, detected;
    if (!parseResponse(m_response, &translated, &detected)) {
        fail(QStringLiteral("Google returned an unexpected translation response. Try again later."));
        return;
    }
    m_deadline.stop();
    m_reply.clear();
    reply->deleteLater();
    m_response.clear();
    m_translated = translated;
    m_detected = detected;
    emit changed();
}

bool TranslationService::parseResponse(const QByteArray &data, QString *translated, QString *detected) {
    if (data.size() > MaximumResponse) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) return false;
    const auto root = document.array();
    if (root.isEmpty() || !root.at(0).isArray()) return false;
    const auto segments = root.at(0).toArray();
    if (segments.isEmpty() || segments.size() > 1024) return false;
    QString result;
    for (const auto &value : segments) {
        if (!value.isArray()) return false;
        const auto segment = value.toArray();
        if (segment.isEmpty() || !segment.at(0).isString()) return false;
        result += segment.at(0).toString();
        if (result.size() > MaximumInput * 6) return false;
    }
    if (result.trimmed().isEmpty()) return false;
    QString source;
    if (root.size() > 2 && root.at(2).isString()) {
        source = root.at(2).toString();
        if (source.size() > 16) return false;
        for (const auto c : source)
            if (!(c >= u'a' && c <= u'z') && !(c >= u'A' && c <= u'Z') && c != u'-') return false;
    }
    *translated = result;
    *detected = source;
    return true;
}

bool TranslationService::copyResult() {
    if (m_translated.isEmpty() || busy()) return false;
    auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!application || !application->clipboard()) return false;
    application->clipboard()->setText(m_translated);
    return true;
}
