#include "ForumTools.h"
#include "DiscordClient.h"
#include <QDateTime>
#include <QJsonArray>
#include <QUrl>
#include <QTimeZone>
#include <algorithm>
#include <limits>

namespace {
constexpr quint64 View=1ULL<<10,Send=1ULL<<11,ManageThreads=1ULL<<34,Reply=1ULL<<38;
quint64 snowflake(const QString &id){bool ok=false;const auto value=id.toULongLong(&ok);return ok?value:0;}
QDateTime createdAt(const QString &id){return QDateTime::fromMSecsSinceEpoch(qint64(snowflake(id)>>22)+1420070400000LL,QTimeZone::UTC);}
}
ForumTools::ForumTools(DiscordClient *client,QObject *parent):QObject(parent),m_client(client){
    if(!client)return;m_session=client->m_generation;m_selection=client->m_selectionGeneration;
    connect(client,&DiscordClient::stateChanged,this,&ForumTools::syncSelection);
    connect(client,&DiscordClient::forumEventReceived,this,&ForumTools::handleEvent);
    connect(client,&QObject::destroyed,this,[this]{reset();});
    syncSelection();
}
bool ForumTools::authenticated()const{return m_client && !m_client->demoMode() && !m_client->m_token.isEmpty() && m_session==m_client->m_generation;}
bool ForumTools::ready()const{return authenticated() && m_client->connected();}
void ForumTools::reset(){
    ++m_operation;++m_write;m_posts.clear();m_threads.clear();m_revisions.clear();m_deleted.clear();m_deletedOrder.clear();m_revision=0;m_forum={};m_guild.clear();m_channel.clear();m_cursor.clear();
    m_error.clear();m_postError.clear();m_filter.clear();m_search.clear();m_loading=m_posting=m_more=false;emit changed();
}
QJsonObject ForumTools::knownChannel(const QString &id)const{
    if(!m_client)return {};
    for(const auto &item:m_client->m_channelCache.value(m_client->currentGuildId()))if(item.toObject().value("id").toString()==id)return item.toObject();
    return {};
}
void ForumTools::syncSelection(){
    if(!m_client)return;
    if(m_session!=m_client->m_generation){m_session=m_client->m_generation;reset();}
    if(m_client->currentGuildId()!=m_guild && !m_channel.isEmpty())reset();
    if(m_client->currentChannelKind()=="thread"){
        const auto current=knownChannel(m_client->currentChannelId());const auto parent=knownChannel(current.value("parent_id").toString());
        if((parent.value("type").toInt()==15 || parent.value("type").toInt()==16) && parent.value("id").toString()!=m_channel){selectForum(parent.value("id").toString());return;}
    }
    if(!m_channel.isEmpty()){
        const auto forum=knownChannel(m_channel);
        if(forum.isEmpty() || !m_client->channelVisible(forum,m_guild)){
            reset();m_error="You no longer have access to this forum.";emit changed();return;
        }
        m_forum=forum;
    }
    const bool moved=m_selection!=m_client->m_selectionGeneration;m_selection=m_client->m_selectionGeneration;
    if(m_client->currentChannelKind()=="forum"){
        if(m_client->currentChannelId()!=m_channel){selectForum(m_client->currentChannelId());return;}
        m_forum=knownChannel(m_channel);if(moved && !m_navigating){refresh();return;}
    }
    emit changed();
}
quint64 ForumTools::permissions()const{
    if(!ready() || m_channel.isEmpty())return 0;
    const auto guild=m_client->m_guildObjects.value(m_guild);const auto self=m_client->currentUserId();
    if(!self.isEmpty() && guild.value("owner_id").toString()==self)return std::numeric_limits<quint64>::max();
    auto own=guild.value("member").toObject();const auto cached=m_client->m_guildMembers.value(m_guild).value(self);
    for(auto it=cached.begin();it!=cached.end();++it)own.insert(it.key(),it.value());
    const auto until=QDateTime::fromString(own.value("communication_disabled_until").toString(),Qt::ISODateWithMs);
    if(until.isValid() && until>QDateTime::currentDateTimeUtc())return 0;
    quint64 bits=0;const auto roles=guild.value("roles").toArray();QSet<QString> ids{m_guild};for(const auto &id:own.value("roles").toArray())ids.insert(id.toString());
    if(own.isEmpty())bits=guild.value("permissions").toString().toULongLong();
    else for(const auto &entry:roles){const auto role=entry.toObject();if(ids.contains(role.value("id").toString()))bits|=role.value("permissions").toString().toULongLong();}
    if(bits&(1ULL<<3))return std::numeric_limits<quint64>::max();
    quint64 deny=0,allow=0,memberDeny=0,memberAllow=0;
    for(const auto &entry:m_forum.value("permission_overwrites").toArray()){
        const auto overwrite=entry.toObject();const auto id=overwrite.value("id").toString();const auto d=overwrite.value("deny").toString().toULongLong(),a=overwrite.value("allow").toString().toULongLong();
        if(overwrite.value("type").toInt()==0){if(id==m_guild)bits=(bits&~d)|a;else if(ids.contains(id)){deny|=d;allow|=a;}}
        else if(id==self){memberDeny|=d;memberAllow|=a;}
    }
    bits=(bits&~deny)|allow;bits=(bits&~memberDeny)|memberAllow;return bits;
}
bool ForumTools::canPost()const{return (permissions()&(View|Send))==(View|Send);}
QString ForumTools::postRestriction()const{return !ready()?"Connect to Discord to create a post.":!canPost()?"You don’t have permission to create posts in this forum.":QString{};}
QVariantList ForumTools::tags()const{
    QVariantList result;const bool manage=permissions()&ManageThreads;
    for(const auto &entry:m_forum.value("available_tags").toArray()){
        const auto tag=entry.toObject();if(result.size()>=20 || !DiscordClient::validId(tag.value("id").toString()))continue;
        result.append(QVariantMap{{"id",tag.value("id").toString()},{"name",tag.value("name").toString().left(20)},
          {"emoji",tag.value("emoji_name").toString().left(16)},{"selectable",!tag.value("moderated").toBool() || manage}});
    }return result;
}
bool ForumTools::currentThread()const{
    return m_client && !m_channel.isEmpty() && m_client->currentChannelKind()=="thread" && knownChannel(m_client->currentChannelId()).value("parent_id").toString()==m_channel;
}
bool ForumTools::canReply()const{
    if(!currentThread())return false;const auto bits=permissions();const auto thread=knownChannel(m_client->currentChannelId());
    return (bits&(View|Reply))==(View|Reply) && (!thread.value("thread_metadata").toObject().value("locked").toBool() || bits&ManageThreads);
}
bool ForumTools::selectForum(const QString &id){
    const auto channel=knownChannel(id);const int type=channel.value("type").toInt(-1);
    if(!ready() || !DiscordClient::validId(id) || (type!=15 && type!=16) || !m_client->channelVisible(channel,m_client->currentGuildId()))return false;
    reset();m_guild=m_client->currentGuildId();m_channel=id;m_forum=channel;
    m_sort=channel.value("default_sort_order").toInt()==1?"newest":"activity";refresh();return true;
}
void ForumTools::refresh(){
    if(!ready() || m_channel.isEmpty())return;
    const auto operation=++m_operation;m_loading=true;m_more=false;m_cursor.clear();m_error.clear();emit changed();
    const auto snapshot=m_revision;
    m_client->request("GET","/guilds/"+m_guild+"/threads/active",{},[this,weak=QPointer<ForumTools>(this),operation,snapshot](const QJsonValue &value,const QString &error){
        if(!weak || operation!=m_operation || !authenticated())return;
        if(error.isEmpty() && value.toObject().value("threads").isArray()){
            for(auto it=m_threads.begin();it!=m_threads.end();)if(m_revisions.value(it.key())<=snapshot)it=m_threads.erase(it);else ++it;
            acceptThreads(value.toObject().value("threads").toArray(),snapshot);
        }
        else m_error=error.isEmpty()?"Discord returned an unexpected forum response.":error;
        fetchArchived(operation);
    });
}
void ForumTools::loadMore(){if(ready() && !m_loading && m_more)fetchArchived(m_operation);}
void ForumTools::fetchArchived(quint64 operation){
    if(!authenticated() || operation!=m_operation)return;
    // Unknown history permissions are left to the server for read-only browsing.
    m_loading=true;const auto previous=m_cursor;QString path="/channels/"+m_channel+"/threads/archived/public?limit=50";
    if(!previous.isEmpty())path+="&before="+QString::fromLatin1(QUrl::toPercentEncoding(previous));emit changed();
    const auto snapshot=m_revision;
    m_client->request("GET",path,{},[this,weak=QPointer<ForumTools>(this),operation,previous,snapshot](const QJsonValue &value,const QString &error){
        if(!weak || operation!=m_operation || !authenticated())return;m_loading=false;
        const auto object=value.toObject();
        if(!error.isEmpty()){m_error=error;m_more=false;emit changed();return;}
        if(!object.value("threads").isArray() || !object.value("has_more").isBool()){m_error="Discord returned an unexpected archived posts response.";m_more=false;emit changed();return;}
        const auto threads=object.value("threads").toArray();QString cursor;
        for(const auto &entry:threads){const auto thread=entry.toObject();const auto stamp=thread.value("thread_metadata").toObject().value("archive_timestamp").toString();
            if(validThread(thread) && stamp.size()<=64 && QDateTime::fromString(stamp,Qt::ISODateWithMs).isValid())cursor=stamp;}
        const auto nextDate=QDateTime::fromString(cursor,Qt::ISODateWithMs),previousDate=QDateTime::fromString(previous,Qt::ISODateWithMs);
        const bool progresses=nextDate.isValid() && (previous.isEmpty() || (previousDate.isValid() && nextDate<previousDate));
        acceptThreads(threads,snapshot);m_cursor=progresses?cursor:previous;m_more=object.value("has_more").toBool() && m_threads.size()<500 && progresses;
        if(object.value("has_more").toBool() && !progresses)m_error="The next page of forum posts could not be loaded. Refresh to try again.";
        emit changed();
    });
}
bool ForumTools::validThread(const QJsonObject &thread)const{
    return DiscordClient::validId(thread.value("id").toString()) && thread.value("parent_id").toString()==m_channel && thread.value("type").toInt()==11
      && (thread.value("guild_id").toString().isEmpty() || thread.value("guild_id").toString()==m_guild)
      && m_client && m_client->channelVisible(thread,m_guild);
}
void ForumTools::acceptThreads(const QJsonArray &threads,quint64 snapshot){
    for(int i=0;i<std::min(1000,int(threads.size()));++i){const auto thread=threads[i].toObject();const auto id=thread.value("id").toString();if(!validThread(thread))continue;
        if(m_deleted.contains(id) || m_revisions.value(id)>snapshot)continue;
        if(m_threads.size()>=500 && !m_threads.contains(id)){
            auto oldest=m_threads.begin();for(auto it=m_threads.begin();it!=m_threads.end();++it)if(snowflake(it.key())<snowflake(oldest.key()))oldest=it;
            if(snowflake(oldest.key())>=snowflake(id))continue;m_threads.erase(oldest);
        }m_threads[id]=thread;
    }
    rebuild();
}
void ForumTools::markRevision(const QString &id){
    m_revisions[id]=++m_revision;
    while(m_revisions.size()>1500){auto oldest=m_revisions.begin();for(auto it=m_revisions.begin();it!=m_revisions.end();++it)if(it.value()<oldest.value())oldest=it;m_revisions.erase(oldest);}
}
void ForumTools::handleEvent(const QString &event,const QJsonObject &data){
    if(!ready() || m_channel.isEmpty())return;
    if((event=="CHANNEL_UPDATE" || event=="CHANNEL_DELETE") && data.value("id").toString()==m_channel && data.value("guild_id").toString()==m_guild){
        if(event=="CHANNEL_DELETE"){reset();return;}
        for(auto it=data.begin();it!=data.end();++it)m_forum.insert(it.key(),it.value());
        if(!m_client->channelVisible(m_forum,m_guild)){reset();m_error="You no longer have access to this forum.";}
        emit changed();return;
    }
    if(event=="MESSAGE_CREATE"){
        const auto channel=data.value("channel_id").toString(),id=data.value("id").toString();
        if(!m_threads.contains(channel) || !DiscordClient::validId(id))return;
        auto &thread=m_threads[channel];if(snowflake(id)<=snowflake(thread.value("last_message_id").toString(channel)))return;
        thread.insert("last_message_id",id);thread.insert("message_count",thread.value("message_count").toInt()+1);markRevision(channel);rebuild();return;
    }
    if(data.value("guild_id").toString()!=m_guild)return;
    if(event=="THREAD_DELETE"){
        const auto id=data.value("id").toString();if(!DiscordClient::validId(id) || (!m_threads.contains(id) && data.value("parent_id").toString()!=m_channel))return;
        markRevision(id);m_threads.remove(id);if(!m_deleted.contains(id)){m_deleted.insert(id);m_deletedOrder.enqueue(id);}
        while(m_deletedOrder.size()>1000)m_deleted.remove(m_deletedOrder.dequeue());rebuild();return;
    }
    if(event=="THREAD_LIST_SYNC"){
        if(data.contains("channel_ids") && !data.value("channel_ids").toArray().contains(m_channel))return;
        for(auto it=m_threads.begin();it!=m_threads.end();)if(!it.value().value("thread_metadata").toObject().value("archived").toBool()){markRevision(it.key());it=m_threads.erase(it);}else ++it;
        for(const auto &entry:data.value("threads").toArray())if(validThread(entry.toObject()))markRevision(entry.toObject().value("id").toString());
        acceptThreads(data.value("threads").toArray());return;
    }
    if(event=="THREAD_CREATE" || event=="THREAD_UPDATE"){
        auto thread=m_threads.value(data.value("id").toString());for(auto it=data.begin();it!=data.end();++it)thread.insert(it.key(),it.value());
        if(validThread(thread)){markRevision(thread.value("id").toString());acceptThreads(QJsonArray{thread});}
    }
}
void ForumTools::rebuild(){
    QVector<QJsonObject> ordered;for(const auto &thread:m_threads){
        if(!m_filter.isEmpty() && !thread.value("applied_tags").toArray().contains(m_filter))continue;
        if(!m_search.isEmpty() && !thread.value("name").toString().contains(m_search,Qt::CaseInsensitive))continue;ordered.append(thread);
    }
    std::sort(ordered.begin(),ordered.end(),[this](const auto &a,const auto &b){
        const bool ap=a.value("flags").toInt()&2,bp=b.value("flags").toInt()&2;if(ap!=bp)return ap;
        const auto aid=a.value(m_sort=="newest"?"id":"last_message_id").toString(a.value("id").toString()),bid=b.value(m_sort=="newest"?"id":"last_message_id").toString(b.value("id").toString());
        return aid==bid?snowflake(a.value("id").toString())>snowflake(b.value("id").toString()):snowflake(aid)>snowflake(bid);
    });
    QVector<QVariantMap> rows;const auto available=tags();
    for(const auto &thread:ordered){
        const auto id=thread.value("id").toString(),owner=thread.value("owner_id").toString();const auto member=m_client?m_client->memberInfoForGuild(owner,m_guild):QVariantMap{};
        QVariantList applied;for(const auto &tag:available)if(thread.value("applied_tags").toArray().contains(tag.toMap().value("id").toString()))applied.append(tag);
        const auto meta=thread.value("thread_metadata").toObject();auto activity=createdAt(thread.value("last_message_id").toString(id));
        rows.append({{"recordId",id},{"name",thread.value("name").toString().left(100)},{"authorId",owner},{"author",member.value("displayName",member.value("name","Member"))},
          {"avatarUrl",member.value("avatarUrl")},{"timestamp",activity.toString(Qt::ISODate)},{"replyCount",std::max(0,thread.value("message_count").toInt())},
          {"forumTags",applied},{"archived",meta.value("archived").toBool()},{"locked",meta.value("locked").toBool()},{"pinned",bool(thread.value("flags").toInt()&2)}});
    }m_posts.reconcile(std::move(rows));emit changed();
}
void ForumTools::setFilterTag(const QString &id){if(m_filter==id)return;m_filter=id.left(22);rebuild();}
void ForumTools::setSearchText(const QString &text){const auto clean=text.trimmed().left(100);if(clean==m_search)return;m_search=clean;rebuild();}
void ForumTools::setSortOrder(const QString &order){if((order!="activity" && order!="newest") || m_sort==order)return;m_sort=order;rebuild();}
void ForumTools::cacheThread(const QJsonObject &thread){
    if(!m_client || !validThread(thread))return;auto channels=m_client->m_channelCache.value(m_guild);bool found=false;
    for(int i=0;i<channels.size();++i)if(channels[i].toObject().value("id").toString()==thread.value("id").toString()){channels[i]=thread;found=true;break;}
    if(!found)channels.append(thread);
    m_client->cacheChannels(m_guild,channels);m_client->rebuildChannels(false);
}
bool ForumTools::openPost(const QString &id){
    if(!ready() || m_guild!=m_client->currentGuildId() || !m_threads.contains(id))return false;
    const auto thread=m_threads.value(id);if(!validThread(thread))return false;
    cacheThread(thread);m_navigating=true;m_client->selectChannel(id);m_navigating=false;emit changed();return m_client->currentChannelId()==id;
}
void ForumTools::returnToForum(){if(m_client && m_guild==m_client->currentGuildId() && !m_channel.isEmpty())m_client->selectChannel(m_channel);}
bool ForumTools::createPost(const QString &title,const QString &content,const QVariantList &selected){
    if(m_posting)return false;m_postError.clear();const auto name=title.trimmed();
    auto fail=[this](const QString &reason){m_postError=reason;emit changed();return false;};
    if(!canPost())return fail(postRestriction());
    if(name.isEmpty() || name.size()>100 || name.contains('\n') || name.contains('\r'))return fail("Give your post a title between 1 and 100 characters.");
    if(content.trimmed().isEmpty() || content.size()>2000)return fail("Write a message between 1 and 2,000 characters.");
    if(selected.size()>5)return fail("Choose up to five tags.");QJsonArray applied;const auto available=tags();
    for(const auto &value:selected){const auto id=value.toString();bool allowed=false;for(const auto &tag:available)if(tag.toMap().value("id").toString()==id && tag.toMap().value("selectable").toBool())allowed=true;
        if(!allowed || applied.contains(id))return fail("Choose valid tags available to you in this forum.");applied.append(id);}
    if(tagRequired() && applied.isEmpty())return fail("Choose at least one tag for this forum.");
    m_posting=true;const auto write=++m_write,operation=m_operation;const auto forum=m_channel,guild=m_guild;emit changed();
    m_client->request("POST","/channels/"+forum+"/threads",{{"name",name},{"applied_tags",applied},
      {"message",QJsonObject{{"content",content},{"allowed_mentions",DiscordClient::allowedMentions(content)}}}},
      [this,weak=QPointer<ForumTools>(this),write,operation,forum,guild](const QJsonValue &value,const QString &error){
        Q_UNUSED(operation);if(!weak || write!=m_write || !authenticated() || forum!=m_channel || guild!=m_guild)return;m_posting=false;
        if(!error.isEmpty()){m_postError=error;emit changed();return;}const auto thread=value.toObject();
        if(!validThread(thread)){m_postError="Discord created the post but returned an unexpected response. Refresh the forum to find it.";emit changed();return;}
        acceptThreads(QJsonArray{thread});emit postCreated(thread.value("id").toString());
        if(m_client->currentChannelId()==forum)openPost(thread.value("id").toString());emit changed();
    });return true;
}
