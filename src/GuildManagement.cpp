#include "GuildManagement.h"
#include "DiscordClient.h"
#include <QBuffer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>
#include <algorithm>
#include <limits>
#include <cmath>

namespace {
constexpr quint64 Invite=1ULL, Kick=1ULL<<1, Ban=1ULL<<2, Admin=1ULL<<3, ManageChannels=1ULL<<4,
    ManageGuild=1ULL<<5, Audit=1ULL<<7, View=1ULL<<10, Connect=1ULL<<20, Mute=1ULL<<22,
    Deafen=1ULL<<23, Move=1ULL<<24, ManageRoles=1ULL<<28, ManageEvents=1ULL<<33,
    Moderate=1ULL<<40, CreateEvents=1ULL<<44;
struct Permission { const char *key; const char *label; quint64 bit; };
const QList<Permission> &permissionOptions() {
    static const QList<Permission> options{
        {"administrator","Administrator",Admin},{"manageGuild","Manage server",ManageGuild},
        {"manageChannels","Manage channels",ManageChannels},{"manageRoles","Manage roles",ManageRoles},
        {"viewAuditLog","View audit log",Audit},{"viewChannel","View channels",View},
        {"createInvite","Create invites",Invite},{"sendMessages","Send messages",1ULL<<11},
        {"manageMessages","Manage messages",1ULL<<13},{"embedLinks","Embed links",1ULL<<14},
        {"attachFiles","Attach files",1ULL<<15},{"readHistory","Read message history",1ULL<<16},
        {"mentionEveryone","Mention everyone",1ULL<<17},{"useCommands","Use app commands",1ULL<<31},
        {"connect","Connect to voice",Connect},{"speak","Speak",1ULL<<21},
        {"stream","Video and streaming",1ULL<<9},{"voiceActivity","Use voice activity",1ULL<<25},
        {"muteMembers","Mute members",Mute},{"deafenMembers","Deafen members",Deafen},
        {"moveMembers","Move members",Move},{"kickMembers","Kick members",Kick},
        {"banMembers","Ban members",Ban},{"moderateMembers","Time out members",Moderate},
        {"createEvents","Create events",CreateEvents},{"manageEvents","Manage events",ManageEvents}};
    return options;
}
bool idValid(const QString &id) { return QRegularExpression("^[0-9]{1,21}$").match(id).hasMatch(); }
QString text(QString value,int limit) { value.remove(QChar::Null); value.replace(QRegularExpression("[\\x01-\\x08\\x0b\\x0c\\x0e-\\x1f]"),{}); return value.left(limit); }
QJsonObject merge(QJsonObject a,const QJsonObject &b) { for(auto it=b.begin();it!=b.end();++it)a.insert(it.key(),it.value());return a; }
quint64 memberBits(const QJsonObject &guild,const QJsonObject &member) {
    quint64 bits=0; const auto ids=member.value("roles").toArray();
    for(const auto &entry:guild.value("roles").toArray()) {
        const auto role=entry.toObject();
        if(role.value("id")==guild.value("id") || ids.contains(role.value("id"))) bits|=role.value("permissions").toString().toULongLong();
    }
    return bits;
}
QJsonObject topRole(const QJsonObject &guild,const QJsonObject &member) {
    QJsonObject top; const auto ids=member.value("roles").toArray();
    for(const auto &entry:guild.value("roles").toArray()) {
        const auto role=entry.toObject(); if(!ids.contains(role.value("id")))continue;
        const int p=role.value("position").toInt(),old=top.value("position").toInt(-1);
        if(p>old || (p==old && role.value("id").toString().toULongLong()<top.value("id").toString().toULongLong()))top=role;
    }
    return top;
}
bool above(const QJsonObject &a,const QJsonObject &b) {
    if(a.isEmpty())return false;
    if(b.isEmpty())return true;
    if(a.value("id")==b.value("id"))return false;
    const int ap=a.value("position").toInt(),bp=b.value("position").toInt();
    return ap>bp || (ap==bp && a.value("id").toString().toULongLong()<b.value("id").toString().toULongLong());
}
QString createdAt(const QString &id) { bool ok=false;const auto n=id.toULongLong(&ok);return ok?QDateTime::fromMSecsSinceEpoch(qint64(n>>22)+1420070400000LL,QTimeZone::UTC).toString(Qt::ISODate):QString{}; }
}

