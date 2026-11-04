#include "NotificationManager.h"
#include <QDBusMessage>
#include <algorithm>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>

namespace { const QString Path = "/org/freedesktop/Notifications", Interface = "org.freedesktop.Notifications"; }
NotificationManager::NotificationManager(QObject *parent) : NotificationManager(QDBusConnection::sessionBus(), "org.freedesktop.Notifications", parent) {}
NotificationManager::NotificationManager(QDBusConnection bus, QString service, QObject *parent)
    : QObject(parent), m_bus(std::move(bus)), m_service(std::move(service)) {
    m_bus.connect(m_service, Path, Interface, "ActionInvoked", this, SLOT(activated(uint,QString)));
    m_bus.connect(m_service, Path, Interface, "NotificationClosed", this, SLOT(closed(uint,uint)));
    auto watcher = new QDBusServiceWatcher(m_service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] {
        ++m_serviceEpoch;
        // IDs belong to the old daemon. Closing them on the replacement service
        // could dismiss a different application's newly allocated notification.
        reset(false); m_available = false; refresh();
    });
    m_timer.setInterval(250); connect(&m_timer, &QTimer::timeout, this, &NotificationManager::flush);
    QTimer::singleShot(0, this, &NotificationManager::refresh);
}
QString NotificationManager::status() const { return m_available ? "Desktop notifications are ready." : "Desktop notifications are unavailable in this session."; }
void NotificationManager::refresh() {
    auto message = QDBusMessage::createMethodCall(m_service, Path, Interface, "GetCapabilities");
    auto watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 2000), this);
    const auto epoch = m_serviceEpoch, serial = ++m_refreshSerial;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, epoch, serial] {
        if (epoch != m_serviceEpoch || serial != m_refreshSerial) { watcher->deleteLater(); return; }
        QDBusPendingReply<QStringList> reply = *watcher;
        m_available = !reply.isError(); m_capabilities = m_available ? reply.value() : QStringList{};
        m_error = m_available ? QString{} : "Your desktop notification service could not be reached.";
        emit changed(); watcher->deleteLater();
    });
}
void NotificationManager::deliver(const QString &channel, const QString &title, const QString &body, bool sound) {
    if (!m_available || channel.size() > 128) return;
    if (!m_pending.contains(channel)) {
        if (m_pending.size() >= 32) m_pending.remove(m_order.takeFirst());
        m_order.append(channel);
    }
    m_pending.insert(channel, {title.left(180), body.left(500), sound});
    if (!m_timer.isActive()) m_timer.start();
}
void NotificationManager::setMinimumDuration(int seconds) { m_minimumDuration=std::clamp(seconds,3,20); }
void NotificationManager::sendTest() { deliver({}, "Relay", "Notifications are working. Your messages will appear here.", true); }
void NotificationManager::flush() {
    if (m_inFlight || !m_available) return;
    if (m_order.isEmpty()) { m_timer.stop(); return; }
    const auto channel = m_order.takeFirst(); const auto content = m_pending.take(channel);
    auto message = QDBusMessage::createMethodCall(m_service, Path, Interface, "Notify");
    const bool actions = !channel.isEmpty() && m_capabilities.contains("actions");
    const QString body = m_capabilities.contains("body-markup") ? content.body.toHtmlEscaped() : content.body;
    // Relay supplies its own controllable sound. Never play a second desktop sound.
    const QVariantMap hints{{"desktop-entry", "relay"}, {"category", "im.received"}, {"suppress-sound", true}, {"urgency", QVariant::fromValue<uchar>(1)}};
    message.setArguments({"Relay", m_channelIds.value(channel, 0), "relay", content.title, body,
        actions ? QStringList{"default", "Open conversation"} : QStringList{}, hints, m_minimumDuration * 1000});
    const auto generation = m_generation;
    const auto epoch = m_serviceEpoch, ticket = ++m_notifyTicket;
    m_inFlight = true;
    auto watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 2000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, channel, generation, epoch, ticket, sound=content.sound] {
        QDBusPendingReply<uint> reply = *watcher;
        if (ticket == m_notifyTicket) m_inFlight = false;
        if (generation == m_generation && epoch == m_serviceEpoch) {
            if (reply.isError()) { m_error = "The desktop could not display a notification."; emit changed(); }
            else {
                const auto id = reply.value();
                if (!id) { m_error = "The desktop returned an invalid notification ID."; emit changed(); watcher->deleteLater(); return; }
                if (m_channelIds.size() >= 128 && !m_channelIds.contains(channel)) {
                    const auto key = *m_channelIds.keyBegin();
                    const auto expiredId = m_channelIds.take(key);
                    m_channels.remove(expiredId); closeNotification(expiredId);
                }
                m_channels.remove(m_channelIds.value(channel)); m_channelIds.insert(channel,id); m_channels.insert(id,channel);
                m_error.clear(); emit notificationDisplayed(sound); emit changed();
            }
        } else if (!reply.isError() && epoch == m_serviceEpoch) closeNotification(reply.value());
        watcher->deleteLater();
    });
}
void NotificationManager::activated(uint id, const QString &action) {
    if (action == "default" && m_channels.contains(id) && !m_channels.value(id).isEmpty()) emit openChannelRequested(m_channels.value(id));
}
void NotificationManager::closed(uint id, uint) {
    if (!m_channels.contains(id)) return;
    const auto channel = m_channels.take(id);
    if (m_channelIds.value(channel) == id) m_channelIds.remove(channel);
}
void NotificationManager::closeNotification(uint id) {
    if (!id) return;
    auto message = QDBusMessage::createMethodCall(m_service, Path, Interface, "CloseNotification");
    message.setArguments({id}); m_bus.asyncCall(message, 1000);
}
void NotificationManager::reset(bool closeVisible) {
    ++m_generation; ++m_notifyTicket; m_inFlight = false;
    m_pending.clear(); m_order.clear(); m_timer.stop();
    if (closeVisible) for (const auto id : std::as_const(m_channelIds)) closeNotification(id);
    m_channelIds.clear(); m_channels.clear();
}
void NotificationManager::clear() { reset(true); }
