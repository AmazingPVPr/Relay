#include "ChannelTools.h"
#include "DiscordClient.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QTimeZone>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString encoded(const QString &text) {
    // Escape dots too: the shared request queue rejects literal '..' path text.
    return QString::fromLatin1(QUrl::toPercentEncoding(text, {}, "."));
}
}

ChannelTools::ChannelTools(DiscordClient *client, QObject *parent) : QObject(parent), m_client(client) {
    if (!client) return;
    m_session = client->m_generation;
    m_lastSelection = client->m_selectionGeneration;
    connect(client, &DiscordClient::stateChanged, this, [this] {
        if (!m_client) return;
        if (m_session != m_client->m_generation) {
            m_session = m_client->m_generation;
            reset();
        }
        if (m_lastSelection != m_client->m_selectionGeneration) {
            m_lastSelection = m_client->m_selectionGeneration;
            if (m_jumpBusy && !m_navigating) {
                ++m_jumpOperation;
                m_jumpBusy = false;
                m_jumpHistorySelection = 0;
                emit changed();
            }
        }
    });
    connect(client, &QObject::destroyed, this, [this] { reset(); });
}

bool ChannelTools::ready() const {
    return m_client && m_client->connected() && !m_client->demoMode() && !m_client->m_token.isEmpty();
}

void ChannelTools::reset() {
    ++m_pinsOperation;
    ++m_searchOperation;
    ++m_actionOperation;
    ++m_jumpOperation;
    m_pinsBusy = m_searchBusy = m_actionBusy = m_jumpBusy = false;
    m_jumpHistorySelection = 0;
    m_pinsMore = m_searchMore = false;
    m_pins.clear();
    m_search.clear();
    m_resultChannels.clear();
    m_pinsError.clear(); m_searchError.clear(); m_actionError.clear(); m_searchStatus.clear();
    m_pinsChannel.clear(); m_pinCursor.clear(); m_query.clear();
    m_searchGuild.clear(); m_searchChannel.clear(); m_searchScope.clear();
    m_totalResults = m_searchOffset = 0;
    emit changed();
}

QJsonObject ChannelTools::knownChannel(const QString &id) const {
    if (!m_client) return {};
    if (m_client->m_dmObjects.contains(id)) return m_client->m_dmObjects.value(id);
    for (auto it = m_client->m_channelCache.cbegin(); it != m_client->m_channelCache.cend(); ++it) {
        for (const auto &entry : it.value()) {
            auto channel = entry.toObject();
            if (channel.value("id").toString() == id) {
                channel.insert("guild_id", it.key());
                return channel;
            }
        }
    }
    return m_resultChannels.value(id);
}

QVariantMap ChannelTools::resultRow(QJsonObject message, const QString &guildId) const {
    if (!m_client) return {};
    const auto channelId = message.value("channel_id").toString();
    const auto channel = knownChannel(channelId);
    const auto guild = guildId.isEmpty() ? channel.value("guild_id").toString() : guildId;
    message.insert("guild_id", guild);
    auto row = m_client->messageRow(message);
    row.insert("channelId", channelId);
    row.insert("guildId", guild);
    row.insert("channelName", channel.value("name").toString("Channel"));
    row.insert("pinned", message.value("pinned").toBool());
    return row;
}

bool ChannelTools::readPins(const QString &channelId) {
    ++m_pinsOperation;
    m_pins.clear();
    m_pinsChannel = channelId;
    m_pinCursor.clear();
    m_pinsError.clear();
    m_pinsMore = m_pinsBusy = false;
    if (!ready() || !DiscordClient::validId(channelId)) {
        m_pinsError = QStringLiteral("Connect to Discord and choose a channel to view its pins.");
        emit changed();
        return false;
    }
    fetchPins(m_pinsOperation);
    return true;
}

void ChannelTools::loadMorePins() {
    if (ready() && !m_pinsBusy && m_pinsMore) fetchPins(m_pinsOperation);
}

