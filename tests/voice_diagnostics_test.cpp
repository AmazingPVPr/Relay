#include "VoiceDiagnostics.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

class VoiceDiagnosticsTest final : public QObject {
    Q_OBJECT
private slots:
    void failureSurvivesJoinAndCounterReset() {
        VoiceDiagnostics history;
        history.beginAttempt();
        history.record(VoiceDiagnostics::Event::SelfState, VoiceDiagnostics::Stage::AwaitingGatewayState);
        VoiceDiagnostics::Counts counts;
        counts.gateway = 5;
        counts.sent = 200;
        counts.opcode = 21;
        counts.closeCode = 4006;
        counts.heartbeatPending = true;
        history.fail(VoiceDiagnostics::Failure::SocketClosed, VoiceDiagnostics::Stage::Encrypted, counts);
        counts = {};
        history.beginAttempt();
        history.record(VoiceDiagnostics::Event::SelfState, VoiceDiagnostics::Stage::AwaitingGatewayState);

        const auto summary = history.summary();
        QCOMPARE(summary["attempts"].toInteger(), 2);
        const auto failures = summary["failures"].toArray();
        QCOMPARE(failures.size(), 1);
        const auto previous = failures.first().toObject();
        QCOMPARE(previous["attempt"].toInteger(), 1);
        QCOMPARE(previous["closeCode"].toInt(), 4006);
        QCOMPARE(previous["sent"].toInteger(), 200);
        QCOMPARE(previous["stage"].toString(), "encrypted");
        QVERIFY(previous["heartbeatPending"].toBool());
        history.record(VoiceDiagnostics::Event::Left, VoiceDiagnostics::Stage::Idle);
        QCOMPARE(history.summary()["failures"], summary["failures"]);
    }

    void storageIsBoundedAndChronological() {
        VoiceDiagnostics history;
        for (int attempt = 0; attempt < 80; ++attempt) {
            history.beginAttempt();
            history.fail(VoiceDiagnostics::Failure::Protocol, VoiceDiagnostics::Stage::WebSocket, {});
        }
        const auto summary = history.summary();
        const auto events = summary["events"].toArray(), failures = summary["failures"].toArray();
        QCOMPARE(summary["attempts"].toInteger(), 80);
        QCOMPARE(events.size(), 32);
        QCOMPARE(failures.size(), 8);
        QCOMPARE(failures.first().toObject()["attempt"].toInteger(), 73);
        QCOMPARE(failures.last().toObject()["attempt"].toInteger(), 80);
        qint64 previousTime = -1;
        for (const auto &value : events) {
            const auto time = value.toObject()["ms"].toInteger();
            QVERIFY(time >= previousTime);
            previousTime = time;
        }
        QVERIFY(QJsonDocument(summary).toJson(QJsonDocument::Compact).size() < 10000);
    }

    void onlyKnownStagesAndBoundedNumericCodesAreExposed() {
        VoiceDiagnostics history;
        history.beginAttempt();
        history.record(VoiceDiagnostics::Event(999),
            VoiceDiagnostics::stage(u"fixture-token user-id hostname.example payload"), 1234567890);
        VoiceDiagnostics::Counts counts;
        counts.opcode = 1234567890;
        counts.closeCode = 1234567890;
        counts.socketError = 1234567890;
        history.fail(VoiceDiagnostics::Failure(999), VoiceDiagnostics::Stage(999), counts);
        const auto bytes = QJsonDocument(history.summary()).toJson(QJsonDocument::Compact);
        for (const auto *forbidden : {"fixture-token", "user-id", "hostname", "payload", "1234567890"})
            QVERIFY(!bytes.contains(forbidden));
        QVERIFY(bytes.contains("unknown"));
        const auto failure = history.summary()["failures"].toArray().first().toObject();
        QCOMPARE(failure["opcode"].toInt(), -1);
        QCOMPARE(failure["socketError"].toInt(), -1);
        QCOMPARE(failure["closeCode"].toInt(), -1);
    }

    void presenceFlagsExplainPreSocketFailures() {
        VoiceDiagnostics history;
        history.beginAttempt();
        VoiceDiagnostics::Counts counts;
        counts.selfState = true;
        counts.serverUpdate = false;
        history.fail(VoiceDiagnostics::Failure::AwaitingStateTimeout,
            VoiceDiagnostics::Stage::AwaitingGatewayState, counts);
        const auto failure = history.summary()["failures"].toArray().first().toObject();
        QCOMPARE(failure["reason"].toString(), "awaiting_state_timeout");
        QVERIFY(failure["selfState"].toBool());
        QVERIFY(!failure["serverUpdate"].toBool());
        QCOMPARE(failure["gateway"].toInteger(), 0);
    }

    void recoveryStagesAndEventsRemainStructured() {
        VoiceDiagnostics history;
        history.beginAttempt();
        history.record(VoiceDiagnostics::Event::RecoveryScheduled,
            VoiceDiagnostics::stage(u"reconnecting_websocket"));
        history.record(VoiceDiagnostics::Event::Resumed,
            VoiceDiagnostics::Stage::Encrypted);
        history.record(VoiceDiagnostics::Event::AllocationRefresh,
            VoiceDiagnostics::stage(u"awaiting_leave"));
        history.record(VoiceDiagnostics::Event::AllocationChanged,
            VoiceDiagnostics::stage(u"awaiting_voice_server"));
        history.record(VoiceDiagnostics::Event::DuplicateServer,
            VoiceDiagnostics::Stage::Encrypted);
        history.fail(VoiceDiagnostics::Failure::ResumeTimeout,
            VoiceDiagnostics::Stage::ReconnectingWebSocket, {});
        history.fail(VoiceDiagnostics::Failure::LeaveAckTimeout,
            VoiceDiagnostics::Stage::AwaitingLeave, {});
        history.fail(VoiceDiagnostics::Failure::GatewayRequestRejected,
            VoiceDiagnostics::Stage::AwaitingVoiceServer, {});
        const auto bytes = QJsonDocument(history.summary()).toJson(QJsonDocument::Compact);
        for (const auto *expected : {"reconnecting_websocket", "awaiting_leave",
             "awaiting_voice_server", "recovery_scheduled", "resumed", "allocation_refresh",
             "allocation_changed", "duplicate_server", "resume_timeout", "leave_ack_timeout",
             "gateway_request_rejected"}) QVERIFY(bytes.contains(expected));
        QVERIFY(!bytes.contains("unknown"));
    }
};

QTEST_GUILESS_MAIN(VoiceDiagnosticsTest)
#include "voice_diagnostics_test.moc"
