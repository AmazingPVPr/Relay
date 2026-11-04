#pragma once
#include <QObject>
#include <QJsonObject>
#include <memory>
namespace rtc { class PeerConnection; class DataChannel; }

// Separate encrypted text, stream control and bounded stream data channels. No TURN.
class RelayPeerTransport final : public QObject {
    Q_OBJECT
public:
    explicit RelayPeerTransport(const QStringList &stunServers, QObject *parent = nullptr);
    ~RelayPeerTransport() override;
    void offer();
    void acceptDescription(const QString &sdp, const QString &type);
    bool send(const QJsonObject &frame);
    bool connected() const;
    bool streamConnected() const;
    bool sendStreamControl(const QJsonObject &frame);
    bool sendStreamPacket(const QByteArray &packet);
    qsizetype streamBufferedAmount() const;
signals:
    void streamReady();
    void streamControlReceived(const QJsonObject &frame);
    void streamPacketReceived(const QByteArray &packet);
    void descriptionReady(const QString &sdp, const QString &type);
    void frameReceived(const QJsonObject &frame);
    void stateChanged(const QString &state);
private:
    struct CallbackGate;
    void attach(const std::shared_ptr<rtc::DataChannel> &channel);
    void attachStream(const std::shared_ptr<rtc::DataChannel> &channel);
    std::shared_ptr<rtc::DataChannel> m_streamControl, m_streamMedia;
    std::shared_ptr<CallbackGate> m_gate;
    std::shared_ptr<rtc::PeerConnection> m_peer;
    std::shared_ptr<rtc::DataChannel> m_channel;
};
