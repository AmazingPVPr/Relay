#include "DiscordClient.h"
#include "RelayDiscordBridge.h"
#include <QSysInfo>
#include "InlineMedia.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QClipboard>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QUrlQuery>
#include <QTimeZone>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr int PageSize = 50;
constexpr qint64 MaximumResponse = 16 * 1024 * 1024;
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
QJsonObject merged(QJsonObject original, const QJsonObject &update) {
    for (auto it = update.begin(); it != update.end(); ++it) original.insert(it.key(), it.value());
    return original;
}
bool validGateway(const QUrl &url) {
    const auto host = url.host().toLower();
    return url.scheme() == "wss" && url.userInfo().isEmpty() &&
           (url.port() == -1 || url.port() == 443) &&
           (host == "gateway.discord.gg" || host.endsWith(".discord.gg"));
}
}

DiscordClient::DiscordClient(QObject *parent) : QObject(parent), m_network(this) {
    m_readReceiptTimer.setSingleShot(true);
    connect(&m_readReceiptTimer, &QTimer::timeout, this, &DiscordClient::flushReadReceipt);
    connect(this, &DiscordClient::stateChanged, this, [this] {
        if (m_connected && !m_readReceiptInFlight && !m_pendingReadReceipts.isEmpty() && !m_readReceiptTimer.isActive())
            m_readReceiptTimer.start(250);
    });
    // The rail shares the conversation model's activity order and presentation.
    connect(&m_dms, &QAbstractItemModel::modelReset, this, &DiscordClient::rebuildUnreadDms);
    connect(&m_dms, &QAbstractItemModel::rowsInserted, this, &DiscordClient::rebuildUnreadDms);
    connect(&m_dms, &QAbstractItemModel::rowsRemoved, this, &DiscordClient::rebuildUnreadDms);
    connect(&m_dms, &QAbstractItemModel::rowsMoved, this, &DiscordClient::rebuildUnreadDms);
    connect(&m_dms, &QAbstractItemModel::dataChanged, this, &DiscordClient::rebuildUnreadDms);
    m_unreadDmMuteTimer.setSingleShot(true);
    connect(&m_unreadDmMuteTimer, &QTimer::timeout, this, [this] {
        ++m_unreadDmBatchDepth;
        const auto finishBatch=qScopeGuard([this] { if (!--m_unreadDmBatchDepth && m_unreadDmsDirty) rebuildUnreadDms(); });
        for (auto it=m_dmObjects.cbegin(); it!=m_dmObjects.cend(); ++it) syncDm(it.key());
    });
    m_heartbeat.setSingleShot(true);
    m_reconnect.setSingleShot(true);
    m_requestPump.setSingleShot(true);
    m_gatewayDeadline.setSingleShot(true);
    m_memberDeadline.setSingleShot(true);
    connect(&m_memberDeadline, &QTimer::timeout, this, [this] {
        m_membersLoading = false;
        rebuildMembers();
    });
    m_socket.setMaxAllowedIncomingFrameSize(MaximumResponse);
    m_socket.setMaxAllowedIncomingMessageSize(MaximumResponse);
    connect(&m_heartbeat, &QTimer::timeout, this, &DiscordClient::heartbeat);
    connect(&m_reconnect, &QTimer::timeout, this, &DiscordClient::openGateway);
    connect(&m_requestPump, &QTimer::timeout, this, &DiscordClient::pumpRequests);
    connect(&m_gatewayDeadline, &QTimer::timeout, this, [this] {
        reconnect(false, 1500);
    });
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &DiscordClient::onGatewayText);
    connect(&m_socket, &QWebSocket::disconnected, this, &DiscordClient::onDisconnected);
    connect(&m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (!m_closing && !m_token.isEmpty()) reconnect(false);
    });
}

DiscordClient::~DiscordClient() {
    m_closing = true;
    ++m_generation;
    m_socket.abort();
    const auto replies = m_replies;
    for (auto *reply : replies) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
    }
    m_token.fill('\0');
}

bool DiscordClient::validId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9]{1,22}$"));
    return pattern.match(id).hasMatch();
}

QString DiscordClient::cleanText(QString text, int limit) {
    text = text.left(limit);
    // Strip control characters except ordinary line breaks and tabs; never interpret markup.
    text.remove(QRegularExpression(QStringLiteral("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F\\x7F]")));
    return text;
}

QString DiscordClient::displayName(const QJsonObject &user) {
    auto name = user.value("global_name").toString();
    if (name.isEmpty()) name = user.value("username").toString();
    return cleanText(name.isEmpty() ? QStringLiteral("Unknown user") : name, 128);
}

QString DiscordClient::initials(const QString &name) {
    const auto words = name.simplified().split(' ', Qt::SkipEmptyParts);
    if (words.isEmpty()) return QStringLiteral("?");
    QString result = words.first().left(1);
    if (words.size() > 1) result += words.last().left(1);
    else result = name.left(2);
    return result.toUpper();
}

QString DiscordClient::safeCdnUrl(const QString &raw) {
    const QUrl url(raw);
    const auto host = url.host().toLower();
    if (!url.isValid() || url.scheme() != "https" || !url.userInfo().isEmpty() ||
        (url.port() != -1 && url.port() != 443) ||
        (host != "cdn.discordapp.com" && host != "media.discordapp.net")) return {};
    return url.toString(QUrl::FullyEncoded);
}

QString DiscordClient::avatarUrl(const QJsonObject &user) {
    const auto id = user.value("id").toString();
    const auto hash = user.value("avatar").toString();
    static const QRegularExpression safeHash("^[a-zA-Z0-9_]{1,128}$");
    if (!validId(id) || !safeHash.match(hash).hasMatch()) return {};
    return QStringLiteral("https://cdn.discordapp.com/avatars/%1/%2.png?size=4096").arg(id, hash);
}

QString DiscordClient::channelKind(int type) {
    switch (type) {
    case 0: case 5: return "text";
    case 1: case 3: return "dm";
    case 2: return "voice";
    case 4: return "category";
    case 10: case 11: case 12: return "thread";
    case 13: return "stage";
    case 15: case 16: return "forum";
    default: return {};
    }
}

QVariantMap DiscordClient::guildInfo(const QString &guildId) const {
    if (!validId(guildId)) return {};
    const auto guild = m_guildObjects.value(guildId);
    const auto row = m_guilds.byId(guildId);
    return {{"id", guildId}, {"joined", !guild.isEmpty()}, {"name", row.value("name").toString()},
        {"iconUrl", row.value("iconUrl").toString()}, {"description", cleanText(guild.value("description").toString(), 1000)},
        {"memberCount", guild.value("member_count").toInt(guild.value("approximate_member_count").toInt())},
        {"countKnown", guild.contains("member_count") || guild.contains("approximate_member_count")},
        {"onlineCount", guild.value("approximate_presence_count").toInt()},
        {"onlineCountKnown", guild.contains("approximate_presence_count")}};
}

bool DiscordClient::navigateGuild(const QString &guildId) {
    if (!m_guildObjects.contains(guildId) || m_guilds.indexOf(guildId) < 0) return false;
    selectGuild(guildId);
    return true;
}

QVariantMap DiscordClient::guildHoverInfo(const QString &guildId) const {
    if (!m_guildObjects.contains(guildId)) return {};
    auto info=guildInfo(guildId);
    const auto settings=m_notificationSettings.value(guildId);
    const auto end=QDateTime::fromString(settings.value("mute_config").toObject().value("end_time").toString(),Qt::ISODateWithMs);
    info.insert("muted",settings.value("muted").toBool() && (!end.isValid() || end.toMSecsSinceEpoch()>nowMs()));
    info.insert("unread",m_guilds.byId(guildId).value("unread"));
    QMap<QString,QVariantList> grouped;int people=0;
    const auto states=m_voiceStates.value(guildId);
    for(auto it=states.cbegin();it!=states.cend();++it){
        const auto channel=it.value().value("channel_id").toString();if(channel.isEmpty())continue;++people;
        if(grouped[channel].size()>=6)continue;
        auto user=m_users.value(it.key());if(user.isEmpty())user=it.value().value("member").toObject().value("user").toObject();
        auto person=memberPresentation(guildId,user,m_guildMembers.value(guildId).value(it.key()));
        person.insert("streaming",it.value().value("streaming").toBool());
        person.insert("camera",it.value().value("camera").toBool());grouped[channel].append(person);
    }
    QVariantList channels;
    for(auto it=grouped.cbegin();it!=grouped.cend() && channels.size()<4;++it){
        QString name="Voice channel";
        for(const auto &entry:m_channelCache.value(guildId))if(entry.toObject().value("id").toString()==it.key()){name=entry.toObject().value("name").toString();break;}
        channels.append(QVariantMap{{"name",name},{"people",it.value()}});
    }
    info.insert("voiceChannels",channels);info.insert("voiceCount",people);return info;
}

QVariantMap DiscordClient::primaryGuild(const QJsonObject &user) const {
    const auto primary = user.value("primary_guild").toObject();
    const auto id = primary.value("identity_guild_id").toString();
    const auto tag = cleanText(primary.value("tag").toString(), 8);
    if (!primary.value("identity_enabled").toBool() || !validId(id) || tag.isEmpty()) return {};
    const auto badge = primary.value("badge").toString();
    QString badgeUrl;
    if (QRegularExpression("^[a-zA-Z0-9_]{1,128}$").match(badge).hasMatch())
        badgeUrl = QString("https://cdn.discordapp.com/guild-tag-badges/%1/%2.png?size=4096").arg(id, badge);
    const auto info = guildInfo(id);
    return {{"id", id}, {"tag", tag}, {"badgeUrl", badgeUrl}, {"name", info.value("name")},
        {"iconUrl", info.value("iconUrl")}, {"joined", info.value("joined")}};
}

QVariantMap DiscordClient::guildRow(const QJsonObject &guild) const {
    const auto id = guild.value("id").toString();
    auto name = cleanText(guild.value("name").toString(), 128);
    if (name.isEmpty()) name = QStringLiteral("Loading server…");
    QString icon;
    const auto hash = guild.value("icon").toString();
    if (QRegularExpression("^[a-zA-Z0-9_]{1,128}$").match(hash).hasMatch() && validId(id))
        icon = QStringLiteral("https://cdn.discordapp.com/icons/%1/%2.png?size=4096").arg(id, hash);
    const auto states = m_voiceStates.value(id);
    const bool streaming = std::any_of(states.cbegin(), states.cend(), [](const auto &state) {
        return state.value("streaming").toBool();
    });
    return {{"recordId", id}, {"name", name}, {"initials", initials(name)},
            {"iconUrl", icon}, {"kind", "guild"}, {"unread", m_unread.value(id)},
            {"voiceActive", !states.isEmpty()}, {"streaming", streaming}};
}

QVariantMap DiscordClient::channelRow(const QJsonObject &channel, const QString &guildId) const {
    const auto id = channel.value("id").toString();
    const auto kind = channelKind(channel.value("type").toInt());
    QString name = cleanText(channel.value("name").toString(), 128);
    QString avatar;
    QString status = "unknown", activity;
    bool inVoice = false, streaming = false;
    QVariantMap primary, indicators;
    QString recipientId;
    if (kind == "dm") {
        QStringList recipients;
        auto users = channel.value("recipients").toArray();
        if (users.isEmpty()) {
            for (const auto &recipientId : channel.value("recipient_ids").toArray())
                users.append(m_users.value(recipientId.toString()));
        }
        for (const auto &value : users) {
            const auto user = merged(value.toObject(), m_users.value(value.toObject().value("id").toString()));
            if (user.value("id").toString() == m_currentUserId) continue;
            auto recipientName = cleanText(m_relationships.value(user.value("id").toString()).value("nickname").toString(), 32);
            recipients.append(recipientName.isEmpty() ? displayName(user) : recipientName);
            if (avatar.isEmpty()) avatar = avatarUrl(user);
            const auto userId = user.value("id").toString();
            if (channel.value("type").toInt() == 1) {
                primary = primaryGuild(merged(user, m_users.value(userId)));
                indicators = userIndicators(userId);
                recipientId = userId;
                const auto presence = m_userPresence.value(userId);
                status = presence.value("status").toString("unknown");
                activity = presence.value("activity").toString();
                for (const auto &states : m_voiceStates) {
                    const auto state = states.value(userId);
                    if (!state.value("channel_id").toString().isEmpty()) inVoice = true;
                    if (state.value("streaming").toBool()) streaming = true;
                }
            }
        }
        if (name.isEmpty()) name = recipients.join(", ");
        if (name.isEmpty()) name = "Direct message";
        const auto icon = channel.value("icon").toString();
        if (channel.value("type").toInt() == 3 && validId(id) && QRegularExpression("^[a-zA-Z0-9_]{1,128}$").match(icon).hasMatch())
            avatar = QString("https://cdn.discordapp.com/channel-icons/%1/%2.png?size=4096").arg(id, icon);
    }
    bool locked=false, permissionsKnown=false;
    quint64 everyonePermissions=0;
    for (const auto &entry : m_guildObjects.value(guildId).value("roles").toArray()) {
        const auto role=entry.toObject();
        if (role.value("id").toString()==guildId) { everyonePermissions=role.value("permissions").toString().toULongLong(&permissionsKnown); break; }
    }
    if (permissionsKnown) {
        for (const auto &entry : channel.value("permission_overwrites").toArray()) {
            const auto overwrite=entry.toObject();
            if (overwrite.value("id").toString()!=guildId || overwrite.value("type").toInt()!=0) continue;
            everyonePermissions=(everyonePermissions & ~overwrite.value("deny").toString().toULongLong()) | overwrite.value("allow").toString().toULongLong();
        }
        locked=!(everyonePermissions & (1ULL<<10)) || ((kind=="voice" || kind=="stage") && !(everyonePermissions & (1ULL<<20)));
    }
    const qint64 startSeconds=channel.value("voice_start_time").toInteger();
    const qint64 startedAt=startSeconds>=1420070400 && startSeconds<=QDateTime::currentSecsSinceEpoch()+300 ? startSeconds*1000 : 0;
    QVariantMap result{{"recordId", id}, {"name", name}, {"initials", initials(name)}, {"kind", kind},
            {"guildId", guildId}, {"parentId", channel.value("parent_id").toString()},
            {"topic", cleanText(channel.value("topic").toString(), 4096)},
            {"iconUrl", avatar}, {"avatarUrl", avatar}, {"status", status}, {"activity", activity},
            {"inVoice", inVoice}, {"streaming", streaming}, {"primaryGuild", primary}, {"unread", m_unread.value(id)},
            {"conversationMuted", kind == "dm" && conversationMuted(id)},
            {"pinned", kind == "dm" && m_pinnedDms.contains(id)},
            {"hidden", !guildId.isEmpty() && !channelVisible(channel, guildId)},
            {"voiceStartedAt", startedAt}, {"locked",locked}, {"recipientId",recipientId},
            {"bitrate",channel.value("bitrate").toInt()}, {"userLimit",channel.value("user_limit").toInt()}};
    for (auto it=indicators.cbegin(); it!=indicators.cend(); ++it) result.insert(it.key(),it.value());
    return result;
}

QVariantList DiscordClient::normalizedComponents(const QJsonObject &message, bool readOnly) {
    int budget=40, textBudget=16000, mediaBudget=20, optionBudget=100;
    auto text=[&](const QJsonValue &value,int maximum) {
        auto result=cleanText(value.toString(),qMin(maximum,textBudget)); textBudget-=result.size(); return result;
    };
    auto emoji=[&](const QJsonObject &raw) {
        QVariantMap result{{"name",text(raw.value("name"),64)},{"animated",raw.value("animated").toBool()}};
        if (validId(raw.value("id").toString())) result.insert("id",raw.value("id").toString());
        return result;
    };
    auto media=[&](QJsonObject raw) {
        if (mediaBudget--<=0) return QVariantMap{};
        const auto original=raw.value("url").toString();
        if (original.startsWith("attachment://")) {
            int count=0;
            for (const auto &entry : message.value("attachments").toArray()) {
                if (++count>50) break;
                if (entry.toObject().value("filename").toString()==original.mid(13)) { raw=entry.toObject(); break; }
            }
        }
        auto proxy=InlineMedia::discordMedia(raw.value("proxy_url").toString());
        const auto url=InlineMedia::discordMedia(raw.value("url").toString());
        if (proxy.isEmpty()) proxy=url;
        if (proxy.isEmpty()) return QVariantMap{};
        const auto mime=text(raw.value("content_type"),128);
        const bool video=InlineMedia::videoFile(url.isEmpty()?proxy:url,mime);
        const bool gif=InlineMedia::gifFile(url.isEmpty()?proxy:url,mime);
        return QVariantMap{{"url",url.isEmpty()?proxy:url},{"proxy_url",proxy},
            {"filename",text(raw.value("filename"),256)},{"content_type",mime},{"isVideo",video},{"isGif",gif},
            {"videoUrl",video?(url.isEmpty()?proxy:url):QString{}},{"posterUrl",QString{}},
            {"width",qBound(0,raw.value("width").toInt(),16384)},{"height",qBound(0,raw.value("height").toInt(),16384)}};
    };
    std::function<QVariantList(const QJsonArray &,int)> walk;
    walk=[&](const QJsonArray &items,int depth) {
        QVariantList result; if (depth>8) return result;
        for (const auto &entry : items) {
            if (budget--<=0) break;
            const auto raw=entry.toObject(); const int type=raw.value("type").toInt();
            if (!QSet<int>{1,2,3,9,10,11,12,13,14,17}.contains(type)) continue;
            QVariantMap item{{"type",type}};
            if (raw.contains("spoiler")) item.insert("spoiler",raw.value("spoiler").toBool());
            if (type==1 || type==9 || type==17) item.insert("components",walk(raw.value("components").toArray(),depth+1));
            if (type==9 && raw.value("accessory").isObject()) {
                const auto accessory=walk(QJsonArray{raw.value("accessory")},depth+1);
                if (!accessory.isEmpty()) item.insert("accessory",accessory.first());
            }
            if (type==17 && raw.value("accent_color").isDouble()) {
                const auto color=raw.value("accent_color").toInteger(-1);
                if (color>=0 && color<=0xffffff) item.insert("accent_color",color);
            }
            if (type==10) item.insert("content",text(raw.value("content"),4000));
            if (type==2 || type==3) {
                const auto custom=raw.value("custom_id").toString();
                if (!custom.isEmpty() && custom.size()<=100) item.insert("custom_id",custom);
                item.insert("disabled",readOnly || raw.value("disabled").toBool() || custom.isEmpty() || custom.size()>100);
            }
            if (type==2) {
                const int style=raw.value("style").toInt();
                item.insert("style",style); item.insert("label",text(raw.value("label"),80));
                if (raw.value("emoji").isObject()) item.insert("emoji",emoji(raw.value("emoji").toObject()));
                if (style==5) {
                    const auto url=safeExternalUrl(raw.value("url").toString());
                    item.insert("url",url.toString(QUrl::FullyEncoded));
                    item.insert("disabled",raw.value("disabled").toBool() || url.isEmpty());
                } else if (style<1 || style>4) item.insert("disabled",true);
            }
            if (type==3) {
                item.insert("placeholder",text(raw.value("placeholder"),150));
                item.insert("min_values",qBound(0,raw.value("min_values").toInt(1),25));
                item.insert("max_values",qBound(1,raw.value("max_values").toInt(1),25));
                QVariantList options;
                for (const auto &entry : raw.value("options").toArray()) {
                    if (options.size()>=25 || optionBudget--<=0) break;
                    const auto option=entry.toObject(); const auto value=option.value("value").toString();
                    if (value.isEmpty() || value.size()>100) continue;
                    QVariantMap row{{"label",text(option.value("label"),100)},{"value",value},
                        {"description",text(option.value("description"),100)},{"default",option.value("default").toBool()}};
                    if (option.value("emoji").isObject()) row.insert("emoji",emoji(option.value("emoji").toObject()));
                    options.append(row);
                }
                item.insert("options",options);
                if (options.isEmpty()) item.insert("disabled",true);
            }
            if (type==11 || type==13) {
                item.insert(type==13?"file":"media",media(raw.value(type==13?"file":"media").toObject()));
                item.insert("description",text(raw.value("description"),1024));
                item.insert("name",text(raw.value("name"),256));
            }
            if (type==12) {
                QVariantList gallery;
                for (const auto &entry : raw.value("items").toArray()) {
                    if (gallery.size()>=10 || mediaBudget<=0) break;
                    const auto raw=entry.toObject();
                    const auto image=media(raw.value("media").toObject());
                    if (image.isEmpty()) continue;
                    gallery.append(QVariantMap{{"media",image},{"description",text(raw.value("description"),1024)},{"spoiler",raw.value("spoiler").toBool()}});
                }
                item.insert("items",gallery);
            }
            if (type==14) { item.insert("divider",raw.value("divider").toBool(true)); item.insert("spacing",qBound(1,raw.value("spacing").toInt(1),2)); }
            result.append(item);
        }
        return result;
    };
    return walk(message.value("components").toArray(),0);
}

bool DiscordClient::executeComponent(const QString &messageId,const QString &customId,int componentType,const QVariantList &values) {
    auto fail=[&](const QString &reason) { emit componentError(messageId,reason); return false; };
    if (!m_connected || m_demoMode || m_token.isEmpty() || m_sessionId.isEmpty()) return fail("Reconnect to Discord to use this control.");
    if (!validId(messageId) || m_messages.indexOf(messageId)<0 || customId.isEmpty() || customId.size()>100 || (componentType!=2 && componentType!=3))
        return fail("This control is no longer available. Refresh the conversation.");
    const auto message=m_messageObjects.value(messageId);
    if (message.value("channel_id").toString()!=m_currentChannelId) return fail("Open this message's conversation first.");
    QJsonObject found; int budget=40;
    std::function<void(const QJsonArray &,int)> walk=[&](const QJsonArray &items,int depth) {
        if (depth>8) return;
        for (const auto &entry : items) {
            if (budget--<=0 || !found.isEmpty()) break;
            const auto item=entry.toObject();
            if (item.value("type").toInt()==componentType && item.value("custom_id").toString()==customId) { found=item; break; }
            walk(item.value("components").toArray(),depth+1);
            if (item.value("accessory").isObject()) walk(QJsonArray{item.value("accessory")},depth+1);
        }
    };
    walk(message.value("components").toArray(),0);
    if (found.isEmpty() || found.value("disabled").toBool()) return fail("This control is disabled or has changed.");
    if (componentType==2 && (found.value("style").toInt()<1 || found.value("style").toInt()>4 || !values.isEmpty()))
        return fail("This button cannot be used as an app action.");
    QJsonObject data{{"component_type",componentType},{"custom_id",customId}};
    if (componentType==3) {
        if (values.size()>25 || values.size()<found.value("min_values").toInt(1) || values.size()>found.value("max_values").toInt(1))
            return fail("Choose the required number of options.");
        QSet<QString> allowed,seen; for (const auto &entry : found.value("options").toArray()) {
            if (allowed.size()>=25) break;
            const auto value=entry.toObject().value("value").toString();
            if (!value.isEmpty() && value.size()<=100) allowed.insert(value);
        }
        QJsonArray selected;
        for (const auto &value : values) {
            if (value.metaType().id()!=QMetaType::QString || !allowed.contains(value.toString()) || seen.contains(value.toString())) return fail("One of the selected options is no longer available.");
            seen.insert(value.toString()); selected.append(value.toString());
        }
        data.insert("values",selected);
    }
    const auto app=message.value("application_id").toString(message.value("author").toObject().value("id").toString());
    if (!validId(app)) return fail("This message is missing its app identity.");
    const auto pending=messageId+":"+customId;
    if (m_componentsPending.contains(pending)) return false;
    if (m_componentsPending.size()>=8) return fail("Wait for the previous app actions to finish.");
    QJsonObject body{{"type",3},{"application_id",app},{"channel_id",m_currentChannelId},
        {"message_id",messageId},{"message_flags",message.value("flags").toInteger()},{"session_id",m_sessionId},
        {"nonce",QString::number((quint64(qMax(qint64(0),nowMs()-1420070400000LL))<<22) | QRandomGenerator::global()->bounded(1U<<22))},{"data",data}};
    if (!m_currentGuildId.isEmpty()) body.insert("guild_id",m_currentGuildId);
    m_componentsPending.insert(pending);
    request("POST","/interactions",body,[this,messageId,customId,pending](const QJsonValue &,const QString &error) {
        m_componentsPending.remove(pending);
        if (!error.isEmpty()) emit componentError(messageId,error);
        else emit componentFinished(messageId,customId);
    });
    return true;
}

QVariantMap DiscordClient::memberPresentation(const QString &guildId, const QJsonObject &user, const QJsonObject &member) const {
    const auto userId = user.value("id").toString();
    auto name = guildId.isEmpty() ? QString{} : cleanText(member.value("nick").toString(), 128);
    if (name.isEmpty()) name = displayName(user);
    auto avatar = avatarUrl(user);
    const auto hash = member.value("avatar").toString();
    if (validId(guildId) && validId(userId) && QRegularExpression("^[a-zA-Z0-9_]{1,128}$").match(hash).hasMatch())
        avatar = QString("https://cdn.discordapp.com/guilds/%1/users/%2/avatars/%3.png?size=4096").arg(guildId, userId, hash);
    QJsonObject highest, colored, hoisted;
    QList<QJsonObject> assigned;
    auto higher = [](const QJsonObject &a, const QJsonObject &b) {
        if (b.isEmpty()) return true;
        const int ap = a.value("position").toInt(), bp = b.value("position").toInt();
        return ap > bp || (ap == bp && a.value("id").toString().toULongLong() < b.value("id").toString().toULongLong());
    };
    if (!guildId.isEmpty()) for (const auto &entry : m_guildObjects.value(guildId).value("roles").toArray()) {
        const auto role = entry.toObject();
        if (!member.value("roles").toArray().contains(role.value("id"))) continue;
        assigned.append(role);
        if (higher(role, highest)) highest = role;
        if (role.value("hoist").toBool() && higher(role, hoisted)) hoisted = role;
        const int color = role.value("colors").toObject().value("primary_color").toInt(role.value("color").toInt());
        if (color > 0 && color <= 0xffffff && higher(role, colored)) colored = role;
    }
    std::sort(assigned.begin(), assigned.end(), higher);
    QStringList roleNames;
    for (const auto &role : assigned) { if (roleNames.size() >= 100) break; roleNames.append(cleanText(role.value("name").toString(), 100)); }
    const int color = colored.value("colors").toObject().value("primary_color").toInt(colored.value("color").toInt());
    const bool bot = user.value("bot").toBool();
    return {{"recordId", userId}, {"guildId", guildId}, {"name", name}, {"initials", initials(name)}, {"avatarUrl", avatar},
        {"roleName", cleanText(highest.value("name").toString(), 100)}, {"rolePosition", highest.value("position").toInt(-1)},
        {"hoistedRoleId",hoisted.value("id").toString()}, {"hoistedRoleName",cleanText(hoisted.value("name").toString(),100)}, {"hoistedRolePosition",hoisted.value("position").toInt(-1)},
        {"roleColor", color > 0 ? QString("#%1").arg(color, 6, 16, QChar('0')) : QString{}}, {"roleNames", roleNames},
        {"bot", bot}, {"verifiedBot", bot && bool(user.value("public_flags").toInteger() & (1LL << 16))}, {"primaryGuild", primaryGuild(user)}};
}

