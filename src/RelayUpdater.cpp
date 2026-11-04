#include "RelayUpdater.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {
QString buildVersion(const QString &root) {
    QFile file(root + "/build-info.json");
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object().value("release_id").toString();
}
}
RelayUpdater::RelayUpdater(bool enabled, QObject *parent) : QObject(parent), m_enabled(enabled) {
    m_root = qEnvironmentVariable("RELAY_INSTALL_ROOT");
    const auto runtime = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..");
    if (m_root.isEmpty()) m_root = runtime;
    m_root = QDir(m_root).canonicalPath();
    m_current = QCoreApplication::instance()->property("relayBuildId").toString();
    if (m_current.isEmpty()) m_current = buildVersion(runtime);
    if (m_current.isEmpty()) m_current = buildVersion(m_root);
#ifdef Q_OS_WIN
    m_program = QCoreApplication::applicationDirPath() + "/relay-launcher.exe";
    if (!QFile::exists(m_program)) m_program = m_root + "/bin/relay-launcher.exe";
#else
    m_program = QStandardPaths::findExecutable("python3");
    m_script = runtime + "/updater/relay-updater.py";
    if (!QFile::exists(m_script)) m_script = m_root + "/updater/relay-updater.py";
#endif
    m_status = !enabled ? "Update checks are disabled in this preview."
        : supported() ? "Ready to check for updates." : "This package does not include the Relay updater.";
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
        const auto output = m_process.readAllStandardOutput().trimmed();
        const auto stderrText = QString::fromUtf8(m_process.readAllStandardError()).trimmed().left(300);
        m_running = false;
        m_lastChecked = QDateTime::currentDateTime().toString(Qt::ISODate);
        const auto result = QJsonDocument::fromJson(output).object();
        if (exit != QProcess::NormalExit || code != 0 || result.isEmpty() || !result.value("error").toString().isEmpty()) {
            m_error = result.value("error").toString().left(300);
            if (m_error.isEmpty()) m_error = stderrText.isEmpty() ? "The updater did not complete. Try again." : stderrText;
            m_status = "Update check failed: " + m_error;
            m_installing = false;
            emit changed();
            return;
        }
        m_error.clear();
        if (m_installing) {
            m_installing = false;
            if (result.value("current_version").toString() != m_available || m_available.isEmpty()) {
                m_status = "The requested update was not installed. Check again.";
            } else if (restartAfterExit()) {
                m_status = "Update installed. Restarting Relay…";
                emit changed();
                QCoreApplication::quit();
                return;
            } else {
                m_status = "Update installed. Close and reopen Relay to finish.";
            }
        } else {
            m_available = result.value("available_version").toString();
            m_status = available() ? "An update is available. Install it and restart when you are ready."
                : "You have the latest published update for this platform.";
        }
        emit changed();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_running = false; m_installing = false;
        m_error = m_process.errorString(); m_status = "Could not start the updater: " + m_error;
        emit changed();
    });
    m_timer.setInterval(60 * 60 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &RelayUpdater::check);
    if (supported()) { m_timer.start(); QTimer::singleShot(15000, this, &RelayUpdater::check); }
}
RelayUpdater::~RelayUpdater() {
    m_timer.stop();
    if (m_process.state() != QProcess::NotRunning) { m_process.kill(); m_process.waitForFinished(300); }
}
bool RelayUpdater::supported() const {
    if (!m_enabled || m_program.isEmpty() || !QFile::exists(m_program)) return false;
#ifdef Q_OS_WIN
    return true;
#else
    return QFile::exists(m_script) && !QStandardPaths::findExecutable("openssl").isEmpty();
#endif
}
void RelayUpdater::check() { if (!m_running) start(false); }
void RelayUpdater::install() { if (!m_running && available()) start(true); }
void RelayUpdater::start(bool install) {
    if (!supported()) { m_status = m_enabled ? "This package does not include the Relay updater." : "Update checks are disabled in this preview."; emit changed(); return; }
    m_installing = install; m_running = true; m_error.clear();
    m_status = install ? "Downloading and verifying the update…" : "Checking for updates…";
    QStringList args;
#ifdef Q_OS_WIN
    args << (install ? "--install" : "--check") << "--json";
#else
    args << m_script << m_root;
    if (!install) args << "--check";
    args << "--json";
#endif
    emit changed();
    m_process.start(m_program, args);
}
bool RelayUpdater::restartAfterExit() {
    QStringList args;
#ifdef Q_OS_WIN
    args << "--restart-after" << QString::number(QCoreApplication::applicationPid());
#else
    args << m_script << m_root << "--restart-after" << QString::number(QCoreApplication::applicationPid());
#endif
    return QProcess::startDetached(m_program, args);
}
