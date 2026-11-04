#include "MemberActions.h"
#include "DiscordClient.h"
#include "VoiceEngine.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

MemberActions::MemberActions(DiscordClient *client, VoiceEngine *voice, QObject *parent)
    : MemberActions(client, voice, QString{}, parent) {}

MemberActions::MemberActions(DiscordClient *client, VoiceEngine *voice, const QString &settingsFile, QObject *parent)
    : QObject(parent), m_client(client), m_voice(voice), m_settings(std::make_unique<QSettings>(
          settingsFile.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/member-audio.ini" : settingsFile,
          QSettings::IniFormat)) {
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(250);
    connect(&m_saveTimer, &QTimer::timeout, this, &MemberActions::savePending);
    if (client) {
        connect(client, &DiscordClient::stateChanged, this, [this] { synchronizeAccount(); notify(); });
        connect(client, &QObject::destroyed, this, [this] { synchronizeAccount(); notify(); });
    }
    if (voice) connect(voice, &VoiceEngine::userAudioChanged, this, [this](const QString &) { if (!m_loading) notify(); });
    synchronizeAccount();
}

MemberActions::~MemberActions() { savePending(); }

void MemberActions::notify() { ++m_revision; emit changed(); }
void MemberActions::clearError() { if (m_error.isEmpty()) return; m_error.clear(); notify(); }

void MemberActions::synchronizeAccount() {
    const QString account = m_client && !m_client->demoMode() ? m_client->currentUserId() : QString{};
    const quint64 session = m_client ? m_client->m_generation : 0;
    if (account == m_account && session == m_session) return;
    savePending();
    m_account = DiscordClient::validId(account) ? account : QString{};
    m_session = session;
    ++m_operation;
    m_busy = false;
    m_error.clear();
    m_local.clear();
    m_loading = true;
    if (m_voice) m_voice->clearUserAudioSettings();
    if (!m_account.isEmpty()) {
        m_settings->beginGroup("accounts/" + m_account + "/voice");
        const auto ids = m_settings->childGroups();
        for (const auto &id : ids) {
            if (m_local.size() >= 1000) break;
            if (!DiscordClient::validId(id) || id == m_account) continue;
            bool valid = false;
            const double value = m_settings->value(id + "/volume", 1.0).toDouble(&valid);
            if (!valid || !std::isfinite(value)) continue;
            const double soundboard = m_settings->value(id + "/soundboardVolume", 1.0).toDouble(&valid);
            LocalAudio audio{m_settings->value(id + "/muted", false).toBool(), std::clamp(value, 0.0, 2.0),
                             valid && std::isfinite(soundboard) ? std::clamp(soundboard, 0.0, 2.0) : 1.0};
            if (!audio.muted && audio.volume == 1 && audio.soundboardVolume == 1) continue;
            m_local.insert(id, audio);
            if (m_voice) {
                m_voice->setUserMuted(id, audio.muted);
                m_voice->setUserVolume(id, audio.volume);
                m_voice->setUserSoundboardVolume(id, audio.soundboardVolume);
            }
        }
        m_settings->endGroup();
    }
    m_loading = false;
}

void MemberActions::rememberAudio(const QString &id) {
    if (!m_voice) return;
    const LocalAudio value{m_voice->userMuted(id), m_voice->userVolume(id), m_voice->userSoundboardVolume(id)};
    if (!value.muted && value.volume == 1 && value.soundboardVolume == 1) m_local.remove(id);
    else m_local.insert(id, value);
    if (!m_account.isEmpty()) { m_pending.insert(id); m_saveTimer.start(); }
}

void MemberActions::savePending() {
    m_saveTimer.stop();
    if (m_account.isEmpty() || m_pending.isEmpty()) { m_pending.clear(); return; }
    for (const auto &id : std::as_const(m_pending)) {
        const QString path = "accounts/" + m_account + "/voice/" + id;
        if (!m_local.contains(id)) m_settings->remove(path);
        else {
            const auto value = m_local.value(id);
            m_settings->setValue(path + "/volume", value.volume);
            m_settings->setValue(path + "/muted", value.muted);
            m_settings->setValue(path + "/soundboardVolume", value.soundboardVolume);
        }
    }
    m_pending.clear();
    m_settings->sync();
    if (m_settings->status() != QSettings::NoError) {
        m_error = QStringLiteral("The audio setting is active, but couldn’t be saved for next time.");
        notify();
    }
}

