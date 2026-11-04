#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QProcess>
#include <functional>
#include <QWebSocket>
#include <memory>
#include "AuthCrypto.h"

class QNetworkReply;
class DiscordAuth final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool rememberLogin READ rememberLogin WRITE setRememberLogin NOTIFY rememberLoginChanged)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool qrReady READ qrReady NOTIFY changed)
    Q_PROPERTY(QString qrImage READ qrImage NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString scannedUserName READ scannedUserName NOTIFY changed)
    Q_PROPERTY(int expiresIn READ expiresIn NOTIFY changed)
    Q_PROPERTY(bool completed READ completed NOTIFY changed)
    Q_PROPERTY(QString bitwardenServer READ bitwardenServer NOTIFY changed)
    Q_PROPERTY(QString bitwardenItem READ bitwardenItem NOTIFY changed)
    Q_PROPERTY(bool mfaRequired READ mfaRequired NOTIFY changed)
public:
    bool rememberLogin() const;
    void setRememberLogin(bool enabled);
    QString bitwardenItem() const;
    QString bitwardenServer() const;
    Q_INVOKABLE void connectBitwarden(const QString &server, const QString &email, const QString &password, const QString &code, int method);
    Q_INVOKABLE void disconnectBitwarden();
    bool mfaRequired() const { return !m_mfaTicket.isEmpty(); }
    Q_INVOKABLE void signInPassword(const QString &email, const QString &password);
    Q_INVOKABLE void submitCode(const QString &code, bool backup = false);
    Q_INVOKABLE void signInBitwarden(const QString &itemId, const QString &masterPassword);
    void signInSavedCredential(const QString &encoded);
    explicit DiscordAuth(QObject *parent = nullptr);
    ~DiscordAuth() override;
    bool active() const { return m_active; }
    bool qrReady() const { return m_qrReady; }
    QString qrImage() const { return m_qrImage; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    QString scannedUserName() const { return m_scannedUserName; }
    int expiresIn() const { return m_expiresIn; }
    bool completed() const { return m_completed; }
    Q_INVOKABLE void start();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void retry();
signals:
    void changed();
    void rememberLoginChanged();
    void authenticated(const QString &token);
    void rememberedLoginReady(const QString &encoded);
private:
    friend class AuthTests;
    enum class Stage { Idle, Hello, Nonce, Fingerprint, Scan, Confirmation, Exchange, Complete };
    void passwordRequest(const QString &path, const QJsonObject &body);
    void runBitwarden(const QStringList &arguments, const QString &password, std::function<void(QByteArray)> done);
    void fetchBitwardenCode();
    QString m_bwProgram; // Private generated-fixture seam; runtime always resolves bw.
    static QString totpCode(const QString &secret, qint64 seconds);
    QByteArray m_rememberedLogin;
    QString m_savedTotp;
    bool m_savedTotpAttempted = false;
    QString m_apiBase = QStringLiteral("https://discord.com/api/v9");
    QString m_mfaTicket, m_bwItem;
    QByteArray m_bwSession;
    QPointer<QProcess> m_bwProcess;
    bool m_bwTotpAttempted = false;
    bool m_bwConnecting = false;
    void stopTransport();
    void fail(const QString &reason);
    void onText(const QString &message);
    void send(const QJsonObject &payload);
    void heartbeat();
    void exchangeTicket(const QString &ticket);
    static QString renderQr(const QByteArray &url);
    std::unique_ptr<AuthCrypto> m_crypto;
    std::unique_ptr<QWebSocket> m_socket;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_heartbeat;
    QTimer m_deadline;
    QTimer m_countdown;
    QElapsedTimer m_elapsed;
    quint64 m_generation = 0;
    Stage m_stage = Stage::Idle;
    bool m_active = false;
    bool m_qrReady = false;
    bool m_completed = false;
    bool m_waitingHeartbeat = false;
    bool m_closing = false;
    int m_timeoutMs = 0;
    int m_expiresIn = 0;
    QString m_qrImage;
    QString m_status = QStringLiteral("Sign in with the Discord app on your phone");
    QString m_error;
    QString m_scannedUserName;
    QByteArray m_fingerprint;
    // Test-only private seams for isolated loopback fixtures; never exposed to QML.
    QString m_gatewayUrl = QStringLiteral("wss://remote-auth-gateway.discord.gg/?v=2");
    QString m_loginUrl = QStringLiteral("https://discord.com/api/v9/users/@me/remote-auth/login");
};
