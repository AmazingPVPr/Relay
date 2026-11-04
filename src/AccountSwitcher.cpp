#include "AccountSwitcher.h"
#include "CredentialStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QLockFile>
#include <QStandardPaths>
#include <QUrl>
#include <sodium.h>
#include <algorithm>

namespace {
const QString SavingAccountMessage = QStringLiteral("Please wait for your account changes to finish saving before switching or signing out.");
}

AccountSwitcher::AccountSwitcher(const QString &metadataFile, QObject *parent)
    : QObject(parent), m_file(metadataFile.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/accounts.json" : metadataFile) {
    loadMetadata();
}
AccountSwitcher::~AccountSwitcher() { wipe(m_stagedSecret); }
void AccountSwitcher::wipe(QByteArray &value) { if (!value.isEmpty()) sodium_memzero(value.data(),size_t(value.size())); value.clear(); }
bool AccountSwitcher::validId(const QString &id) { static const QRegularExpression pattern("^[0-9]{1,22}$"); return pattern.match(id).hasMatch(); }
QString AccountSwitcher::displayName(QString name) {
    name=name.left(128); name.remove(QRegularExpression("[\\x00-\\x1f\\x7f]")); return name.isEmpty()?QStringLiteral("Saved account"):name;
}
QString AccountSwitcher::avatarUrl(const QString &value) {
    if (value.size()>1024) return {};
    const QUrl url(value);const auto host=url.host().toLower();
    return url.scheme()=="https" && url.userInfo().isEmpty() && (url.port()==-1 || url.port()==443) &&
        (host=="cdn.discordapp.com" || host=="media.discordapp.net") ? value : QString{};
}
QVariantList AccountSwitcher::accounts() const {
    QVariantList result;
    for (const auto &account:m_accounts) result.append(QVariantMap{{"id",account.id},{"name",account.name},
        {"avatarUrl",account.avatar},{"current",account.id==m_currentId},{"remembered",account.remembered}});
    return result;
}
bool AccountSwitcher::busy() const {
    // A canceled read may still be waiting on the desktop wallet's own dialog.
    // Its generation is invalidated; it must not block a new QR sign-in.
    return m_waitingForIdentity || !m_switchingId.isEmpty() || !m_saving.isEmpty() || !m_removing.isEmpty();
}
void AccountSwitcher::setSessionHandler(std::function<void(const QString &)> handler) { m_sessionHandler=std::move(handler); }
void AccountSwitcher::setSessionBusy(bool busy) {
    if (m_sessionBusy==busy) return;
    m_sessionBusy=busy;
    if (!busy && m_error==SavingAccountMessage) m_error.clear();
    emit changed();
}
bool AccountSwitcher::allowSessionChange() {
    if (!m_sessionBusy) return true;
    m_error=SavingAccountMessage;m_status.clear();emit changed();return false;
}
void AccountSwitcher::refreshAccounts() {
    if (busy()) return;
    m_accounts.clear();m_baseline.clear();loadMetadata();emit changed();
}
void AccountSwitcher::loadMetadata() {
    QFile file(m_file);if (!file.open(QIODevice::ReadOnly) || file.size()>32768) return;
    const auto document=QJsonDocument::fromJson(file.readAll());if (!document.isObject()) return;
    QSet<QString> ids;
    for (const auto &entry:document.object().value("accounts").toArray()) {
        const auto object=entry.toObject();const auto id=object.value("id").toString();
        if (!validId(id) || ids.contains(id)) continue;
        if (m_accounts.size()>=5) break;
        ids.insert(id);m_accounts.append({id,displayName(object.value("name").toString()),avatarUrl(object.value("avatarUrl").toString()),object.value("remembered").toBool()});
    }
    m_baseline=m_accounts;
}
bool AccountSwitcher::saveMetadata() {
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QLockFile lock(m_file + ".lock");
    if (!lock.tryLock(1000)) { m_error="The account list is busy in another window. Try again.";return false; }
    // Merge this window's edits with the latest registry under a process lock.
    AccountSwitcher latest(m_file);
    auto merged=latest.m_accounts;
    for (const auto &old:m_baseline) {
        const bool removed=std::none_of(m_accounts.cbegin(),m_accounts.cend(),[&](const Account &a) { return a.id==old.id; });
        if (removed) merged.removeIf([&](const Account &a) { return a.id==old.id; });
    }
    for (const auto &account:m_accounts) {
        const auto old=std::find_if(m_baseline.cbegin(),m_baseline.cend(),[&](const Account &a) { return a.id==account.id; });
        if (old!=m_baseline.cend() && old->name==account.name && old->avatar==account.avatar && old->remembered==account.remembered) continue;
        auto target=std::find_if(merged.begin(),merged.end(),[&](const Account &a) { return a.id==account.id; });
        if (target!=merged.end()) *target=account;
        else if (merged.size()<5) merged.append(account);
    }
    QJsonArray rows;for (const auto &account:merged) rows.append(QJsonObject{{"id",account.id},{"name",account.name},{"avatarUrl",account.avatar},{"remembered",account.remembered}});
    if (!QDir().mkpath(QFileInfo(m_file).absolutePath())) { m_error="Could not save the account list."; return false; }
    QSaveFile file(m_file);
    if (!file.open(QIODevice::WriteOnly)) { m_error="Could not save the account list."; return false; }
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"accounts",rows}}).toJson(QJsonDocument::Compact);
    if (file.write(bytes)!=bytes.size() || !file.commit()) { m_error="Could not save the account list."; return false; }
    m_accounts=merged;m_baseline=merged;
    return true;
}
CredentialStore *AccountSwitcher::storeFor(const QString &id) {
    if (!validId(id)) return nullptr;
    if (m_stores.contains(id)) return m_stores.value(id);
    if (m_stores.size()>=10) return nullptr;
    const auto preferences=QFileInfo(m_file).absolutePath()+"/account-preferences/"+id+".ini";
    auto *store=new CredentialStore(preferences,"discord-account-"+id,this);m_stores.insert(id,store);
    connect(store,&CredentialStore::changed,this,[this,id] { storeChanged(id); });
    connect(store,&CredentialStore::restored,this,[this,id](const QString &token) {
        if (m_switchingId!=id) return;
        m_switchingId.clear();activate(token.toUtf8(),id);
    });
    connect(store,&CredentialStore::restoreFinished,this,[this,id](bool found) {
        if (m_switchingId!=id) return;
        m_switchingId.clear();
        if (!found) { m_error="This saved account needs a new sign-in. Use Add Account to sign in again.";m_status.clear(); }
        emit changed();
    });
    return store;
}
void AccountSwitcher::storeChanged(const QString &id) {
    auto *store=m_stores.value(id);if (!store) return;
    if (!store->busy()) {
        if (m_saving.remove(id)) {
            for (auto &account:m_accounts) if (account.id==id) account.remembered=store->remembered();
            m_error=store->error();m_status=store->remembered()?"Account saved securely in your desktop wallet":"Signed in for this session";
            saveMetadata();
        }
        if (m_removing.remove(id)) {
            if (!store->error().isEmpty()) m_error=store->error();
            const bool retained=std::any_of(m_accounts.cbegin(),m_accounts.cend(),[&](const Account &account) { return account.id==id; });
            if (!retained) { m_stores.remove(id);store->deleteLater(); }
        }
    }
    emit changed();
}
void AccountSwitcher::activate(QByteArray secret, const QString &expectedId) {
    if (secret.size()<20 || secret.size()>512 || std::any_of(secret.cbegin(),secret.cend(),[](char c) { return quint8(c)<0x21 || quint8(c)>0x7e; }) || !m_sessionHandler) {
        wipe(secret);m_error="This account could not be opened. Sign in again with Discord.";emit changed();return;
    }
    // A settings write can begin while the desktop wallet is being unlocked.
    // Keep the current session intact if it is no longer safe to replace it.
    if (!allowSessionChange()) { wipe(secret);return; }
    wipe(m_stagedSecret);m_stagedSecret=std::move(secret);m_expectedId=expectedId;m_waitingForIdentity=true;
    m_error.clear();m_status="Opening account…";emit changed();
    if (m_sessionBusy) {
        wipe(m_stagedSecret);m_expectedId.clear();m_waitingForIdentity=false;
        allowSessionChange();return;
    }
    m_sessionHandler(QString::fromUtf8(m_stagedSecret));
}
void AccountSwitcher::captureAuthenticated(const QString &token) {
    if (!allowSessionChange()) return;
    const auto id=m_switchingId;m_switchingId.clear();if (auto *store=m_stores.value(id)) store->cancelRestore();
    wipe(m_stagedSecret);m_waitingForIdentity=false;m_expectedId.clear();
    // Retain the current identity until the session callback actually replaces
    // it; a reentrant settings write can still prevent activation below.
    activate(token.toUtf8(),{});
}
void AccountSwitcher::rememberCurrentCredential(const QString &token) {
    auto secret=token.toUtf8();
    if (!validId(m_currentId) || secret.size()<20 || secret.size()>512 ||
        std::any_of(secret.cbegin(),secret.cend(),[](char c) { return quint8(c)<0x21 || quint8(c)>0x7e; })) { wipe(secret);return; }
    if (auto *store=storeFor(m_currentId)) { m_saving.insert(m_currentId);store->save(QString::fromUtf8(secret)); }
    wipe(secret);
}
void AccountSwitcher::updateCurrentAccount(const QString &id, const QString &name, const QString &avatar) {
    if (id.isEmpty()) { if (!m_currentId.isEmpty()) { m_currentId.clear();emit changed(); } return; }
    if (!validId(id)) return;
    if (m_waitingForIdentity && !m_expectedId.isEmpty() && id!=m_expectedId) {
        resetSession();m_error="The saved sign-in did not match the selected account. Sign in again.";emit changed();emit signOutRequested();return;
    }
    bool changedState=m_currentId!=id,metadataChanged=false;m_currentId=id;
    auto found=std::find_if(m_accounts.begin(),m_accounts.end(),[&](const Account &account) { return account.id==id; });
    if (found==m_accounts.end() && m_waitingForIdentity && m_accounts.size()<5) { m_accounts.append({id,displayName(name),avatarUrl(avatar),false});found=std::prev(m_accounts.end());metadataChanged=true; }
    if (found!=m_accounts.end()) {
        const auto updatedName=displayName(name),updatedAvatar=avatarUrl(avatar);
        metadataChanged=metadataChanged || found->name!=updatedName || found->avatar!=updatedAvatar;
        found->name=updatedName;found->avatar=updatedAvatar;
    }
    if (m_waitingForIdentity) {
        m_waitingForIdentity=false;m_expectedId.clear();changedState=true;
        if (found!=m_accounts.end()) {
            if (auto *store=storeFor(id)) { m_saving.insert(id);store->save(QString::fromUtf8(m_stagedSecret)); }
            else m_error="This account could not be saved. Try again after the wallet finishes.";
        } else m_error="Relay can remember up to five accounts. Remove one to save this sign-in.";
        wipe(m_stagedSecret);
    }
    if (metadataChanged) saveMetadata();if (changedState || metadataChanged) emit changed();
}
void AccountSwitcher::setWindowHandler(std::function<bool(const QString &)> handler) { m_windowHandler=std::move(handler); }
bool AccountSwitcher::openAccountWindow(const QString &id) {
    if (busy() || !validId(id) || !m_windowHandler) return false;
    const auto found=std::find_if(m_accounts.cbegin(),m_accounts.cend(),[&](const Account &account) { return account.id==id && account.remembered; });
    if (found==m_accounts.cend()) return false;
    const bool opened=m_windowHandler(id);
    m_error=opened?QString{}:QStringLiteral("Could not open a new Relay window. Try again.");
    m_status=opened?QStringLiteral("Account opened in a new window"):QString{};
    emit changed();return opened;
}
bool AccountSwitcher::switchAccount(const QString &id) {
    if (!allowSessionChange()) return false;
    if (busy() || id==m_currentId || !validId(id)) return false;
    const auto found=std::find_if(m_accounts.cbegin(),m_accounts.cend(),[&](const Account &account) { return account.id==id && account.remembered; });
    if (found==m_accounts.cend()) return false;
    auto *store=storeFor(id);if (!store) return false;
    if (store->busy()) { m_error="The desktop wallet is still finishing the previous request for this account. Close its dialog, or use Add Account.";emit changed();return false; }
    m_switchingId=id;m_error.clear();m_status="Unlocking saved sign-in…";emit changed();store->restore();return true;
}
void AccountSwitcher::addAccount() { if (!allowSessionChange() || !canAdd()) return;emit addAccountRequested(); }
void AccountSwitcher::removeAccount(const QString &id) {
    if (!allowSessionChange()) return;
    if (busy() || !validId(id)) return;
    const auto found=std::find_if(m_accounts.begin(),m_accounts.end(),[&](const Account &account) { return account.id==id; });
    if (found==m_accounts.end()) return;
    const bool current=id==m_currentId;m_accounts.erase(found);saveMetadata();
    if (auto *store=storeFor(id)) { m_removing.insert(id);store->clear(); }
    if (current) { m_currentId.clear();emit signOutRequested(); }
    emit changed();
}
void AccountSwitcher::signOutCurrent() {
    if (!allowSessionChange()) return;
    const auto id=m_currentId.isEmpty()?m_expectedId:m_currentId;
    resetSession();
    if (validId(id)) {
        m_accounts.removeIf([&](const Account &account) { return account.id==id; });saveMetadata();
        if (auto *store=storeFor(id)) { m_removing.insert(id);store->clear(); }
    }
    emit changed();emit signOutRequested();
}
void AccountSwitcher::cancelPending() {
    if (m_waitingForIdentity) { if (!allowSessionChange()) return;resetSession();emit signOutRequested();return; }
    const auto id=m_switchingId;m_switchingId.clear();if (auto *store=m_stores.value(id)) store->cancelRestore();m_status.clear();emit changed();
}
void AccountSwitcher::resetSession() {
    const auto id=m_switchingId;m_switchingId.clear();if (auto *store=m_stores.value(id)) store->cancelRestore();
    wipe(m_stagedSecret);m_waitingForIdentity=false;m_expectedId.clear();m_currentId.clear();m_error.clear();m_status.clear();emit changed();
}
void AccountSwitcher::rejectAuthentication() {
    const auto rejected=m_expectedId.isEmpty()?m_currentId:m_expectedId;resetSession();
    for (auto &account:m_accounts) if (account.id==rejected) account.remembered=false;
    if (!rejected.isEmpty()) if (auto *store=storeFor(rejected)) { m_removing.insert(rejected);store->clear(); }
    saveMetadata();m_error="This account needs a new sign-in.";emit changed();
}
