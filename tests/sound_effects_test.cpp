#include "SoundEffects.h"
#include "AppSettings.h"
#include <QAudioDevice>
#include <QBuffer>
#include <QFile>
#include <QPointer>
#include <QtTest>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>

namespace {
const QStringList names{"join","leave","mute","unmute","deafen","undeafen","user-join","user-leave","stream-start","stream-stop","watch-start","watch-stop","message"};
QByteArray monoPcm(const QString &name){
 QFile file(":/relay/sounds/"+name+".wav");if(!file.open(QIODevice::ReadOnly))return {};
 const auto wave=file.readAll();
 for(qsizetype at=12;at+8<=wave.size();){
  const auto size=qFromLittleEndian<quint32>(wave.constData()+at+4);
  if(size>quint32(wave.size()-at-8))return {};
  if(wave.mid(at,4)=="data")return wave.mid(at+8,size);
  at+=8+size+(size&1);
 }
 return {};
}
class PartialOutput final : public QIODevice {
public:
 QByteArray captured;qsizetype maximumWrite=17;qsizetype failAfter=-1;
 PartialOutput(){open(QIODevice::WriteOnly);}
protected:
 qint64 readData(char *,qint64)override{return -1;}
 qint64 writeData(const char *data,qint64 length)override{
  if(failAfter>=0 && captured.size()>=failAfter)return -1;
  const auto count=qMin<qint64>(length,maximumWrite);captured.append(data,count);return count;
 }
};
}

