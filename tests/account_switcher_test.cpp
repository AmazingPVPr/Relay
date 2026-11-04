#include "AccountSwitcher.h"
#include "CredentialStore.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <qt6keychain/keychain.h>

// Generated credentials and an injected asynchronous wallet. No desktop wallet,
// existing account, real credential or network service is opened by this suite.
class AccountSwitcherTest final : public QObject {
    Q_OBJECT
    using Operation = CredentialStore::Operation;
    struct Pending { QString id; Operation operation; QByteArray secret; CredentialStore::Completion complete; };
    QList<Pending> requests;
    QList<QString> activations;
    static QString token(const QString &id) { return "generated-fixture-only-secret-for-account-"+id; }
    CredentialStore *attach(AccountSwitcher &service,const QString &id) {
        auto *store=service.storeFor(id);
        store->m_executor=[this,id](Operation operation,const QByteArray &secret,CredentialStore::Completion complete) {
            requests.append({id,operation,secret,std::move(complete)});
        };
        return store;
    }
    void setup(AccountSwitcher &service) { service.setSessionHandler([this](const QString &secret) { activations.append(secret); }); }
    void finish(int code=QKeychain::NoError,QByteArray value={}) {
        QVERIFY(!requests.isEmpty());auto request=requests.takeFirst();request.complete(code,std::move(value));
    }
    void remember(AccountSwitcher &service,const QString &id) {
        attach(service,id);service.captureAuthenticated(token(id));
        service.updateCurrentAccount(id,"Generated "+id,"https://cdn.discordapp.com/avatars/"+id+"/fixture.png");
        QCOMPARE(requests.size(),1);QCOMPARE(requests.first().operation,Operation::Write);finish();
    }
    static QByteArray readFile(const QString &path) { QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll(); }
private slots:
    void init() { requests.clear();activations.clear(); }
    void newWindowDoesNotReplaceCurrentSessionOrReadSecrets() {
        QTemporaryDir directory;AccountSwitcher service(directory.filePath("accounts.json"));setup(service);
        remember(service,"101");remember(service,"202");
        QString requested;
        service.setWindowHandler([&](const QString &id) { requested=id;return true; });
        const auto before=activations.size();
        QVERIFY(!service.openAccountWindow("../invalid"));
        QVERIFY(!service.openAccountWindow("303"));
        QVERIFY(service.openAccountWindow("101"));
        QCOMPARE(requested,QString("101"));QCOMPARE(service.currentAccountId(),QString("202"));
        QCOMPARE(activations.size(),before);QVERIFY(requests.isEmpty());QVERIFY(!service.busy());
        service.setWindowHandler([](const QString &) { return false; });
        QVERIFY(!service.openAccountWindow("101"));QVERIFY(!service.error().isEmpty());
        QCOMPARE(service.currentAccountId(),QString("202"));QVERIFY(requests.isEmpty());
    }
    void concurrentWindowsMergeRegistryChanges() {
        QTemporaryDir directory;const auto path=directory.filePath("accounts.json");
        AccountSwitcher first(path);setup(first);remember(first,"101");
        AccountSwitcher second(path);setup(second);remember(second,"202");
        first.updateCurrentAccount("101","Renamed first",{});
        AccountSwitcher check(path);
        QCOMPARE(check.accounts().size(),2);
        QCOMPARE(check.accounts().first().toMap().value("name").toString(),QString("Renamed first"));
        // A stale second window updating its own account must retain that rename.
        second.updateCurrentAccount("202","Renamed second",{});
        first.refreshAccounts();QCOMPARE(first.accounts().size(),2);
        QCOMPARE(first.accounts().first().toMap().value("name").toString(),QString("Renamed first"));
        QCOMPARE(first.accounts().last().toMap().value("name").toString(),QString("Renamed second"));
    }
    void constructionAndNamedKeysNeverStartWalletJobs() {
        QTemporaryDir directory;AccountSwitcher service(directory.filePath("accounts.json"));setup(service);
        QVERIFY(service.accounts().isEmpty());QVERIFY(requests.isEmpty());QVERIFY(!service.busy());
        auto *store=attach(service,"101");
        for(auto operation:{Operation::Read,Operation::Write,Operation::Delete}) {
            auto *job=store->createJob(operation,token("101").toUtf8());
            QCOMPARE(job->service(),"com.relay.desktop");QCOMPARE(job->key(),"discord-account-101");QVERIFY(!job->insecureFallback());delete job;
        }
        QVERIFY(requests.isEmpty());
        const auto &meta=AccountSwitcher::staticMetaObject;
        for(int i=meta.methodOffset();i<meta.methodCount();++i) {
            const auto method=meta.method(i);QVERIFY(!method.methodSignature().contains("token"));
            if(method.methodType()==QMetaMethod::Signal)QVERIFY(method.parameterCount()==0);
        }
        for(int i=meta.propertyOffset();i<meta.propertyCount();++i)QVERIFY(!QByteArray(meta.property(i).name()).contains("token"));
    }
    void identityVerificationPrecedesStorageAndMetadataHasNoSecret() {
        QTemporaryDir directory;const auto path=directory.filePath("accounts.json");AccountSwitcher service(path);setup(service);attach(service,"101");
        service.captureAuthenticated(token("101"));QCOMPARE(activations.size(),1);QVERIFY(requests.isEmpty());QVERIFY(service.busy());
        service.updateCurrentAccount({}, {}, {});QVERIFY(requests.isEmpty());
        service.updateCurrentAccount("101","Generated\nName","https://evil.invalid/a");QCOMPARE(requests.size(),1);
        QCOMPARE(requests.first().id,"101");QCOMPARE(requests.first().secret,token("101").toUtf8());finish();
        QCOMPARE(service.currentAccountId(),"101");QVERIFY(!service.busy());QVERIFY(service.accounts().first().toMap().value("remembered").toBool());
        const auto bytes=readFile(path);QVERIFY(!bytes.contains(token("101").toUtf8()));QVERIFY(!bytes.contains("secret"));QVERIFY(!bytes.contains("token"));
        QCOMPARE(service.accounts().first().toMap().value("name").toString(),"GeneratedName");QVERIFY(service.accounts().first().toMap().value("avatarUrl").toString().isEmpty());
        QCOMPARE(QFile::permissions(path)&(QFileDevice::ReadGroup|QFileDevice::WriteGroup|QFileDevice::ReadOther|QFileDevice::WriteOther),QFileDevice::Permissions{});
        QSignalSpy changed(&service,&AccountSwitcher::changed);
        service.updateCurrentAccount("101","Generated\nName","https://evil.invalid/a");QCOMPARE(changed.size(),0);
    }
    void switchOnlyActivatesSuccessfulSelectedWalletRead() {
        QTemporaryDir directory;AccountSwitcher service(directory.filePath("accounts.json"));setup(service);
        remember(service,"101");remember(service,"202");activations.clear();
        QVERIFY(service.switchAccount("101"));QCOMPARE(service.currentAccountId(),"202");QVERIFY(activations.isEmpty());
        QCOMPARE(requests.first().id,"101");QCOMPARE(requests.first().operation,Operation::Read);
        finish(QKeychain::NoError,token("101").toUtf8());QCOMPARE(activations,QList<QString>{token("101")});QVERIFY(service.busy());
        service.updateCurrentAccount({}, {}, {});service.updateCurrentAccount("101","Generated 101",{});QCOMPARE(requests.size(),1);finish();
        QCOMPARE(service.currentAccountId(),"101");QVERIFY(!service.busy());QVERIFY(!service.switchAccount("101"));
        QVERIFY(service.switchAccount("202"));finish(QKeychain::AccessDeniedByUser);
        QCOMPARE(service.currentAccountId(),"101");QCOMPARE(activations.size(),1);QVERIFY(!service.error().isEmpty());QVERIFY(!service.busy());
    }
    void cancelledReadNeverOverridesNewSignin() {
        QTemporaryDir directory;AccountSwitcher service(directory.filePath("accounts.json"));setup(service);
        remember(service,"101");remember(service,"202");activations.clear();
        QVERIFY(service.switchAccount("101"));service.cancelPending();
        service.captureAuthenticated(token("303"));attach(service,"303");
        finish(QKeychain::NoError,token("101").toUtf8());QCOMPARE(activations,QList<QString>{token("303")});
        service.updateCurrentAccount("303","Generated 303",{});finish();QCOMPARE(service.currentAccountId(),"303");
    }
    void cancelledWalletDialogDoesNotBlockQrOrStrandRepeatedSelection() {
        QTemporaryDir directory;AccountSwitcher service(directory.filePath("accounts.json"));setup(service);
        remember(service,"101");remember(service,"202");activations.clear();
        QVERIFY(service.switchAccount("101"));service.cancelPending();QVERIFY(!service.busy());QVERIFY(service.canAdd());
        QVERIFY(!service.switchAccount("101"));QVERIFY(!service.busy());QVERIFY(!service.error().isEmpty());
        QSignalSpy add(&service,&AccountSwitcher::addAccountRequested);service.addAccount();QCOMPARE(add.size(),1);
        service.captureAuthenticated(token("303"));finish(QKeychain::NoError,token("101").toUtf8());
        QCOMPARE(activations,QList<QString>{token("303")});attach(service,"303");service.updateCurrentAccount("303","Generated 303",{});finish();QVERIFY(!service.busy());
        QVERIFY(service.switchAccount("101"));finish(QKeychain::NoError,token("101").toUtf8());service.updateCurrentAccount("101","Generated 101",{});finish();
        QCOMPARE(service.currentAccountId(),"101");
    }
    void savingAccountChangesBlocksAllUserSessionChangesWithoutTouchingWalletOrMetadata() {
        QTemporaryDir directory;const auto path=directory.filePath("accounts.json");AccountSwitcher service(path);setup(service);
        remember(service,"101");remember(service,"202");activations.clear();const auto before=readFile(path);
        QSignalSpy add(&service,&AccountSwitcher::addAccountRequested),signout(&service,&AccountSwitcher::signOutRequested);
        service.setSessionBusy(true);QVERIFY(service.sessionChangeBlocked());QVERIFY(!service.canAdd());QVERIFY(!service.busy());
        QVERIFY(!service.switchAccount("101"));service.addAccount();service.removeAccount("101");service.removeAccount("202");service.signOutCurrent();
        service.captureAuthenticated(token("303"));
        QCOMPARE(service.currentAccountId(),"202");QCOMPARE(service.accounts().size(),2);QCOMPARE(readFile(path),before);
        QVERIFY(requests.isEmpty());QVERIFY(activations.isEmpty());QCOMPARE(add.size(),0);QCOMPARE(signout.size(),0);
        QVERIFY(service.error().contains("finish saving"));
        service.setSessionBusy(false);QVERIFY(!service.sessionChangeBlocked());QVERIFY(service.canAdd());QVERIFY(service.error().isEmpty());
        service.addAccount();QCOMPARE(add.size(),1);QVERIFY(service.switchAccount("101"));service.cancelPending();finish(QKeychain::NoError,token("101").toUtf8());
    }
    void settingsWriteStartedDuringWalletReadPreventsLateSessionReplacement() {
        QTemporaryDir directory;const auto path=directory.filePath("accounts.json");AccountSwitcher service(path);setup(service);
        remember(service,"101");remember(service,"202");activations.clear();const auto before=readFile(path);
        QSignalSpy signout(&service,&AccountSwitcher::signOutRequested);
        QVERIFY(service.switchAccount("101"));QCOMPARE(requests.size(),1);service.setSessionBusy(true);
        finish(QKeychain::NoError,token("101").toUtf8());
        QCOMPARE(service.currentAccountId(),"202");QVERIFY(activations.isEmpty());QCOMPARE(signout.size(),0);QVERIFY(requests.isEmpty());
        QVERIFY(!service.busy());QVERIFY(service.sessionChangeBlocked());QVERIFY(service.error().contains("finish saving"));
        QVERIFY(service.m_stagedSecret.isEmpty());QVERIFY(service.m_expectedId.isEmpty());QCOMPARE(readFile(path),before);
        service.setSessionBusy(false);QVERIFY(service.switchAccount("101"));finish(QKeychain::NoError,token("101").toUtf8());
        QCOMPARE(activations,QList<QString>{token("101")});service.updateCurrentAccount("101","Generated 101",{});finish();
        QCOMPARE(service.currentAccountId(),"101");
    }
    void reentrantSettingsWriteAtActivationStillPreventsSessionCallback() {
        QTemporaryDir directory;AccountSwitcher service(directory.filePath("accounts.json"));setup(service);
        remember(service,"101");remember(service,"202");activations.clear();
        const auto connection=connect(&service,&AccountSwitcher::changed,&service,[&] {
            if (service.m_waitingForIdentity) service.setSessionBusy(true);
        });
        QVERIFY(service.switchAccount("101"));finish(QKeychain::NoError,token("101").toUtf8());
        QCOMPARE(service.currentAccountId(),"202");QVERIFY(activations.isEmpty());QVERIFY(!service.busy());QVERIFY(service.sessionChangeBlocked());
        QVERIFY(service.m_stagedSecret.isEmpty());QVERIFY(service.m_expectedId.isEmpty());QVERIFY(service.error().contains("finish saving"));
        service.setSessionBusy(false);service.captureAuthenticated(token("303"));
        QCOMPARE(service.currentAccountId(),"202");QVERIFY(activations.isEmpty());QVERIFY(!service.busy());QVERIFY(service.m_stagedSecret.isEmpty());
        disconnect(connection);service.setSessionBusy(false);
    }
    void savedIdentityMismatchCannotOverwriteAnotherAccount() {
        QTemporaryDir directory;AccountSwitcher service(directory.filePath("accounts.json"));setup(service);
        remember(service,"101");remember(service,"202");activations.clear();QSignalSpy signout(&service,&AccountSwitcher::signOutRequested);
        QVERIFY(service.switchAccount("101"));finish(QKeychain::NoError,token("101").toUtf8());
        service.updateCurrentAccount("999","Wrong generated account",{});QCOMPARE(signout.size(),1);QVERIFY(requests.isEmpty());
        QVERIFY(service.currentAccountId().isEmpty());QVERIFY(!service.busy());QCOMPARE(service.accounts().size(),2);QVERIFY(!service.error().isEmpty());
    }
    void signoutDuringWriteQueuesDeletionAndKeepsOtherAccounts() {
        QTemporaryDir directory;AccountSwitcher service(directory.filePath("accounts.json"));setup(service);remember(service,"202");attach(service,"101");
        service.captureAuthenticated(token("101"));service.updateCurrentAccount("101","Generated 101",{});
        QCOMPARE(requests.size(),1);QSignalSpy signout(&service,&AccountSwitcher::signOutRequested);service.signOutCurrent();
        QCOMPARE(signout.size(),1);QCOMPARE(service.accounts().size(),1);QCOMPARE(service.accounts().first().toMap().value("id").toString(),"202");
        finish();QCOMPARE(requests.size(),1);QCOMPARE(requests.first().operation,Operation::Delete);finish();
        QVERIFY(!service.busy());QVERIFY(service.currentAccountId().isEmpty());QVERIFY(!service.switchAccount("101"));
        QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(!service.m_stores.contains("101"));
    }
    void rotationSavesWithoutReloginAndRejectionOnlyInvalidatesSelectedAccount() {
        QTemporaryDir directory;AccountSwitcher service(directory.filePath("accounts.json"));setup(service);remember(service,"101");remember(service,"202");activations.clear();
        service.rememberCurrentCredential(token("rotated202"));QCOMPARE(requests.first().id,"202");finish();QVERIFY(activations.isEmpty());
        QVERIFY(service.switchAccount("101"));finish(QKeychain::NoError,token("101").toUtf8());service.rejectAuthentication();
        QCOMPARE(requests.first().id,"101");QCOMPARE(requests.first().operation,Operation::Delete);finish();
        QCOMPARE(service.accounts().size(),2);QVERIFY(!service.accounts().first().toMap().value("remembered").toBool());QVERIFY(service.accounts().last().toMap().value("remembered").toBool());
        QVERIFY(!service.switchAccount("101"));QVERIFY(service.switchAccount("202"));service.cancelPending();finish(QKeychain::NoError,token("202").toUtf8());
    }
    void boundedRosterAndUntrustedMetadata() {
        QTemporaryDir directory;const auto path=directory.filePath("accounts.json");
        QJsonArray rows;rows.append(QJsonObject{{"id","bad/path"},{"name","bad"}});
        rows.append(QJsonObject{{"id","1"},{"name",QString(300,'x')},{"avatarUrl","https://u:password@cdn.discordapp.com/a"},{"remembered",true}});
        rows.append(QJsonObject{{"id","1"},{"name","duplicate"}});
        for(int i=2;i<30;++i)rows.append(QJsonObject{{"id",QString::number(i)},{"name","Generated"},{"avatarUrl","https://cdn.discordapp.com/a"}});
        QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(QJsonObject{{"accounts",rows}}).toJson());file.close();
        AccountSwitcher service(path);setup(service);QCOMPARE(service.accounts().size(),5);QVERIFY(!service.canAdd());QVERIFY(requests.isEmpty());
        QCOMPARE(service.accounts().first().toMap().value("name").toString().size(),128);QVERIFY(service.accounts().first().toMap().value("avatarUrl").toString().isEmpty());
        QSignalSpy add(&service,&AccountSwitcher::addAccountRequested);service.addAccount();QCOMPARE(add.size(),0);
        attach(service,"99");service.captureAuthenticated(token("99"));service.updateCurrentAccount("99","Generated 99",{});QVERIFY(requests.isEmpty());QCOMPARE(service.accounts().size(),5);QVERIFY(!service.error().isEmpty());
    }
    void walletFailureNeverPersistsSecretsOrClaimsRemembered() {
        QTemporaryDir directory;const auto path=directory.filePath("accounts.json");AccountSwitcher service(path);setup(service);attach(service,"101");
        service.captureAuthenticated(token("101"));service.updateCurrentAccount("101","Generated",{});finish(QKeychain::NoBackendAvailable);
        QVERIFY(!service.accounts().first().toMap().value("remembered").toBool());QVERIFY(!service.error().isEmpty());QVERIFY(!service.busy());
        const auto bytes=readFile(path);QVERIFY(!bytes.contains(token("101").toUtf8()));QVERIFY(!service.switchAccount("101"));
    }
};
QTEST_GUILESS_MAIN(AccountSwitcherTest)
#include "account_switcher_test.moc"
