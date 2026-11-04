#include "ContextActions.h"
#include "DiscordClient.h"
#include <QDateTime>
#include <QJsonArray>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

namespace {
QJsonObject merge(QJsonObject base, const QJsonObject &patch) {
    for (auto it = patch.begin(); it != patch.end(); ++it) base.insert(it.key(), it.value());
    return base;
}
}
ContextActions::ContextActions(DiscordClient *client, QObject *parent) : ContextActions(client, QString{}, parent) {}
ContextActions::ContextActions(DiscordClient *client, const QString &settingsFile, QObject *parent)
    : QObject(parent), m_client(client), m_settings(std::make_unique<QSettings>(settingsFile.isEmpty() ?
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/context-layout.ini" : settingsFile, QSettings::IniFormat)) {
    if (client) {
        connect(client, &DiscordClient::stateChanged, this, [this] { synchronizeAccount(); notify(); });
        connect(client, &DiscordClient::modelsReady, this, [this] { synchronizeAccount(); notify(); });
        connect(client, &QObject::destroyed, this, [this] { synchronizeAccount(); notify(); });
    }
    synchronizeAccount();
}
ContextActions::~ContextActions() = default;
void ContextActions::notify() { ++m_revision; emit changed(); }
void ContextActions::clearError() { m_error.clear(); notify(); }
bool ContextActions::fail(const QString &error) { m_error = error; notify(); return false; }
void ContextActions::synchronizeAccount() {
    if (m_synchronizing) return;
    const auto account = m_client && !m_client->demoMode() ? m_client->currentUserId() : QString{};
    const auto session = m_client ? m_client->m_generation : 0;
    if (account == m_account && session == m_session) return;
    m_synchronizing = true;
    m_account = DiscordClient::validId(account) ? account : QString{}; m_session = session;
    ++m_operation; m_busy = false; m_error.clear();
    if (m_client) {
        m_client->m_pinnedDms.clear(); m_client->m_favoriteDms.clear(); m_client->m_hideMutedGuilds.clear();
        if (!m_account.isEmpty()) {
            const auto read = [this](const QString &key) {
                QSet<QString> ids;
                const auto values = m_settings->value("accounts/" + m_account + '/' + key).toStringList();
                for (const auto &id : values) if (DiscordClient::validId(id) && ids.size() < 1000) ids.insert(id);
                return ids;
            };
            m_client->m_pinnedDms = read("pinnedDms"); m_client->m_favoriteDms = read("favoriteDms"); m_client->m_hideMutedGuilds = read("hideMutedGuilds");
        }
        for (const auto &id : m_client->m_dmObjects.keys()) m_client->syncDm(id);
        m_client->rebuildChannels(false);
    }
    m_synchronizing = false;
}
bool ContextActions::ready() {
    synchronizeAccount();
    if (m_busy) return false;
    if (!m_client || !m_client->connected() || m_client->demoMode() || m_client->m_token.isEmpty() || m_client->m_token.startsWith("Bot "))
        return fail("Sign in to change this menu setting.");
    m_error.clear(); return true;
}
bool ContextActions::knownDm(const QString &id) const {
    if (!m_client || !DiscordClient::validId(id)) return false;
    const auto channel = m_client->m_dmObjects.value(id); const int type = channel.value("type").toInt(-1);
    return (type == 1 || type == 3) && !DiscordClient::pendingMessageRequest(channel);
}
bool ContextActions::knownGuild(const QString &id) const { return m_client && DiscordClient::validId(id) && m_client->m_guildObjects.contains(id); }
QString ContextActions::dmChannelForUser(const QString &id) const {
    if (!m_client || !DiscordClient::validId(id)) return {};
    for (auto it = m_client->m_dmObjects.cbegin(); it != m_client->m_dmObjects.cend(); ++it)
        if (it.value().value("type").toInt() == 1 && m_client->dmRecipientId(it.value()) == id && knownDm(it.key())) return it.key();
    return {};
}
bool ContextActions::muted(const QJsonObject &settings) {
    if (!settings.value("muted").toBool()) return false;
    const auto end = QDateTime::fromString(settings.value("mute_config").toObject().value("end_time").toString(), Qt::ISODateWithMs);
    return !end.isValid() || end > QDateTime::currentDateTimeUtc();
}
QVariantMap ContextActions::dmInfo(const QString &id) const {
    if (!knownDm(id)) return {{"available", false}};
    return {{"available", true}, {"channelId", id}, {"pinned", m_client->m_pinnedDms.contains(id)}, {"favorite", m_client->m_favoriteDms.contains(id)},
        {"muted", m_client->conversationMuted(id)}, {"unread", m_client->m_unread.value(id)},
        {"canMarkRead", DiscordClient::validId(m_client->m_dmObjects.value(id).value("last_message_id").toString())}};
}
QVariantMap ContextActions::guildInfo(const QString &id) const {
    if (!knownGuild(id)) return {{"available", false}};
    const auto notification = m_client->m_notificationSettings.value(id);
    const int storedLevel = notification.value("message_notifications").toInt(3);
    const int level = storedLevel == 3 ? m_client->m_guildObjects.value(id).value("default_message_notifications").toInt(1) : storedLevel;
    return {{"available", true}, {"guildId", id}, {"name", m_client->m_guildObjects.value(id).value("name").toString()}, {"muted", muted(notification)},
        {"nickname", m_client->m_guildObjects.value(id).value("member").toObject().value("nick").toString()},
        {"notificationLevel", level}, {"hideMuted", m_client->m_hideMutedGuilds.contains(id)},
        {"showAll", true}, {"showAllAvailable", false}, {"unread", m_client->m_unread.value(id)}};
}
bool ContextActions::saveLocal(const QString &key, const QSet<QString> &values) {
    if (m_account.isEmpty() || values.size() > 1000) return fail("This account's local organization could not be saved.");
    auto ids = values.values(); std::sort(ids.begin(), ids.end());
    m_settings->setValue("accounts/" + m_account + '/' + key, ids); m_settings->sync();
    return m_settings->status() == QSettings::NoError || fail("Relay could not save this local menu setting.");
}
bool ContextActions::organizeDm(const QString &id, bool enabled, bool pinned) {
    synchronizeAccount();
    if (!knownDm(id) || m_account.isEmpty()) return fail("Open an existing direct message first.");
    auto &values = pinned ? m_client->m_pinnedDms : m_client->m_favoriteDms; const auto previous = values;
    if (enabled) { if (values.size() >= 1000 && !values.contains(id)) return fail("Too many saved conversations."); values.insert(id); }
    else values.remove(id);
    if (!saveLocal(pinned ? "pinnedDms" : "favoriteDms", values)) { values = previous; return false; }
    for (const auto &channelId : m_client->m_dmObjects.keys()) m_client->syncDm(channelId);
    notify(); return true;
}
bool ContextActions::setDmPinned(const QString &id, bool enabled) { return organizeDm(id, enabled, true); }
bool ContextActions::setDmFavorite(const QString &id, bool enabled) { return organizeDm(id, enabled, false); }
bool ContextActions::markDmRead(const QString &id) {
    if (!ready()) return false;
    if (!knownDm(id)) return fail("Choose a direct message first.");
    const auto latest = m_client->m_dmObjects.value(id).value("last_message_id").toString();
    if (!DiscordClient::validId(latest)) return fail("There are no known messages to mark read in this conversation.");
    m_busy = true; const auto operation = ++m_operation; notify();
    m_client->request("POST", "/channels/" + id + "/messages/" + latest + "/ack", {{"token", QJsonValue::Null}},
        [this, weak = QPointer<ContextActions>(this), id, latest, operation](const QJsonValue &, const QString &error) {
        if (!weak || !m_client || operation != m_operation) return;
        m_busy = false;
        if (!error.isEmpty()) m_error = error;
        else m_client->acceptDmReadState({{"channel_id", id}, {"message_id", latest}, {"mention_count", 0}});
        notify();
    }); return true;
}
bool ContextActions::closeDm(const QString &id) {
    if (!ready()) return false;
    if (!knownDm(id) || m_client->m_dmObjects.value(id).value("type").toInt() != 1) return fail("Choose a one-to-one direct message to close.");
    const auto version = m_client->m_dmSocialVersions.value(id);
    const auto latest = m_client->m_dmObjects.value(id).value("last_message_id").toString();
    m_busy = true; const auto operation = ++m_operation; notify();
    m_client->request("DELETE", "/channels/" + id, {}, [this, weak = QPointer<ContextActions>(this), id, version, latest, operation](const QJsonValue &, const QString &error) {
        if (!weak || !m_client || operation != m_operation) return;
        m_busy = false;
        if (!error.isEmpty()) m_error = error;
        else if (m_client->m_dmSocialVersions.value(id) == version &&
                 !DiscordClient::newerSnowflake(m_client->m_dmObjects.value(id).value("last_message_id").toString(), latest)) {
            m_client->m_dmObjects.remove(id); m_client->m_dmReadMessageIds.remove(id); m_client->m_unread.remove(id);
            m_client->m_dmSocialVersions.insert(id, ++m_client->m_socialVersion);
            m_client->m_dms.remove(id); if (m_client->m_currentGuildId.isEmpty()) m_client->m_channels.remove(id);
            if (m_client->m_currentChannelId == id) m_client->selectHome();
            m_client->rebuildSocial();
        }
        notify();
    }); return true;
}
bool ContextActions::muteBody(int seconds, QJsonObject &body) {
    static const QSet<int> durations{-1, 0, 900, 3600, 10800, 28800, 86400};
    if (!durations.contains(seconds)) return false;
    const bool enabled = seconds >= 0;
    body = {{"muted", enabled}, {"mute_config", QJsonObject{{"selected_time_window", seconds > 0 ? seconds : -1},
        {"end_time", seconds > 0 ? QJsonValue(QDateTime::currentDateTimeUtc().addSecs(seconds).toString(Qt::ISODateWithMs)) : QJsonValue(QJsonValue::Null)}}}};
    return true;
}
bool ContextActions::patchNotification(const QString &guildId, const QJsonObject &body) {
    if (!ready()) return false;
    const auto before = m_client->m_notificationSettings.value(guildId);
    m_busy = true; const auto operation = ++m_operation; notify();
    m_client->request("PATCH", "/users/@me/guilds/" + (guildId.isEmpty() ? QStringLiteral("@me") : guildId) + "/settings", body,
        [this, weak = QPointer<ContextActions>(this), guildId, body, before, operation](const QJsonValue &value, const QString &error) {
        if (!weak || !m_client || operation != m_operation) return;
        m_busy = false;
        if (!error.isEmpty()) m_error = error;
        else {
            auto updated = m_client->m_notificationSettings.value(guildId);
            const auto response = value.toObject();
            for (auto it = body.begin(); it != body.end(); ++it)
                if (it.key() != "channel_overrides" && updated.value(it.key()) == before.value(it.key()))
                    updated.insert(it.key(), response.contains(it.key()) ? response.value(it.key()) : it.value());
            if (body.contains("channel_overrides")) {
                auto overrides = updated.value("channel_overrides").toArray(); const auto patch = body.value("channel_overrides").toObject();
                for (auto it = patch.begin(); it != patch.end(); ++it) {
                    QJsonObject original, returned;
                    for (const auto &entry : before.value("channel_overrides").toArray()) if (entry.toObject().value("channel_id").toString() == it.key()) original = entry.toObject();
                    for (const auto &entry : response.value("channel_overrides").toArray()) if (entry.toObject().value("channel_id").toString() == it.key()) returned = entry.toObject();
                    bool found = false;
                    for (int i = 0; i < overrides.size(); ++i) if (overrides[i].toObject().value("channel_id").toString() == it.key()) {
                        auto current = overrides[i].toObject(); const auto fields = it.value().toObject();
                        for (auto field = fields.begin(); field != fields.end(); ++field)
                            if (current.value(field.key()) == original.value(field.key()))
                                current.insert(field.key(), returned.contains(field.key()) ? returned.value(field.key()) : field.value());
                        overrides[i] = current; found = true; break;
                    }
                    if (!found) overrides.append(merge(it.value().toObject(), {{"channel_id", it.key()}}));
                }
                updated.insert("channel_overrides", overrides);
            }
            updated.insert("guild_id", guildId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(guildId));
            m_client->acceptNotificationSettings(updated);
            if (m_client->m_currentGuildId == guildId) m_client->rebuildChannels(false);
            emit m_client->stateChanged();
        }
        notify();
    }); return true;
}
bool ContextActions::muteDm(const QString &id, int seconds) {
    if (!knownDm(id)) return fail("Choose an existing direct message first.");
    QJsonObject body; if (!muteBody(seconds, body)) return fail("Choose a supported mute duration.");
    return patchNotification({}, {{"channel_overrides", QJsonObject{{id, body}}}});
}
bool ContextActions::muteGuild(const QString &id, int seconds) {
    if (!knownGuild(id)) return fail("Choose a server you have joined first.");
    QJsonObject body; if (!muteBody(seconds, body)) return fail("Choose a supported mute duration.");
    return patchNotification(id, body);
}
bool ContextActions::setGuildNotificationLevel(const QString &id, int level) {
    if (!knownGuild(id) || level < 0 || level > 2) return fail("Choose a supported server notification level.");
    return patchNotification(id, {{"message_notifications", level}});
}
bool ContextActions::markGuildRead(const QString &id) {
    if (!ready()) return false;
    if (!knownGuild(id)) return fail("Choose a server you have joined first.");
    m_busy = true; const auto operation = ++m_operation; notify();
    QHash<QString, int> counts; counts.insert(id, m_client->m_unread.value(id));
    for (const auto &entry : m_client->m_channelCache.value(id)) {
        const auto channelId = entry.toObject().value("id").toString(); counts.insert(channelId, m_client->m_unread.value(channelId));
    }
    m_client->request("POST", "/guilds/" + id + "/ack", {}, [this, weak = QPointer<ContextActions>(this), counts, id, operation](const QJsonValue &, const QString &error) {
        if (!weak || !m_client || operation != m_operation) return;
        m_busy = false;
        if (!error.isEmpty()) m_error = error;
        else for (auto it = counts.begin(); it != counts.end(); ++it) {
            // New arrivals while the acknowledgement was in flight stay unread.
            const int remaining = std::max(0, m_client->m_unread.value(it.key()) - it.value());
            m_client->m_unread.insert(it.key(), remaining);
            if (it.key() == id) m_client->m_guilds.patch(id, {{"unread", remaining}});
            else m_client->m_channels.patch(it.key(), {{"unread", remaining}});
        }
        notify();
    }); return true;
}
bool ContextActions::setGuildHideMuted(const QString &id, bool enabled) {
    synchronizeAccount();
    if (!knownGuild(id) || m_account.isEmpty()) return fail("Choose a server you have joined first.");
    auto &values = m_client->m_hideMutedGuilds; const auto previous = values;
    if (enabled) values.insert(id); else values.remove(id);
    if (!saveLocal("hideMutedGuilds", values)) { values = previous; return false; }
    if (m_client->currentGuildId() == id) m_client->rebuildChannels(false);
    emit m_client->stateChanged(); notify(); return true;
}
bool ContextActions::setGuildShowAll(const QString &id, bool enabled) {
    if (!knownGuild(id)) return fail("Choose a server you have joined first.");
    if (enabled) return true;
    return fail("Relay currently shows all accessible channels. Channel opt-in selection is not available yet.");
}
bool ContextActions::setGuildNickname(const QString &id, const QString &input) {
    if (!ready()) return false;
    const auto nickname = input.trimmed();
    if (!knownGuild(id) || !DiscordClient::validId(m_client->currentUserId()) || nickname.size() > 32 ||
        DiscordClient::cleanText(nickname, 32) != nickname || nickname.contains('\n') || nickname.contains('\t'))
        return fail("Choose a joined server and enter a nickname of at most 32 characters.");
    const auto membership = m_client->m_guildMembershipVersions.value(id);
    m_busy = true; const auto operation = ++m_operation; notify();
    m_client->request("PATCH", "/guilds/" + id + "/members/@me", {{"nick", nickname.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(nickname)}},
        [this, weak = QPointer<ContextActions>(this), id, membership, nickname, operation](const QJsonValue &value, const QString &error) {
        if (!weak || !m_client || operation != m_operation) return;
        m_busy = false;
        if (!error.isEmpty()) m_error = error;
        else if (m_client->m_guildObjects.contains(id) && m_client->m_guildMembershipVersions.value(id) == membership) {
            auto guild = m_client->m_guildObjects.value(id); auto member = guild.value("member").toObject();
            const auto returned = value.toObject();
            member.insert("nick", returned.contains("nick") ? returned.value("nick") : QJsonValue(nickname));
            member.insert("user", m_client->m_users.value(m_client->currentUserId()));
            guild.insert("member", member); m_client->m_guildObjects.insert(id, guild);
            m_client->cacheMember(id, member); m_client->rebuildMembers(); m_client->rebuildVoiceMembers();
            emit m_client->stateChanged();
        }
        notify();
    }); return true;
}