GuildManagement::GuildManagement(DiscordClient *client,QObject *parent):QObject(parent),m_client(client) {
    if(!client)return;
    m_session=client->m_generation;
    connect(client,&DiscordClient::stateChanged,this,[this] {
        if(!m_client)return;
        if(m_session!=m_client->m_generation) { m_session=m_client->m_generation;reset(); }
        else emit changed();
    });
    connect(client,&DiscordClient::guildRemoved,this,[this](const QString &id) {
        m_pendingLeaves.remove(id);
        if (id == m_guildId) reset();
    });
}
void GuildManagement::reset() {
    ++m_generation;++m_readOperation;++m_writeOperation;m_busy=m_mutating=m_hasMore=m_refreshPending=false;
    m_guildId.clear();m_guild={};m_items.clear();m_members.clear();m_events.clear();m_cursor.clear();m_error.clear();m_status.clear();emit changed();
    m_pendingLeaves.clear();
}
bool GuildManagement::ready() const { return m_client && m_client->connected() && !m_client->demoMode() && !m_client->m_token.isEmpty() && idValid(m_guildId); }
bool GuildManagement::fail(const QString &reason) { m_error=reason;emit changed();return false; }
QJsonObject GuildManagement::snapshot(const QString &id) const {
    if(!m_client)return {};
    auto data=id==m_guildId?m_guild:QJsonObject{};
    return merge(data,m_client->m_guildObjects.value(id));
}
QJsonObject GuildManagement::member(const QString &guildId,const QString &userId) const {
    if(!m_client)return {};
    auto data=guildId==m_guildId?m_members.value(userId):QJsonObject{};
    if(userId==m_client->currentUserId())data=merge(snapshot(guildId).value("member").toObject(),data);
    data=merge(data,m_client->m_guildMembers.value(guildId).value(userId));
    return data;
}
QJsonObject GuildManagement::channel(const QString &guildId,const QString &channelId) const {
    if(m_client)for(const auto &entry:m_client->m_channelCache.value(guildId))
        if(entry.toObject().value("id").toString()==channelId)return entry.toObject();
    return {};
}
QJsonObject GuildManagement::role(const QString &id) const {
    for(const auto &entry:snapshot(m_guildId).value("roles").toArray())if(entry.toObject().value("id").toString()==id)return entry.toObject();
    return {};
}
quint64 GuildManagement::permissionBits(const QString &guildId,const QString &channelId) const {
    if(!m_client)return 0;
    const auto data=snapshot(guildId);const auto self=m_client->currentUserId();const auto own=member(guildId,self);
    if(!channelId.isEmpty() && channel(guildId,channelId).isEmpty())return 0;
    if(data.value("owner_id").toString()==self && !self.isEmpty())return std::numeric_limits<quint64>::max();
    quint64 bits=own.isEmpty()?data.value("permissions").toString().toULongLong():memberBits(data,own);
    if(bits&Admin)return std::numeric_limits<quint64>::max();
    if(channelId.isEmpty())return bits;
    const auto target=channel(guildId,channelId);if(target.isEmpty())return 0;
    quint64 roleDeny=0,roleAllow=0,selfDeny=0,selfAllow=0;
    const auto ids=own.value("roles").toArray();
    for(const auto &entry:target.value("permission_overwrites").toArray()) {
        const auto overwrite=entry.toObject();const auto id=overwrite.value("id").toString();
        const auto deny=overwrite.value("deny").toString().toULongLong(),allow=overwrite.value("allow").toString().toULongLong();
        if(overwrite.value("type").toInt()==0) {
            if(id==guildId)bits=(bits&~deny)|allow;
            else if(ids.contains(id)){roleDeny|=deny;roleAllow|=allow;}
        }else if(id==self){selfDeny|=deny;selfAllow|=allow;}
    }
    bits=(bits&~roleDeny)|roleAllow;bits=(bits&~selfDeny)|selfAllow;
    if(!(bits&View))return 0;
    return bits;
}
QVariantMap GuildManagement::capabilities() const {
    const auto bits=permissionBits(m_guildId);QVariantMap result;
    for(const auto &p:permissionOptions())result.insert(p.key,bool(bits&p.bit));
    result.insert("owner",m_client && snapshot(m_guildId).value("owner_id").toString()==m_client->currentUserId() && !m_client->currentUserId().isEmpty());
    const bool owner = result.value("owner").toBool();
    const bool knownOwner = !snapshot(m_guildId).value("owner_id").toString().isEmpty();
    const bool member = m_client && m_client->m_guildObjects.contains(m_guildId);
    result.insert("leaveGuild", ready() && member && knownOwner && !owner && m_pendingLeaves.isEmpty());
    result.insert("leaveReason", owner ? "Transfer ownership before leaving this server." :
        !knownOwner ? "Wait for the server details to load." : !ready() ? "Reconnect to Discord before leaving." : QString{});
    result.insert("manageInvites",bool(bits&ManageGuild));return result;
}
QVariantMap GuildManagement::userPermissions(const QString &guildId,const QString &userId) const {
    const auto data=snapshot(guildId),target=member(guildId,userId);const auto self=m_client?m_client->currentUserId():QString{};
    const auto bits=permissionBits(guildId);const bool owner=data.value("owner_id").toString()==self && !self.isEmpty();
    const bool protectedUser=userId==self || userId==data.value("owner_id").toString() || target.isEmpty();
    const bool hierarchy=!protectedUser && (owner || above(topRole(data,member(guildId,self)),topRole(data,target)));
    const auto voice=m_client?m_client->m_voiceStates.value(guildId).value(userId):QJsonObject{};
    const auto voiceChannel=voice.value("channel_id").toString();const auto voiceBits=voiceChannel.isEmpty()?quint64(0):permissionBits(guildId,voiceChannel);
    return {{"kick",hierarchy && bool(bits&Kick)},{"ban",hierarchy && bool(bits&Ban)},
        {"timeout",hierarchy && bool(bits&Moderate) && !(memberBits(data,target)&Admin)},
        {"manageRoles",!target.isEmpty() && bool(bits&ManageRoles)},
        {"mute",!protectedUser && bool(voiceBits&Mute)},{"deafen",!protectedUser && bool(voiceBits&Deafen)},
        {"move",!protectedUser && bool(voiceBits&Move)}, {"roleIds",target.value("roles").toArray().toVariantList()},
        {"muted",voice.value("muted").toBool()},{"deafened",voice.value("deafened").toBool()},
        {"serverMuted",voice.value("server_muted").toBool()},{"serverDeafened",voice.value("server_deafened").toBool()},
        {"reason",target.isEmpty()?"Load this member before using moderation actions.":protectedUser?"You cannot moderate yourself or the server owner.":!hierarchy?"This member's highest role is at or above yours.":QString{}}};
}
QVariantMap GuildManagement::channelPermissions(const QString &guildId,const QString &channelId) const {
    const auto bits=permissionBits(guildId,channelId);
    return {{"manage",bool(bits&ManageChannels)},{"managePermissions",bool(bits&ManageRoles)},{"createInvite",bool(bits&Invite)},{"view",bool(bits&View)},{"connect",bool(bits&Connect)}};
}
bool GuildManagement::roleEditable(const QJsonObject &target) const {
    if(target.isEmpty() || !idValid(target.value("id").toString()) || target.value("managed").toBool() || !(permissionBits(m_guildId)&ManageRoles))return false;
    const auto data=snapshot(m_guildId);const auto self=m_client->currentUserId();
    return data.value("owner_id").toString()==self || above(topRole(data,member(m_guildId,self)),target);
}
QVariantList GuildManagement::roles() const {
    QVariantList rows;
    for(const auto &entry:snapshot(m_guildId).value("roles").toArray()) {
        if(rows.size()>=250)break;
        const auto role=entry.toObject();const auto bits=role.value("permissions").toString().toULongLong();
        QVariantList options;for(const auto &p:permissionOptions())options.append(QVariantMap{{"key",p.key},{"label",p.label},{"enabled",bool(bits&p.bit)}});
        const int color=qBound(0,role.value("color").toInt(),0xffffff);
        rows.append(QVariantMap{{"id",role.value("id").toString()},{"name",text(role.value("name").toString(),100)},
            {"color",color?QString("#%1").arg(color,6,16,QChar('0')):QString{}},{"colorValue",color},
            {"position",role.value("position").toInt()},{"permissions",QString::number(bits)},
            {"managed",role.value("managed").toBool()},{"hoist",role.value("hoist").toBool()},{"mentionable",role.value("mentionable").toBool()},
            {"editable",roleEditable(role)},{"assignable",role.value("id").toString()!=m_guildId && roleEditable(role)},{"permissionOptions",options}});
    }
    std::sort(rows.begin(),rows.end(),[](const QVariant&a,const QVariant&b){return a.toMap().value("position").toInt()>b.toMap().value("position").toInt();});
    return rows;
}
QVariantMap GuildManagement::guild() const {
    const auto data=snapshot(m_guildId);QVariantMap result;
    const QMap<QString,QString> keys{{"id","id"},{"name","name"},{"description","description"},{"ownerId","owner_id"},
        {"memberCount","member_count"},{"premiumTier","premium_tier"},{"premiumSubscriptionCount","premium_subscription_count"},
        {"preferredLocale","preferred_locale"},{"verificationLevel","verification_level"},{"explicitContentFilter","explicit_content_filter"},
        {"defaultMessageNotifications","default_message_notifications"},{"afkChannelId","afk_channel_id"},{"afkTimeout","afk_timeout"}};
    for(auto it=keys.cbegin();it!=keys.cend();++it)if(data.contains(it.value()))result.insert(it.key(),data.value(it.value()).toVariant());
    const auto features=data.value("features").toArray();result.insert("features",features.toVariantList());
    result.insert("channels",channels());
    const int tier=data.value("premium_tier").toInt();
    result.insert("maxBitrate",features.contains("VIP_REGIONS") || tier>=3?384000:tier==2?256000:tier==1?128000:96000);
    for(const auto &kind:{"icon","banner"}) {
        const auto hash=data.value(kind).toString();QString url;
        if(idValid(m_guildId) && QRegularExpression("^[A-Za-z0-9_]{1,128}$").match(hash).hasMatch())
            url=QString("https://cdn.discordapp.com/%1/%2/%3.png?size=4096").arg(QString(kind)=="icon"?"icons":"banners",m_guildId,hash);
        result.insert(QString(kind)+"Url",url);
    }
    return result;
}
void GuildManagement::acceptGuild(const QJsonObject &data) {
    static const QStringList fields{"id","name","description","owner_id","icon","banner","member_count","approximate_member_count","roles","member","permissions",
        "premium_tier","premium_subscription_count","features","preferred_locale","verification_level","explicit_content_filter","default_message_notifications","afk_channel_id","afk_timeout"};
    for(const auto &key:fields)if(data.contains(key)) {
        auto value=data.value(key);
        if(key=="roles" || key=="features") {QJsonArray bounded;for(const auto &entry:value.toArray()){if(bounded.size()>=(key=="roles"?250:100))break;bounded.append(entry);}value=bounded;}
        else if(value.isString())value=text(value.toString(),key=="description"?1000:128);
        m_guild.insert(key,value);
    }
    if(!m_guild.contains("member_count") && m_guild.contains("approximate_member_count"))m_guild.insert("member_count",m_guild.value("approximate_member_count"));
    if(m_client && m_client->m_guildObjects.contains(m_guildId)) {
        auto update=data;update.insert("id",m_guildId);m_client->acceptGuild(update);
    }
}
void GuildManagement::selectGuild(const QString &id) {
    if(id==m_guildId)return;
    if(!m_client || !idValid(id) || !m_client->m_guildObjects.contains(id)){fail("Choose a server you belong to.");return;}
    ++m_generation;++m_readOperation;++m_writeOperation;m_busy=m_mutating=m_hasMore=m_refreshPending=false;m_items.clear();m_members.clear();m_events.clear();m_cursor.clear();m_error.clear();
    m_guildId=id;m_guild=m_client->m_guildObjects.value(id);m_section="overview";emit changed();fetchOverview();
}
void GuildManagement::fetchOverview() {
    if(m_mutating){m_refreshPending=true;return;}
    if(!ready())return;
    const auto generation=m_generation,operation=++m_readOperation;const auto id=m_guildId;QPointer<GuildManagement> guard(this);
    m_busy=true;m_error.clear();m_status="Loading server…";emit changed();
    m_client->request("GET","/guilds/"+id+"?with_counts=true",{},[guard,generation,operation](const QJsonValue &value,const QString &error) {
        if(!guard || guard->m_generation!=generation || guard->m_readOperation!=operation)return;
        guard->m_busy=false;
        if(!error.isEmpty())guard->m_error=error;
        else if(value.toObject().value("id").toString()==guard->m_guildId)guard->acceptGuild(value.toObject());
        else guard->m_error="Discord returned incomplete server details.";
        guard->m_status=guard->m_error.isEmpty()?"Server details loaded":QString{};emit guard->changed();
    });
    if(!m_client->m_channelCache.contains(id))m_client->request("GET","/guilds/"+id+"/channels",{},[guard,generation,operation,id](const QJsonValue &value,const QString &error) {
        if(!guard || guard->m_generation!=generation || guard->m_readOperation!=operation || !error.isEmpty() || !value.isArray())return;
        guard->m_client->cacheChannels(id,value.toArray());emit guard->changed();
    });
}
void GuildManagement::refresh() { if(m_section=="overview" || m_section=="boosts")fetchOverview();else fetchItems(false); }
void GuildManagement::loadSection(const QString &section) {
    if(!QStringList{"overview","boosts","roles","members","invites","bans","audit","events"}.contains(section))return;
    ++m_readOperation;m_section=section;m_items.clear();m_cursor.clear();m_hasMore=false;m_error.clear();
    if(section=="overview" || section=="boosts")fetchOverview();else fetchItems(false);
}
void GuildManagement::loadMore() {if(m_hasMore && !m_busy)fetchItems(true);}
QVariantMap GuildManagement::itemRow(const QString &section,const QJsonObject &object) const {
    if(section=="members" || section=="bans") {
        const auto user=object.value("user").toObject();const auto id=user.value("id").toString();
        auto name=text(object.value("nick").toString(),128);if(name.isEmpty())name=DiscordClient::displayName(user);
        QVariantMap row{{"id",id},{"userId",id},{"name",name},{"username",text(user.value("username").toString(),128)},
            {"avatarUrl",DiscordClient::avatarUrl(user)},{"bot",user.value("bot").toBool()},{"reason",text(object.value("reason").toString(),512)}};
        if(section=="members") {
            row.insert("nick",text(object.value("nick").toString(),128));row.insert("roles",object.value("roles").toArray().toVariantList());
            row.insert("roleIds",object.value("roles").toArray().toVariantList());row.insert("joinedAt",object.value("joined_at").toString());
            row.insert("premiumSince",object.value("premium_since").toString());row.insert("permissions",userPermissions(m_guildId,id));
            QStringList names;for(const auto &role:roles())if(object.value("roles").toArray().contains(role.toMap().value("id").toString()))names.append(role.toMap().value("name").toString());
            row.insert("roleNames",names);
        }
        return row;
    }
    if(section=="invites") {
        const auto code=object.value("code").toString();const bool valid=QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(code).hasMatch();
        return {{"id",valid?code:QString{}},{"code",valid?code:QString{}},{"url",valid?"https://discord.gg/"+code:QString{}},
            {"channelId",object.value("channel").toObject().value("id").toString()},{"channelName",text(object.value("channel").toObject().value("name").toString(),128)},
            {"inviterName",DiscordClient::displayName(object.value("inviter").toObject())},{"uses",object.value("uses").toInt()},
            {"maxUses",object.value("max_uses").toInt()},{"maxAge",object.value("max_age").toInt()},
            {"temporary",object.value("temporary").toBool()},{"createdAt",object.value("created_at").toString()}};
    }
    if(section=="audit") {
        const auto actor=object.value("user_id").toString();const auto user=m_client?m_client->m_users.value(actor):QJsonObject{};
        static const QHash<int,QString> actions{{1,"Server updated"},{10,"Channel created"},{11,"Channel updated"},{12,"Channel deleted"},{13,"Channel permission added"},{14,"Channel permission updated"},{15,"Channel permission removed"},{20,"Member kicked"},{21,"Members pruned"},{22,"Member banned"},{23,"Member unbanned"},{24,"Member updated"},{25,"Member roles updated"},{26,"Member moved"},{27,"Member disconnected"},{28,"Bot added"},{30,"Role created"},{31,"Role updated"},{32,"Role deleted"},{40,"Invite created"},{41,"Invite updated"},{42,"Invite deleted"},{50,"Webhook created"},{51,"Webhook updated"},{52,"Webhook deleted"},{60,"Emoji created"},{61,"Emoji updated"},{62,"Emoji deleted"},{72,"Message deleted"},{73,"Messages deleted"},{74,"Message pinned"},{75,"Message unpinned"},{100,"Scheduled event created"},{101,"Scheduled event updated"},{102,"Scheduled event deleted"},{110,"Thread created"},{111,"Thread updated"},{112,"Thread deleted"}};
        const int type=object.value("action_type").toInt();const auto target=object.value("target_id").toString();
        auto targetName=text(channel(m_guildId,target).value("name").toString(),128);
        if(targetName.isEmpty())targetName=text(role(target).value("name").toString(),128);
        if(targetName.isEmpty() && m_client && m_client->m_users.contains(target))targetName=DiscordClient::displayName(m_client->m_users.value(target));
        QVariantList changes;
        for(const auto &entry:object.value("changes").toArray()) {
            if(changes.size()>=25)break;
            const auto change=entry.toObject();
            auto render=[](const QJsonValue &value){if(value.isString())return text(value.toString(),512);if(value.isUndefined())return QString{};return text(QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)),512);};
            const auto key=text(change.value("key").toString(),100);
            changes.append(QVariantMap{{"key",key},{"before",key.contains("token",Qt::CaseInsensitive)?QString("Hidden"):render(change.value("old_value"))},{"after",key.contains("token",Qt::CaseInsensitive)?QString("Hidden"):render(change.value("new_value"))}});
        }
        return {{"id",object.value("id").toString()},{"actionType",type},{"actionName",actions.value(type,QString("Action %1").arg(type))},
            {"actorId",actor},{"actorName",user.isEmpty()?QString("Unknown member"):DiscordClient::displayName(user)},
            {"targetId",target},{"targetName",targetName},{"changes",changes},{"reason",text(object.value("reason").toString(),512)},{"createdAt",createdAt(object.value("id").toString())}};
    }
    if(section=="events") {
        QVariantMap row{{"id",object.value("id").toString()},{"name",text(object.value("name").toString(),100)},
            {"description",text(object.value("description").toString(),1000)},{"scheduledStartTime",object.value("scheduled_start_time").toString()},
            {"scheduledEndTime",object.value("scheduled_end_time").toString()},{"entityType",object.value("entity_type").toInt()},
            {"channelId",object.value("channel_id").toString()},{"location",text(object.value("entity_metadata").toObject().value("location").toString(),100)},
            {"status",object.value("status").toInt()},{"creatorId",object.value("creator_id").toString()},{"interestKnown",object.contains("current_user_interested")}};
        const auto bits=object.value("entity_type").toInt()==3?permissionBits(m_guildId):permissionBits(m_guildId,object.value("channel_id").toString());
        const auto entity=object.value("entity_type").toInt();
        const quint64 required=entity==1?(ManageChannels|Mute|Move):entity==2?(View|Connect):0;
        const bool manages=(bits&required)==required && (bool(bits&ManageEvents) || (m_client && object.value("creator_id").toString()==m_client->currentUserId() && bool(bits&CreateEvents)));
        row.insert("canEdit",manages && object.value("status").toInt()<3);row.insert("canDelete",manages);
        if(object.contains("user_count"))row.insert("userCount",object.value("user_count").toInt());
        if(object.contains("current_user_interested"))row.insert("currentUserInterested",object.value("current_user_interested").toBool());
        return row;
    }
    return {};
}
void GuildManagement::fetchItems(bool more) {
    if(m_mutating){m_refreshPending=true;return;}
    if(!ready()){fail("Reconnect to Discord to load server settings.");return;}
    const auto caps=capabilities();
    if((m_section=="invites" && !caps.value("manageInvites").toBool()) ||
       (m_section=="bans" && !caps.value("banMembers").toBool() && !caps.value("viewAuditLog").toBool()) ||
       (m_section=="audit" && !caps.value("viewAuditLog").toBool())) {m_busy=false;fail("Your role does not permit viewing this section.");return;}
    QString endpoint;
    if(m_section=="roles")endpoint="/roles";
    else if(m_section=="members")endpoint="/members?limit=100"+(more && !m_cursor.isEmpty()?"&after="+m_cursor:QString{});
    else if(m_section=="invites")endpoint="/invites";
    else if(m_section=="bans")endpoint="/bans?limit=100"+(more && !m_cursor.isEmpty()?"&after="+m_cursor:QString{});
    else if(m_section=="audit")endpoint="/audit-logs?limit=50"+(more && !m_cursor.isEmpty()?"&before="+m_cursor:QString{});
    else if(m_section=="events")endpoint="/scheduled-events?with_user_count=true";
    else return;
    const auto operation=++m_readOperation,generation=m_generation;const auto section=m_section;
    if(!more){m_items.clear();m_cursor.clear();m_hasMore=false;}
    m_busy=true;m_error.clear();m_status="Loading "+section+"…";emit changed();QPointer<GuildManagement> guard(this);
    m_client->request("GET","/guilds/"+m_guildId+endpoint,{},[guard,operation,generation,section,more](const QJsonValue &value,const QString &error) {
        if(!guard || guard->m_generation!=generation || guard->m_readOperation!=operation)return;
        auto *self=guard.data();self->m_busy=false;
        if(!error.isEmpty()){self->m_error=error;self->m_status.clear();emit self->changed();return;}
        const auto payload=section=="audit"?value.toObject().value("audit_log_entries"):value;
        if(!payload.isArray()){self->fail("Discord returned an unreadable list.");return;}
        const auto array=payload.toArray();
        const auto previousEvents=self->m_events;
        if(!more && section=="members")self->m_members.clear();
        if(!more && section=="events")self->m_events.clear();
        if(section=="audit") {
            int count=0;for(const auto &entry:value.toObject().value("users").toArray()) {
                if(count++>=500)break;
                self->m_client->rememberMember(QJsonObject{{"user",entry}});
            }
        }
        if(section=="roles") {
            self->acceptGuild(QJsonObject{{"id",self->m_guildId},{"roles",array}});self->m_items=self->roles();
        } else {
            for(const auto &entry:array) {
                if(self->m_items.size()>=500)break;
                auto object=entry.toObject();
                if(section=="members") {
                    const auto id=object.value("user").toObject().value("id").toString();if(!idValid(id))continue;
                    if(self->m_members.contains(id) || self->m_members.size()<500)self->m_members.insert(id,object);
                    self->m_client->cacheMember(self->m_guildId,object);
                }
                if(section=="events") {
                    const auto id=object.value("id").toString();if(!idValid(id))continue;
                    if(!object.contains("current_user_interested") && previousEvents.value(id).contains("current_user_interested"))object.insert("current_user_interested",previousEvents.value(id).value("current_user_interested"));
                    if(self->m_events.contains(id) || self->m_events.size()<500)self->m_events.insert(id,object);
                }
                const auto row=self->itemRow(section,object);if(row.value("id").toString().isEmpty())continue;
                bool duplicate=false;for(const auto &existing:self->m_items)if(existing.toMap().value("id")==row.value("id")){duplicate=true;break;}
                if(!duplicate)self->m_items.append(row);
            }
        }
        const bool pageable=section=="members" || section=="bans" || section=="audit";
        self->m_hasMore=pageable && array.size()>=(section=="audit"?50:100) && self->m_items.size()<500;
        if(!array.isEmpty()) {
            const auto last=array.last().toObject();self->m_cursor=section=="audit"?last.value("id").toString():last.value("user").toObject().value("id").toString();
            if(!idValid(self->m_cursor))self->m_hasMore=false;
        }
        self->m_status=QString("%1 %2 loaded%3").arg(self->m_items.size()).arg(section,self->m_hasMore?" · more available":self->m_items.size()>=500?" · display limit reached":QString{});
        emit self->changed();
    });
}

