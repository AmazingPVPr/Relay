#include "AddonBroadcastBridge.h"
#include "AddonManager.h"
#include "BroadcastSourceInterface.h"
#include "BroadcastEngine.h"

AddonBroadcastBridge::AddonBroadcastBridge(AddonManager *addons, BroadcastEngine *broadcast, QObject *parent)
    : QObject(parent), m_addons(addons), m_broadcast(broadcast) {
    connect(addons, &AddonManager::serviceCreated, this, &AddonBroadcastBridge::attach);
    connect(addons, &AddonManager::itemsChanged, this, &AddonBroadcastBridge::sync);
    connect(broadcast, &BroadcastEngine::changed, this, &AddonBroadcastBridge::sync);
}

AddonBroadcastBridge::~AddonBroadcastBridge() { if (m_owner) m_broadcast->stop(); }

void AddonBroadcastBridge::attach(const QString &id, QObject *service) {
    if (!m_addons->broadcastSourceAllowed(id) || !qobject_cast<RelayBroadcastSourceInterface *>(service)) return;
    m_services.insert(service, id);
    // Legacy settings panels may emit publish requests themselves. A source
    // contributed only through the host dialog does not need these signals.
    if (service->metaObject()->indexOfSignal("broadcastRequested(int,int,bool)") >= 0)
        connect(service, SIGNAL(broadcastRequested(int,int,bool)), this, SLOT(startForSender(int,int,bool)));
    if (service->metaObject()->indexOfSignal("broadcastStopRequested()") >= 0)
        connect(service, SIGNAL(broadcastStopRequested()), this, SLOT(stopForSender()));
    connect(service, &QObject::destroyed, this, [this, service] { m_services.remove(service); m_errors.remove(service); });
    sync();
}

void AddonBroadcastBridge::setVoiceContext(bool connected, const QString &guild, const QString &channel, const QString &name) {
    if (m_owner && (!connected || guild != m_guild || channel != m_channel)) m_broadcast->stop();
    m_voiceConnected = connected; m_guild = guild; m_channel = channel; m_channelName = name;
    sync();
}

bool AddonBroadcastBridge::startSource(const QString &id, int height, int fps, bool audio) {
    // Only enabled contributions are selectable from the host's share dialog.
    const auto source = m_addons->openBroadcastSource(id);
    auto *service = source.value("service").value<QObject *>();
    return service && startService(service, height, fps, audio);
}

void AddonBroadcastBridge::startForSender(int height, int fps, bool audio) {
    startService(sender(), height, fps, audio);
}

bool AddonBroadcastBridge::startService(QObject *service, int height, int fps, bool audio) {
    if (!m_services.contains(service) || !m_addons->broadcastSourceAllowed(m_services.value(service))) return false;
    QString error;
    if (!m_voiceConnected) error = "Join a voice channel before streaming.";
    else if (!m_broadcast->available()) error = "Streaming is unavailable in this build.";
    else if (m_broadcast->active()) error = "Stop the current stream before starting another.";
    else if (!service->property("running").toBool() || !service->property("receiving").toBool()) error = "Connect the input and wait for video before streaming.";
    if (!error.isEmpty()) { m_errors[service] = error; sync(); return false; }
    m_errors.remove(service); m_owner = service;
    m_broadcast->startExternal(m_guild, m_channel, service, height, fps, audio);
    sync();
    return m_broadcast->active();
}

void AddonBroadcastBridge::stopForSender() { if (m_owner && m_owner == sender()) m_broadcast->stop(); }

void AddonBroadcastBridge::sync() {
    if (m_owner && m_broadcast->active() && !m_addons->broadcastSourceAllowed(m_services.value(m_owner))) m_broadcast->stop();
    if (m_owner && !m_broadcast->active()) { m_errors[m_owner] = m_broadcast->error(); m_owner.clear(); }
    for (auto it = m_services.cbegin(); it != m_services.cend(); ++it) {
        auto *source = qobject_cast<RelayBroadcastSourceInterface *>(it.key());
        if (!source) continue;
        const bool own = m_owner == it.key() && m_broadcast->active();
        source->setBroadcastContext({
            {"available", m_addons->broadcastSourceAllowed(it.value()) && m_broadcast->available()},
            {"voiceAvailable", m_voiceConnected}, {"channelName", m_channelName},
            {"active", own}, {"sharing", own && m_broadcast->sharing()},
            {"busy", m_broadcast->active() && !own},
            {"audioActive", own && m_broadcast->audioActive()},
            {"status", own ? m_broadcast->status() : QString{}},
            {"quality", own ? m_broadcast->qualitySummary() : QString{}},
            {"error", own ? m_broadcast->error() : m_errors.value(it.key())}
        });
    }
}