void ChannelTools::fetchPins(quint64 operation) {
    if (!m_client || operation != m_pinsOperation) return;
    m_pinsBusy = true;
    m_pinsError.clear();
    const auto channelId = m_pinsChannel;
    const auto previousCursor = m_pinCursor;
    QString path = "/channels/" + channelId + "/messages/pins?limit=50";
    if (!previousCursor.isEmpty()) path += "&before=" + encoded(previousCursor);
    emit changed();
    m_client->request("GET", path, {}, [this, weak = QPointer<ChannelTools>(this), operation, channelId, previousCursor](const QJsonValue &value, const QString &error) {
        if (!weak || !m_client || operation != m_pinsOperation) return;
        m_pinsBusy = false;
        if (!error.isEmpty()) { m_pinsError = error; emit changed(); return; }
        const auto result = value.toObject();
        if (!result.value("items").isArray() || !result.value("has_more").isBool()) {
            m_pinsError = QStringLiteral("Discord returned an unexpected pins response.");
            emit changed(); return;
        }
        const auto items = result.value("items").toArray();
        QString cursor;
        for (int i = 0; i < std::min(50, int(items.size())); ++i) {
            const auto item = items.at(i).toObject();
            auto message = item.value("message").toObject();
            if (!DiscordClient::validId(message.value("id").toString()) ||
                message.value("channel_id").toString() != channelId) continue;
            const auto pinnedAt = item.value("pinned_at").toString();
            if (pinnedAt.size() <= 64 && QDateTime::fromString(pinnedAt, Qt::ISODateWithMs).isValid()) cursor = pinnedAt;
            message.insert("pinned", true);
            auto row = resultRow(message);
            row.insert("pinnedAt", pinnedAt.left(64));
            if (m_pins.count() < m_pins.maximumRows() || m_pins.indexOf(message.value("id").toString()) >= 0)
                m_pins.upsert(row);
        }
        m_pinCursor = cursor;
        m_pinsMore = result.value("has_more").toBool() && m_pins.count() < m_pins.maximumRows() &&
            !cursor.isEmpty() && cursor != previousCursor;
        if (result.value("has_more").toBool() && (cursor.isEmpty() || cursor == previousCursor))
            m_pinsError = QStringLiteral("Couldn’t load the next page of pins. Refresh to try again.");
        emit changed();
    });
}


namespace {
const QStringList searchFilters = {"from", "mentions", "in", "has", "before", "after", "on", "during", "pinned", "author", "sort"};
const QStringList searchHas = {"image", "video", "sound", "file", "link", "embed", "sticker", "poll", "snapshot"};
QString unquote(QString value) {
    if (value.startsWith('"') && value.endsWith('"')) value = value.mid(1, value.size() - 2);
    return value;
}
QString quoteSearch(QString value) {
    value.replace('"', "");
    return value.contains(' ') ? '"' + value + '"' : value;
}
}

QString ChannelTools::resolveSearchTarget(const QString &input, bool channel, QString *error) const {
    QString value = unquote(input);
    if (value == "me" && !channel) return m_client->currentUserId();
    static const QRegularExpression mention("^<[@#]!?([0-9]+)>$");
    const auto match = mention.match(value);
    if (match.hasMatch()) value = match.captured(1);
    if (DiscordClient::validId(value)) return value;
    if (value.startsWith(channel ? '#' : '@')) value.remove(0, 1);
    QSet<QString> matches;
    if (channel) {
        for (const auto &row : m_client->m_channels.rows())
            if (row.value("name").toString().compare(value, Qt::CaseInsensitive) == 0 &&
                row.value("kind").toString() != "category") matches.insert(row.value("recordId").toString());
    } else {
        // Account usernames are unique, even when somebody else's display name matches.
        for (auto it = m_client->m_users.cbegin(); it != m_client->m_users.cend(); ++it)
            if (it.value().value("username").toString().compare(value, Qt::CaseInsensitive) == 0) return it.key();
        for (auto it = m_client->m_users.cbegin(); it != m_client->m_users.cend(); ++it) {
            const auto user = it.value();
            const auto member = m_client->m_guildMembers.value(m_client->currentGuildId()).value(it.key()).toVariantMap();
            if (user.value("username").toString().compare(value, Qt::CaseInsensitive) == 0 ||
                user.value("global_name").toString().compare(value, Qt::CaseInsensitive) == 0 ||
                member.value("nick").toString().compare(value, Qt::CaseInsensitive) == 0)
                matches.insert(it.key());
        }
    }
    if (matches.size() == 1) return *matches.cbegin();
    *error = matches.isEmpty() ? QStringLiteral("Unknown %1 ‘%2’. Choose a suggestion or paste an ID.").arg(channel ? "channel" : "user", value)
                              : QStringLiteral("Several %1 match ‘%2’. Use a unique username or ID.").arg(channel ? "channels" : "users", value);
    return {};
}


