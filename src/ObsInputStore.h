#pragma once
#include <QObject>
#include <QSettings>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
namespace QKeychain { class Job; }

// Only OBS input configuration is stored here. Server URLs may include secret
// paths/query strings, so the complete configuration goes into the wallet.
class ObsInputStore final : public QObject {
    Q_OBJECT
public:
    explicit ObsInputStore(const QString &preferencesFile = {}, QObject *parent = nullptr);
    ~ObsInputStore() override;
    bool remember() const { return m_remember; }
    bool busy() const { return m_running || m_pending.has_value(); }
    bool saved() const { return m_saved; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    void restore();
    void save(int mode, const QString &server, const QString &key);
    void setRemember(bool remember);
    void clear();
    void cancelRestore();
signals:
    void changed();
    void restored(int mode, const QString &server, const QString &key);
private:
    friend class ObsInputStoreTest;
    friend class StudioInputPersistenceTest;
    enum class Operation { Read, Write, Delete };
    using Completion = std::function<void(int, QByteArray)>;
    using Executor = std::function<void(Operation, const QByteArray &, Completion)>;
    struct Request { Operation operation; QByteArray bytes; quint64 generation; bool preferenceSafe = true; };
    static bool decode(const QByteArray &bytes, int &mode, QString &server, QString &key);
    static void wipe(QByteArray &bytes);
    bool preferences(bool restoreAllowed);
    void enqueue(Operation operation, QByteArray bytes = {}, bool preferenceSafe = true);
    void pump();
    void finish(quint64 ticket, const Request &request, int result, QByteArray value);
    QKeychain::Job *createJob(Operation operation, const QByteArray &bytes);
    std::unique_ptr<QSettings> m_preferences;
    Executor m_executor;
    std::optional<Request> m_pending;
    QTimer m_waitNotice;
    quint64 m_generation = 0, m_ticket = 0;
    Operation m_operation = Operation::Read;
    bool m_running = false, m_remember = true, m_saved = false;
    QString m_status = "Input will be remembered in your desktop wallet", m_error;
};
