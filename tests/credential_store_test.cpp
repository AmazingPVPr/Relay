#include "CredentialStore.h"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <qt6keychain/keychain.h>

// Jobs are replaced with an in-memory asynchronous fixture. These tests never
// access a desktop wallet, existing credentials, or a Discord account.
class CredentialStoreTest final : public QObject {
    Q_OBJECT
    using Operation = CredentialStore::Operation;
    struct Pending { Operation operation; QByteArray secret; CredentialStore::Completion complete; };
    QList<Pending> requests;
    void attach(CredentialStore &store) {
        store.m_executor = [this](Operation operation, const QByteArray &secret, CredentialStore::Completion completed) {
            requests.append({operation, secret, std::move(completed)});
        };
    }
    void finish(int result, QByteArray value = {}) {
        QVERIFY(!requests.isEmpty());
        auto pending = requests.takeFirst();
        pending.complete(result, std::move(value));
    }
    static QString fixtureToken() { return QStringLiteral("fixture-only-token-never-a-real-account-credential"); }
private slots:
    void init() { requests.clear(); }
    void onlyExplicitRestoreStartsWalletWorkAndJobsForbidPlaintextFallback() {
        QTemporaryDir directory; CredentialStore store(directory.filePath("preferences.ini")); attach(store);
        QVERIFY(requests.isEmpty()); QVERIFY(!store.busy());
        for (const auto operation : {Operation::Read, Operation::Write, Operation::Delete}) {
            auto *job = store.createJob(operation, fixtureToken().toUtf8());
            QCOMPARE(job->service(), "com.relay.desktop");
            QCOMPARE(job->key(), "discord-session");
            QVERIFY(!job->insecureFallback());
            delete job; // Never started: no wallet or service access.
        }
        store.restore(); QCOMPARE(requests.size(), 1); QVERIFY(store.busy());
        store.restore(); QCOMPARE(requests.size(), 1);
        finish(QKeychain::EntryNotFound); QVERIFY(!store.busy()); QVERIFY(!store.remembered());
    }
    void successfulStoreAndRestoreDoNotWriteTokensToPreferences() {
        QTemporaryDir directory; const auto path = directory.filePath("preferences.ini");
        CredentialStore store(path); attach(store);
        QSignalSpy restored(&store, &CredentialStore::restored), ready(&store, &CredentialStore::restoreFinished);
        store.save(fixtureToken()); QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().operation, Operation::Write); QCOMPARE(requests.first().secret, fixtureToken().toUtf8());
        QVERIFY(!store.m_preferences->value("session/restoreAllowed").toBool());
        finish(QKeychain::NoError); QVERIFY(store.remembered()); QVERIFY(!store.busy());
        QVERIFY(store.m_preferences->value("session/restoreAllowed").toBool());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto data = file.readAll();
        QVERIFY(!data.contains("fixture")); QVERIFY(!data.contains("token"));
        store.restore(); finish(QKeychain::NoError, fixtureToken().toUtf8());
        QCOMPARE(restored.size(), 1); QCOMPARE(restored.first().first().toString(), fixtureToken());
        QCOMPARE(ready.size(), 1); QVERIFY(ready.first().first().toBool());
    }
    void signoutSuppressesLateRestoreAndQueuesDeletion() {
        QTemporaryDir directory; const auto path = directory.filePath("preferences.ini");
        CredentialStore store(path); attach(store); QSignalSpy restored(&store, &CredentialStore::restored);
        QSignalSpy cleared(&store, &CredentialStore::cleared);
        store.restore(); store.clear(); QCOMPARE(requests.size(), 1);
        finish(QKeychain::NoError, fixtureToken().toUtf8()); QCOMPARE(restored.size(), 0);
        QCOMPARE(requests.size(), 1); QCOMPARE(requests.first().operation, Operation::Delete);
        finish(QKeychain::NoError); QCOMPARE(cleared.size(), 1); QVERIFY(!store.remembered());
        CredentialStore nextLaunch(path); attach(nextLaunch); nextLaunch.restore();
        QVERIFY(requests.isEmpty()); QVERIFY(!nextLaunch.busy());
    }
    void deletionCannotRaceAheadOfAnInFlightWrite() {
        QTemporaryDir directory; CredentialStore store(directory.filePath("preferences.ini")); attach(store);
        store.save(fixtureToken()); store.clear();
        QCOMPARE(requests.size(), 1); QCOMPARE(requests.first().operation, Operation::Write);
        finish(QKeychain::NoError); QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().operation, Operation::Delete);
        QVERIFY(!store.m_preferences->value("session/restoreAllowed").toBool());
        finish(QKeychain::NoError); QVERIFY(!store.busy()); QVERIFY(!store.remembered());
    }
    void failedDeleteStillDisablesAutomaticSigninAcrossLaunches() {
        QTemporaryDir directory; const auto path = directory.filePath("preferences.ini");
        CredentialStore store(path); attach(store);
        store.save(fixtureToken()); finish(QKeychain::NoError);
        store.clear(); finish(QKeychain::AccessDeniedByUser);
        QVERIFY(!store.error().isEmpty()); QVERIFY(!store.m_preferences->value("session/restoreAllowed").toBool());
        CredentialStore nextLaunch(path); attach(nextLaunch); nextLaunch.restore(); QVERIFY(requests.isEmpty());
    }
    void unavailableWalletIsMemoryOnlyAndErrorsContainNoSecrets() {
        QTemporaryDir directory; CredentialStore store(directory.filePath("preferences.ini")); attach(store);
        store.save(fixtureToken()); finish(QKeychain::NoBackendAvailable);
        QVERIFY(!store.remembered()); QVERIFY(!store.error().isEmpty());
        QVERIFY(store.status().contains("this session"));
        QVERIFY(!store.status().contains(fixtureToken())); QVERIFY(!store.error().contains(fixtureToken()));
        store.restore(); QVERIFY(requests.isEmpty());
    }
    void cancelledRestoreCannotOverrideNewQrSignin() {
        QTemporaryDir directory; CredentialStore store(directory.filePath("preferences.ini")); attach(store);
        QSignalSpy restored(&store, &CredentialStore::restored), ready(&store, &CredentialStore::restoreFinished);
        store.restore(); store.cancelRestore();
        QCOMPARE(ready.size(), 1); QVERIFY(!ready.first().first().toBool());
        store.save(fixtureToken()); finish(QKeychain::NoError, "old-fixture-token-that-must-not-be-restored");
        QCOMPARE(restored.size(), 0); QCOMPARE(requests.first().operation, Operation::Write);
        finish(QKeychain::NoError); QVERIFY(store.remembered());
    }
    void malformedSecretNeverEscapesAndPendingQueueIsBounded() {
        QTemporaryDir directory; CredentialStore store(directory.filePath("preferences.ini")); attach(store);
        QSignalSpy restored(&store, &CredentialStore::restored);
        store.save("short"); QVERIFY(requests.isEmpty());
        store.save(QString(9000, 'x')); QVERIFY(requests.isEmpty());
        store.restore(); finish(QKeychain::NoError, "bad\nfixture-secret-with-control");
        QCOMPARE(restored.size(), 0); QVERIFY(!store.error().isEmpty());
        store.save(fixtureToken());
        for (int i = 0; i < 100; ++i) store.save(fixtureToken() + QString::number(i));
        QCOMPARE(requests.size(), 1); QVERIFY(store.m_pending.has_value());
        finish(QKeychain::NoError); QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().secret, (fixtureToken() + "99").toUtf8());
        finish(QKeychain::NoError); QVERIFY(!store.busy());
    }
};

QTEST_GUILESS_MAIN(CredentialStoreTest)
#include "credential_store_test.moc"