void ChannelTools::requestSearchSuggestions(const QString &text, int cursor) {
    const auto operation = ++m_suggestionOperation;
    if (!ready() || text.size() > 1024 || !DiscordClient::validId(m_client->currentGuildId())) return;
    static const QRegularExpression target(R"re((?:^|\s)(?:from|mentions):\s*"?([^\s"]{2,32})$)re", QRegularExpression::CaseInsensitiveOption);
    const auto match = target.match(text.left(std::clamp(cursor, 0, int(text.size()))));
    if (!match.hasMatch()) return;
    auto needle = match.captured(1);
    if (needle.startsWith('@')) needle.remove(0, 1);
    if (needle == "me" || DiscordClient::validId(needle)) return;
    const auto guild = m_client->currentGuildId();
    const auto session = m_client->m_generation;
    const auto key = guild + ":" + needle.toLower();
    if (key == m_suggestionKey && m_suggestionTime.isValid() && m_suggestionTime.elapsed() < 3000) return;
    m_suggestionKey = key; m_suggestionTime.start();
    m_client->request("GET", "/guilds/" + guild + "/members/search?query=" + encoded(needle) + "&limit=25", {},
        [this, weak = QPointer<ChannelTools>(this), operation, guild, session](const QJsonValue &value, const QString &error) {
            if (!weak || !m_client || operation != m_suggestionOperation || session != m_client->m_generation || guild != m_client->currentGuildId() || !error.isEmpty()) return;
            int count = 0;
            for (const auto &entry : value.toArray()) {
                if (++count > 25) break;
                m_client->cacheMember(guild, entry.toObject());
            }
            emit searchSuggestionsChanged();
        });
}

QVariantList ChannelTools::searchSuggestions(const QString &text, int cursor) const {
    QVariantList result;
    if (!m_client || text.size() > 1024) return result;
    cursor = std::clamp(cursor, 0, int(text.size()));
    // Find the token at the caret, including quoted names and whitespace after a colon.
    static const QRegularExpression tokens(R"re((?:[A-Za-z]+:\s*)?(?:"[^"]*"?|[^\s"]+))re");
    int start = cursor, end = cursor;
    auto iterator = tokens.globalMatch(text);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        if (match.capturedStart() <= cursor && match.capturedEnd() >= cursor) {
            start = match.capturedStart(); end = match.capturedEnd(); break;
        }
    }
    const auto prefix = text.mid(start, cursor - start);
    const int colon = prefix.indexOf(':');
    const auto key = prefix.left(colon).toLower();
    auto needle = unquote(prefix.mid(colon + 1).trimmed()).toLower();
    if (needle.startsWith('"')) needle.remove(0, 1);
    if ((key == "from" || key == "mentions") && needle.startsWith('@')) needle.remove(0, 1);
    auto add = [&](const QString &label, const QString &replacement, const QString &detail) {
        if (result.size() >= 12) return;
        const auto completed = text.left(start) + replacement + text.mid(end);
        result.append(QVariantMap{{"label", label}, {"detail", detail}, {"text", completed}, {"cursor", start + replacement.size()}});
    };
    if (colon < 0) {
        for (const auto &filter : searchFilters)
            if (filter.startsWith(prefix.toLower())) add(filter + ":", filter + ":", "Search filter");
    } else if (key == "from" || key == "mentions") {
        if (QStringLiteral("me").startsWith(needle)) add("me", key + ":me ", "Your account");
        auto ids = m_client->m_users.keys();
        std::sort(ids.begin(), ids.end(), [this](const QString &a, const QString &b) {
            return m_client->m_users.value(a).value("username").toString().toLower() < m_client->m_users.value(b).value("username").toString().toLower();
        });
        for (const auto &id : ids) {
            if (result.size() >= 12) break;
            const auto user = m_client->m_users.value(id);
            const auto username = user.value("username").toString();
            const auto display = DiscordClient::displayName(user);
            const auto nick = m_client->m_guildMembers.value(m_client->currentGuildId()).value(id).value("nick").toString();
            if (username.isEmpty()) continue;
            if (needle.isEmpty() || username.contains(needle, Qt::CaseInsensitive) || display.contains(needle, Qt::CaseInsensitive) || nick.contains(needle, Qt::CaseInsensitive) || id.startsWith(needle))
                add(nick.isEmpty() ? display : nick, key + ":" + quoteSearch(username) + " ", "@" + username);
        }
    } else if (key == "in") {
        for (const auto &row : m_client->m_channels.rows()) {
            const auto name = row.value("name").toString();
            if (row.value("kind").toString() == "category") continue;
            if (name.contains(needle.startsWith('#') ? needle.mid(1) : needle, Qt::CaseInsensitive))
                add("#" + name, "in:" + row.value("recordId").toString() + " ", "Channel");
        }
    } else {
        QStringList values;
        if (key == "has") { values = searchHas; for (const auto &v : searchHas) values << "-" + v; }
        else if (key == "pinned") values = {"true", "false"};
        else if (key == "author") values = {"user", "bot", "webhook", "-user", "-bot", "-webhook"};
        else if (key == "sort") values = {"newest", "oldest", "relevant"};
        else if (key == "before" || key == "after" || key == "on" || key == "during") {
            values = {"today", "yesterday", QDate::currentDate().toString(Qt::ISODate)};
            if (key == "during") values << QDate::currentDate().toString("yyyy-MM") << QDate::currentDate().toString("yyyy");
        }
        for (const auto &value : values)
            if (value.startsWith(needle)) add(value, key + ":" + value + " ", "Filter value");
    }
    return result;
}

bool ChannelTools::parseSearch() {
    m_searchParameters.clear();
    QStringList content;
    QString sortBy = "timestamp", sortOrder = "desc";
    qint64 lower = 0, upper = std::numeric_limits<qint64>::max();
    bool hasLower = false, hasUpper = false;
    auto parameter = [this](const QString &key, const QString &value) { m_searchParameters += "&" + key + "=" + encoded(value); };
    static const QRegularExpression tokens(R"re((?:[A-Za-z]+:\s*)?(?:"[^"]*"|[^\s"]+))re");
    auto iterator = tokens.globalMatch(m_query);
    while (iterator.hasNext()) {
        const auto token = iterator.next().captured();
        const int colon = token.indexOf(':');
        const auto key = token.left(colon).toLower();
        if (colon < 0 || !searchFilters.contains(key)) { content << token; continue; }
        auto value = unquote(token.mid(colon + 1).trimmed());
        if (value.isEmpty()) { m_searchError = "Choose a value for " + key + ":"; return false; }
        if (key == "from" || key == "mentions" || key == "in") {
            const auto id = resolveSearchTarget(value, key == "in", &m_searchError);
            if (id.isEmpty()) return false;
            if (key == "in" && m_searchGuild.isEmpty() && id != m_searchChannel) {
                m_searchError = "Search this direct message without an in: filter for another channel."; return false;
            }
            parameter(key == "from" ? "author_id" : key == "in" ? "channel_id" : "mentions", id);
        } else if (key == "has" || key == "author") {
            const auto plain = value.startsWith('-') ? value.mid(1) : value;
            const QStringList allowed = key == "has" ? searchHas : QStringList{"user", "bot", "webhook"};
            if (!allowed.contains(plain)) { m_searchError = "Choose a suggested value for " + key + ":"; return false; }
            parameter(key == "has" ? "has" : "author_type", value);
        } else if (key == "pinned") {
            if (value != "true" && value != "false") { m_searchError = "Use pinned:true or pinned:false."; return false; }
            parameter("pinned", value);
        } else if (key == "sort") {
            if (!QStringList{"newest", "oldest", "relevant"}.contains(value)) { m_searchError = "Use sort:newest, sort:oldest or sort:relevant."; return false; }
            sortBy = value == "relevant" ? "relevance" : "timestamp"; sortOrder = value == "oldest" ? "asc" : "desc";
        } else {
            QDate date;
            int period = 0;
            if (value == "today") date = QDate::currentDate();
            else if (value == "yesterday") date = QDate::currentDate().addDays(-1);
            else {
                if (key == "during" && value.size() == 4) { value += "-01-01"; period = 2; }
                else if (key == "during" && value.size() == 7) { value += "-01"; period = 1; }
                date = QDate::fromString(value, Qt::ISODate);
            }
            if (!date.isValid() || date.year() < 2015 || date.year() > 2100) { m_searchError = "Use a valid date (YYYY-MM-DD), today or yesterday. during: also accepts YYYY-MM or YYYY."; return false; }
            // Discord snowflakes encode UTC milliseconds since 2015-01-01.
            const auto boundary = [](QDate d) { return QDateTime(d, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch(); };
            const auto begin = boundary(date);
            const auto end = boundary(period == 2 ? date.addYears(1) : period == 1 ? date.addMonths(1) : date.addDays(1));
            if (key == "before") { upper = std::min(upper, begin); hasUpper = true; }
            else if (key == "after") { lower = std::max(lower, end); hasLower = true; }
            else { lower = std::max(lower, begin); upper = std::min(upper, end); hasLower = hasUpper = true; }
        }
    }
    if (hasLower && hasUpper && lower >= upper) { m_searchError = "These dates do not overlap. Choose a wider range."; return false; }
    const auto snowflake = [](qint64 ms) { return QString::number(quint64(std::max(qint64(0), ms - 1420070400000LL)) << 22); };
    if (hasLower) parameter("min_id", snowflake(lower));
    if (hasUpper) parameter("max_id", snowflake(upper));
    if (!content.isEmpty()) parameter("content", content.join(' '));
    parameter("sort_by", sortBy); parameter("sort_order", sortOrder);
    return true;
}

bool ChannelTools::searchMessages(const QString &query) {
    clearSearch();
    m_query = query.trimmed();
    if (!ready() || m_query.isEmpty() || m_query.size() > 1024) {
        m_searchError = !ready() ? QStringLiteral("Connect to Discord to search messages.") : QStringLiteral("Enter between 1 and 1,024 characters to search.");
        emit changed(); return false;
    }
    m_searchGuild = m_client->currentGuildId();
    m_searchChannel = m_client->currentChannelId();
    m_searchScope = m_searchGuild.isEmpty() ? m_client->currentChannelName() : m_client->currentGuildName();
    if (m_searchGuild.isEmpty() && !DiscordClient::validId(m_searchChannel)) {
        m_searchError = QStringLiteral("Choose a server or direct message to search.");
        emit changed(); return false;
    }
    if (!parseSearch()) { emit changed(); return false; }
    m_indexingTime.start();
    fetchSearch(m_searchOperation);
    return true;
}

void ChannelTools::clearSearch() {
    ++m_searchOperation;
    m_search.clear();
    m_resultChannels.clear();
    m_searchBusy = m_searchMore = false;
    m_searchError.clear(); m_searchStatus.clear(); m_query.clear();
    m_searchGuild.clear(); m_searchChannel.clear(); m_searchScope.clear();
    m_searchParameters.clear();
    m_searchOffset = m_totalResults = 0;
    emit changed();
}

void ChannelTools::loadMoreSearch() {
    if (!ready() || m_searchBusy || !m_searchMore) return;
    m_indexingTime.start();
    fetchSearch(m_searchOperation);
}

void ChannelTools::fetchSearch(quint64 operation, int indexingAttempts) {
    if (!m_client || operation != m_searchOperation) return;
    m_searchBusy = true;
    m_searchError.clear();
    m_searchStatus = indexingAttempts ? QStringLiteral("Discord is preparing the search index…") : QStringLiteral("Searching…");
    const QString path = (m_searchGuild.isEmpty() ? "/channels/" + m_searchChannel : "/guilds/" + m_searchGuild) +
        "/messages/search?limit=25&offset=" + QString::number(m_searchOffset) + m_searchParameters;
    emit changed();
    m_client->request("GET", path, {}, [this, weak = QPointer<ChannelTools>(this), operation, indexingAttempts](const QJsonValue &value, const QString &error) {
        if (!weak || !m_client || operation != m_searchOperation) return;
        if (!error.isEmpty()) {
            m_searchBusy = false; m_searchStatus.clear(); m_searchError = error; emit changed(); return;
        }
        const auto result = value.toObject();
        if (result.value("code").toInt() == 110000) {
            double seconds = result.value("retry_after").toDouble(1.0);
            if (!std::isfinite(seconds) || seconds < 0) seconds = 1.0;
            const int delay = int(std::clamp(seconds, 0.25, 10.0) * 1000);
            if (indexingAttempts < 3 && seconds <= 10.0 && m_indexingTime.elapsed() + delay <= 20000) {
                m_searchStatus = QStringLiteral("Discord is preparing the search index…");
                emit changed();
                QTimer::singleShot(delay, this, [this, operation, indexingAttempts] {
                    if (operation == m_searchOperation && ready()) fetchSearch(operation, indexingAttempts + 1);
                    else if (operation == m_searchOperation) { m_searchBusy = false; m_searchStatus.clear(); emit changed(); }
                });
                return;
            }
            m_searchBusy = false; m_searchStatus.clear();
            m_searchError = QStringLiteral("Discord is still indexing these messages. Try searching again in a moment.");
            emit changed(); return;
        }
        m_searchBusy = false;
        m_searchStatus.clear();
        if (!result.value("messages").isArray() || !result.value("total_results").isDouble()) {
            m_searchError = QStringLiteral("Discord returned an unexpected search response."); emit changed(); return;
        }
        for (const auto &entry : result.value("threads").toArray()) {
            auto channel = entry.toObject();
            const auto id = channel.value("id").toString();
            if (!DiscordClient::validId(id) || m_resultChannels.size() >= 500) continue;
            channel.insert("guild_id", m_searchGuild);
            m_resultChannels.insert(id, channel);
        }
        const auto groups = result.value("messages").toArray();
        int received = 0;
        for (int i = 0; i < std::min(25, int(groups.size())); ++i) {
            const auto group = groups.at(i).toArray();
            for (int j = 0; j < std::min(25, int(group.size())); ++j) {
                auto message = group.at(j).toObject();
                if (message.contains("hit") && !message.value("hit").toBool()) continue;
                const auto id = message.value("id").toString();
                const auto channelId = message.value("channel_id").toString();
                if (!DiscordClient::validId(id) || !DiscordClient::validId(channelId)) continue;
                if (m_searchGuild.isEmpty() && channelId != m_searchChannel) continue;
                if (++received > 25) break;
                if (m_search.count() < m_search.maximumRows() || m_search.indexOf(id) >= 0)
                    m_search.upsert(resultRow(message, m_searchGuild));
            }
            if (received >= 25) break;
        }
        m_totalResults = std::clamp(result.value("total_results").toDouble(), 0.0, double(std::numeric_limits<int>::max()));
        m_searchOffset += 25;
        m_searchMore = m_searchOffset < m_totalResults && m_searchOffset <= 9975 && m_search.count() < m_search.maximumRows();
        m_searchStatus = m_search.count() >= m_search.maximumRows() && m_totalResults > m_search.count()
            ? QStringLiteral("Showing the first 500 results. Narrow your search to find more.") : QString{};
        emit changed();
    });
}

void ChannelTools::clearActionError() { m_actionError.clear(); emit changed(); }

bool ChannelTools::messageAction(const QString &action, const QString &channelId, const QString &messageId,
                                 const QString &text, const QString &targetChannel) {
    if (m_actionBusy) return false;
    m_actionError.clear();
    auto fail = [this](const QString &reason) { m_actionError = reason; emit changed(); return false; };
    if (!ready() || !DiscordClient::validId(channelId) || !DiscordClient::validId(messageId))
        return fail("Connect to Discord and choose a message first.");
    const auto source = m_client->m_messageObjects.value(messageId);
    if ((action == "edit" || action == "delete") &&
        (source.value("channel_id").toString() != channelId ||
         source.value("author").toObject().value("id").toString() != m_client->currentUserId()))
        return fail("You can only edit or delete your own loaded messages.");
    QByteArray method;
    QString path = "/channels/" + channelId + "/messages/" + messageId;
    QJsonObject body;
    if (action == "delete") method = "DELETE";
    else if (action == "edit" || action == "reply") {
        if (text.trimmed().isEmpty() || text.size() > 2000) return fail("Enter a message of 1 to 2,000 characters.");
        body = {{"content", text}, {"allowed_mentions", QJsonObject{{"parse", QJsonArray{}}, {"replied_user", false}}}};
        method = action == "edit" ? "PATCH" : "POST";
        if (action == "reply") {
            path = "/channels/" + channelId + "/messages";
            body.insert("message_reference", QJsonObject{{"message_id", messageId}, {"channel_id", channelId}, {"fail_if_not_exists", true}});
        }
    } else if (action == "forward") {
        if (!DiscordClient::validId(targetChannel)) return fail("Choose a destination channel.");
        method = "POST"; path = "/channels/" + targetChannel + "/messages";
        body.insert("message_reference", QJsonObject{{"type", 1}, {"message_id", messageId}, {"channel_id", channelId}});
        body.insert("allowed_mentions", QJsonObject{{"parse", QJsonArray{}}});
    } else if (action == "react" || action == "unreact") {
        if (text.isEmpty() || text.size() > 100 || text.contains('/') || text.contains('\\'))
            return fail("Choose a valid emoji.");
        method = action == "react" ? "PUT" : "DELETE";
        path += "/reactions/" + encoded(text) + "/@me";
    } else return fail("Unknown message action.");
    const auto operation = ++m_actionOperation;
    m_actionBusy = true; emit changed();
    m_client->request(method, path, body,
        [this, weak = QPointer<ChannelTools>(this), operation, action, channelId, messageId, targetChannel](const QJsonValue &value, const QString &error) {
        if (!weak || !m_client || operation != m_actionOperation) return;
        m_actionBusy = false;
        if (!error.isEmpty()) { m_actionError = error; emit changed(); return; }
        if (action == "edit" || action == "reply" || action == "forward") {
            const auto message = value.toObject();
            const auto destination = action == "forward" ? targetChannel : channelId;
            if (!DiscordClient::validId(message.value("id").toString()) ||
                (action == "edit" && message.value("id").toString() != messageId) ||
                (!message.value("channel_id").toString().isEmpty() && message.value("channel_id").toString() != destination)) {
                m_actionError = "Discord did not confirm this action. Check the conversation before retrying.";
                emit changed(); return;
            }
        }
        emit messageActionFinished(action, messageId);
        if (action == "delete") {
            m_client->dispatch("MESSAGE_DELETE", {{"channel_id", channelId}, {"id", messageId}});
            m_search.remove(messageId); if (m_pinsChannel == channelId) m_pins.remove(messageId);
        } else if (action == "react" || action == "unreact") {
            // Gateway events own reaction counts; a parallel GET can double-count an arriving event.
        }
        else {
            auto message = value.toObject();
            const auto destination = action == "forward" ? targetChannel : channelId;
            if (DiscordClient::validId(message.value("id").toString())) {
                message.insert("channel_id", destination);
                m_client->acceptMessage(message, action == "edit");
            }
        }
        emit changed();
    });
    return true;
}

bool ChannelTools::setPinned(const QString &channelId, const QString &messageId, bool pinned) {
    if (m_actionBusy) return false;
    m_actionError.clear();
    if (!ready() || !DiscordClient::validId(channelId) || !DiscordClient::validId(messageId)) {
        m_actionError = QStringLiteral("Connect to Discord and choose a message first."); emit changed(); return false;
    }
    const auto operation = ++m_actionOperation;
    m_actionBusy = true;
    emit changed();
    m_client->request(pinned ? "PUT" : "DELETE", "/channels/" + channelId + "/messages/pins/" + messageId, {},
        [this, weak = QPointer<ChannelTools>(this), operation, channelId, messageId, pinned](const QJsonValue &, const QString &error) {
        if (!weak || !m_client || operation != m_actionOperation) return;
        m_actionBusy = false;
        if (!error.isEmpty()) { m_actionError = error; emit changed(); return; }
        if (m_client->currentChannelId() == channelId && m_client->m_messageObjects.contains(messageId)) {
            m_client->m_messageObjects[messageId].insert("pinned", pinned);
            m_client->m_messages.patch(messageId, {{"pinned", pinned}});
        }
        if (m_search.byId(messageId).value("channelId").toString() == channelId)
            m_search.patch(messageId, {{"pinned", pinned}});
        if (m_pinsChannel == channelId) {
            if (!pinned) m_pins.remove(messageId);
            // Invalidate an in-flight pins read before refreshing after the write.
            readPins(channelId);
        }
        emit changed();
        emit pinnedChanged(channelId, messageId, pinned);
    });
    return true;
}

bool ChannelTools::openMessage(const QString &channelId, const QString &messageId) {
    m_actionError.clear();
    if (!ready() || !DiscordClient::validId(channelId) || !DiscordClient::validId(messageId)) {
        m_actionError = QStringLiteral("Connect to Discord and choose a message first."); emit changed(); return false;
    }
    if (m_jumpHistorySelection && m_jumpHistorySelection == m_client->m_selectionGeneration) {
        // A replacement jump may fail before selecting any channel. Release the
        // previous owned history read now so its abandoned loading state cannot stick.
        m_lastSelection = ++m_client->m_selectionGeneration;
        m_client->m_loadingMessages = false;
        emit m_client->stateChanged();
    }
    m_jumpHistorySelection = 0;
    const auto operation = ++m_jumpOperation;
    m_jumpBusy = true;
    emit changed();
    const auto channel = knownChannel(channelId);
    if (!channel.isEmpty()) { navigateAndFetch(channelId, messageId, operation); return true; }
    m_client->request("GET", "/channels/" + channelId, {},
        [this, weak = QPointer<ChannelTools>(this), operation, channelId, messageId](const QJsonValue &value, const QString &error) {
        if (!weak || !m_client || operation != m_jumpOperation) return;
        auto channel = value.toObject();
        if (!error.isEmpty() || channel.value("id").toString() != channelId) {
            m_jumpBusy = false;
            m_actionError = error.isEmpty() ? QStringLiteral("This message’s channel is no longer available.") : error;
            emit changed(); return;
        }
        if (m_resultChannels.size() >= 500) m_resultChannels.erase(m_resultChannels.begin());
        m_resultChannels.insert(channelId, channel);
        navigateAndFetch(channelId, messageId, operation);
    });
    return true;
}

void ChannelTools::navigateAndFetch(const QString &channelId, const QString &messageId, quint64 operation) {
    if (!m_client || operation != m_jumpOperation) return;
    auto *client = m_client.data();
    const auto channel = knownChannel(channelId);
    const auto guild = channel.value("guild_id").toString();
    if (!guild.isEmpty() && client->m_guilds.indexOf(guild) >= 0) {
        auto channels = client->m_channelCache.value(guild);
        bool found = false;
        for (const auto &entry : channels) if (entry.toObject().value("id").toString() == channelId) { found = true; break; }
        if (!found && channels.size() < 2000) {
            channels.append(channel);
            client->cacheChannels(guild, channels);
            if (client->currentGuildId() == guild) client->rebuildChannels(false);
        }
    } else if (guild.isEmpty() && (channel.value("type").toInt(-1) == 1 || channel.value("type").toInt(-1) == 3)) {
        client->acceptDm(channel);
    }
    m_navigating = true;
    client->activateChannel(channelId);
    m_navigating = false;
    if (client->currentChannelId() != channelId) {
        m_jumpBusy = false; m_actionError = QStringLiteral("This message’s channel is no longer available."); emit changed(); return;
    }
    const int existing = client->m_messages.indexOf(messageId);
    if (existing >= 0) {
        m_jumpBusy = false; emit changed(); emit messageOpened(channelId, messageId, existing); return;
    }
    // Cancel the normal initial-history callback started by activateChannel().
    // Its response must not replace the specifically requested historical window.
    const auto selection = ++client->m_selectionGeneration;
    m_jumpHistorySelection = selection;
    m_lastSelection = selection;
    client->m_messages.clear();
    client->m_messageObjects.clear();
    client->m_loadingMessages = true;
    client->m_hasMoreMessages = false;
    client->m_hasNewerMessages = true;
    client->m_deferredLiveMessages.clear();
    emit client->stateChanged();
    client->request("GET", "/channels/" + channelId + "/messages?around=" + messageId + "&limit=50", {},
        [this, weak = QPointer<ChannelTools>(this), operation, selection, channelId, messageId](const QJsonValue &value, const QString &error) {
        if (!weak || !m_client || operation != m_jumpOperation || selection != m_client->m_selectionGeneration ||
            channelId != m_client->currentChannelId()) return;
        m_jumpBusy = false;
        m_jumpHistorySelection = 0;
        m_client->m_loadingMessages = false;
        if (!error.isEmpty() || !value.isArray()) {
            m_actionError = error.isEmpty() ? QStringLiteral("Discord returned an unexpected message response.") : error;
            emit m_client->stateChanged(); emit changed(); return;
        }
        QJsonArray page;
        const auto messages = value.toArray();
        for (int i = 0; i < std::min(50, int(messages.size())); ++i) {
            const auto message = messages.at(i).toObject();
            if (message.value("channel_id").toString() == channelId && DiscordClient::validId(message.value("id").toString())) page.append(message);
        }
        m_client->mergeMessages(page);
        m_client->m_hasMoreMessages = page.size() == 50;
        const int index = m_client->m_messages.indexOf(messageId);
        if (index < 0) m_actionError = QStringLiteral("That message was deleted or is no longer available.");
        emit m_client->stateChanged();
        emit changed();
        if (index >= 0) emit messageOpened(channelId, messageId, index);
    });
}
