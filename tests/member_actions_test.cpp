#include "MemberActions.h"
#include "DiscordClient.h"
#include "VoiceEngine.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>

namespace {
class RelationshipFixture final : public QTcpServer {
public:
    struct Response { int status = 204; QByteArray body; int delay = 0; };
    QQueue<Response> responses;
    QList<QByteArray> requests;
    RelationshipFixture() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    const auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    const int end = bytes.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    int length = 0;
                    for (const auto &header : bytes.left(end).split('\n'))
                        if (header.toLower().startsWith("content-length:")) length = header.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    requests.append(bytes);
                    const auto response = responses.isEmpty() ? Response{} : responses.dequeue();
                    QTimer::singleShot(response.delay, socket, [socket, response] {
                        if (socket->state() != QAbstractSocket::ConnectedState) return;
                        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(response.body.size()) + "\r\n\r\n" + response.body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
    QString base() const { return "http://127.0.0.1:" + QString::number(serverPort()); }
};
}

class MemberActionsTest final : public QObject {
    Q_OBJECT
    static void configure(DiscordClient &client, const RelationshipFixture &fixture) {
        client.m_apiBase = fixture.base(); client.m_token = "synthetic-fixture-token";
        client.m_currentUserId = "42"; client.m_connected = true;
    }
private slots:
    void localAudioIsImmediatePersistentAndAccountScoped() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("members.ini");
        DiscordClient client; client.m_currentUserId = "42";
        VoiceEngine voice;
        {
            MemberActions actions(&client, &voice, path);
            QVERIFY(actions.setLocalMuted("111", true));
            QVERIFY(actions.setLocalVolume("111", .35));
            QVERIFY(voice.userMuted("111")); QCOMPARE(voice.userVolume("111"), .35);
            QVERIFY(!actions.setLocalMuted("42", true));
            QVERIFY(!actions.setLocalVolume("bad/id", 1));
            client.m_currentUserId = "43"; ++client.m_generation; emit client.stateChanged();
            QVERIFY(!voice.userMuted("111")); QCOMPARE(voice.userVolume("111"), 1.0);
            QVERIFY(actions.setLocalVolume("111", 1.5));
            client.m_currentUserId = "42"; ++client.m_generation; emit client.stateChanged();
            QVERIFY(voice.userMuted("111")); QCOMPARE(voice.userVolume("111"), .35);
            QVERIFY(actions.state("111").value("muted").toBool());
            QVERIFY(actions.state("42").value("own").toBool());
        }
        voice.clearUserAudioSettings();
        {
            MemberActions reopened(&client, &voice, path);
            QVERIFY(voice.userMuted("111")); QCOMPARE(voice.userVolume("111"), .35);
            QVERIFY(reopened.setLocalMuted("111", false)); QVERIFY(reopened.setLocalVolume("111", 1));
        }
        QSettings saved(path, QSettings::IniFormat);
        QVERIFY(!saved.contains("accounts/42/voice/111/volume"));
        QCOMPARE(saved.value("accounts/43/voice/111/volume").toDouble(), 1.5);
        client.logout();
        QVERIFY(!voice.connected()); QVERIFY(!voice.micTesting());
    }

