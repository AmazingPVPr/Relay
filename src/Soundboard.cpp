#include "Soundboard.h"
#include "DiscordClient.h"
#include "VoiceEngine.h"
#include "GuildManagement.h"
#include "AppSettings.h"
#include <QAudioDecoder>
#include <QAudioBuffer>
#include <QAudioFormat>
#include <QTimer>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <cmath>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QDateTime>
#include <algorithm>
namespace {QString soundId(const QJsonValue &value){return value.isString()?value.toString():value.isDouble()&&value.toInteger()>0?QString::number(value.toInteger()):QString{};}}
struct Soundboard::ReceivedClip final : QObject {
    explicit ReceivedClip(Soundboard *owner):QObject(owner),buffer(this),decoder(this){}
    ~ReceivedClip() override {
        disconnect(&decoder,nullptr,this,nullptr);decoder.stop();
        if(download){disconnect(download,nullptr,this,nullptr);download->abort();download->deleteLater();}
    }
    QString id,user,guild,channel;quint64 generation=0,playbackGeneration=0;double volume=1;
    QBuffer buffer;QAudioDecoder decoder;QByteArray pcm;QPointer<QNetworkReply> download;bool finished=false;
};
Soundboard::Soundboard(DiscordClient *client,VoiceEngine *voice,GuildManagement *permissions,AppSettings *settings,QObject *parent):QObject(parent),m_client(client),m_voice(voice),m_permissions(permissions),m_settings(settings){
    m_generation=client->m_generation;m_preview.setAudioOutput(&m_output);
    m_libraryFile=settings->m_settings->fileName();syncLibrary();
    const auto updateVolume=[this]{m_output.setVolume(float(m_sourceVolume*m_settings->soundboardVolume()));if(m_voice)m_voice->setSoundboardVolume(m_settings->soundboardVolume());};
    connect(settings,&AppSettings::changed,this,updateVolume);updateVolume();
    connect(client,&DiscordClient::soundboardEffect,this,&Soundboard::receiveEffect);
    connect(client,&DiscordClient::stateChanged,this,[this]{syncLibrary();if(m_client && m_generation!=m_client->m_generation){m_generation=m_client->m_generation;m_audioCache.clear();stopReceivedClips();reset();}else emit changed();});
    const auto retireStale=[this]{const auto clips=m_received;for(auto *clip:clips)if(!receivedClipCurrent(clip))finishReceivedClip(clip);};
    connect(voice,&VoiceEngine::micTestStarting,this,retireStale);
    connect(voice,&VoiceEngine::changed,this,[this,retireStale]{
        if(m_voice->deafened())stopPreview();
        if(!m_voice->connected() || m_voice->deafened() || m_voice->micTesting() || (!m_receivedChannel.isEmpty()
            && (m_voice->channelId()!=m_receivedChannel || m_voice->currentGuildId()!=m_receivedGuild)))stopReceivedClips();
        else retireStale();
        emit changed();
    });
    connect(voice,&VoiceEngine::devicesChanged,this,[this]{auto device=QMediaDevices::defaultAudioOutput();for(const auto &candidate:QMediaDevices::audioOutputs())if(QString::fromLatin1(candidate.id().toBase64())==m_voice->outputDeviceId())device=candidate;m_output.setDevice(device);});
    connect(&m_preview,&QMediaPlayer::errorOccurred,this,[this]{m_audioCache.remove(m_previewId);m_error="This sound could not be decoded. Try its preview again.";emit changed();});
}
Soundboard::~Soundboard(){stopReceivedClips();stopPreview();}
bool Soundboard::receivedClipCurrent(const ReceivedClip *clip) const {
    if(!clip || !m_client || !m_voice || !m_voice->connected() || m_voice->deafened() || m_voice->micTesting()
        || clip->generation!=m_client->m_generation || clip->playbackGeneration!=m_voice->soundboardPlaybackGeneration() || clip->channel!=m_voice->channelId() || clip->guild!=m_voice->currentGuildId())return false;
    const auto own=m_client->m_voiceStates.value(clip->guild).value(m_client->currentUserId());
    return !own.value("deaf").toBool() && !own.value("self_deaf").toBool() && !own.value("server_deafened").toBool();
}
void Soundboard::receiveEffect(const QJsonObject &event) {
    if(!m_voice || !m_client || !m_voice->connected() || m_voice->deafened() || m_voice->micTesting()
        || event.value("channel_id").toString()!=m_voice->channelId() || m_voice->userMuted(event.value("user_id").toString()))return;
    const auto id=soundId(event.value("sound_id")),user=event.value("user_id").toString();
    const auto volume=event.value("sound_volume").toDouble(1);
    if(!DiscordClient::validId(id) || !DiscordClient::validId(user) || !std::isfinite(volume) || m_received.size()>=8)return;
    auto *clip=new ReceivedClip(this);clip->id=id;clip->user=user;clip->volume=std::clamp(volume,0.,1.);
    clip->guild=m_voice->currentGuildId();clip->channel=m_voice->channelId();clip->generation=m_client->m_generation;clip->playbackGeneration=m_voice->soundboardPlaybackGeneration();
    if(!receivedClipCurrent(clip)){delete clip;return;}
    m_receivedGuild=clip->guild;m_receivedChannel=clip->channel;m_received.append(clip);
    QTimer::singleShot(10000,clip,[this,clip]{finishReceivedClip(clip,"This sound could not be prepared in time.");});
    if(const auto cached=m_audioCache.object(id)){startReceivedClip(clip,*cached);return;}
    QNetworkRequest request{QUrl(m_cdnBase+id)};request.setTransferTimeout(10000);request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    auto *reply=m_mediaNetwork.get(request);clip->download=reply;reply->setReadBufferSize(2*1024*1024+1);
    connect(reply,&QNetworkReply::downloadProgress,clip,[reply](qint64 received,qint64 total){if(received>2*1024*1024 || total>2*1024*1024)reply->abort();});
    connect(reply,&QNetworkReply::readyRead,clip,[reply]{if(reply->bytesAvailable()>2*1024*1024)reply->abort();});
    connect(reply,&QNetworkReply::finished,clip,[this,clip,reply]{
        clip->download=nullptr;reply->deleteLater();if(clip->finished)return;
        const auto bytes=reply->read(2*1024*1024+1);
        if(!receivedClipCurrent(clip)){finishReceivedClip(clip);return;}
        if(reply->error()!=QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()!=200
            || bytes.isEmpty() || bytes.size()>2*1024*1024){finishReceivedClip(clip,"This sound could not be downloaded.");return;}
        m_audioCache.insert(clip->id,new QByteArray(bytes),qMax(1,int((bytes.size()+1023)/1024)));startReceivedClip(clip,bytes);
    });
}
void Soundboard::startReceivedClip(ReceivedClip *clip,const QByteArray &bytes) {
    if(!receivedClipCurrent(clip)){finishReceivedClip(clip);return;}
    QAudioFormat format;format.setSampleRate(48000);format.setChannelCount(2);format.setSampleFormat(QAudioFormat::Int16);
    clip->buffer.setData(bytes);clip->buffer.open(QIODevice::ReadOnly);clip->decoder.setAudioFormat(format);
    connect(&clip->decoder,&QAudioDecoder::bufferReady,clip,[this,clip,format]{
        if(clip->finished)return;
        const auto buffer=clip->decoder.read();if(!buffer.isValid())return;
        if((buffer.format().sampleRate()!=format.sampleRate() || buffer.format().channelCount()!=format.channelCount() || buffer.format().sampleFormat()!=format.sampleFormat()) || clip->pcm.size()+buffer.byteCount()>48000*4*10){finishReceivedClip(clip,"This sound could not be decoded to supported audio.");return;}
        clip->pcm.append(buffer.constData<char>(),buffer.byteCount());
    });
    connect(&clip->decoder,&QAudioDecoder::finished,clip,[this,clip]{
        if(clip->finished)return;
        if(receivedClipCurrent(clip) && !clip->pcm.isEmpty())m_voice->playReceivedSoundboardPcm(clip->user,clip->pcm,clip->volume,clip->playbackGeneration);
        finishReceivedClip(clip);
    });
    connect(&clip->decoder,qOverload<QAudioDecoder::Error>(&QAudioDecoder::error),clip,[this,clip](QAudioDecoder::Error){finishReceivedClip(clip,"This sound could not be decoded.");});
    clip->decoder.setSourceDevice(&clip->buffer);clip->decoder.start();
}
void Soundboard::finishReceivedClip(ReceivedClip *clip,const QString &error) {
    if(!clip || clip->finished)return;
    clip->finished=true;m_received.removeAll(clip);
    if(!error.isEmpty() && receivedClipCurrent(clip)){m_error=error;emit changed();}
    clip->deleteLater();
}
void Soundboard::stopReceivedClips() {
    const auto clips=std::move(m_received);m_received.clear();
    for(auto *clip:clips){clip->finished=true;delete clip;}
    m_receivedGuild.clear();m_receivedChannel.clear();if(m_voice)m_voice->stopReceivedSoundboards();
}
void Soundboard::reset(){++m_operation;m_pendingActivation.clear();m_sounds.clear();m_guild.clear();m_error.clear();m_pending=0;m_sending=false;stopPreview();emit changed();}
void Soundboard::add(const QJsonArray &items,const QString &guild){
    for(const auto &value:items){if(m_sounds.size()>=500)break;const auto sound=value.toObject();const auto id=soundId(sound.value("sound_id"));
        if(!DiscordClient::validId(id) || sound.value("name").toString().isEmpty() || !find(id).isEmpty())continue;
        double volume=sound.value("volume").toDouble(1);if(!std::isfinite(volume))volume=1;
        m_sounds.append(QVariantMap{{"id",id},{"name",sound.value("name").toString().left(80)},{"guildId",guild},{"emoji",sound.value("emoji_name").toString().left(32)},{"available",sound.value("available").toBool(true)},{"volume",qBound(0.,volume,1.)}});
    }
}
QVariantMap Soundboard::find(const QString &id)const{for(const auto &sound:m_sounds)if(sound.toMap().value("id").toString()==id)return sound.toMap();return {};}
void Soundboard::load(const QString &guild){
    reset();m_guild=guild;
    if(!m_client || !m_client->connected() || m_client->demoMode()){m_error="Sign in to load Discord soundboards.";emit changed();return;}
    const bool hasGuild=DiscordClient::validId(guild) && m_client->m_guildObjects.contains(guild);
    if(!guild.isEmpty() && !hasGuild){m_error="Choose one of your servers.";emit changed();return;}
    m_pending=hasGuild?2:1;const auto operation=m_operation;emit changed();
    auto request=[this,operation](const QString &path,const QString &source){m_client->request("GET",path,{},[weak=QPointer<Soundboard>(this),operation,source](const QJsonValue &value,const QString &error){
        if(!weak || weak->m_operation!=operation)return;
        --weak->m_pending;
        if(!error.isEmpty())weak->m_error=error;
        else {const auto list=source.isEmpty()?value:value.toObject().value("items");if(!list.isArray())weak->m_error="Discord returned an unexpected soundboard response.";else weak->add(list.toArray(),source);}
        if(weak->m_pending==0)weak->finishActivation();
        emit weak->changed();
    });};
    request("/soundboard-default-sounds",{});if(hasGuild)request("/guilds/"+guild+"/soundboard-sounds",guild);
}
bool Soundboard::canPlay()const{
    if(!m_voice || !m_client || !m_voice->connected() || m_voice->deafened() || !m_client->connected() || m_client->demoMode() || !m_permissions)return false;
    const auto state=m_client->m_voiceStates.value(m_voice->currentGuildId()).value(m_client->currentUserId());
    if(state.value("channel_id").toString()!=m_voice->channelId() || state.value("mute").toBool() || state.value("deaf").toBool() || state.value("self_deaf").toBool() || state.value("server_muted").toBool() || state.value("server_deafened").toBool() || state.value("suppress").toBool())return false;
    const auto bits=m_permissions->permissionBits(m_voice->currentGuildId(),m_voice->channelId());
    return (bits&(quint64(1)<<21)) && (bits&(quint64(1)<<42));
}
bool Soundboard::play(const QString &id){
    const auto sound=find(id);
    if(!canPlay() || sound.isEmpty() || !sound.value("available").toBool() || m_sending){m_error="Join a voice channel with permission to use soundboard sounds.";emit changed();return false;}
    const auto source=sound.value("guildId").toString(),guild=m_voice->currentGuildId();
    if(!source.isEmpty()&&!m_client->m_guildObjects.contains(source)){m_error="This sound is no longer available from its server.";emit changed();return false;}
    if(!source.isEmpty() && source!=guild && !(m_permissions->permissionBits(guild,m_voice->channelId())&(quint64(1)<<45))){m_error="This channel does not allow sounds from other servers.";emit changed();return false;}
    if(m_cooldown.isValid() && m_cooldown.elapsed()<500)return false;
    m_cooldown.start();m_sending=true;m_error.clear();const auto operation=m_operation;
    QJsonObject body{{"sound_id",id}};if(!source.isEmpty())body.insert("source_guild_id",source);
    m_client->request("POST","/channels/"+m_voice->channelId()+"/send-soundboard-sound",body,[weak=QPointer<Soundboard>(this),operation,id](const QJsonValue&,const QString &error){if(!weak || weak->m_operation!=operation)return;weak->m_sending=false;weak->m_error=error;if(error.isEmpty())weak->rememberUse(id);emit weak->changed();});
    emit changed();return true;
}
bool Soundboard::preview(const QString &id){const auto sound=find(id);if(sound.isEmpty() || !sound.value("available",true).toBool() || (!sound.value("guildId").toString().isEmpty()&&(!m_client||!m_client->m_guildObjects.contains(sound.value("guildId").toString()))))return false;startPreview(id,sound.value("volume",1.).toDouble());return true;}
void Soundboard::startPreview(const QString &id,double volume){
    stopPreview();m_previewId=id;m_error.clear();m_sourceVolume=volume;m_output.setVolume(float(volume*m_settings->soundboardVolume()));
    auto output=QMediaDevices::defaultAudioOutput();if(m_voice)for(const auto &device:QMediaDevices::audioOutputs())if(QString::fromLatin1(device.id().toBase64())==m_voice->outputDeviceId())output=device;m_output.setDevice(output);
    if(const auto cached=m_audioCache.object(id)){playDownloaded(*cached);return;}
    const auto generation=m_previewGeneration;m_previewLoading=true;
    QNetworkRequest request{QUrl(m_cdnBase+id)};request.setTransferTimeout(10000);request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    auto *reply=m_mediaNetwork.get(request);m_download=reply;reply->setReadBufferSize(2*1024*1024+1);
    connect(reply,&QNetworkReply::downloadProgress,reply,[reply](qint64 received,qint64 total){if(received>2*1024*1024 || total>2*1024*1024)reply->abort();});
    connect(reply,&QNetworkReply::readyRead,reply,[reply]{if(reply->bytesAvailable()>2*1024*1024)reply->abort();});
    connect(reply,&QNetworkReply::finished,this,[this,reply,id,generation]{
        reply->deleteLater();if(generation!=m_previewGeneration)return;
        m_download=nullptr;m_previewLoading=false;const auto bytes=reply->read(2*1024*1024+1);
        if(reply->error()!=QNetworkReply::NoError || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()!=200 || bytes.isEmpty() || bytes.size()>2*1024*1024){m_error="This sound could not be downloaded. Try its preview again.";emit changed();return;}
        m_audioCache.insert(id,new QByteArray(bytes),qMax(1,int((bytes.size()+1023)/1024)));playDownloaded(bytes);
    });emit changed();
}
void Soundboard::playDownloaded(const QByteArray &bytes){
    m_previewData.setData(bytes);m_previewData.open(QIODevice::ReadOnly);
    // Decode the complete, seekable clip. CDN assets have no extension, and
    // streaming them directly through the media backend is format-dependent.
    m_preview.setSourceDevice(&m_previewData);m_preview.play();emit changed();
}
void Soundboard::stopPreview(){if(m_activationPreview)m_pendingActivation.clear();++m_previewGeneration;if(m_download){auto reply=m_download;m_download=nullptr;reply->abort();}m_previewLoading=false;m_preview.stop();m_preview.setSourceDevice(nullptr);m_previewData.close();m_previewData.setData({});emit changed();}

