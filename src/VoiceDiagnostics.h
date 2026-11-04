#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QStringView>
#include <QVariantList>
#include <QVariantMap>
#include <algorithm>
#include <array>

// This accepts only closed event/stage/reason enums, counts and presence flags.
// Callers must never provide names, IDs, endpoints, error bodies or media.
class VoiceDiagnostics final {
public:
    enum class Stage {
        Unknown, Idle, AwaitingGatewayState, WebSocket, UdpDiscovery,
        SessionDescription, WaitingForParticipants, MlsExchange, Encrypted, Failed,
        AwaitingLeave, AwaitingVoiceServer, ReconnectingWebSocket
    };
    enum class Event {
        Unknown, AttemptStarted, StageChanged, SelfState, ServerUpdate,
        WebSocketOpening, WebSocketConnected, WebSocketClosed, WebSocketError,
        TransportReady, EncryptionReady, Deadline, Failure, Left,
        RecoveryScheduled, Resumed, AllocationRefresh, AllocationChanged, DuplicateServer
    };
    enum class Failure {
        Unknown, HeartbeatTimeout, AwaitingStateTimeout, SocketConnect,
        SocketClosed, UdpDiscoveryTimeout, SessionDescriptionTimeout,
        EncryptionTimeout, RemoteDisconnect, ChannelChanged, ServerMoved,
        Protocol, Audio, LeaveAckTimeout, ResumeTimeout, GatewayRequestRejected
    };
    struct Counts {
        quint64 gateway = 0, binary = 0, udp = 0, sent = 0, received = 0, decoded = 0;
        quint64 transportErrors = 0, mediaErrors = 0;
        int opcode = -1, closeCode = 0, socketError = -1;
        bool selfState = false, serverUpdate = false, heartbeatPending = false;
        bool transportReady = false, encryptedReady = false;
    };

    VoiceDiagnostics() { m_clock.start(); }

    void beginAttempt() {
        ++m_attempt;
        record(Event::AttemptStarted, Stage::AwaitingGatewayState);
    }

    void record(Event event, Stage stage, int code = 0) {
        push(m_events, m_eventCount, m_eventNext,
             EventRecord{m_attempt, m_clock.elapsed(), event, stage, safeCode(code)});
    }

    // Take this snapshot before aborting sockets or clearing session state.
    void fail(Failure reason, Stage stage, Counts counts) {
        counts.opcode = counts.opcode >= -1 && counts.opcode <= 255 ? counts.opcode : -1;
        counts.closeCode = safeCode(counts.closeCode);
        counts.socketError = safeCode(counts.socketError);
        push(m_failures, m_failureCount, m_failureNext,
             FailureRecord{m_attempt, m_clock.elapsed(), reason, stage, counts});
        record(Event::Failure, stage, int(reason));
    }

    QJsonObject summary() const {
        QVariantList events, failures;
        each(m_events, m_eventCount, m_eventNext, [&](const auto &entry) {
            events.append(QVariantMap{{"attempt", entry.attempt}, {"ms", entry.ms},
                {"event", eventName(entry.event)}, {"stage", stageName(entry.stage)},
                {"code", entry.code}});
        });
        each(m_failures, m_failureCount, m_failureNext, [&](const auto &entry) {
            const auto &c = entry.counts;
            failures.append(QVariantMap{{"attempt", entry.attempt}, {"ms", entry.ms},
                {"reason", failureName(entry.reason)}, {"stage", stageName(entry.stage)},
                {"gateway", c.gateway}, {"binary", c.binary}, {"udp", c.udp},
                {"sent", c.sent}, {"received", c.received}, {"decoded", c.decoded},
                {"transportErrors", c.transportErrors}, {"mediaErrors", c.mediaErrors},
                {"opcode", c.opcode}, {"closeCode", c.closeCode}, {"socketError", c.socketError},
                {"selfState", c.selfState}, {"serverUpdate", c.serverUpdate},
                {"heartbeatPending", c.heartbeatPending}, {"transportReady", c.transportReady},
                {"encryptedReady", c.encryptedReady}});
        });
        return QJsonObject::fromVariantMap({{"attempts", m_attempt},
            {"events", events}, {"failures", failures}});
    }

