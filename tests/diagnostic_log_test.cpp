#include "DiagnosticLog.h"
#include <QtTest>

class ErrorSource : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
public:
    QString error;
    void setError(const QString &value) { error = value; emit changed(); }
signals:
    void changed();
};
class DiagnosticLogTest : public QObject {
    Q_OBJECT
private slots:
    void recordsRedactsDeduplicatesAndClears() {
        DiagnosticLog log(true);
        ErrorSource source;
        log.watch(&source, "Fixture", SIGNAL(changed()));
        source.setError("Failed https://example.test/secret /home/person/private abcdefghijklmnopqrstuvwxyz1234567890");
        const QString first = log.text();
        QVERIFY(first.contains("[Fixture]"));
        QVERIFY(first.contains("[address removed]"));
        QVERIFY(first.contains("[path removed]"));
        QVERIFY(first.contains("[value removed]"));
        QVERIFY(!first.contains("secret"));
        source.setError(source.error);
        QCOMPARE(log.text(), first);
        source.setError(""); source.setError("Failed again");
        QVERIFY(log.text().contains("Failed again"));
        log.obsEvent("Stream connection closed (broken pipe); the OBS input ended.");
        QVERIFY(log.text().contains("[OBS input]"));
        log.clear(); QVERIFY(log.text().isEmpty());
    }
};
QTEST_MAIN(DiagnosticLogTest)
#include "diagnostic_log_test.moc"
