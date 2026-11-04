#include "SoundEffects.h"
#include "AppSettings.h"
#include <QAudioSink>
#include <QAudioFormat>
#include <QBuffer>
#include <QFile>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
namespace {
const QStringList &cueNames() {
    static const QStringList names{"mute","unmute","deafen","undeafen","join","leave","user-join","user-leave","stream-start","stream-stop","watch-start","watch-stop","message"};
    return names;
}
QAudioFormat cueFormat() {
    QAudioFormat format;format.setSampleRate(48000);format.setChannelCount(2);format.setSampleFormat(QAudioFormat::Int16);return format;
}
QByteArray applyGain(const QByteArray &pcm,double gain) {
    QByteArray scaled(pcm.size(),Qt::Uninitialized);
    for(qsizetype at=0;at+2<=pcm.size();at+=2)
        qToLittleEndian<qint16>(qint16(std::lround(qFromLittleEndian<qint16>(pcm.constData()+at)*gain)),scaled.data()+at);
    return scaled;
}
}
SoundEffects::SoundEffects(AppSettings *settings,QObject *parent):QObject(parent),m_settings(settings) {
    m_clock.start();m_outputId=settings->outputDeviceId();m_volume=settings->soundEffectsVolume();configureOutput();
    connect(settings,&AppSettings::changed,this,[this]{
        if(m_outputId!=m_settings->outputDeviceId()){m_outputId=m_settings->outputDeviceId();configureOutput();}
        if(m_volume!=m_settings->soundEffectsVolume()){m_volume=m_settings->soundEffectsVolume();stopAll();}
        for(const auto &name:m_playbacks.keys())if(!allowed(name))stopCue(name);
    });
    connect(&m_devices,&QMediaDevices::audioOutputsChanged,this,&SoundEffects::configureOutput);
}
SoundEffects::~SoundEffects(){stopAll();}
bool SoundEffects::allowed(const QString &name)const {
    if (m_previewRequest) return cueNames().contains(name) && m_settings->soundEffectsVolume()>0;
    return cueNames().contains(name) && m_settings->soundEnabled(name) && m_settings->activitySoundsEnabled() && m_settings->soundEffectsVolume()>0 &&
        (!name.startsWith("user-") || m_settings->voiceActivitySounds()) &&
        (!(name.startsWith("stream-") || name.startsWith("watch-")) || m_settings->streamActivitySounds()) &&
        (name!="message" || m_settings->notificationSound());
}
void SoundEffects::setError(const QString &error){if(m_error!=error){m_error=error;emit changed();}}
void SoundEffects::configureOutput(){
    auto output=QMediaDevices::defaultAudioOutput();
    for(const auto &device:QMediaDevices::audioOutputs())if(QString::fromLatin1(device.id().toBase64())==m_outputId){output=device;break;}
    if(output.isNull() && !QMediaDevices::audioOutputs().isEmpty())output=QMediaDevices::audioOutputs().first();
    if(m_output!=output){stopAll();m_output=output;}
    // Loading resources and settings never starts an audio stream.
}
QByteArray SoundEffects::sample(const QString &name){
    if(m_samples.contains(name))return m_samples.value(name);
    QFile file(":/relay/sounds/"+name+".wav");
    if(!file.open(QIODevice::ReadOnly))return {};
    const auto wave=file.readAll();
    if(wave.size()<44 || wave.size()>96044 || wave.first(4)!="RIFF" || wave.mid(8,4)!="WAVE")return {};
    QByteArray mono;bool validFormat=false;
    for(qsizetype at=12;at+8<=wave.size();){
        const quint32 length=qFromLittleEndian<quint32>(wave.constData()+at+4);
        if(length>quint32(wave.size()-at-8))return {};
        const auto chunk=wave.mid(at,4);const auto *data=wave.constData()+at+8;
        if(chunk=="fmt ")validFormat=length>=16 && qFromLittleEndian<quint16>(data)==1 && qFromLittleEndian<quint16>(data+2)==1
            && qFromLittleEndian<quint32>(data+4)==48000 && qFromLittleEndian<quint16>(data+12)==2 && qFromLittleEndian<quint16>(data+14)==16;
        else if(chunk=="data")mono=wave.mid(at+8,length);
        at+=8+length+(length&1);
    }
    if(!validFormat || mono.isEmpty() || mono.size()%2 || mono.size()>48000)return {};
    QByteArray stereo(mono.size()*2,Qt::Uninitialized);
    for(qsizetype at=0;at<mono.size();at+=2){
        stereo[at*2]=stereo[at*2+2]=mono[at];stereo[at*2+1]=stereo[at*2+3]=mono[at+1];
    }
    m_samples.insert(name,stereo);return stereo;
}
void SoundEffects::stopCue(const QString &name){
    const auto found=m_playbacks.find(name);if(found==m_playbacks.end())return;
    const auto playback=*found;m_playbacks.erase(found);
    if(playback.source)m_pcmBytes+=std::max<qint64>(0,playback.source->pos());
    if(playback.sink){playback.sink->reset();playback.sink->deleteLater();}
    if(playback.source)playback.source->deleteLater();
    ++m_canceled;emit diagnosticsChanged();
}
void SoundEffects::stopAll(){for(const auto &name:m_playbacks.keys())stopCue(name);}
void SoundEffects::finishCue(const QString &name,quint64 generation,bool failed){
    const auto found=m_playbacks.find(name);if(found==m_playbacks.end() || found->generation!=generation)return;
    const auto playback=*found;m_playbacks.erase(found);
    if(playback.source)m_pcmBytes+=std::max<qint64>(0,playback.source->pos());
    if(playback.sink){playback.sink->reset();playback.sink->deleteLater();}
    if(playback.source)playback.source->deleteLater();
    if(failed){++m_failures;setError("Sound playback stopped. Check Relay's selected output and try the sound again.");}
    else ++m_completed;
    emit diagnosticsChanged();
}
void SoundEffects::cancelOpposite(const QString &name){
    static const QHash<QString,QString> opposites{{"mute","unmute"},{"unmute","mute"},{"deafen","undeafen"},{"undeafen","deafen"},
        {"join","leave"},{"leave","join"},{"stream-start","stream-stop"},{"stream-stop","stream-start"},
        {"watch-start","watch-stop"},{"watch-stop","watch-start"}};
    const auto opposite=opposites.value(name);if(opposite.isEmpty())return;
    m_lastPlayed.remove(opposite);stopCue(opposite);
}
void SoundEffects::preview(const QString &name) {
    m_previewRequest=true; play(name); m_previewRequest=false;
}
void SoundEffects::play(const QString &name){
    ++m_requested;
    if(!allowed(name)){++m_blocked;emit diagnosticsChanged();return;}
    const auto now=m_clock.elapsed();
    if(m_lastPlayed.contains(name) && now-m_lastPlayed.value(name)<100){++m_blocked;emit diagnosticsChanged();return;}
    cancelOpposite(name);stopCue(name);m_lastPlayed.insert(name,now);
    const auto pcm=sample(name);
    if(pcm.isEmpty()){++m_failures;setError("A bundled sound could not load.");emit diagnosticsChanged();return;}
    if(!m_fixtureMode && (m_output.isNull() || !m_output.isFormatSupported(cueFormat()))){
        ++m_failures;setError("Select an output that supports 48 kHz stereo audio for sound effects.");emit diagnosticsChanged();return;
    }
    // Bound simultaneous events while allowing e.g. a voice join and stream cue.
    if(m_playbacks.size()>=8){
        const auto oldest=std::min_element(m_playbacks.cbegin(),m_playbacks.cend(),[](const auto &a,const auto &b){return a.generation<b.generation;});
        stopCue(oldest.key());
    }
    const quint64 generation=++m_generation;
    auto *source=new QBuffer(this);source->setData(applyGain(pcm,m_settings->soundEffectsVolume()));source->open(QIODevice::ReadOnly);
    m_playbacks.insert(name,Playback{nullptr,source,generation});
    if(m_fixtureMode){
        if(!m_fixtureOutput || !m_fixtureOutput->isWritable()){finishCue(name,generation,true);return;}
        const auto data=source->readAll();qsizetype written=0;
        while(written<data.size()){
            const auto count=m_fixtureOutput->write(data.constData()+written,data.size()-written);
            if(count<=0){finishCue(name,generation,true);return;}written+=count;
        }
    }else{
        // QSoundEffect uses the OS notification/event role, which can be muted
        // independently of Relay. Plain QAudioSink matches the working voice
        // output; Relay owns cue gain and the selected device.
        auto *sink=new QAudioSink(m_output,cueFormat(),this);m_playbacks[name].sink=sink;
        sink->setBufferSize(3840*3);sink->setVolume(1);
        connect(sink,&QAudioSink::stateChanged,this,[this,name,generation,sink](QAudio::State state){
            if(state==QAudio::IdleState || (state==QAudio::StoppedState && sink->error()!=QAudio::NoError)){
                const bool failed=sink->error()!=QAudio::NoError;
                QTimer::singleShot(0,this,[this,name,generation,failed]{finishCue(name,generation,failed);});
            }
        });
        sink->start(source);
        if(sink->error()!=QAudio::NoError || sink->state()==QAudio::StoppedState){finishCue(name,generation,true);return;}
    }
    setError({});++m_started;emit effectTriggered(name);emit diagnosticsChanged();
}
QString SoundEffects::diagnosticSummary()const{
    return QString("requested/started/completed/canceled/blocked/errors %1/%2/%3/%4/%5/%6 · active %7 · enabled %8 · volume %9 · output available %10 · consumed PCM bytes %11")
        .arg(m_requested).arg(m_started).arg(m_completed).arg(m_canceled).arg(m_blocked).arg(m_failures).arg(m_playbacks.size())
        .arg(m_settings->activitySoundsEnabled()).arg(m_settings->soundEffectsVolume()).arg(!m_output.isNull()).arg(m_pcmBytes);
}