bool GuildManagement::imageData(const QVariant &source,QString &data) {
    const auto input=source.toString();if(input.isEmpty()){data.clear();return true;}
    const QUrl url(input);const auto path=url.isLocalFile()?url.toLocalFile():input;
    const QFileInfo file(path);
    if(!file.isAbsolute() || !file.isFile() || file.size()<=0 || file.size()>8*1024*1024)return fail("Choose a local PNG, JPEG, GIF or WebP image smaller than 8 MB.");
    QFile stream(path);if(!stream.open(QIODevice::ReadOnly))return fail("The selected image could not be opened.");
    const auto bytes=stream.read(8*1024*1024+1);if(bytes.size()>8*1024*1024)return fail("The image is too large.");
    QBuffer buffer;buffer.setData(bytes);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer);
    const auto format=reader.format().toLower();
    if(!QList<QByteArray>{"png","jpeg","jpg","gif","webp"}.contains(format))return fail("Choose a PNG, JPEG, GIF or WebP image.");
    const auto size=reader.size();if(size.isEmpty() || size.width()>4096 || size.height()>4096)return fail("Choose an image no larger than 4096 × 4096 pixels.");
    if(reader.read().isNull())return fail("The selected image is damaged or unsupported.");
    const auto mime=format=="jpg"?QByteArray("jpeg"):format;
    data="data:image/"+QString::fromLatin1(mime)+";base64,"+QString::fromLatin1(bytes.toBase64());return true;
}
bool GuildManagement::prepare(const QString &action,const QString &target,const QVariantMap &values,QByteArray &method,QString &path,QJsonObject &body) {
    const auto caps=capabilities();
    if (action == "leaveGuild") {
        if (!idValid(target) || target != m_guildId || !m_client || !m_client->m_guildObjects.contains(target))
            return fail("Choose the server you want to leave.");
        if (caps.value("owner").toBool()) return fail("Transfer ownership before leaving this server.");
        if (!caps.value("leaveGuild").toBool()) return fail(caps.value("leaveReason").toString().isEmpty() ? "Wait for the previous server action to finish." : caps.value("leaveReason").toString());
        method = "DELETE"; path = "/users/@me/guilds/" + target; body = {}; return true;
    }
    auto require=[&](bool allowed,const QString &reason="Your role does not permit this action."){return allowed || fail(reason);};
    auto stringField=[&](const QString &key,const QString &wire,int minimum,int maximum) {
        if(!values.contains(key))return true;
        const auto value=values.value(key).toString();
        if(value.size()<minimum || value.size()>maximum || value.contains(QChar::Null))return fail(QString("%1 must contain %2–%3 characters.").arg(key).arg(minimum).arg(maximum));
        body.insert(wire,value);return true;
    };
    auto numberField=[&](const QString &key,const QString &wire,int low,int high) {
        if(!values.contains(key))return true;
        bool ok=false;const double n=values.value(key).toDouble(&ok);
        if(!ok || !std::isfinite(n) || std::floor(n)!=n || n<low || n>high)return fail(QString("%1 must be between %2 and %3.").arg(key).arg(low).arg(high));
        body.insert(wire,int(n));return true;
    };
    auto boolField=[&](const QString &key,const QString &wire) {
        if(!values.contains(key))return true;
        if(values.value(key).metaType().id()!=QMetaType::Bool)return fail("An on/off value is required.");
        body.insert(wire,values.value(key).toBool());return true;
    };
    const QString base="/guilds/"+m_guildId;
    if(action=="createChannel" || action=="editChannel" || action=="duplicateChannel" || action=="deleteChannel") {
        const bool create=action=="createChannel",duplicate=action=="duplicateChannel";
        const auto original=create?QJsonObject{}:channel(m_guildId,target);
        if(!create && original.isEmpty())return fail("This channel is not in the selected server.");
        const auto channelCaps=create?caps:channelPermissions(m_guildId,target);
        if(!require(create?caps.value("manageChannels").toBool():channelCaps.value("manage").toBool()))return false;
        if(duplicate && !require(caps.value("manageChannels").toBool()))return false;
        if(action=="deleteChannel"){method="DELETE";path="/channels/"+target;return true;}
        if(create && values.contains("type")) {
            bool ok=false;const double value=values.value("type").toDouble(&ok);
            if(!ok || !std::isfinite(value) || std::floor(value)!=value)return fail("Choose a valid channel type.");
        }
        const int type=create?values.value("type",0).toInt():original.value("type").toInt();
        if(!QSet<int>{0,2,4,15}.contains(type))return fail("Only text, voice, category and forum channels are supported here.");
        if(create || duplicate) {
            method="POST";path=base+"/channels";body.insert("type",type);
            if(duplicate)for(const auto &key:{"name","topic","parent_id","nsfw","bitrate","user_limit","rate_limit_per_user","permission_overwrites","available_tags","default_reaction_emoji","default_sort_order","default_forum_layout"})
                if(original.contains(key))body.insert(key,original.value(key));
            if(duplicate && !values.contains("name"))body.insert("name",text(original.value("name").toString()+"-copy",100));
            if(create && !values.contains("name"))return fail("Give the channel a name.");
        }else{method="PATCH";path="/channels/"+target;}
        if(!stringField("name","name",1,100) || !stringField("topic","topic",0,type==15?4096:1024) ||
           !numberField("bitrate","bitrate",8000,guild().value("maxBitrate",96000).toInt()) ||
           !numberField("userLimit","user_limit",0,99) || !numberField("slowmode","rate_limit_per_user",0,21600) || !boolField("nsfw","nsfw"))return false;
        if(values.contains("parentId")) {
            const auto parent=values.value("parentId").toString();
            if(!parent.isEmpty() && (channel(m_guildId,parent).value("type").toInt()!=4 || parent==target))return fail("Choose a valid category in this server.");
            if(type==4 && !parent.isEmpty())return fail("Categories cannot be nested.");
            body.insert("parent_id",parent.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(parent));
        }
        if(values.contains("private")) {
            if(!create && !duplicate && !require(channelCaps.value("managePermissions").toBool(),"Changing channel permissions requires Manage Roles in this channel."))return false;
            if(values.value("private").metaType().id()!=QMetaType::Bool)return fail("Choose whether this channel is private.");
            auto overwrites=original.value("permission_overwrites").toArray();
            if(overwrites.size()>1000)return fail("This channel has too many overwrites to edit safely.");
            const quint64 access=View | (type==2?Connect:0);
            const bool hidden=values.value("private").toBool();bool found=false;
            for(int i=0;i<overwrites.size();++i){auto overwrite=overwrites[i].toObject();
                if(overwrite.value("id").toString()!=m_guildId || overwrite.value("type").toInt()!=0)continue;
                auto deny=overwrite.value("deny").toString().toULongLong(),allow=overwrite.value("allow").toString().toULongLong();
                deny=hidden?deny|access:deny&~access;allow=hidden?allow&~access:allow|access;
                overwrite.insert("deny",QString::number(deny));overwrite.insert("allow",QString::number(allow));overwrites[i]=overwrite;found=true;break;
            }
            if(!found)overwrites.append(QJsonObject{{"id",m_guildId},{"type",0},{"deny",QString::number(hidden?access:0)},{"allow",QString::number(hidden?0:access)}});
            if(hidden) {
                bool own=false;for(int i=0;i<overwrites.size();++i){auto overwrite=overwrites[i].toObject();
                    if(overwrite.value("id").toString()!=m_client->currentUserId() || overwrite.value("type").toInt()!=1)continue;
                    overwrite.insert("allow",QString::number(overwrite.value("allow").toString().toULongLong()|access));
                    overwrite.insert("deny",QString::number(overwrite.value("deny").toString().toULongLong()&~access));overwrites[i]=overwrite;own=true;break;
                }
                if(!own)overwrites.append(QJsonObject{{"id",m_client->currentUserId()},{"type",1},{"allow",QString::number(access)},{"deny","0"}});
            }
            body.insert("permission_overwrites",overwrites);
        }
        return !body.isEmpty() || fail("Choose a channel setting to change.");
    }
    if(action=="editGuild") {
        if(target!=m_guildId)return fail("The selected server changed. Open its settings again.");
        if(!require(caps.value("manageGuild").toBool()))return false;
        method="PATCH";path=base;
        if(!stringField("name","name",2,100) || !stringField("description","description",0,120) || !stringField("preferredLocale","preferred_locale",2,16) ||
           !numberField("verificationLevel","verification_level",0,4) || !numberField("explicitContentFilter","explicit_content_filter",0,2) ||
           !numberField("defaultMessageNotifications","default_message_notifications",0,1))return false;
        if(values.contains("afkTimeout")) {
            const int timeout=values.value("afkTimeout").toInt();if(!QSet<int>{60,300,900,1800,3600}.contains(timeout))return fail("Choose a supported inactive timeout.");body.insert("afk_timeout",timeout);
        }
        if(values.contains("afkChannelId")) {
            const auto id=values.value("afkChannelId").toString();if(!id.isEmpty() && channel(m_guildId,id).value("type").toInt()!=2)return fail("Choose a voice channel for inactive members.");
            body.insert("afk_channel_id",id.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(id));
        }
        for(const auto &key:{"icon","banner"})if(values.contains(QString(key)+"Path")) {
            QString data;if(!imageData(values.value(QString(key)+"Path"),data))return false;
            body.insert(key,data.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(data));
        }
        return !body.isEmpty() || fail("Choose a server setting to change.");
    }
    if(action=="createRole" || action=="editRole" || action=="deleteRole") {
        if(!require(caps.value("manageRoles").toBool()))return false;
        const bool create=action=="createRole";const auto existing=create?QJsonObject{}:role(target);
        if(!create && !require(roleEditable(existing),"You can only manage ordinary roles below your highest role."))return false;
        if(action=="deleteRole") {
            if(target==m_guildId)return fail("The everyone role cannot be deleted.");
            method="DELETE";path=base+"/roles/"+target;return true;
        }
        method=create?"POST":"PATCH";path=base+"/roles"+(create?QString{}:"/"+target);
        if(create && !values.contains("name"))return fail("Give the role a name.");
        if(!stringField("name","name",1,100) || !numberField("color","color",0,0xffffff) || !boolField("hoist","hoist") || !boolField("mentionable","mentionable"))return false;
        if(values.contains("permissions") || values.contains("permissionKeys")) {
            quint64 bits=existing.value("permissions").toString().toULongLong();
            if(values.contains("permissions")) {
                bool ok=false;bits=values.value("permissions").toString().toULongLong(&ok);if(!ok)return fail("Permissions must be a decimal integer string.");
            }else{
                const auto selected=values.value("permissionKeys").toStringList();if(selected.size()>permissionOptions().size())return fail("Too many permission choices.");
                QSet<QString> allowed;for(const auto &p:permissionOptions()){allowed.insert(p.key);bits&=~p.bit;if(selected.contains(p.key))bits|=p.bit;}
                for(const auto &key:selected)if(!allowed.contains(key))return fail("An unsupported permission was selected.");
            }
            if(bits & ~permissionBits(m_guildId))return fail("You cannot grant permissions that you do not have.");
            body.insert("permissions",QString::number(bits));
        }
        return !body.isEmpty() || fail("Choose a role setting to change.");
    }
    if(action=="assignRole" || action=="removeRole") {
        const auto roleId=values.value("roleId").toString();
        if(!require(userPermissions(m_guildId,target).value("manageRoles").toBool(),"You cannot change this member's roles.") ||
           !require(roleId!=m_guildId && roleEditable(role(roleId)),"This role is managed or at or above your highest role."))return false;
        method=action=="assignRole"?"PUT":"DELETE";path=base+"/members/"+target+"/roles/"+roleId;return true;
    }
    if(action=="serverMute" || action=="serverDeafen" || action=="moveMember" || action=="kickMember" || action=="banMember" || action=="unbanMember" || action=="timeoutMember") {
        if(!idValid(target))return fail("Choose a valid member.");
        if(action=="unbanMember") {
            if(!require(caps.value("banMembers").toBool()))return false;
            method="DELETE";path=base+"/bans/"+target;return true;
        }
        const auto permissions=userPermissions(m_guildId,target);
        const QHash<QString,QString> names{{"serverMute","mute"},{"serverDeafen","deafen"},{"moveMember","move"},{"kickMember","kick"},{"banMember","ban"},{"timeoutMember","timeout"}};
        if(!require(permissions.value(names.value(action)).toBool(),permissions.value("reason").toString().isEmpty()?"Your role does not permit moderating this member.":permissions.value("reason").toString()))return false;
        path=base+"/members/"+target;method="PATCH";
        if(action=="kickMember"){method="DELETE";return true;}
        if(action=="banMember") {
            method="PUT";path=base+"/bans/"+target;
            return numberField("deleteMessageSeconds","delete_message_seconds",0,604800);
        }
        if(action=="serverMute" || action=="serverDeafen") {
            if(!values.contains("enabled"))return fail("Choose the new voice state.");
            return boolField("enabled",action=="serverMute"?"mute":"deaf");
        }
        if(action=="moveMember") {
            if(!values.contains("channelId"))return fail("Choose a voice channel or Disconnect.");
            const auto destination=values.value("channelId").toString();
            if(!destination.isEmpty()) {
                const auto targetChannel=channel(m_guildId,destination);
                if(!QSet<int>{2,13}.contains(targetChannel.value("type").toInt()) || !(permissionBits(m_guildId,destination)&Connect))return fail("Choose a voice channel you can connect to.");
            }
            body.insert("channel_id",destination.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(destination));return true;
        }
        if(!values.contains("until"))return fail("Choose when the time out ends.");
        const auto until=values.value("until").toString();const auto date=QDateTime::fromString(until,Qt::ISODate);
        if(!until.isEmpty() && (!date.isValid() || date<=QDateTime::currentDateTimeUtc() || date>QDateTime::currentDateTimeUtc().addDays(28)))return fail("The time out must end within the next 28 days.");
        body.insert("communication_disabled_until",until.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(date.toUTC().toString(Qt::ISODate)));return true;
    }
    if(action=="createInvite") {
        if(!idValid(target) || channel(m_guildId,target).isEmpty())return fail("Choose a channel in the selected server.");
        if(!require(channelPermissions(m_guildId,target).value("createInvite").toBool()))return false;
        method="POST";path="/channels/"+target+"/invites";body={{"max_age",86400},{"max_uses",0},{"temporary",false},{"unique",false}};
        return numberField("maxAge","max_age",0,604800) && numberField("maxUses","max_uses",0,100) && boolField("temporary","temporary");
    }
    if(action=="deleteInvite") {
        if(!require(caps.value("manageInvites").toBool()))return false;
        if(!QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(target).hasMatch())return fail("Choose a valid invite.");
        bool known=false;for(const auto &row:m_items)if(m_section=="invites" && row.toMap().value("code").toString()==target)known=true;
        if(!known)return fail("Reload this server's invites before deleting one.");
        method="DELETE";path="/invites/"+target;return true;
    }
    if(action=="createEvent" || action=="editEvent" || action=="deleteEvent" || action=="rsvpEvent") {
        const bool create=action=="createEvent";const auto existing=create?QJsonObject{}:m_events.value(target);
        if(!create && existing.isEmpty())return fail("Reload scheduled events before changing this event.");
        if(!create && action!="rsvpEvent") {
            const auto existingBits=existing.value("entity_type").toInt()==3?permissionBits(m_guildId):permissionBits(m_guildId,existing.value("channel_id").toString());
            const bool owns=existing.value("creator_id").toString()==m_client->currentUserId();
            const auto oldEntity=existing.value("entity_type").toInt();
            const quint64 required=oldEntity==1?(ManageChannels|Mute|Move):oldEntity==2?(View|Connect):0;
            if(!require((existingBits&required)==required && (bool(existingBits&ManageEvents) || (owns && bool(existingBits&CreateEvents)))))return false;
            if(action=="deleteEvent"){method="DELETE";path=base+"/scheduled-events/"+target;return true;}
        }
        const int entity=action=="rsvpEvent"?existing.value("entity_type").toInt():values.value("entityType",existing.value("entity_type").toInt(3)).toInt();
        if(!QSet<int>{1,2,3}.contains(entity))return fail("Choose a voice, stage or external event.");
        const auto channelId=action=="rsvpEvent"?existing.value("channel_id").toString():values.value("channelId",existing.value("channel_id").toString()).toString();
        const auto bits=entity==3?permissionBits(m_guildId):permissionBits(m_guildId,channelId);
        if(action=="rsvpEvent") {
            if(entity!=3 && !(bits&View))return fail("You cannot view this event's channel.");
            if(!values.contains("enabled") || values.value("enabled").metaType().id()!=QMetaType::Bool)return fail("Choose whether you are interested in this event.");
            method=values.value("enabled").toBool()?"PUT":"DELETE";path=base+"/scheduled-events/"+target+"/users/@me";return true;
        }
        const bool owns=existing.value("creator_id").toString()==m_client->currentUserId();
        const bool permitted=create?bool(bits&CreateEvents):bool(bits&ManageEvents) || (owns && bool(bits&CreateEvents));
        if(!require(permitted))return false;
        if(entity!=3) {
            const auto object=channel(m_guildId,channelId);
            if(object.value("type").toInt()!= (entity==1?13:2) || !(bits&View) || (entity==2 && !(bits&Connect)))return fail("Choose an accessible channel matching this event's type.");
            if(entity==1 && ((bits&(ManageChannels|Mute|Move))!=(ManageChannels|Mute|Move)))return fail("Stage events require Manage Channels, Mute Members and Move Members in that stage.");
        }
        path=base+"/scheduled-events"+(create?QString{}:"/"+target);
        if(action=="deleteEvent"){method="DELETE";return true;}
        method=create?"POST":"PATCH";
        if(create && !values.contains("name"))return fail("Give the event a name.");
        if(!stringField("name","name",1,100) || !stringField("description","description",0,1000))return false;
        body.insert("privacy_level",2);body.insert("entity_type",entity);
        body.insert("channel_id",entity==3?QJsonValue(QJsonValue::Null):QJsonValue(channelId));
        if(entity==3) {
            const auto location=values.value("location",existing.value("entity_metadata").toObject().value("location").toString()).toString().trimmed();
            if(location.isEmpty() || location.size()>100)return fail("Give the external event a location of up to 100 characters.");
            body.insert("entity_metadata",QJsonObject{{"location",location}});
        }
        const auto start=values.value("scheduledStartTime",existing.value("scheduled_start_time").toString()).toString();
        const auto end=values.value("scheduledEndTime",existing.value("scheduled_end_time").toString()).toString();
        const auto startTime=QDateTime::fromString(start,Qt::ISODate),endTime=QDateTime::fromString(end,Qt::ISODate);
        const bool startChanged=create || startTime!=QDateTime::fromString(existing.value("scheduled_start_time").toString(),Qt::ISODate);
        if(!startTime.isValid() || (startChanged && startTime<=QDateTime::currentDateTimeUtc()))return fail("Choose a future event start time.");
        if((entity==3 && end.isEmpty()) || (!end.isEmpty() && (!endTime.isValid() || endTime<=startTime)))return fail("The event end time must be after its start time.");
        if(startChanged)body.insert("scheduled_start_time",startTime.toUTC().toString(Qt::ISODate));
        if(create || values.contains("scheduledEndTime"))body.insert("scheduled_end_time",end.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(endTime.toUTC().toString(Qt::ISODate)));
        if(values.contains("status")) {
            const int status=values.value("status").toInt(),old=existing.value("status").toInt(1);
            if(!((old==1 && (status==1 || status==2 || status==4)) || (old==2 && (status==2 || status==3))))return fail("This event cannot change to that state.");
            body.insert("status",status);
        }
        return true;
    }
    return fail("This server action is not supported.");
}

