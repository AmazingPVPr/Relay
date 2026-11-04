#include "CredentialStore.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStandardPaths>
#include <qt6keychain/keychain.h>
#include <sodium.h>

namespace {
const QString Service = QStringLiteral("com.relay.desktop");
const QString Entry = QStringLiteral("discord-session");
QString defaultPreferences() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/session-preferences.ini";
}
}

CredentialStore::CredentialStore(QObject *parent) : CredentialStore({}, parent) {}
CredentialStore::CredentialStore(const QString &preferencesFile, QObject *parent)
    : CredentialStore(preferencesFile, Entry, parent) {}
CredentialStore::CredentialStore(const QString &preferencesFile, const QString &entryKey, QObject *parent)
    : QObject(parent), m_preferences(std::make_unique<QSettings>(preferencesFile.isEmpty() ? defaultPreferences() : preferencesFile, QSettings::IniFormat)), m_entryKey(entryKey) {
    m_waitNotice.setSingleShot(true);
    m_waitNotice.setInterval(20000);
    connect(&m_waitNotice, &QTimer::timeout, this, [this] {
        if (!m_running) return;
        m_status = "Waiting for your desktop wallet";
        m_error = "Unlock the desktop wallet to finish. You can keep using this session while waiting.";
        emit changed();
    });
    m_executor = [this](Operation operation, const QByteArray &secret, Completion completed) {
        auto *job = createJob(operation, secret);
        connect(job, &QKeychain::Job::finished, this, [completed = std::move(completed), operation](QKeychain::Job *finished) mutable {
            QByteArray value;
            if (operation == Operation::Read && finished->error() == QKeychain::NoError)
                value = static_cast<QKeychain::ReadPasswordJob *>(finished)->binaryData();
            completed(int(finished->error()), std::move(value));
        });
        job->start();
    };
}

CredentialStore::~CredentialStore() {
    ++m_generation;
    if (m_pending) wipe(m_pending->secret);
}

void CredentialStore::wipe(QByteArray &secret) {
    if (!secret.isEmpty()) sodium_memzero(secret.data(), size_t(secret.size()));
    secret.clear();
}

bool CredentialStore::validToken(const QByteArray &token) {
    if (token.size() < 20 || token.size() > 8192) return false;
    for (const auto character : token) {
        const auto byte = quint8(character);
        if (byte < 0x21 || byte > 0x7e) return false;
    }
    return true;
}

QKeychain::Job *CredentialStore::createJob(Operation operation, const QByteArray &secret) {
    QKeychain::Job *job = nullptr;
    switch (operation) {
    case Operation::Read: job = new QKeychain::ReadPasswordJob(Service, this); break;
    case Operation::Write: {
        auto *write = new QKeychain::WritePasswordJob(Service, this);
        write->setBinaryData(secret);
        job = write;
        break;
    }
    case Operation::Delete: job = new QKeychain::DeletePasswordJob(Service, this); break;
    }
    job->setKey(m_entryKey);
    job->setInsecureFallback(false);
    job->setAutoDelete(true);
    return job;
}

bool CredentialStore::setRestoreAllowed(bool allowed) {
    // This file contains only a boolean preference. Disabling restore first means
    // a wallet deletion failure cannot silently sign the user back in next launch.
    QDir().mkpath(QFileInfo(m_preferences->fileName()).absolutePath());
    m_preferences->setValue("session/restoreAllowed", allowed);
    m_preferences->sync();
    return m_preferences->status() == QSettings::NoError;
}

void CredentialStore::restore() {
    if ((m_pending && m_pending->operation != Operation::Read) || (m_running && m_operation != Operation::Read)) return;
    if (!m_preferences->value("session/restoreAllowed", true).toBool()) {
        m_status = "Sign in with Discord";
        emit changed(); emit restoreFinished(false); return;
    }
    if (m_running || m_pending) return;
    enqueue(Operation::Read);
}

