#include "TranslationService.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtTest>
#include <functional>
#include <memory>

namespace {
class HttpFixture final : public QTcpServer {
public:
    QList<QByteArray> requests;
    std::function<void(QTcpSocket *)> respond;
    HttpFixture() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    if (!buffer->contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                    socket->setProperty("handled", true);
                    requests.append(*buffer);
                    if (respond) respond(socket);
                });
            }
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/translate_a/single").arg(serverPort())); }
    static void send(QTcpSocket *socket, const QByteArray &body, int status = 200, const QByteArray &extraHeaders = {}) {
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n" + extraHeaders + "\r\n" + body);
        socket->disconnectFromHost();
    }
};
const QByteArray HelloResponse = "[[[\"Hola\",\"Hello\",null,null,10]],null,\"en\"]";
}

class TranslationTest final : public QObject {
    Q_OBJECT
private slots:
    void parserJoinsSegmentsAndRetainsPlainText() {
        QString translated, detected;
        const QByteArray response = "[[[\"Hola <b>mundo</b>\\n\",\"Hello world\\n\"],[\"A+B & C\",\"A+B & C\"]],null,\"en\"]";
        QVERIFY(TranslationService::parseResponse(response, &translated, &detected));
        QCOMPARE(translated, QStringLiteral("Hola <b>mundo</b>\nA+B & C"));
        QCOMPARE(detected, QStringLiteral("en"));
        for (const auto &invalid : QList<QByteArray>{"{}", "not json", "[]", "[[{}]]", "[[[12]]]", "[[[null]]]", "[[[\"\"]]]", "[[[\"ok\"]],null,\"<script>\"]", "[[[\"ok\"]]] trailing"})
            QVERIFY2(!TranslationService::parseResponse(invalid, &translated, &detected), invalid.constData());
        const auto oversized = QJsonDocument(QJsonArray{QJsonArray{QJsonArray{QString(24001, u'x')}}}).toJson();
        QVERIFY(!TranslationService::parseResponse(oversized, &translated, &detected));
    }

    void onlyExplicitValidRequestsSendTextWithoutCredentials() {
        HttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [](QTcpSocket *socket) { HttpFixture::send(socket, HelloResponse); };
        TranslationService service;
        service.m_endpoint = fixture.url();
        QTest::qWait(20);
        QVERIFY(fixture.requests.isEmpty());
        QVERIFY(!service.translate(QString(4001, u'x'), "es"));
        QVERIFY(!service.translate("  \n ", "es"));
        QVERIFY(!service.translate("Hello", "../invalid"));
        QVERIFY(fixture.requests.isEmpty());
        const QString original = QString::fromUtf8("Hello + plus & ? # / %2B ñ 💛\nnext");
        QVERIFY(service.translate(original, "es"));
        QVERIFY(service.busy());
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 2000);
        QVERIFY2(service.error().isEmpty(), qPrintable(service.error()));
        QCOMPARE(service.translatedText(), QStringLiteral("Hola"));
        QCOMPARE(service.originalText(), original);
        QCOMPARE(service.targetLanguage(), QStringLiteral("es"));
        QCOMPARE(service.detectedLanguage(), QStringLiteral("en"));
        QCOMPARE(fixture.requests.size(), 1);
        const auto request = fixture.requests.first();
        const auto target = request.split(' ').value(1);
        const QUrlQuery query(QUrl::fromEncoded(target));
        QCOMPARE(query.queryItemValue("q", QUrl::FullyDecoded), original);
        QCOMPARE(query.queryItemValue("sl"), QStringLiteral("auto"));
        QCOMPARE(query.queryItemValue("tl"), QStringLiteral("es"));
        QVERIFY(!target.contains('+'));
        QVERIFY(!request.toLower().contains("authorization:"));
        QVERIFY(!request.toLower().contains("cookie:"));
        service.clear();
        QVERIFY(service.originalText().isEmpty());
        QVERIFY(service.translatedText().isEmpty());
        QVERIFY(service.error().isEmpty());
        QCOMPARE(service.targetLanguage(), QStringLiteral("es"));
    }

    void cancelAndReplacementIgnoreOldReplies() {
        HttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        QPointer<QTcpSocket> delayed;
        fixture.respond = [&](QTcpSocket *socket) {
            if (fixture.requests.size() == 1) delayed = socket;
            else HttpFixture::send(socket, HelloResponse);
        };
        TranslationService service;
        service.m_endpoint = fixture.url();
        QVERIFY(service.translate("Old request", "fr"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 1, 2000);
        QVERIFY(service.translate("Hello", "es"));
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 2000);
        QCOMPARE(service.translatedText(), QStringLiteral("Hola"));
        QCOMPARE(service.originalText(), QStringLiteral("Hello"));
        if (delayed && delayed->state() == QAbstractSocket::ConnectedState) HttpFixture::send(delayed, "[[[\"wrong\"]]]");
        QTest::qWait(30);
        QCOMPARE(service.translatedText(), QStringLiteral("Hola"));
        fixture.respond = [](QTcpSocket *) { };
        QVERIFY(service.translate("Cancel me", "es"));
        QTRY_COMPARE_WITH_TIMEOUT(fixture.requests.size(), 3, 2000);
        service.cancel();
        QVERIFY(!service.busy());
        QVERIFY(!service.m_deadline.isActive());
        QVERIFY(service.translatedText().isEmpty());
    }

    void deadlineRateLimitAndResponseLimitsAreHonest() {
        HttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        TranslationService service;
        service.m_endpoint = fixture.url();
        service.m_timeoutMs = 80;
        QVERIFY(service.translate("Hello", "es"));
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 2000);
        QVERIFY(!service.error().isEmpty());
        QCOMPARE(fixture.requests.size(), 1);
        fixture.respond = [](QTcpSocket *socket) { HttpFixture::send(socket, "private server diagnostic", 429); };
        service.m_timeoutMs = 2000;
        QVERIFY(service.translate("Hello", "es"));
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 2000);
        QVERIFY(service.error().contains("limiting"));
        QVERIFY(!service.error().contains("private"));
        fixture.respond = [](QTcpSocket *socket) { HttpFixture::send(socket, QByteArray(256 * 1024 + 1, 'x')); };
        QVERIFY(service.translate("Hello", "es"));
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 2000);
        QVERIFY(service.error().contains("too large"));
        QVERIFY(service.m_response.isEmpty());
        QVERIFY(service.translatedText().isEmpty());
        fixture.respond = [](QTcpSocket *socket) { HttpFixture::send(socket, "<html>challenge</html>"); };
        QVERIFY(service.translate("Hello", "es"));
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 2000);
        QVERIFY(service.error().contains("unexpected"));
    }

    void redirectsAreNotFollowed() {
        HttpFixture destination;
        QVERIFY(destination.listen(QHostAddress::LocalHost));
        HttpFixture fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        fixture.respond = [&](QTcpSocket *socket) { HttpFixture::send(socket, {}, 302, "Location: " + destination.url().toEncoded() + "\r\n"); };
        TranslationService service;
        service.m_endpoint = fixture.url();
        QVERIFY(service.translate("Hello", "es"));
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 2000);
        QVERIFY(!service.error().isEmpty());
        QVERIFY(destination.requests.isEmpty());
        QCOMPARE(fixture.requests.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TranslationTest)
#include "translation_test.moc"