void Soundboard::syncLibrary(){
    const auto account=m_client && DiscordClient::validId(m_client->currentUserId())?QString::fromLatin1(QCryptographicHash::hash(m_client->currentUserId().toUtf8(),QCryptographicHash::Sha256).toHex()):QString{};
    if(account==m_libraryAccount)return;
    m_libraryAccount=account;m_library.clear();m_pendingActivation.clear();
    if(account.isEmpty())return;
    QSettings store(m_libraryFile,QSettings::IniFormat);
    const auto bytes=store.value("accounts/"+account+"/soundboard/library").toByteArray();
    if(bytes.size()>256*1024)return;
    const auto list=QJsonDocument::fromJson(bytes).array();
    for(const auto &value:list){
        if(m_library.size()>=200)break;
        const auto row=value.toObject();const auto id=row.value("id").toString(),guild=row.value("guildId").toString();
        if(!DiscordClient::validId(id) || (!guild.isEmpty()&&!DiscordClient::validId(guild)))continue;
        m_library.insert(id,{{"id",id},{"guildId",guild},{"name",row.value("name").toString().left(80)},{"emoji",row.value("emoji").toString().left(32)},{"favorite",row.value("favorite").toBool()},{"uses",qBound(0,row.value("uses").toInt(),100000)},{"lastUsed",qBound(qint64(0),row.value("lastUsed").toInteger(),QDateTime::currentMSecsSinceEpoch())}});
    }
}
void Soundboard::saveLibrary(){
    if(m_libraryAccount.isEmpty())return;
    while(m_library.size()>200){auto oldest=m_library.end();for(auto it=m_library.begin();it!=m_library.end();++it)if(!it->value("favorite").toBool()&&(oldest==m_library.end()||it->value("lastUsed").toLongLong()<oldest->value("lastUsed").toLongLong()))oldest=it;if(oldest==m_library.end())break;m_library.erase(oldest);}
    QJsonArray rows;for(const auto &row:m_library)rows.append(QJsonObject::fromVariantMap(row));
    QSettings store(m_libraryFile,QSettings::IniFormat);store.setValue("accounts/"+m_libraryAccount+"/soundboard/library",QJsonDocument(rows).toJson(QJsonDocument::Compact));store.sync();
}
QVariantMap Soundboard::libraryRow(const QString &id) const{
    auto row=find(id);if(row.isEmpty()){row=m_library.value(id);row.insert("available",false);row.insert("needsReload",true);}else row.insert("needsReload",false);
    row.insert("favorite",isFavorite(id));return row;
}
bool Soundboard::isFavorite(const QString &id) const{return m_library.value(id).value("favorite").toBool();}
QVariantList Soundboard::favorites() const{
    QVariantList rows;for(auto it=m_library.cbegin();it!=m_library.cend();++it)if(it->value("favorite").toBool())rows.append(libraryRow(it.key()));
    std::sort(rows.begin(),rows.end(),[](const QVariant &a,const QVariant &b){return a.toMap().value("name").toString().compare(b.toMap().value("name").toString(),Qt::CaseInsensitive)<0;});return rows;
}
QVariantList Soundboard::frequent() const{
    QVariantList records;for(const auto &row:m_library)if(row.value("uses").toInt()>0)records.append(row);
    std::sort(records.begin(),records.end(),[](const QVariant &a,const QVariant &b){const auto x=a.toMap(),y=b.toMap();return x.value("uses").toInt()!=y.value("uses").toInt()?x.value("uses").toInt()>y.value("uses").toInt():x.value("lastUsed").toLongLong()>y.value("lastUsed").toLongLong();});
    QVariantList rows;for(const auto &row:records){if(rows.size()>=12)break;rows.append(libraryRow(row.toMap().value("id").toString()));}return rows;
}
bool Soundboard::setFavorite(const QString &id,bool favorite){
    syncLibrary();const auto current=find(id);
    if(m_libraryAccount.isEmpty() || !DiscordClient::validId(id) || (favorite&&current.isEmpty()&&!m_library.contains(id)))return false;
    if(favorite&&!isFavorite(id)&&favorites().size()>=100){m_error="You can save up to 100 favorite sounds.";emit changed();return false;}
    auto row=m_library.value(id);if(!current.isEmpty()){for(const auto &key:{"id","guildId","name","emoji"})row.insert(key,current.value(key));}
    if(row.isEmpty())return true;
    row.insert("favorite",favorite);m_library.insert(id,row);
    if(!favorite&&row.value("uses").toInt()==0)m_library.remove(id);
    saveLibrary();emit changed();return true;
}
void Soundboard::rememberUse(const QString &id){
    const auto current=find(id);if(current.isEmpty() || m_libraryAccount.isEmpty())return;
    auto row=m_library.value(id);for(const auto &key:{"id","guildId","name","emoji"})row.insert(key,current.value(key));
    row.insert("uses",qMin(100000,row.value("uses").toInt()+1));row.insert("lastUsed",QDateTime::currentMSecsSinceEpoch());m_library.insert(id,row);
    while(m_library.size()>200){auto oldest=m_library.end();for(auto it=m_library.begin();it!=m_library.end();++it)if(!it->value("favorite").toBool()&&(oldest==m_library.end()||it->value("lastUsed").toLongLong()<oldest->value("lastUsed").toLongLong()))oldest=it;if(oldest==m_library.end())break;m_library.erase(oldest);}
    saveLibrary();
}
bool Soundboard::activate(const QString &id,bool previewOnly){
    syncLibrary();m_pendingActivation.clear();if(!DiscordClient::validId(id))return false;
    if(!find(id).isEmpty())return previewOnly?preview(id):play(id);
    const auto row=m_library.value(id);if(row.isEmpty())return false;
    load(row.value("guildId").toString());if(!busy())return false;
    m_pendingActivation=id;m_activationPreview=previewOnly;m_activationVoiceChannel=m_voice?m_voice->channelId():QString{};m_activationVoiceGuild=m_voice?m_voice->currentGuildId():QString{};emit changed();return true;
}
void Soundboard::finishActivation(){
    const auto id=m_pendingActivation;const bool previewOnly=m_activationPreview;m_pendingActivation.clear();if(id.isEmpty())return;
    const auto current=find(id);if(current.isEmpty()||!current.value("available").toBool()){m_error="This sound is no longer available from its server.";return;}
    if(!previewOnly&&(!m_voice||m_voice->channelId()!=m_activationVoiceChannel||m_voice->currentGuildId()!=m_activationVoiceGuild)){m_error="The voice channel changed before this sound was ready.";return;}
    if(previewOnly)preview(id);else play(id);
}
