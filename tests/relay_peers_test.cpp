#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QJsonDocument>
#include <QSignalSpy>
#include <cstdio>
#include <sodium.h>
#include "RelayPeers.h"
#include "RelayStreams.h"
#include "BroadcastEngine.h"
#include "BroadcastPacket.h"
#include "addons/BroadcastSourceInterface.h"
#include "addons/EncodedBroadcastSourceInterface.h"
#include "StreamTestMedia.h"
#include <QVideoSink>
#include <QImage>
#include <QDataStream>
#include "RelayRendezvous.h"
#include "RelayPeerTransport.h"

class PeerStreamSource : public QObject, public RelayBroadcastSourceInterface, public RelayEncodedBroadcastSourceInterface {
    Q_OBJECT
    Q_INTERFACES(RelayBroadcastSourceInterface RelayEncodedBroadcastSourceInterface)
public:
    bool enabled=false, raw=false, audio=false;
    QList<RelayEncodedVideoFrame> frames;
    void setBroadcastContext(const QVariantMap &) override {}
    void setBroadcastCaptureEnabled(bool value,int,int,bool sound) override {enabled=value;audio=sound;}
    RelayEncodedVideo takeEncodedBroadcastVideo() override {
        RelayEncodedVideo value;
        if(!enabled || raw)return value;
        value.available=true;value.width=320;value.height=180;value.fps=30;
        value.frames=frames;frames.clear();return value;
    }
    RelayBroadcastMedia takeBroadcastMedia() override {
        RelayBroadcastMedia value;
        if(enabled && raw){QImage image(320,180,QImage::Format_RGB32);image.fill(Qt::cyan);value.video=QVideoFrame(image);}
        if(enabled && audio)value.audio=QByteArray(3840,0);
        return value;
    }
};

