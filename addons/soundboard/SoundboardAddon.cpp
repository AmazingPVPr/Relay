#include "addons/AddonInterface.h"
#include <QAudioDecoder>
#include <QAudioBuffer>
#include <QAudioOutput>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QPointer>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
class CustomSoundboard final:public QObject {
    Q_OBJECT
    Q_PROPERTY(double draftDuration READ draftDuration NOTIFY changed)
    Q_PROPERTY(QVariantList sounds READ sounds NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool voiceAvailable READ voiceAvailable WRITE setVoiceAvailable NOTIFY changed)
    Q_PROPERTY(QString outputDeviceId READ outputDeviceId WRITE setOutputDeviceId NOTIFY changed)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY changed)
public:
    explicit CustomSoundboard(QObject *owner):QObject(owner){
        directory=QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)+"/custom-soundboard";
        QDir().mkpath(directory);QFile file(directory+"/index.json");
        if(file.open(QIODevice::ReadOnly) && file.size()<128*1024){const auto object=QJsonDocument::fromJson(file.readAll()).object();gain=qBound(0.,object.value("volume").toDouble(.5),1.);
            for(const auto &entry:object.value("sounds").toArray()){if(rows.size()>=200)break;const auto row=entry.toObject();const auto id=row.value("id").toString(),filename=row.value("file").toString();
                if(QUuid(id).isNull() || !filename.startsWith(id+'.') || filename.contains('/') || filename.contains('\\') || !QFileInfo::exists(directory+'/'+filename))continue;
                rows.append(QVariantMap{{"id",id},{"name",row.value("name").toString().left(80)},{"file",filename}});
            }}
        player.setAudioOutput(&output);output.setVolume(float(gain));
        connect(&player,&QMediaPlayer::errorOccurred,this,[this]{problem="This sound could not be previewed.";emit changed();});
    }
    QVariantList sounds()const{return rows;}QString error()const{return problem;}bool loading()const{return decoder;}
    bool voiceAvailable()const{return ready;}void setVoiceAvailable(bool value){if(ready==value)return;ready=value;if(!ready)stop();emit changed();}
    QString outputDeviceId()const{return outputId;}
    void setOutputDeviceId(const QString &value){outputId=value;auto device=QMediaDevices::defaultAudioOutput();for(const auto &candidate:QMediaDevices::audioOutputs())if(QString::fromLatin1(candidate.id().toBase64())==value)device=candidate;output.setDevice(device);}
    double volume()const{return gain;}void setVolume(double value){if(!std::isfinite(value))return;gain=qBound(0.,value,1.);output.setVolume(float(gain));save();emit changed();}
    double draftDuration() const { return draft.size()/192000.; }
    Q_INVOKABLE void cancelDraft(){stop();draft.clear();QFile::remove(directory+"/draft.wav");emit changed();}
    Q_INVOKABLE void prepareSound(const QUrl &url){
        cancelDraft();problem.clear();
        const QFileInfo source(url.toLocalFile());
        if(!url.isLocalFile() || !source.isFile() || source.size()<=0 || source.size()>16*1024*1024){problem="Choose an audio file up to 16 MB.";emit changed();return;}
        auto *current=new QAudioDecoder(this);decoder=current;
        QAudioFormat format;format.setSampleRate(48000);format.setChannelCount(2);format.setSampleFormat(QAudioFormat::Int16);current->setAudioFormat(format);
        connect(current,&QAudioDecoder::bufferReady,this,[this,current,format]{if(decoder!=current)return;auto buffer=current->read();if(!buffer.isValid())return;
            if(buffer.format()!=format || pcm.size()+buffer.byteCount()>57600000){stopDecode();problem="Choose audio up to five minutes, then trim it to 30 seconds.";emit changed();return;}pcm.append(buffer.constData<char>(),buffer.byteCount());});
        connect(current,&QAudioDecoder::finished,this,[this,current]{if(decoder!=current)return;draft=pcm;stopDecode();if(draft.isEmpty())problem="This file contains no audio.";emit changed();});
        connect(current,qOverload<QAudioDecoder::Error>(&QAudioDecoder::error),this,[this,current](QAudioDecoder::Error){if(decoder!=current)return;stopDecode();problem="This audio file could not be decoded.";emit changed();});
        current->setSource(url);current->start();emit changed();
    }
    Q_INVOKABLE void previewDraft(double start,double end){
        const auto audio=selection(start,end);if(audio.isEmpty())return;
        player.stop();const auto file=directory+"/draft.wav";if(!writeWave(file,audio))return;
        player.setSource(QUrl::fromLocalFile(file));player.play();
    }
    Q_INVOKABLE bool saveDraft(const QString &name,double start,double end){
        problem.clear();const auto audio=selection(start,end);
        if(name.trimmed().isEmpty() || rows.size()>=200 || audio.isEmpty()){problem="Enter a name and select between 0.05 and 30 seconds of audio (up to 200 sounds).";emit changed();return false;}
        const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces),filename=id+".wav";
        if(!writeWave(directory+'/'+filename,audio)){problem="The sound could not be saved.";emit changed();return false;}
        rows.append(QVariantMap{{"id",id},{"name",name.trimmed().left(80)},{"file",filename}});
        if(!save()){rows.removeLast();QFile::remove(directory+'/'+filename);emit changed();return false;}
        cancelDraft();return true;
    }
    Q_INVOKABLE bool addSound(const QUrl &url){
        const QFileInfo source(url.toLocalFile());const auto extension=source.suffix().toLower();
        if(!url.isLocalFile() || !source.isFile() || source.size()<=0 || source.size()>16*1024*1024 || !QStringList{"wav","mp3","ogg","flac","m4a","opus"}.contains(extension) || rows.size()>=200){problem="Choose an audio file up to 16 MB. A board can hold 200 sounds.";emit changed();return false;}
        const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces),filename=id+'.'+extension;
        if(!QFile::copy(source.absoluteFilePath(),directory+'/'+filename)){problem="The sound could not be added.";emit changed();return false;}
        rows.append(QVariantMap{{"id",id},{"name",source.completeBaseName().left(80)},{"file",filename}});problem.clear();save();emit changed();return true;
    }
    Q_INVOKABLE void removeSound(const QString &id){for(int i=0;i<rows.size();++i)if(rows[i].toMap().value("id").toString()==id){stop();QFile::remove(path(id));rows.removeAt(i);save();emit changed();return;}}
    Q_INVOKABLE void renameSound(const QString &id,const QString &name){if(name.trimmed().isEmpty())return;for(int i=0;i<rows.size();++i){auto row=rows[i].toMap();if(row.value("id").toString()==id){row["name"]=name.trimmed().left(80);rows[i]=row;save();emit changed();return;}}}
    Q_INVOKABLE void preview(const QString &id){const auto file=path(id);if(file.isEmpty())return;problem.clear();player.setSource(QUrl::fromLocalFile(file));player.play();emit changed();}
    Q_INVOKABLE void play(const QString &id){
        if(!ready){problem="Join voice and unmute your microphone to play custom sounds.";emit changed();return;}
        const auto file=path(id);if(file.isEmpty())return;stop();problem.clear();pcm.clear();
        auto *current=new QAudioDecoder(this);decoder=current;QAudioFormat format;format.setSampleRate(48000);format.setChannelCount(2);format.setSampleFormat(QAudioFormat::Int16);current->setAudioFormat(format);
        connect(current,&QAudioDecoder::bufferReady,this,[this,current,format]{if(decoder!=current)return;const auto buffer=current->read();
            if(!buffer.isValid())return;
            if(buffer.format()!=format || pcm.size()+buffer.byteCount()>5760000){stopDecode();problem="Use a sound up to 30 seconds that can decode to stereo audio.";emit changed();return;}
            pcm.append(buffer.constData<char>(),buffer.byteCount());
        });
        connect(current,&QAudioDecoder::finished,this,[this,current]{if(decoder!=current)return;auto audio=std::move(pcm);stopDecode();
            if(!ready || audio.isEmpty()){problem="The sound could not be played in voice.";emit changed();return;}
            for(qsizetype i=0;i+1<audio.size();i+=2){qint16 sample;std::memcpy(&sample,audio.constData()+i,2);sample=qint16(std::lround(sample*gain));std::memcpy(audio.data()+i,&sample,2);}
            emit voiceClipRequested(audio);emit changed();
        });
        connect(current,qOverload<QAudioDecoder::Error>(&QAudioDecoder::error),this,[this,current](QAudioDecoder::Error){if(decoder!=current)return;stopDecode();problem="The audio file could not be decoded.";emit changed();});
        current->setSource(QUrl::fromLocalFile(file));current->start();emit changed();
    }
    Q_INVOKABLE void stop(){stopDecode();player.stop();player.setSource({});emit stopVoiceClip();emit changed();}
