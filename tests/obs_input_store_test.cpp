#include "ObsInputStore.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <qt6keychain/keychain.h>

// Every started job is replaced with an in-memory asynchronous fixture.
// Real QtKeychain jobs below are inspected only; they are never started.
class ObsInputStoreTest final : public QObject {
    Q_OBJECT
    using Operation = ObsInputStore::Operation;
    struct Pending { Operation operation; QByteArray bytes; ObsInputStore::Completion complete; };
    QList<Pending> requests;
    static QString address() { return "rtmps://fixture-user:fixture-password@example.invalid:443/live/full-fixture-secret?access=fixture-query-secret"; }
    static QString streamKey() { return "fixture-stream-secret?token=fixture-key-secret"; }
    static QByteArray payload(int mode = 1) { return QJsonDocument(QJsonObject{{"version", 1}, {"mode", mode}, {"server", address()}, {"key", streamKey()}}).toJson(QJsonDocument::Compact); }
    void attach(ObsInputStore &store) { store.m_executor = [this](Operation operation, const QByteArray &bytes, ObsInputStore::Completion complete) { requests.append({operation, bytes, std::move(complete)}); }; }
    void finish(int result, QByteArray value = {}) { QVERIFY(!requests.isEmpty()); auto pending = requests.takeFirst(); pending.complete(result, std::move(value)); }
private slots:
    void init() { requests.clear(); }
    void defaultConstructionHasNoWalletWorkAndDedicatedJobsNeverFallback() {
        QTemporaryDir directory; ObsInputStore store(directory.filePath("input.ini")); attach(store); QVERIFY(requests.isEmpty()); QVERIFY(!store.busy()); QVERIFY(store.remember());
        for (const auto operation : {Operation::Read, Operation::Write, Operation::Delete}) {
            auto *job = store.createJob(operation, payload()); QCOMPARE(job->service(), QString("org.relayclient.obs")); QCOMPARE(job->key(), QString("rtmp-input-v1")); QVERIFY(!job->insecureFallback()); delete job;
        }
        store.restore(); QCOMPARE(requests.size(), 1); QCOMPARE(requests.first().operation, Operation::Read); QVERIFY(requests.first().bytes.isEmpty()); store.restore(); QCOMPARE(requests.size(), 1);
        finish(QKeychain::EntryNotFound); QVERIFY(!store.saved()); QVERIFY(!store.busy()); QVERIFY(store.error().isEmpty());
    }
    void completeUrlKeyAndModeRestoreAcrossLaunchWithoutPlaintextSettings() {
        QTemporaryDir directory; const auto file = directory.filePath("input.ini"); QByteArray wallet;
        {
            ObsInputStore store(file); attach(store); store.save(1, address(), streamKey()); QCOMPARE(requests.size(), 1); QCOMPARE(requests.first().operation, Operation::Write);
            wallet = requests.first().bytes; QCOMPARE(wallet, payload()); QVERIFY(!store.m_preferences->value("input/restoreAllowed").toBool()); finish(QKeychain::NoError); QVERIFY(store.saved()); QVERIFY(store.m_preferences->value("input/restoreAllowed").toBool());
            QFile preferences(file); QVERIFY(preferences.open(QIODevice::ReadOnly)); const auto contents = preferences.readAll(); QVERIFY(!contents.contains("fixture")); QVERIFY(!contents.contains("rtmp")); QVERIFY(!contents.contains("password")); QVERIFY(!contents.contains("token"));
        }
        ObsInputStore next(file); attach(next); QSignalSpy restored(&next, &ObsInputStore::restored); next.restore(); QCOMPARE(requests.size(), 1); finish(QKeychain::NoError, wallet);
        QCOMPARE(restored.size(), 1); QCOMPARE(restored.first().value(0).toInt(), 1); QCOMPARE(restored.first().value(1).toString(), address()); QCOMPARE(restored.first().value(2).toString(), streamKey()); QVERIFY(next.saved()); QVERIFY(next.status().contains("Connect when"));
        // The store has no capture, network input or broadcasting API. Restore
        // emits only these fields for the bridge to prefill.
        QVERIFY(next.metaObject()->indexOfMethod("start()") < 0);
    }
    void earlyEditsCancelLateRestoreAndLatestWriteWins() {
        QTemporaryDir directory; ObsInputStore store(directory.filePath("input.ini")); attach(store); QSignalSpy restored(&store, &ObsInputStore::restored);
        store.restore(); store.cancelRestore(); store.save(0, "rtmp://new.example.invalid/live", "new-fixture"); QCOMPARE(requests.size(), 1);
        finish(QKeychain::NoError, payload()); QCOMPARE(restored.size(), 0); QCOMPARE(requests.size(), 1); QCOMPARE(requests.first().operation, Operation::Write);
        store.save(1, "rtmp://latest.example.invalid/live", "latest-fixture"); finish(QKeychain::NoError); QCOMPARE(requests.size(), 1);
        const auto latest = QJsonDocument::fromJson(requests.first().bytes).object(); QCOMPARE(latest.value("mode").toInt(), 1); QCOMPARE(latest.value("key").toString(), QString("latest-fixture"));
        finish(QKeychain::NoError); QVERIFY(store.saved()); QVERIFY(!store.busy());
    }
    void forgetQueuesAfterPendingWriteAndDisablesRestoreEvenIfWalletDeleteFails() {
        QTemporaryDir directory; const auto file = directory.filePath("input.ini"); ObsInputStore store(file); attach(store); store.save(1, address(), streamKey()); store.clear();
        QVERIFY(!store.remember()); QVERIFY(!store.m_preferences->value("input/restoreAllowed").toBool()); QCOMPARE(requests.size(), 1); QCOMPARE(requests.first().operation, Operation::Write);
        finish(QKeychain::NoError); QCOMPARE(requests.size(), 1); QCOMPARE(requests.first().operation, Operation::Delete); finish(QKeychain::AccessDeniedByUser); QVERIFY(!store.saved()); QVERIFY(!store.error().isEmpty());
        ObsInputStore next(file); attach(next); QVERIFY(!next.remember()); next.restore(); QVERIFY(requests.isEmpty());
        next.setRemember(true); next.save(0, {}, {}); QCOMPARE(requests.size(), 1); finish(QKeychain::NoError); QVERIFY(next.remember()); QVERIFY(next.saved());
    }
    void forgettingPendingRestoreNeverRepopulatesFields() {
        QTemporaryDir directory; ObsInputStore store(directory.filePath("input.ini")); attach(store); QSignalSpy restored(&store, &ObsInputStore::restored);
        store.restore(); store.clear(); finish(QKeychain::NoError, payload()); QCOMPARE(restored.size(), 0); QCOMPARE(requests.size(), 1); QCOMPARE(requests.first().operation, Operation::Delete); finish(QKeychain::NoError); QVERIFY(!store.saved()); QVERIFY(!store.remember());
    }
    void unavailableWalletKeepsCurrentInputMemoryOnlyAndDoesNotRetry() {
        QTemporaryDir directory; const auto file = directory.filePath("input.ini"); ObsInputStore store(file); attach(store); store.save(1, address(), streamKey()); finish(QKeychain::NoBackendAvailable);
        QVERIFY(!store.saved()); QVERIFY(!store.error().isEmpty()); QVERIFY(store.status().contains("this session")); QVERIFY(!store.error().contains("fixture")); QVERIFY(!store.status().contains("fixture"));
        QTest::qWait(30); QVERIFY(requests.isEmpty()); ObsInputStore next(file); attach(next); next.restore(); QVERIFY(requests.isEmpty());
        store.save(1, address(), streamKey()); QCOMPARE(requests.size(), 1); finish(QKeychain::NoError); QVERIFY(store.saved());
    }
    void boundedValidationRejectsMalformedSavedInputsAndNeverEchoesValues() {
        QTemporaryDir directory; const auto file = directory.filePath("input.ini"); ObsInputStore store(file); attach(store); QSignalSpy restored(&store, &ObsInputStore::restored);
        for (const auto &bad : {QByteArray("{\"server\":\"fixture-private\"}"), QByteArray("[]"), QByteArray(32769, 'x'), QByteArray("{\"version\":1,\"mode\":9,\"server\":\"fixture-private\",\"key\":\"secret\"}")}) {
            store.restore(); QCOMPARE(requests.size(), 1); finish(QKeychain::NoError, bad); QCOMPARE(restored.size(), 0); QVERIFY(!store.saved()); QVERIFY(!store.error().contains("fixture")); QVERIFY(!store.error().contains("secret"));
        }
        store.save(1, QString(4097, 'x'), "key"); store.save(1, "rtmp://example.invalid/live", QString(2049, 'x')); store.save(2, {}, {}); store.save(1, "rtmp://example.invalid/\nprivate", "key"); QVERIFY(requests.isEmpty());
    }
    void simultaneousPreferenceAndWalletFailuresAreReportedTruthfully() {
        QTemporaryDir directory; const auto file = directory.filePath("is-a-directory"); QVERIFY(QDir().mkpath(file)); ObsInputStore store(file); attach(store);
        store.clear(); QCOMPARE(requests.size(), 1); finish(QKeychain::AccessDeniedByUser); QVERIFY(store.error().contains("may still restore")); QVERIFY(!store.status().contains("only for this session"));
        store.setRemember(true); store.save(1, address(), streamKey()); QCOMPARE(requests.size(), 1); finish(QKeychain::NoBackendAvailable); QVERIFY(store.error().contains("older saved input"));
    }
    void destroyedStoreIgnoresLateReadCompletion() {
        QTemporaryDir directory; auto store = std::make_unique<ObsInputStore>(directory.filePath("input.ini")); attach(*store); QSignalSpy restored(store.get(), &ObsInputStore::restored); store->restore(); QCOMPARE(requests.size(), 1); store.reset(); finish(QKeychain::NoError, payload()); QCOMPARE(restored.size(), 0);
    }
};
QTEST_GUILESS_MAIN(ObsInputStoreTest)
#include "obs_input_store_test.moc"