    void soundboardVolumeIsIndependentPersistentAndAccountScoped() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("members.ini");
        DiscordClient client; client.m_currentUserId = "42";
        VoiceEngine voice;
        {
            MemberActions actions(&client, &voice, path);
            QCOMPARE(actions.state("111").value("soundboardVolume").toDouble(), 1.0);
            QVERIFY(actions.setLocalVolume("111", .35));
            QVERIFY(actions.setLocalMuted("111", true));
            QVERIFY(actions.setLocalSoundboardVolume("111", 0));
            QCOMPARE(voice.userSoundboardVolume("111"), 0.0);
            QCOMPARE(voice.userVolume("111"), .35);
            QVERIFY(voice.userMuted("111"));
            QCOMPARE(actions.state("111").value("soundboardVolume").toDouble(), 0.0);
            QVERIFY(!actions.setLocalSoundboardVolume("42", 0));
            QVERIFY(!actions.setLocalSoundboardVolume("bad/id", 0));
            QVERIFY(!actions.setLocalSoundboardVolume("111", std::numeric_limits<double>::quiet_NaN()));
            QVERIFY(!actions.setLocalSoundboardVolume("111", std::numeric_limits<double>::infinity()));
            QCOMPARE(voice.userSoundboardVolume("111"), 0.0);
            QVERIFY(actions.setLocalSoundboardVolume("222", 3));
            QCOMPARE(voice.userSoundboardVolume("222"), 2.0);
            QVERIFY(actions.setLocalSoundboardVolume("222", -1));
            QCOMPARE(voice.userSoundboardVolume("222"), 0.0);
            QVERIFY(actions.setLocalSoundboardVolume("222", 1));
            // Resetting speech must retain this person's separate soundboard mute.
            QVERIFY(actions.setLocalMuted("111", false));
            QVERIFY(actions.setLocalVolume("111", 1));
            QCOMPARE(voice.userSoundboardVolume("111"), 0.0);
            client.m_currentUserId = "43"; ++client.m_generation; emit client.stateChanged();
            QCOMPARE(voice.userSoundboardVolume("111"), 1.0);
            QCOMPARE(voice.userVolume("111"), 1.0);
            QVERIFY(actions.setLocalSoundboardVolume("111", 1.8));
            client.m_currentUserId = "42"; ++client.m_generation; emit client.stateChanged();
            QCOMPARE(voice.userSoundboardVolume("111"), 0.0);
            QCOMPARE(voice.userVolume("111"), 1.0);
            QVERIFY(!voice.userMuted("111"));
        }
        voice.clearUserAudioSettings();
        {
            MemberActions reopened(&client, &voice, path);
            QCOMPARE(voice.userSoundboardVolume("111"), 0.0);
            QCOMPARE(voice.userVolume("111"), 1.0);
            QVERIFY(reopened.setLocalSoundboardVolume("111", 1));
        }
        QSettings saved(path, QSettings::IniFormat);
        QVERIFY(!saved.contains("accounts/42/voice/111/soundboardVolume"));
        QVERIFY(!saved.contains("accounts/42/voice/111/volume"));
        QCOMPARE(saved.value("accounts/43/voice/111/soundboardVolume").toDouble(), 1.8);
        {
            MemberActions reopened(&client, &voice, path);
            client.m_currentUserId = "43"; ++client.m_generation; emit client.stateChanged();
            QCOMPARE(voice.userSoundboardVolume("111"), 1.8);
            client.logout();
            QCOMPARE(voice.userSoundboardVolume("111"), 1.0);
        }
        QVERIFY(!voice.connected()); QVERIFY(!voice.micTesting());
    }