    static Stage stage(QStringView value) {
        if (value == u"idle") return Stage::Idle;
        if (value == u"awaiting_gateway_state") return Stage::AwaitingGatewayState;
        if (value == u"websocket") return Stage::WebSocket;
        if (value == u"udp_discovery") return Stage::UdpDiscovery;
        if (value == u"session_description") return Stage::SessionDescription;
        if (value == u"waiting_for_participants") return Stage::WaitingForParticipants;
        if (value == u"mls_exchange") return Stage::MlsExchange;
        if (value == u"encrypted") return Stage::Encrypted;
        if (value == u"failed") return Stage::Failed;
        if (value == u"awaiting_leave") return Stage::AwaitingLeave;
        if (value == u"awaiting_voice_server") return Stage::AwaitingVoiceServer;
        if (value == u"reconnecting_websocket") return Stage::ReconnectingWebSocket;
        return Stage::Unknown;
    }

private:
    struct EventRecord {
        quint64 attempt = 0;
        qint64 ms = 0;
        Event event = Event::Unknown;
        Stage stage = Stage::Unknown;
        int code = 0;
    };
    struct FailureRecord {
        quint64 attempt = 0;
        qint64 ms = 0;
        Failure reason = Failure::Unknown;
        Stage stage = Stage::Unknown;
        Counts counts;
    };

    QElapsedTimer m_clock;
    quint64 m_attempt = 0;
    std::array<EventRecord, 32> m_events{};
    std::array<FailureRecord, 8> m_failures{};
    qsizetype m_eventCount = 0, m_eventNext = 0, m_failureCount = 0, m_failureNext = 0;

    static int safeCode(int code) { return code >= -1 && code <= 65535 ? code : -1; }

    template<class T, std::size_t N>
    static void push(std::array<T, N> &items, qsizetype &count, qsizetype &next, T value) {
        items[next] = std::move(value);
        next = (next + 1) % N;
        count = std::min<qsizetype>(N, count + 1);
    }

    template<class T, std::size_t N, class F>
    static void each(const std::array<T, N> &items, qsizetype count, qsizetype next, F read) {
        const auto start = (next + N - count) % N;
        for (qsizetype index = 0; index < count; ++index) read(items[(start + index) % N]);
    }

    static QString stageName(Stage value) {
        switch (value) {
        case Stage::Idle: return "idle";
        case Stage::AwaitingGatewayState: return "awaiting_gateway_state";
        case Stage::WebSocket: return "websocket";
        case Stage::UdpDiscovery: return "udp_discovery";
        case Stage::SessionDescription: return "session_description";
        case Stage::WaitingForParticipants: return "waiting_for_participants";
        case Stage::MlsExchange: return "mls_exchange";
        case Stage::Encrypted: return "encrypted";
        case Stage::Failed: return "failed";
        case Stage::AwaitingLeave: return "awaiting_leave";
        case Stage::AwaitingVoiceServer: return "awaiting_voice_server";
        case Stage::ReconnectingWebSocket: return "reconnecting_websocket";
        default: return "unknown";
        }
    }

    static QString eventName(Event value) {
        switch (value) {
        case Event::AttemptStarted: return "attempt_started";
        case Event::StageChanged: return "stage_changed";
        case Event::SelfState: return "self_state";
        case Event::ServerUpdate: return "server_update";
        case Event::WebSocketOpening: return "websocket_opening";
        case Event::WebSocketConnected: return "websocket_connected";
        case Event::WebSocketClosed: return "websocket_closed";
        case Event::WebSocketError: return "websocket_error";
        case Event::TransportReady: return "transport_ready";
        case Event::EncryptionReady: return "encryption_ready";
        case Event::Deadline: return "deadline";
        case Event::Failure: return "failure";
        case Event::Left: return "left";
        case Event::RecoveryScheduled: return "recovery_scheduled";
        case Event::Resumed: return "resumed";
        case Event::AllocationRefresh: return "allocation_refresh";
        case Event::AllocationChanged: return "allocation_changed";
        case Event::DuplicateServer: return "duplicate_server";
        default: return "unknown";
        }
    }

    static QString failureName(Failure value) {
        switch (value) {
        case Failure::HeartbeatTimeout: return "heartbeat_timeout";
        case Failure::AwaitingStateTimeout: return "awaiting_state_timeout";
        case Failure::SocketConnect: return "socket_connect";
        case Failure::SocketClosed: return "socket_closed";
        case Failure::UdpDiscoveryTimeout: return "udp_discovery_timeout";
        case Failure::SessionDescriptionTimeout: return "session_description_timeout";
        case Failure::EncryptionTimeout: return "encryption_timeout";
        case Failure::RemoteDisconnect: return "remote_disconnect";
        case Failure::ChannelChanged: return "channel_changed";
        case Failure::ServerMoved: return "server_moved";
        case Failure::Protocol: return "protocol";
        case Failure::Audio: return "audio";
        case Failure::LeaveAckTimeout: return "leave_ack_timeout";
        case Failure::ResumeTimeout: return "resume_timeout";
        case Failure::GatewayRequestRejected: return "gateway_request_rejected";
        default: return "unknown";
        }
    }
};
