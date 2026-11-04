#include "WindowsNotificationManager.h"
#include <QCoreApplication>
#include <QIcon>
#include <QVariant>
#include <algorithm>
NotificationManager::NotificationManager(QObject *parent):QObject(parent),m_tray(QIcon(":/relay/icons/relay.svg"),this){
    m_tray.setToolTip("Relay");
    connect(&m_tray,&QSystemTrayIcon::messageClicked,this,[this]{if(!m_channel.isEmpty())emit openChannelRequested(m_channel);});
}
bool NotificationManager::available()const{return !QCoreApplication::instance()->property("relayPreviewMode").toBool() && QSystemTrayIcon::isSystemTrayAvailable() && QSystemTrayIcon::supportsMessages();}
QString NotificationManager::status()const{return available()?"Windows notifications are ready.":"Desktop notifications are unavailable in this session.";}
void NotificationManager::setMinimumDuration(int seconds){m_duration=std::clamp(seconds,3,20)*1000;}
void NotificationManager::deliver(const QString &channel,const QString &title,const QString &body,bool sound){
    if(!available() || channel.size()>128)return;
    m_channel=channel;m_tray.show();
    m_tray.showMessage(title.left(180),body.left(500),QSystemTrayIcon::Information,m_duration);
    // Windows owns balloon-notification sound policy. Avoid a second sound.
    Q_UNUSED(sound);emit notificationDisplayed(false);
}
void NotificationManager::sendTest(){deliver({},"Relay","Notifications are working. Your messages will appear here.",true);}
void NotificationManager::clear(){m_channel.clear();m_tray.hide();}
