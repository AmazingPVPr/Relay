#include "CredentialStatus.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class CredentialStatusTest final : public QObject {
    Q_OBJECT
private slots:
    void qmlFacadeContainsProgressAndCancellationWithoutSecretSignals() {
        QTemporaryDir directory;QVERIFY(directory.isValid());
        CredentialStore store(directory.filePath("generated.ini"));CredentialStatus view(&store);
        QVERIFY(store.metaObject()->indexOfSignal("restored(QString)")>=0);
        QCOMPARE(view.metaObject()->indexOfSignal("restored(QString)"),-1);
        QCOMPARE(view.metaObject()->indexOfMethod("save(QString)"),-1);
        QCOMPARE(view.metaObject()->indexOfMethod("restore()"),-1);
        QVERIFY(view.metaObject()->indexOfMethod("cancelRestore()")>=0);
        QVERIFY(!view.property("token").isValid());QVERIFY(view.children().isEmpty());
        QCOMPARE(view.status(),store.status());QCOMPARE(view.busy(),store.busy());
        QSignalSpy changed(&view,&CredentialStatus::changed);emit store.changed();QCOMPARE(changed.count(),1);
    }
};
QTEST_GUILESS_MAIN(CredentialStatusTest)
#include "credential_status_test.moc"
