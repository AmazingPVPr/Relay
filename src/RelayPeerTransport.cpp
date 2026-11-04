#include "RelayPeerTransport.h"
#include <QJsonDocument>
#include <QPointer>
#include <rtc/rtc.hpp>
#include <mutex>
#include <atomic>

struct RelayPeerTransport::CallbackGate {
    std::mutex mutex;
    RelayPeerTransport *owner = nullptr;
    std::atomic<qsizetype> pendingMedia{0};
    template<class Action> void post(Action action) {
        std::lock_guard lock(mutex);
        if (!owner) return;
        // Posting and destruction are serialized; Qt removes queued calls with the receiver.
        QPointer<RelayPeerTransport> weak(owner);
        QMetaObject::invokeMethod(owner, [weak, action] { if (weak) action(weak.data()); }, Qt::QueuedConnection);
    }
};

RelayPeerTransport::RelayPeerTransport(const QStringList &servers, QObject *parent) : QObject(parent) {
    rtc::Configuration config;
    config.disableAutoNegotiation = true;
    config.maxMessageSize = 16384;
    config.disableFingerprintVerification = false;
    for (const auto &server : servers) {
        // No user-controlled TURN configuration and no relay fallback.
        if (!server.startsWith("stun:")) throw std::runtime_error("Only STUN discovery is supported");
        rtc::IceServer ice(server.toStdString());
        if (ice.type != rtc::IceServer::Type::Stun) throw std::runtime_error("TURN is disabled");
        config.iceServers.push_back(ice);
    }
    m_peer = std::make_shared<rtc::PeerConnection>(config);
    m_gate = std::make_shared<CallbackGate>(); m_gate->owner = this;
    const auto gate = m_gate;
    m_peer->onGatheringStateChange([gate](rtc::PeerConnection::GatheringState state) {
        if (state != rtc::PeerConnection::GatheringState::Complete) return;
        gate->post([](RelayPeerTransport *self) {
            if (auto local = self->m_peer->localDescription())
                emit self->descriptionReady(QString::fromStdString(std::string(*local)), QString::fromStdString(local->typeString()));
        });
    });
    m_peer->onStateChange([gate](rtc::PeerConnection::State state) {
        gate->post([state](RelayPeerTransport *self) {
            if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Closed)
                emit self->stateChanged("Direct connection unavailable. Relay never falls back to a message server.");
        });
    });
    m_peer->onDataChannel([gate](std::shared_ptr<rtc::DataChannel> channel) {
        gate->post([channel](RelayPeerTransport *self) { self->attach(channel); });
    });
}
RelayPeerTransport::~RelayPeerTransport() {
    { std::lock_guard lock(m_gate->mutex); m_gate->owner = nullptr; }
    for (auto channel : {m_streamControl, m_streamMedia}) if (channel) { channel->resetCallbacks(); channel->close(); }
    if (m_channel) { m_channel->resetCallbacks(); m_channel->close(); }
    if (m_peer) { m_peer->resetCallbacks(); m_peer->close(); }
}
void RelayPeerTransport::offer() {
    attach(m_peer->createDataChannel("relay-text-v1"));
    attachStream(m_peer->createDataChannel("relay-stream-control-v1"));
    attachStream(m_peer->createDataChannel("relay-stream-media-v1"));
    m_peer->setLocalDescription(rtc::Description::Type::Offer);
}
void RelayPeerTransport::acceptDescription(const QString &sdp, const QString &type) {
    if ((type != "offer" && type != "answer") || sdp.size() > 48000 ||
        !sdp.contains("m=application ") || sdp.contains("m=audio ") || sdp.contains("m=video ") ||
        sdp.contains(" typ relay")) throw std::runtime_error("Not a direct text session");
    m_peer->setRemoteDescription(rtc::Description(sdp.toStdString(), type.toStdString()));
    if (type == "offer") m_peer->setLocalDescription(rtc::Description::Type::Answer);
}
void RelayPeerTransport::attach(const std::shared_ptr<rtc::DataChannel> &channel) {
    if (channel->label().starts_with("relay-stream-")) { attachStream(channel); return; }
    if (m_channel || channel->label() != "relay-text-v1") { channel->close(); return; }
    m_channel = channel;
    const auto gate = m_gate;
    const auto opened = [gate] {
        gate->post([](RelayPeerTransport *self) { emit self->stateChanged("Connected directly · encrypted text"); if (self->streamConnected()) emit self->streamReady(); });
    };
    channel->onOpen(opened);
    channel->onClosed([gate] {
        gate->post([](RelayPeerTransport *self) { emit self->stateChanged("Peer disconnected"); });
    });
    channel->onMessage([gate](rtc::message_variant data) {
        auto text = std::get_if<std::string>(&data);
        if (!text || text->size() > 16384) {
            gate->post([](RelayPeerTransport *self) { if (self->m_channel) self->m_channel->close(); });
            return;
        }
        const auto frame = QJsonDocument::fromJson(QByteArray::fromStdString(*text)).object();
        gate->post([frame](RelayPeerTransport *self) { emit self->frameReceived(frame); });
    });
    if (channel->isOpen()) opened();
}
bool RelayPeerTransport::connected() const { return m_channel && m_channel->isOpen(); }
bool RelayPeerTransport::send(const QJsonObject &frame) {
    if (!connected() || m_channel->bufferedAmount() > 262144) return false;
    const auto data = QJsonDocument(frame).toJson(QJsonDocument::Compact);
    if (data.size() > 16384) return false;
    try { m_channel->send(data.toStdString()); return true; } catch (...) { return false; }
}