QVariantMap MemberActions::state(const QString &id) const {
    return {{"muted", m_voice && m_voice->userMuted(id)}, {"volume", m_voice ? m_voice->userVolume(id) : 1.0},
            {"soundboardVolume", m_voice ? m_voice->userSoundboardVolume(id) : 1.0},
            {"blocked", m_client && m_client->m_blockedUsers.contains(id)},
            {"own", m_client && id == m_client->currentUserId()}};
}

bool MemberActions::validateLocalTarget(const QString &id) {
    synchronizeAccount();
    m_error.clear();
    if (!m_voice || !DiscordClient::validId(id) || (m_client && id == m_client->currentUserId())) {
        m_error = QStringLiteral("Choose another person to adjust their audio."); notify(); return false;
    }
    return true;
}

bool MemberActions::setLocalMuted(const QString &id, bool muted) {
    if (!validateLocalTarget(id)) return false;
    if (!m_voice->setUserMuted(id, muted)) { m_error = QStringLiteral("Too many saved audio adjustments. Reset an unused adjustment first."); notify(); return false; }
    rememberAudio(id); notify(); return true;
}

bool MemberActions::setLocalVolume(const QString &id, double volume) {
    if (!validateLocalTarget(id)) return false;
    if (!std::isfinite(volume)) { m_error = QStringLiteral("Choose a volume between 0% and 200%."); notify(); return false; }
    if (!m_voice->setUserVolume(id, std::clamp(volume, 0.0, 2.0))) { m_error = QStringLiteral("Too many saved audio adjustments. Reset an unused adjustment first."); notify(); return false; }
    rememberAudio(id); notify(); return true;
}

bool MemberActions::setLocalSoundboardVolume(const QString &id, double volume) {
    if (!validateLocalTarget(id)) return false;
    if (!std::isfinite(volume)) { m_error = QStringLiteral("Choose a volume between 0% and 200%."); notify(); return false; }
    if (!m_voice->setUserSoundboardVolume(id, std::clamp(volume, 0.0, 2.0))) { m_error = QStringLiteral("Too many saved audio adjustments. Reset an unused adjustment first."); notify(); return false; }
    rememberAudio(id); notify(); return true;
}

bool MemberActions::setBlocked(const QString &id, bool blocked) {
    synchronizeAccount();
    if (m_busy) return false;
    m_error.clear();
    if (!m_client || !m_client->connected() || m_client->demoMode() || m_client->m_token.isEmpty() ||
        !DiscordClient::validId(id) || id == m_client->currentUserId()) {
        m_error = QStringLiteral("Connect to Discord and choose another person first."); notify(); return false;
    }
    // Never DELETE an unrelated friendship when the requested state is already unblocked.
    if (m_client->m_blockedUsers.contains(id) == blocked) { notify(); return true; }
    m_busy = true;
    const auto operation = ++m_operation;
    notify();
    m_client->request(blocked ? "PUT" : "DELETE", "/users/@me/relationships/" + id,
        blocked ? QJsonObject{{"type", 2}} : QJsonObject{},
        [this, weak = QPointer<MemberActions>(this), operation, id, blocked](const QJsonValue &, const QString &error) {
        if (!weak || !m_client || operation != m_operation) return;
        m_busy = false;
        if (!error.isEmpty()) { m_error = error; notify(); return; }
        m_client->acceptRelationship(QJsonObject{{"id", id}, {"type", 2}}, !blocked);
        notify();
        emit blockedChanged(id, blocked);
    });
    return true;
}

QString MemberActions::messageLink(const QString &guild, const QString &channel, const QString &message) {
    if ((!guild.isEmpty() && guild != "@me" && !DiscordClient::validId(guild)) ||
        !DiscordClient::validId(channel) || !DiscordClient::validId(message)) return {};
    return "https://discord.com/channels/" + (guild.isEmpty() ? QStringLiteral("@me") : guild) + '/' + channel + '/' + message;
}

bool MemberActions::copyText(const QString &text) {
    if (text.isEmpty() || !qobject_cast<QGuiApplication *>(QCoreApplication::instance())) return false;
    auto *clipboard = QGuiApplication::clipboard();
    if (!clipboard) return false;
    clipboard->setText(text);
    return true;
}

bool MemberActions::copyId(const QString &id) { return DiscordClient::validId(id) && copyText(id); }
bool MemberActions::copyMessageLink(const QString &guild, const QString &channel, const QString &message) { return copyText(messageLink(guild, channel, message)); }