void CredentialStore::save(const QString &token) {
    auto secret = token.toUtf8();
    if (!validToken(secret)) {
        wipe(secret);
        m_error = "This sign-in could not be saved. Sign in again with Discord.";
        emit changed(); return;
    }
    setRestoreAllowed(false);
    enqueue(Operation::Write, std::move(secret));
}

void CredentialStore::clear() {
    const bool preferenceSaved = setRestoreAllowed(false);
    enqueue(Operation::Delete);
    if (!preferenceSaved) {
        m_error = "Could not save the sign-out preference. Keep Relay open until the wallet entry is removed.";
        emit changed();
    }
}

void CredentialStore::cancelRestore() {
    if (m_pending && m_pending->operation == Operation::Read) m_pending.reset();
    if (m_running && m_operation == Operation::Read) {
        ++m_generation;
        m_status = "Sign in with Discord";
        m_error.clear();
        emit changed(); emit restoreFinished(false);
    }
}

void CredentialStore::enqueue(Operation operation, QByteArray secret) {
    ++m_generation;
    if (m_pending) wipe(m_pending->secret);
    m_pending = Request{operation, std::move(secret), m_generation};
    m_error.clear();
    m_status = operation == Operation::Read ? "Restoring sign-in from your desktop wallet…" :
        operation == Operation::Write ? "Saving sign-in in your desktop wallet…" : "Removing saved sign-in…";
    emit changed();
    pump();
}

void CredentialStore::pump() {
    if (m_running || !m_pending) return;
    auto request = std::move(*m_pending);
    m_pending.reset();
    m_running = true;
    m_operation = request.operation;
    const auto ticket = ++m_ticket;
    m_waitNotice.start();
    const QPointer<CredentialStore> guard(this);
    m_executor(request.operation, request.secret,
        [guard, ticket, operation = request.operation, generation = request.generation](int error, QByteArray value) {
            if (guard) guard->finish(ticket, operation, generation, error, std::move(value));
            else wipe(value);
        });
    wipe(request.secret);
}

void CredentialStore::finish(quint64 ticket, Operation operation, quint64 generation, int result, QByteArray value) {
    if (!m_running || ticket != m_ticket) { wipe(value); return; }
    m_running = false;
    m_waitNotice.stop();
    if (generation != m_generation) { wipe(value); pump(); emit changed(); return; }
    m_error.clear();
    if (operation == Operation::Read) {
        const bool found = result == QKeychain::NoError && validToken(value);
        if (found) {
            m_remembered = true;
            m_status = "Saved sign-in restored";
            emit restored(QString::fromUtf8(value));
        } else {
            m_remembered = false;
            m_status = "Sign in with Discord";
            if (result == QKeychain::NoError) {
                setRestoreAllowed(false);
                m_error = "The saved sign-in is invalid. Sign in again with Discord.";
            } else if (result != QKeychain::EntryNotFound) {
                m_error = "The desktop wallet could not restore sign-in. Unlock it, or sign in with Discord for this session.";
            }
        }
        wipe(value);
        emit restoreFinished(found);
    } else if (operation == Operation::Write) {
        wipe(value);
        m_remembered = result == QKeychain::NoError;
        if (m_remembered) {
            m_status = "Sign-in saved securely in your desktop wallet";
            if (!setRestoreAllowed(true)) m_error = "Sign-in was saved securely, but automatic sign-in preferences could not be saved.";
        } else {
            m_status = "Signed in for this session";
            m_error = "The desktop wallet could not save sign-in. Unlock or enable your wallet to remember this device.";
        }
    } else {
        wipe(value);
        if (result == QKeychain::NoError || result == QKeychain::EntryNotFound) {
            m_remembered = false;
            m_status = "Saved sign-in removed";
            emit cleared();
        } else {
            m_status = "Automatic sign-in disabled";
            m_error = "The desktop wallet could not remove the saved sign-in. Remove Relay’s entry in your wallet when it is available.";
        }
    }
    emit changed();
    pump();
}
