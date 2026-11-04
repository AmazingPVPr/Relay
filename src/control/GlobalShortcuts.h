#pragma once
#include <QtGlobal>
#ifdef Q_OS_WIN
#include "../platform/WindowsGlobalShortcuts.h"
#else
#pragma once

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>

struct PortalShortcut {
    QString id;
    QVariantMap properties;
};
using PortalShortcutList = QList<PortalShortcut>;
Q_DECLARE_METATYPE(PortalShortcut)
Q_DECLARE_METATYPE(PortalShortcutList)
QDBusArgument &operator<<(QDBusArgument &argument, const PortalShortcut &shortcut);
const QDBusArgument &operator>>(const QDBusArgument &argument, PortalShortcut &shortcut);

class GlobalShortcuts final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QVariantList bindings READ bindings NOTIFY changed)
public:
    explicit GlobalShortcuts(QObject *parent = nullptr);
    // Injectable bus/service for isolated portal fixtures.
    GlobalShortcuts(const QDBusConnection &bus, const QString &service, QObject *parent = nullptr);
    ~GlobalShortcuts() override;
    bool active() const { return m_active; }
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    QVariantList bindings() const { return m_bindings; }
    Q_INVOKABLE void enable(const QVariantList &bindings);
    Q_INVOKABLE void disable();
    Q_INVOKABLE bool isBound(const QString &action) const { return m_boundIds.contains(action); }
    static QString preferredTrigger(const QString &sequence);
signals:
    void changed();
    void actionRequested(const QString &action);
private slots:
    void onResponse(uint response, const QVariantMap &results, const QDBusMessage &message);
    void onActivated(const QDBusObjectPath &session, const QString &id, qulonglong timestamp, const QVariantMap &options);
    void onDeactivated(const QDBusObjectPath &session, const QString &id, qulonglong timestamp, const QVariantMap &options);
    void onShortcutsChanged(const QDBusObjectPath &session, const PortalShortcutList &shortcuts);
    void onSessionClosed(const QVariantMap &details, const QDBusMessage &message);
private:
    enum Stage { Idle, Creating, Binding };
    void beginRequest(const QString &method, QVariantList arguments, QVariantMap options, Stage stage);
    void detachRequest();
    void release(bool closeSession);
    void fail(const QString &error);
    void updateBindings(const PortalShortcutList &shortcuts);
    void closeObject(const QString &path, const QString &interface);
    QString senderPart() const;
    bool validPortalPath(const QString &path, const QString &kind) const;
    QDBusConnection m_bus;
    QString m_service;
    QDBusServiceWatcher m_serviceWatcher;
    QTimer m_deadline;
    Stage m_stage = Idle;
    quint64 m_generation = 0;
    bool m_active = false, m_busy = false;
    QString m_status = "System shortcuts are off.", m_error;
    QString m_requestPath, m_requestToken, m_sessionPath, m_expectedSession;
    PortalShortcutList m_requested;
    QSet<QString> m_boundIds, m_pressed;
    QVariantList m_bindings;
};

#endif