QVariantMap DiscordClient::messageRow(const QJsonObject &message) const {
    const int type = message.value("type").toInt();
    const auto reference = message.value("referenced_message");
    // Thread starters carry their body exclusively in referenced_message.
    const QJsonObject source = type == 21 && reference.isObject() ? reference.toObject() : message;
    const auto author = source.value("author").isObject() ? source.value("author").toObject() : message.value("author").toObject();
    const auto user = merged(m_users.value(author.value("id").toString()), author);
    QString guildId = message.value("guild_id").toString();
    const auto messageChannel = message.value("channel_id").toString();
    if (!message.contains("guild_id") && !m_dmObjects.contains(messageChannel)) {
        for (auto it = m_channelCache.cbegin(); it != m_channelCache.cend() && guildId.isEmpty(); ++it)
            for (const auto &entry : it.value()) if (entry.toObject().value("id").toString() == messageChannel) { guildId = it.key(); break; }
        if (guildId.isEmpty() && messageChannel == m_currentChannelId) guildId = m_currentGuildId;
    }
    const auto member = guildId.isEmpty() ? QJsonObject{} : merged(source.value("member").toObject(), m_guildMembers.value(guildId).value(user.value("id").toString()));
    const auto presentation = memberPresentation(guildId, user, member);
    const auto name = presentation.value("name").toString();
    QHash<QString, QString> mentionedUsers;
    QHash<QString, QJsonObject> mentionedObjects;
    auto collectMentions = [&](const QJsonObject &payload) {
        int count = 0;
        for (const auto &value : payload.value("mentions").toArray()) {
            if (++count > 100) break;
            const auto mentioned = value.toObject();
            const auto id = mentioned.value("id").toString();
            if (!validId(id)) continue;
            const auto memberNick = mentioned.value("member").toObject().value("nick").toString();
            mentionedUsers.insert(id, memberNick.isEmpty() ? displayName(mentioned) : cleanText(memberNick, 128));
            mentionedObjects.insert(id, mentioned);
        }
    };
    collectMentions(message);
    collectMentions(source);
    collectMentions(reference.toObject());
    const bool forwarded = message.value("message_reference").toObject().value("type").toInt() == 1;
    const auto snapshots = message.value("message_snapshots").toArray();
    if (forwarded && !snapshots.isEmpty()) collectMentions(snapshots.first().toObject().value("message").toObject());
    auto plainRichText = [&](QString text, int limit) {
        text = cleanText(text, limit);
        for (auto it = mentionedUsers.cbegin(); it != mentionedUsers.cend(); ++it) {
            text.replace("<@" + it.key() + ">", "@" + it.value());
            text.replace("<@!" + it.key() + ">", "@" + it.value());
        }
        static const QRegularExpression customEmoji("<a?:([a-zA-Z0-9_]{1,64}):[0-9]{1,22}>");
        text.replace(customEmoji, ":\\1:");
        return text.left(limit);
    };
    QVariantList attachments, embeds, stickers;
    QString content;
    auto appendText = [&](const QString &text) {
        if (text.isEmpty() || content.size() >= 8000) return;
        if (!content.isEmpty()) content += '\n';
        content += plainRichText(text, 8000 - content.size());
    };
    auto externalLink = [](const QString &raw) {
        const QUrl url(raw.left(4096));
        if (!url.isValid() || url.scheme() != "https" || url.host().isEmpty() ||
            !url.userInfo().isEmpty() || (url.port() != -1 && url.port() != 443)) return QString{};
        return url.toString(QUrl::FullyEncoded);
    };
    auto mediaUrl = [](const QJsonObject &media) {
        auto allowed = [](const QString &raw) {
            const auto cdn = safeCdnUrl(raw);
            if (!cdn.isEmpty()) return cdn;
            const QUrl url(raw);
            const auto host = url.host().toLower();
            if (!url.isValid() || url.scheme() != "https" || !url.userInfo().isEmpty() ||
                (url.port() != -1 && url.port() != 443) ||
                (host != "images-ext-1.discordapp.net" && host != "images-ext-2.discordapp.net")) return QString{};
            return url.toString(QUrl::FullyEncoded);
        };
        auto url = allowed(media.value("proxy_url").toString());
        if (url.isEmpty()) url = allowed(media.value("url").toString());
        return url;
    };
    auto appendAttachment = [&](const QJsonObject &attachment, bool spoiler = false) {
        const auto url = mediaUrl(attachment);
        if (url.isEmpty()) return;
        const auto directUrl = safeCdnUrl(attachment.value("url").toString());
        const auto filename = cleanText(attachment.value("filename").toString(
            QUrl(url).fileName()), 256);
        const auto mime = cleanText(attachment.value("content_type").toString(), 128).toLower();
        const auto suffix = filename.section('.', -1).toLower();
        const bool isImage = mime.startsWith("image/") || (mime.isEmpty() &&
            QStringList{"png", "jpg", "jpeg", "gif", "webp", "avif", "apng"}.contains(suffix));
        const bool isVideo = mime.startsWith("video/") || (mime.isEmpty() &&
            QStringList{"mp4", "webm", "mov", "mkv"}.contains(suffix));
        const bool hidden = spoiler || filename.startsWith("SPOILER_");
        const auto openUrl = directUrl.isEmpty() ? url : directUrl;
        // A component may refer to an attachment already included in the payload.
        for (auto &existing : attachments) {
            auto row = existing.toMap();
            if (row.value("url").toString() != openUrl) continue;
            if (hidden) { row.insert("spoiler", true); existing = row; }
            return;
        }
        if (attachments.size() >= 10) return;
        attachments.append(QVariantMap{{"filename", filename.isEmpty() ? QStringLiteral("Attachment") : filename},
            {"url", openUrl}, {"size", std::max(0.0, attachment.value("size").toDouble())},
            {"contentType", mime}, {"previewUrl", isImage ? url : QString{}},
            {"width", std::clamp(attachment.value("width").toInt(), 0, 16384)},
            {"height", std::clamp(attachment.value("height").toInt(), 0, 16384)},
            {"isImage", isImage}, {"isVideo", isVideo}, {"isGif", InlineMedia::gifFile(openUrl,mime)},
            {"videoUrl",isVideo?openUrl:QString{}},{"posterUrl",QString{}},{"spoiler", hidden}});
    };
    QVariantList richComponents;
    int embedTextBudget = 6000;
    auto embedText = [&](const QJsonValue &value, int limit) {
        const auto text = plainRichText(value.toString(), std::min(limit, embedTextBudget));
        embedTextBudget -= text.size();
        return text;
    };
    auto appendPayload = [&](const QJsonObject &payload) {
        appendText(payload.value("content").toString());
        const auto rawAttachments = payload.value("attachments").toArray();
        auto resolveMedia = [&](QJsonObject media) {
            const auto rawUrl = media.value("url").toString();
            if (rawUrl.startsWith("attachment://")) {
                for (const auto &value : rawAttachments) {
                    const auto candidate = value.toObject();
                    if (candidate.value("filename").toString() == rawUrl.mid(13)) return candidate;
                }
            }
            return media;
        };
        // Components v2 explicitly choose which attachments are visible.
        const bool componentsV2 = (payload.value("flags").toInteger() & (1 << 15)) != 0;
        if (!componentsV2) {
            for (const auto &value : rawAttachments) {
                appendAttachment(value.toObject());
                if (attachments.size() >= 10) break;
            }
        }
        const auto components=normalizedComponents(payload,forwarded || payload.value("id")!=message.value("id"));
        for (const auto &item : components) { if (richComponents.size()>=40) break; richComponents.append(item); }
        const bool suppressed=(payload.value("flags").toInteger() & 4)!=0;
        const auto payloadEmbeds=suppressed?QJsonArray{}:payload.value("embeds").toArray();
        for (const auto &value : payloadEmbeds) {
            if (embeds.size() >= 10) break;
            const auto embed = value.toObject();
            // Legacy embed components are display-only because they do not carry
            // independently actionable controls in the cached message payload.
            const auto nested=normalizedComponents(embed,true);
            for (const auto &item : nested) { if (richComponents.size()>=40) break; richComponents.append(item); }
            QVariantList fields;
            for (const auto &fieldValue : embed.value("fields").toArray()) {
                const auto field = fieldValue.toObject();
                fields.append(QVariantMap{{"name", embedText(field.value("name"), 256)},
                                          {"value", embedText(field.value("value"), 1024)}, {"inline",field.value("inline").toBool()}});
                if (fields.size() >= 25 || embedTextBudget == 0) break;
            }
            QVariantMap row{{"title", embedText(embed.value("title"), 256)},
                {"rich",embed.value("type").toString()=="rich"},
                {"description", embedText(embed.value("description"), 4096)},
                {"url", externalLink(embed.value("url").toString())},
                {"provider", embedText(embed.value("provider").toObject().value("name"), 256)},
                {"author", embedText(embed.value("author").toObject().value("name"), 256)},
                {"authorUrl",externalLink(embed.value("author").toObject().value("url").toString())},
                {"authorIconUrl",mediaUrl(QJsonObject{{"url",embed.value("author").toObject().value("icon_url")},{"proxy_url",embed.value("author").toObject().value("proxy_icon_url")}})},
                {"footerIconUrl",mediaUrl(QJsonObject{{"url",embed.value("footer").toObject().value("icon_url")},{"proxy_url",embed.value("footer").toObject().value("proxy_icon_url")}})},
                {"imageUrl", mediaUrl(resolveMedia(embed.value("image").toObject()))},
                {"thumbnailUrl", mediaUrl(resolveMedia(embed.value("thumbnail").toObject()))},
                {"imageWidth", std::clamp(embed.value("image").toObject().value("width").toInt(), 0, 100000)},
                {"imageHeight", std::clamp(embed.value("image").toObject().value("height").toInt(), 0, 100000)},
                {"thumbnailWidth", std::clamp(embed.value("thumbnail").toObject().value("width").toInt(), 0, 100000)},
                {"thumbnailHeight", std::clamp(embed.value("thumbnail").toObject().value("height").toInt(), 0, 100000)},
                {"fields", fields}, {"footer", embedText(embed.value("footer").toObject().value("text"), 2048)}};
            const auto color=embed.value("color").toInteger(-1);
            row.insert("accentColor",color>=0 && color<=0xffffff?QString("#%1").arg(color,6,16,QChar('0')):QString{});
            const auto rawTimestamp=embed.value("timestamp").toString().left(64);
            auto timestamp=QDateTime::fromString(rawTimestamp,Qt::ISODateWithMs);
            if(!timestamp.isValid())timestamp=QDateTime::fromString(rawTimestamp,Qt::ISODate);
            row.insert("timestamp",timestamp.isValid()?timestamp.toUTC().toString(Qt::ISODateWithMs):QString{});
            const auto video=resolveMedia(embed.value("video").toObject());
            auto videoUrl=mediaUrl(video);
            if(videoUrl.isEmpty())videoUrl=InlineMedia::directMedia(video.value("url").toString());
            auto pageUrl=InlineMedia::youtubePage(embed.value("url").toString());
            if(pageUrl.isEmpty())pageUrl=InlineMedia::youtubePage(video.value("url").toString());
            const auto linkedPage=InlineMedia::youtubePage(payload.value("content").toString().trimmed());
            if(!linkedPage.isEmpty() && InlineMedia::youtubeId(linkedPage)==InlineMedia::youtubeId(pageUrl)) {
                // Discord may canonicalize away the sender's explicit start time.
                pageUrl=linkedPage;row.insert("url",linkedPage);
            }
            const bool gif=embed.value("type").toString()=="gifv" || InlineMedia::gifFile(row.value("imageUrl").toString());
            const QString kind=!videoUrl.isEmpty() || !pageUrl.isEmpty()?(gif?"gifv":"video"):!row.value("imageUrl").toString().isEmpty()?"image":"link";
            row.insert("kind",kind);row.insert("gif",gif);row.insert("videoUrl",videoUrl);
            row.insert("pageUrl",pageUrl);row.insert("providerName",row.value("provider"));
            row.insert("posterUrl",row.value("thumbnailUrl").toString().isEmpty()?row.value("imageUrl"):row.value("thumbnailUrl"));
            row.insert("videoWidth",std::clamp(video.value("width").toInt(),0,16384));
            row.insert("videoHeight",std::clamp(video.value("height").toInt(),0,16384));
            row.insert("startSeconds",InlineMedia::startSeconds(pageUrl));
            // Video-only embeds remain useful before the user presses Play.
            if (row.value("title").toString().isEmpty() && row.value("description").toString().isEmpty() &&
                row.value("imageUrl").toString().isEmpty() && row.value("thumbnailUrl").toString().isEmpty() &&
                row.value("author").toString().isEmpty() && row.value("provider").toString().isEmpty() &&
                row.value("footer").toString().isEmpty() && fields.isEmpty()) {
                if (embed.value("video").isObject()) row.insert("title", "Video");
                else if (!row.value("url").toString().isEmpty()) row.insert("title", "Link preview");
                else continue; // Components have already contributed their own visible content.
            }
            embeds.append(row);
        }
        if(!suppressed && !componentsV2) {
            const auto raw=payload.value("content").toString().trimmed();
            const auto youtube=InlineMedia::youtubePage(raw);
            const auto direct=InlineMedia::discordMedia(raw);
            if(!youtube.isEmpty() || (!direct.isEmpty() && (InlineMedia::videoFile(direct) || InlineMedia::gifFile(direct)))) {
                bool exists=false;for(const auto &item:embeds) {
                    const auto row=item.toMap();
                    if(row.value("url").toString()==raw || (!youtube.isEmpty() && InlineMedia::youtubeId(row.value("pageUrl").toString())==InlineMedia::youtubeId(youtube)) || (!direct.isEmpty() && (row.value("videoUrl").toString()==direct || row.value("imageUrl").toString()==direct))) {exists=true;break;}
                }
                if(!exists && embeds.size()<10) {
                    const bool gif=InlineMedia::gifFile(direct);const bool video=!youtube.isEmpty() || InlineMedia::videoFile(direct);
                    embeds.append(QVariantMap{{"kind",video?"video":"image"},{"title",!youtube.isEmpty()?"YouTube video":video?"Video":"GIF"},
                        {"url",raw},{"pageUrl",youtube},{"videoUrl",video?direct:QString{}},{"imageUrl",gif?direct:QString{}},
                        {"provider",youtube.isEmpty()?QString{}:QString("YouTube")},{"providerName",youtube.isEmpty()?QString{}:QString("YouTube")},
                        {"gif",gif},{"videoWidth",0},{"videoHeight",0},{"posterUrl",QString{}},{"startSeconds",InlineMedia::startSeconds(youtube)}});
                }
            }
        }
        auto rawStickers = payload.value("sticker_items").toArray();
        if (rawStickers.isEmpty()) rawStickers = payload.value("stickers").toArray();
        for (const auto &value : rawStickers) {
            if (stickers.size() >= 3) break;
            const auto sticker = value.toObject();
            const auto id = sticker.value("id").toString();
            const int format = sticker.value("format_type").toInt();
            QString url;
            if (validId(id) && (format == 1 || format == 2))
                url = QStringLiteral("https://cdn.discordapp.com/stickers/%1.png").arg(id);
            else if (validId(id) && format == 4)
                url = QStringLiteral("https://media.discordapp.net/stickers/%1.gif").arg(id);
            stickers.append(QVariantMap{{"stickerId", id},
                {"name", cleanText(sticker.value("name").toString("Sticker"), 128)},
                {"url", url}, {"format", format}, {"animated", format == 2 || format == 3 || format == 4}});
        }
        const auto poll = payload.value("poll").toObject();
        if (!poll.isEmpty()) {
            appendText("Poll: " + cleanText(poll.value("question").toObject().value("text").toString(), 300));
            const auto answers = poll.value("answers").toArray();
            for (int i = 0; i < std::min(10, int(answers.size())); ++i)
                appendText("• " + cleanText(answers.at(i).toObject().value("poll_media").toObject().value("text").toString(), 300));
        }
    };
    appendPayload(source);
    if (forwarded && !snapshots.isEmpty()) appendPayload(snapshots.first().toObject().value("message").toObject());

    const bool systemMessage = type != 0 && type != 19 && type != 20 && type != 21 && type != 23;
    const auto detail = plainRichText(message.value("content").toString(), 1024);
    QStringList mentionedNames;
    for (const auto &value : message.value("mentions").toArray()) {
        mentionedNames.append(displayName(value.toObject()));
        if (mentionedNames.size() >= 10) break;
    }
    const auto recipients = mentionedNames.isEmpty() ? QStringLiteral("a member") : mentionedNames.join(", ");
    switch (type) {
    case 1: content = name + " added " + recipients + " to the group."; break;
    case 2:
        content = message.value("mentions").toArray().isEmpty() ||
                  message.value("mentions").toArray().first().toObject().value("id") == user.value("id")
            ? name + " left the group." : name + " removed " + recipients + " from the group.";
        break;
    case 3: content = name + (message.value("call").toObject().value("ended_timestamp").toString().isEmpty()
            ? " started a call." : " started a call that has ended."); break;
    case 4: content = name + " changed the channel name" + (detail.isEmpty() ? "." : " to “" + detail + "”."); break;
    case 5: content = name + " changed the channel icon."; break;
    case 6: content = name + " pinned a message to this channel."; break;
    case 7: content = name + " joined the server."; break;
    case 8: content = name + " boosted the server."; break;
    case 9: case 10: case 11:
        content = name + " boosted the server. It reached Boost Level " + QString::number(type - 8) + "."; break;
    case 12: content = "This channel now follows " + (detail.isEmpty() ? QStringLiteral("an announcement channel.") : detail + "."); break;
    case 14: content = "This server is no longer eligible for Server Discovery."; break;
    case 15: content = "This server is eligible for Server Discovery again."; break;
    case 16: case 17: content = "This server needs to meet the Server Discovery requirements to remain listed."; break;
    case 18: content = name + " started a thread" + (detail.isEmpty() ? "." : ": " + detail); break;
    case 22: content = "Invite friends to this server."; break;
    case 24: if (content.isEmpty()) content = "AutoMod took an action on a message."; break;
    case 25: {
        const auto subscription = message.value("role_subscription_data").toObject();
        const auto tier = cleanText(subscription.value("tier_name").toString(), 128);
        content = name + (subscription.value("is_renewal").toBool() ? " renewed a subscription" : " subscribed") +
            (tier.isEmpty() ? " to this server." : " to “" + tier + "”.");
        break;
    }
    case 26: if (content.isEmpty()) content = "This interaction has a premium option."; break;
    case 27: content = name + " started the stage" + (detail.isEmpty() ? "." : ": " + detail); break;
    case 28: content = name + " ended the stage."; break;
    case 29: content = name + " is now a stage speaker."; break;
    case 31: content = name + " changed the stage topic" + (detail.isEmpty() ? "." : ": " + detail); break;
    case 32: content = name + " subscribed to an app in this server."; break;
    case 36: content = "Server security actions were enabled."; break;
    case 37: content = "Server security actions were disabled."; break;
    case 38: content = "A raid was reported to Discord."; break;
    case 39: content = "A raid report was marked as a false alarm."; break;
    case 44: if (content.isEmpty()) content = "A purchase was completed."; break;
    case 46: if (content.isEmpty()) content = "Poll results"; break;
    default: break;
    }
    QString replyAuthor, replyContent, replyId, replyChannelId;
    bool replyMissing = false, replyDeleted = false;
    if ((type == 19 || type == 21 || type == 23) && !forwarded) {
        const auto messageReference = message.value("message_reference").toObject();
        replyId = messageReference.value("message_id").toString();
        replyChannelId = messageReference.value("channel_id").toString();
        if (!validId(replyChannelId)) replyChannelId = message.value("channel_id").toString();
        if (reference.isObject()) {
            const auto reply = reference.toObject();
            if (!validId(replyId)) replyId = reply.value("id").toString();
            replyAuthor = displayName(reply.value("author").toObject());
            replyContent = plainRichText(reply.value("content").toString(), 240);
            if (replyContent.isEmpty()) {
                if (!reply.value("attachments").toArray().isEmpty()) replyContent = "Attachment";
                else if (!reply.value("sticker_items").toArray().isEmpty()) replyContent = "Sticker";
                else if (!reply.value("embeds").toArray().isEmpty()) {
                    const auto embed = reply.value("embeds").toArray().first().toObject();
                    replyContent = plainRichText(embed.value("title").toString(), 240);
                    if (replyContent.isEmpty()) replyContent = "Link or media preview";
                } else if (!reply.value("components").toArray().isEmpty()) replyContent = "Rich message";
                else replyContent = "Message";
            }
        } else if (!replyId.isEmpty()) {
            replyMissing = true;
            replyDeleted = reference.isNull();
            replyContent = replyDeleted ? "Original message was deleted." : "Original message is unavailable.";
        }
        if (replyChannelId == m_currentChannelId && m_deletedMessages.contains(replyId)) {
            replyDeleted = true;
            replyMissing = true;
            replyContent = "Original message was deleted.";
        }
    }
    if (content.isEmpty() && attachments.isEmpty() && embeds.isEmpty() && stickers.isEmpty() && richComponents.isEmpty()) {
        if (type == 21) content = reference.isNull() ? "The original message was deleted." : "The original message is unavailable.";
        else if (forwarded) content = "The forwarded message is unavailable.";
        else if (message.value("flags").toInteger() & (1 << 7)) content = "Waiting for the app’s response…";
        else if (systemMessage) content = QStringLiteral("Discord event (type %1).").arg(type);
        else content = "This message has no displayable content.";
    }
    if (guildId.isEmpty() && m_dmObjects.value(messageChannel).value("type").toInt() == 1) {
        const auto setup = RelayDiscordBridge::setupSummary(source.value("content").toString());
        if (!setup.isEmpty()) content = setup;
    }
    const bool bodyOnly = content == plainRichText(source.value("content").toString(), 8000);
    QVariantList mentionUsers, mentionRoles;
    for (auto it = mentionedUsers.cbegin(); it != mentionedUsers.cend(); ++it)
        mentionUsers.append(QVariantMap{{"id", it.key()}, {"name", it.value()},
            {"avatarUrl", avatarUrl(mentionedObjects.value(it.key()))}, {"bot", mentionedObjects.value(it.key()).value("bot").toBool()}});
    const auto guildRoles = m_guildObjects.value(guildId).value("roles").toArray();
    const auto selfRoles = m_guildObjects.value(guildId).value("member").toObject().value("roles").toArray();
    bool currentUserMentioned = false;
    for (const auto &entry : message.value("mentions").toArray())
        if (entry.toObject().value("id").toString() == m_currentUserId) currentUserMentioned = true;
    int roleCount = 0;
    for (const auto &entry : source.value("mention_roles").toArray()) {
        if (++roleCount > 100) break;
        const auto id = entry.toString();
        if (!validId(id)) continue;
        QString roleName = "role", color;
        for (const auto &value : guildRoles) {
            const auto role = value.toObject();
            if (role.value("id").toString() != id) continue;
            roleName = cleanText(role.value("name").toString(), 100);
            const int number = role.value("colors").toObject().value("primary_color").toInt(role.value("color").toInt());
            if (number > 0 && number <= 0xffffff) color = QString("#%1").arg(number, 6, 16, QChar('0'));
            break;
        }
        mentionRoles.append(QVariantMap{{"id", id}, {"name", roleName}, {"color", color}});
        content.replace("<@&" + id + ">", "@" + roleName);
    }
    for (const auto &entry : message.value("mention_roles").toArray())
        if (selfRoles.contains(entry)) currentUserMentioned = true;
    currentUserMentioned = currentUserMentioned || message.value("mention_everyone").toBool();
    // Preserve source tokens separately so the UI can distinguish real mentions
    // from ordinary text with the same spelling. Plain content remains available.
    const auto rawContent = !systemMessage && !forwarded && bodyOnly ? cleanText(source.value("content").toString(), 8000) : QString{};
    QVariantList reactions;
    for (const auto &entry : message.value("reactions").toArray()) {
        const auto reaction = entry.toObject(), emoji = reaction.value("emoji").toObject();
        const auto id = emoji.value("id").toString(), name = emoji.value("name").toString();
        const int count = reaction.value("count_details").isObject() ? reaction.value("count_details").toObject().value("normal").toInt() : reaction.value("count").toInt();
        if (name.isEmpty() || count <= 0) continue;
        reactions.append(QVariantMap{{"emoji", id.isEmpty() ? name : name + ":" + id},
            {"label", name}, {"customId", id}, {"animated", emoji.value("animated").toBool()},
            {"count", count}, {"me", reaction.value("me").toBool()}});
    }
    return {{"recordId", message.value("id").toString()}, {"kind", "message"},
            {"channelId", message.value("channel_id").toString()}, {"guildId", guildId},
            {"pinned", message.value("pinned").toBool()}, {"bot", presentation.value("bot")}, {"verifiedBot", presentation.value("verifiedBot")}, {"roleColor", presentation.value("roleColor")}, {"roleName", presentation.value("roleName")}, {"components", richComponents},
            {"authorId", user.value("id").toString()}, {"author", name},
            {"initials", initials(name)}, {"avatarUrl", presentation.value("avatarUrl")}, {"content", content},
            {"rawContent", rawContent}, {"mentionUsers", mentionUsers}, {"mentionRoles", mentionRoles},
            {"primaryGuild", primaryGuild(user)},
            {"mentionEveryone", source.value("mention_everyone").toBool()}, {"currentUserMentioned", currentUserMentioned},
            {"timestamp", message.value("timestamp").toString()},
            {"edited", !source.value("edited_timestamp").toString().isEmpty()},
            {"own", user.value("id").toString() == m_currentUserId},
            {"reactions", reactions}, {"attachments", attachments}, {"embeds", embeds}, {"stickers", stickers},
            {"replyAuthor", replyAuthor}, {"replyContent", replyContent}, {"replyId", replyId},
            {"replyMissing", replyMissing}, {"replyChannelId", replyChannelId}, {"replyDeleted", replyDeleted}, {"forwarded", forwarded},
            {"systemMessage", systemMessage}, {"messageType", type}, {"pending", false}, {"failed", false}};
}

void DiscordClient::setError(const QString &error) {
    auto safeError = cleanText(error, 600);
    if (!m_token.isEmpty()) safeError.replace(QString::fromUtf8(m_token), "[redacted]");
    m_error = safeError;
    emit stateChanged();
}
void DiscordClient::clearError() { m_error.clear(); emit stateChanged(); }
void DiscordClient::clearReadReceiptError() { if (!m_readReceiptError.isEmpty()) { m_readReceiptError.clear(); emit stateChanged(); } }

QString DiscordClient::routeKey(const RestRequest &job) const {
    // Query strings must share a route bucket (pagination is the same endpoint).
    return QString::fromUtf8(job.method) + ' ' + job.path.section('?', 0, 0);
}

void DiscordClient::request(const QByteArray &method, const QString &path,
                            const QJsonObject &body, ReplyHandler callback, const QString &auditReason, int expectedStatus,
                            std::function<bool()> preflight) {
    if (m_token.isEmpty() || m_demoMode) return;
    if (!path.startsWith('/') || path.startsWith("//") || path.contains("..")) {
        callback({}, "Invalid API path.");
        return;
    }
    if (m_queue.size() >= 128) { callback({}, "Too many pending requests. Please wait."); return; }
    auto job = std::make_shared<RestRequest>();
    job->method = method;
    job->path = path;
    job->body = body;
    job->auditReason = QUrl::toPercentEncoding(cleanText(auditReason,512));
    job->callback = std::move(callback);
    job->generation = m_generation;
    job->expectedStatus = expectedStatus;
    job->preflight = std::move(preflight);
    m_queue.enqueue(job);
    pumpRequests();
}

void DiscordClient::pumpRequests() {
    if (m_queue.isEmpty() || m_replies.size() >= 3 || m_token.isEmpty()) return;
    const qint64 now = nowMs();
    if (m_rateReset.size() > 512) {
        for (auto it = m_rateReset.begin(); it != m_rateReset.end();) {
            if (it.value() <= now) it = m_rateReset.erase(it);
            else ++it;
        }
    }
    qint64 earliest = now + 60000;
    const int count = m_queue.size();
    for (int i = 0; i < count && m_replies.size() < 3; ++i) {
        const auto job = m_queue.dequeue();
        if (job->generation != m_generation) continue;
        const auto due = std::max({job->dueAt, m_globalReset, m_rateReset.value(routeKey(*job))});
        if (due > now) {
            earliest = std::min(earliest, due);
            m_queue.enqueue(job);
        } else startRequest(job);
    }
    if (!m_queue.isEmpty() && m_replies.size() < 3)
        m_requestPump.start(static_cast<int>(std::clamp(earliest - now, qint64(1), qint64(60000))));
}

QString DiscordClient::describeError(int status, const QJsonObject &body, bool networkError) const {
    if (status == 401) return "Discord rejected this session. Sign in again using a new QR code.";
    if (status == 403) return "Discord denied access. This account may not have permission for this channel or action.";
    if (status == 404) return "This channel or message is no longer available.";
    if (status == 429) return "Discord asked Relay to slow down. Please wait before trying again.";
    if (body.contains("captcha_key") || body.contains("captcha_sitekey"))
        return "Discord requires an account challenge. Complete it in the official client, then reconnect.";
    if (networkError && status == 0)
        return "The request could not be confirmed. Check your connection; messages are never automatically resent.";
    const auto detail = cleanText(body.value("message").toString(), 200);
    return QStringLiteral("Discord request failed (%1). %2").arg(status).arg(detail);
}

void DiscordClient::startRequest(const std::shared_ptr<RestRequest> &job) {
    if (job->preflight && !job->preflight()) {
        job->callback({}, QStringLiteral("Read receipt no longer current."));
        return;
    }
    QNetworkRequest request(QUrl(m_apiBase + job->path));
    request.setRawHeader("Authorization", m_token);
    request.setRawHeader("User-Agent", "Relay/0.1 (native Qt client)");
    if (!job->auditReason.isEmpty()) request.setRawHeader("X-Audit-Log-Reason", job->auditReason);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // No redirects: an authenticated request must never send a token to another host.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(20000);
    QNetworkReply *reply = job->method == "GET" ? m_network.get(request) :
        m_network.sendCustomRequest(request, job->method, QJsonDocument(job->body).toJson(QJsonDocument::Compact));
    m_replies.insert(reply);
    reply->setReadBufferSize(MaximumResponse + 1);
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
        if (received > MaximumResponse || total > MaximumResponse) {
            reply->setProperty("responseTooLarge", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, job] {
        m_replies.remove(reply);
        reply->deleteLater();
        if (job->generation != m_generation) { pumpRequests(); return; }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto raw = reply->read(MaximumResponse + 1);
        if (raw.size() > MaximumResponse || reply->property("responseTooLarge").toBool()) {
            job->callback({}, "Discord returned a response too large to load safely.");
            pumpRequests();
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(raw, &parseError);
        const auto object = document.object();
        const auto key = routeKey(*job);
        if (reply->hasRawHeader("X-RateLimit-Remaining") &&
            reply->rawHeader("X-RateLimit-Remaining").toInt() == 0) {
            const double seconds = reply->rawHeader("X-RateLimit-Reset-After").toDouble();
            if (std::isfinite(seconds) && seconds > 0)
                m_rateReset.insert(key, nowMs() + static_cast<qint64>(std::min(seconds, 86400.0) * 1000) + 100);
        }
        if (status == 429) {
            double seconds = object.value("retry_after").toDouble();
            if (seconds <= 0) seconds = reply->rawHeader("Retry-After").toDouble();
            if (!std::isfinite(seconds) || seconds <= 0) seconds = 2;
            const auto reset = nowMs() + static_cast<qint64>(std::clamp(seconds, 0.1, 86400.0) * 1000) + 100;
            m_rateReset.insert(key, reset);
            if (object.value("global").toBool()) m_globalReset = reset;
            // Only idempotent reads retry automatically. A POST may have succeeded even
            // when its response was lost, so the user's draft stays for explicit recovery.
            if (job->method == "GET" && job->attempts < 3) {
                ++job->attempts;
                job->dueAt = reset;
                m_queue.enqueue(job);
                pumpRequests();
                return;
            }
        }
        if (status == 401 && !m_token.isEmpty() && reply->request().rawHeader("Authorization") != m_token) {
            // Account/security changes can rotate credentials while ordinary
            // reads are in flight. An old rejection cannot revoke the new session.
            if (job->method == "GET" && job->attempts < 3) {
                ++job->attempts;
                m_queue.enqueue(job);
            } else job->callback({}, "This request used an older sign-in session. Try the action again.");
            pumpRequests();
            return;
        }
        if (status >= 200 && status < 300 && reply->error() == QNetworkReply::NoError) {
            if (job->expectedStatus && status != job->expectedStatus)
                job->callback({}, "Discord did not return the expected confirmation. Refresh before trying again.");
            else if (status != 204 && parseError.error != QJsonParseError::NoError)
                job->callback({}, "Discord returned an unreadable response.");
            else job->callback(document.isArray() ? QJsonValue(document.array()) : QJsonValue(object), {});
        } else {
            const auto reason = describeError(status, object, reply->error() != QNetworkReply::NoError);
            job->callback({}, reason);
            if (status == 401 && job->generation == m_generation) {
                emit authenticationRejected();
                logout();
                setError(reason);
            }
        }
        pumpRequests();
    });
}

void DiscordClient::login(const QString &input) {
    logout();
    const auto token = input.trimmed();
    if (token.size() < 20 || token.size() > 512 || token.contains(QRegularExpression("[\\r\\n\\t]"))) {
        setError("The sign-in session was invalid. Create a new QR code and try again.");
        return;
    }
    m_token = token.toUtf8();
    m_connecting = true;
    m_status = "Checking Discord session…";
    emit stateChanged();
    request("GET", "/users/@me", {}, [this](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) {
            m_connecting = false;
            m_status = "Could not connect";
            setError(error);
            return;
        }
        const auto user = value.toObject();
        if (!validId(user.value("id").toString())) {
            m_connecting = false;
            setError("Discord did not return a valid account.");
            return;
        }
        updateUser(user);
        fetchGuilds();
        if (!m_token.startsWith("Bot ")) fetchDms();
        m_status = "Connecting to live events…";
        emit stateChanged();
        request("GET", "/gateway", {}, [this](const QJsonValue &gateway, const QString &error) {
            if (!error.isEmpty()) {
                m_connecting = false;
                setError(error);
                return;
            }
            const auto url = gateway.toObject().value("url").toString();
            if (!validGateway(QUrl(url))) {
                m_connecting = false;
                setError("Discord supplied an invalid gateway address.");
                return;
            }
            m_gatewayUrl = url;
            openGateway();
        });
    });
}

void DiscordClient::clearSelection() {
    ++m_selectionGeneration;
    m_currentChannelId.clear();
    m_currentChannelName.clear();
    m_currentChannelTopic.clear();
    m_currentChannelKind.clear();
    m_loadingMessages = false;
    m_hasMoreMessages = false;
    m_hasNewerMessages = false;
    m_followingMessages = true;
    m_deferredLiveMessages.clear();
    m_messages.clear();
    m_messageObjects.clear();
    m_pendingMessageUpdates.clear();
    m_deletedMessages.clear();
}

