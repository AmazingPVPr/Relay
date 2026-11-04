#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>

class RelayUpdater final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool supported READ supported NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY changed)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY changed)
    Q_PROPERTY(QString lastChecked READ lastChecked NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit RelayUpdater(bool enabled, QObject *parent = nullptr);
    ~RelayUpdater() override;
    bool available() const { return !m_available.isEmpty(); }
    bool busy() const { return m_running; }
    bool supported() const;
    QString status() const { return m_status; }
    QString currentVersion() const { return m_current; }
    QString availableVersion() const { return m_available; }
    QString lastChecked() const { return m_lastChecked; }
    QString error() const { return m_error; }
    Q_INVOKABLE void check();
    // The UI confirms that this installs the update and restarts Relay.
    Q_INVOKABLE void install();
signals:
    void changed();
private:
    friend class RelayUpdaterTest;
    void start(bool install);
    bool restartAfterExit();
    QString m_root, m_program, m_script, m_current, m_available, m_status, m_error, m_lastChecked;
    bool m_enabled = false, m_running = false, m_installing = false;
    QProcess m_process;
    QTimer m_timer;
};
