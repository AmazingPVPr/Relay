#include "RelayDiscordBridge.h"
#include "RelayPeers.h"
#include "DiscordClient.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QUrlQuery>
#include <QUuid>
#include <sodium.h>

namespace {
const QString Marker = "\nrelay-setup-v1:";
const QString Heading = "Relay connection setup — sent automatically. Please don't reply.";
const QRegularExpression Snowflake("^[0-9]{1,22}$"), Key("^[0-9a-f]{64}$"), Uuid("^[0-9a-f-]{36}$");
qint64 now() { return QDateTime::currentSecsSinceEpoch(); }
bool validKey(const QString &key) {
    if (!Key.match(key).hasMatch()) return false;
    const auto bytes = QByteArray::fromHex(key.toLatin1()); unsigned char curve[32];
    return crypto_sign_ed25519_pk_to_curve25519(curve, reinterpret_cast<const unsigned char *>(bytes.constData())) == 0;
}
}
RelayDiscordBridge::RelayDiscordBridge(DiscordClient *client, RelayPeers *peers, QObject *parent)
    : QObject(parent), m_client(client), m_peers(peers) {
    m_peers->m_externalOnly = true;
    m_scanTimer.setSingleShot(true); m_scanTimer.setInterval(500);
    connect(&m_scanTimer, &QTimer::timeout, this, &RelayDiscordBridge::scan);
    connect(client, &DiscordClient::stateChanged, this, &RelayDiscordBridge::accountChanged);
    connect(client, &DiscordClient::relaySetupObserved, this, &RelayDiscordBridge::receive);
    connect(peers, &RelayPeers::externalSignal, this, &RelayDiscordBridge::sendSignal);
    connect(peers, &RelayPeers::externalReconnect, this, [this](const QString &peer) {
        for (auto it=m_peers->m_pairings.begin();it!=m_peers->m_pairings.end();++it)
            if (it.value().toObject().value("peer").toString()==peer && it.key().startsWith(m_account+":")) { reconnect(it.key()); break; }
    });
    accountChanged();
}
QString RelayDiscordBridge::bindingFor(const QString &channel, const QString &author) const {
    if (!m_client || m_client->m_demoMode || m_account.isEmpty() || m_client->currentUserId()!=m_account) return {};
    const auto dm=m_client->m_dmObjects.value(channel);
    if (dm.value("type").toInt()!=1 || m_client->pendingMessageRequest(dm)) return {};
    const auto recipients=dm.value("recipients").toArray();
    if (recipients.size()!=1) return {};
    const auto recipient=recipients.first().toObject().value("id").toString();
    if (!Snowflake.match(recipient).hasMatch() || recipient==m_account || m_client->m_blockedUsers.contains(recipient)
        || m_client->m_relationships.value(recipient).value("user_ignored").toBool()
        || (!author.isEmpty() && author!=recipient)) return {};
    return m_account+":"+recipient;
}
QString RelayDiscordBridge::nameFor(const QString &channel) const {
    const auto recipients=m_client->m_dmObjects.value(channel).value("recipients").toArray();
    return recipients.isEmpty() ? QString("Your friend") : DiscordClient::displayName(recipients.first().toObject());
}
QJsonObject RelayDiscordBridge::decode(const QString &content) {
    if (content.size()>2000 || !content.startsWith(Heading)) return {};
    const auto at=content.indexOf(Marker); if (at<0 || content.indexOf(Marker,at+1)>=0) return {};
    const auto encoded=content.mid(at+Marker.size()).toLatin1();
    const auto decoded=QByteArray::fromBase64Encoding(encoded,QByteArray::Base64UrlEncoding|QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size()>1600) return {};
    const auto body=QJsonDocument::fromJson(decoded.decoded).object();
    const auto kind=body.value("kind").toString();
    if (body.value("v").toInt()!=1 || !QStringList{"invite","disable","signal"}.contains(kind)
        || !Snowflake.match(body.value("from").toString()).hasMatch() || !Snowflake.match(body.value("to").toString()).hasMatch()
        || !validKey(body.value("key").toString())) return {};
    return body;
}
QString RelayDiscordBridge::encode(const QJsonObject &body) {
    const auto kind=body.value("kind").toString();
    const QString description=kind=="invite" ? "To accept, open this DM in Relay, choose Relay, and enable Relay chat."
        : kind=="disable" ? "Relay chat was turned off. This is an automatic connection notice."
        : "Encrypted connection details for Relay. Your chat text is sent directly between the apps.";
    return Heading+"\n"+description+Marker+QString::fromLatin1(QJsonDocument(body).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals));
}
QString RelayDiscordBridge::setupSummary(const QString &content) {
    const auto body=decode(content); if(body.isEmpty()) return {};
    return body.value("kind")=="invite" ? "Relay chat invitation — open the Relay tab to accept. Automatic setup; please don't reply."
        : body.value("kind")=="disable" ? "Relay chat was turned off. Automatic setup; please don't reply."
        : "Relay encrypted connection setup. Automatic message; please don't reply.";
}
bool RelayDiscordBridge::persist(const QString &binding, const QJsonObject &row) {
    const auto previous=m_peers->m_pairings;
    if (!previous.contains(binding) && previous.size()>=128) return false;
    m_peers->m_pairings.insert(binding,row);
    if (!m_peers->saveSettings()) { m_peers->m_pairings=previous; emit changed(); return false; }
    emit changed(); return true;
}
void RelayDiscordBridge::status(const QString &binding,const QString &text) {
    auto row=m_peers->m_pairings.value(binding).toObject();row.insert("status",text);persist(binding,row);
}
QVariantMap RelayDiscordBridge::conversation(const QString &binding) const {
    if (!m_peers || !binding.startsWith(m_account+":") || m_account.isEmpty()) return {};
    const auto row=m_peers->m_pairings.value(binding).toObject();
    return {{"enabled",row.value("enabled").toBool()},{"pending",!row.value("enabled").toBool() && !row.value("remoteOffer").toString().isEmpty()
        && row.value("remoteOffer")!=row.value("dismissed") && !row.value("remoteDisabled").toBool()},
        {"status",row.value("status").toString()},{"name",row.value("name").toString()},
        {"identityChanged",row.value("identityChanged").toBool()},{"pendingKey",row.value("candidate").toString()},{"channel",row.value("channel").toString()}};
}
QVariantList RelayDiscordBridge::invitations() const {
    QVariantList rows;if(!m_peers)return rows;
    for(auto it=m_peers->m_pairings.begin();it!=m_peers->m_pairings.end();++it) {
        const auto state=conversation(it.key());if(state.value("pending").toBool()) {auto row=state;row.insert("binding",it.key());rows.append(row);}
    }
    return rows;
}
void RelayDiscordBridge::accountChanged() {
    const auto account=m_client->m_demoMode ? QString{} : m_client->currentUserId();
    const bool connected=m_client->connected() && !account.isEmpty();
    if (m_account!=account) {
        ++m_generation;m_outbound.clear();m_sending=false;m_scanned.clear();m_assemblies.clear();m_scanTimer.stop();
        // Account changes revoke the running AI endpoint and close direct sessions.
        m_peers->setMcpEnabled(false);m_peers->setEnabled(false);m_peers->setSelectedPeer({});
        m_account=account;m_connected=false;
        for(auto it=m_peers->m_pairings.begin();it!=m_peers->m_pairings.end();++it) {
            const auto row=it.value().toObject();const auto peer=row.value("peer").toString();
            if(m_peers->m_contacts.contains(peer) && m_peers->m_contacts[peer].external) {
                const bool active=it.key().startsWith(account+":") && row.value("enabled").toBool() && row.value("mutual").toBool() && !row.value("identityChanged").toBool();
                m_peers->setContactActive(peer,active);
            }
        }
        emit changed();
    }
    if (connected && !m_connected) {
        m_connected=true;
        for(auto it=m_peers->m_pairings.begin();it!=m_peers->m_pairings.end();++it) {
            const auto row=it.value().toObject();
            if(it.key().startsWith(m_account+":") && row.value("enabled").toBool() && bindingFor(row.value("channel").toString())==it.key()) {
                m_peers->setEnabled(true);sendInvite(it.key(),row.value("remoteOffer").toString());
            }
        }
    } else if (!connected) m_connected=false;
    if(connected && !m_scanTimer.isActive()) m_scanTimer.start();
}
void RelayDiscordBridge::scan() {
    if (!m_client || !m_client->connected() || m_account.isEmpty()) return;
    int queued=0;
    // Existing DM permissions, rate limits and generation checks stay in DiscordClient.
    // This bounded discovery only examines setup envelopes; nothing is exposed to MCP.
    for (int i=0;i<m_client->m_dms.count() && queued<128;++i) {
        const auto channel=m_client->m_dms.get(i).value("recordId").toString();
        if(bindingFor(channel).isEmpty())continue;
        const auto tail=m_client->m_dmObjects.value(channel).value("last_message_id").toString();
        if(tail.isEmpty() || m_scanned.value(channel)==tail)continue;
        m_scanned.insert(channel,tail);++queued;
        const auto generation=m_generation;
        const QPointer<RelayDiscordBridge> alive(this);
        m_client->request("GET","/channels/"+channel+"/messages?limit=50",{},[this,alive,channel,generation](const QJsonValue &value,const QString &error){
            if(!alive || generation!=m_generation)return;
            if(!error.isEmpty()){m_scanned.remove(channel);return;}
            const auto rows=value.toArray();
            for(qsizetype i=rows.size();i>0;--i) {auto message=rows.at(i-1).toObject();if(message.value("channel_id").toString()==channel)receive(message);}
        },{},0,[this,alive,generation,channel]{return alive && m_client && generation==m_generation && !bindingFor(channel).isEmpty();});
    }
}
void RelayDiscordBridge::setConversationEnabled(const QString &binding,const QString &channel,bool enabled) {
    if(bindingFor(channel)!=binding || (enabled && !m_client->connected()))return;
    auto row=m_peers->m_pairings.value(binding).toObject();
    if(enabled && row.value("identityChanged").toBool()){status(binding,"Their Relay identity changed. Review Details before connecting.");return;}
    row.insert("enabled",enabled);row.insert("channel",channel);row.insert("name",nameFor(channel));
    if(enabled) {
        row.insert("offer",QUuid::createUuid().toString(QUuid::WithoutBraces));
        row.insert("status",row.value("remoteOffer").toString().isEmpty()?"Invitation sent · waiting for them to enable Relay chat":"Connecting directly…");
    } else {row.insert("dismissed",row.value("remoteOffer"));row.insert("mutual",false);row.insert("status","Relay chat is off. Your local history is kept.");}
    if(!persist(binding,row))return;
    if(enabled) {
        m_peers->setEnabled(true);sendInvite(binding,row.value("remoteOffer").toString());
        if(!row.value("remoteOffer").toString().isEmpty() && !row.value("remoteDisabled").toBool())connectPeer(binding);
    } else {
        m_peers->setContactActive(row.value("peer").toString(),false);
        queue(binding,{{"kind","disable"}});
    }
}
void RelayDiscordBridge::sendInvite(const QString &binding,const QString &replyTo) {
    const auto row=m_peers->m_pairings.value(binding).toObject();
    queue(binding,{{"kind","invite"},{"offer",row.value("offer")},{"replyTo",replyTo}});
}
void RelayDiscordBridge::connectPeer(const QString &binding) {
    auto row=m_peers->m_pairings.value(binding).toObject();
    if(!row.value("enabled").toBool() || row.value("remoteDisabled").toBool() || row.value("identityChanged").toBool())return;
    const auto key=row.value("candidate").toString();if(!validKey(key))return;
    const auto pinned=row.value("peer").toString();
    if(!pinned.isEmpty() && pinned!=key){row.insert("identityChanged",true);row.insert("status","Their Relay identity changed. Review Details before connecting.");persist(binding,row);m_peers->setContactActive(pinned,false);return;}
    const auto card="relay-contact-v1:"+QString::fromLatin1(QJsonDocument(QJsonObject{{"key",key},{"rendezvous",m_peers->rendezvousUrl()}}).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals));
    const auto peer=m_peers->addContact(card,row.value("name").toString(),binding,true);if(peer.isEmpty())return;
    row.insert("peer",peer);row.insert("mutual",true);row.insert("status","Connecting directly…");
    if(!persist(binding,row))return;
    m_peers->setContactActive(peer,true);m_peers->setEnabled(true);m_peers->startExternalConnection(peer);
}
void RelayDiscordBridge::receive(const QJsonObject &message) {
    if(!m_client || !m_peers || !message.value("guild_id").toString().isEmpty() || !message.value("webhook_id").toString().isEmpty()
        || message.value("author").toObject().value("bot").toBool() || message.value("type").toInt()!=0)return;
    const auto channel=message.value("channel_id").toString(), author=message.value("author").toObject().value("id").toString();
    const auto binding=bindingFor(channel,author);if(binding.isEmpty())return;
    const auto body=decode(message.value("content").toString());
    if(body.isEmpty() || body.value("from").toString()!=author || body.value("to").toString()!=m_account)return;
    const auto timestamp=QDateTime::fromString(message.value("timestamp").toString(),Qt::ISODateWithMs);
    if(!timestamp.isValid() || timestamp.toSecsSinceEpoch()>now()+300 || now()-timestamp.toSecsSinceEpoch()>7*86400)return;
    const auto kind=body.value("kind").toString(), key=body.value("key").toString();
    auto row=m_peers->m_pairings.value(binding).toObject();
    const auto pinned=row.value("peer").toString();
    if(kind=="signal") {
        if(!row.value("enabled").toBool() || !row.value("mutual").toBool() || pinned!=key || row.value("identityChanged").toBool())return;
        const int count=body.value("count").toInt(),part=body.value("part").toInt(-1);const auto id=body.value("id").toString(),data=body.value("data").toString();
        if(count<1 || count>128 || part<0 || part>=count || data.isEmpty() || data.size()>850 || !Uuid.match(id).hasMatch()
            || now()-timestamp.toSecsSinceEpoch()>90 || !QRegularExpression("^[A-Za-z0-9+/=]+$").match(data).hasMatch())return;
        for(auto it=m_assemblies.begin();it!=m_assemblies.end();)if(now()-it->created>90)it=m_assemblies.erase(it);else ++it;
        const auto assemblyKey=binding+":"+id;
        if(!m_assemblies.contains(assemblyKey) && m_assemblies.size()>=16)return;
        auto &assembly=m_assemblies[assemblyKey];
        if(!assembly.count){assembly.count=count;assembly.created=now();}
        if(assembly.count!=count || (assembly.parts.contains(part) && assembly.parts.value(part)!=data)){m_assemblies.remove(assemblyKey);return;}
        assembly.parts.insert(part,data);
        if(assembly.parts.size()==count) {
            QString payload;for(int i=0;i<count;++i)payload+=assembly.parts.value(i);
            m_assemblies.remove(assemblyKey);
            m_peers->signalReceived({{"from",pinned},{"payload",payload}},true);
        }
        return;
    }
    const auto id=message.value("id").toString();
    if(!Snowflake.match(id).hasMatch())return;
    const auto last=row.value("lastMessage").toString();
    if(!last.isEmpty() && !DiscordClient::newerSnowflake(id,last))return;
    row.insert("lastMessage",id);row.insert("channel",channel);row.insert("name",nameFor(channel));
    if(kind=="disable") {
        if(pinned.isEmpty() || pinned!=key)return;
        row.insert("remoteDisabled",true);row.insert("mutual",false);row.insert("status","They turned off Relay chat. Your local history is kept.");
        if(persist(binding,row))m_peers->setContactActive(pinned,false);
        return;
    }
    const auto offer=body.value("offer").toString();if(!Uuid.match(offer).hasMatch())return;
    const bool newOffer=row.value("remoteOffer").toString()!=offer;
    row.insert("candidate",key);row.insert("remoteOffer",offer);row.insert("remoteDisabled",false);
    if(!pinned.isEmpty() && pinned!=key) {
        row.insert("identityChanged",true);row.insert("mutual",false);row.insert("status","Their Relay identity changed. Review Details before connecting.");
        if(persist(binding,row))m_peers->setContactActive(pinned,false);
        emit invitationReceived(channel,row.value("name").toString());return;
    }
    row.insert("status",row.value("enabled").toBool()?"Connecting directly…":"Wants to connect with you on Relay");
    if(!persist(binding,row))return;
    if(row.value("enabled").toBool()) {
        connectPeer(binding);
        if(body.value("replyTo").toString()!=row.value("offer").toString() && newOffer)sendInvite(binding,offer);
    } else if(newOffer && row.value("dismissed")!=offer)emit invitationReceived(channel,row.value("name").toString());
}
void RelayDiscordBridge::queue(const QString &binding,QJsonObject body) {
    const auto row=m_peers->m_pairings.value(binding).toObject();
    const auto channel=row.value("channel").toString();
    if(bindingFor(channel)!=binding || !m_client->connected() || m_outbound.size()>=256)return;
    body.insert("v",1);body.insert("from",m_account);body.insert("to",binding.section(':',1));body.insert("key",m_peers->identity());
    const auto content=encode(body);if(content.size()>2000)return;
    m_outbound.enqueue({binding,channel,content,body.value("kind").toString(),row.value("offer").toString(),m_generation});sendNext();
}
void RelayDiscordBridge::sendNext() {
    if(m_sending || m_outbound.isEmpty() || !m_client)return;
    const auto outgoing=m_outbound.dequeue();
    const QPointer<RelayDiscordBridge> alive(this);
    const auto valid=[this,alive,outgoing]{
        if(!alive || !m_client || outgoing.generation!=m_generation || !m_client->connected() || bindingFor(outgoing.channel)!=outgoing.binding)return false;
        const auto row=m_peers->m_pairings.value(outgoing.binding).toObject();
        return row.value("offer").toString()==outgoing.offer && (outgoing.kind=="disable"?!row.value("enabled").toBool():row.value("enabled").toBool());
    };
    if(!valid()){QTimer::singleShot(0,this,&RelayDiscordBridge::sendNext);return;}
    m_sending=true;
    QJsonObject body{{"content",outgoing.content},{"nonce",QString::number(QRandomGenerator::global()->generate64())},{"enforce_nonce",true},
        {"tts",false},{"allowed_mentions",QJsonObject{{"parse",QJsonArray{}},{"replied_user",false}}}};
    m_client->request("POST","/channels/"+outgoing.channel+"/messages",body,[this,alive,outgoing](const QJsonValue &value,const QString &error){
        if(!alive || outgoing.generation!=m_generation)return;
        m_sending=false;
        if(!error.isEmpty() || !Snowflake.match(value.toObject().value("id").toString()).hasMatch()) {
            status(outgoing.binding,"Discord could not send the Relay setup message. Use Reconnect to try again.");
            QQueue<Outbound> keep;while(!m_outbound.isEmpty()){auto item=m_outbound.dequeue();if(item.binding!=outgoing.binding)keep.enqueue(item);}m_outbound=keep;
        }
        sendNext();
    },{},0,valid);
}
void RelayDiscordBridge::sendSignal(const QString &peer,const QString &payload) {
    for(auto it=m_peers->m_pairings.begin();it!=m_peers->m_pairings.end();++it) {
        const auto row=it.value().toObject();if(!it.key().startsWith(m_account+":") || row.value("peer").toString()!=peer || !row.value("enabled").toBool() || !row.value("mutual").toBool())continue;
        const int count=(payload.size()+849)/850;if(count>128 || m_outbound.size()+count>256){status(it.key(),"Connection setup is busy. Use Reconnect to try again.");return;}
        const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        for(int part=0;part<count;++part)queue(it.key(),{{"kind","signal"},{"id",id},{"part",part},{"count",count},{"data",payload.mid(part*850,850)}});
        break;
    }
}
void RelayDiscordBridge::reconnect(const QString &binding) {
    auto row=m_peers->m_pairings.value(binding).toObject();if(!row.value("enabled").toBool() || bindingFor(row.value("channel").toString())!=binding)return;
    const auto peer=row.value("peer").toString();
    m_peers->closeTransport(peer);m_peers->m_lastAttempts.remove(peer);
    row.insert("offer",QUuid::createUuid().toString(QUuid::WithoutBraces));row.insert("status","Trying the direct connection again…");
    if(!persist(binding,row))return;
    sendInvite(binding,row.value("remoteOffer").toString());connectPeer(binding);
}
void RelayDiscordBridge::dismiss(const QString &binding) {
    if(!binding.startsWith(m_account+":"))return;
    auto row=m_peers->m_pairings.value(binding).toObject();row.insert("dismissed",row.value("remoteOffer"));persist(binding,row);
}
void RelayDiscordBridge::resetIdentity(const QString &binding) {
    if(!binding.startsWith(m_account+":"))return;
    auto row=m_peers->m_pairings.value(binding).toObject();
    const auto peer=row.value("peer").toString();m_peers->setContactActive(peer,false);
    if(m_peers->m_contacts.contains(peer) && m_peers->m_contacts[peer].binding==binding){m_peers->m_contacts[peer].binding.clear();m_peers->saveSettings();}
    row.remove("peer");row.insert("enabled",false);row.insert("mutual",false);row.insert("identityChanged",false);
    row.insert("status","Old identity unlinked. Enable Relay chat to trust the current invitation.");persist(binding,row);
}