void DiscordClient::logout() {
    clearPendingReadReceipt();
    ++m_readReceiptOperation; m_readReceiptInFlight = false;
    m_confirmedReadMessageIds.clear(); m_latestChannelMessageIds.clear(); m_localReadMessageIds.clear(); m_readChannelGuilds.clear();
    m_deletedMessageChannels.clear(); m_deletedLatestReadBoundaries.clear();
    m_readReceiptLastSentAt = 0; m_readReceiptError.clear(); m_readReceiptErrorChannel.clear(); m_blockedReadReceipts.clear();
    stopBroadcast();
    stopWatchingStream();
    m_closing = true;
    ++m_generation;
    ++m_guildGeneration;
    m_queue.clear();
    m_heartbeat.stop();
    m_reconnect.stop();
    m_requestPump.stop();
    m_gatewayDeadline.stop();
    m_memberDeadline.stop();
    m_socket.abort();
    const auto replies = m_replies;
    for (auto *reply : replies) reply->abort();
    m_token.fill('\0');
    m_token.clear();
    m_sessionId.clear();
    m_resumeUrl.clear();
    m_sequence = -1;
    m_reconnectAttempts = 0;
    m_rateReset.clear();
    m_globalReset = 0;
    m_connected = false;
    m_connecting = false;
    m_demoMode = false;
    m_sendingMessage = false;
    m_waitingHeartbeat = false;
    m_status = "Not connected";
    m_error.clear();
    m_currentUserId.clear();
    m_currentUserName.clear();
    m_currentPresenceStatus = "online";
    m_authSessionIdHash.clear();
    m_currentGuildId.clear();
    m_currentGuildName = "Direct messages";
    clearSelection();
    m_guilds.clear();
    m_channels.clear();
    m_dms.clear();
    m_voiceMembers.clear();
    m_voiceStates.clear();
    m_voiceSnapshots.clear();
    m_streams.clear();
    m_streamObjects.clear();
    m_members.clear();
    m_guildMembers.clear();
    m_presences.clear();
    m_userPresence.clear();
    m_blockedUsers.clear();
    m_relationships.clear(); m_relationshipVersions.clear(); m_dmSocialVersions.clear();
    m_userNotes.clear(); m_userNoteVersions.clear(); m_pinnedDms.clear(); m_favoriteDms.clear(); m_hideMutedGuilds.clear();
    m_friends.clear(); m_pendingFriends.clear(); m_messageRequests.clear(); m_spamRequests.clear();
    m_socialBusy = false; m_socialError.clear(); m_socialStatus.clear();
    m_privateVoiceAllocations.clear(); m_privateCalls.clear(); m_outgoingRinging.clear(); m_privateLeaveAllocations.clear();
    m_directCallChannel.clear(); m_directCallRecipient.clear(); ++m_directCallOperation; m_directCallStarting = false;
    m_socialBatchDepth = 0; m_socialDirty = false;
    clearRequestPreview();
    m_profiles.clear(); m_profileFetchedAt.clear(); m_profilesLoading.clear(); m_channelInfoRequestedAt.clear(); m_componentsPending.clear();
    m_memberSlots.clear();
    m_subscribedGuild.clear();
    m_subscribedChannel.clear();
    m_expectedMemberList.clear();
    m_memberListReceived = m_membersLoading = false;
    m_membersPartial = true;
    m_memberCount = 0;
    m_membersStatus = "Choose a server to view members";
    m_notificationSettings.clear();
    m_seenLiveMessages.clear();
    m_seenLiveOrder.clear();
    m_notificationStartMs = 0;
    m_inviteBusy = false;
    m_inviteUrl.clear();
    m_inviteError.clear();
    emit inviteChanged();
    m_voiceSessionId.clear();
    m_pendingVoiceState = {}; m_pendingVoiceAccount.clear(); m_pendingVoiceGeneration = m_generation;
    m_voiceGuildId.clear();
    m_guildObjects.clear();
    m_guildMembershipVersions.clear();
    m_removedGuilds.clear();
    m_channelCache.clear();
    m_channelCacheOrder.clear();
    m_dmObjects.clear();
    m_dmReadMessageIds.clear();
    m_users.clear();
    m_unread.clear();
    m_closing = false;
    emit stateChanged();
}

void DiscordClient::updateUser(const QJsonObject &user) {
    if (m_currentUserId != user.value("id").toString()) {
        clearPendingReadReceipt(); ++m_readReceiptOperation; m_readReceiptInFlight = false;
        m_confirmedReadMessageIds.clear(); m_localReadMessageIds.clear(); m_blockedReadReceipts.clear();
        m_readReceiptError.clear(); m_readReceiptErrorChannel.clear(); m_readReceiptLastSentAt = 0;
    }
    m_currentUserId = user.value("id").toString();
    m_currentUserName = displayName(user);
    m_users.insert(m_currentUserId, user);
    emit stateChanged();
}

void DiscordClient::fetchGuilds(const QString &after) {
    QString path = "/users/@me/guilds?limit=200";
    if (!after.isEmpty()) path += "&after=" + after;
    request("GET", path, {}, [this](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) { setError(error); return; }
        const auto list = value.toArray();
        for (const auto &entry : list) acceptGuild(entry.toObject());
        emit modelsReady();
        if (list.size() == 200 && m_guilds.count() < m_guilds.maximumRows()) {
            const auto lastId = list.last().toObject().value("id").toString();
            if (validId(lastId)) fetchGuilds(lastId);
        }
    });
}

void DiscordClient::fetchDms() {
    const auto version = m_socialVersion;
    request("GET", "/users/@me/channels", {}, [this, version](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) { setError("Could not load direct messages. " + error); return; }
        ++m_unreadDmBatchDepth;
        const auto finishBatch=qScopeGuard([this] { if (!--m_unreadDmBatchDepth && m_unreadDmsDirty) rebuildUnreadDms(); });
        for (const auto &entry : value.toArray()) {
            auto channel = entry.toObject(); const auto id = channel.value("id").toString();
            if (m_dmSocialVersions.value(id) > version) {
                if (!m_dmObjects.contains(id)) continue;
                channel.remove("is_message_request"); channel.remove("is_spam");
            }
            acceptDm(channel);
        }
        // An empty selection represents Friends/Requests. Background snapshots
        // must not navigate into a DM or acknowledge it on the user's behalf.
        if (m_currentGuildId.isEmpty()) rebuildChannels(false);
        emit modelsReady();
    });
}

void DiscordClient::acceptGuild(QJsonObject guild) {
    const auto id = guild.value("id").toString();
    if (!validId(id) || m_removedGuilds.contains(id)) return;
    if (!m_guildObjects.contains(id) && m_guildObjects.size() >= m_guilds.maximumRows()) return;
    // User READY payloads can nest guild metadata in `properties`.
    const auto properties = guild.value("properties").toObject();
    if (!properties.isEmpty()) guild = merged(properties, guild);
    if (guild.contains("channels")) cacheChannels(id, guild.value("channels").toArray(), true);
    for (const auto &member : guild.value("members").toArray()) {
        cacheMember(id, member.toObject());
        if (member.toObject().value("user").toObject().value("id").toString() == m_currentUserId) {
            guild.insert("member", member);
        }
    }
    for (const auto &presence : guild.value("presences").toArray()) acceptPresence(id, presence.toObject());
    if (guild.contains("voice_states")) {
        m_voiceStates.remove(id);
        for (const auto &state : guild.value("voice_states").toArray()) acceptVoiceState(state.toObject(), id);
        m_voiceSnapshots.insert(id);
        if (id == m_currentGuildId || id == m_voiceGuildId) rebuildVoiceMembers();
    }
    // Retain only the current member and data used by this client, rather than huge guild snapshots.
    static const QSet<QString> retainedKeys = {"id", "name", "icon", "owner_id", "roles", "member", "permissions", "unavailable", "member_count", "default_message_notifications", "description", "approximate_member_count", "approximate_presence_count"};
    for (auto it = guild.begin(); it != guild.end();) {
        if (!retainedKeys.contains(it.key())) it = guild.erase(it);
        else ++it;
    }
    guild = merged(m_guildObjects.value(id), guild);
    if (!m_guildObjects.contains(id)) m_guildMembershipVersions.insert(id, ++m_nextGuildMembershipVersion);
    m_guildObjects.insert(id, guild);
    m_guilds.upsert(guildRow(guild));
    emit channelReferencesChanged();
    if (id == m_currentGuildId) {
        m_currentGuildName = m_guilds.byId(id).value("name").toString();
        rebuildChannels(m_currentChannelId.isEmpty());
        rebuildMembers();
        emit stateChanged();
    }
}

void DiscordClient::acceptDm(const QJsonObject &channel) {
    const auto id = channel.value("id").toString();
    if (!validId(id)) return;
    if (!m_dmObjects.contains(id) && m_dmObjects.size() >= m_dms.maximumRows()) return;
    const auto previous = m_dmObjects.value(id);
    auto object = merged(previous, channel);
    // A slow initial snapshot must not undo activity already received live.
    const auto lastSeen = m_dmObjects.value(id).value("last_message_id").toString();
    if (newerSnowflake(lastSeen, object.value("last_message_id").toString()))
        object.insert("last_message_id", lastSeen);
    if (m_deletedMessageChannels.value(object.value("last_message_id").toString()) == id)
        object.remove("last_message_id");
    m_dmObjects.insert(id, object);
    noteLatestChannelMessage(id, object.value("last_message_id").toString());
    if (previous.isEmpty() || object.value("is_message_request") != previous.value("is_message_request") ||
        object.value("is_spam") != previous.value("is_spam") || object.value("type") != previous.value("type"))
        m_dmSocialVersions.insert(id, ++m_socialVersion);
    for (const auto &recipient : channel.value("recipients").toArray()) rememberMember({{"user",recipient.toObject()}});
    syncDm(id);
    rebuildSocial();
}

void DiscordClient::rebuildUnreadDms() {
    if (m_unreadDmBatchDepth) { m_unreadDmsDirty=true; return; }
    m_unreadDmsDirty=false;
    QVector<QVariantMap> rows;
    qint64 nextMuteExpiry=0;
    const auto now=nowMs();
    QHash<QString,qint64> muted;
    for (const auto &entry : m_notificationSettings.value({}).value("channel_overrides").toArray()) {
        const auto setting=entry.toObject();
        if (!setting.value("muted").toBool()) continue;
        const auto end=QDateTime::fromString(setting.value("mute_config").toObject().value("end_time").toString(),Qt::ISODateWithMs);
        if (!end.isValid() || end.toMSecsSinceEpoch()>now) muted.insert(setting.value("channel_id").toString(),end.isValid()?end.toMSecsSinceEpoch():0);
    }
    for (const auto &row : m_dms.rows()) {
        const auto id = row.value("recordId").toString();
        const int type = m_dmObjects.value(id).value("type").toInt(-1);
        if ((type != 1 && type != 3) || row.value("unread").toInt() <= 0 ||
            (type == 1 && (row.value("blocked").toBool() || m_relationships.value(dmRecipientId(m_dmObjects.value(id))).value("user_ignored").toBool()))) continue;
        if (muted.contains(id)) {
            const auto end=muted.value(id);
            if (end && (!nextMuteExpiry || end<nextMuteExpiry)) nextMuteExpiry=end;
            continue;
        }
        rows.append(row);
    }
    m_unreadDms.reconcile(std::move(rows));
    if (nextMuteExpiry) m_unreadDmMuteTimer.start(int(std::clamp(nextMuteExpiry-nowMs()+1,qint64(1),qint64(86400000))));
    else m_unreadDmMuteTimer.stop();
}

void DiscordClient::acceptDmReadState(const QJsonObject &state) {
    const auto channelId = state.value("channel_id").toString(state.value("id").toString());
    const auto messageId = state.value("message_id").toString(state.value("last_message_id").toString());
    const bool direct = m_dmObjects.contains(channelId);
    const auto guildId = direct ? QString{} : readChannelGuild(channelId);
    if ((!direct && guildId.isEmpty()) || !validId(messageId) || state.value("read_state_type").toInt() != 0) return;
    if (newerSnowflake(messageId, m_confirmedReadMessageIds.value(channelId)) &&
        (m_confirmedReadMessageIds.contains(channelId) || m_confirmedReadMessageIds.size() < 20000))
        m_confirmedReadMessageIds.insert(channelId, messageId);
    if (state.value("mention_count").toInt() > 0) return;
    applyReadWatermark(channelId, messageId);
}

void DiscordClient::applyReadWatermark(const QString &channelId, const QString &messageId) {
    const bool direct = m_dmObjects.contains(channelId);
    const auto guildId = direct ? QString{} : readChannelGuild(channelId);
    if ((!direct && guildId.isEmpty()) || !validId(messageId)) return;
    if (newerSnowflake(messageId, m_localReadMessageIds.value(channelId)) &&
        (m_localReadMessageIds.contains(channelId) || m_localReadMessageIds.size() < 20000))
        m_localReadMessageIds.insert(channelId, messageId);
    if (direct && newerSnowflake(messageId, m_dmReadMessageIds.value(channelId))) m_dmReadMessageIds.insert(channelId, messageId);
    auto latest = m_latestChannelMessageIds.value(channelId);
    const auto dmLatest = m_dmObjects.value(channelId).value("last_message_id").toString();
    if (m_deletedMessageChannels.value(dmLatest) != channelId && newerSnowflake(dmLatest, latest)) latest = dmLatest;
    const auto deletedBoundary = m_deletedLatestReadBoundaries.value(channelId);
    if (newerSnowflake(deletedBoundary, latest)) latest = deletedBoundary;
    // An acknowledgement for older history must not erase newer live arrivals.
    if (newerSnowflake(latest, messageId)) return;
    if (direct) {
        m_unread.remove(channelId); m_channels.patch(channelId, {{"unread", 0}}); m_dms.patch(channelId, {{"unread", 0}});
    }
    else {
        const int previous = m_unread.take(channelId);
        m_channels.patch(channelId, {{"unread", 0}});
        if (previous > 0) {
            const int remaining = std::max(0, m_unread.value(guildId) - previous);
            m_unread.insert(guildId, remaining); m_guilds.patch(guildId, {{"unread", remaining}});
        }
    }
}

QString DiscordClient::readChannelGuild(const QString &channelId) const {
    if (channelId == m_currentChannelId && m_guildObjects.contains(m_currentGuildId)) return m_currentGuildId;
    const auto remembered = m_readChannelGuilds.value(channelId);
    if (m_guildObjects.contains(remembered)) return remembered;
    for (auto guild = m_channelCache.cbegin(); guild != m_channelCache.cend(); ++guild)
        for (const auto &entry : guild.value())
            if (m_guildObjects.contains(guild.key()) && entry.toObject().value("id").toString() == channelId) return guild.key();
    return {};
}

void DiscordClient::noteLatestChannelMessage(const QString &channelId, const QString &messageId, const QString &guildId) {
    if (validId(channelId) && validId(guildId) && (m_readChannelGuilds.contains(channelId) || m_readChannelGuilds.size() < 20000))
        m_readChannelGuilds.insert(channelId, guildId);
    if (validId(channelId) && m_deletedMessageChannels.value(messageId) != channelId && newerSnowflake(messageId, m_latestChannelMessageIds.value(channelId)) &&
        (m_latestChannelMessageIds.contains(channelId) || m_latestChannelMessageIds.size() < 20000))
        m_latestChannelMessageIds.insert(channelId, messageId);
}

void DiscordClient::retireDeletedReadTarget(const QString &channelId, const QString &messageId) {
    if (!validId(channelId) || !validId(messageId) || m_deletedMessageChannels.value(messageId) == channelId) return;
    const bool latest = m_latestChannelMessageIds.value(channelId) == messageId ||
        m_dmObjects.value(channelId).value("last_message_id").toString() == messageId;
    const bool loaded = channelId == m_currentChannelId && m_messageObjects.contains(messageId);
    if (!latest && !loaded) return;
    if (m_deletedMessageChannels.size() >= 20000) m_deletedMessageChannels.erase(m_deletedMessageChannels.begin());
    m_deletedMessageChannels.insert(messageId, channelId);
    if (!latest) return;
    if (m_latestChannelMessageIds.value(channelId) == messageId) m_latestChannelMessageIds.remove(channelId);
    if (m_dmObjects.value(channelId).value("last_message_id").toString() == messageId)
        m_dmObjects[channelId].remove("last_message_id");
    // Deletion is not a read. Until a fresh visible tail is observed, an old
    // ACK cannot use the missing latest marker to erase unknown earlier unread.
    if (newerSnowflake(messageId, m_deletedLatestReadBoundaries.value(channelId)) &&
        (m_deletedLatestReadBoundaries.contains(channelId) || m_deletedLatestReadBoundaries.size() < 20000))
        m_deletedLatestReadBoundaries.insert(channelId, messageId);
}

bool DiscordClient::readReceiptAllowed(const QString &channelId, const QString &messageId, bool requireTail) const {
    if (!m_connected || m_demoMode || m_token.isEmpty() || m_token.startsWith("Bot ") || !validId(m_currentUserId) ||
        !m_readingActive || !m_followingMessages || m_hasNewerMessages || m_loadingMessages ||
        !validId(channelId) || !validId(messageId) || channelId != m_currentChannelId ||
        pendingMessageRequest(m_dmObjects.value(channelId)) || m_currentChannelKind == "hidden" ||
        (m_currentChannelKind != "text" && m_currentChannelKind != "dm" && m_currentChannelKind != "thread" &&
         m_currentChannelKind != "voice" && m_currentChannelKind != "stage")) return false;
    const auto message = m_messageObjects.value(messageId);
    if (message.value("channel_id").toString() != channelId || m_messages.indexOf(messageId) < 0 ||
        m_deletedMessages.contains(messageId)) return false;
    if (requireTail) {
        QString tail;
        // Failed/pending local sends can remain after the last received server
        // message. They are not read-state targets and cannot hide that tail.
        for (int at = m_messages.count() - 1; at >= 0; --at) {
            const auto candidate = m_messages.recordIdAt(at);
            if (validId(candidate) && m_messageObjects.value(candidate).value("channel_id").toString() == channelId) {
                const auto row = m_messages.get(at);
                if (row.value("pending").toBool() || row.value("failed").toBool()) continue;
                tail = candidate; break;
            }
        }
        if (tail != messageId) return false;
    }
    if (m_currentGuildId.isEmpty()) {
        const int type = m_dmObjects.value(channelId).value("type").toInt(-1);
        return type == 1 || type == 3;
    }
    const auto channel = cachedChannel(m_currentGuildId, channelId);
    return m_guildObjects.contains(m_currentGuildId) && !channel.isEmpty() && channelVisible(channel, m_currentGuildId);
}

void DiscordClient::clearPendingReadReceipt() {
    m_readReceiptTimer.stop(); m_pendingReadReceipts.clear(); m_readReceiptOrder.clear();
}

bool DiscordClient::acknowledgeVisibleMessages(const QString &channelId, const QString &messageId) {
    if (!readReceiptAllowed(channelId, messageId, true)) return false;
    auto latest = m_latestChannelMessageIds.value(channelId);
    const auto dmLatest = m_dmObjects.value(channelId).value("last_message_id").toString();
    if (m_deletedMessageChannels.value(dmLatest) != channelId && newerSnowflake(dmLatest, latest)) latest = dmLatest;
    if (!newerSnowflake(latest, messageId)) m_deletedLatestReadBoundaries.remove(channelId);
    applyReadWatermark(channelId, messageId);
    if (!newerSnowflake(messageId, m_confirmedReadMessageIds.value(channelId))) return true;
    // An existing retained proof may advance to a newer actually seen message
    // during its cooldown. Its retry deadline and bounded budget stay intact.
    if (m_blockedReadReceipts.value(channelId) > nowMs() && !m_pendingReadReceipts.contains(channelId)) return false;
    if (!m_pendingReadReceipts.contains(channelId)) {
        if (m_pendingReadReceipts.size() >= 512) {
            m_readReceiptError = "Read status is waiting to sync. View this conversation again after Discord reconnects.";
            m_readReceiptErrorChannel = channelId; emit stateChanged(); return false;
        }
        m_pendingReadReceipts.insert(channelId, {messageId, m_currentUserId, m_generation, 0, 0});
        m_readReceiptOrder.enqueue(channelId);
    } else if (newerSnowflake(messageId, m_pendingReadReceipts.value(channelId).messageId)) {
        m_pendingReadReceipts[channelId].messageId = messageId;
    }
    if (!m_readReceiptInFlight && (!m_readReceiptTimer.isActive() || m_readReceiptTimer.remainingTime() > 250))
        m_readReceiptTimer.start(250);
    return true;
}

bool DiscordClient::readReceiptAccessible(const QString &channelId) const {
    if (m_demoMode || m_token.isEmpty() || m_token.startsWith("Bot ")) return false;
    if (m_dmObjects.contains(channelId)) {
        const auto channel = m_dmObjects.value(channelId);
        const int type = channel.value("type").toInt(-1);
        return (type == 1 || type == 3) && !pendingMessageRequest(channel);
    }
    const auto guildId = readChannelGuild(channelId);
    if (guildId.isEmpty()) return false;
    for (const auto &entry : m_channelCache.value(guildId))
        if (entry.toObject().value("id").toString() == channelId) return channelVisible(entry.toObject(), guildId);
    // A remembered guild ID does not prove current channel access after the
    // bounded permission snapshot has been evicted.
    return false;
}

void DiscordClient::flushReadReceipt() {
    if (m_readReceiptInFlight || !m_connected || m_pendingReadReceipts.isEmpty()) return;
    QString channel; ObservedReadReceipt observed;
    qint64 earliest = nowMs() + 60000;
    for (int remaining = m_readReceiptOrder.size(); remaining > 0; --remaining) {
        const auto candidate = m_readReceiptOrder.dequeue();
        if (!m_pendingReadReceipts.contains(candidate)) continue;
        const auto receipt = m_pendingReadReceipts.value(candidate);
        if (receipt.generation != m_generation || receipt.accountId != m_currentUserId || !readReceiptAccessible(candidate) ||
            !newerSnowflake(receipt.messageId, m_confirmedReadMessageIds.value(candidate))) {
            m_pendingReadReceipts.remove(candidate); continue;
        }
        const auto due = std::max(m_readReceiptLastSentAt + 1000, receipt.retryAfter);
        if (due > nowMs()) { earliest = std::min(earliest, due); m_readReceiptOrder.enqueue(candidate); continue; }
        channel = candidate; observed = m_pendingReadReceipts.take(candidate); break;
    }
    if (channel.isEmpty()) {
        if (!m_pendingReadReceipts.isEmpty()) m_readReceiptTimer.start(int(std::clamp(earliest - nowMs(), qint64(1), qint64(60000))));
        return;
    }
    const auto message = observed.messageId;
    m_readReceiptInFlight = true;
    const auto operation = ++m_readReceiptOperation, generation = m_generation;
    const auto account = m_currentUserId;
    request("POST", "/channels/" + channel + "/messages/" + message + "/ack", {{"token", QJsonValue::Null}},
        [this, operation, account, channel, message, observed](const QJsonValue &value, const QString &error) {
        if (operation != m_readReceiptOperation) return;
        m_readReceiptInFlight = false;
        if (account != m_currentUserId) return;
        if (error.isEmpty()) {
            m_blockedReadReceipts.remove(channel);
            if (m_readReceiptErrorChannel == channel) { m_readReceiptError.clear(); m_readReceiptErrorChannel.clear(); }
            acceptDmReadState({{"channel_id", channel}, {"message_id", message}, {"mention_count", 0}});
            Q_UNUSED(value);
        } else if (error == "Read receipt no longer current.") {
            // A connection loss while waiting behind a REST rate limit does
            // not undo an already observed read. Resume that proof on reconnect.
            if (!m_connected && readReceiptAccessible(channel) && newerSnowflake(message, m_confirmedReadMessageIds.value(channel)) &&
                !m_pendingReadReceipts.contains(channel)) {
                m_pendingReadReceipts.insert(channel, observed); m_readReceiptOrder.enqueue(channel);
            }
        } else {
            const int failures = observed.failures + 1;
            const bool transient = error.contains("could not be confirmed") || error.contains("slow down") ||
                error.startsWith("Discord request failed (5");
            const bool newerObserved = m_pendingReadReceipts.contains(channel) &&
                newerSnowflake(m_pendingReadReceipts.value(channel).messageId, message);
            const bool messageUnavailable = error == "This channel or message is no longer available.";
            if (messageUnavailable && newerObserved && readReceiptAccessible(channel)) {
                // An older observed message may have been deleted while its
                // ACK was in flight. Its 404 says nothing about the newer proof.
                m_blockedReadReceipts.remove(channel);
            } else if (transient && failures <= 2 && readReceiptAccessible(channel)) {
                if (!m_pendingReadReceipts.contains(channel)) {
                    m_pendingReadReceipts.insert(channel, observed); m_readReceiptOrder.enqueue(channel);
                }
                auto &pending = m_pendingReadReceipts[channel];
                if (newerSnowflake(message, pending.messageId)) pending.messageId = message;
                pending.failures = failures; pending.retryAfter = nowMs() + (failures == 1 ? 1500 : 5000);
            } else {
                const bool retainNewer = transient && newerObserved && readReceiptAccessible(channel);
                m_readReceiptError = retainNewer ? "Read status could not sync with Discord. Relay will retry automatically."
                    : "Read status could not sync with Discord. Relay will try again when you view this conversation.";
                m_readReceiptErrorChannel = channel;
                if (m_blockedReadReceipts.size() >= 512 && !m_blockedReadReceipts.contains(channel)) m_blockedReadReceipts.erase(m_blockedReadReceipts.begin());
                const auto retryAt = nowMs() + 60000;
                m_blockedReadReceipts.insert(channel, retryAt);
                if (retainNewer) {
                    // Exhausting an older target's budget must not discard an
                    // already observed newer target. Keep the cooldown, then
                    // give that newer proof its own bounded attempt budget.
                    auto &pending = m_pendingReadReceipts[channel];
                    pending.failures = 0; pending.retryAfter = retryAt;
                } else { m_pendingReadReceipts.remove(channel); m_readReceiptOrder.removeAll(channel); }
            }
        }
        if (!m_pendingReadReceipts.isEmpty()) m_readReceiptTimer.start(0);
        emit stateChanged();
    }, {}, 0, [this, generation, account, channel, message] {
        if (m_generation != generation || m_currentUserId != account || !m_connected ||
            !readReceiptAccessible(channel) || !newerSnowflake(message, m_confirmedReadMessageIds.value(channel))) return false;
        m_readReceiptLastSentAt = nowMs(); return true;
    });
}

bool DiscordClient::newerSnowflake(const QString &a, const QString &b) {
    if (!validId(a)) return false;
    if (!validId(b)) return true;
    // Compare decimal strings, never floating point: snowflakes exceed JS's
    // integer precision, including adjacent IDs within the same millisecond.
    return a.size() == b.size() ? a > b : a.size() > b.size();
}

void DiscordClient::syncDm(const QString &id) {
    const auto object = m_dmObjects.value(id);
    if (object.isEmpty()) return;
    // Pending requests belong in Requests/Spam, never in normal conversations
    // or the unread DM rail. Accepting one explicitly moves it into this model.
    if (pendingMessageRequest(object)) {
        m_dms.remove(id);
        if (m_currentGuildId.isEmpty()) m_channels.remove(id);
        return;
    }
    ++m_unreadDmBatchDepth;
    const auto finishBatch=qScopeGuard([this] { if (!--m_unreadDmBatchDepth && m_unreadDmsDirty) rebuildUnreadDms(); });
    const auto activity = [this](const QString &channelId) {
        const auto last = m_dmObjects.value(channelId).value("last_message_id").toString();
        return validId(last) ? last : channelId;
    };
    const auto latest = activity(id);
    int position = 0;
    for (const auto &row : m_dms.rows()) {
        const auto otherId = row.value("recordId").toString();
        if (otherId == id) continue;
        const int rank = m_pinnedDms.contains(id) ? 2 : (m_favoriteDms.contains(id) ? 1 : 0);
        const int otherRank = m_pinnedDms.contains(otherId) ? 2 : (m_favoriteDms.contains(otherId) ? 1 : 0);
        if (rank != otherRank) { if (otherRank > rank) ++position; continue; }
        const auto other = activity(otherId);
        if (newerSnowflake(other, latest) || (other == latest && newerSnowflake(otherId, id))) ++position;
    }
    const auto row = channelRow(object);
    m_dms.upsert(row);
    m_dms.move(id, position);
    if (m_currentGuildId.isEmpty()) {
        m_channels.upsert(row);
        m_channels.move(id, position);
        if (m_currentChannelId == id && m_currentChannelName != row.value("name").toString()) {
            m_currentChannelName = row.value("name").toString(); emit stateChanged();
        }
    }
}

void DiscordClient::recordDmActivity(const QJsonObject &message) {
    const auto channelId = message.value("channel_id").toString();
    const auto messageId = message.value("id").toString();
    if (m_deletedMessageChannels.value(messageId) == channelId) return;
    noteLatestChannelMessage(channelId, messageId, message.value("guild_id").toString());
    if (!m_dmObjects.contains(channelId) || !validId(messageId)) return;
    const auto old = m_dmObjects.value(channelId).value("last_message_id").toString();
    if (!newerSnowflake(messageId, old)) return;
    m_dmObjects[channelId].insert("last_message_id", messageId);
    syncDm(channelId);
    if (pendingMessageRequest(m_dmObjects.value(channelId))) rebuildSocial();
}

void DiscordClient::rememberMember(const QJsonObject &member) {
    const auto user = member.value("user").toObject();
    const auto userId = user.value("id").toString();
    if (validId(userId) && (m_users.contains(userId) || m_users.size() < 5000)) {
        const auto previous = m_users.value(userId), updated = merged(previous,user);
        if (previous == updated) return;
        m_users.insert(userId,updated);
        for (auto it=m_dmObjects.cbegin(); it!=m_dmObjects.cend(); ++it) {
            bool matches=it.value().value("recipient_ids").toArray().contains(userId);
            for (const auto &recipient : it.value().value("recipients").toArray())
                if (recipient.toObject().value("id").toString()==userId) matches=true;
            if (matches) { syncDm(it.key()); if (pendingMessageRequest(it.value())) rebuildSocial(); }
        }
        if (m_relationships.contains(userId)) rebuildSocial();
    }
}

void DiscordClient::cacheMember(const QString &guildId, const QJsonObject &input) {
    if (!validId(guildId)) return;
    auto member = input;
    auto user = member.value("user").toObject();
    auto id = user.value("id").toString(member.value("user_id").toString());
    if (!validId(id)) return;
    if (user.isEmpty()) user = m_users.value(id);
    user.insert("id", id);
    member.insert("user", user);
    rememberMember(member);
    if (!m_guildMembers.contains(guildId) && m_guildMembers.size() >= 1000) return;
    auto &members = m_guildMembers[guildId];
    if (!members.contains(id)) {
        qsizetype total = 0;
        for (auto it = m_guildMembers.cbegin(); it != m_guildMembers.cend(); ++it) total += it.value().size();
        if (members.size() >= 500 || total >= 5000) {
            // The visible subscription has priority over old snapshot entries.
            if (guildId != m_currentGuildId) return;
            if (members.size() >= 500) members.erase(members.begin());
            else for (auto it = m_guildMembers.begin(); it != m_guildMembers.end(); ++it)
                if (it.key() != guildId && !it.value().isEmpty()) { it.value().erase(it.value().begin()); break; }
        }
    }
    static const QSet<QString> keys{"user", "nick", "avatar", "roles", "presence"};
    for (auto it = member.begin(); it != member.end();)
        if (!keys.contains(it.key())) it = member.erase(it); else ++it;
    members.insert(id, merged(members.value(id), member));
    if (member.value("presence").isObject()) {
        auto presence = member.value("presence").toObject();
        presence.insert("user", user);
        acceptPresence(guildId, presence);
    }
}

