#include "addons/AddonManager.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QtEndian>
#include <cmath>
class CustomSoundboardTest final:public QObject {
 Q_OBJECT
private slots:
 void importsPersistsDecodesAndStopsOnDisableWithoutARealVoiceConnection(){
  QTemporaryDir directory;const auto previous=qgetenv("XDG_DATA_HOME");qputenv("XDG_DATA_HOME",directory.path().toUtf8());
  const auto path=directory.filePath("tone.wav");QByteArray wav(44+19200,'\0');memcpy(wav.data(),"RIFF",4);qToLittleEndian<quint32>(wav.size()-8,wav.data()+4);memcpy(wav.data()+8,"WAVEfmt ",8);qToLittleEndian<quint32>(16,wav.data()+16);qToLittleEndian<quint16>(1,wav.data()+20);qToLittleEndian<quint16>(2,wav.data()+22);qToLittleEndian<quint32>(48000,wav.data()+24);qToLittleEndian<quint32>(192000,wav.data()+28);qToLittleEndian<quint16>(4,wav.data()+32);qToLittleEndian<quint16>(16,wav.data()+34);memcpy(wav.data()+36,"data",4);qToLittleEndian<quint32>(19200,wav.data()+40);
  for(int i=0;i<9600;++i)qToLittleEndian<qint16>(qint16(10000*std::sin(i/2*2*3.141592653589793*440/48000)),wav.data()+44+i*2);
  QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write(wav);file.close();
  AddonManager manager({QFileInfo(QStringLiteral(RELAY_SOUNDBOARD_ADDON_PATH)).absolutePath()}, {}, directory.filePath("addons.ini"));
  const QString id="org.relay.soundboard";QVERIFY(!manager.enabled(id));QVERIFY(manager.setEnabled(id,true));QVERIFY(manager.voiceAudioAllowed(id));auto *service=manager.service(id);QVERIFY(service);
  bool added=false;QVERIFY(QMetaObject::invokeMethod(service,"addSound",Q_RETURN_ARG(bool,added),Q_ARG(QUrl,QUrl::fromLocalFile(path))));QVERIFY(added);
  QVERIFY(QMetaObject::invokeMethod(service,"prepareSound",Q_ARG(QUrl,QUrl::fromLocalFile(path))));
  QTRY_VERIFY_WITH_TIMEOUT(!service->property("loading").toBool(),5000);
  QCOMPARE(service->property("draftDuration").toDouble(),.1);
  QCOMPARE(service->property("sounds").toList().size(),1);
  bool saved=false;
  QVERIFY(QMetaObject::invokeMethod(service,"saveDraft",Q_RETURN_ARG(bool,saved),Q_ARG(QString,QString("Trimmed tone")),Q_ARG(double,.02),Q_ARG(double,.08)));QVERIFY(saved);
  auto trimmed=service->property("sounds").toList().last().toMap();
  service->setProperty("voiceAvailable",true);QSignalSpy trimClips(service,SIGNAL(voiceClipRequested(QByteArray)));
  QVERIFY(QMetaObject::invokeMethod(service,"play",Q_ARG(QString,trimmed.value("id").toString())));
  QTRY_COMPARE_WITH_TIMEOUT(trimClips.size(),1,5000);QCOMPARE(trimClips[0][0].toByteArray().size(),11520);
  QVERIFY(QMetaObject::invokeMethod(service,"removeSound",Q_ARG(QString,trimmed.value("id").toString())));service->setProperty("voiceAvailable",false);
  auto rows=service->property("sounds").toList();QCOMPARE(rows.size(),1);const auto soundId=rows[0].toMap().value("id").toString();
  QSignalSpy clips(service,SIGNAL(voiceClipRequested(QByteArray)));QSignalSpy stopped(service,SIGNAL(stopVoiceClip()));
  QVERIFY(QMetaObject::invokeMethod(service,"play",Q_ARG(QString,soundId)));QCOMPARE(clips.size(),0);QVERIFY(!service->property("error").toString().isEmpty());
  service->setProperty("voiceAvailable",true);QVERIFY(QMetaObject::invokeMethod(service,"play",Q_ARG(QString,soundId)));
  QTRY_VERIFY_WITH_TIMEOUT(!clips.isEmpty() || (!service->property("loading").toBool() && !service->property("error").toString().isEmpty()),5000);
  QVERIFY2(!clips.isEmpty(),qPrintable(service->property("error").toString()));QCOMPARE(clips[0][0].toByteArray().size(),19200);
  QVERIFY(manager.setEnabled(id,false));QVERIFY(stopped.size()>0);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
  QVERIFY(manager.setEnabled(id,true));service=manager.service(id);QVERIFY(service);QCOMPARE(service->property("sounds").toList().size(),1);
  QVERIFY(QMetaObject::invokeMethod(service,"removeSound",Q_ARG(QString,soundId)));QVERIFY(service->property("sounds").toList().isEmpty());
  manager.shutdown();if(previous.isNull())qunsetenv("XDG_DATA_HOME");else qputenv("XDG_DATA_HOME",previous);
 }
};
QTEST_GUILESS_MAIN(CustomSoundboardTest)
#include "custom_soundboard_test.moc"