bool GuildManagement::execute(const QString &action,const QString &target,const QVariantMap &values) {
    if(m_mutating || !m_pendingLeaves.isEmpty())return fail("Wait for the previous server action to finish.");
    if(!ready())return fail("Reconnect to Discord before changing server settings.");
    if (action == "leaveGuild") return leaveGuild(target);
    QByteArray method;QString path;QJsonObject body;m_error.clear();
    if(!prepare(action,target,values,method,path,body))return false;
    const auto generation=m_generation,operation=++m_writeOperation;const auto guildId=m_guildId;
    ++m_readOperation;m_busy=false;m_mutating=true;m_status="Saving changes…";emit changed();
    QPointer<GuildManagement> guard(this);
    m_client->request(method,path,body,[guard,generation,operation,action,target,body,values,guildId](const QJsonValue &value,const QString &error) {
        if(!guard || guard->m_generation!=generation || guard->m_writeOperation!=operation)return;
        auto *self=guard.data();self->m_mutating=false;++self->m_readOperation;
        if(!error.isEmpty()){
            self->m_error=error;self->m_status="The change was not confirmed.";
            // A failed mutation must remain visible; an explicit refresh can retry the read.
            self->m_refreshPending=false;self->m_busy=false;emit self->changed();return;
        }
        auto object=value.toObject();QVariantMap result;QString id=object.value("id").toString(target);
        if((action=="createChannel" || action=="duplicateChannel" || action=="createRole" || action=="createEvent") && !idValid(object.value("id").toString())) {
            self->m_error="Discord accepted the request but returned incomplete details. Refresh before trying again.";self->m_status.clear();emit self->changed();return;
        }
        if(action=="editGuild")self->acceptGuild(merge(body,object));
        else if(action.endsWith("Channel")) {
            if(action=="deleteChannel")self->m_client->dispatch("CHANNEL_DELETE",{{"guild_id",guildId},{"id",target}});
            else if(idValid(id)){object=merge(body,object);object.insert("guild_id",guildId);object.insert("id",id);self->m_client->dispatch(action=="editChannel"?"CHANNEL_UPDATE":"CHANNEL_CREATE",object);}
        }else if(action=="createRole" || action=="editRole" || action=="deleteRole") {
            auto roles=self->snapshot(guildId).value("roles").toArray();
            const auto roleId=action=="createRole"?id:target;bool found=false;
            for(int i=roles.size()-1;i>=0;--i)if(roles[i].toObject().value("id").toString()==roleId) {
                if(action=="deleteRole")roles.removeAt(i);else roles[i]=merge(roles[i].toObject(),merge(body,object));found=true;break;
            }
            if(action=="createRole" && !found && idValid(id))roles.append(merge(body,object));
            self->acceptGuild({{"id",guildId},{"roles",roles}});if(self->m_section=="roles")self->m_items=self->roles();
        }else if(action=="assignRole" || action=="removeRole") {
            auto member=self->member(guildId,target);auto ids=member.value("roles").toArray();const auto roleId=values.value("roleId").toString();
            if(action=="assignRole" && !ids.contains(roleId))ids.append(roleId);
            if(action=="removeRole")for(int i=ids.size()-1;i>=0;--i)if(ids[i].toString()==roleId)ids.removeAt(i);
            member.insert("roles",ids);self->m_members.insert(target,member);self->m_client->cacheMember(guildId,member);
        }else if(action=="kickMember" || action=="banMember") {
            self->m_members.remove(target);self->m_client->dispatch("GUILD_MEMBER_REMOVE",{{"guild_id",guildId},{"user",QJsonObject{{"id",target}}}});
        }else if(action=="serverMute" || action=="serverDeafen" || action=="moveMember" || action=="timeoutMember") {
            auto member=merge(self->member(guildId,target),object);member=merge(member,body);self->m_members.insert(target,member);self->m_client->cacheMember(guildId,member);
            // Voice snapshots come from the live gateway; do not overwrite self-mute
            // or stream state using the REST member response.
        }else if(action=="createEvent" || action=="editEvent") {
            if(idValid(id)){self->m_events.insert(id,merge(self->m_events.value(id),merge(body,object)));result=self->itemRow("events",self->m_events.value(id));}
        }else if(action=="deleteEvent")self->m_events.remove(target);
        else if(action=="rsvpEvent")self->m_events[target].insert("current_user_interested",values.value("enabled").toBool());
        else if(action=="createInvite")result=self->itemRow("invites",object);
        if(self->m_section=="events" && action.endsWith("Event")) {
            self->m_items.clear();for(const auto &event:self->m_events)self->m_items.append(self->itemRow("events",event));
        }
        if(self->m_section=="members" && (action.endsWith("Member") || action=="assignRole" || action=="removeRole")) {
            self->m_items.clear();for(const auto &member:self->m_members)self->m_items.append(self->itemRow("members",member));
        }
        if((action=="deleteInvite" && self->m_section=="invites") || (action=="unbanMember" && self->m_section=="bans"))
            for(int i=self->m_items.size()-1;i>=0;--i)if(self->m_items[i].toMap().value("id").toString()==target)self->m_items.removeAt(i);
        self->m_client->rebuildMembers();self->m_status="Changes saved.";self->m_error.clear();emit self->changed();emit self->actionCompleted(action,id,result);
        if(self->m_refreshPending){self->m_refreshPending=false;self->refresh();}
    },values.value("reason").toString());
    return true;
}

