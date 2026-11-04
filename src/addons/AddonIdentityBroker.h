#pragma once
#include <QObject>
#include <QUrl>
#include <QNetworkAccessManager>
#include <QPointer>
class DiscordClient;
class QNetworkReply;
// Narrow host-owned authorization broker. Reusable Discord credentials never
// cross the plug-in boundary; only a one-use identify-scope OAuth callback does.
class AddonIdentityBroker final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString accountName READ accountName NOTIFY changed)
    Q_PROPERTY(QString accountId READ accountId NOTIFY changed)
public:
    AddonIdentityBroker(DiscordClient *client, const QString &clientId, const QUrl &redirectUri, QObject *parent = nullptr);
    ~AddonIdentityBroker() override;
    bool available() const;
    QString accountId() const;
    QString accountName() const;
    Q_INVOKABLE void authorize(const QUrl &authorizationUrl);
    Q_INVOKABLE void cancel();
signals:
    void changed();
    void authorizationFinished(const QUrl &callback, const QString &error);
private:
    bool validate(const QUrl &url, QString *state) const;
    QPointer<DiscordClient> m_client;
    QString m_clientId;
    QUrl m_redirect;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_pending;
    quint64 m_request = 0;
};
