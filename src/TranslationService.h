#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

class QNetworkReply;

// Deliberately separate from the account client's network manager and credentials.
// Text leaves the device only when translate() is explicitly invoked.
class TranslationService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString originalText READ originalText NOTIFY changed)
    Q_PROPERTY(QString translatedText READ translatedText NOTIFY changed)
    Q_PROPERTY(QString targetLanguage READ targetLanguage NOTIFY changed)
    Q_PROPERTY(QString detectedLanguage READ detectedLanguage NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList languages READ languages CONSTANT)
public:
    explicit TranslationService(QObject *parent = nullptr);
    ~TranslationService() override;
    bool busy() const { return !m_reply.isNull(); }
    QString originalText() const { return m_original; }
    QString translatedText() const { return m_translated; }
    QString targetLanguage() const { return m_target; }
    QString detectedLanguage() const { return m_detected; }
    QString error() const { return m_error; }
    QVariantList languages() const;
    Q_INVOKABLE bool translate(const QString &text, const QString &targetLanguage);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clear();
    Q_INVOKABLE bool copyResult();
signals:
    void changed();
private:
    friend class TranslationTest;
    static constexpr qsizetype MaximumInput = 4000;
    static constexpr qsizetype MaximumResponse = 256 * 1024;
    static bool parseResponse(const QByteArray &data, QString *translated, QString *detected);
    void abortRequest();
    void fail(const QString &message);
    void consume(QNetworkReply *reply);
    void finish(QNetworkReply *reply);
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    QByteArray m_response;
    QString m_original, m_translated, m_detected, m_error;
    QString m_target = QStringLiteral("en");
    // Private fixture override: never exposed as a setting or QML API.
    QUrl m_endpoint{QStringLiteral("https://translate.googleapis.com/translate_a/single")};
    int m_timeoutMs = 10000;
};
