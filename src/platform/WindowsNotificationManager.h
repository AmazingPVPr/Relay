#pragma once
#include <QObject>
#include <QSystemTrayIcon>
class NotificationManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit NotificationManager(QObject *parent=nullptr);
    bool available() const;
    QString status() const;
    QString error() const {return {};}
    void deliver(const QString &channel,const QString &title,const QString &body,bool sound);
    Q_INVOKABLE void sendTest();
    void clear();
    void setMinimumDuration(int seconds);
signals:
    void changed();
    void openChannelRequested(const QString &channel);
    void notificationDisplayed(bool sound);
private:
    QSystemTrayIcon m_tray;
    QString m_channel;
    int m_duration=3000;
};