class SoundEffectsTest final:public QObject {
 Q_OBJECT
private slots:
 void everyCueWritesRealScaledStereoPcmToTheOutput(){
  QTemporaryDir directory;QVERIFY(directory.isValid());AppSettings prefs(directory.filePath("settings.ini"));SoundEffects effects(&prefs);
  QBuffer output;QVERIFY(output.open(QIODevice::WriteOnly));effects.m_fixtureMode=true;effects.m_fixtureOutput=&output;
  QSignalSpy played(&effects,&SoundEffects::effectTriggered);
  for(const auto &name:names){
   output.buffer().clear();output.seek(0);effects.play(name);
   QCOMPARE(played.last(),QList<QVariant>{name});QVERIFY(effects.error().isEmpty());QCOMPARE(effects.m_playbacks.size(),1);
   const auto mono=monoPcm(name),stereo=output.data();QVERIFY(!mono.isEmpty());QCOMPARE(stereo.size(),mono.size()*2);
   int peak=0;
   for(qsizetype at=0;at<mono.size();at+=2){
    const int expected=int(std::lround(qFromLittleEndian<qint16>(mono.constData()+at)*.5));
    const int left=qFromLittleEndian<qint16>(stereo.constData()+at*2),right=qFromLittleEndian<qint16>(stereo.constData()+at*2+2);
    QCOMPARE(left,right);QVERIFY(qAbs(left-expected)<=1);peak=qMax(peak,qAbs(left));
   }
   QVERIFY2(peak>4000,"Triggering a cue must deliver nonzero audible PCM, not just emit a signal");
   const auto playback=effects.m_playbacks.value(name);QVERIFY(playback.source);QVERIFY(!playback.sink);
   QCOMPARE(playback.source->data(),stereo);QVERIFY(playback.source->atEnd());
   effects.finishCue(name,playback.generation);QVERIFY(effects.m_playbacks.isEmpty());
  }
  QCOMPARE(played.count(),names.size());QCOMPARE(effects.m_started,quint64(names.size()));QCOMPARE(effects.m_completed,quint64(names.size()));
  QCOMPARE(effects.m_failures,quint64(0));QCOMPARE(effects.m_samples.size(),names.size());
 }
 void bundledCuesStayShortSoftSmoothAndDistinct(){
  QSet<QByteArray> waveforms;
  for(const auto &name:QStringList{"join","leave","mute","unmute","deafen","undeafen","user-join","user-leave","stream-start","stream-stop","watch-start","watch-stop","message"}){
   QFile file(":/relay/sounds/"+name+".wav");QVERIFY(file.open(QIODevice::ReadOnly));const auto wav=file.readAll();
   QCOMPARE(wav.first(4),QByteArray("RIFF"));QCOMPARE(wav.mid(8,4),QByteArray("WAVE"));
   QCOMPARE(qFromLittleEndian<quint16>(wav.constData()+20),quint16(1)); // PCM
   QCOMPARE(qFromLittleEndian<quint16>(wav.constData()+22),quint16(1));
   QCOMPARE(qFromLittleEndian<quint32>(wav.constData()+24),quint32(48000));
   QCOMPARE(qFromLittleEndian<quint16>(wav.constData()+34),quint16(16));
   QCOMPARE(wav.mid(36,4),QByteArray("data"));
   const auto pcm=wav.mid(44);const int samples=pcm.size()/2;QVERIFY(samples>=4800 && samples<=19200);
   int peak=0,previous=0,maximumStep=0;double square=0,startSquare=0,endSquare=0;
   constexpr int edgeSamples=48; // One millisecond at 48 kHz.
   for(int i=0;i<samples;++i){
    const int value=qFromLittleEndian<qint16>(pcm.constData()+2*i);
    peak=qMax(peak,qAbs(value));square+=double(value)*value;
    maximumStep=qMax(maximumStep,qAbs(value-previous));previous=value;
    if(i<edgeSamples)startSquare+=double(value)*value;
    if(i>=samples-edgeSamples)endSquare+=double(value)*value;
   }
   const double amplitude=peak/32768.;QVERIFY(amplitude>.25 && amplitude<.5);
   const double rms=std::sqrt(square/samples)/32768.;QVERIFY(rms>.06 && rms<.17);
   // Leave room for a soft transient followed by a decay, rather than the old
   // sustained alarm envelope. These bounds do not prescribe notes or timbre.
   QVERIFY(amplitude/rms>2.5);
   QVERIFY(maximumStep/32768.<.08);
   QVERIFY(std::sqrt(startSquare/edgeSamples)/32768.<.15*rms);
   QVERIFY(std::sqrt(endSquare/edgeSamples)/32768.<.15*rms);
   QCOMPARE(qFromLittleEndian<qint16>(pcm.constData()),qint16(0));
   QCOMPARE(qFromLittleEndian<qint16>(pcm.constData()+pcm.size()-2),qint16(0));
   QVERIFY(!waveforms.contains(pcm));waveforms.insert(pcm);
  }
 }
 void cueGainIsIndependentOfVoiceVolumeAndDoesNotAlterCachedSamples(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("settings.ini"));prefs.setSoundEffectsVolume(1);
  SoundEffects effects(&prefs);QBuffer output;QVERIFY(output.open(QIODevice::WriteOnly));effects.m_fixtureMode=true;effects.m_fixtureOutput=&output;
  effects.play("mute");QVERIFY(!output.data().isEmpty());const auto full=output.data();const auto cached=effects.m_samples.value("mute");QCOMPARE(full,cached);
  const auto oldGeneration=effects.m_playbacks.value("mute").generation;
  prefs.setOutputVolume(0);QVERIFY(effects.m_playbacks.contains("mute"));QCOMPARE(effects.m_playbacks.value("mute").generation,oldGeneration);
  prefs.setSoundEffectsVolume(.25);QVERIFY(effects.m_playbacks.isEmpty());
  effects.m_lastPlayed.remove("mute");output.buffer().clear();output.seek(0);effects.play("mute");const auto quiet=output.data();QCOMPARE(quiet.size(),full.size());
  int peak=0;
  for(qsizetype at=0;at<full.size();at+=2){
   const int expected=int(std::lround(qFromLittleEndian<qint16>(full.constData()+at)*.25));
   const int actual=qFromLittleEndian<qint16>(quiet.constData()+at);QVERIFY(qAbs(actual-expected)<=1);peak=qMax(peak,qAbs(actual));
  }
  QVERIFY(peak>2500);QCOMPARE(effects.m_samples.value("mute"),cached);QVERIFY(effects.error().isEmpty());
 }
 void categoriesMasterAndZeroVolumeStopActiveCuesAndBlockNewOutput(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("settings.ini"));SoundEffects effects(&prefs);
  QBuffer output;QVERIFY(output.open(QIODevice::WriteOnly));effects.m_fixtureMode=true;effects.m_fixtureOutput=&output;
  QSignalSpy played(&effects,&SoundEffects::effectTriggered);
  effects.play("user-join");effects.play("watch-start");effects.play("stream-start");effects.play("message");effects.play("mute");QCOMPARE(effects.m_playbacks.size(),5);
  prefs.setVoiceActivitySounds(false);QVERIFY(!effects.m_playbacks.contains("user-join"));QVERIFY(effects.m_playbacks.contains("mute"));
  prefs.setStreamActivitySounds(false);QVERIFY(!effects.m_playbacks.contains("watch-start"));QVERIFY(!effects.m_playbacks.contains("stream-start"));
  prefs.setNotificationSound(false);QVERIFY(!effects.m_playbacks.contains("message"));QCOMPARE(effects.m_playbacks.size(),1);
  const auto size=output.size();const int count=played.count();
  for(const auto &name:QStringList{"user-join","user-leave","stream-start","stream-stop","watch-start","watch-stop","message","../bad","unknown"})effects.play(name);
  QCOMPARE(played.count(),count);QCOMPARE(output.size(),size);
  prefs.setSoundEffectsVolume(0);QVERIFY(effects.m_playbacks.isEmpty());effects.play("unmute");QCOMPARE(played.count(),count);QCOMPARE(output.size(),size);
  prefs.setSoundEffectsVolume(.75);effects.play("unmute");QCOMPARE(played.count(),count+1);QVERIFY(effects.m_playbacks.contains("unmute"));
  prefs.setActivitySoundsEnabled(false);QVERIFY(effects.m_playbacks.isEmpty());const auto mutedSize=output.size();
  for(const auto &name:names)effects.play(name);QCOMPARE(played.count(),count+1);QCOMPARE(output.size(),mutedSize);
 }
 void rapidOppositeTogglesCancelOldBuffersAndPreserveTheFinalAction(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("settings.ini"));SoundEffects effects(&prefs);
  QBuffer output;QVERIFY(output.open(QIODevice::WriteOnly));effects.m_fixtureMode=true;effects.m_fixtureOutput=&output;
  QSignalSpy played(&effects,&SoundEffects::effectTriggered);
  for(const auto &pair:QList<QPair<QString,QString>>{{"mute","unmute"},{"deafen","undeafen"},{"join","leave"},{"stream-start","stream-stop"},{"watch-start","watch-stop"}}){
   effects.play(pair.first);const auto first=effects.m_playbacks.value(pair.first);QVERIFY(first.source);
   effects.play(pair.second);QVERIFY(!effects.m_playbacks.contains(pair.first));QVERIFY(effects.m_playbacks.contains(pair.second));
   effects.play(pair.first);QVERIFY(effects.m_playbacks.contains(pair.first));QVERIFY(!effects.m_playbacks.contains(pair.second));
   const auto final=effects.m_playbacks.value(pair.first);QVERIFY(final.generation!=first.generation);
   effects.finishCue(pair.first,first.generation,true);QCOMPARE(effects.m_playbacks.value(pair.first).generation,final.generation);QVERIFY(effects.error().isEmpty());
   const int count=played.count();const auto bytes=output.size();effects.play(pair.first);QCOMPARE(played.count(),count);QCOMPARE(output.size(),bytes);
   effects.finishCue(pair.first,final.generation);QVERIFY(!effects.m_playbacks.contains(pair.first));
  }
  QCOMPARE(played.count(),15);QCOMPARE(effects.m_canceled,quint64(10));QCOMPARE(effects.m_completed,quint64(5));
 }
 void partialWritesFailuresAndCompletionAllowRecoveryWithoutFalseTriggers(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("settings.ini"));SoundEffects effects(&prefs);
  PartialOutput output;effects.m_fixtureMode=true;effects.m_fixtureOutput=&output;QSignalSpy played(&effects,&SoundEffects::effectTriggered);
  effects.play("join");QCOMPARE(played.count(),1);QCOMPARE(output.captured,effects.m_playbacks.value("join").source->data());QVERIFY(output.captured.size()>1000);
  const auto completed=effects.m_playbacks.value("join").generation;effects.finishCue("join",completed);effects.finishCue("join",completed,true);
  QCOMPARE(effects.m_completed,quint64(1));QCOMPARE(effects.m_failures,quint64(0));QVERIFY(effects.error().isEmpty());
  output.failAfter=output.captured.size()+17;effects.play("mute");QCOMPARE(played.count(),1);QVERIFY(!effects.m_playbacks.contains("mute"));QCOMPARE(effects.m_failures,quint64(1));QVERIFY(!effects.error().isEmpty());
  output.failAfter=-1;effects.m_lastPlayed.remove("mute");effects.play("mute");QCOMPARE(played.count(),2);QVERIFY(effects.error().isEmpty());
  effects.finishCue("mute",effects.m_playbacks.value("mute").generation,true);QCOMPARE(effects.m_failures,quint64(2));QVERIFY(!effects.error().isEmpty());
  effects.m_lastPlayed.remove("mute");effects.play("mute");QCOMPARE(played.count(),3);QVERIFY(effects.error().isEmpty());
  effects.stopAll();QVERIFY(effects.m_playbacks.isEmpty());output.close();effects.play("deafen");QCOMPARE(played.count(),3);QVERIFY(effects.m_playbacks.isEmpty());QCOMPARE(effects.m_failures,quint64(3));
 }
 void overlapsStayBoundedAndDeferredCleanupCannotStopNewPlayback(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("settings.ini"));SoundEffects effects(&prefs);
  QBuffer output;QVERIFY(output.open(QIODevice::WriteOnly));effects.m_fixtureMode=true;effects.m_fixtureOutput=&output;
  const QStringList compatible{"mute","deafen","join","stream-start","watch-start","message","user-join","user-leave"};
  for(const auto &name:compatible){effects.play(name);QVERIFY(effects.m_playbacks.size()<=8);}QCOMPARE(effects.m_playbacks.size(),8);
  const auto original=effects.m_playbacks.value("message");effects.m_lastPlayed.remove("message");effects.play("message");
  QCOMPARE(effects.m_playbacks.size(),8);const auto current=effects.m_playbacks.value("message");QVERIFY(current.generation>original.generation);
  effects.finishCue("message",original.generation);QCOMPARE(effects.m_playbacks.value("message").generation,current.generation);
  QPointer<QBuffer> oldBuffer=original.source;QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QVERIFY(oldBuffer.isNull());QVERIFY(current.source);
  effects.stopAll();QVERIFY(effects.m_playbacks.isEmpty());QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
 }
 void unavailableSavedOutputFallsBackAndChangingSettingsNeverStartsAudio(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("settings.ini"));prefs.setOutputDeviceId("fixture-output-not-present");
  SoundEffects effects(&prefs);QBuffer output;QVERIFY(output.open(QIODevice::WriteOnly));effects.m_fixtureMode=true;effects.m_fixtureOutput=&output;
  const auto outputs=QMediaDevices::audioOutputs();const auto expected=QMediaDevices::defaultAudioOutput().isNull() && !outputs.isEmpty()?outputs.first():QMediaDevices::defaultAudioOutput();
  QCOMPARE(effects.m_output.id(),expected.id());QVERIFY(effects.m_playbacks.isEmpty());QCOMPARE(effects.m_started,quint64(0));
  if(!outputs.isEmpty()){
   prefs.setOutputDeviceId(QString::fromLatin1(outputs.last().id().toBase64()));QCOMPARE(effects.m_output.id(),outputs.last().id());QVERIFY(output.data().isEmpty());
  }
  effects.play("mute");QVERIFY(!output.data().isEmpty());QVERIFY(effects.error().isEmpty());
  const auto summary=effects.diagnosticSummary();QVERIFY(summary.contains("started"));QVERIFY(!summary.contains("fixture-output"));
 }
 void disablingSoundsFromATriggerCallbackCancelsTheJustStartedCue(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("settings.ini"));SoundEffects effects(&prefs);
  QBuffer output;QVERIFY(output.open(QIODevice::WriteOnly));effects.m_fixtureMode=true;effects.m_fixtureOutput=&output;
  connect(&effects,&SoundEffects::effectTriggered,&prefs,[&]{prefs.setActivitySoundsEnabled(false);});
  effects.play("mute");QVERIFY(!output.data().isEmpty());QVERIFY(effects.m_playbacks.isEmpty());QCOMPARE(effects.m_started,quint64(1));QCOMPARE(effects.m_canceled,quint64(1));
  const auto size=output.size();effects.play("unmute");QCOMPARE(output.size(),size);
 }
 void individualCueTogglesStopOnlyThatCueAndBlockItsPcm(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("settings.ini"));SoundEffects effects(&prefs);
  QBuffer output;QVERIFY(output.open(QIODevice::WriteOnly));effects.m_fixtureMode=true;effects.m_fixtureOutput=&output;
  QSignalSpy played(&effects,&SoundEffects::effectTriggered);
  for(const auto &name:names){
   effects.stopAll();effects.m_lastPlayed.clear();effects.play(name);QVERIFY(effects.m_playbacks.contains(name));
   prefs.setSoundEnabled(name,false);QVERIFY(!effects.m_playbacks.contains(name));
   const auto count=played.count();const auto bytes=output.size();effects.m_lastPlayed.remove(name);effects.play(name);
   QCOMPARE(played.count(),count);QCOMPARE(output.size(),bytes);QVERIFY(effects.error().isEmpty());
   prefs.setSoundEnabled(name,true);effects.play(name);QCOMPARE(played.count(),count+1);QVERIFY(effects.m_playbacks.contains(name));
  }
  effects.stopAll();effects.m_lastPlayed.clear();effects.play("mute");effects.play("deafen");
  prefs.setSoundEnabled("mute",false);QVERIFY(!effects.m_playbacks.contains("mute"));QVERIFY(effects.m_playbacks.contains("deafen"));
 }
 void explicitPreviewPlaysDisabledCuesWithoutChangingAnySavedEnablement(){
  QTemporaryDir directory;const auto file=directory.filePath("settings.ini");AppSettings prefs(file);
  prefs.setActivitySoundsEnabled(false);prefs.setVoiceActivitySounds(false);prefs.setStreamActivitySounds(false);prefs.setNotificationSound(false);
  for(const auto &name:names)prefs.setSoundEnabled(name,false);
  QFile saved(file);QVERIFY(saved.open(QIODevice::ReadOnly));const auto original=saved.readAll();saved.close();
  SoundEffects effects(&prefs);QBuffer output;QVERIFY(output.open(QIODevice::WriteOnly));effects.m_fixtureMode=true;effects.m_fixtureOutput=&output;
  QSignalSpy played(&effects,&SoundEffects::effectTriggered);
  for(const auto &name:names){
   const auto bytes=output.size();effects.preview(name);QVERIFY(output.size()>bytes);QVERIFY(effects.m_playbacks.contains(name));
   QCOMPARE(played.last(),QList<QVariant>{name});effects.finishCue(name,effects.m_playbacks.value(name).generation);
   const auto previewBytes=output.size();effects.m_lastPlayed.remove(name);effects.play(name);QCOMPARE(output.size(),previewBytes);QVERIFY(!effects.m_playbacks.contains(name));
   QVERIFY(!prefs.soundEnabled(name));
  }
  QCOMPARE(played.count(),names.size());QVERIFY(!prefs.activitySoundsEnabled());QVERIFY(!prefs.voiceActivitySounds());QVERIFY(!prefs.streamActivitySounds());QVERIFY(!prefs.notificationSound());
  QVERIFY(saved.open(QIODevice::ReadOnly));QCOMPARE(saved.readAll(),original);
  const auto bytes=output.size();for(const auto &name:QStringList{"unknown","../mute",""})effects.preview(name);QCOMPARE(output.size(),bytes);QCOMPARE(played.count(),names.size());
  prefs.setSoundEffectsVolume(0);effects.m_lastPlayed.clear();effects.preview("mute");QCOMPARE(output.size(),bytes);QCOMPARE(played.count(),names.size());
 }
};
QTEST_GUILESS_MAIN(SoundEffectsTest)
#include "sound_effects_test.moc"
