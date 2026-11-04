#pragma once
#include <QtGlobal>
#ifdef Q_OS_WIN
#include "platform/WindowsNotificationManager.h"
#else
#pragma once
#include <QObject>
#include <QDBusConnection>
#include <QHash>
#include <QTimer>
#include <QStringList>

class NotificationManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit NotificationManager(QObject *parent = nullptr);
    NotificationManager(QDBusConnection bus, QString service, QObject *parent = nullptr);
    bool available() const { return m_available; }
    QString status() const;
    QString error() const { return m_error; }
    void deliver(const QString &channel, const QString &title, const QString &body, bool sound);
    Q_INVOKABLE void sendTest();
    void clear();
    void setMinimumDuration(int seconds);
signals:
    void changed();
    void openChannelRequested(const QString &channel);
    void notificationDisplayed(bool sound);
private slots:
    void refresh();
    void flush();
    void activated(uint id, const QString &action);
    void closed(uint id, uint reason);
private:
    struct Message { QString title, body; bool sound = true; };
    void closeNotification(uint id);
    void reset(bool closeVisible);
    QDBusConnection m_bus;
    QString m_service, m_error;
    QStringList m_capabilities, m_order;
    QHash<QString, Message> m_pending;
    QHash<QString, uint> m_channelIds;
    QHash<uint, QString> m_channels;
    QTimer m_timer;
    int m_minimumDuration = 3;
    bool m_available = false;
    quint64 m_generation = 0;
    quint64 m_serviceEpoch = 0, m_refreshSerial = 0, m_notifyTicket = 0;
    bool m_inFlight = false;
};

#endif