class RelayPeersTest : public QObject {
    Q_OBJECT
    static QJsonObject rpc(const QString &name, const QJsonObject &args = {}) {
        return {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", name}, {"arguments", args}}}};
    }
    static QJsonObject payload(const QJsonObject &response) {
        return QJsonDocument::fromJson(response.value("result").toObject().value("content").toArray().first().toObject().value("text").toString().toUtf8()).object();
    }
    static void enableLocal(RelayPeers &peers, quint16 port) {
        peers.m_url = "ws://127.0.0.1:" + QString::number(port);
        peers.setEnabled(true);
    }
private slots:
    void directStreamsPreserveOriginalAndCoexistWithText() {
        QTemporaryDir root;RelayRendezvous server;QVERIFY(server.listen(QHostAddress::LocalHost,0));
        RelayPeers a(root.path()+"/a",{}),b(root.path()+"/b",{}),c(root.path()+"/c",{});
        const auto peerB=a.addContact(b.contactCard(),"B"),peerA=b.addContact(a.contactCard(),"A");
        const auto peerC=a.addContact(c.contactCard(),"C");c.addContact(a.contactCard(),"A");
        BroadcastEngine ba,bb,bc;ba.setIdentity("3333","generated-session");
        RelayStreams sa(&a,&ba),sb(&b,&bb),sc(&c,&bc);
        sb.setAudioMuted(true);sc.setAudioMuted(true); // Generated tests never open an audio device.
        QVideoSink sinkB,sinkC;sb.setVideoSink(&sinkB);sc.setVideoSink(&sinkC);
        enableLocal(a,server.port());enableLocal(b,server.port());enableLocal(c,server.port());
        QTRY_VERIFY_WITH_TIMEOUT(a.streamConnected(peerB) && a.streamConnected(peerC),15000);
        PeerStreamSource source;ba.startExternal("1111","2222",&source,720,30,true);
        QVERIFY(ba.active());QTRY_COMPARE(sb.streams().size(),1);QTRY_COMPARE(sc.streams().size(),1);
        sb.watch(peerA);sc.watch(peerA);QTRY_COMPARE(sa.viewerCount(),2);
        QSignalSpy packets(&b,&RelayPeers::streamPacketReceived);
        const auto picture=QByteArray::fromHex("000000010c")+QByteArray(100000,char(0xff))+QByteArray(1,char(0x80))+StreamTestMedia::encodedFrame("H264");
        source.frames.append({picture,9000,true});
        QTRY_VERIFY_WITH_TIMEOUT(sinkB.videoFrame().isValid() && sinkC.videoFrame().isValid(),5000);
        QCOMPARE(sinkB.videoFrame().width(),320);QVERIFY(!ba.sharing());QCOMPARE(ba.framesSent(),quint64(0));
        QByteArray received;
        for(const auto &row:packets){const auto packet=row[1].toByteArray();if(packet.size()>38 && quint8(packet[16])==1)received+=packet.mid(38);}
        // Sustain an approximately 24 Mbps original source to two actual local peers.
        packets.clear();int sentFrames=0;QTimer sourceTimer;sourceTimer.setInterval(33);
        connect(&sourceTimer,&QTimer::timeout,&source,[&]{source.frames.append({picture,quint32(12000+sentFrames*3000),true});if(++sentFrames==30)sourceTimer.stop();});
        sourceTimer.start();QTRY_COMPARE_WITH_TIMEOUT(sentFrames,30,3000);QTest::qWait(200);
        qsizetype originalBytes=0;for(const auto &row:packets){const auto packet=row[1].toByteArray();if(packet.size()>38 && quint8(packet[16])==1)originalBytes+=packet.size()-38;}
        QVERIFY(originalBytes>=29*BroadcastPacket::normalizeH264(picture).size());
        QCOMPARE(received,BroadcastPacket::normalizeH264(picture)); // Original video crosses unchanged, including the >12K fragmentation.
        QVERIFY(a.sendText(peerB,"Text while streaming"));QTRY_COMPARE(b.messagesForPeer(peerA).size(),1);
        QTRY_VERIFY(!a.messagesForPeer(peerB).last().toMap().value("pending").toBool());
        sb.stopWatching();QTRY_COMPARE(sa.viewerCount(),1);QVERIFY(sb.watchingPeer().isEmpty());
        sa.setPublishEnabled(false);QTRY_VERIFY(sc.watchingPeer().isEmpty());QTRY_VERIFY(sb.streams().isEmpty());QVERIFY(ba.active());
        sa.setPublishEnabled(true);QTRY_COMPARE(sb.streams().size(),1);sb.watch(peerA);QTRY_COMPARE(sa.viewerCount(),1);
        // The raw capture encoder works independently of Discord's allocation too.
        source.raw=true;QTRY_VERIFY_WITH_TIMEOUT(sinkB.videoFrame().isValid(),5000);
        QVERIFY(a.sendText(peerB,"Text after stream restart"));QTRY_COMPARE(b.messagesForPeer(peerA).size(),2);
        ba.stop();QTRY_VERIFY(sb.watchingPeer().isEmpty());QCOMPARE(sa.viewerCount(),0);
        QVERIFY(b.streamConnected(peerA)); // Stopping media leaves encrypted text connected.
    }
    void streamTransportSeparatesBinaryFromTextAndBoundsMessages() {
        RelayPeerTransport a({}),b({});
        connect(&a,&RelayPeerTransport::descriptionReady,&b,&RelayPeerTransport::acceptDescription);
        connect(&b,&RelayPeerTransport::descriptionReady,&a,&RelayPeerTransport::acceptDescription);
        QSignalSpy text(&b,&RelayPeerTransport::frameReceived),control(&b,&RelayPeerTransport::streamControlReceived),media(&b,&RelayPeerTransport::streamPacketReceived);
        a.offer();QTRY_VERIFY_WITH_TIMEOUT(a.streamConnected() && b.streamConnected(),15000);
        QVERIFY(a.sendStreamControl({{"v",1},{"type","available"}}));
        QVERIFY(a.sendStreamPacket(QByteArray(12000,'x')));QVERIFY(a.send({{"text","ordinary text"}}));
        QTRY_COMPARE(control.size(),1);QTRY_COMPARE(media.size(),1);QTRY_COMPARE(text.size(),1);
        QCOMPARE(media.first().first().toByteArray(),QByteArray(12000,'x'));
        QVERIFY(!a.sendStreamPacket(QByteArray(16385,'x')));QVERIFY(!a.sendStreamControl({{"large",QString(2049,'x')}}));
    }
    void perPeerEncryptedSavesSurviveRestart() {
        QTemporaryDir root;
        QString own, peerB, peerC;
        {
            RelayPeers a(root.path() + "/a", {}), b(root.path() + "/b", {}), c(root.path() + "/c", {});
            QVERIFY(a.m_initialized); own = a.identity(); peerB = b.identity(); peerC = c.identity();
            QCOMPARE(a.addContact(b.contactCard(), "B", "local-account:dm-b"), peerB);
            QCOMPARE(a.addContact(c.contactCard(), "C", "local-account:dm-c"), peerC);
            QVERIFY(a.sendText(peerB, "Private message for B — 你好"));
            QVERIFY(a.sendText(peerC, "Separate message for C"));
            QCOMPARE(a.messagesForPeer(peerB).size(), 1);
            QCOMPARE(a.messagesForPeer(peerC).size(), 1);
            QCOMPARE(a.peerForBinding("local-account:dm-b"), peerB);
            QFile file(root.path() + "/a/" + peerB + ".relay"); QVERIFY(file.open(QIODevice::ReadOnly));
            const auto encrypted = file.readAll(); QVERIFY(!encrypted.contains("Private message")); QVERIFY(!encrypted.contains("text"));
#ifdef Q_OS_UNIX
            const auto permissions = QFile::permissions(root.path() + "/a/identity.key");
            QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
#endif
            RelayPeers same(root.path() + "/a", {}); QVERIFY(!same.m_initialized);
        }
        RelayPeers restored(root.path() + "/a", {});
        QVERIFY(restored.m_initialized); QCOMPARE(restored.identity(), own);
        QCOMPARE(restored.contacts().size(), 2);
        QCOMPARE(restored.messagesForPeer(peerB).first().toMap().value("text").toString(), "Private message for B — 你好");
        QCOMPARE(restored.messagesForPeer(peerC).first().toMap().value("text").toString(), "Separate message for C");
        QVERIFY(!restored.enabled()); QVERIFY(!restored.mcpEnabled());
    }
    void damagedSaveFailsClosedAndIsPreserved() {
        QTemporaryDir root; QString peer;
        {
            RelayPeers a(root.path() + "/a", {}), b(root.path() + "/b", {}); peer = a.addContact(b.contactCard(), "B");
            QVERIFY(a.sendText(peer, "Preserve this"));
        }
        const auto path = root.path() + "/a/" + peer + ".relay";
        QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); auto bytes = file.readAll(); bytes[35] = char(bytes[35] ^ 1);
        file.seek(0); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        RelayPeers restored(root.path() + "/a", {});
        QVERIFY(restored.messagesForPeer(peer).isEmpty());
        QVERIFY(!restored.sendText(peer, "Cannot overwrite damaged history"));
        QVERIFY(!restored.clearHistory(peer));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), bytes);
    }
    void missingKeyNeverReplacesExistingSaves() {
        QTemporaryDir root; QString identity;
        {
            RelayPeers a(root.path() + "/a", {}), b(root.path() + "/b", {});
            const auto peer = a.addContact(b.contactCard(), "B"); QVERIFY(a.sendText(peer, "Keep"));
            identity = a.identity();
        }
        QVERIFY(QFile::remove(root.path() + "/a/identity.key"));
        RelayPeers broken(root.path() + "/a", {});
        QVERIFY(!broken.m_initialized); QVERIFY(!QFile::exists(root.path() + "/a/identity.key"));
    }
    void contactValidationAndTextOnlyLimits() {
        QTemporaryDir root;
        RelayPeers a(root.path() + "/a", {}), b(root.path() + "/b", {});
        QVERIFY(a.addContact(a.contactCard(), "self").isEmpty());
        QVERIFY(a.addContact("not a card", "unknown").isEmpty());
        const auto peer = a.addContact(b.contactCard(), "B"); QVERIFY(!peer.isEmpty());
        QVERIFY(!a.sendText(peer, "  \n ")); QVERIFY(!a.sendText(peer, QString(4001, 'x')));
        QVERIFY(!a.sendText("../../bad", "text"));
        QVERIFY(a.sendText(peer, "<img src='https://invalid.example/x'> plain text; no preview"));
        QVERIFY(a.sendText(peer, QString(1000, QChar(0x4e00))));
        QVERIFY(!a.sendText(peer, QString(1400, QChar(0x4e00))));
        const auto old = a.rendezvousUrl(); a.setRendezvousUrl("ws://insecure.example"); QCOMPARE(a.rendezvousUrl(), old);
        a.setRendezvousUrl("wss://different.example/setup"); QVERIFY(a.addContact(b.contactCard(), "wrong service").isEmpty());
    }
    void setupRejectsForgedIdentityAndReplayedLogin() {
        RelayRendezvous server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QWebSocket socket;
        QSignalSpy messages(&socket, &QWebSocket::textMessageReceived);
        socket.open(QUrl("ws://127.0.0.1:" + QString::number(server.port())));
        QTRY_COMPARE(messages.size(), 1);
        unsigned char publicKey[crypto_sign_PUBLICKEYBYTES], secretKey[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(publicKey, secretKey);
        const auto key = QString::fromLatin1(QByteArray(reinterpret_cast<char *>(publicKey), sizeof publicKey).toHex());
        auto challenge = QJsonDocument::fromJson(messages.takeFirst().first().toString().toUtf8()).object().value("challenge").toString();
        const auto data = QByteArray("relay-rendezvous-v1:") + QByteArray::fromBase64(challenge.toLatin1());
        QByteArray signature(crypto_sign_BYTES, Qt::Uninitialized);
        crypto_sign_detached(reinterpret_cast<unsigned char *>(signature.data()), nullptr, reinterpret_cast<const unsigned char *>(data.constData()), size_t(data.size()), secretKey);
        const QJsonObject login{{"key", key}, {"signature", QString::fromLatin1(signature.toBase64())}};
        socket.sendTextMessage(QString::fromUtf8(QJsonDocument(login).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE(server.onlineCount(), 1);
        socket.close(); QTRY_COMPARE(server.onlineCount(), 0);
        QWebSocket replay;
        QSignalSpy replayMessages(&replay, &QWebSocket::textMessageReceived);
        replay.open(QUrl("ws://127.0.0.1:" + QString::number(server.port())));
        QTRY_COMPARE(replayMessages.size(), 1);
        replay.sendTextMessage(QString::fromUtf8(QJsonDocument(login).toJson(QJsonDocument::Compact)));
        QTRY_COMPARE(replay.state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(server.onlineCount(), 0);
        sodium_memzero(secretKey, sizeof secretKey);
    }
    void forgedSetupAndSwappedHistoriesAreRejected() {
        QTemporaryDir root; QString peerB, peerC;
        {
            RelayPeers a(root.path() + "/a", {}), b(root.path() + "/b", {}), c(root.path() + "/c", {});
            peerB = a.addContact(b.contactCard(), "B"); peerC = a.addContact(c.contactCard(), "C");
            QVERIFY(a.sendText(peerB, "B private")); QVERIFY(a.sendText(peerC, "C private"));
            a.m_enabled = true; a.m_ready = true;
            a.signalReceived({{"from", peerB}, {"payload", QString::fromLatin1(QByteArray(100, 'x').toBase64())}});
            QVERIFY(a.m_transports.isEmpty());
        }
        QFile source(root.path() + "/a/" + peerB + ".relay"); QVERIFY(source.open(QIODevice::ReadOnly)); const auto bytes = source.readAll();
        QFile target(root.path() + "/a/" + peerC + ".relay"); QVERIFY(target.open(QIODevice::WriteOnly)); QCOMPARE(target.write(bytes), bytes.size()); target.close();
        RelayPeers restored(root.path() + "/a", {});
        QVERIFY(restored.messagesForPeer(peerC).isEmpty());
        QVERIFY(!restored.sendText(peerC, "cannot overwrite"));
        QCOMPARE(restored.messagesForPeer(peerB).size(), 1);
    }
    void actualDirectDeliveryPersistsAndSurvivesSetupDisconnect() {
        QTemporaryDir root; RelayRendezvous server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        RelayPeers a(root.path() + "/a", {}), b(root.path() + "/b", {}), stranger(root.path() + "/stranger", {});
        const auto peerB = a.addContact(b.contactCard(), "B"); const auto peerA = b.addContact(a.contactCard(), "A");
        for (int i = 0; i < 35; ++i) QVERIFY(a.sendText(peerB, "Offline queued message " + QString::number(i)));
        enableLocal(a, server.port()); enableLocal(b, server.port()); enableLocal(stranger, server.port());
        QTRY_VERIFY_WITH_TIMEOUT(a.m_ready && b.m_ready && stranger.m_ready, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(a.m_transports.contains(peerB) && a.m_transports.value(peerB)->connected(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(b.m_transports.contains(peerA) && b.m_transports.value(peerA)->connected(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(b.messagesForPeer(peerA).size(), 35, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!a.messagesForPeer(peerB).last().toMap().value("pending").toBool(), 5000);
        a.m_signal.close(); b.m_signal.close(); a.m_poll.stop(); b.m_poll.stop();
        QTRY_VERIFY(!a.m_ready && !b.m_ready);
        QVERIFY(a.sendText(peerB, "Text crosses the direct channel after setup service disconnects"));
        QTRY_COMPARE_WITH_TIMEOUT(b.messagesForPeer(peerA).size(), 36, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!a.messagesForPeer(peerB).first().toMap().value("pending").toBool(), 5000);
        QCOMPARE(b.messagesForPeer(peerA).last().toMap().value("text").toString(), "Text crosses the direct channel after setup service disconnects");
        const auto row = a.m_contacts[peerB].history.first().toObject();
        QVERIFY(a.m_transports[peerB]->send({{"v", 1}, {"type", "text"}, {"id", row.value("id")}, {"text", row.value("text")}}));
        QTest::qWait(100); QCOMPARE(b.messagesForPeer(peerA).size(), 36); // Retries do not duplicate records.
        // A protocol frame carrying attachments is rejected, rather than interpreted by another service.
        QVERIFY(a.m_transports[peerB]->send({{"v", 1}, {"type", "file"}, {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"url", "https://invalid.example/file"}}));
        QTRY_VERIFY_WITH_TIMEOUT(!b.m_transports.contains(peerA), 5000);
        QCOMPARE(stranger.messages().size(), 0);
    }
    void rapidShutdownDoesNotRaceNetworkCallbacks() {
        QTemporaryDir root;
        RelayRendezvous server; QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        for (int i = 0; i < 8; ++i) {
            auto a = std::make_unique<RelayPeers>(root.path() + "/a" + QString::number(i), QStringList{});
            auto b = std::make_unique<RelayPeers>(root.path() + "/b" + QString::number(i), QStringList{});
            QVERIFY(!a->addContact(b->contactCard(), "B").isEmpty());
            QVERIFY(!b->addContact(a->contactCard(), "A").isEmpty());
            enableLocal(*a, server.port()); enableLocal(*b, server.port());
            QTRY_VERIFY_WITH_TIMEOUT(a->m_ready && b->m_ready, 3000);
            QCoreApplication::processEvents();
            a.reset(); b.reset(); QCoreApplication::processEvents();
        }
    }
    void mcpGrantsPaginationAndNoDiscordSurface() {
        QTemporaryDir root;
        RelayPeers a(root.path() + "/a", {}), b(root.path() + "/b", {}), c(root.path() + "/c", {});
        const auto peerB = a.addContact(b.contactCard(), "B"); const auto peerC = a.addContact(c.contactCard(), "C");
        a.setMcpEnabled(true); QVERIFY2(a.mcpEnabled(), qPrintable(a.error()));
        QVERIFY(a.mcpRequest(rpc("relay_read_messages", {{"peer", peerB}})).value("result").toObject().value("isError").toBool());
        a.setAiAccess(peerB, true);
        QVERIFY(a.sendText(peerB, "one")); QVERIFY(a.sendText(peerB, "two")); QVERIFY(a.sendText(peerC, "C stays private"));
        auto page = payload(a.mcpRequest(rpc("relay_read_messages", {{"peer", peerB}, {"limit", 1}})));
        QCOMPARE(page.value("messages").toArray().size(), 1); QVERIFY(page.value("hasMore").toBool()); QVERIFY(page.value("untrustedPeerContent").toBool());
        const auto cursor = page.value("nextCursor").toString();
        page = payload(a.mcpRequest(rpc("relay_read_messages", {{"peer", peerB}, {"after", cursor}, {"limit", 1}})));
        QCOMPARE(page.value("messages").toArray().first().toObject().value("text").toString(), "two"); QVERIFY(!page.value("hasMore").toBool());
        QVERIFY(a.mcpRequest(rpc("relay_send_text", {{"peer", peerC}, {"text", "not authorized"}})).value("result").toObject().value("isError").toBool());
        QVERIFY(a.mcpRequest(rpc("discord_send_message", {{"channel", "123"}, {"text", "forbidden"}})).contains("error"));
        QVERIFY(a.mcpRequest(rpc("relay_send_text", {{"peer", peerB}, {"text", "text"}, {"attachments", QJsonArray{}}})).contains("error"));
        QVERIFY(a.mcpRequest(rpc("relay_read_messages", {{"peer", peerB}, {"limit", 101}})).contains("error"));
        QVERIFY(!payload(a.mcpRequest(rpc("relay_send_text", {{"peer", peerB}, {"text", "Authorized AI text"}}))).value("id").toString().isEmpty());
        a.setAiAccess(peerB, false);
        QVERIFY(a.mcpRequest(rpc("relay_read_messages", {{"peer", peerB}})).value("result").toObject().value("isError").toBool());
        a.setMcpEnabled(false); QVERIFY(a.mcpRequest(rpc("relay_list_conversations")).contains("error"));
    }
    void stdioMcpLifecycleUsesPrivatePipe() {
        QTemporaryDir root;
        RelayPeers a(root.path() + "/a", {}), b(root.path() + "/b", {});
        const auto peer = a.addContact(b.contactCard(), "B"); a.setAiAccess(peer, true); a.setMcpEnabled(true); QVERIFY(a.mcpEnabled());
        QProcess process; process.start(QCoreApplication::applicationFilePath(), {"--mcp-fixture", a.mcpEndpoint()}); QVERIFY(process.waitForStarted());
        auto send = [&](const QJsonObject &request) { process.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n'); };
        send({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{{"protocolVersion", "2025-11-25"}}}});
        send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        send({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}});
        send(rpc("relay_send_text", {{"peer", peer}, {"text", "MCP process to actual local Relay pipe"}}));
        process.closeWriteChannel();
        QTRY_COMPARE_WITH_TIMEOUT(process.state(), QProcess::NotRunning, 10000);
        QCOMPARE(process.exitCode(), 0);
        const auto lines = process.readAllStandardOutput().trimmed().split('\n'); QCOMPARE(lines.size(), 3);
        const auto tools = QJsonDocument::fromJson(lines[1]).object().value("result").toObject().value("tools").toArray(); QCOMPARE(tools.size(), 3);
        QVERIFY(!lines[1].contains("discord_"));
        QCOMPARE(a.messagesForPeer(peer).size(), 1);
        QCOMPARE(a.messagesForPeer(peer).first().toMap().value("text").toString(), "MCP process to actual local Relay pipe");
    }
};
static int probeSetupService(QCoreApplication &app, const QString &url) {
    QTemporaryDir root;
    RelayPeers a(root.path() + "/a"), b(root.path() + "/b");
    a.setRendezvousUrl(url); b.setRendezvousUrl(url);
    const auto peerB = a.addContact(b.contactCard(), "Generated deployment probe B");
    const auto peerA = b.addContact(a.contactCard(), "Generated deployment probe A");
    if (peerB.isEmpty() || peerA.isEmpty()) return 2;
    bool sent = false;
    QTimer pulse;
    pulse.setInterval(100);
    QObject::connect(&pulse, &QTimer::timeout, &app, [&] {
        bool connected = false;
        for (const auto &value : a.contacts()) if (value.toMap().value("key").toString() == peerB) connected = value.toMap().value("connected").toBool();
        if (!sent && connected) { sent = true; if (!a.sendText(peerB, "Generated Relay deployment health probe")) app.exit(2); }
        const auto received = b.messagesForPeer(peerA);
        const auto outgoing = a.messagesForPeer(peerB);
        if (sent && received.size() == 1 && outgoing.size() == 1 && !outgoing.first().toMap().value("pending").toBool()) {
            std::puts("{\"setupAuthenticated\":true,\"directConnected\":true,\"peerSaved\":true,\"generatedIdentitiesOnly\":true}");
            app.exit(0);
        }
    });
    QTimer::singleShot(45000, &app, [&app] { std::fputs("Generated setup/direct health probe timed out.\n", stderr); app.exit(2); });
    pulse.start(); a.setEnabled(true); b.setEnabled(true);
    return app.exec();
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv); app.setApplicationName("Relay-peer-fixture");
    if (argc == 3 && QByteArray(argv[1]) == "--setup-probe") return probeSetupService(app, QString::fromLocal8Bit(argv[2]));
    if (argc == 3 && QByteArray(argv[1]) == "--mcp-fixture") return RelayPeers::runMcp(QString::fromLocal8Bit(argv[2]));
    RelayPeersTest test; return QTest::qExec(&test, argc, argv);
}
#include "relay_peers_test.moc"
