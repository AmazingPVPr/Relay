#include "MessageReferences.h"
#include "DiscordClient.h"
#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>

namespace {
const QRegularExpression channelToken(QStringLiteral("<#([0-9]{1,20})>"));
const QRegularExpression channelLink(QStringLiteral("^relay-channel:([0-9]{1,20})$"));
const QRegularExpression guildLink(QStringLiteral("^relay-guild:([0-9]{1,20}):(customize|browse)$"));
}

MessageReferences::MessageReferences(DiscordClient *client,QObject *parent):QObject(parent),m_client(client) {
    if(!client)return;
    m_session=client->m_generation;
    invalidateReferences();
    connect(client,&DiscordClient::channelReferencesChanged,this,&MessageReferences::invalidateReferences);
    connect(client,&DiscordClient::stateChanged,this,[this] {
        if(!m_client)return;
        if(m_session!=m_client->m_generation){m_session=m_client->m_generation;reset();invalidateReferences();}
        if(!m_guild.isEmpty() && !memberOf(m_guild))reset();
    });
    connect(client,&DiscordClient::guildRemoved,this,[this](const QString &id) {
        if(id==m_guild)reset();
        invalidateReferences();
    });
    connect(client,&QObject::destroyed,this,[this]{reset();invalidateReferences();});
}
bool MessageReferences::ready()const {
    return m_client && m_client->connected() && !m_client->demoMode() && !m_client->m_token.isEmpty() && m_session==m_client->m_generation;
}
bool MessageReferences::memberOf(const QString &guild)const {
    return m_client && DiscordClient::validId(guild) && m_client->m_guildObjects.contains(guild)
        && !m_client->m_removedGuilds.contains(guild);
}
void MessageReferences::reset() {
    ++m_operation;++m_channelOperation;m_guild.clear();m_section="customize";m_error.clear();m_status.clear();
    m_prompts.clear();m_original.clear();m_selected.clear();m_seenPrompts={};m_seenOptions={};
    m_loading=m_saving=m_loaded=false;emit changed();
}
void MessageReferences::close(){reset();}
void MessageReferences::invalidateReferences(){
    m_channelsById.clear();m_channelGuilds.clear();
    if(m_client)for(auto guild=m_client->m_channelCache.cbegin();guild!=m_client->m_channelCache.cend();++guild)
        for(const auto &entry:guild.value()){
            const auto channel=entry.toObject();const auto id=channel.value("id").toString();
            m_channelsById.insert(id,channel);m_channelGuilds.insert(id,guild.key());
        }
    ++m_revision;emit referencesChanged();if(!m_guild.isEmpty())emit changed();
}
QString MessageReferences::guildName()const {
    return m_client?m_client->m_guildObjects.value(m_guild).value("name").toString("Server"):QString{};
}
QJsonObject MessageReferences::knownChannel(const QString &id,QString *guild)const {
    if(guild)*guild=m_channelGuilds.value(id);
    return m_channelsById.value(id);
}
QVariantMap MessageReferences::resolve(const QString &input,const QString &guild)const {
    QVariantList channels;QSet<QString> seen;
    auto matches=channelToken.globalMatch(input.left(20000));
    while(matches.hasNext() && seen.size()<100){
        const auto id=matches.next().captured(1);if(seen.contains(id))continue;seen.insert(id);
        QString owner;const auto channel=knownChannel(id,&owner);
        const bool visible=!channel.isEmpty() && memberOf(owner) && m_client->channelVisible(channel,owner);
        QVariantMap item{{"id",id},{"known",!channel.isEmpty()},{"accessible",visible}};
        if(visible)item.insert("name",DiscordClient::cleanText(channel.value("name").toString(),100));
        channels.append(item);
    }
    return {{"guildId",memberOf(guild)?guild:QString{}},{"mentionChannels",channels}};
}
QVariantList MessageReferences::channels()const {
    QVariantList result;if(!memberOf(m_guild))return result;
    for(const auto &entry:m_client->m_channelCache.value(m_guild)){
        const auto channel=entry.toObject();const auto kind=DiscordClient::channelKind(channel.value("type").toInt(-1));
        if(kind.isEmpty() || kind=="category" || kind=="thread" || !m_client->channelVisible(channel,m_guild))continue;
        const auto parent=m_client->cachedChannel(m_guild,channel.value("parent_id").toString());
        result.append(QVariantMap{{"id",channel.value("id").toString()},{"name",DiscordClient::cleanText(channel.value("name").toString(),100)},
            {"kind",kind},{"category",DiscordClient::cleanText(parent.value("name").toString(),100)},{"position",channel.value("position").toInt()}});
        if(result.size()>=2000)break;
    }
    std::stable_sort(result.begin(),result.end(),[](const auto &a,const auto &b){
        const auto am=a.toMap(),bm=b.toMap();
        const auto order=QString::localeAwareCompare(am.value("category").toString(),bm.value("category").toString());
        return order?order<0:am.value("position").toInt()<bm.value("position").toInt();
    });
    return result;
}
bool MessageReferences::activate(const QString &link) {
    if(const auto nav=guildLink.match(link);nav.hasMatch())return openGuild(nav.captured(1),nav.captured(2));
    const auto target=channelLink.match(link);
    if(!target.hasMatch() || !m_client)return false;
    const auto id=target.captured(1);QString guild;const auto channel=knownChannel(id,&guild);
    if(!channel.isEmpty()){
        if(!memberOf(guild) || !m_client->channelVisible(channel,guild) || DiscordClient::channelKind(channel.value("type").toInt())=="category"){
            m_error="This channel is unavailable or you don’t have access to it.";emit changed();emit panelRequested();return false;
        }
        ++m_channelOperation;emit channelRequested(id);return true;
    }
    if(!ready())return false;
    const auto operation=++m_channelOperation,session=m_session,selection=m_client->m_selectionGeneration;
    QPointer<MessageReferences> self(this);
    m_client->request("GET","/channels/"+id,{},[self,id,operation,session,selection](const QJsonValue &result,const QString &error){
        if(!self || !self->m_client || self->m_channelOperation!=operation || self->m_session!=session || self->m_client->m_generation!=session
            || self->m_client->m_selectionGeneration!=selection)return;
        const auto channel=result.toObject();const auto guild=channel.value("guild_id").toString();
        if(!error.isEmpty() || channel.value("id").toString()!=id || !self->memberOf(guild) ||
            !self->m_client->channelVisible(channel,guild) || DiscordClient::channelKind(channel.value("type").toInt(-1)).isEmpty() ||
            DiscordClient::channelKind(channel.value("type").toInt(-1))=="category"){
            self->m_error=error.isEmpty()?"This channel is unavailable or you don’t have access to it.":error;
            emit self->changed();emit self->panelRequested();return;
        }
        auto cache=self->m_client->m_channelCache.value(guild);cache.append(channel);self->m_client->cacheChannels(guild,cache);
        emit self->channelRequested(id);
    },{},0,[self,operation,session,selection]{
        return self && self->ready() && self->m_session==session && self->m_channelOperation==operation && self->m_client->m_selectionGeneration==selection;
    });
    return true;
}
bool MessageReferences::openGuild(const QString &guild,const QString &section) {
    if(!memberOf(guild) || (section!="customize" && section!="browse"))return false;
    if(!ready()){m_error="Reconnect to Discord to open this server’s choices.";emit changed();emit panelRequested();return false;}
    if(guild!=m_guild){reset();m_guild=guild;}
    m_section=section;m_error.clear();m_status.clear();emit changed();emit panelRequested();
    if(section=="customize" && !m_loaded && !m_loading)refresh();
    else if(section=="browse" && !m_client->m_channelCache.contains(guild)){
        const auto operation=m_operation,session=m_session;QPointer<MessageReferences> self(this);
        m_loading=true;emit changed();
        m_client->request("GET","/guilds/"+guild+"/channels",{},[self,guild,operation,session](const QJsonValue &result,const QString &error){
            if(!self || !self->m_client || self->m_operation!=operation || self->m_session!=session || self->m_client->m_generation!=session || self->m_guild!=guild)return;
            self->m_loading=false;self->m_error=error;
            if(error.isEmpty() && result.isArray())self->m_client->cacheChannels(guild,result.toArray(),true);
            emit self->changed();
        });
    }
    return true;
}
void MessageReferences::refresh() {
    if(!ready() || !memberOf(m_guild) || m_saving || m_loading)return;
    const auto operation=++m_operation,session=m_session;const auto guild=m_guild;
    m_loading=true;m_error.clear();m_status.clear();emit changed();QPointer<MessageReferences> self(this);
    m_client->request("GET","/guilds/"+guild+"/onboarding",{},[self,guild,operation,session](const QJsonValue &result,const QString &error){
        if(!self || !self->m_client || self->m_operation!=operation || self->m_session!=session || self->m_client->m_generation!=session || self->m_guild!=guild)return;
        self->m_loading=false;self->m_error=error;
        if(error.isEmpty())self->acceptOnboarding(result.toObject());
        emit self->changed();
    });
}
void MessageReferences::acceptOnboarding(const QJsonObject &data) {
    m_loaded=false;
    if(data.value("guild_id").toString()!=m_guild || !data.value("prompts").isArray()){
        m_error="Discord returned incomplete server choices. Try again.";return;
    }
    if(!data.value("enabled").toBool()){m_prompts.clear();m_original.clear();m_selected.clear();m_loaded=true;return;}
    if(!data.value("responses").isArray() || data.value("responses").toArray().size()>750 || data.value("prompts").toArray().size()>100){
        m_error="Discord didn’t return your existing choices. Try again before changing them.";return;
    }
    QVariantList prompts;QSet<QString> promptIds,optionIds;
    for(const auto &entry:data.value("prompts").toArray()){
        const auto prompt=entry.toObject();const auto id=prompt.value("id").toString();
        if(!DiscordClient::validId(id) || promptIds.contains(id) || !prompt.value("options").isArray() || prompt.value("options").toArray().size()>50){
            m_error="Discord returned invalid server choices. Try again.";return;
        }
        promptIds.insert(id);QVariantList options;
        for(const auto &value:prompt.value("options").toArray()){
            const auto option=value.toObject();const auto optionId=option.value("id").toString();
            if(!DiscordClient::validId(optionId) || optionIds.contains(optionId) || optionIds.size()>=750){
                m_error="Discord returned invalid server choices. Try again.";return;
            }
            optionIds.insert(optionId);
            options.append(QVariantMap{{"id",optionId},{"title",DiscordClient::cleanText(option.value("title").toString(),100)},
                {"description",DiscordClient::cleanText(option.value("description").toString(),400)}});
        }
        prompts.append(QVariantMap{{"id",id},{"title",DiscordClient::cleanText(prompt.value("title").toString(),200)},
            {"single",prompt.value("single_select").toBool()},{"required",prompt.value("required").toBool()},{"options",options}});
    }
    QSet<QString> responses;
    for(const auto &value:data.value("responses").toArray()){
        if(!value.isString() || !DiscordClient::validId(value.toString())){m_error="Discord returned invalid existing choices.";return;}
        responses.insert(value.toString());
    }
    m_prompts=prompts;m_original=m_selected=responses;
    const auto seen=[](const QJsonObject &source){
        QJsonObject safe;
        for(auto it=source.begin();it!=source.end() && safe.size()<1000;++it){
            if(DiscordClient::validId(it.key()) && it.value().isDouble() && it.value().toDouble()>=0 && it.value().toDouble()<=9007199254740991.)
                safe.insert(it.key(),it.value());
        }
        return safe;
    };
    m_seenPrompts=seen(data.value("onboarding_prompts_seen").toObject());
    m_seenOptions=seen(data.value("onboarding_responses_seen").toObject());m_loaded=true;
}
QVariantList MessageReferences::prompts()const {
    auto rows=m_prompts;
    for(auto &entry:rows){
        auto prompt=entry.toMap();auto options=prompt.value("options").toList();
        for(auto &value:options){auto option=value.toMap();option.insert("selected",m_selected.contains(option.value("id").toString()));value=option;}
        prompt.insert("options",options);entry=prompt;
    }
    return rows;
}
bool MessageReferences::choose(const QString &promptId,const QString &optionId,bool selected) {
    if(!ready()){m_error="Reconnect to Discord before changing server choices.";emit changed();return false;}
    if(!m_loaded || m_loading || m_saving)return false;
    for(const auto &entry:m_prompts){
        const auto prompt=entry.toMap();if(prompt.value("id").toString()!=promptId)continue;
        const auto options=prompt.value("options").toList();
        if(std::none_of(options.begin(),options.end(),[&](const auto &v){return v.toMap().value("id").toString()==optionId;}))return false;
        auto next=m_selected;
        if(selected && prompt.value("single").toBool())for(const auto &value:options)next.remove(value.toMap().value("id").toString());
        if(selected)next.insert(optionId);else next.remove(optionId);
        if(next.size()>750){m_error="Too many choices are selected. Remove a choice before adding another.";emit changed();return false;}
        m_selected=std::move(next);
        const auto now=QDateTime::currentMSecsSinceEpoch();m_seenPrompts.insert(promptId,now);m_seenOptions.insert(optionId,now);
        m_error.clear();m_status.clear();emit changed();return true;
    }
    return false;
}
bool MessageReferences::save() {
    if(!ready()){m_error="Reconnect to Discord before saving your server choices.";emit changed();return false;}
    if(!memberOf(m_guild) || !m_loaded || m_loading || m_saving || !dirty())return false;
    for(const auto &entry:m_prompts){
        const auto prompt=entry.toMap();if(!prompt.value("required").toBool())continue;
        bool selected=false;for(const auto &option:prompt.value("options").toList())selected=selected || m_selected.contains(option.toMap().value("id").toString());
        if(!selected){m_error="Choose an option for “"+prompt.value("title").toString()+"” before saving.";emit changed();return false;}
    }
    QStringList values(m_selected.begin(),m_selected.end());std::sort(values.begin(),values.end());QJsonArray responses;for(const auto &id:values)responses.append(id);
    const QJsonObject body{{"onboarding_responses",responses},{"onboarding_prompts_seen",m_seenPrompts},{"onboarding_responses_seen",m_seenOptions}};
    const auto operation=m_operation,session=m_session;const auto guild=m_guild;const auto submitted=m_selected;QPointer<MessageReferences> self(this);
    m_saving=true;m_error.clear();m_status.clear();emit changed();
    m_client->request("PUT","/guilds/"+guild+"/onboarding-responses",body,[self,guild,operation,session,submitted](const QJsonValue &,const QString &error){
        if(!self || !self->m_client || self->m_operation!=operation || self->m_session!=session || self->m_client->m_generation!=session || self->m_guild!=guild)return;
        self->m_saving=false;self->m_error=error;
        if(error.isEmpty()){self->m_original=submitted;self->m_status="Your server choices have been saved.";self->refreshAccess(guild,session);}
        emit self->changed();
    },{},0,[self,guild,operation,session]{return self && self->ready() && self->memberOf(guild) && self->m_guild==guild && self->m_operation==operation && self->m_session==session;});
    return true;
}
void MessageReferences::refreshAccess(const QString &guild,quint64 session) {
    QPointer<MessageReferences> self(this);
    m_client->request("GET","/guilds/"+guild+"/members/@me",{},[self,guild,session](const QJsonValue &result,const QString &error){
        if(!self || !self->m_client || self->m_client->m_generation!=session || !self->memberOf(guild) || !error.isEmpty())return;
        const auto member=result.toObject();if(member.value("user").toObject().value("id").toString()!=self->m_client->currentUserId())return;
        self->m_client->cacheMember(guild,member);self->m_client->m_guildObjects[guild].insert("member",member);
        if(guild==self->m_client->currentGuildId()){self->m_client->rebuildChannels(false);self->m_client->rebuildMembers();}
        if(guild==self->m_client->currentGuildId() || guild==self->m_client->m_voiceGuildId)self->m_client->rebuildVoiceMembers();
        self->invalidateReferences();
    });
    m_client->request("GET","/guilds/"+guild+"/channels",{},[self,guild,session](const QJsonValue &result,const QString &error){
        if(!self || !self->m_client || self->m_client->m_generation!=session || !self->memberOf(guild) || !error.isEmpty() || !result.isArray())return;
        self->m_client->cacheChannels(guild,result.toArray(),true);
        if(guild==self->m_client->currentGuildId())self->m_client->rebuildChannels(false);
    });
}