void RelayPeerTransport::attachStream(const std::shared_ptr<rtc::DataChannel> &channel) {
    const bool control = channel->label() == "relay-stream-control-v1";
    auto &slot = control ? m_streamControl : m_streamMedia;
    if ((!control && channel->label() != "relay-stream-media-v1") || slot) { channel->close(); return; }
    slot = channel;
    const auto gate = m_gate;
    const auto opened = [gate] { gate->post([](RelayPeerTransport *self) { if (self->streamConnected()) emit self->streamReady(); }); };
    channel->onOpen(opened);
    channel->onClosed([gate] { gate->post([](RelayPeerTransport *self) { emit self->stateChanged("Relay stream channel closed"); }); });
    channel->onMessage([gate, control](rtc::message_variant data) {
        if (control) {
            auto value = std::get_if<std::string>(&data);
            if (!value || value->size() > 2048) return;
            auto frame = QJsonDocument::fromJson(QByteArray::fromStdString(*value)).object();
            gate->post([frame](RelayPeerTransport *self) { emit self->streamControlReceived(frame); });
        } else {
            auto value = std::get_if<rtc::binary>(&data);
            if (!value || value->size() > 16384) return;
            const auto count = qsizetype(value->size());
            if (gate->pendingMedia.fetch_add(count) + count > 8 * 1024 * 1024) { gate->pendingMedia.fetch_sub(count); return; }
            QByteArray packet(reinterpret_cast<const char *>(value->data()), count);
            gate->post([gate, packet, count](RelayPeerTransport *self) { gate->pendingMedia.fetch_sub(count); emit self->streamPacketReceived(packet); });
        }
    });
    if (channel->isOpen()) opened();
}
bool RelayPeerTransport::streamConnected() const {
    return connected() && m_streamControl && m_streamControl->isOpen() && m_streamMedia && m_streamMedia->isOpen();
}
qsizetype RelayPeerTransport::streamBufferedAmount() const { return m_streamMedia ? qsizetype(m_streamMedia->bufferedAmount()) : 0; }
bool RelayPeerTransport::sendStreamControl(const QJsonObject &frame) {
    auto data = QJsonDocument(frame).toJson(QJsonDocument::Compact);
    if (!streamConnected() || data.size() > 2048 || m_streamControl->bufferedAmount() > 65536) return false;
    try { m_streamControl->send(data.toStdString()); return true; } catch (...) { return false; }
}
bool RelayPeerTransport::sendStreamPacket(const QByteArray &packet) {
    if (!streamConnected() || packet.size() > 16384 || streamBufferedAmount() > 5 * 1024 * 1024) return false;
    try { m_streamMedia->send(reinterpret_cast<const std::byte *>(packet.constData()), size_t(packet.size())); return true; } catch (...) { return false; }
}
