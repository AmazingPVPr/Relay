#pragma once
#include <QObject>
#include <QPointer>
#include <QNetworkAccessManager>
#include <QSet>
#include <QVariantMap>
#include <QVariantList>
#include <QJsonObject>
#include <QTimer>
#include <memory>
#include <functional>

class DiscordClient;
class QNetworkReply;

// Explicit native account operations. No passwords, payment credentials or
// account responses are written to settings, diagnostics or a persistent cache.
class AccountSettingsService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap account READ account NOTIFY changed)
    Q_PROPERTY(QString page READ page NOTIFY changed)
    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(QVariantMap details READ details NOTIFY changed)
    Q_PROPERTY(QVariantMap capabilities READ capabilities NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool mutating READ mutating NOTIFY changed)
    Q_PROPERTY(bool committed READ committed NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QStringList challengeMethods READ challengeMethods NOTIFY changed)
public:
    explicit AccountSettingsService(DiscordClient *client, QObject *parent = nullptr);
    ~AccountSettingsService() override;
    QVariantMap account() const { return m_account; }
    QString page() const { return m_page; }
    QVariantList items() const { return m_items; }
    QVariantMap details() const { return m_details; }
    QVariantMap capabilities() const;
    bool busy() const { return m_reads > 0; }
    bool mutating() const { return m_mutating; }
    bool committed() const { return m_mutating && m_activeAction != "mfa-finish"; }
    QString error() const { return m_error; }
    QString status() const { return m_status; }
    QStringList challengeMethods() const { return m_challengeMethods; }
    Q_INVOKABLE void loadPage(const QString &page);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void close();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool execute(const QString &action, const QString &targetId = {}, const QVariantMap &values = {});
    Q_INVOKABLE bool confirmMfa(const QString &method, const QString &data);
    Q_INVOKABLE void cancelMfa();
signals:
    void changed();
    void actionCompleted(const QString &action);
    // C++ integration only: main forwards this to the existing credential store.
    // It is never exposed through an account property or diagnostic response.
    void credentialRotated(const QString &token);
private:
    friend class AccountSettingsServiceTest;
    using Handler = std::function<void(const QJsonValue &, const QString &)>;
    struct PendingMfa { QByteArray method; QString path, ticket, action; QJsonObject body; Handler callback; };
    bool ready() const;
    bool fail(const QString &reason);
    void reset();
    void cancelRequests();
    void send(const QByteArray &method, const QString &path, QJsonObject body, bool mutation, Handler callback, const QByteArray &mfaAuthorization = {}, bool allowChallenge = true);
    bool acceptAccount(const QJsonObject &object);
    void acceptItems(const QString &page, const QJsonValue &value);
    bool acceptPreferences(const QJsonObject &response);
    bool acceptEmailPreferences(const QJsonObject &response);
    bool preparePreferences(const QVariantMap &values, QJsonObject &body);
    void clearSensitive();
    bool rotateCredential(const QJsonObject &response, bool required = false);
    bool acceptBackupCodes(const QJsonObject &response);
    QPointer<DiscordClient> m_client;
    QNetworkAccessManager m_network;
    QSet<QNetworkReply *> m_replies;
    QVariantMap m_account, m_details;
    QVariantList m_items;
    QHash<QString, QJsonObject> m_targets;
    QString m_page = "account", m_error, m_status, m_userId;
    QByteArray m_preferences;
    QString m_mfaSecret, m_backupNonce, m_regenerateNonce, m_activeAction;
    QTimer m_sensitiveTimer;
    std::unique_ptr<PendingMfa> m_pendingMfa;
    QStringList m_challengeMethods;
    quint64 m_session = 0, m_operation = 0;
    int m_reads = 0;
    bool m_mutating = false, m_preferencesReady = false, m_fixtureMode = false, m_mfaRetrying = false;
};
