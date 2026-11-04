#pragma once

#include <QObject>
#include <QSettings>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace QKeychain { class Job; }

// Only Relay's own session key is accessed. No token property or plaintext fallback.
class CredentialStore final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool remembered READ remembered NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit CredentialStore(QObject *parent = nullptr);
    explicit CredentialStore(const QString &preferencesFile, QObject *parent = nullptr);
    CredentialStore(const QString &preferencesFile, const QString &entryKey, QObject *parent);
    ~CredentialStore() override;
    bool busy() const { return m_running || m_pending.has_value(); }
    bool remembered() const { return m_remembered; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    Q_INVOKABLE void restore();
    void save(const QString &token);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void cancelRestore();
signals:
    void changed();
    void restored(const QString &token);
    void restoreFinished(bool found);
    void cleared();
private:
    friend class CredentialStoreTest;
    friend class AccountSwitcherTest;
    enum class Operation { Read, Write, Delete };
    struct Request { Operation operation; QByteArray secret; quint64 generation; };
    using Completion = std::function<void(int, QByteArray)>;
    using Executor = std::function<void(Operation, const QByteArray &, Completion)>;
    void enqueue(Operation operation, QByteArray secret = {});
    void pump();
    void finish(quint64 ticket, Operation operation, quint64 generation, int error, QByteArray value);
    QKeychain::Job *createJob(Operation operation, const QByteArray &secret);
    bool setRestoreAllowed(bool allowed);
    static bool validToken(const QByteArray &token);
    static void wipe(QByteArray &secret);
    std::unique_ptr<QSettings> m_preferences;
    QString m_entryKey;
    Executor m_executor;
    std::optional<Request> m_pending;
    QTimer m_waitNotice;
    quint64 m_generation = 0, m_ticket = 0;
    bool m_running = false, m_remembered = false;
    Operation m_operation = Operation::Read;
    QString m_status = QStringLiteral("Sign-in is stored in your desktop wallet");
    QString m_error;
};
