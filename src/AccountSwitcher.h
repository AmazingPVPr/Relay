#pragma once

#include <QObject>
#include <QHash>
#include <QSet>
#include <QVariantList>
#include <functional>

class CredentialStore;

// Saved display metadata is separate from per-account desktop-wallet secrets.
// The session callback is C++ only: no token property, invokable or Qt signal.
class AccountSwitcher final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY changed)
    Q_PROPERTY(QString currentAccountId READ currentAccountId NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool sessionChangeBlocked READ sessionChangeBlocked NOTIFY changed)
    Q_PROPERTY(bool canAdd READ canAdd NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit AccountSwitcher(const QString &metadataFile = {}, QObject *parent = nullptr);
    ~AccountSwitcher() override;
    QVariantList accounts() const;
    QString currentAccountId() const { return m_currentId; }
    bool busy() const;
    bool sessionChangeBlocked() const { return m_sessionBusy; }
    bool canAdd() const { return m_accounts.size() < 5 && !busy() && !m_sessionBusy; }
    QString error() const { return m_error; }
    QString status() const { return m_status; }
    void setSessionHandler(std::function<void(const QString &)> handler);
    void setSessionBusy(bool busy);
    void captureAuthenticated(const QString &token);
    void rememberCurrentCredential(const QString &token);
    void updateCurrentAccount(const QString &id, const QString &name, const QString &avatarUrl);
    void resetSession();
    void rejectAuthentication();
    void setWindowHandler(std::function<bool(const QString &)> handler);
    Q_INVOKABLE void refreshAccounts();
    Q_INVOKABLE bool openAccountWindow(const QString &id);
    Q_INVOKABLE bool switchAccount(const QString &id);
    Q_INVOKABLE void addAccount();
    Q_INVOKABLE void removeAccount(const QString &id);
    Q_INVOKABLE void signOutCurrent();
    Q_INVOKABLE void cancelPending();
signals:
    void changed();
    void addAccountRequested();
    void signOutRequested();
private:
    friend class AccountSwitcherTest;
    struct Account { QString id, name, avatar; bool remembered = false; };
    CredentialStore *storeFor(const QString &id);
    void activate(QByteArray secret, const QString &expectedId);
    bool allowSessionChange();
    void storeChanged(const QString &id);
    bool saveMetadata();
    void loadMetadata();
    static bool validId(const QString &id);
    static QString displayName(QString name);
    static QString avatarUrl(const QString &url);
    static void wipe(QByteArray &value);
    QString m_file, m_currentId, m_switchingId, m_expectedId;
    QString m_error, m_status;
    QList<Account> m_accounts, m_baseline;
    QHash<QString, CredentialStore *> m_stores;
    QSet<QString> m_saving, m_removing;
    QByteArray m_stagedSecret;
    bool m_waitingForIdentity = false;
    bool m_sessionBusy = false;
    std::function<void(const QString &)> m_sessionHandler;
    std::function<bool(const QString &)> m_windowHandler;
};
