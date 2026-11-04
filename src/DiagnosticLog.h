#pragma once
#include <QObject>
#include <QDateTime>
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QGuiApplication>
#include <QClipboard>
#include <QHash>
class DiagnosticLog final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString text READ text NOTIFY changed)
public:
    explicit DiagnosticLog(bool temporary, QObject *parent = nullptr) : QObject(parent) {
        if (!temporary) {
            const auto dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            QDir().mkpath(dir); path = dir + "/diagnostics.log";
            QFile f(path); if (f.open(QIODevice::ReadOnly)) entries = QString::fromUtf8(f.read(131072));
        }
    }
    QString text() const { return entries; }
    void watch(QObject *source, const QString &name, const char *signal) {
        source->setProperty("relayDiagnosticSource", name);
        connect(source, signal, this, SLOT(captureError()));
        connect(source, &QObject::destroyed, this, [this, source] { last.remove(source); });
    }
    Q_INVOKABLE void copy() { QGuiApplication::clipboard()->setText(entries); }
    Q_INVOKABLE void clear() { entries.clear(); save(); emit changed(); }
public slots:
    void captureError() {
        auto *s = sender(); if (!s) return;
        const QString name = s->property("relayDiagnosticSource").toString();
        const QString value = s->property("error").toString();
        if (last.value(s) == value) return;
        last[s] = value;
        if (!value.isEmpty()) append(name, value);
    }
    void obsEvent(const QString &message) { append("OBS input", message); }
signals:
    void changed();
private:
    void append(const QString &source, QString message) {
        message.replace(QRegularExpression(R"((?i)\b(?:https?|rtmps?|wss?)://[^\s]+)"), "[address removed]");
        message.replace(QRegularExpression(R"((?:/[^\s/:]+){2,})"), "[path removed]");
        message.replace(QRegularExpression(R"(\b[A-Za-z0-9_\-]{32,}\b)"), "[value removed]");
        message.replace(QRegularExpression("[\r\n]+"), " ");
        entries += QDateTime::currentDateTime().toString(Qt::ISODate) + " [" + source + "] " + message.left(500) + "\n";
        if (entries.size() > 65536) { entries = entries.right(65536); entries.remove(0, entries.indexOf('\n') + 1); }
        save(); emit changed();
    }
    void save() {
        if (path.isEmpty()) return;
        QSaveFile f(path); if (!f.open(QIODevice::WriteOnly)) return;
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(entries.toUtf8()); f.commit();
    }
    QString path, entries;
    QHash<QObject *, QString> last;
};
