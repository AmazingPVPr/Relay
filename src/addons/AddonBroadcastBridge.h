#pragma once
#include <QObject>
#include <QHash>
#include <QPointer>

class AddonManager;
class BroadcastEngine;

// Host-side routing for an explicit add-on broadcast request. Add-ons receive
// media/status hooks, never an account, Discord transport, or voice session.
class AddonBroadcastBridge final : public QObject {
    Q_OBJECT
public:
    AddonBroadcastBridge(AddonManager *addons, BroadcastEngine *broadcast, QObject *parent = nullptr);
    ~AddonBroadcastBridge() override;
    void setVoiceContext(bool connected, const QString &guild, const QString &channel, const QString &name = {});
    Q_INVOKABLE bool startSource(const QString &id, int maximumHeight, int fps, bool audio);
private slots:
    void startForSender(int maximumHeight, int fps, bool audio);
    void stopForSender();
private:
    void attach(const QString &id, QObject *service);
    bool startService(QObject *service, int height, int fps, bool audio);
    void sync();
    AddonManager *m_addons;
    BroadcastEngine *m_broadcast;
    QHash<QObject *,QString> m_services, m_errors;
    QPointer<QObject> m_owner;
    bool m_voiceConnected = false;
    QString m_guild, m_channel, m_channelName;
};