signals:
    void changed();void voiceClipRequested(const QByteArray &pcm);void stopVoiceClip();
private:
    QString path(const QString &id)const{for(const auto &entry:rows){const auto row=entry.toMap();if(row.value("id").toString()==id)return directory+'/'+row.value("file").toString();}return {};}
    void stopDecode(){if(decoder){auto *current=decoder.data();decoder=nullptr;disconnect(current,nullptr,this,nullptr);current->stop();current->deleteLater();}pcm.clear();}
    QByteArray selection(double start,double end) const {
        if(!std::isfinite(start)||!std::isfinite(end)||start<0||end>draftDuration()+.0001||end-start<.05||end-start>30.0001)return {};
        const auto first=qint64(std::llround(start*48000))*4,last=qMin(qint64(draft.size()),qint64(std::llround(end*48000))*4);
        return draft.mid(first,last-first);
    }
    bool writeWave(const QString &path,const QByteArray &audio){
        QByteArray header(44,'\0');memcpy(header.data(),"RIFF",4);qToLittleEndian<quint32>(audio.size()+36,header.data()+4);memcpy(header.data()+8,"WAVEfmt ",8);
        qToLittleEndian<quint32>(16,header.data()+16);qToLittleEndian<quint16>(1,header.data()+20);qToLittleEndian<quint16>(2,header.data()+22);
        qToLittleEndian<quint32>(48000,header.data()+24);qToLittleEndian<quint32>(192000,header.data()+28);qToLittleEndian<quint16>(4,header.data()+32);qToLittleEndian<quint16>(16,header.data()+34);
        memcpy(header.data()+36,"data",4);qToLittleEndian<quint32>(audio.size(),header.data()+40);
        QSaveFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(header)==header.size()&&file.write(audio)==audio.size()&&file.commit();
    }
    bool save(){QSaveFile file(directory+"/index.json");const auto data=QJsonDocument(QJsonObject{{"volume",gain},{"sounds",QJsonArray::fromVariantList(rows)}}).toJson();if(!file.open(QIODevice::WriteOnly)||file.write(data)!=data.size()||!file.commit()){problem="Soundboard changes could not be saved.";return false;}return true;}
    QString directory,problem,outputId;QVariantList rows;double gain=.5;bool ready=false;QMediaPlayer player;QAudioOutput output;QPointer<QAudioDecoder> decoder;QByteArray pcm,draft;
};
class SoundboardAddon final:public QObject,public RelayAddonInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID RelayAddonInterface_iid FILE "metadata.json")
    Q_INTERFACES(RelayAddonInterface)
public:
    QString id()const override{return "org.relay.soundboard";}
    QObject *createService(QObject *owner)override{return new CustomSoundboard(owner);}
    void stopService(QObject *service)override{if(auto *board=qobject_cast<CustomSoundboard*>(service))board->stop();}
    QUrl settingsUrl()const override{return QUrl("qrc:/relay/addons/org.relay.soundboard/Settings.qml");}
};
#include "SoundboardAddon.moc"