QVariantMap DiscordClient::userIndicators(const QString &userId) const {
    const auto devices = m_userPresence.value(userId).value("client_status").toObject();
    QVariantMap result{{"desktopStatus", devices.value("desktop").toString()},
        {"mobileStatus", devices.value("mobile").toString()}, {"webStatus", devices.value("web").toString()},
        {"blocked", m_blockedUsers.contains(userId)}, {"inVoice", false}, {"streaming", false}, {"camera", false}, {"muted", false}, {"deafened", false}};
    for (const auto &states : m_voiceStates) {
        const auto state = states.value(userId);
        if (state.value("channel_id").toString().isEmpty()) continue;
        result.insert("inVoice", true);
        for (const auto &key : {"streaming", "camera", "muted", "deafened"}) result.insert(key, result.value(key).toBool() || state.value(key).toBool());
    }
    return result;
}

bool DiscordClient::conversationMuted(const QString &channelId) const {
    const auto settings = m_notificationSettings.value({});
    for (const auto &entry : settings.value("channel_overrides").toArray()) {
        const auto setting = entry.toObject();
        if (setting.value("channel_id").toString() != channelId || !setting.value("muted").toBool()) continue;
        const auto end = QDateTime::fromString(setting.value("mute_config").toObject().value("end_time").toString(), Qt::ISODateWithMs);
        return !end.isValid() || end.toMSecsSinceEpoch() > nowMs();
    }
    return false;
}

void DiscordClient::acceptRelationship(const QJsonObject &relationship, bool removed) {
    const auto user = relationship.value("user").toObject();
    const auto id = relationship.value("id").toString(user.value("id").toString());
    if (!validId(id) || (!user.isEmpty() && user.value("id").toString() != id)) return;
    rememberMember(QJsonObject{{"user", user}});
    const auto updated = merged(m_relationships.value(id), relationship);
    const int type = updated.value("type").toInt();
    if (!removed && type == 2) {
        if (m_blockedUsers.size() < 5000) m_blockedUsers.insert(id);
    } else m_blockedUsers.remove(id);
    if (removed || type < 0 || type > 6 || (!type && !updated.value("user_ignored").toBool())) m_relationships.remove(id);
    else if (m_relationships.contains(id) || m_relationships.size() < 5000) m_relationships.insert(id, updated);
    m_relationshipVersions.insert(id, ++m_socialVersion);
    if (!m_socialBatchDepth) {
        for (auto it=m_dmObjects.cbegin(); it!=m_dmObjects.cend(); ++it) syncDm(it.key());
        rebuildMembers(); rebuildVoiceMembers();
    }
    rebuildSocial();
}

QString DiscordClient::dmRecipientId(const QJsonObject &channel) const {
    for (const auto &entry : channel.value("recipients").toArray()) {
        const auto id = entry.toObject().value("id").toString();
        if (validId(id) && id != m_currentUserId) return id;
    }
    for (const auto &entry : channel.value("recipient_ids").toArray()) {
        const auto id = entry.toString();
        if (validId(id) && id != m_currentUserId) return id;
    }
    return {};
}

bool DiscordClient::pendingMessageRequest(const QJsonObject &channel) {
    if (channel.value("type").toInt(-1) != 1) return false;
    const auto request = channel.value("is_message_request");
    // Accepted requests can retain is_spam=true in Discord's channel snapshot.
    // The explicit consent flag wins over that historical classification.
    return request.isBool() ? request.toBool() : channel.value("is_spam").toBool();
}

QVariantMap DiscordClient::socialUserRow(const QString &userId) const {
    const auto user = m_users.value(userId);
    const auto relationship = m_relationships.value(userId);
    auto name = cleanText(relationship.value("nickname").toString(), 128);
    if (name.isEmpty()) name = displayName(user);
    const auto presence = m_userPresence.value(userId);
    auto row = userIndicators(userId);
    row.insert("recordId", userId); row.insert("userId", userId);
    row.insert("name", name); row.insert("username", cleanText(user.value("username").toString(), 128));
    row.insert("initials", initials(name)); row.insert("avatarUrl", avatarUrl(user));
    row.insert("status", presence.value("status").toString("offline"));
    row.insert("activity", presence.value("activity").toString());
    row.insert("bot", user.value("bot").toBool());
    row.insert("verifiedBot", bool(user.value("public_flags").toInt() & (1 << 16)));
    row.insert("primaryGuild", primaryGuild(user));
    row.insert("relationshipType", relationship.value("type").toInt());
    return row;
}

void DiscordClient::rebuildSocial() {
    if (m_socialBatchDepth) { m_socialDirty = true; return; }
    m_socialDirty = false;
    QVariantList friends, pending, requests, spam;
    for (auto it = m_relationships.cbegin(); it != m_relationships.cend(); ++it) {
        const int type = it.value().value("type").toInt();
        if (type != 1 && type != 3 && type != 4) continue;
        auto row = socialUserRow(it.key());
        if (type == 1) friends.append(row);
        else { row.insert("direction", type == 3 ? "incoming" : "outgoing"); pending.append(row); }
    }
    const auto nameOrder = [](const QVariant &a, const QVariant &b) {
        const auto left = a.toMap(), right = b.toMap();
        const int name = QString::compare(left.value("name").toString(), right.value("name").toString(), Qt::CaseInsensitive);
        return name ? name < 0 : left.value("recordId").toString() < right.value("recordId").toString();
    };
    std::sort(friends.begin(), friends.end(), nameOrder); std::sort(pending.begin(), pending.end(), nameOrder);
    for (auto it = m_dmObjects.cbegin(); it != m_dmObjects.cend(); ++it) {
        if (!pendingMessageRequest(it.value())) continue;
        const auto recipient = dmRecipientId(it.value());
        if (m_blockedUsers.contains(recipient) || m_relationships.value(recipient).value("user_ignored").toBool()) continue;
        auto row = socialUserRow(recipient);
        row.insert("recordId", it.key()); row.insert("channelId", it.key());
        row.insert("requestedAt", cleanText(it.value().value("is_message_request_timestamp").toString(), 80));
        row.insert("preview", QStringLiteral("Message request"));
        row.insert("spam", it.value().value("is_spam").toBool());
        if (row.value("spam").toBool()) spam.append(row); else requests.append(row);
    }
    const auto newest = [this](const QVariant &a, const QVariant &b) {
        const auto left = a.toMap().value("recordId").toString(), right = b.toMap().value("recordId").toString();
        const auto la = m_dmObjects.value(left).value("last_message_id").toString(left);
        const auto ra = m_dmObjects.value(right).value("last_message_id").toString(right);
        return la == ra ? newerSnowflake(left, right) : newerSnowflake(la, ra);
    };
    std::sort(requests.begin(), requests.end(), newest); std::sort(spam.begin(), spam.end(), newest);
    m_friends = std::move(friends); m_pendingFriends = std::move(pending);
    m_messageRequests = std::move(requests); m_spamRequests = std::move(spam);
    const auto selectedId = m_selectedMessageRequest.value("channelId").toString();
    if (!selectedId.isEmpty()) {
        QVariantMap selected;
        for (const auto &rows : {m_messageRequests, m_spamRequests})
            for (const auto &row : rows) if (row.toMap().value("channelId").toString() == selectedId) selected = row.toMap();
        if (selected.isEmpty()) clearRequestPreview(); else m_selectedMessageRequest = selected;
    }
    emit socialChanged();
}

void DiscordClient::clearRequestPreview() {
    ++m_requestPreviewGeneration;
    m_selectedMessageRequest.clear(); m_requestPreviewMessages.clear(); m_requestPreviewLoading = false;
    emit socialChanged();
}

void DiscordClient::clearSocialFeedback() {
    m_socialError.clear(); m_socialStatus.clear(); emit socialChanged();
}

bool DiscordClient::socialReady() {
    if (m_socialBusy) return false;
    if (!m_demoMode && m_connected && !m_token.isEmpty() && !m_token.startsWith("Bot ")) return true;
    m_socialError = "Sign in to your Discord account before changing friends or message requests.";
    m_socialStatus.clear(); emit socialChanged(); return false;
}

void DiscordClient::selectHome() {
    selectGuildInternal({}, false);
    if (!m_currentChannelId.isEmpty()) { clearSelection(); emit stateChanged(); }
}

void DiscordClient::refreshSocial() {
    if (!socialReady()) return;
    m_socialBusy = true; m_socialError.clear(); m_socialStatus = "Refreshing…"; emit socialChanged();
    const auto version = m_socialVersion;
    request("GET", "/users/@me/relationships", {}, [this, version](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) { m_socialBusy = false; m_socialError = error; m_socialStatus.clear(); emit socialChanged(); return; }
        if (!value.isArray()) { m_socialBusy = false; m_socialError = "Discord did not return a readable friends list."; m_socialStatus.clear(); emit socialChanged(); return; }
        ++m_socialBatchDepth;
        QSet<QString> seen;
        for (const auto &entry : value.toArray()) {
            const auto relationship = entry.toObject();
            const auto id = relationship.value("id").toString(relationship.value("user").toObject().value("id").toString());
            if (!validId(id)) continue;
            seen.insert(id);
            // A REST snapshot cannot reverse an event received after it began.
            if (m_relationshipVersions.value(id) <= version) acceptRelationship(relationship);
        }
        const auto previous = m_relationships.keys();
        for (const auto &id : previous) if (!seen.contains(id) && m_relationshipVersions.value(id) <= version) acceptRelationship({{"id", id}}, true);
        --m_socialBatchDepth; rebuildSocial();
        request("GET", "/users/@me/channels", {}, [this, version](const QJsonValue &channels, const QString &channelError) {
            if (channelError.isEmpty() && channels.isArray()) {
                ++m_socialBatchDepth;
                for (const auto &entry : channels.toArray()) {
                    const auto channel = entry.toObject(); const auto id = channel.value("id").toString();
                    if (validId(id) && m_dmSocialVersions.value(id) <= version) acceptDm(channel);
                }
                --m_socialBatchDepth; rebuildSocial();
            } else m_socialError = channelError.isEmpty() ? "Discord did not return a readable message requests list." : channelError;
            m_socialBusy = false; m_socialStatus.clear(); emit socialChanged();
        });
    });
}

bool DiscordClient::sendFriendRequest(const QString &input) {
    if (!socialReady()) return false;
    const auto text = input.trimmed();
    auto username = text; QJsonValue discriminator(QJsonValue::Null);
    const auto legacy = QRegularExpression("^(.{2,32})#([0-9]{4})$").match(text);
    if (legacy.hasMatch()) { username = legacy.captured(1); discriminator = legacy.captured(2).toInt(); }
    else if (!QRegularExpression("^[A-Za-z0-9_.]{2,32}$").match(username).hasMatch()) {
        m_socialError = "Enter a Discord username, or a legacy username#0000 tag."; m_socialStatus.clear(); emit socialChanged(); return false;
    }
    if (username.contains(QChar(0)) || cleanText(username, 32) != username) return false;
    m_socialBusy = true; m_socialError.clear(); m_socialStatus = "Sending friend request…"; emit socialChanged();
    request("POST", "/users/@me/relationships", {{"username", username}, {"discriminator", discriminator}},
        [this](const QJsonValue &value, const QString &error) {
        m_socialBusy = false;
        if (!error.isEmpty()) { m_socialError = error; m_socialStatus.clear(); emit socialChanged(); return; }
        const auto relationship = value.toObject();
        if (relationship.value("type").toInt() == 4) acceptRelationship(relationship);
        m_socialStatus = "Friend request sent."; emit socialChanged();
        // The normal response has no user object. The gateway supplies it; this
        // read also covers clients where that event was already missed.
        const auto version = m_socialVersion;
        request("GET", "/users/@me/relationships", {}, [this, version](const QJsonValue &relationships, const QString &readError) {
            if (!readError.isEmpty() || !relationships.isArray()) return;
            ++m_socialBatchDepth;
            for (const auto &entry : relationships.toArray()) {
                const auto item = entry.toObject();
                const auto id = item.value("id").toString(item.value("user").toObject().value("id").toString());
                if (m_relationshipVersions.value(id) <= version) acceptRelationship(item);
            }
            --m_socialBatchDepth; rebuildSocial();
        });
    });
    return true;
}

bool DiscordClient::changeFriendRelationship(const QString &userId, int expectedType, bool accept) {
    if (!socialReady()) return false;
    const auto relationship = m_relationships.value(userId);
    const int type = relationship.value("type").toInt();
    if (!validId(userId) || userId == m_currentUserId || (expectedType ? type != expectedType : (type != 3 && type != 4))) {
        m_socialError = "This friend action is no longer available. Refresh the list first."; emit socialChanged(); return false;
    }
    const auto version = m_relationshipVersions.value(userId);
    m_socialBusy = true; m_socialError.clear(); m_socialStatus = "Saving…"; emit socialChanged();
    request(accept ? "PUT" : "DELETE", "/users/@me/relationships/" + userId, {},
        [this, userId, version, accept](const QJsonValue &, const QString &error) {
        m_socialBusy = false; m_socialStatus.clear();
        if (!error.isEmpty()) m_socialError = error;
        else if (m_relationshipVersions.value(userId) == version) acceptRelationship({{"id", userId}, {"type", 1}}, !accept);
        emit socialChanged();
    });
    return true;
}

bool DiscordClient::acceptFriendRequest(const QString &userId) { return changeFriendRelationship(userId, 3, true); }
bool DiscordClient::dismissFriendRequest(const QString &userId) { return changeFriendRelationship(userId, 0, false); }
bool DiscordClient::removeFriend(const QString &userId) { return changeFriendRelationship(userId, 1, false); }

bool DiscordClient::addFriend(const QString &userId) {
    if (!socialReady()) return false;
    const int type = m_relationships.value(userId).value("type").toInt();
    if (!validId(userId) || userId == m_currentUserId || (type != 0 && type != 5 && type != 6) || m_blockedUsers.contains(userId)) {
        m_socialError = "This friend request is no longer available. Refresh the person’s profile first."; emit socialChanged(); return false;
    }
    const auto version = m_relationshipVersions.value(userId);
    m_socialBusy = true; m_socialError.clear(); m_socialStatus = "Sending friend request…"; emit socialChanged();
    request("PUT", "/users/@me/relationships/" + userId, {}, [this, userId, version](const QJsonValue &, const QString &error) {
        m_socialBusy = false; m_socialStatus.clear();
        if (!error.isEmpty()) m_socialError = error;
        else if (m_relationshipVersions.value(userId) == version) { acceptRelationship({{"id", userId}, {"type", 4}}); m_socialStatus = "Friend request sent."; }
        emit socialChanged();
    });
    return true;
}

QVariantMap DiscordClient::relationshipInfo(const QString &userId) const {
    const auto relationship = m_relationships.value(userId);
    const int type = relationship.value("type").toInt();
    return {{"type", type}, {"friend", type == 1}, {"incoming", type == 3}, {"outgoing", type == 4},
        {"blocked", m_blockedUsers.contains(userId)}, {"ignored", relationship.value("user_ignored").toBool()},
        {"nickname", cleanText(relationship.value("nickname").toString(), 32)}};
}

bool DiscordClient::setFriendNickname(const QString &userId, const QString &input) {
    if (!socialReady()) return false;
    const auto nickname = input.trimmed();
    if (!validId(userId) || m_relationships.value(userId).value("type").toInt() != 1 ||
        nickname.size() > 32 || cleanText(nickname, 32) != nickname || nickname.contains('\n') || nickname.contains('\t')) {
        m_socialError = "Choose a friend and enter a nickname of at most 32 characters."; emit socialChanged(); return false;
    }
    const auto version = m_relationshipVersions.value(userId);
    m_socialBusy = true; m_socialError.clear(); m_socialStatus = "Saving nickname…"; emit socialChanged();
    request("PATCH", "/users/@me/relationships/" + userId,
        {{"nickname", nickname.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(nickname)}},
        [this, userId, version, nickname](const QJsonValue &, const QString &error) {
        m_socialBusy = false; m_socialStatus.clear();
        if (!error.isEmpty()) m_socialError = error;
        else if (m_relationshipVersions.value(userId) == version) acceptRelationship({{"id", userId}, {"nickname", nickname}});
        emit socialChanged();
    });
    return true;
}

bool DiscordClient::setUserIgnored(const QString &userId, bool ignored) {
    if (!socialReady()) return false;
    if (!validId(userId) || userId == m_currentUserId) return false;
    if (m_relationships.value(userId).value("user_ignored").toBool() == ignored) return true;
    const auto version = m_relationshipVersions.value(userId);
    m_socialBusy = true; m_socialError.clear(); m_socialStatus = "Saving…"; emit socialChanged();
    request(ignored ? "PUT" : "DELETE", "/users/@me/relationships/" + userId + "/ignore", {},
        [this, userId, version, ignored](const QJsonValue &, const QString &error) {
        m_socialBusy = false; m_socialStatus.clear();
        if (!error.isEmpty()) m_socialError = error;
        else if (m_relationshipVersions.value(userId) == version) acceptRelationship({{"id", userId}, {"user_ignored", ignored}});
        emit socialChanged();
    });
    return true;
}

bool DiscordClient::fetchUserNote(const QString &userId) {
    if (!validId(userId) || m_demoMode || !m_connected || m_token.isEmpty() || m_token.startsWith("Bot ")) return false;
    const auto version = m_userNoteVersions.value(userId);
    request("GET", "/users/@me/notes/" + userId, {}, [this, userId, version](const QJsonValue &value, const QString &error) {
        if (m_userNoteVersions.value(userId) != version) return;
        if (!error.isEmpty()) { m_socialError = error; emit socialChanged(); return; }
        const auto object = value.toObject();
        if (!object.value("note").isString() && !object.value("note").isNull()) { m_socialError = "Discord did not return a readable user note."; emit socialChanged(); return; }
        const auto note = cleanText(object.value("note").toString(), 256);
        if (m_userNotes.contains(userId) || m_userNotes.size() < 5000) m_userNotes.insert(userId, note);
        emit userNoteLoaded(userId, note); emit socialChanged();
    });
    return true;
}

bool DiscordClient::setUserNote(const QString &userId, const QString &note) {
    if (!socialReady()) return false;
    if (!validId(userId) || note.size() > 256 || cleanText(note, 256) != note) {
        m_socialError = "User notes can contain at most 256 characters."; emit socialChanged(); return false;
    }
    const auto version = m_userNoteVersions.value(userId);
    m_socialBusy = true; m_socialError.clear(); m_socialStatus = "Saving note…"; emit socialChanged();
    request("PUT", "/users/@me/notes/" + userId, {{"note", note}},
        [this, userId, version, note](const QJsonValue &, const QString &error) {
        m_socialBusy = false; m_socialStatus.clear();
        if (!error.isEmpty()) m_socialError = error;
        else if (m_userNoteVersions.value(userId) == version) {
            if (m_userNotes.contains(userId) || m_userNotes.size() < 5000) m_userNotes.insert(userId, note);
            m_userNoteVersions.insert(userId, ++m_socialVersion); emit userNoteLoaded(userId, note);
        }
        emit socialChanged();
    });
    return true;
}

bool DiscordClient::setPresenceStatus(const QString &status) {
    static const QSet<QString> statuses{"online", "idle", "dnd", "invisible"};
    if (!statuses.contains(status)) return false;
    if (m_demoMode) { m_currentPresenceStatus = status; emit stateChanged(); return true; }
    if (!m_connected || m_token.isEmpty() || m_token.startsWith("Bot ") || m_socket.state() != QAbstractSocket::ConnectedState) {
        setError("Reconnect before changing your presence."); return false;
    }
    const QJsonObject presence{{"status", status}, {"since", status == "idle" ? QJsonValue(nowMs()) : QJsonValue(QJsonValue::Null)},
        {"activities", QJsonArray{}}, {"afk", status == "idle"}};
    if (!sendGateway(3, presence)) { setError("Discord did not accept the presence update. Try again after reconnecting."); return false; }
    // Gateway presence updates have no acknowledgement packet. Publish only
    // after the local transport accepts it; a later self-presence event wins.
    m_currentPresenceStatus = status;
    auto ownPresence = m_userPresence.value(m_currentUserId);
    ownPresence.insert("status", status == "invisible" ? "offline" : status);
    m_userPresence.insert(m_currentUserId, ownPresence);
    emit stateChanged(); return true;
}

bool DiscordClient::previewMessageRequest(const QString &channelId) {
    if (channelId.isEmpty()) { clearRequestPreview(); return true; }
    const auto channel = m_dmObjects.value(channelId);
    if (!pendingMessageRequest(channel) || m_blockedUsers.contains(dmRecipientId(channel))) return false;
    QVariantMap selected;
    for (const auto &rows : {m_messageRequests, m_spamRequests})
        for (const auto &row : rows) if (row.toMap().value("channelId").toString() == channelId) selected = row.toMap();
    if (selected.isEmpty()) return false;
    const auto generation = ++m_requestPreviewGeneration;
    m_selectedMessageRequest = selected; m_requestPreviewMessages.clear(); m_requestPreviewLoading = true;
    m_socialError.clear(); emit socialChanged();
    if (m_demoMode) {
        m_requestPreviewMessages.append(QVariantMap{{"recordId", "9900001"}, {"author", selected.value("name")},
            {"content", "Hi! Would you like to chat?"}, {"timestamp", "Demo message"}});
        m_requestPreviewLoading = false; emit socialChanged(); return true;
    }
    if (!m_connected || m_token.isEmpty()) { m_requestPreviewLoading = false; m_socialError = "Reconnect to preview this message request."; emit socialChanged(); return false; }
    request("GET", "/channels/" + channelId + "/messages?limit=50", {},
        [this, channelId, generation](const QJsonValue &value, const QString &error) {
        if (generation != m_requestPreviewGeneration || m_selectedMessageRequest.value("channelId").toString() != channelId) return;
        m_requestPreviewLoading = false;
        if (!error.isEmpty()) m_socialError = error;
        else if (!value.isArray()) m_socialError = "Discord did not return a readable request preview.";
        else {
            QVector<QJsonObject> ordered;
            for (const auto &entry : value.toArray()) {
                const auto message = entry.toObject();
                if (ordered.size() >= 50) break;
                if (validId(message.value("id").toString()) && message.value("channel_id").toString(channelId) == channelId) ordered.append(message);
            }
            std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) { return newerSnowflake(b.value("id").toString(), a.value("id").toString()); });
            for (auto message : ordered) {
                message.insert("channel_id", channelId);
                auto row = messageRow(message); row.insert("components", QVariantList{});
                m_requestPreviewMessages.append(row);
            }
        }
        emit socialChanged();
    });
    return true;
}

bool DiscordClient::changeMessageRequest(const QString &channelId, bool accept) {
    if (!socialReady()) return false;
    const auto channel = m_dmObjects.value(channelId);
    if (!validId(channelId) || !pendingMessageRequest(channel) || m_blockedUsers.contains(dmRecipientId(channel))) {
        m_socialError = "This message request is no longer available. Refresh the list first."; emit socialChanged(); return false;
    }
    const auto version = m_dmSocialVersions.value(channelId);
    m_socialBusy = true; m_socialError.clear(); m_socialStatus = "Saving…"; emit socialChanged();
    // Consent routes are used by Discord's DM request implementation, not
    // friendship routes. Accepting a DM does not add its sender as a friend.
    request(accept ? "PUT" : "DELETE", "/channels/" + channelId + "/recipients/@me",
        accept ? QJsonObject{{"consent_status", 2}} : QJsonObject{},
        [this, channelId, version, accept](const QJsonValue &, const QString &error) {
        m_socialBusy = false; m_socialStatus.clear();
        if (!error.isEmpty()) { m_socialError = error; emit socialChanged(); return; }
        if (m_dmSocialVersions.value(channelId) == version) {
            if (accept) acceptDm({{"id", channelId}, {"is_message_request", false}});
            else {
                m_dmObjects.remove(channelId); m_dmReadMessageIds.remove(channelId); m_unread.remove(channelId);
                m_dmSocialVersions.insert(channelId, ++m_socialVersion);
                m_dms.remove(channelId); if (m_currentGuildId.isEmpty()) m_channels.remove(channelId);
                rebuildSocial();
            }
        }
        if (accept && m_dmObjects.contains(channelId) && !pendingMessageRequest(m_dmObjects.value(channelId))) activateChannel(channelId);
        emit socialChanged();
    });
    return true;
}

bool DiscordClient::acceptMessageRequest(const QString &channelId) { return changeMessageRequest(channelId, true); }
bool DiscordClient::dismissMessageRequest(const QString &channelId) { return changeMessageRequest(channelId, false); }

void DiscordClient::acceptPresence(const QString &guildId, const QJsonObject &presence) {
    const auto user = presence.value("user").toObject();
    const auto id = user.value("id").toString(presence.value("user_id").toString());
    if ((!guildId.isEmpty() && !validId(guildId)) || !validId(id)) return;
    rememberMember(QJsonObject{{"user", user}});
    auto status = presence.value("status").toString();
    static const QSet<QString> statuses{"online", "idle", "dnd", "offline"};
    if (!statuses.contains(status)) status = "unknown";
    if (id == m_currentUserId && status != "unknown") {
        // Discord presents invisible to other users as offline.
        m_currentPresenceStatus = status == "offline" ? "invisible" : status;
        emit stateChanged();
    }
    QString activity;
    for (const auto &entry : presence.value("activities").toArray()) {
        const auto object = entry.toObject();
        const auto name = cleanText(object.value("name").toString(), 100);
        switch (object.value("type").toInt(-1)) {
        case 0: if (!name.isEmpty()) activity = "Playing " + name; break;
        case 1: if (!name.isEmpty()) activity = "Streaming " + name; break;
        case 2: if (!name.isEmpty()) activity = "Listening to " + name; break;
        case 3: if (!name.isEmpty()) activity = "Watching " + name; break;
        case 4: activity = cleanText(object.value("state").toString(), 120); break;
        default: break;
        }
        if (!activity.isEmpty()) break;
    }
    QJsonObject devices;
    for (const auto &key : {"desktop", "mobile", "web"}) {
        const auto device = presence.value("client_status").toObject().value(key).toString();
        if (statuses.contains(device)) devices.insert(key, device);
    }
    if (m_userPresence.contains(id) || m_userPresence.size() < 5000)
        m_userPresence.insert(id, {{"status", status}, {"activity", activity}, {"client_status", devices}});
    if (!guildId.isEmpty() && (m_presences.contains(guildId) || m_presences.size() < 1000)) {
        auto &presences = m_presences[guildId];
        qsizetype total = 0;
        for (auto it = m_presences.cbegin(); it != m_presences.cend(); ++it) total += it.value().size();
        if (presences.contains(id) || (presences.size() < 500 && total < 5000)) presences.insert(id, status);
    }
    for (auto it = m_dmObjects.cbegin(); it != m_dmObjects.cend(); ++it) {
        bool matches = it.value().value("recipient_ids").toArray().contains(id);
        for (const auto &entry : it.value().value("recipients").toArray())
            if (entry.toObject().value("id").toString() == id) matches = true;
        if (matches) syncDm(it.key());
    }
    if (m_relationships.contains(id)) rebuildSocial();
}

void DiscordClient::updateGuildActivity(const QString &guildId) {
    const auto states = m_voiceStates.value(guildId);
    const bool streaming = std::any_of(states.cbegin(), states.cend(), [](const auto &state) {
        return state.value("streaming").toBool();
    });
    m_guilds.patch(guildId, {{"voiceActive", !states.isEmpty()}, {"streaming", streaming}});
}

QString DiscordClient::memberListId(const QString &channelId) const {
    QJsonObject channel;
    for (const auto &entry : m_channelCache.value(m_currentGuildId))
        if (entry.toObject().value("id").toString() == channelId) { channel = entry.toObject(); break; }
    if (channel.isEmpty()) return {};
    if (m_guildObjects.value(m_currentGuildId).value("roles").toArray().isEmpty()) return {};
    // Discord's user-client member-list identity is determined by view-channel
    // overwrites. Verified against discord.py-self's GuildChannel.member_list_id:
    // https://github.com/dolfies/discord.py-self/blob/master/discord/abc.py
    constexpr quint64 ViewChannel = 1 << 10;
    bool everyone = false;
    for (const auto &entry : m_guildObjects.value(m_currentGuildId).value("roles").toArray()) {
        const auto role = entry.toObject();
        if (role.value("id").toString() == m_currentGuildId)
            everyone = (role.value("permissions").toVariant().toULongLong() & ViewChannel) != 0;
    }
    QStringList overwrites;
    for (const auto &entry : channel.value("permission_overwrites").toArray()) {
        const auto overwrite = entry.toObject();
        const auto allow = overwrite.value("allow").toVariant().toULongLong();
        const auto deny = overwrite.value("deny").toVariant().toULongLong();
        if (deny & ViewChannel) everyone = false;
        if (allow & ViewChannel) overwrites.append("allow:" + overwrite.value("id").toString());
        else if (deny & ViewChannel) overwrites.append("deny:" + overwrite.value("id").toString());
    }
    if (everyone) return "everyone";
    std::sort(overwrites.begin(), overwrites.end());
    const auto bytes = overwrites.join(',').toUtf8();
    // MurmurHash3 x86_32, seed zero. Wire format requires an unsigned decimal string.
    auto rotate = [](quint32 x, int n) { return (x << n) | (x >> (32 - n)); };
    auto mix = [rotate](quint32 x) { x *= 0xcc9e2d51u; x = rotate(x, 15); return x * 0x1b873593u; };
    quint32 hash = 0;
    qsizetype at = 0;
    for (; at + 4 <= bytes.size(); at += 4) {
        hash ^= mix(qFromLittleEndian<quint32>(bytes.constData() + at));
        hash = rotate(hash, 13) * 5 + 0xe6546b64u;
    }
    quint32 tail = 0;
    for (int i = 0; at + i < bytes.size(); ++i) tail |= quint32(quint8(bytes[at + i])) << (i * 8);
    if (at < bytes.size()) hash ^= mix(tail);
    hash ^= quint32(bytes.size());
    hash ^= hash >> 16; hash *= 0x85ebca6bu; hash ^= hash >> 13; hash *= 0xc2b2ae35u; hash ^= hash >> 16;
    return QString::number(hash);
}

QJsonObject DiscordClient::memberSubscription() const {
    // Bound the visible list to two 100-slot ranges, including group headings.
    QJsonObject channels;
    if (!m_subscribedChannel.isEmpty())
        channels.insert(m_subscribedChannel, QJsonArray{QJsonArray{0, 99}, QJsonArray{100, 199}});
    return {{"guild_id", m_subscribedGuild}, {"typing", true}, {"activities", false},
            {"threads", false}, {"channels", channels}};
}

void DiscordClient::subscribeMembers() {
    QString channel = m_currentChannelId;
    if (m_currentChannelKind == "thread") channel = cachedChannel(m_currentGuildId, channel).value("parent_id").toString();
    if (m_currentChannelKind == "hidden" || m_currentGuildId.isEmpty() || !validId(channel)) channel.clear();
    if (m_subscribedGuild == m_currentGuildId && m_subscribedChannel == channel) return;
    if (m_connected && !m_demoMode && !m_subscribedGuild.isEmpty())
        sendGateway(14, QJsonObject{{"guild_id", m_subscribedGuild}, {"channels", QJsonObject{}}});
    m_subscribedGuild = m_currentGuildId;
    m_subscribedChannel = channel;
    m_expectedMemberList = memberListId(channel);
    m_memberSlots.clear();
    m_memberListReceived = false;
    m_memberDeadline.stop();
    m_membersLoading = m_connected && !m_demoMode && !m_subscribedGuild.isEmpty() && !channel.isEmpty();
    if (m_membersLoading) {
        sendGateway(14, memberSubscription());
        m_memberDeadline.start(10000);
    }
    rebuildMembers();
}