    void savedSoundboardVolumeDefaultsAndBoundsKeepSpeechSettings() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("members.ini");
        {
            QSettings saved(path, QSettings::IniFormat);
            // A pre-existing speech-only setting has an unchanged soundboard default.
            saved.setValue("accounts/42/voice/111/volume", .4);
            saved.setValue("accounts/42/voice/222/volume", .6);
            saved.setValue("accounts/42/voice/222/soundboardVolume", "invalid");
            saved.setValue("accounts/42/voice/333/soundboardVolume", 4.0);
            saved.setValue("accounts/42/voice/444/soundboardVolume", -2.0);
            saved.setValue("accounts/42/voice/555/volume", .8);
            saved.setValue("accounts/42/voice/555/soundboardVolume", std::numeric_limits<double>::infinity());
        }
        DiscordClient client; client.m_currentUserId = "42";
        VoiceEngine voice;
        MemberActions actions(&client, &voice, path);
        QCOMPARE(voice.userVolume("111"), .4);
        QCOMPARE(voice.userSoundboardVolume("111"), 1.0);
        QCOMPARE(voice.userVolume("222"), .6);
        QCOMPARE(voice.userSoundboardVolume("222"), 1.0);
        QCOMPARE(voice.userSoundboardVolume("333"), 2.0);
        QCOMPARE(voice.userSoundboardVolume("444"), 0.0);
        QCOMPARE(voice.userVolume("555"), .8);
        QCOMPARE(voice.userSoundboardVolume("555"), 1.0);
    }

    void blockWritesOnlyOnExplicitActionsAndUpdatesAfterSuccess() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        RelationshipFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.responses.enqueue({204, {}});
        fixture.responses.enqueue({403, "{\"message\":\"Missing Permissions\"}"});
        fixture.responses.enqueue({204, {}});
        DiscordClient client; configure(client, fixture); VoiceEngine voice;
        MemberActions actions(&client, &voice, directory.filePath("members.ini"));
        QSignalSpy changed(&actions, &MemberActions::blockedChanged);
        QVERIFY(actions.setLocalMuted("111", true)); QVERIFY(actions.setLocalVolume("111", .5));
        QVERIFY(actions.setBlocked("111", false)); // Already unblocked must not DELETE a friendship.
        QVERIFY(fixture.requests.isEmpty());
        QVERIFY(actions.setBlocked("111", true));
        QVERIFY(!actions.setBlocked("222", true));
        QVERIFY(!client.m_blockedUsers.contains("111"));
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 2000);
        QVERIFY(client.m_blockedUsers.contains("111"));
        QCOMPARE(changed.size(), 1);
        QCOMPARE(fixture.requests.size(), 1);
        QVERIFY(fixture.requests.first().startsWith("PUT /users/@me/relationships/111 "));
        const auto body = fixture.requests.first().mid(fixture.requests.first().indexOf("\r\n\r\n") + 4);
        QCOMPARE(QJsonDocument::fromJson(body).object().value("type").toInt(), 2);
        QVERIFY(actions.setBlocked("111", true)); QCOMPARE(fixture.requests.size(), 1);
        QVERIFY(actions.setBlocked("111", false));
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 2000);
        QVERIFY(!actions.error().isEmpty()); QVERIFY(client.m_blockedUsers.contains("111"));
        QCOMPARE(changed.size(), 1);
        QVERIFY(actions.setBlocked("111", false));
        QTRY_VERIFY_WITH_TIMEOUT(!actions.busy(), 2000);
        QVERIFY(!client.m_blockedUsers.contains("111")); QCOMPARE(changed.size(), 2);
        QVERIFY(fixture.requests.last().startsWith("DELETE /users/@me/relationships/111 "));
        QVERIFY(voice.userMuted("111")); QCOMPARE(voice.userVolume("111"), .5);
        QCOMPARE(fixture.requests.size(), 3);
    }

    void logoutDropsPendingRelationshipResultAndLocalOverrides() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        RelationshipFixture fixture; QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.responses.enqueue({204, {}, 150});
        DiscordClient client; configure(client, fixture); VoiceEngine voice;
        MemberActions actions(&client, &voice, directory.filePath("members.ini"));
        QSignalSpy changed(&actions, &MemberActions::blockedChanged);
        QVERIFY(actions.setLocalMuted("111", true));
        QVERIFY(actions.setBlocked("111", true));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 1, 2000);
        client.logout();
        QVERIFY(!actions.busy()); QVERIFY(!voice.userMuted("111"));
        QTest::qWait(200);
        QVERIFY(!client.m_blockedUsers.contains("111")); QCOMPARE(changed.size(), 0);
        QVERIFY(!actions.setBlocked("111", true));
        QCOMPARE(fixture.requests.size(), 1);
    }

    void messageLinksUseOnlyValidatedIdentifiers() {
        QCOMPARE(MemberActions::messageLink("100", "200", "300"), QStringLiteral("https://discord.com/channels/100/200/300"));
        QCOMPARE(MemberActions::messageLink({}, "200", "300"), QStringLiteral("https://discord.com/channels/@me/200/300"));
        QCOMPARE(MemberActions::messageLink("@me", "200", "300"), QStringLiteral("https://discord.com/channels/@me/200/300"));
        QVERIFY(MemberActions::messageLink("../outside", "200", "300").isEmpty());
        QVERIFY(MemberActions::messageLink("100", "200?token=x", "300").isEmpty());
        QVERIFY(MemberActions::messageLink("100", "200", "").isEmpty());
    }
};

QTEST_GUILESS_MAIN(MemberActionsTest)
#include "member_actions_test.moc"
