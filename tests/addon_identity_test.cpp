#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QSignalSpy>
#include "DiscordClient.h"
#include "addons/AddonIdentityBroker.h"

class AddonIdentityTest : public QObject {
    Q_OBJECT
    const QString appId="123456789012345678";
    const QString callback="https://identity.example.test/auth/discord/callback";
    const QString state="fixture_state_12345678901234567890";
    QUrl authorization(QString scope="identify", QString redirect={}) const {
        QUrl url("https://discord.com/oauth2/authorize"); QUrlQuery q;
        q.addQueryItem("client_id",appId);q.addQueryItem("response_type","code");q.addQueryItem("scope",scope);
        q.addQueryItem("redirect_uri",redirect.isEmpty()?callback:redirect);q.addQueryItem("state",state);url.setQuery(q);return url;
    }
    void prepare(DiscordClient &client) {
        client.m_token="generated.fixture.token.only";client.m_currentUserId="111111111111111111";client.m_currentUserName="Generated account";
    }
private slots:
    void rejectsScopeExpansionRedirectsAndWrongClient() {
        DiscordClient client; prepare(client); AddonIdentityBroker broker(&client,appId,QUrl(callback));
        QSignalSpy result(&broker,&AddonIdentityBroker::authorizationFinished);
        broker.authorize(authorization("identify email")); broker.authorize(authorization("identify","https://evil.invalid/callback"));
        auto bad=authorization(); QUrlQuery q(bad);q.removeAllQueryItems("client_id");q.addQueryItem("client_id","999999999999999999");bad.setQuery(q);broker.authorize(bad);
        bad=authorization();bad.setHost("discord.com.evil.invalid");broker.authorize(bad);
        bad=authorization();q=QUrlQuery(bad);q.addQueryItem("scope","bot");bad.setQuery(q);broker.authorize(bad);
        QCOMPARE(result.size(),5);
        for(const auto &entry:result){QVERIFY(entry[0].toUrl().isEmpty());QVERIFY(!entry[1].toString().isEmpty());}
        QVERIFY(broker.metaObject()->indexOfProperty("token")<0);
        QCOMPARE(broker.accountId(),QString("111111111111111111"));
    }
    void grantsOnlyOneUseCodeAndBindsState_data() {
        QTest::addColumn<QString>("suffix"); QTest::addColumn<bool>("accept");
        QTest::newRow("legacy-code-state") << QString{} << true;
        QTest::newRow("discord-issuer") << QString("&iss=https%3A%2F%2Fdiscord.com") << true;
        QTest::newRow("wrong-issuer") << QString("&iss=https%3A%2F%2Fevil.invalid") << false;
        QTest::newRow("duplicate-issuer") << QString("&iss=https%3A%2F%2Fdiscord.com&iss=https%3A%2F%2Fdiscord.com") << false;
        QTest::newRow("empty-issuer") << QString("&iss=") << false;
        QTest::newRow("issuer-path") << QString("&iss=https%3A%2F%2Fdiscord.com%2Fevil") << false;
        QTest::newRow("duplicate-code") << QString("&code=second") << false;
        QTest::newRow("duplicate-state") << QString("&state="+state) << false;
        QTest::newRow("unknown-field") << QString("&access_token=forbidden") << false;
        QTest::newRow("fragment") << QString("#fragment") << false;
    }
    void grantsOnlyOneUseCodeAndBindsState() {
        QFETCH(QString, suffix); QFETCH(bool, accept);
        QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost,0));
        DiscordClient client;prepare(client);client.m_apiBase="http://127.0.0.1:"+QString::number(server.serverPort())+"/api/v10";
        QByteArray seen;
        connect(&server,&QTcpServer::newConnection,this,[&]{
            auto *socket=server.nextPendingConnection();
            connect(socket,&QTcpSocket::readyRead,socket,[&,socket]{
                seen+=socket->readAll();if(!seen.contains("\r\n\r\n")||!seen.endsWith('}'))return;
                const auto body=QJsonDocument(QJsonObject{{"location",callback+"?code=one_use_fixture_code&state="+state+suffix}}).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();
            });
        });
        AddonIdentityBroker broker(&client,appId,QUrl(callback));QSignalSpy result(&broker,&AddonIdentityBroker::authorizationFinished);
        broker.authorize(authorization());QTRY_COMPARE_WITH_TIMEOUT(result.size(),1,3000);
        if (accept) {
            QVERIFY(result.first()[1].toString().isEmpty());QVERIFY(result.first()[0].toUrl().query().contains("one_use_fixture_code"));
        } else {
            QVERIFY(result.first()[0].toUrl().isEmpty());QVERIFY(!result.first()[1].toString().isEmpty());
        }
        QVERIFY(seen.startsWith("POST /api/v10/oauth2/authorize?"));QVERIFY(seen.contains("Authorization: generated.fixture.token.only"));
        QVERIFY(!result.first()[0].toUrl().toString().contains("generated.fixture.token"));
        QVERIFY(seen.contains("\"authorize\":true"));QVERIFY(seen.contains("scope=identify"));
    }
    void accountSwitchDiscardsInFlightApproval() {
        QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost,0));
        DiscordClient client;prepare(client);client.m_apiBase="http://127.0.0.1:"+QString::number(server.serverPort());
        QPointer<QTcpSocket> socket;
        connect(&server,&QTcpServer::newConnection,this,[&]{socket=server.nextPendingConnection();});
        AddonIdentityBroker broker(&client,appId,QUrl(callback));QSignalSpy result(&broker,&AddonIdentityBroker::authorizationFinished);
        broker.authorize(authorization());QTRY_VERIFY(socket);
        client.m_currentUserId="222222222222222222";++client.m_generation;
        const auto body=QJsonDocument(QJsonObject{{"location",callback+"?code=unused&state="+state}}).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();
        QTRY_COMPARE_WITH_TIMEOUT(result.size(),1,3000);QVERIFY(result.first()[0].toUrl().isEmpty());QVERIFY(result.first()[1].toString().contains("account changed"));
    }
    void refusesWhenNotSignedIn() {
        DiscordClient client;AddonIdentityBroker broker(&client,appId,QUrl(callback));QVERIFY(!broker.available());
        QSignalSpy result(&broker,&AddonIdentityBroker::authorizationFinished);broker.authorize(authorization());QCOMPARE(result.size(),1);
    }
};
QTEST_GUILESS_MAIN(AddonIdentityTest)
#include "addon_identity_test.moc"