void DiscordClient::acceptMemberList(const QJsonObject &data) {
    if (data.value("guild_id").toString() != m_currentGuildId || m_currentGuildId.isEmpty() ||
        m_subscribedChannel.isEmpty()) return;
    const auto list = data.value("id").toString();
    if (!list.isEmpty() && !m_expectedMemberList.isEmpty() && list != m_expectedMemberList) return;
    if (data.contains("member_count")) m_guildObjects[m_currentGuildId].insert("member_count", std::max(0, data.value("member_count").toInt()));
    int processed = 0;
    for (const auto &entry : data.value("ops").toArray()) {
        if (++processed > 400) break;
        const auto op = entry.toObject();
        const auto type = op.value("op").toString();
        const auto range = op.value("range").toArray();
        if ((type == "SYNC" || type == "INVALIDATE") && range.size() == 2) {
            const int start = range[0].toInt(-1), end = range[1].toInt(-1);
            if (start < 0 || end < start || start >= 200) continue;
            for (int i = start; i <= std::min(199, end); ++i) m_memberSlots.remove(i);
            if (type == "SYNC") {
                const auto items = op.value("items").toArray();
                for (int i = 0; i < items.size() && start + i <= end && start + i < 200; ++i)
                    m_memberSlots.insert(start + i, items[i].toObject());
                m_memberListReceived = true;
            }
        } else {
            const int index = op.value("index").toInt(-1);
            if (index < 0 || index >= 200) continue;
            if (type == "INSERT") {
                for (int i = 199; i > index; --i)
                    if (m_memberSlots.contains(i - 1)) m_memberSlots[i] = m_memberSlots.value(i - 1); else m_memberSlots.remove(i);
                m_memberSlots[index] = op.value("item").toObject();
            } else if (type == "DELETE") {
                for (int i = index; i < 199; ++i)
                    if (m_memberSlots.contains(i + 1)) m_memberSlots[i] = m_memberSlots.value(i + 1); else m_memberSlots.remove(i);
                m_memberSlots.remove(199);
            } else if (type == "UPDATE") m_memberSlots[index] = op.value("item").toObject();
        }
    }
    for (const auto &item : std::as_const(m_memberSlots))
        if (item.value("member").isObject()) cacheMember(m_currentGuildId, item.value("member").toObject());
    if (m_memberListReceived) { m_membersLoading = false; m_memberDeadline.stop(); }
    rebuildMembers();
}

void DiscordClient::rebuildMembers() {
    QVector<QVariantMap> rows;
    if (m_currentGuildId.isEmpty()) {
        const auto dm = m_dmObjects.value(m_currentChannelId);
        auto recipients = dm.value("recipients").toArray();
        if (recipients.isEmpty()) for (const auto &id : dm.value("recipient_ids").toArray()) {
            auto user = m_users.value(id.toString()); user.insert("id",id); recipients.append(user);
        }
        for (const auto &entry : recipients) {
            if (rows.size() >= m_members.maximumRows()) break;
            const auto id = entry.toObject().value("id").toString();
            if (!validId(id)) continue;
            const auto user = merged(entry.toObject(),m_users.value(id));
            const auto name = displayName(user);
            auto row = memberPresentation({}, user, {});
            const auto indicators = userIndicators(id);
            for (auto it = indicators.cbegin(); it != indicators.cend(); ++it) row.insert(it.key(), it.value());
            row.insert("recordId",id); row.insert("name",name); row.insert("initials",initials(name));
            row.insert("avatarUrl",avatarUrl(user)); row.insert("kind","member"); row.insert("bot",user.value("bot").toBool());
            row.insert("primaryGuild",primaryGuild(user)); row.insert("status",m_userPresence.value(id).value("status").toString("unknown"));
            row.insert("conversationMuted",conversationMuted(m_currentChannelId)); rows.append(row);
        }
        m_members.reconcile(std::move(rows)); m_memberCount=recipients.size(); m_membersPartial=m_members.count()<m_memberCount;
        m_membersLoading=false; m_membersStatus=m_currentChannelId.isEmpty() ? "Choose a conversation" : QString("%1 participants").arg(m_memberCount);
        emit stateChanged(); return;
    }
    m_memberCount = std::max(0, m_guildObjects.value(m_currentGuildId).value("member_count").toInt());
    auto records = m_guildMembers.value(m_currentGuildId);
    QStringList ids;
    if (m_memberListReceived) {
        for (const auto &item : std::as_const(m_memberSlots)) {
            const auto member = item.value("member").toObject();
            const auto id = member.value("user").toObject().value("id").toString(member.value("user_id").toString());
            if (validId(id) && !ids.contains(id)) ids.append(id);
        }
    } else ids = records.keys();
    for (const auto &id : ids) {
        if (rows.size() >= m_members.maximumRows()) break;
        const auto member = records.value(id);
        auto user = merged(member.value("user").toObject(), m_users.value(id));
        auto row = memberPresentation(m_currentGuildId, user, member);
        const auto voice = m_voiceStates.value(m_currentGuildId).value(id);
        row.insert("kind", "member"); row.insert("status", m_presences.value(m_currentGuildId).value(id, "unknown"));
        row.insert("channelId", voice.value("channel_id").toString()); row.insert("streaming", voice.value("streaming").toBool());
        row.insert("camera", voice.value("camera").toBool());
        row.insert("muted", voice.value("muted").toBool()); row.insert("deafened", voice.value("deafened").toBool());
        row.insert("speaking", m_currentGuildId == m_voiceGuildId && m_voiceMembers.byId(id).value("speaking").toBool());
        rows.append(row);
    }
    for (auto &row : rows) {
        const auto indicators=userIndicators(row.value("recordId").toString());
        for (const auto &key : {"desktopStatus","mobileStatus","webStatus","blocked","inVoice"}) row.insert(key,indicators.value(key));
    }
    if (!m_memberListReceived) std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return QString::localeAwareCompare(a.value("name").toString(), b.value("name").toString()) < 0;
    });
    m_members.reconcile(std::move(rows));
    m_membersPartial = !m_memberListReceived || m_memberCount <= 0 || m_members.count() < m_memberCount;
    if (m_currentGuildId.isEmpty()) m_membersStatus = "Choose a server to view members";
    else if (m_membersLoading) m_membersStatus = "Loading members…";
    else if (!m_memberListReceived) m_membersStatus = m_members.count() ? "Known members · full list unavailable" : "Member list unavailable for this channel";
    else if (m_membersPartial) m_membersStatus = QString("Showing %1 members · partial list").arg(m_members.count());
    else m_membersStatus = QString("%1 members").arg(m_members.count());
    emit stateChanged();
}

QVariantMap DiscordClient::memberInfo(const QString &userId) const {
    return memberInfoForGuild(userId, m_currentGuildId);
}

QVariantMap DiscordClient::memberInfoForGuild(const QString &userId, const QString &guildId) const {
    if (!validId(userId) || (!guildId.isEmpty() && !validId(guildId))) return {};
    auto member = m_guildMembers.value(guildId).value(userId);
    QJsonObject user = m_users.value(userId);
    if (!member.isEmpty()) user = merged(member.value("user").toObject(), m_users.value(userId));
    if (guildId.isEmpty()) {
        const auto dm = m_dmObjects.value(m_currentChannelId);
        for (const auto &entry : dm.value("recipients").toArray())
            if (entry.toObject().value("id").toString() == userId) user = merged(entry.toObject(), m_users.value(userId));
        if (dm.value("recipient_ids").toArray().contains(userId)) user = m_users.value(userId);
    }
    if (userId == m_currentUserId) user = m_users.value(userId);
    if (user.isEmpty() || (!guildId.isEmpty() && member.isEmpty())) for (const auto &message : m_messageObjects) {
        const auto messageGuild = messageRow(message).value("guildId").toString();
        if (messageGuild != guildId) continue;
        const auto author = message.value("author").toObject();
        if (author.value("id").toString() == userId) {
            user = merged(author, m_users.value(userId));
            member = message.value("member").toObject();
            break;
        }
    }
    if (user.isEmpty()) return {};
    QVariantMap result = memberPresentation(guildId, user, member);
    result.insert("status", m_presences.value(guildId).value(userId, "unknown"));
    result.insert("username", cleanText(user.value("username").toString(), 128));
    result.insert("primaryGuild", primaryGuild(user));
    result.insert("bio", cleanText(user.value("bio").toString(), 1000));
    const auto banner = user.value("banner").toString();
    result.insert("bannerUrl", QRegularExpression("^[a-zA-Z0-9_]{1,128}$").match(banner).hasMatch() ?
        QString("https://cdn.discordapp.com/banners/%1/%2.png?size=4096").arg(userId, banner) : QString{});
    result.insert("roleIds", member.value("roles").toArray().toVariantList());
    const auto voice = m_voiceStates.value(guildId).value(userId);
    const auto channelId = voice.value("channel_id").toString();
    result.insert("channelId", channelId);
    result.insert("serverMuted",voice.value("server_muted").toBool());
    result.insert("serverDeafened",voice.value("server_deafened").toBool());
    result.insert("streaming", voice.value("streaming").toBool());
    result.insert("camera", voice.value("camera").toBool());
    result.insert("streamKey", voice.value("streaming").toBool() ? "guild:" + guildId + ":" + channelId + ":" + userId : QString{});
    QString channelName;
    for (const auto &entry : m_channelCache.value(guildId))
        if (entry.toObject().value("id").toString() == channelId) channelName = cleanText(entry.toObject().value("name").toString(), 128);
    result.insert("channelName", channelName);
    const auto indicators=userIndicators(userId);
    for (const auto &field : {"desktopStatus", "mobileStatus", "webStatus", "blocked", "inVoice"}) result.insert(field, indicators.value(field));
    if (guildId.isEmpty()) result.insert("camera", indicators.value("camera"));
    result.insert("muted", voice.value("muted").toBool()); result.insert("deafened", voice.value("deafened").toBool());
    if (guildId.isEmpty()) result.insert("status",m_userPresence.value(userId).value("status").toString("unknown"));
    result.insert("pronouns",cleanText(user.value("pronouns").toString(),100));
    bool valid=false; const quint64 snowflake=userId.toULongLong(&valid);
    if (valid) result.insert("createdAt",QDateTime::fromMSecsSinceEpoch(qint64(snowflake >> 22)+1420070400000LL,QTimeZone::UTC).toString(Qt::ISODate));
    const auto key=userId+":"+guildId;
    const auto profile=m_profiles.value(key);
    for (auto it=profile.cbegin(); it!=profile.cend(); ++it) result.insert(it.key(),it.value());
    result.insert("profileLoading",m_profilesLoading.contains(key));
    if (!result.contains("profileError")) result.insert("profileError",QString{});
    return result;
}

void DiscordClient::fetchProfile(const QString &userId) {
    fetchProfileForGuild(userId, m_currentGuildId);
}

void DiscordClient::fetchProfileForGuild(const QString &userId, const QString &guildId) {
    if (!validId(userId) || (!guildId.isEmpty() && (!validId(guildId) || !m_guildObjects.contains(guildId))) || memberInfoForGuild(userId, guildId).isEmpty()) return;
    const auto key=userId+":"+guildId;
    if (m_profilesLoading.contains(key)) return;
    const qint64 fetched=m_profileFetchedAt.value(key);
    if (fetched>0 && nowMs()-fetched<300000) { emit profileUpdated(userId); return; }
    if (!m_connected || m_token.isEmpty() || m_demoMode) { emit profileUpdated(userId); return; }
    if (m_profilesLoading.size()>=4) return;
    // Only explicitly opened profiles are fetched. Cache display fields, never
    // account credentials, connected accounts, private flags or raw responses.
    while (m_profiles.size()>=128 && !m_profiles.contains(key)) {
        auto oldest=m_profiles.begin();
        for (auto it=m_profiles.begin(); it!=m_profiles.end(); ++it)
            if (m_profileFetchedAt.value(it.key())<m_profileFetchedAt.value(oldest.key())) oldest=it;
        m_profileFetchedAt.remove(oldest.key()); m_profiles.erase(oldest);
    }
    m_profiles[key].remove("profileError"); m_profilesLoading.insert(key); emit profileUpdated(userId);
    QString path="/users/"+userId+"/profile?with_mutual_guilds=true&with_mutual_friends=false&with_mutual_friends_count=true";
    if (!guildId.isEmpty()) path+="&guild_id="+guildId;
    request("GET",path,{},[this,userId,guildId,key](const QJsonValue &value,const QString &error) {
        m_profilesLoading.remove(key);
        if (!guildId.isEmpty() && !m_guildObjects.contains(guildId)) return;
        const auto profile=value.toObject();
        if (!error.isEmpty()) m_profiles[key].insert("profileError",cleanText(error,240));
        else if (profile.value("user").toObject().value("id").toString()!=userId)
            m_profiles[key].insert("profileError","Discord returned an incomplete profile. Try again.");
        else acceptProfile(userId,guildId,profile);
        while (m_profiles.size()>128) {
            auto oldest=m_profiles.begin();
            for (auto it=m_profiles.begin(); it!=m_profiles.end(); ++it)
                if (m_profileFetchedAt.value(it.key())<m_profileFetchedAt.value(oldest.key())) oldest=it;
            m_profileFetchedAt.remove(oldest.key()); m_profiles.erase(oldest);
        }
        emit profileUpdated(userId);
    });
}

void DiscordClient::acceptProfile(const QString &userId,const QString &guildId,const QJsonObject &profile) {
    if (!validId(userId) || profile.value("user").toObject().value("id").toString()!=userId) return;
    QJsonObject user;
    const auto raw=profile.value("user").toObject();
    for (const auto &key : {"id","username","global_name","avatar","banner","bio","pronouns","bot","primary_guild","discriminator"}) {
        const auto value=raw.value(key);
        if (value.isString()) user.insert(key,cleanText(value.toString(),QString(key)=="bio"?1000:128));
        else if (value.isBool() || value.isNull()) user.insert(key,value);
        else if (QString(key)=="primary_guild" && value.isObject()) {
            QJsonObject tag;
            for (const auto &field : {"identity_guild_id","identity_enabled","tag","badge"}) {
                const auto entry=value.toObject().value(field);
                if (entry.isString()) tag.insert(field,cleanText(entry.toString(),128));
                else if (entry.isBool()) tag.insert(field,entry);
            }
            user.insert(key,tag);
        }
    }
    if (raw.value("public_flags").isDouble()) user.insert("public_flags", raw.value("public_flags").toInteger() & (1LL << 16));
    rememberMember(QJsonObject{{"user",user}});
    auto metadata=profile.value("user_profile").toObject();
    const auto guildMetadata=profile.value("guild_member_profile").toObject();
    const bool guildBanner=!guildId.isEmpty() && guildMetadata.contains("banner");
    if (!guildId.isEmpty()) {
        for (const auto &key : {"bio","pronouns","banner"}) if (guildMetadata.contains(key)) metadata.insert(key,guildMetadata.value(key));
        auto member=profile.value("guild_member").toObject();
        if (!member.isEmpty()) { member.insert("user",user); cacheMember(guildId,member); }
    }
    QVariantMap display{{"bio",cleanText(metadata.value("bio").toString(user.value("bio").toString()),1000)},
        {"pronouns",cleanText(metadata.value("pronouns").toString(user.value("pronouns").toString()),100)}, {"profileError",QString{}}};
    const auto banner=metadata.value("banner").toString(user.value("banner").toString());
    if (QRegularExpression("^[a-zA-Z0-9_]{1,128}$").match(banner).hasMatch()) {
        display.insert("bannerUrl",guildBanner ? QString("https://cdn.discordapp.com/guilds/%1/users/%2/banners/%3.png?size=4096").arg(guildId,userId,banner)
            : QString("https://cdn.discordapp.com/banners/%1/%2.png?size=4096").arg(userId,banner));
    } else display.insert("bannerUrl",QString{});
    if (profile.value("mutual_guilds").isArray()) {
        const auto guilds=profile.value("mutual_guilds").toArray(); QVariantList mutual;
        for (const auto &entry : guilds) {
            if (mutual.size()>=1000) break;
            const auto id=entry.toObject().value("id").toString(); if (!validId(id)) continue;
            const auto info=guildInfo(id); mutual.append(QVariantMap{{"id",id},{"name",info.value("name")},{"iconUrl",info.value("iconUrl")}});
        }
        display.insert("mutualGuilds",mutual); display.insert("mutualGuildCount",mutual.size());
    }
    if (profile.value("mutual_friends_count").isDouble()) {
        const auto count=profile.value("mutual_friends_count").toInteger(-1);
        if (count>=0 && count<=1000000) display.insert("mutualFriendCount",count);
    }
    const auto key=userId+":"+guildId;
    while (m_profiles.size()>=128 && !m_profiles.contains(key)) {
        auto oldest=m_profiles.begin();
        for (auto it=m_profiles.begin(); it!=m_profiles.end(); ++it)
            if (m_profileFetchedAt.value(it.key())<m_profileFetchedAt.value(oldest.key())) oldest=it;
        m_profileFetchedAt.remove(oldest.key()); m_profiles.erase(oldest);
    }
    m_profiles.insert(key,display); m_profileFetchedAt.insert(key,nowMs());
    rebuildMembers(); rebuildVoiceMembers();
    for (auto it=m_dmObjects.cbegin(); it!=m_dmObjects.cend(); ++it) syncDm(it.key());
}

void DiscordClient::requestChannelInfo() {
    if (!m_connected || m_demoMode || !validId(m_currentGuildId)) return;
    if (nowMs()-m_channelInfoRequestedAt.value(m_currentGuildId)<30000) return;
    if (m_channelInfoRequestedAt.size()>=1000 && !m_channelInfoRequestedAt.contains(m_currentGuildId)) m_channelInfoRequestedAt.erase(m_channelInfoRequestedAt.begin());
    m_channelInfoRequestedAt.insert(m_currentGuildId,nowMs());
    // Official gateway opcode 43 returns server-owned voice session timestamps.
    sendGateway(43,QJsonObject{{"guild_id",m_currentGuildId},{"fields",QJsonArray{"voice_start_time"}}});
}

void DiscordClient::acceptChannelInfo(const QString &guildId,const QJsonObject &channel) {
    const auto id=channel.value("id").toString();
    if (!validId(guildId) || !validId(id) || !m_channelCache.contains(guildId) || !channel.contains("voice_start_time")) return;
    qint64 seconds=channel.value("voice_start_time").toInteger();
    if (seconds<1420070400 || seconds>QDateTime::currentSecsSinceEpoch()+300) seconds=0;
    auto channels=m_channelCache.value(guildId);
    for (int i=0;i<channels.size();++i) {
        auto entry=channels[i].toObject();
        if (entry.value("id").toString()!=id || (entry.value("type").toInt()!=2 && entry.value("type").toInt()!=13)) continue;
        entry.insert("voice_start_time",seconds); channels[i]=entry; m_channelCache.insert(guildId,channels);
        if (guildId==m_currentGuildId) m_channels.patch(id,{{"voiceStartedAt",seconds*1000}});
        break;
    }
}

QVariantList DiscordClient::mentionCandidates(const QString &input) const {
    auto query = cleanText(input, 128).trimmed();
    if (query.startsWith('@')) query.remove(0, 1);
    QStringList ids;
    if (!m_currentGuildId.isEmpty()) ids = m_guildMembers.value(m_currentGuildId).keys();
    else {
        const auto dm = m_dmObjects.value(m_currentChannelId);
        for (const auto &entry : dm.value("recipients").toArray()) ids.append(entry.toObject().value("id").toString());
        for (const auto &entry : dm.value("recipient_ids").toArray()) ids.append(entry.toString());
    }
    ids.removeDuplicates();
    QVector<QVariantMap> candidates;
    for (const auto &id : ids) {
        const auto info = memberInfo(id);
        if (info.isEmpty() || (!query.isEmpty() && !info.value("name").toString().contains(query, Qt::CaseInsensitive) &&
            !m_users.value(id).value("username").toString().contains(query, Qt::CaseInsensitive))) continue;
        candidates.append({{"id", id}, {"name", info.value("name")}, {"avatarUrl", info.value("avatarUrl")}, {"bot", info.value("bot")}});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [&query](const auto &a, const auto &b) {
        const auto left = a.value("name").toString(), right = b.value("name").toString();
        const bool leftStarts = left.startsWith(query, Qt::CaseInsensitive), rightStarts = right.startsWith(query, Qt::CaseInsensitive);
        if (leftStarts != rightStarts) return leftStarts;
        return QString::localeAwareCompare(left, right) < 0;
    });
    QVariantList result;
    for (const auto &candidate : candidates) { if (result.size() >= 20) break; result.append(candidate); }
    return result;
}

void DiscordClient::acceptNotificationSettings(const QJsonObject &settings) {
    const auto guildId = settings.value("guild_id").toString();
    if ((!guildId.isEmpty() && !validId(guildId)) ||
        (!m_notificationSettings.contains(guildId) && m_notificationSettings.size() >= 1001)) return;
    QJsonObject bounded;
    for (const auto &key : {"muted", "mute_config", "message_notifications", "suppress_everyone", "suppress_roles"})
        if (settings.contains(key)) bounded.insert(key, settings.value(key));
    if (settings.contains("channel_overrides")) {
        QJsonArray overrides;
        for (const auto &entry : settings.value("channel_overrides").toArray()) {
            if (overrides.size() >= 2000) break;
            const auto object = entry.toObject();
            if (!validId(object.value("channel_id").toString())) continue;
            QJsonObject item;
            for (const auto &key : {"channel_id", "muted", "mute_config", "message_notifications"})
                if (object.contains(key)) item.insert(key, object.value(key));
            overrides.append(item);
        }
        bounded.insert("channel_overrides", overrides);
    }
    m_notificationSettings.insert(guildId, merged(m_notificationSettings.value(guildId), bounded));
    if (guildId.isEmpty()) {
        for (auto it=m_dmObjects.cbegin(); it!=m_dmObjects.cend(); ++it) syncDm(it.key());
        if (m_currentGuildId.isEmpty()) rebuildMembers();
    }
}

void DiscordClient::notifyMessage(const QJsonObject &message) {
    const auto id = message.value("id").toString();
    const auto channelId = message.value("channel_id").toString();
    if (!validId(id) || !validId(channelId) || m_seenLiveMessages.contains(id)) return;
    m_seenLiveMessages.insert(id);
    m_seenLiveOrder.enqueue(id);
    while (m_seenLiveOrder.size() > 4096) m_seenLiveMessages.remove(m_seenLiveOrder.dequeue());
    const auto author = message.value("author").toObject();
    if (m_dmObjects.value(channelId).value("type").toInt() == 1 && !RelayDiscordBridge::setupSummary(message.value("content").toString()).isEmpty()) return;
    if (!m_connected || m_demoMode || author.value("id").toString() == m_currentUserId ||
        !validId(author.value("id").toString()) || m_messageObjects.contains(id) ||
        (message.value("flags").toInt() & (1 << 12))) return;
    const auto created = message.value("timestamp").toString();
    auto timestamp = QDateTime::fromString(created, Qt::ISODateWithMs);
    if (!timestamp.isValid()) timestamp = QDateTime::fromString(created, Qt::ISODate);
    const qint64 createdMs = timestamp.isValid() ? timestamp.toMSecsSinceEpoch() : qint64((id.toULongLong() >> 22) + 1420070400000ULL);
    const auto authorId = message.value("author").toObject().value("id").toString();
    if (createdMs < m_notificationStartMs || m_blockedUsers.contains(authorId) || m_relationships.value(authorId).value("user_ignored").toBool()) return;
    const auto guildId = message.value("guild_id").toString();
    const bool direct = guildId.isEmpty() && m_dmObjects.contains(channelId);
    if (direct && pendingMessageRequest(m_dmObjects.value(channelId))) return;
    if (!direct && !m_guildObjects.contains(guildId)) return;
    const auto settings = m_notificationSettings.value(guildId);
    auto isMuted = [](const QJsonObject &setting) {
        if (!setting.value("muted").toBool()) return false;
        const auto end = QDateTime::fromString(setting.value("mute_config").toObject().value("end_time").toString(), Qt::ISODateWithMs);
        return !end.isValid() || end.toMSecsSinceEpoch() > nowMs();
    };
    if (isMuted(settings)) return;
    QJsonObject channel;
    for (const auto &entry : m_channelCache.value(guildId))
        if (entry.toObject().value("id").toString() == channelId) { channel = entry.toObject(); break; }
    int level = settings.value("message_notifications").toInt(3);
    QJsonObject channelSetting;
    for (const auto &entry : settings.value("channel_overrides").toArray()) {
        const auto setting = entry.toObject();
        const auto overrideId = setting.value("channel_id").toString();
        if (overrideId == channelId) channelSetting = setting;
        else if (!channel.value("parent_id").toString().isEmpty() && overrideId == channel.value("parent_id").toString()) {
            if (isMuted(setting)) return;
            if (setting.value("message_notifications").toInt(3) != 3) level = setting.value("message_notifications").toInt();
        }
    }
    if (isMuted(channelSetting)) return;
    if (channelSetting.value("message_notifications").toInt(3) != 3) level = channelSetting.value("message_notifications").toInt();
    if (level == 3) level = direct ? 0 : m_guildObjects.value(guildId).value("default_message_notifications").toInt(1);
    bool mention = false;
    for (const auto &entry : message.value("mentions").toArray())
        if (entry.toObject().value("id").toString() == m_currentUserId) { mention = true; break; }
    if (!settings.value("suppress_everyone").toBool() && message.value("mention_everyone").toBool()) mention = true;
    if (!settings.value("suppress_roles").toBool()) {
        const auto roles = m_guildObjects.value(guildId).value("member").toObject().value("roles").toArray();
        for (const auto &entry : message.value("mention_roles").toArray())
            if (roles.contains(entry)) { mention = true; break; }
    }
    if (level == 2 || (level == 1 && !mention)) return;
    const auto row = messageRow(message);
    auto body = row.value("content").toString();
    if (body.isEmpty()) {
        if (!row.value("attachments").toList().isEmpty()) body = "Sent an attachment";
        else if (!row.value("stickers").toList().isEmpty()) body = "Sent a sticker";
        else body = "Sent a message";
    }
    const auto name = row.value("author").toString();
    const auto title = direct ? name : name + " · #" + cleanText(channel.value("name").toString("channel"), 80) + " · " + m_guilds.byId(guildId).value("name").toString();
    emit notificationRequested(channelId, cleanText(title.simplified(), 180), cleanText(body.simplified(), 320), direct, mention);
}

void DiscordClient::acceptVoiceState(const QJsonObject &state, const QString &fallbackGuild) {
    const auto guildId = state.value("guild_id").toString(fallbackGuild);
    const auto userId = state.value("user_id").toString();
    if (!validId(guildId) || !validId(userId)) return;
    rememberMember(state.value("member").toObject());
    if (!state.value("member").toObject().isEmpty()) cacheMember(guildId, state.value("member").toObject());
    const auto channelId = state.value("channel_id").toString();
    if (userId == m_currentUserId) {
        const bool pendingLeaveAck = !m_pendingVoiceState.isEmpty() &&
            m_pendingVoiceGeneration == m_generation && m_pendingVoiceAccount == m_currentUserId &&
            m_pendingVoiceState.value("guild_id").toString() == guildId &&
            m_pendingVoiceState.value("channel_id").isNull() && state.contains("channel_id") && channelId.isEmpty();
        if (pendingLeaveAck) {
            // A replayed acknowledgement can arrive before RESUMED. Remove the
            // queued leave before the voice engine issues its replacement join.
            m_pendingVoiceState = {}; m_pendingVoiceAccount.clear();
            if (!fallbackGuild.isEmpty()) {
                auto acknowledged = state; acknowledged.insert("guild_id",guildId);
                emit voiceStateReceived(acknowledged);
            }
        }
        if (channelId.isEmpty()) { m_voiceSessionId.clear(); stopWatchingStream(); }
        else if (!state.value("session_id").toString().isEmpty()) m_voiceSessionId = state.value("session_id").toString();
        if (!m_desiredStream.isEmpty() && m_desiredStream.section(':', 2, 2) != channelId) stopWatchingStream();
        if (!m_broadcastStream.isEmpty() && m_broadcastStream.section(':', 2, 2) != channelId) stopBroadcast();
    }
    const auto previous = m_voiceStates.value(guildId).value(userId);
    const auto previousChannel = previous.value("channel_id").toString();
    if (!previousChannel.isEmpty() && (channelId != previousChannel || !state.value("self_stream").toBool()))
        m_streamObjects.remove("guild:" + guildId + ":" + previousChannel + ":" + userId);
    if (!previousChannel.isEmpty() && channelId != previousChannel) {
        const auto prefix = "guild:" + guildId + ":" + previousChannel + ":";
        for (auto stream = m_streamObjects.begin(); stream != m_streamObjects.end(); ++stream) {
            if (!stream.key().startsWith(prefix)) continue;
            auto viewers = stream.value().value("viewer_ids").toArray();
            for (qsizetype i = viewers.size(); i > 0; --i)
                if (viewers.at(i - 1).toString() == userId) viewers.removeAt(i - 1);
            stream.value().insert("viewer_ids", viewers);
        }
    }
    if (channelId.isEmpty()) {
        if (m_voiceStates.contains(guildId)) m_voiceStates[guildId].remove(userId);
    } else if (validId(channelId)) {
        if (!m_voiceStates.contains(guildId) && m_voiceStates.size() >= 1000) return;
        auto &states = m_voiceStates[guildId];
        if (!states.contains(userId) && states.size() >= 1000) return;
        if (!states.contains(userId)) {
            qsizetype total = 0;
            for (auto it = m_voiceStates.cbegin(); it != m_voiceStates.cend(); ++it) total += it.value().size();
            if (total >= 5000) return;
        }
        // Keep transport session details private; display models contain only presence.
        QJsonObject display{{"user_id", userId}, {"channel_id", channelId},
            {"streaming", state.value("self_stream").toBool()},
            {"camera", state.value("self_video").toBool()},
            {"muted", state.value("self_mute").toBool() || state.value("mute").toBool()},
            {"server_muted",state.value("mute").toBool()}, {"server_deafened",state.value("deaf").toBool()},
            {"deafened", state.value("self_deaf").toBool() || state.value("deaf").toBool()},
            {"nick", cleanText(state.value("member").toObject().value("nick").toString(), 128)}};
        states.insert(userId, display);
    }
    updateGuildActivity(guildId);
    for (auto it = m_dmObjects.cbegin(); it != m_dmObjects.cend(); ++it) {
        bool matches = it.value().value("recipient_ids").toArray().contains(userId);
        for (const auto &entry : it.value().value("recipients").toArray())
            if (entry.toObject().value("id").toString() == userId) matches = true;
        if (matches) syncDm(it.key());
    }
    if (guildId == m_currentGuildId || m_currentGuildId.isEmpty()) rebuildMembers();
    if (m_relationships.contains(userId)) rebuildSocial();
}

