#include "ObsInputStore.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <qt6keychain/keychain.h>

namespace {
const QString Service = "org.relayclient.obs", Entry = "rtmp-input-v1";
bool textValid(const QString &text, int limit) { if (text.size() > limit) return false; for (auto c : text) if (c.unicode() < 32 || c.unicode() == 127) return false; return true; }
}
ObsInputStore::ObsInputStore(const QString &file, QObject *parent) : QObject(parent),
    m_preferences(std::make_unique<QSettings>(file.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/obs-input.ini" : file, QSettings::IniFormat)) {
    m_remember = m_preferences->value("input/remember", true).toBool();
    if (!m_remember) m_status = "Input is kept only for this session";
    m_waitNotice.setSingleShot(true); m_waitNotice.setInterval(20000);
    connect(&m_waitNotice, &QTimer::timeout, this, [this] { if (!m_running) return; m_status = "Waiting for your desktop wallet"; m_error = "Unlock your wallet to remember this input. You can connect using the current fields meanwhile."; emit changed(); });
    m_executor = [this](Operation operation, const QByteArray &bytes, Completion completion) {
        auto *job = createJob(operation, bytes);
        connect(job, &QKeychain::Job::finished, this, [operation, completion = std::move(completion)](QKeychain::Job *finished) mutable {
            QByteArray value; if (operation == Operation::Read && finished->error() == QKeychain::NoError) value = static_cast<QKeychain::ReadPasswordJob *>(finished)->binaryData();
            completion(int(finished->error()), std::move(value));
        }); job->start();
    };
}
ObsInputStore::~ObsInputStore() { ++m_generation; if (m_pending) wipe(m_pending->bytes); }
void ObsInputStore::wipe(QByteArray &bytes) { if (!bytes.isEmpty()) { bytes.detach(); volatile char *data = bytes.data(); for (qsizetype n = 0; n < bytes.size(); ++n) data[n] = 0; } bytes.clear(); }
bool ObsInputStore::decode(const QByteArray &bytes, int &mode, QString &server, QString &key) {
    if (bytes.isEmpty() || bytes.size() > 32 * 1024) return false;
    QJsonParseError error; const auto document = QJsonDocument::fromJson(bytes, &error); if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto object = document.object();
    if (object.value("version").toInt(-1) != 1 || !object.value("mode").isDouble() || !object.value("server").isString() || !object.value("key").isString()) return false;
    const auto rawMode = object.value("mode").toDouble(-1); if (rawMode != 0 && rawMode != 1) return false;
    server = object.value("server").toString(); key = object.value("key").toString(); mode = int(rawMode);
    return textValid(server, 4096) && textValid(key, 2048);
}
QKeychain::Job *ObsInputStore::createJob(Operation operation, const QByteArray &bytes) {
    QKeychain::Job *job = nullptr;
    if (operation == Operation::Read) job = new QKeychain::ReadPasswordJob(Service, this);
    else if (operation == Operation::Delete) job = new QKeychain::DeletePasswordJob(Service, this);
    else { auto *write = new QKeychain::WritePasswordJob(Service, this); write->setBinaryData(bytes); job = write; }
    job->setKey(Entry); job->setInsecureFallback(false); job->setAutoDelete(true); return job;
}
bool ObsInputStore::preferences(bool restoreAllowed) {
    QDir().mkpath(QFileInfo(m_preferences->fileName()).absolutePath());
    m_preferences->setValue("input/remember", m_remember); m_preferences->setValue("input/restoreAllowed", restoreAllowed); m_preferences->sync();
    return m_preferences->status() == QSettings::NoError;
}
void ObsInputStore::restore() {
    if (!m_remember || !m_preferences->value("input/restoreAllowed", true).toBool() || busy()) return;
    enqueue(Operation::Read);
}
void ObsInputStore::save(int mode, const QString &server, const QString &key) {
    if (!m_remember) return;
    if ((mode != 0 && mode != 1) || !textValid(server, 4096) || !textValid(key, 2048)) { m_error = "This input cannot be remembered. Check the address and stream key length."; emit changed(); return; }
    const bool preferenceSafe = preferences(false);
    enqueue(Operation::Write, QJsonDocument(QJsonObject{{"version", 1}, {"mode", mode}, {"server", server}, {"key", key}}).toJson(QJsonDocument::Compact), preferenceSafe);
}
void ObsInputStore::setRemember(bool remember) {
    if (remember == m_remember) return;
    if (!remember) { clear(); return; }
    m_remember = true; const bool savedPreference = preferences(false); m_status = "Input will be remembered in your desktop wallet"; m_error.clear();
    if (!savedPreference) m_error = "The remember preference could not be saved. An older saved input may still restore next time.";
    emit changed();
}
void ObsInputStore::clear() {
    m_remember = false; m_saved = false; const bool savedPreference = preferences(false); enqueue(Operation::Delete, {}, savedPreference);
    if (!savedPreference) { m_error = "Could not save the forget preference. Keep Relay open until wallet removal finishes."; emit changed(); }
}
void ObsInputStore::cancelRestore() {
    if ((m_pending && m_pending->operation == Operation::Read) || (m_running && m_operation == Operation::Read)) { ++m_generation; if (m_pending && m_pending->operation == Operation::Read) m_pending.reset(); }
}
void ObsInputStore::enqueue(Operation operation, QByteArray bytes, bool preferenceSafe) {
    ++m_generation; if (m_pending) wipe(m_pending->bytes); m_pending = Request{operation, std::move(bytes), m_generation, preferenceSafe};
    m_error.clear(); m_status = operation == Operation::Read ? "Restoring input from your desktop wallet…" : operation == Operation::Write ? "Saving input in your desktop wallet…" : "Removing saved input…";
    emit changed(); pump();
}
void ObsInputStore::pump() {
    if (m_running || !m_pending) return;
    auto request = std::move(*m_pending); m_pending.reset(); m_running = true; m_operation = request.operation; const auto ticket = ++m_ticket; m_waitNotice.start();
    const QPointer<ObsInputStore> guard(this); const Request metadata{request.operation, {}, request.generation, request.preferenceSafe};
    m_executor(request.operation, request.bytes, [guard, ticket, metadata](int error, QByteArray value) { if (guard) guard->finish(ticket, metadata, error, std::move(value)); else wipe(value); });
    wipe(request.bytes);
}
void ObsInputStore::finish(quint64 ticket, const Request &request, int result, QByteArray value) {
    if (!m_running || ticket != m_ticket) { wipe(value); return; }
    m_running = false; m_waitNotice.stop(); if (request.generation != m_generation) { wipe(value); pump(); emit changed(); return; }
    m_error.clear();
    if (request.operation == Operation::Read) {
        int mode = 0; QString server, key;
        m_saved = result == QKeychain::NoError && decode(value, mode, server, key);
        if (m_saved) { m_status = "Saved input restored. Connect when you are ready."; emit restored(mode, server, key); }
        else if (result == QKeychain::EntryNotFound) m_status = "Input will be remembered in your desktop wallet";
        else { m_status = "Input is kept only for this session"; m_error = result == QKeychain::NoError ? "The saved input could not be read. Enter it again to replace the saved copy." : "The desktop wallet could not restore this input. Unlock it, or enter the input for this session."; }
    } else if (request.operation == Operation::Write) {
        m_saved = result == QKeychain::NoError;
        if (m_saved) { m_status = "Input saved securely in your desktop wallet"; if (!preferences(true)) m_error = "Input was saved securely, but its restore preference could not be saved."; }
        else { m_status = "Current input is kept only for this session"; m_error = request.preferenceSafe ? "The desktop wallet could not save this input. Unlock or enable your wallet, then save again." : "The input and its restore preference could not be saved. An older saved input may still restore next time."; }
    } else {
        m_saved = false; m_status = "Input is kept only for this session";
        if (result != QKeychain::NoError && result != QKeychain::EntryNotFound) {
            m_error = request.preferenceSafe ? "Automatic restore is disabled, but the wallet could not remove the saved input. Remove the Relay OBS entry in your wallet when it is available." : "The wallet entry and forget preference could not be removed. The saved input may still restore next time; remove the Relay OBS entry in your wallet when it is available.";
            if (!request.preferenceSafe) m_status = "Saved input could not be removed";
        }
    }
    wipe(value); emit changed(); pump();
}