bool GuildManagement::leaveGuild(const QString &target) {
    QByteArray method; QString path; QJsonObject body;
    m_error.clear();
    if (!prepare("leaveGuild", target, {}, method, path, body)) return false;
    const auto session = m_client->m_generation;
    const auto membership = m_client->m_guildMembershipVersions.value(target);
    const auto account = m_client->currentUserId();
    const auto generation = m_generation, operation = ++m_writeOperation;
    ++m_readOperation; m_busy = false; m_mutating = true; m_pendingLeaves.insert(target);
    m_status = "Leaving server…"; emit changed();
    QPointer<GuildManagement> guard(this);
    QPointer<DiscordClient> client(m_client);
    m_client->request(method, path, body, [guard,client,target,session,membership,account,generation,operation](const QJsonValue &,const QString &error) {
        if (!guard || !client || client->m_generation != session || client->currentUserId() != account) return;
        guard->m_pendingLeaves.remove(target);
        const bool current = guard->m_generation == generation && guard->m_writeOperation == operation;
        if (current) { guard->m_mutating = false; ++guard->m_readOperation; }
        const bool stillMember = client->m_guildObjects.contains(target);
        // A later membership (leave/rejoin) belongs to a new operation. Never
        // remove it because a slow reply confirmed the earlier membership.
        if (stillMember && client->m_guildMembershipVersions.value(target) != membership) {
            if (current) { guard->m_status.clear(); emit guard->changed(); }
            return;
        }
        if (!error.isEmpty()) {
            // A gateway removal may have confirmed the leave before REST ended.
            if (!stillMember) return;
            guard->m_error = "Could not leave the server. " + error;
            guard->m_status = "The server leave was not confirmed.";
            if (current) { guard->m_busy = false; guard->m_refreshPending = false; }
            emit guard->changed(); return;
        }
        if (stillMember) client->dispatch("GUILD_DELETE", {{"id",target}});
        if (guard) emit guard->actionCompleted("leaveGuild", target, {});
    }, {}, 204);
    return true;
}

QVariantList GuildManagement::channels() const {
    QVariantList rows;if(!m_client)return rows;
    for(const auto &entry:m_client->m_channelCache.value(m_guildId)) {
        if(rows.size()>=2000)break;
        const auto object=entry.toObject();
        rows.append(QVariantMap{{"id",object.value("id").toString()},{"name",text(object.value("name").toString(),100)},
            {"type",object.value("type").toInt()},{"parentId",object.value("parent_id").toString()}});
    }
    return rows;
}