void DiscordClient::rebuildVoiceMembers() {
    QVector<QVariantMap> rows;
    const QStringList guilds = m_voiceGuildId.isEmpty() || m_voiceGuildId == m_currentGuildId
        ? QStringList{m_currentGuildId} : QStringList{m_currentGuildId, m_voiceGuildId};
    for (const auto &guildId : guilds) {
        const auto states = m_voiceStates.value(guildId);
        for (auto it = states.cbegin(); it != states.cend(); ++it) {
            if (rows.size() >= m_voiceMembers.maximumRows()) break;
            const auto state = it.value();
            auto member = m_guildMembers.value(guildId).value(it.key());
            if (!member.contains("nick") && state.contains("nick")) member.insert("nick", state.value("nick"));
            auto user = merged(member.value("user").toObject(), m_users.value(it.key()));
            user.insert("id", it.key());
            auto row = memberPresentation(guildId, user, member);
            row.insert("channelId", state.value("channel_id").toString());
            row.insert("muted", state.value("muted").toBool()); row.insert("deafened", state.value("deafened").toBool());
            const auto previous = m_voiceMembers.byId(it.key());
            row.insert("speaking", previous.value("guildId").toString() == guildId && previous.value("speaking").toBool());
            const auto channel = state.value("channel_id").toString();
            const auto ownState = states.value(m_currentUserId);
            const auto ownKey = "guild:" + guildId + ":" + channel + ":" + m_currentUserId;
            const bool watching = it.key() != m_currentUserId && ownState.value("streaming").toBool()
                && ownState.value("channel_id").toString() == channel
                && m_streamObjects.value(ownKey).value("viewer_ids").toArray().contains(it.key());
            row.insert("watchingYourStream", watching);
            rows.append(row);
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return QString::localeAwareCompare(a.value("name").toString(), b.value("name").toString()) < 0;
    });
    for (auto &row : rows) {
        const auto state = m_voiceStates.value(row.value("guildId").toString()).value(row.value("recordId").toString());
        const bool streaming = state.value("streaming").toBool();
        row.insert("streaming", streaming);
        row.insert("camera", state.value("camera").toBool());
        row.insert("streamKey", streaming ? "guild:" + row.value("guildId").toString() + ":" + row.value("channelId").toString() + ":" + row.value("recordId").toString() : QString{});
    }
    for (auto &row : rows) {
        const auto indicators=userIndicators(row.value("recordId").toString());
        for (const auto &key : {"desktopStatus","mobileStatus","webStatus","blocked"}) row.insert(key,indicators.value(key));
    }
    m_voiceMembers.reconcile(std::move(rows));
    rebuildStreams();
}

void DiscordClient::setVoiceGuild(const QString &guildId) {
    if (m_voiceGuildId == guildId) return;
    if (guildId.isEmpty() && m_directCallStarting) { ++m_directCallOperation; m_directCallStarting = false; emit socialChanged(); }
    m_voiceGuildId = guildId;
    rebuildVoiceMembers();
}

void DiscordClient::setSpeaking(const QString &userId, bool speaking) {
    if (m_voiceMembers.byId(userId).value("guildId").toString() == m_voiceGuildId)
        m_voiceMembers.patch(userId, {{"speaking", speaking}});
    if (m_currentGuildId == m_voiceGuildId) m_members.patch(userId, {{"speaking", speaking}});
}

QJsonObject DiscordClient::cachedChannel(const QString &guildId, const QString &channelId) const {
    for (const auto &entry : m_channelCache.value(guildId))
        if (entry.toObject().value("id").toString() == channelId) return entry.toObject();
    return {};
}

void DiscordClient::cacheChannels(const QString &guildId, const QJsonArray &channels, bool retainThreads) {
    // Threads share navigation metadata with channels, but must never consume
    // the slots needed by the server's structural channel/category snapshot.
    QJsonArray candidates = channels;
    if (retainThreads) {
        QSet<QString> incoming;
        for (const auto &entry : channels) incoming.insert(entry.toObject().value("id").toString());
        for (const auto &entry : m_channelCache.value(guildId)) {
            const auto object = entry.toObject();
            if (channelKind(object.value("type").toInt(-1)) == "thread" && !incoming.contains(object.value("id").toString())) candidates.append(object);
        }
    }
    QJsonArray bounded;
    QHash<QString, int> indices;
    const auto accept = [&](const QJsonObject &object) {
        const auto id = object.value("id").toString();
        // Sparse unknown updates are metadata, not implicit type-zero channels.
        if (!validId(id) || !object.value("type").isDouble() || channelKind(object.value("type").toInt(-1)).isEmpty()) return;
        if (indices.contains(id)) return;
        if (bounded.size() >= m_channels.maximumRows()) return;
        indices.insert(id, bounded.size()); bounded.append(object);
    };
    for (const auto &entry : candidates) {
        const auto object = entry.toObject();
        if (channelKind(object.value("type").toInt(-1)) != "thread") accept(object);
    }
    if (guildId == m_currentGuildId && !m_currentChannelId.isEmpty()) {
        for (const auto &entry : candidates) {
            const auto object = entry.toObject();
            if (object.value("id").toString() == m_currentChannelId && channelKind(object.value("type").toInt(-1)) == "thread") accept(object);
        }
    }
    // Newly received/opened posts replace older thread metadata at the bound.
    for (int at = candidates.size() - 1; at >= 0; --at) {
        const auto object = candidates.at(at).toObject();
        if (channelKind(object.value("type").toInt(-1)) == "thread") accept(object);
    }
    for (const auto &entry : bounded) {
        const auto object = entry.toObject();
        noteLatestChannelMessage(object.value("id").toString(), object.value("last_message_id").toString(), guildId);
    }
    m_channelCache.insert(guildId, bounded);
    m_channelCacheOrder.removeAll(guildId);
    m_channelCacheOrder.append(guildId);
    while (m_channelCacheOrder.size() > 12) {
        auto victim = m_channelCacheOrder.takeFirst();
        if (victim == m_currentGuildId) { m_channelCacheOrder.append(victim); continue; }
        m_channelCache.remove(victim);
    }
    emit channelReferencesChanged();
}

bool DiscordClient::channelVisible(const QJsonObject &channel, const QString &guildId) const {
    if (channelKind(channel.value("type").toInt(-1)) == "thread") {
        const auto parent = cachedChannel(guildId, channel.value("parent_id").toString());
        const int type = parent.value("type").toInt(-1);
        if (type != 0 && type != 5 && type != 15 && type != 16) return false;
        return channelVisible(parent, guildId);
    }
    const auto guild = m_guildObjects.value(guildId);
    if (guild.value("owner_id").toString() == m_currentUserId) return true;
    const auto member = guild.value("member").toObject();
    const auto roles = guild.value("roles").toArray();
    // When Discord omits member role metadata, REST remains authoritative and a
    // denied message fetch is shown as a permission error rather than guessed.
    if (member.isEmpty() || roles.isEmpty()) return true;
    QSet<QString> memberRoles;
    memberRoles.insert(guildId);
    for (const auto &role : member.value("roles").toArray()) memberRoles.insert(role.toString());
    quint64 permissions = 0;
    for (const auto &value : roles) {
        const auto role = value.toObject();
        if (memberRoles.contains(role.value("id").toString()))
            permissions |= role.value("permissions").toString().toULongLong();
    }
    if (permissions & (1ULL << 3)) return true;
    quint64 roleDeny = 0, roleAllow = 0, memberDeny = 0, memberAllow = 0;
    const auto overwrites = channel.value("permission_overwrites").toArray();
    for (const auto &value : overwrites) {
        const auto overwrite = value.toObject();
        const auto id = overwrite.value("id").toString();
        const auto allow = overwrite.value("allow").toString().toULongLong();
        const auto deny = overwrite.value("deny").toString().toULongLong();
        if (id == guildId && overwrite.value("type").toInt() == 0) permissions = (permissions & ~deny) | allow;
        else if (overwrite.value("type").toInt() == 0 && memberRoles.contains(id)) {
            roleDeny |= deny;
            roleAllow |= allow;
        } else if (overwrite.value("type").toInt() == 1 && id == m_currentUserId) {
            memberDeny |= deny;
            memberAllow |= allow;
        }
    }
    permissions = (permissions & ~roleDeny) | roleAllow;
    permissions = (permissions & ~memberDeny) | memberAllow;
    return (permissions & (1ULL << 10)) != 0;
}

void DiscordClient::setShowHiddenChannels(bool enabled) {
    if (m_showHiddenChannels == enabled) return;
    m_showHiddenChannels = enabled;
    const bool wasHidden = m_currentChannelKind == "hidden";
    if (wasHidden && !enabled) clearSelection();
    rebuildChannels(wasHidden && !enabled);
    emit stateChanged();
}

void DiscordClient::rebuildChannels(bool chooseDefault) {
    if (m_currentGuildId.isEmpty()) {
        m_channels.reconcile(m_dms.rows());
    } else {
        const auto raw = m_channelCache.value(m_currentGuildId);
        QSet<QString> mutedChannels;
        if (m_hideMutedGuilds.contains(m_currentGuildId)) {
            for (const auto &entry : m_notificationSettings.value(m_currentGuildId).value("channel_overrides").toArray()) {
                const auto setting = entry.toObject();
                const auto end = QDateTime::fromString(setting.value("mute_config").toObject().value("end_time").toString(), Qt::ISODateWithMs);
                if (setting.value("muted").toBool() && (!end.isValid() || end > QDateTime::currentDateTimeUtc())) mutedChannels.insert(setting.value("channel_id").toString());
            }
        }
        QString selectedParent;
        for (const auto &entry : raw) if (entry.toObject().value("id").toString() == m_currentChannelId) selectedParent = entry.toObject().value("parent_id").toString();
        QVector<QJsonObject> categories, ordered;
        for (const auto &entry : raw) {
            const auto object = entry.toObject();
            const auto id = object.value("id").toString();
            if (channelKind(object.value("type").toInt(-1)) == "thread") continue;
            if (id != m_currentChannelId && id != selectedParent && (mutedChannels.contains(id) || mutedChannels.contains(object.value("parent_id").toString()))) continue;
            if (object.value("type").toInt() == 4) categories.append(object);
            if (m_showHiddenChannels || channelVisible(object, m_currentGuildId)) ordered.append(object);
        }
        const auto positionOrder = [](const QJsonObject &a, const QJsonObject &b) {
            const int ap = a.value("position").toInt(), bp = b.value("position").toInt();
            if (ap != bp) return ap < bp;
            return newerSnowflake(b.value("id").toString(), a.value("id").toString());
        };
        std::sort(categories.begin(), categories.end(), positionOrder);
        QHash<QString, int> categoryRanks;
        for (int i = 0; i < categories.size(); ++i) categoryRanks.insert(categories[i].value("id").toString(), i);
        const auto group = [&categoryRanks](const QJsonObject &item) {
            if (item.value("type").toInt() == 4) return categoryRanks.value(item.value("id").toString(), 100000);
            const auto parent = item.value("parent_id").toString();
            return parent.isEmpty() ? -1 : categoryRanks.value(parent, 100000);
        };
        std::stable_sort(ordered.begin(), ordered.end(), [&group, &positionOrder](const QJsonObject &a, const QJsonObject &b) {
            if (group(a) != group(b)) return group(a) < group(b);
            const int at = a.value("type").toInt(), bt = b.value("type").toInt();
            const bool ac = at == 4, bc = bt == 4;
            if (ac != bc) return ac;
            const bool av = at == 2 || at == 13, bv = bt == 2 || bt == 13;
            if (av != bv) return !av;
            return positionOrder(a, b);
        });
        QVector<QVariantMap> rows;
        const auto selected = cachedChannel(m_currentGuildId, m_currentChannelId);
        const auto selectedParentChannel = cachedChannel(m_currentGuildId, selected.value("parent_id").toString());
        const bool showSelectedThread = channelKind(selected.value("type").toInt(-1)) == "thread"
            && (selectedParentChannel.value("type").toInt(-1) == 0 || selectedParentChannel.value("type").toInt(-1) == 5)
            && channelVisible(selected, m_currentGuildId);
        for (const auto &channel : ordered) {
            rows.append(channelRow(channel, m_currentGuildId));
            if (showSelectedThread && channel.value("id") == selected.value("parent_id")) rows.append(channelRow(selected, m_currentGuildId));
        }
        m_channels.reconcile(std::move(rows));
        // Permission updates must retire the displayed conversation as well as
        // the list row. In-flight history replies use the selection generation.
        if (!m_currentChannelId.isEmpty()) {
            const auto selectedObject = cachedChannel(m_currentGuildId, m_currentChannelId);
            const auto current = channelKind(selectedObject.value("type").toInt(-1)) == "thread"
                ? channelRow(selectedObject, m_currentGuildId) : m_channels.byId(m_currentChannelId);
            const bool denied = current.value("hidden").toBool();
            if (current.isEmpty() || (denied && m_currentChannelKind != "hidden")) {
                const auto id = m_currentChannelId;
                clearSelection();
                if (denied && m_showHiddenChannels) {
                    m_currentChannelId = id;
                    m_currentChannelName = current.value("name").toString();
                    m_currentChannelKind = "hidden";
                } else chooseDefault = true;
                subscribeMembers();
                emit stateChanged();
            } else if (!denied && m_currentChannelKind == "hidden") {
                selectChannel(m_currentChannelId);
            }
        }
    }
    if (chooseDefault) {
        for (const auto &row : m_channels.rows()) {
            const auto kind = row.value("kind").toString();
            if (!row.value("hidden").toBool() && (kind == "text" || kind == "dm")) {
                selectChannel(row.value("recordId").toString());
                break;
            }
        }
        if (m_currentChannelId.isEmpty()) for (const auto &row : m_channels.rows()) {
            if (!row.value("hidden").toBool() && row.value("kind").toString() == "forum") {
                selectChannel(row.value("recordId").toString()); break;
            }
        }
    }
}

void DiscordClient::selectGuild(const QString &id) { selectGuildInternal(id,true); }

void DiscordClient::selectGuildInternal(const QString &id,bool chooseDefault) {
    if (!id.isEmpty() && m_guilds.indexOf(id) < 0) return;
    ++m_guildGeneration;
    const auto guildGeneration = m_guildGeneration;
    m_currentGuildId = id;
    m_memberSlots.clear();
    m_memberListReceived = false;
    m_membersLoading = false;
    m_memberDeadline.stop();
    rebuildVoiceMembers();
    m_currentGuildName = id.isEmpty() ? QStringLiteral("Direct messages") : m_guilds.byId(id).value("name").toString();
    m_error.clear();
    clearSelection();
    m_channels.clear();
    rebuildMembers();
    if (id.isEmpty() || m_channelCache.contains(id)) {
        rebuildChannels(chooseDefault);
    } else if (!m_demoMode && !m_token.isEmpty()) {
        request("GET", "/guilds/" + id + "/channels", {},
                [this, id, guildGeneration, chooseDefault](const QJsonValue &value, const QString &error) {
            if (!m_guildObjects.contains(id)) return;
            if (!error.isEmpty()) {
                if (guildGeneration == m_guildGeneration) setError(error);
                return;
            }
            cacheChannels(id, value.toArray(), true);
            if (id == m_currentGuildId && guildGeneration == m_guildGeneration) rebuildChannels(chooseDefault);
        });
    }
    subscribeMembers();
    requestChannelInfo();
    emit stateChanged();
}

void DiscordClient::selectChannel(const QString &id) {
    auto row = m_channels.byId(id);
    if (row.isEmpty() && !m_currentGuildId.isEmpty()) {
        const auto channel = cachedChannel(m_currentGuildId, id);
        if (channelKind(channel.value("type").toInt(-1)) == "thread" && channelVisible(channel, m_currentGuildId)) row = channelRow(channel, m_currentGuildId);
    }
    if (row.isEmpty() || (row.value("kind").toString() == "category" && !row.value("hidden").toBool())) return;
    if (row.value("hidden").toBool()) {
        clearSelection();
        m_currentChannelId = id;
        m_currentChannelName = row.value("name").toString();
        m_currentChannelKind = "hidden";
        m_error.clear();
        subscribeMembers();
        emit stateChanged();
        return;
    }
    if (id == m_currentChannelId && !m_messages.rows().isEmpty()) return;
    const bool leavingThread = m_currentChannelKind == "thread";
    clearSelection();
    m_currentChannelId = id;
    m_currentChannelName = row.value("name").toString();
    m_currentChannelTopic = row.value("topic").toString();
    m_currentChannelKind = row.value("kind").toString();
    m_error.clear();
    if (!m_currentGuildId.isEmpty() && (leavingThread || m_currentChannelKind == "thread")) rebuildChannels(false);
    if (m_currentChannelId != id) return;
    // Remote read state follows the rendered, focused tail, not selection or
    // fetching alone. acknowledgeVisibleMessages receives that viewport proof.
    if (m_demoMode) loadDemoMessages();
    else if (m_currentChannelKind != "forum") fetchMessages(false);
    subscribeMembers();
    if (m_currentGuildId.isEmpty()) rebuildMembers();
    emit stateChanged();
}

void DiscordClient::activateChannel(const QString &id) {
    if (!validId(id)) return;
    if (m_dmObjects.contains(id)) { if (!m_currentGuildId.isEmpty()) selectGuildInternal({},false); selectChannel(id); return; }
    for (auto it = m_channelCache.cbegin(); it != m_channelCache.cend(); ++it) {
        for (const auto &entry : it.value()) {
            if (entry.toObject().value("id").toString() != id) continue;
            const auto guildId = it.key();
            if (guildId != m_currentGuildId) selectGuild(guildId);
            selectChannel(id);
            return;
        }
    }
}

void DiscordClient::openVoiceChat(const QString &channelId) {
    const auto kind = m_channels.byId(channelId).value("kind").toString();
    if (kind == "voice" || kind == "stage") selectChannel(channelId);
}

bool DiscordClient::createVoiceInvite(const QString &channelId) {
    if (m_inviteBusy) return false;
    m_inviteError.clear();
    m_inviteUrl.clear();
    const auto kind = m_channels.byId(channelId).value("kind").toString();
    if (!m_connected || m_demoMode || m_token.isEmpty() || !validId(channelId) ||
        (kind != "voice" && kind != "stage") || m_channels.byId(channelId).value("hidden").toBool()) {
        m_inviteError = "Sign in and choose a voice channel to create an invite.";
        emit inviteChanged(); return false;
    }
    m_inviteBusy = true;
    emit inviteChanged();
    request("POST", "/channels/" + channelId + "/invites",
        {{"max_age", 86400}, {"max_uses", 0}, {"temporary", false}, {"unique", false}},
        [this](const QJsonValue &result, const QString &error) {
            m_inviteBusy = false;
            if (!error.isEmpty()) m_inviteError = error;
            else {
                const auto code = result.toObject().value("code").toString();
                if (QRegularExpression("^[a-zA-Z0-9_-]{1,128}$").match(code).hasMatch())
                    m_inviteUrl = "https://discord.gg/" + code;
                else m_inviteError = "Discord returned an invalid invite link.";
            }
            emit inviteChanged();
            if (!m_inviteUrl.isEmpty()) emit inviteReady(m_inviteUrl);
        });
    return true;
}

bool DiscordClient::copyInvite() {
    if (m_inviteUrl.isEmpty() || !qobject_cast<QGuiApplication *>(QCoreApplication::instance())) return false;
    auto *clipboard = QGuiApplication::clipboard();
    if (!clipboard) return false;
    clipboard->setText(m_inviteUrl);
    return true;
}

void DiscordClient::loadOlderMessages() {
    if (m_hasMoreMessages && !m_loadingMessages && !m_demoMode && m_error.isEmpty()) fetchMessages(true);
}

void DiscordClient::loadNewerMessages() {
    if (m_hasNewerMessages && !m_loadingMessages && !m_demoMode && m_error.isEmpty()) fetchMessages(false, true);
}

void DiscordClient::setFollowingMessages(bool follow) {
    m_followingMessages = follow;
}

void DiscordClient::setReadingActive(bool active) {
    m_readingActive = active;
}

void DiscordClient::jumpToPresent() {
    if (m_demoMode || !validId(m_currentChannelId) || m_token.isEmpty()) return;
    ++m_selectionGeneration;
    m_loadingMessages = false;
    m_hasMoreMessages = false;
    m_hasNewerMessages = false;
    m_followingMessages = true;
    m_deferredLiveMessages.clear();
    m_messageObjects.clear();
    m_pendingMessageUpdates.clear();
    m_messages.clear();
    m_error.clear();
    fetchMessages(false);
}

bool DiscordClient::jumpToMessage(const QString &channelId, const QString &messageId) {
    const auto reject = [this, channelId, messageId](const QString &reason) {
        QTimer::singleShot(0, this, [this, channelId, messageId, reason] {
            emit messageJumpFailed(channelId, messageId, reason);
        });
        return false;
    };
    // Replies belong to the displayed conversation. Do not turn arbitrary
    // reference metadata into a channel switch or a request for another channel.
    if (!validId(channelId) || !validId(messageId) || channelId != m_currentChannelId)
        return reject("This reply is outside the current conversation.");
    if (m_currentChannelKind == "hidden") return reject("This channel is read-only metadata; its messages are unavailable.");
    if (m_deletedMessages.contains(messageId)) return reject("The original message was deleted.");
    if (m_messages.indexOf(messageId) < 0 && (m_demoMode || m_token.isEmpty()))
        return reject("The original message is unavailable.");

    // A new jump supersedes both an earlier jump and any page currently loading.
    // Leave the displayed history intact until the requested message is found.
    const auto selection = ++m_selectionGeneration;
    m_loadingMessages = false;
    m_error.clear();
    if (m_messages.indexOf(messageId) >= 0) {
        emit stateChanged();
        QTimer::singleShot(0, this, [this, channelId, messageId, selection] {
            if (selection != m_selectionGeneration || channelId != m_currentChannelId) return;
            if (m_messages.indexOf(messageId) >= 0) emit messageJumpReady(channelId, messageId);
            else emit messageJumpFailed(channelId, messageId, "The original message is unavailable.");
        });
        return true;
    }
    m_loadingMessages = true;
    emit stateChanged();
    request("GET", "/channels/" + channelId + "/messages?limit=" + QString::number(PageSize) + "&around=" + messageId, {},
            [this, channelId, messageId, selection](const QJsonValue &value, const QString &error) {
        if (selection != m_selectionGeneration || channelId != m_currentChannelId) return;
        m_loadingMessages = false;
        QJsonArray page;
        bool found = false;
        if (value.isArray()) for (const auto &entry : value.toArray()) {
            const auto object = entry.toObject();
            const auto id = object.value("id").toString();
            if (!validId(id) || object.value("channel_id").toString() != channelId || m_deletedMessages.contains(id)) continue;
            page.append(object);
            if (id == messageId) found = true;
        }
        if (!error.isEmpty() || !found) {
            emit stateChanged();
            emit messageJumpFailed(channelId, messageId, !error.isEmpty() ? error :
                m_deletedMessages.contains(messageId) ? "The original message was deleted." : "The original message is unavailable.");
            return;
        }

        QHash<QString, QJsonObject> window;
        QString latest;
        for (const auto &entry : page) {
            const auto object = entry.toObject();
            const auto id = object.value("id").toString();
            // Current gateway data is newer than the in-flight REST snapshot.
            window.insert(id, merged(merged(merged(object, m_messageObjects.value(id)),
                m_deferredLiveMessages.take(id)), m_pendingMessageUpdates.take(id)));
            if (latest.isEmpty() || newerSnowflake(id, latest)) latest = id;
        }
        for (auto it = m_messageObjects.cbegin(); it != m_messageObjects.cend(); ++it)
            if (newerSnowflake(it.key(), latest)) m_deferredLiveMessages.insert(it.key(), it.value());
        if (m_deferredLiveMessages.size() > 100) {
            auto ids = m_deferredLiveMessages.keys();
            std::sort(ids.begin(), ids.end(), [](const QString &a, const QString &b) { return newerSnowflake(a, b); });
            for (int i = 100; i < ids.size(); ++i) m_deferredLiveMessages.remove(ids.at(i));
        }
        m_messageObjects = std::move(window);
        m_pendingMessageUpdates.clear();
        // Around pages can end at either edge. Confirm exhaustion by paging in
        // that direction instead of guessing from a split page's total size.
        m_hasMoreMessages = true;
        m_hasNewerMessages = true;
        mergeMessages({});
        emit stateChanged();
        emit messageJumpReady(channelId, messageId);
    });
    return true;
}

void DiscordClient::fetchMessages(bool older, bool newer) {
    if (!validId(m_currentChannelId) || m_loadingMessages || m_currentChannelKind == "hidden") return;
    const auto channelId = m_currentChannelId;
    const auto selection = m_selectionGeneration;
    const bool initialPage = m_messages.count() == 0;
    QString edge;
    QString path = "/channels/" + channelId + "/messages?limit=" + QString::number(PageSize);
    if ((older || newer) && m_messages.count()) {
        edge = m_messages.get(older ? 0 : m_messages.count() - 1).value("recordId").toString();
        if (!validId(edge)) return;
        path += (older ? "&before=" : "&after=") + edge;
    }
    m_loadingMessages = true;
    emit stateChanged();
    request("GET", path, {}, [this, channelId, selection, older, newer, initialPage, edge](const QJsonValue &value, const QString &error) {
        if (selection != m_selectionGeneration || channelId != m_currentChannelId) return;
        m_loadingMessages = false;
        if (!error.isEmpty()) { setError(error); return; }
        if (!value.isArray()) { setError("Discord returned an unexpected message response."); return; }
        const auto response = value.toArray();
        QJsonArray page;
        bool invalidPage = false;
        for (const auto &entry : response) {
            const auto object = entry.toObject();
            const auto id = object.value("id").toString();
            // A before/after cursor is exclusive. Never merge another channel,
            // an echoed cursor, or a page from the opposite side of the window.
            if (!validId(id) || object.value("channel_id").toString() != channelId ||
                (!edge.isEmpty() && ((older && !newerSnowflake(edge, id)) || (newer && !newerSnowflake(id, edge))))) {
                invalidPage = true;
                continue;
            }
            page.append(object);
        }
        if (invalidPage && page.isEmpty()) {
            if (older || initialPage) m_hasMoreMessages = false;
            if (newer) {
                // An echoed/incorrect after-page does not prove we reached
                // the present. Keep deferred live messages out of this window.
                m_hasNewerMessages = true;
                setError("Message history did not advance. Retry or jump to present.");
            } else emit stateChanged();
            return;
        }
        mergeMessages(page, older);
        const auto first = m_messages.get(0).value("recordId").toString();
        const auto last = m_messages.get(m_messages.count() - 1).value("recordId").toString();
        if (older || initialPage)
            m_hasMoreMessages = response.size() == PageSize && !page.isEmpty() && validId(first) &&
                (edge.isEmpty() || newerSnowflake(edge, first));
        if (newer) {
            const bool advanced = !page.isEmpty() && newerSnowflake(last, edge);
            if (invalidPage || (response.size() >= PageSize && !advanced)) {
                m_hasNewerMessages = true;
                setError("Message history did not advance. Retry or jump to present.");
                return;
            }
            m_hasNewerMessages = response.size() == PageSize && advanced;
            if (!m_hasNewerMessages) {
                const auto live = std::exchange(m_deferredLiveMessages, {});
                auto ids = live.keys();
                std::sort(ids.begin(), ids.end(), [](const QString &a, const QString &b) {
                    return a.size() == b.size() ? a < b : a.size() < b.size();
                });
                for (const auto &id : ids) acceptMessage(live.value(id));
            }
        }
        emit stateChanged();
    });
}

void DiscordClient::mergeMessages(const QJsonArray &page, bool older) {
    // Gateway events may arrive while history is in flight. Keep both, deduplicate
    // by snowflake and preserve the newer event's content for already known rows.
    for (const auto &entry : page) {
        const auto object = entry.toObject();
        const auto id = object.value("id").toString();
        noteLatestChannelMessage(object.value("channel_id").toString(), id, m_currentGuildId);
        if (validId(id) && !m_deletedMessages.contains(id) && m_deletedMessageChannels.value(id) != object.value("channel_id").toString() && !m_messageObjects.contains(id)) {
            m_messageObjects.insert(id, merged(merged(object, m_deferredLiveMessages.take(id)), m_pendingMessageUpdates.take(id)));
        }
    }
    QStringList ids = m_messageObjects.keys();
    std::sort(ids.begin(), ids.end(), [](const QString &a, const QString &b) {
        return a.size() == b.size() ? a < b : a.size() < b.size();
    });
    if (ids.size() > m_messages.maximumRows()) {
        // Slide the bounded window toward the requested page. Evicted records
        // remain available through pagination in the opposite direction.
        if (older) {
            ids = ids.first(m_messages.maximumRows());
            m_hasNewerMessages = true;
        } else {
            ids = ids.last(m_messages.maximumRows());
            m_hasMoreMessages = true;
        }
    }
    QVector<QVariantMap> rows;
    rows.reserve(ids.size());
    for (const auto &id : ids) {
        const auto existing = m_messages.byId(id);
        rows.append(existing.isEmpty() ? messageRow(m_messageObjects.value(id)) : existing);
    }
    m_messages.reconcile(std::move(rows));
    pruneMessageCache();
}

void DiscordClient::pruneMessageCache() {
    for (auto it = m_messageObjects.begin(); it != m_messageObjects.end();) {
        if (m_messages.indexOf(it.key()) < 0) it = m_messageObjects.erase(it);
        else ++it;
    }
}

void DiscordClient::acceptMessage(QJsonObject message, bool partial) {
    if (m_currentChannelKind == "hidden" && message.value("channel_id").toString() == m_currentChannelId) return;
    const auto id = message.value("id").toString();
    if (!validId(id) || m_deletedMessages.contains(id) || m_deletedMessageChannels.value(id) == m_currentChannelId || message.value("channel_id").toString() != m_currentChannelId) return;
    if (partial && m_deferredLiveMessages.contains(id)) {
        m_deferredLiveMessages[id] = merged(m_deferredLiveMessages.value(id), message);
        return;
    }
    if (!partial && m_messages.count() > 0 && !m_messageObjects.contains(id)) {
        const auto latest = m_messages.get(m_messages.count() - 1).value("recordId").toString();
        if (newerSnowflake(id, latest) && (m_hasNewerMessages ||
            (!m_followingMessages && m_messages.count() >= m_messages.maximumRows()))) {
            // Live arrivals must not evict the older messages being read.
            const bool becameOlderWindow = !m_hasNewerMessages;
            m_hasNewerMessages = true;
            m_deferredLiveMessages[id] = message;
            if (m_deferredLiveMessages.size() > 100) {
                auto ids = m_deferredLiveMessages.keys();
                std::sort(ids.begin(), ids.end(), [](const QString &a, const QString &b) {
                    return a.size() == b.size() ? a < b : a.size() < b.size();
                });
                m_deferredLiveMessages.remove(ids.first());
            }
            if (becameOlderWindow) emit stateChanged();
            return;
        }
    }
    if (partial && !m_messageObjects.contains(id)) {
        if (m_loadingMessages && m_pendingMessageUpdates.size() < m_messages.maximumRows())
            m_pendingMessageUpdates.insert(id, merged(m_pendingMessageUpdates.value(id), message));
        return;
    }
    message = merged(m_messageObjects.value(id), message);
    m_messageObjects.insert(id, message);
    if (m_messages.indexOf(id) < 0 && m_messages.count() > 0) {
        const auto latestId = m_messages.get(m_messages.count() - 1).value("recordId").toString();
        if (id.size() < latestId.size() || (id.size() == latestId.size() && id < latestId)) {
            mergeMessages({});
            return;
        }
    }
    if (m_messages.indexOf(id) < 0 && m_messages.count() == m_messages.maximumRows()) m_hasMoreMessages = true;
    m_messages.upsert(messageRow(message));
    pruneMessageCache();
}

