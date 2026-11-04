#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
class RelayUpdater : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    explicit RelayUpdater(bool enabled, QObject *parent = nullptr) : QObject(parent) {
        root = qEnvironmentVariable("RELAY_INSTALL_ROOT");
        if (root.isEmpty()) root = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..");
        root = QDir(root).canonicalPath();
        connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
            const QString output = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
            running = false;
            if (installing) {
                installing = false;
                if (code == 0 && exit == QProcess::NormalExit && output.endsWith("/" + version)) {
                    if (QProcess::startDetached("/bin/bash", {root + "/run.sh"})) QCoreApplication::quit();
                    else message = "Could not restart Relay. Try again.";
                } else message = "Update could not be installed. Try again.";
            } else if (code == 0 && exit == QProcess::NormalExit) {
                version = output;
                message = version.isEmpty() ? QString() : "Install update and restart Relay (disconnects calls and streams).";
            }
            emit changed();
        });
        connect(&process, &QProcess::errorOccurred, this, [this] { running = false; installing = false; message = "Could not run updater. Try again."; emit changed(); });
        timer.setInterval(60 * 60 * 1000);
        connect(&timer, &QTimer::timeout, this, &RelayUpdater::check);
        if (enabled && QFile::exists(root + "/updater/relay-updater.py")) { timer.start(); QTimer::singleShot(15000, this, &RelayUpdater::check); }
    }
    bool available() const { return !version.isEmpty(); }
    bool busy() const { return running; }
    QString status() const { return message; }
    Q_INVOKABLE void check() { if (!running) start(false); }
    Q_INVOKABLE void install() { if (!running && available()) start(true); }
signals:
    void changed();
private:
    void start(bool install) {
        installing = install; running = true;
        if (install) message = "Installing update… Relay will restart.";
        emit changed();
        QStringList args{root + "/updater/relay-updater.py", root};
        if (!install) args << "--check";
        process.start("python3", args);
    }
    QString root, version, message;
    bool running = false, installing = false;
    QProcess process;
    QTimer timer;
};