QJsonObject DiscordClient::allowedMentions(const QString &content) {
    static const QRegularExpression mention("<@!?([0-9]{1,22})>");
    QJsonArray users;
    auto matches = mention.globalMatch(content);
    while (matches.hasNext() && users.size() < 100) {
        const auto id = matches.next().captured(1);
        if (!users.contains(id)) users.append(id);
    }
    return {{"parse", QJsonArray{}}, {"users", users}, {"replied_user", false}};
}

bool DiscordClient::sendMessage(const QString &content) {
    if (content.trimmed().isEmpty() || m_currentChannelId.isEmpty() || m_sendingMessage) return false;
    if (pendingMessageRequest(m_dmObjects.value(m_currentChannelId))) {
        setError("Accept this message request before replying."); emit messageSendFailed(content, m_error); return false;
    }
    if (content.size() > 2000) {
        setError("Keep messages to 2,000 characters in this version.");
        emit messageSendFailed(content, m_error);
        return false;
    }
    if (m_currentChannelKind == "hidden" || m_currentChannelKind == "category" || m_currentChannelKind == "forum") {
        setError("Choose a text channel or an existing thread to send a message.");
        emit messageSendFailed(content, m_error);
        return false;
    }
    if (m_demoMode) {
        const auto id = QString::number(nowMs());
        QJsonObject message{{"id", id}, {"channel_id", m_currentChannelId},
            {"content", content}, {"timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"author", QJsonObject{{"id", m_currentUserId}, {"username", m_currentUserName}}}};
        acceptMessage(message);
        emit messageSent(content);
        return true;
    }
    if (!m_connected || !validId(m_currentChannelId)) {
        setError("Wait for Discord to reconnect before sending.");
        emit messageSendFailed(content, m_error);
        return false;
    }
    m_sendingMessage = true;
    m_error.clear();
    emit stateChanged();
    const auto channelId = m_currentChannelId;
    const auto nonce = QString::number(nowMs()) + QString::number(QRandomGenerator::global()->bounded(10000)).rightJustified(4, '0');
    QJsonObject body{{"content", content}, {"nonce", nonce}, {"enforce_nonce", true}, {"tts", false},
        {"allowed_mentions", allowedMentions(content)}};
    request("POST", "/channels/" + channelId + "/messages", body,
            [this, content, channelId](const QJsonValue &value, const QString &error) {
        m_sendingMessage = false;
        if (!error.isEmpty()) {
            setError(error);
            emit messageSendFailed(content, error);
            return;
        }
        auto message = value.toObject();
        if (!validId(message.value("id").toString())) {
            const QString reason = "Discord did not confirm the message. Check the channel before resending.";
            setError(reason);
            emit messageSendFailed(content, reason);
            return;
        }
        message.insert("channel_id", channelId);
        recordDmActivity(message);
        if (channelId == m_currentChannelId) {
            if (m_hasNewerMessages) jumpToPresent();
            acceptMessage(message);
        }
        emit stateChanged();
        emit messageSent(content);
    });
    return true;
}

void DiscordClient::openDm(const QString &userId) {
    if (!validId(userId) || m_demoMode || !m_connected) return;
    request("POST", "/users/@me/channels", {{"recipient_id", userId}},
            [this](const QJsonValue &value, const QString &error) {
        if (!error.isEmpty()) { setError(error); return; }
        const auto channel = value.toObject();
        acceptDm(channel);
        selectGuild({});
        selectChannel(channel.value("id").toString());
    });
}

void DiscordClient::openDirectMessage(const QString &userId) {
    if (!validId(userId) || userId == m_currentUserId) return;
    for (auto it = m_dmObjects.cbegin(); it != m_dmObjects.cend(); ++it) {
        if (it.value().value("type").toInt() != 1) continue;
        bool recipient = it.value().value("recipient_ids").toArray().contains(userId);
        for (const auto &entry : it.value().value("recipients").toArray())
            if (entry.toObject().value("id").toString() == userId) recipient = true;
        if (recipient) { activateChannel(it.key()); return; }
    }
    openDm(userId);
}

bool DiscordClient::privateVoiceAllocation(const QString &id) const {
    if (!validId(id)) return false;
    const int type = m_dmObjects.value(id).value("type").toInt(-1);
    return m_privateVoiceAllocations.contains(id) || type == 1 || type == 3;
}

bool DiscordClient::startDirectCall(const QString &userId) {
    if (m_directCallStarting) return false;
    if (!m_connected || m_demoMode || m_token.isEmpty() || m_token.startsWith("Bot ") ||
        !validId(userId) || userId == m_currentUserId || m_users.value(userId).value("bot").toBool() ||
        m_blockedUsers.contains(userId) || m_relationships.value(userId).value("user_ignored").toBool()) {
        setError("Sign in and choose an available person to call."); return false;
    }
    if (!m_directCallChannel.isEmpty() && m_voiceGuildId == m_directCallChannel && dmRecipientId(m_dmObjects.value(m_directCallChannel)) == userId) return true;
    const auto operation = ++m_directCallOperation;
    m_directCallStarting = true; m_socialError.clear(); emit socialChanged();
    for (auto it = m_dmObjects.cbegin(); it != m_dmObjects.cend(); ++it) {
        if (it.value().value("type").toInt() != 1 || dmRecipientId(it.value()) != userId) continue;
        if (pendingMessageRequest(it.value())) { m_directCallStarting = false; emit socialChanged(); setError("Accept this message request before starting a call."); return false; }
        prepareDirectCall(it.key(), userId, operation); return true;
    }
    request("POST", "/users/@me/channels", {{"recipient_id", userId}},
        [this, userId, operation](const QJsonValue &value, const QString &error) {
        if (operation != m_directCallOperation) return;
        if (!error.isEmpty()) { m_directCallStarting = false; emit socialChanged(); setError(error); return; }
        const auto channel = value.toObject(); const auto id = channel.value("id").toString();
        if (!validId(id) || channel.value("type").toInt(-1) != 1 || dmRecipientId(channel) != userId || pendingMessageRequest(channel)) {
            m_directCallStarting = false; emit socialChanged(); setError("Discord did not return an available direct conversation for this person."); return;
        }
        acceptDm(channel); prepareDirectCall(id, userId, operation);
    });
    return true;
}

void DiscordClient::prepareDirectCall(const QString &channelId, const QString &userId, quint64 operation) {
    if (operation != m_directCallOperation) return;
    const bool peerPresent = m_voiceStates.value(channelId).value(userId).value("channel_id").toString() == channelId;
    if (peerPresent || m_relationships.value(userId).value("type").toInt() == 1) { beginDirectCall(channelId, userId, operation); return; }
    // Friends can be rung directly. For strangers Discord's call eligibility
    // endpoint decides before any active call in Relay is interrupted.
    request("GET", "/channels/" + channelId + "/call", {}, [this, channelId, userId, operation](const QJsonValue &value, const QString &error) {
        if (operation != m_directCallOperation) return;
        if (!error.isEmpty() || !value.toObject().value("ringable").toBool()) {
            m_directCallStarting = false; emit socialChanged();
            setError(error.isEmpty() ? "Discord does not allow calling this person right now." : error); return;
        }
        beginDirectCall(channelId, userId, operation);
    });
}

void DiscordClient::beginDirectCall(const QString &channelId, const QString &userId, quint64 operation) {
    if (operation != m_directCallOperation) return;
    if (!m_connected || m_socket.state() != QAbstractSocket::ConnectedState || !m_dmObjects.contains(channelId) ||
        pendingMessageRequest(m_dmObjects.value(channelId)) || m_blockedUsers.contains(userId) ||
        m_relationships.value(userId).value("user_ignored").toBool()) {
        m_directCallStarting = false; emit socialChanged(); setError("Reconnect and reopen this conversation before starting its call."); return;
    }
    if (m_directCallChannel == channelId && m_voiceGuildId == channelId) { m_directCallStarting = false; emit socialChanged(); return; }
    if (!sendGateway(13, QJsonObject{{"channel_id", channelId}})) {
        m_directCallStarting = false; emit socialChanged(); setError("Discord could not connect this call. Reconnect and try again."); return;
    }
    m_privateVoiceAllocations.insert(channelId); m_directCallChannel = channelId;
    const bool peerPresent = m_voiceStates.value(channelId).value(userId).value("channel_id").toString() == channelId;
    m_directCallRecipient = peerPresent ? QString{} : userId;
    m_directCallStarting = false; emit socialChanged();
    const auto name = channelRow(m_dmObjects.value(channelId)).value("name").toString();
    emit directCallRequested(channelId, name);
    activateChannel(channelId);
}

void DiscordClient::onDirectCallConnected(const QString &channelId) {
    if (channelId != m_directCallChannel || channelId != m_voiceGuildId || m_directCallRecipient.isEmpty() || !m_connected) return;
    const auto recipient = m_directCallRecipient; m_directCallRecipient.clear();
    if (m_voiceStates.value(channelId).value(recipient).value("channel_id").toString() == channelId) return;
    const auto operation = m_directCallOperation;
    m_outgoingRinging.insert(channelId);
    // Ring once only after the native encrypted voice transport is ready. A
    // lost response must never cause a second unsolicited ring.
    request("POST", "/channels/" + channelId + "/call/ring", {{"recipients", QJsonArray{recipient}}},
        [this, channelId, operation](const QJsonValue &, const QString &error) {
        if (operation != m_directCallOperation || channelId != m_directCallChannel) return;
        if (!error.isEmpty()) {
            m_directCallChannel.clear(); m_directCallRecipient.clear();
            setError(error); emit directCallEnded(channelId); return;
        }
    });
}

QJsonObject DiscordClient::wireVoiceState(const QJsonObject &state) const {
    auto wire = state;
    if (privateVoiceAllocation(state.value("guild_id").toString())) wire.insert("guild_id", QJsonValue::Null);
    return wire;
}

void DiscordClient::refresh() {
    clearError();
    if (m_currentChannelKind == "hidden") return;
    if (m_demoMode) { loadDemoMessages(); return; }
    if (m_token.isEmpty()) return;
    if (!m_connected && !m_connecting) { m_connecting = true; openGateway(); }
    if (!m_currentGuildId.isEmpty()) {
        const auto id = m_currentGuildId;
        request("GET", "/guilds/" + id + "/channels", {},
                [this, id](const QJsonValue &value, const QString &error) {
            if (!error.isEmpty()) { setError(error); return; }
            cacheChannels(id, value.toArray(), true);
            if (id == m_currentGuildId) rebuildChannels(m_currentChannelId.isEmpty());
        });
    } else fetchDms();
    if (!m_currentChannelId.isEmpty()) { if(m_hasNewerMessages)jumpToPresent();else fetchMessages(false); }
}

void DiscordClient::openAttachment(const QString &url) {
    auto safe = safeCdnUrl(url);
    if (safe.isEmpty()) {
        const QUrl candidate(url);
        const auto host = candidate.host().toLower();
        if (candidate.isValid() && candidate.scheme() == "https" && candidate.userInfo().isEmpty() &&
            (candidate.port() == -1 || candidate.port() == 443) &&
            (host == "images-ext-1.discordapp.net" || host == "images-ext-2.discordapp.net"))
            safe = candidate.toString(QUrl::FullyEncoded);
    }
    if (safe.isEmpty()) { setError("Only secure Discord attachment links can be opened here."); return; }
    QDesktopServices::openUrl(QUrl(safe));
}

QUrl DiscordClient::safeExternalUrl(const QString &raw) {
    if (raw.isEmpty() || raw.size() > 4096 || raw != raw.trimmed() ||
        raw.contains(QRegularExpression("[\\x00-\\x20\\x7F]"))) return {};
    const QUrl url(raw, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative() ||
        (url.scheme() != "https" && url.scheme() != "http") || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || url.port() == 0) return {};
    // Encoded controls are also refused before handing a link to another app.
    if (QUrl::fromPercentEncoding(raw.toUtf8()).contains(QRegularExpression("[\\x00-\\x1F\\x7F]"))) return {};
    return url;
}

bool DiscordClient::openLink(const QString &url) {
    const auto safe = safeExternalUrl(url);
    return !safe.isEmpty() && QDesktopServices::openUrl(safe);
}

void DiscordClient::openGateway() {
    if (m_token.isEmpty() || m_demoMode) return;
    m_reconnect.stop();
    QUrl url(m_sessionId.isEmpty() || m_resumeUrl.isEmpty() ? m_gatewayUrl : m_resumeUrl);
    if (!validGateway(url)) {
        m_connecting = false;
        setError("The gateway address was not a secure Discord address.");
        return;
    }
    QUrlQuery query;
    query.addQueryItem("v", "10");
    query.addQueryItem("encoding", "json");
    url.setQuery(query);
    m_closing = true;
    m_socket.abort();
    m_closing = false;
    m_waitingHeartbeat = false;
    m_connected = false;
    m_connecting = true;
    m_status = m_sessionId.isEmpty() ? "Connecting to Discord…" : "Resuming Discord session…";
    emit stateChanged();
    m_gatewayDeadline.start(30000);
    m_socket.open(url);
}

bool DiscordClient::sendGateway(int opcode, const QJsonValue &data) {
    if (m_socket.state() != QAbstractSocket::ConnectedState) return false;
    const auto json = QJsonDocument(QJsonObject{{"op", opcode}, {"d", data}}).toJson(QJsonDocument::Compact);
    return json.size() <= 4096 && m_socket.sendTextMessage(QString::fromUtf8(json)) >= 0;
}

void DiscordClient::identifyOrResume() {
    auto token = QString::fromUtf8(m_token);
    const bool bot = token.startsWith("Bot ");
    if (bot) token.remove(0, 4);
    if (!m_sessionId.isEmpty() && m_sequence >= 0) {
        sendGateway(6, QJsonObject{{"token", token}, {"session_id", m_sessionId}, {"seq", m_sequence}});
        return;
    }
    QJsonObject identify{{"token", token}, {"compress", false}, {"large_threshold", 50},
        {"properties", QJsonObject{{"os", QSysInfo::kernelType() == "winnt" ? "Windows" : "Linux"}, {"browser", "Relay"}, {"device", "Relay"}}}};
    if (bot) {
        // Guilds, voice states, guild/DM messages, and message content. A bot account
        // requires the message-content intent enabled in its developer settings.
        identify.insert("intents", (1 << 0) | (1 << 7) | (1 << 9) | (1 << 12) | (1 << 15));
    } else {
        identify.insert("capabilities", 0);
        identify.insert("presence", QJsonObject{{"status", m_currentPresenceStatus}, {"since", QJsonValue::Null},
                                               {"activities", QJsonArray{}}, {"afk", false}});
    }
    sendGateway(2, identify);
}

void DiscordClient::heartbeat() {
    if (m_socket.state() != QAbstractSocket::ConnectedState) return;
    if (m_waitingHeartbeat) { reconnect(false); return; }
    sendGateway(1, m_sequence >= 0 ? QJsonValue(m_sequence) : QJsonValue(QJsonValue::Null));
    m_waitingHeartbeat = true;
    m_heartbeat.start(m_heartbeatInterval);
}

void DiscordClient::onGatewayText(const QString &text) {
    if (m_token.isEmpty() || text.size() > MaximumResponse) return;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(text.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return;
    const auto payload = document.object();
    if (payload.value("s").isDouble()) {
        const auto sequence = payload.value("s").toInteger(-1);
        // Duplicate dispatches may follow a resumed transport. Never apply them twice.
        if (payload.value("op").toInt(-1) == 0 && sequence >= 0 && sequence <= m_sequence) return;
        m_sequence = sequence;
    }
    const auto data = payload.value("d");
    switch (payload.value("op").toInt(-1)) {
    case 0: dispatch(payload.value("t").toString(), data.toObject()); break;
    case 1:
        // Server-requested heartbeats are immediate and do not reset the periodic schedule.
        sendGateway(1, m_sequence >= 0 ? QJsonValue(m_sequence) : QJsonValue(QJsonValue::Null));
        break;
    case 7: reconnect(false, 100); break;
    case 9: reconnect(!data.toBool(), 1000 + QRandomGenerator::global()->bounded(4000)); break;
    case 10: {
        const int interval = data.toObject().value("heartbeat_interval").toInt();
        if (interval < 1000 || interval > 300000) {
            m_connecting = false;
            setError("Discord sent an invalid heartbeat interval.");
            m_closing = true;
            m_socket.abort();
            m_closing = false;
            break;
        }
        m_heartbeatInterval = interval;
        m_waitingHeartbeat = false;
        m_heartbeat.start(QRandomGenerator::global()->bounded(interval));
        identifyOrResume();
        break;
    }
    case 11: m_waitingHeartbeat = false; break;
    default: break;
    }
}

void DiscordClient::reconnect(bool freshSession, int minimumDelay) {
    if (m_closing || m_token.isEmpty() || m_demoMode) return;
    if (freshSession) { m_sessionId.clear(); m_resumeUrl.clear(); m_sequence = -1; }
    if (m_reconnect.isActive()) return;
    m_closing = true;
    m_socket.abort();
    m_closing = false;
    m_heartbeat.stop();
    m_gatewayDeadline.stop();
    m_connected = false;
    m_membersLoading = false;
    m_memberDeadline.stop();
    m_connecting = true;
    m_status = "Connection interrupted · reconnecting…";
    const int delay = std::max(minimumDelay, std::min(30000, 500 * (1 << std::min(m_reconnectAttempts, 6))) +
                                                QRandomGenerator::global()->bounded(500));
    ++m_reconnectAttempts;
    m_reconnect.start(delay);
    emit stateChanged();
}

void DiscordClient::onDisconnected() {
    m_heartbeat.stop();
    if (m_closing || m_token.isEmpty() || m_demoMode) return;
    const int code = static_cast<int>(m_socket.closeCode());
    if (code == 4004 || (code >= 4010 && code <= 4014)) {
        if (code == 4004) emit authenticationRejected();
        const QString reason = code == 4004 ?
            QStringLiteral("Discord rejected gateway authentication. Sign in again with a valid session.") :
            QStringLiteral("Discord rejected this gateway configuration (code %1). Reconnecting automatically would not fix it.").arg(code);
        logout();
        setError(reason);
        return;
    }
    reconnect(code == 4007 || code == 4009, code == 4008 ? 60000 : 0);
}

void DiscordClient::dispatch(const QString &event, QJsonObject data) {
    if ((event == "VOICE_STATE_UPDATE" || event == "VOICE_SERVER_UPDATE") && data.value("guild_id").toString().isEmpty()) {
        // Discord's private calls use a null guild ID on the main gateway and
        // the DM channel ID as the voice server ID. Keep that conversion at
        // this boundary so the tested native guild-voice engine is unchanged.
        QString allocation;
        const auto channel = data.value("channel_id").toString();
        const auto user = data.value("user_id").toString();
        if (privateVoiceAllocation(channel)) allocation = channel;
        else if (event == "VOICE_STATE_UPDATE" && channel.isEmpty()) {
            if (user == m_currentUserId && !m_privateLeaveAllocations.isEmpty()) allocation = m_privateLeaveAllocations.dequeue();
            else if (privateVoiceAllocation(m_voiceGuildId) && (user == m_currentUserId || m_voiceStates.value(m_voiceGuildId).contains(user))) allocation = m_voiceGuildId;
            else if (privateVoiceAllocation(m_directCallChannel) && (user == m_currentUserId || m_voiceStates.value(m_directCallChannel).contains(user))) allocation = m_directCallChannel;
        }
        // Preserve the established raw-forwarding contract for unknown events;
        // only a known private allocation can acquire a native routing ID.
        if (!allocation.isEmpty()) data.insert("guild_id", allocation);
    }
    ++m_unreadDmBatchDepth;
    const auto finishBatch=qScopeGuard([this] { if (!--m_unreadDmBatchDepth && m_unreadDmsDirty) rebuildUnreadDms(); });
    if(event=="THREAD_CREATE" || event=="THREAD_UPDATE" || event=="THREAD_DELETE" || event=="THREAD_LIST_SYNC" || event=="MESSAGE_CREATE" || event=="CHANNEL_UPDATE" || event=="CHANNEL_DELETE")
        emit forumEventReceived(event,data);
    auto refreshAuthorPresentation = [this](const QString &guildId, const QString &userId = QString{}) {
        if (guildId.isEmpty()) return;
        for (auto it = m_messageObjects.cbegin(); it != m_messageObjects.cend(); ++it) {
            const auto existing = m_messages.byId(it.key());
            if (existing.isEmpty() || existing.value("guildId").toString() != guildId ||
                (!userId.isEmpty() && existing.value("authorId").toString() != userId)) continue;
            const auto updated = messageRow(it.value());
            QVariantMap fields;
            for (const auto &key : {"author", "initials", "avatarUrl", "roleName", "roleColor", "bot", "verifiedBot", "primaryGuild"})
                fields.insert(key, updated.value(key));
            // Keep pending sends, history order, edit state and message contents intact.
            m_messages.patch(it.key(), fields);
        }
    };
    if (event == "READY") {
        ++m_socialBatchDepth;
        const auto socialBatch = qScopeGuard([this] { --m_socialBatchDepth; rebuildSocial(); });
        m_gatewayDeadline.stop();
        updateUser(data.value("user").toObject());
        m_sessionId = data.value("session_id").toString();
        m_authSessionIdHash = data.value("auth_session_id_hash").toString().left(128);
        const auto resume = data.value("resume_gateway_url").toString();
        m_resumeUrl = validGateway(QUrl(resume)) ? resume : QString{};
        m_connected = true;
        m_connecting = false;
        m_reconnectAttempts = 0;
        m_status = "Connected to Discord";
        m_error.clear();
        m_notificationStartMs = nowMs();
        m_memberSlots.clear();
        m_memberListReceived = false;
        m_subscribedGuild.clear();
        m_subscribedChannel.clear();
        auto notificationSettings = data.value("user_guild_settings").toArray();
        if (notificationSettings.isEmpty()) notificationSettings = data.value("user_guild_settings").toObject().value("entries").toArray();
        for (const auto &value : notificationSettings) acceptNotificationSettings(value.toObject());
        for (const auto &value : data.value("users").toArray()) {
            const auto user = value.toObject();
            const auto id = user.value("id").toString();
            if (validId(id) && m_users.size() < 5000) m_users.insert(id, user);
        }
        for (const auto &id : m_userNotes.keys()) m_userNoteVersions.insert(id, ++m_socialVersion);
        m_userNotes.clear();
        const auto notes = data.value("notes").toObject();
        for (auto it = notes.begin(); it != notes.end() && m_userNotes.size() < 5000; ++it)
            if (validId(it.key()) && it.value().isString()) {
                m_userNotes.insert(it.key(), cleanText(it.value().toString(), 256)); m_userNoteVersions.insert(it.key(), ++m_socialVersion);
            }
        auto readState = data.value("read_state").toArray();
        if (readState.isEmpty()) readState = data.value("read_state").toObject().value("entries").toArray();
        if (data.contains("read_state")) {
            for (auto it=m_dmObjects.cbegin(); it!=m_dmObjects.cend(); ++it) m_unread.remove(it.key());
        }
        for (const auto &value : readState) {
            const auto state = value.toObject();
            if (m_unread.size() >= 20000) break;
            const auto id = state.value("id").toString();
            if (validId(id)) m_unread.insert(id, std::clamp(state.value("mention_count").toInt(), 0, 9999));
        }
        const auto guilds = data.value("guilds").toArray();
        const auto mergedMembers = data.value("merged_members").toArray();
        for (int i = 0; i < guilds.size(); ++i) {
            auto guild = guilds.at(i).toObject();
            if (i < mergedMembers.size()) {
                for (const auto &value : mergedMembers.at(i).toArray()) {
                    auto member = value.toObject();
                    cacheMember(guild.value("id").toString(), member);
                    if (member.value("user_id").toString() == m_currentUserId ||
                        member.value("user").toObject().value("id").toString() == m_currentUserId) {
                        guild.insert("member", member);
                    }
                }
            }
            acceptGuild(guild);
        }
        m_blockedUsers.clear();
        for (const auto &id : m_relationships.keys()) m_relationshipVersions.insert(id, ++m_socialVersion);
        m_relationships.clear();
        for (const auto &value : data.value("relationships").toArray()) {
            acceptRelationship(value.toObject());
        }
        for (const auto &value : data.value("private_channels").toArray()) acceptDm(value.toObject());
        for (auto it=m_dmObjects.cbegin(); it!=m_dmObjects.cend(); ++it) syncDm(it.key());
        for (const auto &value : readState) {
            const auto state=value.toObject();
            acceptDmReadState(state);
        }
        if (m_currentGuildId.isEmpty() && m_currentChannelId.isEmpty()) {
            rebuildChannels(false);
        }
        subscribeMembers();
        requestChannelInfo();
        flushPendingVoiceState();
        emit stateChanged();
        emit modelsReady();
    } else if (event == "AUTH_SESSION_CHANGE") {
        m_authSessionIdHash = data.value("auth_session_id_hash").toString().left(128);
        emit stateChanged();
    } else if (event == "READY_SUPPLEMENTAL") {
        ++m_socialBatchDepth;
        const auto socialBatch = qScopeGuard([this] { --m_socialBatchDepth; rebuildSocial(); });
        const auto supplementalGuilds = data.value("guilds").toArray();
        const auto supplementalMembers = data.value("merged_members").toArray();
        const auto supplementalPresences = data.value("merged_presences").toObject().value("guilds").toArray();
        for (int i = 0; i < supplementalGuilds.size(); ++i) {
            const auto value = supplementalGuilds[i];
            const auto guild = value.toObject();
            const auto id = guild.value("id").toString();
            if (!validId(id)) continue;
            if (guild.contains("voice_states")) {
                m_voiceStates.remove(id);
                for (const auto &state : guild.value("voice_states").toArray()) acceptVoiceState(state.toObject(), id);
                m_voiceSnapshots.insert(id);
            }
            if (i < supplementalMembers.size()) for (const auto &member : supplementalMembers[i].toArray()) cacheMember(id, member.toObject());
            if (i < supplementalPresences.size()) for (const auto &presence : supplementalPresences[i].toArray()) acceptPresence(id, presence.toObject());
            updateGuildActivity(id);
        }
        for (const auto &group : data.value("merged_members").toArray())
            for (const auto &member : group.toArray()) rememberMember(member.toObject());
        for (const auto &presence : data.value("merged_presences").toObject().value("friends").toArray()) acceptPresence({}, presence.toObject());
        for (const auto &channel : data.value("lazy_private_channels").toArray()) acceptDm(channel.toObject());
        rebuildVoiceMembers();
        rebuildMembers();
    } else if (event == "RESUMED") {
        m_gatewayDeadline.stop();
        m_connected = true;
        m_connecting = false;
        m_reconnectAttempts = 0;
        m_status = "Connected to Discord";
        m_notificationStartMs = nowMs();
        m_subscribedGuild.clear();
        m_subscribedChannel.clear();
        m_channelInfoRequestedAt.clear();
        subscribeMembers();
        requestChannelInfo();
        flushPendingVoiceState();
        emit stateChanged();
    } else if (event == "GUILD_CREATE" || event == "GUILD_UPDATE") {
        if (event == "GUILD_CREATE") m_removedGuilds.remove(data.value("id").toString());
        const bool joined=event=="GUILD_CREATE" && m_connected && validId(data.value("id").toString()) && !m_guildObjects.contains(data.value("id").toString());
        acceptGuild(data);
        if(joined)emit activitySoundRequested("join");
    } else if (event == "GUILD_DELETE") {
        if (data.value("unavailable").toBool()) return;
        const auto id = data.value("id").toString();
        if (!validId(id) || !m_guildObjects.contains(id)) return;
        if(m_connected)emit activitySoundRequested("leave");
        if (m_removedGuilds.size() < m_guilds.maximumRows()) m_removedGuilds.insert(id);
        for (const auto &entry : m_channelCache.value(id)) m_unread.remove(entry.toObject().value("id").toString());
        m_unread.remove(id);
        m_guilds.remove(id);
        m_guildObjects.remove(id);
        m_guildMembershipVersions.remove(id);
        if (m_pendingVoiceState.value("guild_id").toString() == id && !m_pendingVoiceState.value("channel_id").isNull()) {
            m_pendingVoiceState = {}; m_pendingVoiceAccount.clear();
        }
        m_channelCache.remove(id);
        m_channelCacheOrder.removeAll(id);
        m_voiceStates.remove(id);
        m_voiceSnapshots.remove(id);
        m_guildMembers.remove(id);
        m_presences.remove(id);
        m_notificationSettings.remove(id);
        m_channelInfoRequestedAt.remove(id);
        const auto profileSuffix = ':' + id;
        for (const auto &key : m_profiles.keys()) if (key.endsWith(profileSuffix)) {
            m_profiles.remove(key); m_profileFetchedAt.remove(key); m_profilesLoading.remove(key);
        }
        const auto streamPrefix = "guild:" + id + ':';
        for (const auto &key : m_streamObjects.keys()) if (key.startsWith(streamPrefix)) m_streamObjects.remove(key);
        emit guildRemoved(id);
        rebuildVoiceMembers();
        if (id == m_currentGuildId) selectGuild({});
        else emit stateChanged();
    } else if (event == "CHANNEL_CREATE" || event == "CHANNEL_UPDATE" || event == "THREAD_CREATE" || event == "THREAD_UPDATE") {
        const auto guildId = data.value("guild_id").toString();
        const auto id = data.value("id").toString();
        if (!validId(id)) return;
        if (guildId.isEmpty()) { acceptDm(data); return; }
        if (!m_channelCache.contains(guildId)) return;
        auto channels = m_channelCache.value(guildId);
        bool found = false;
        for (int i = 0; i < channels.size(); ++i) {
            if (channels.at(i).toObject().value("id").toString() == id) {
                channels.replace(i, merged(channels.at(i).toObject(), data));
                found = true;
                break;
            }
        }
        if (!found) {
            if (!data.value("type").isDouble() || channelKind(data.value("type").toInt(-1)).isEmpty()) return;
            channels.append(data);
        }
        cacheChannels(guildId, channels);
        if (guildId == m_currentGuildId) {
            if (event.startsWith("CHANNEL_") || id == m_currentChannelId) rebuildChannels(false);
            if (id == m_currentChannelId) {
                const auto row = channelRow(cachedChannel(guildId, id), guildId);
                m_currentChannelName = row.value("name").toString();
                m_currentChannelTopic = row.value("topic").toString();
                emit stateChanged();
            }
        }
    } else if (event == "CHANNEL_DELETE" || event == "THREAD_DELETE") {
        const auto guildId = data.value("guild_id").toString();
        const auto id = data.value("id").toString();
        auto channels = m_channelCache.value(guildId);
        for (int i = channels.size() - 1; i >= 0; --i)
            if (channels.at(i).toObject().value("id").toString() == id || (channelKind(channels.at(i).toObject().value("type").toInt(-1)) == "thread" && channels.at(i).toObject().value("parent_id").toString() == id)) channels.removeAt(i);
        if (!guildId.isEmpty()) cacheChannels(guildId, channels);
        m_dmObjects.remove(id);
        m_dmSocialVersions.insert(id, ++m_socialVersion);
        m_dmReadMessageIds.remove(id);
        m_confirmedReadMessageIds.remove(id); m_latestChannelMessageIds.remove(id); m_localReadMessageIds.remove(id); m_readChannelGuilds.remove(id);
        m_deletedLatestReadBoundaries.remove(id);
        m_unread.remove(id);
        m_dms.remove(id);
        m_channels.remove(id);
        rebuildSocial();
        if (id == m_currentChannelId) clearSelection();
        if (guildId == m_currentGuildId) { rebuildChannels(m_currentChannelId.isEmpty()); emit stateChanged(); }
    } else if (event == "GUILD_ROLE_CREATE" || event == "GUILD_ROLE_UPDATE" || event == "GUILD_ROLE_DELETE") {
        const auto guildId=data.value("guild_id").toString();
        if(!m_guildObjects.contains(guildId))return;
        const auto updated=data.value("role").toObject();const auto roleId=data.value("role_id").toString(updated.value("id").toString());
        if(!validId(roleId))return;
        auto roles=m_guildObjects.value(guildId).value("roles").toArray();bool found=false;
        for(int i=roles.size()-1;i>=0;--i)if(roles[i].toObject().value("id").toString()==roleId) {
            if(event=="GUILD_ROLE_DELETE")roles.removeAt(i);else roles[i]=merged(roles[i].toObject(),updated);found=true;break;
        }
        if(!found && event!="GUILD_ROLE_DELETE" && roles.size()<250)roles.append(updated);
        m_guildObjects[guildId].insert("roles",roles);
        emit channelReferencesChanged();
        if(guildId==m_currentGuildId){rebuildMembers();rebuildChannels(false);}
        if(guildId==m_currentGuildId || guildId==m_voiceGuildId)rebuildVoiceMembers();
        refreshAuthorPresentation(guildId);emit stateChanged();
    } else if (event == "GUILD_MEMBER_UPDATE" || event == "GUILD_MEMBER_ADD") {
        const auto id = data.value("guild_id").toString();
        cacheMember(id, data);
        if (data.value("user").toObject().value("id").toString() == m_currentUserId && m_guildObjects.contains(id)) {
            m_guildObjects[id].insert("member", data);
            emit channelReferencesChanged();
            if (id == m_currentGuildId) rebuildChannels(false);
        }
        if (id == m_currentGuildId) rebuildMembers();
        if (id == m_currentGuildId || id == m_voiceGuildId) rebuildVoiceMembers();
        const auto userId = data.value("user").toObject().value("id").toString();
        refreshAuthorPresentation(id, userId);
        if (validId(userId)) emit profileUpdated(userId);
    } else if (event == "GUILD_MEMBER_REMOVE") {
        const auto guildId = data.value("guild_id").toString();
        const auto id = data.value("user").toObject().value("id").toString();
        m_guildMembers[guildId].remove(id);
        m_presences[guildId].remove(id);
        if (guildId == m_currentGuildId) {
            for (auto it = m_memberSlots.begin(); it != m_memberSlots.end(); ++it)
                if (it.value().value("member").toObject().value("user").toObject().value("id").toString() == id) it.value() = {};
            rebuildMembers();
        }
    } else if (event == "RELATIONSHIP_ADD" || event == "RELATIONSHIP_UPDATE" || event == "RELATIONSHIP_REMOVE") {
        acceptRelationship(data,event=="RELATIONSHIP_REMOVE");
    } else if (event == "USER_NOTE_UPDATE") {
        const auto id = data.value("id").toString();
        if (!validId(id) || (!data.value("note").isString() && !data.value("note").isNull())) return;
        const auto note = cleanText(data.value("note").toString(), 256);
        if (m_userNotes.contains(id) || m_userNotes.size() < 5000) m_userNotes.insert(id, note);
        m_userNoteVersions.insert(id, ++m_socialVersion); emit userNoteLoaded(id, note); emit socialChanged();
    } else if (event == "CHANNEL_INFO") {
        const auto guildId=data.value("guild_id").toString();
        int count=0; for (const auto &entry : data.value("channels").toArray()) {
            if (++count>2000) break;
            acceptChannelInfo(guildId,entry.toObject());
        }
    } else if (event == "CALL_CREATE" || event == "CALL_UPDATE") {
        const auto channelId = data.value("channel_id").toString();
        if (!privateVoiceAllocation(channelId) || (!m_privateCalls.contains(channelId) && m_privateCalls.size() >= 1000)) return;
        m_privateCalls.insert(channelId);
        m_privateVoiceAllocations.insert(channelId);
        if (data.contains("voice_states")) {
            if (event == "CALL_CREATE") m_voiceStates.remove(channelId);
            int count = 0;
            for (const auto &entry : data.value("voice_states").toArray()) {
                if (++count > 1000) break;
                auto state = entry.toObject(); state.insert("guild_id", channelId);
                if (!state.contains("channel_id")) state.insert("channel_id", channelId);
                acceptVoiceState(state);
                if (state.value("user_id").toString() == m_currentUserId && !state.value("session_id").toString().isEmpty()) emit voiceStateReceived(state);
            }
        }
        m_voiceSnapshots.insert(channelId); rebuildVoiceMembers();
    } else if (event == "CALL_DELETE") {
        const auto channelId = data.value("channel_id").toString();
        if (!privateVoiceAllocation(channelId)) return;
        if (data.value("unavailable").toBool()) return;
        m_privateCalls.remove(channelId); m_outgoingRinging.remove(channelId);
        m_voiceStates.remove(channelId); m_voiceSnapshots.remove(channelId);
        if (m_directCallChannel == channelId) { m_directCallChannel.clear(); m_directCallRecipient.clear(); ++m_directCallOperation; }
        rebuildVoiceMembers();
        for (const auto &id : m_dmObjects.keys()) syncDm(id);
        rebuildSocial(); emit directCallEnded(channelId);
    } else if (event == "VOICE_CHANNEL_START_TIME_UPDATE") {
        acceptChannelInfo(data.value("guild_id").toString(),data);
    } else if (event == "PRESENCE_UPDATE") {
        const auto guildId = data.value("guild_id").toString();
        acceptPresence(guildId, data);
        if (guildId == m_currentGuildId || m_currentGuildId.isEmpty()) rebuildMembers();
        rebuildVoiceMembers();
    } else if (event == "GUILD_MEMBER_LIST_UPDATE") {
        acceptMemberList(data);
    } else if (event == "GUILD_SYNC" || event == "GUILD_MEMBERS_CHUNK") {
        const auto guildId = data.value("guild_id").toString(data.value("id").toString());
        for (const auto &member : data.value("members").toArray()) cacheMember(guildId, member.toObject());
        for (const auto &presence : data.value("presences").toArray()) acceptPresence(guildId, presence.toObject());
        if (guildId == m_currentGuildId) rebuildMembers();
    } else if (event == "USER_GUILD_SETTINGS_UPDATE") {
        acceptNotificationSettings(data);
    } else if (event == "MESSAGE_ACK") {
        acceptDmReadState(data);
    } else if (event == "READ_STATE_UPDATE") {
        acceptDmReadState(data);
        for (const auto &entry : data.value("entries").toArray()) acceptDmReadState(entry.toObject());
    } else if (event == "MESSAGE_CREATE") {
        const auto channelId = data.value("channel_id").toString();
        const auto messageId = data.value("id").toString(), authorId=data.value("author").toObject().value("id").toString();
        if (m_deletedMessageChannels.contains(messageId) && m_deletedMessageChannels.value(messageId) == channelId) return;
        const auto guildId = data.value("guild_id").toString();
        const int directType=m_dmObjects.value(channelId).value("type").toInt(-1);
        const bool direct=guildId.isEmpty() && (directType==1 || directType==3) && !pendingMessageRequest(m_dmObjects.value(channelId));
        const bool directUnread=direct && validId(channelId) && validId(messageId) && validId(authorId) &&
            !m_blockedUsers.contains(authorId) && !(data.value("flags").toInt() & (1<<12)) && !m_messageObjects.contains(messageId) &&
            newerSnowflake(messageId,m_dmReadMessageIds.value(channelId)) &&
            newerSnowflake(messageId,m_dmObjects.value(channelId).value("last_message_id").toString());
        const bool duplicate = m_seenLiveMessages.contains(messageId);
        if (directType == 1 && !duplicate) emit relaySetupObserved(data);
        notifyMessage(data);
        recordDmActivity(data);
        if (m_selectedMessageRequest.value("channelId").toString() == channelId && validId(messageId) && !duplicate && !m_requestPreviewLoading) {
            auto preview = messageRow(data); preview.insert("components", QVariantList{});
            m_requestPreviewMessages.append(preview);
            if (m_requestPreviewMessages.size() > 50) m_requestPreviewMessages.removeFirst();
            emit socialChanged();
        }
        if (channelId == m_currentChannelId) acceptMessage(data);
        const bool guildUnread = !guildId.isEmpty() && newerSnowflake(messageId, m_localReadMessageIds.value(channelId)) &&
            newerSnowflake(messageId, m_confirmedReadMessageIds.value(channelId));
        if ((guildUnread || (direct && directUnread)) &&
            !duplicate && authorId != m_currentUserId) {
            const int count = std::min(9999, m_unread.value(channelId) + 1);
            if (m_unread.size() < 20000 || m_unread.contains(channelId)) m_unread.insert(channelId, count);
            m_channels.patch(channelId, {{"unread", count}});
            m_dms.patch(channelId, {{"unread", count}});
            if (!guildId.isEmpty()) {
                const int guildCount = std::min(9999, m_unread.value(guildId) + 1);
                m_unread.insert(guildId, guildCount);
                m_guilds.patch(guildId, {{"unread", guildCount}});
            }
        }
    } else if (event.startsWith("MESSAGE_REACTION_")) {
        const auto channel = data.value("channel_id").toString(), id = data.value("message_id").toString();
        if (channel != m_currentChannelId || !m_messageObjects.contains(id)) return;
        auto reactions = m_messageObjects.value(id).value("reactions").toArray();
        const auto emoji = data.value("emoji").toObject();
        auto same = [&emoji](const QJsonObject &value) {
            const auto other = value.value("emoji").toObject();
            return emoji.value("id").toString().isEmpty()
                ? other.value("id").toString().isEmpty() && other.value("name") == emoji.value("name")
                : other.value("id") == emoji.value("id");
        };
        if (event == "MESSAGE_REACTION_REMOVE_ALL") reactions = {};
        else if (event == "MESSAGE_REACTION_REMOVE_EMOJI") {
            for (int i = reactions.size() - 1; i >= 0; --i) if (same(reactions.at(i).toObject())) reactions.removeAt(i);
        } else if (event == "MESSAGE_REACTION_ADD" || event == "MESSAGE_REACTION_REMOVE") {
            // Burst reactions have separate semantics; refresh rather than mixing their counts.
            if (data.value("burst").toBool() || data.value("type").toInt() == 1) return;
            int index = -1;
            for (int i = 0; i < reactions.size(); ++i) if (same(reactions.at(i).toObject())) { index = i; break; }
            auto reaction = index >= 0 ? reactions.at(index).toObject() : QJsonObject{{"emoji", emoji}, {"count", 0}};
            const bool add = event == "MESSAGE_REACTION_ADD";
            reaction.insert("count", std::max(0, reaction.value("count").toInt() + (add ? 1 : -1)));
            if (reaction.value("count_details").isObject()) {
                auto details = reaction.value("count_details").toObject();
                details.insert("normal", std::max(0, details.value("normal").toInt() + (add ? 1 : -1)));
                reaction.insert("count_details", details);
            }
            if (data.value("user_id").toString() == m_currentUserId) reaction.insert("me", add);
            if (index >= 0) reactions.removeAt(index);
            if (reaction.value("count").toInt() > 0) reactions.append(reaction);
        }
        acceptMessage({{"id", id}, {"channel_id", channel}, {"reactions", reactions}}, true);
    } else if (event == "MESSAGE_UPDATE") {
        acceptMessage(data, true);
    } else if (event == "MESSAGE_DELETE" || event == "MESSAGE_DELETE_BULK") {
        const auto channelId = data.value("channel_id").toString();
        auto ids = data.value("ids").toArray();
        if (event == "MESSAGE_DELETE") ids.append(data.value("id"));
        for (const auto &value : ids) retireDeletedReadTarget(channelId, value.toString());
        if (channelId != m_currentChannelId) return;
        for (const auto &value : ids) {
            const auto id = value.toString();
            if (m_deletedMessages.size() < m_messages.maximumRows()) m_deletedMessages.insert(id);
            m_pendingMessageUpdates.remove(id);
            m_deferredLiveMessages.remove(id);
            m_messageObjects.remove(id);
            m_messages.remove(id);
            for (const auto &row : m_messages.rows()) {
                if (row.value("replyId").toString() == id && row.value("replyChannelId").toString() == m_currentChannelId)
                    m_messages.patch(row.value("recordId").toString(), {{"replyDeleted", true}, {"replyMissing", true},
                        {"replyContent", "Original message was deleted."}});
            }
        }
    } else if (event == "VOICE_STATE_UPDATE") {
        const auto guildId = data.value("guild_id").toString();
        const auto userId = data.value("user_id").toString();
        const auto before = m_voiceStates.value(guildId).value(userId);
        const auto ownChannel = m_voiceStates.value(guildId).value(m_currentUserId).value("channel_id").toString();
        // Snapshot loads use acceptVoiceState directly. Only live changes after an
        // authoritative snapshot can produce sounds, and only in our own call.
        if (m_connected && !m_demoMode && m_voiceSnapshots.contains(guildId) &&
            !ownChannel.isEmpty() && userId != m_currentUserId && validId(userId)) {
            const bool wasHere = before.value("channel_id").toString() == ownChannel;
            const bool isHere = data.value("channel_id").toString() == ownChannel;
            if (!wasHere && isHere) emit activitySoundRequested("user-join");
            else if (wasHere && !isHere) emit activitySoundRequested("user-leave");
            else if (wasHere && isHere) {
                const bool wasStreaming = before.value("streaming").toBool(), streaming = data.value("self_stream").toBool();
                const bool hadCamera = before.value("camera").toBool(), camera = data.value("self_video").toBool();
                const bool started = (!wasStreaming && streaming) || (!hadCamera && camera);
                const bool stopped = (wasStreaming && !streaming) || (hadCamera && !camera);
                // A screen/camera handoff in one update needs no pair of
                // contradictory cues. Ordinary starts and stops share a cue.
                if (started != stopped) emit activitySoundRequested(started ? "stream-start" : "stream-stop");
            }
        }
        acceptVoiceState(data);
        if (!m_desiredStream.isEmpty() && m_watchingStream.isEmpty() &&
            guildId==m_desiredStream.section(':',1,1) && userId==m_desiredStream.section(':',3,3) &&
            data.value("channel_id").toString()==m_desiredStream.section(':',2,2) &&
            data.value("self_stream").toBool() && !before.value("streaming").toBool())
            resumeWatchingStream();
        // Some gateway sessions omit the initial voice roster. A live own join
        // for Relay's selected call establishes a baseline without announcing
        // the existing participants, just as a received snapshot would.
        if (m_connected && !m_demoMode && validId(guildId) && guildId == m_voiceGuildId && userId == m_currentUserId &&
            validId(data.value("channel_id").toString()) && !data.value("session_id").toString().isEmpty())
            m_voiceSnapshots.insert(guildId);
        if (data.value("guild_id").toString() == m_currentGuildId ||
            data.value("guild_id").toString() == m_voiceGuildId) rebuildVoiceMembers();
        emit voiceStateReceived(data);
    } else if (event == "VOICE_CHANNEL_EFFECT_SEND") {
        emit soundboardEffect(data);
    } else if (event == "STREAM_CREATE" || event == "STREAM_UPDATE") {
        const auto key = data.value("stream_key").toString();
        if (!validStreamKey(key)) return;
        if (m_streamObjects.contains(key) || m_streamObjects.size() < 1000) {
            // Store display metadata only; route the original transport fields directly to the engine.
            auto display = m_streamObjects.value(key);
            if (data.contains("paused")) display.insert("paused", data.value("paused").toBool());
            // Lists are snapshots; omitted fields preserve the last known list.
            if (data.value("viewer_ids").isArray()) {
                QJsonArray viewers;
                QSet<QString> seen;
                for (const auto &value : data.value("viewer_ids").toArray()) {
                    const auto id = value.toString();
                    if (validId(id) && !seen.contains(id)) { viewers.append(id); seen.insert(id); }
                    if (viewers.size() >= 1000) break;
                }
                display.insert("viewer_ids", viewers);
            }
            m_streamObjects.insert(key, display);
        }
        rebuildVoiceMembers();
        if (event == "STREAM_CREATE" && key == m_desiredStream && m_watchingStream.isEmpty()) resumeWatchingStream();
        if (event == "STREAM_CREATE" && key == m_watchingStream) emit streamCreated(data);
        if (event == "STREAM_CREATE" && key == m_broadcastStream) emit broadcastCreated(data);
    } else if (event == "STREAM_SERVER_UPDATE") {
        if (data.value("stream_key").toString() == m_watchingStream) emit streamServerReceived(data);
        if (data.value("stream_key").toString() == m_broadcastStream) emit broadcastServerReceived(data);
    } else if (event == "STREAM_DELETE") {
        const auto key = data.value("stream_key").toString();
        m_streamObjects.remove(key);
        // Viewer transport deletion (including temporary unavailability) does not
        // change the owner's presence. VOICE_STATE_UPDATE owns self_stream.
        if (key == m_watchingStream) { m_watchingStream.clear(); emit streamDeleted(data); }
        if (key == m_broadcastStream) { m_broadcastStream.clear(); emit broadcastDeleted(data); }
        rebuildVoiceMembers();
    } else if (event == "VOICE_SERVER_UPDATE") {
        emit voiceServerReceived(data);
    } else if (event == "USER_UPDATE") {
        rememberMember({{"user",data}});
        if (data.value("id").toString() == m_currentUserId) updateUser(m_users.value(m_currentUserId));
    }
}

bool DiscordClient::validStreamKey(const QString &key) {
    const auto parts = key.split(':');
    return parts.size() == 4 && parts[0] == "guild" && validId(parts[1]) && validId(parts[2]) && validId(parts[3]);
}

void DiscordClient::rebuildStreams() {
    QVector<QVariantMap> rows;
    for (auto guild = m_voiceStates.cbegin(); guild != m_voiceStates.cend(); ++guild) {
        if (guild.key() != m_currentGuildId && guild.key() != m_voiceGuildId) continue;
        for (auto member = guild.value().cbegin(); member != guild.value().cend(); ++member) {
            if (!member.value().value("streaming").toBool() || rows.size() >= 1000) continue;
            const auto channel = member.value().value("channel_id").toString();
            const auto key = "guild:" + guild.key() + ":" + channel + ":" + member.key();
            const auto user = m_users.value(member.key());
            rows.append({{"recordId", key}, {"streamKey", key}, {"name", displayName(user)},
                {"authorId", member.key()}, {"guildId", guild.key()}, {"channelId", channel},
                {"avatarUrl", avatarUrl(user)}, {"paused", m_streamObjects.value(key).value("paused").toBool()}});
        }
    }
    m_streams.reconcile(std::move(rows));
}

void DiscordClient::resumeWatchingStream() {
    if (m_desiredStream.isEmpty() || !m_watchingStream.isEmpty() || !m_connected || m_demoMode) return;
    const auto parts=m_desiredStream.split(':');
    if (parts.size()!=4 || m_voiceSessionId.isEmpty() ||
        m_voiceStates.value(parts[1]).value(m_currentUserId).value("channel_id").toString()!=parts[2]) {
        stopWatchingStream(); return;
    }
    m_watchingStream=m_desiredStream;
    emit streamRestarting(m_watchingStream);
    sendGateway(20,QJsonObject{{"stream_key",m_watchingStream}});
}

bool DiscordClient::watchStream(const QString &key) {
    if (!m_connected || m_demoMode || !validStreamKey(key)) { setError("Sign in and choose an available stream."); return false; }
    const auto parts = key.split(':');
    const auto own = m_voiceStates.value(parts[1]).value(m_currentUserId);
    if (parts[3] == m_currentUserId || m_voiceSessionId.isEmpty() || own.value("channel_id").toString() != parts[2]) {
        setError("Join the same voice channel before watching this stream."); return false;
    }
    stopWatchingStream();
    m_desiredStream = key;
    m_watchingStream = key;
    sendGateway(20, QJsonObject{{"stream_key", key}});
    return true;
}

void DiscordClient::stopWatchingStream(const QString &key) {
    if (m_desiredStream.isEmpty() && m_watchingStream.isEmpty()) return;
    const auto previous = m_desiredStream.isEmpty() ? m_watchingStream : m_desiredStream;
    if (!key.isEmpty() && key != previous) return;
    const bool subscribed = !m_watchingStream.isEmpty();
    m_desiredStream.clear();
    m_watchingStream.clear();
    if (subscribed && m_connected && !m_demoMode) sendGateway(19, QJsonObject{{"stream_key", previous}});
    emit streamDeleted(QJsonObject{{"stream_key", previous},{"relay_cancelled",true}});
}

bool DiscordClient::startBroadcast(const QString &guildId, const QString &channelId) {
    if (isDirectCall(guildId)) { setError("Screen sharing in direct calls is not available yet. Join a server voice channel to share."); return false; }
    if (!m_connected || m_demoMode || !validId(guildId) || !validId(channelId) ||
        !validId(m_currentUserId) || m_voiceSessionId.isEmpty() ||
        m_voiceStates.value(guildId).value(m_currentUserId).value("channel_id").toString() != channelId) return false;
    const auto key = "guild:" + guildId + ":" + channelId + ":" + m_currentUserId;
    if (m_broadcastStream == key) return true;
    stopBroadcast();
    m_broadcastStream = key;
    sendGateway(18, QJsonObject{{"type", "guild"}, {"guild_id", guildId}, {"channel_id", channelId},
                               {"preferred_region", QJsonValue::Null}});
    return true;
}

void DiscordClient::stopBroadcast() {
    if (m_broadcastStream.isEmpty()) return;
    const auto key = m_broadcastStream;
    m_broadcastStream.clear();
    m_streamObjects.remove(key);
    rebuildVoiceMembers();
    if (m_connected && !m_demoMode) sendGateway(19, QJsonObject{{"stream_key", key}});
    emit broadcastDeleted(QJsonObject{{"stream_key", key}});
}

bool DiscordClient::sendVoiceState(const QString &guildId, const QString &channelId, bool mute, bool deaf) {
    if (!validId(guildId) || (!channelId.isEmpty() && !validId(channelId)))
        return rejectVoiceState(guildId,channelId,"Choose an available voice channel before joining.");
    if (m_token.isEmpty() || m_demoMode || !validId(m_currentUserId)) {
        // Normal logout has already discarded its account's pending intent.
        if (channelId.isEmpty()) return false;
        return rejectVoiceState(guildId,channelId,"Sign in before joining a voice channel.");
    }
    if (!m_connected && !m_connecting)
        return rejectVoiceState(guildId,channelId,"Reconnect to Discord before changing voice channels.");
    const bool privateCall = privateVoiceAllocation(guildId);
    if (privateCall && !channelId.isEmpty() && channelId != guildId)
        return rejectVoiceState(guildId,channelId,"This private call does not match its conversation.");
    if (!channelId.isEmpty() && m_removedGuilds.contains(guildId))
        return rejectVoiceState(guildId,channelId,"This server is no longer available to this account.");
    if (!privateCall && !channelId.isEmpty()) for (const auto &entry : m_channelCache.value(guildId)) {
        const auto channel = entry.toObject();
        if (channel.value("id").toString() == channelId && !channelVisible(channel, guildId))
            return rejectVoiceState(guildId,channelId,"This voice channel is not accessible to this account.");
    }
    const QJsonObject state{{"guild_id", guildId},
        {"channel_id", channelId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(channelId)},
        {"self_mute", mute || deaf}, {"self_deaf", deaf}, {"self_video", false}};
    if (privateCall && channelId.isEmpty()) {
        if (!m_privateLeaveAllocations.contains(guildId) && m_privateLeaveAllocations.size() < 32) m_privateLeaveAllocations.enqueue(guildId);
        if (m_directCallChannel == guildId) { m_directCallChannel.clear(); m_directCallRecipient.clear(); ++m_directCallOperation; }
        if (m_outgoingRinging.remove(guildId) && m_connected)
            request("POST", "/channels/" + guildId + "/call/stop-ringing", {}, [](const QJsonValue &, const QString &) {});
    }
    cancelPendingVoiceState(guildId,channelId);
    m_pendingVoiceState = state; m_pendingVoiceAccount = m_currentUserId; m_pendingVoiceGeneration = m_generation;
    if (m_connected && sendGateway(4,wireVoiceState(state))) {
        m_pendingVoiceState = {}; m_pendingVoiceAccount.clear(); return true;
    }
    // Only explicit user/voice-engine intent is retained. Resuming the main
    // gateway must not restart an otherwise healthy independent voice socket.
    if (m_connected) reconnect(false);
    if (m_connecting) return true;
    return rejectVoiceState(guildId,channelId,"Discord could not send the voice request. Reconnect and try again.");
}

bool DiscordClient::rejectVoiceState(const QString &guildId,const QString &channelId,const QString &reason) {
    cancelPendingVoiceState(guildId,channelId);
    emit voiceStateRequestRejected(guildId,channelId,reason);
    setError(reason); return false;
}

void DiscordClient::cancelPendingVoiceState(const QString &replacementGuild,const QString &replacementChannel) {
    const auto previous = m_pendingVoiceState;
    const bool sameAccount = m_pendingVoiceGeneration == m_generation && m_pendingVoiceAccount == m_currentUserId;
    m_pendingVoiceState = {}; m_pendingVoiceAccount.clear();
    if (previous.isEmpty() || !sameAccount) return;
    const auto guild = previous.value("guild_id").toString(), channel = previous.value("channel_id").toString();
    // Mute/deafen changes retain the same allocation/leave acknowledgement.
    if (guild != replacementGuild || channel != replacementChannel)
        emit voiceStateRequestRejected(guild,channel,"Voice request superseded.");
}

void DiscordClient::flushPendingVoiceState() {
    if (m_pendingVoiceState.isEmpty()) return;
    const auto state = m_pendingVoiceState;
    const bool sameAccount = m_pendingVoiceGeneration == m_generation && m_pendingVoiceAccount == m_currentUserId;
    m_pendingVoiceState = {}; m_pendingVoiceAccount.clear();
    if (!sameAccount) return;
    sendVoiceState(state.value("guild_id").toString(),state.value("channel_id").toString(),
                   state.value("self_mute").toBool(),state.value("self_deaf").toBool());
}

void DiscordClient::loadDemo() {
    logout();
    m_demoMode = true;
    m_currentUserId = "1000001";
    m_currentUserName = "You";
    m_status = "Demo · messages stay on this device";
    const QJsonArray studioChannels{
        QJsonObject{{"id", "2100"}, {"type", 4}, {"name", "THE COMMON ROOM"}, {"position", 0}},
        QJsonObject{{"id", "2101"}, {"type", 0}, {"name", "general"}, {"parent_id", "2100"}, {"position", 0},
            {"topic", "Good conversations. A little less noise."}},
        QJsonObject{{"id", "2102"}, {"type", 0}, {"name", "projects"}, {"parent_id", "2100"}, {"position", 1},
            {"topic", "Share what you’re making."}},
        QJsonObject{{"id", "2103"}, {"type", 0}, {"name", "screenshots"}, {"parent_id", "2100"}, {"position", 2}},
        QJsonObject{{"id", "2200"}, {"type", 4}, {"name", "VOICE LOUNGE"}, {"position", 1}},
        QJsonObject{{"id", "2201"}, {"type", 2}, {"name", "Lounge"}, {"parent_id", "2200"}, {"position", 0}},
        QJsonObject{{"id", "2202"}, {"type", 2}, {"name", "Studio"}, {"parent_id", "2200"}, {"position", 1}}
    };
    acceptGuild(QJsonObject{{"id", "2000"}, {"name", "The Workshop"}, {"channels", studioChannels}});
    acceptGuild(QJsonObject{{"id", "3000"}, {"name", "After Hours"}, {"channels", QJsonArray{
        QJsonObject{{"id", "3101"}, {"type", 0}, {"name", "hangout"}, {"position", 0}},
        QJsonObject{{"id", "3102"}, {"type", 2}, {"name", "The late shift"}, {"position", 1}}}}});
    acceptGuild(QJsonObject{{"id", "4000"}, {"name", "Friends"}, {"channels", QJsonArray{
        QJsonObject{{"id", "4101"}, {"type", 0}, {"name", "general"}, {"position", 0}},
        QJsonObject{{"id", "4102"}, {"type", 2}, {"name", "Hangout"}, {"position", 1}}}}});
    acceptDm(QJsonObject{{"id", "5001"}, {"type", 1}, {"recipients", QJsonArray{
        QJsonObject{{"id", "1000002"}, {"username", "Maya"}}}}});
    acceptDm(QJsonObject{{"id", "5002"}, {"type", 1}, {"recipients", QJsonArray{
        QJsonObject{{"id", "1000003"}, {"username", "Alex"}}}}});
    m_unread.insert("5001",3); m_unread.insert("5002",1);
    syncDm("5001"); syncDm("5002");
    ++m_socialBatchDepth;
    acceptRelationship({{"id", "1000002"}, {"type", 1}});
    acceptRelationship({{"id", "1000003"}, {"type", 1}});
    acceptRelationship({{"id", "1000004"}, {"type", 3}, {"user", QJsonObject{{"id", "1000004"}, {"username", "Sam"}}}});
    acceptRelationship({{"id", "1000005"}, {"type", 4}, {"user", QJsonObject{{"id", "1000005"}, {"username", "Jordan"}}}});
    acceptPresence({}, {{"user_id", "1000002"}, {"status", "online"}, {"activities", QJsonArray{QJsonObject{{"type", 0}, {"name", "Stardew Valley"}}}}});
    acceptPresence({}, {{"user_id", "1000003"}, {"status", "idle"}});
    acceptDm({{"id", "5003"}, {"type", 1}, {"is_message_request", true}, {"is_message_request_timestamp", "2026-10-25T12:00:00Z"},
        {"recipients", QJsonArray{QJsonObject{{"id", "1000006"}, {"username", "Taylor"}}}}});
    acceptDm({{"id", "5004"}, {"type", 1}, {"is_message_request", true}, {"is_spam", true},
        {"recipients", QJsonArray{QJsonObject{{"id", "1000007"}, {"username", "Unknown sender"}}}}});
    --m_socialBatchDepth; rebuildSocial();
    selectGuild("2000");
    emit stateChanged();
    emit modelsReady();
}

void DiscordClient::loadDemoMessages() {
    m_messages.clear();
    m_messageObjects.clear();
    m_hasMoreMessages = false;
    const auto base = QDateTime::currentDateTimeUtc().addSecs(-3600);
    struct Sample { const char *id; const char *name; const char *content; };
    const QList<Sample> samples = {
        {"1000002", "Maya", "Anyone around this evening?"},
        {"1000003", "Alex", "Yep, I should be free after seven."},
        {"1000004", "Sam", "I'll be in Lounge. Come hang out whenever."},
        {"1000002", "Maya", "Perfect. I've got a few things to show you all."},
        {"1000001", "You", "Sounds good, see you there."},
        {"1000003", "Alex", "See you soon!"}
    };
    int i = 0;
    for (const auto &sample : samples) {
        const auto content = m_currentChannelId == "2101" ? QString::fromUtf8(sample.content) :
            (i == 0 ? QStringLiteral("Welcome to %1. This is a local demonstration conversation.").arg(m_currentChannelName) :
                      QString::fromUtf8(sample.content));
        QJsonObject message{{"id", QString::number(9000000 + i)}, {"channel_id", m_currentChannelId},
            {"author", QJsonObject{{"id", sample.id}, {"username", sample.name}}}, {"content", content},
            {"timestamp", base.addSecs(i * 240).toString(Qt::ISODateWithMs)}};
        acceptMessage(message);
        ++i;
    }
}
