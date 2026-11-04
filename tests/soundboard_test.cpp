#include "Soundboard.h"
#include "DiscordClient.h"
#include "VoiceEngine.h"
#include "GuildManagement.h"
#include "AppSettings.h"
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QUdpSocket>
#include <QJsonDocument>
#include <QtEndian>
#include <QAudioBufferOutput>
#include <QProcess>
#include <QFile>
#include <QStandardPaths>
#include <cmath>
namespace {
class RelationshipFixture final : public QTcpServer {
public:
    struct Response { int status = 204; QByteArray body; int delay = 0; };
    QQueue<Response> responses;
    QList<QByteArray> requests;
    RelationshipFixture() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    const auto bytes = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", bytes);
                    const int end = bytes.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    int length = 0;
                    for (const auto &header : bytes.left(end).split('\n'))
                        if (header.toLower().startsWith("content-length:")) length = header.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    requests.append(bytes);
                    const auto response = responses.isEmpty() ? Response{} : responses.dequeue();
                    QTimer::singleShot(response.delay, socket, [socket, response] {
                        if (socket->state() != QAbstractSocket::ConnectedState) return;
                        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " Fixture\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(response.body.size()) + "\r\n\r\n" + response.body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
    QString base() const { return "http://127.0.0.1:" + QString::number(serverPort()); }
};
class LocalVoiceFixture final : public QObject {
public:
 QWebSocketServer gateway{"received soundboard fixture",QWebSocketServer::NonSecureMode};QUdpSocket udp;
 bool start(){
  if(!gateway.listen(QHostAddress::LocalHost) || !udp.bind(QHostAddress::LocalHost,0))return false;
  connect(&udp,&QUdpSocket::readyRead,this,[this]{while(udp.hasPendingDatagrams()){
   QByteArray packet(2048,Qt::Uninitialized);QHostAddress address;quint16 port;const auto size=udp.readDatagram(packet.data(),packet.size(),&address,&port);if(size!=74)continue;
   QByteArray response(74,'\0');qToBigEndian<quint16>(2,response.data());qToBigEndian<quint16>(70,response.data()+2);qToBigEndian<quint32>(101,response.data()+4);memcpy(response.data()+8,"127.0.0.1",9);qToBigEndian(port,response.data()+72);udp.writeDatagram(response,address,port);
  }});
  connect(&gateway,&QWebSocketServer::newConnection,this,[this]{auto *socket=gateway.nextPendingConnection();connect(socket,&QWebSocket::disconnected,socket,&QObject::deleteLater);
   connect(socket,&QWebSocket::textMessageReceived,this,[this,socket](const QString &message){const auto root=QJsonDocument::fromJson(message.toUtf8()).object();const auto data=root.value("d").toObject();QJsonObject reply;int opcode=-1;
    switch(root.value("op").toInt()){
     case 0:opcode=2;reply={{"ssrc",101},{"ip","127.0.0.1"},{"port",int(udp.localPort())},{"modes",QJsonArray{"aead_xchacha20_poly1305_rtpsize"}}};break;
     case 1:{opcode=4;QJsonArray key;for(int i=0;i<32;++i)key.append(1);reply={{"mode","aead_xchacha20_poly1305_rtpsize"},{"secret_key",key},{"dave_protocol_version",0}};break;}
     case 3:opcode=6;reply=data;break;
    }
    if(opcode>=0)socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op",opcode},{"d",reply}}).toJson(QJsonDocument::Compact)));
   });socket->sendTextMessage(R"({"op":8,"d":{"heartbeat_interval":1000}})");
  });return true;
 }
 QUrl url()const{return QUrl("ws://127.0.0.1:"+QString::number(gateway.serverPort()));}
 bool connectVoice(VoiceEngine &voice){
  voice.join("100","200","Fixture","42");voice.onVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","42"},{"session_id","fake-session"}});voice.onVoiceServer({{"guild_id","100"},{"token","fake-voice-token"},{"endpoint","fixture.discord.media"}});return true;
 }
};
}

class SoundboardTest final:public QObject {
 Q_OBJECT
private slots:

 void receivedServerEffectsApplyListeningGainsToMeasuredPcm_data(){QTest::addColumn<QString>("extension");QTest::newRow("wav")<<QString("wav");QTest::newRow("ogg")<<QString("ogg");QTest::newRow("mp3")<<QString("mp3");}
 void receivedServerEffectsApplyListeningGainsToMeasuredPcm(){
  QFETCH(QString,extension);const auto ffmpeg=QStandardPaths::findExecutable("ffmpeg");if(ffmpeg.isEmpty())QSKIP("FFmpeg fixture generator unavailable");
  QTemporaryDir directory;const auto path=directory.filePath("received."+extension);QProcess generator;generator.start(ffmpeg,{"-hide_banner","-loglevel","error","-f","lavfi","-i","sine=frequency=440:sample_rate=44100","-t","0.12","-y",path});QVERIFY(generator.waitForFinished(5000));QCOMPARE(generator.exitCode(),0);
  QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();QVERIFY(!bytes.isEmpty());
  AppSettings prefs(directory.filePath("prefs.ini"));RelationshipFixture cdn;QVERIFY(cdn.listen(QHostAddress::LocalHost));cdn.responses.enqueue({200,bytes});LocalVoiceFixture gateway;QVERIFY(gateway.start());
  VoiceEngine voice;QVERIFY(voice.configureLocalFixture(gateway.url()));QVERIFY(gateway.connectVoice(voice));QTRY_VERIFY(voice.connected());
  DiscordClient client;client.m_token="fake-soundboard-token";client.m_currentUserId="42";client.m_connected=true;client.m_guildObjects.insert("100",{{"id","100"}});client.m_voiceStates["100"]["42"]={{"channel_id","200"}};
  GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);board.m_cdnBase=cdn.base()+"/";
  const QJsonObject event{{"guild_id","100"},{"channel_id","200"},{"user_id","43"},{"sound_id","123"},{"sound_volume",1.}};
  const auto render=[&](double master,double userGain){
   voice.stopReceivedSoundboards();voice.takeFixtureAudio();prefs.setSoundboardVolume(master);voice.setUserSoundboardVolume("43",userGain);
   client.dispatch("VOICE_CHANNEL_EFFECT_SEND",event);board.stopPreview(); // Closing the menu must not cancel received audio.
   if(!QTest::qWaitFor([&]{return board.m_received.isEmpty();},3000))return QByteArray{};
   for(int i=0;i<20;++i)voice.processFixturePlayback();return voice.takeFixtureAudio();
  };
  const auto original=render(1,1);QVERIFY2(!original.isEmpty(),qPrintable(board.error()));QVERIFY(original.size()%3840==0);
  bool heard=false;for(int at=0;at+1<original.size();at+=2){qint16 sample;memcpy(&sample,original.constData()+at,2);heard|=sample!=0;}QVERIFY(heard);
  for(const auto gain:QList<double>{0,.25,1}){const auto output=render(gain,1);QCOMPARE(output.size(),original.size());for(int at=0;at+1<output.size();at+=2){qint16 before,after;memcpy(&before,original.constData()+at,2);memcpy(&after,output.constData()+at,2);QCOMPARE(after,qint16(std::lround(before*gain)));}}
  for(const auto gain:QList<double>{0,.5,2}){const auto output=render(1,gain);QCOMPARE(output.size(),original.size());for(int at=0;at+1<output.size();at+=2){qint16 before,after;memcpy(&before,original.constData()+at,2);memcpy(&after,output.constData()+at,2);QCOMPARE(after,qint16(std::lround(before*gain)));}}
  voice.setUserVolume("43",0);QCOMPARE(render(1,1),original);QCOMPARE(cdn.requests.size(),1);QVERIFY(board.m_previewData.data().isEmpty());QVERIFY(board.error().isEmpty());QCOMPARE(voice.fixtureCounters().value("sent").toULongLong(),quint64(0));voice.leave();
 }
 void pendingReceivedEffectsDoNotResumeAfterPrivacyOrAccountChanges_data(){QTest::addColumn<QString>("action");for(const auto &action:{"mic-test","deafen","leave","account"})QTest::newRow(action)<<QString(action);}
 void pendingReceivedEffectsDoNotResumeAfterPrivacyOrAccountChanges(){
  QFETCH(QString,action);const auto ffmpeg=QStandardPaths::findExecutable("ffmpeg");if(ffmpeg.isEmpty())QSKIP("FFmpeg fixture generator unavailable");
  QTemporaryDir directory;const auto path=directory.filePath("delayed.wav");QProcess generator;generator.start(ffmpeg,{"-hide_banner","-loglevel","error","-f","lavfi","-i","sine=frequency=440:sample_rate=48000","-t","0.12","-y",path});QVERIFY(generator.waitForFinished(5000));QCOMPARE(generator.exitCode(),0);QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));
  AppSettings prefs(directory.filePath("prefs.ini"));RelationshipFixture cdn;QVERIFY(cdn.listen(QHostAddress::LocalHost));cdn.responses.enqueue({200,file.readAll(),200});LocalVoiceFixture gateway;QVERIFY(gateway.start());VoiceEngine voice;QVERIFY(voice.configureLocalFixture(gateway.url()));QVERIFY(gateway.connectVoice(voice));QTRY_VERIFY(voice.connected());
  DiscordClient client;client.m_token="fake-token";client.m_currentUserId="42";client.m_connected=true;client.m_guildObjects.insert("100",{{"id","100"}});client.m_voiceStates["100"]["42"]={{"channel_id","200"}};
  GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);board.m_cdnBase=cdn.base()+"/";
  const QJsonObject event{{"guild_id","100"},{"channel_id","200"},{"user_id","43"},{"sound_id","123"},{"sound_volume",1.}};client.dispatch("VOICE_CHANNEL_EFFECT_SEND",event);QTRY_COMPARE(cdn.requests.size(),1);
  if(action=="mic-test"){voice.startMicTest();QTRY_VERIFY(voice.micTesting());client.dispatch("VOICE_CHANNEL_EFFECT_SEND",event);voice.stopMicTest();}
  else if(action=="deafen"){voice.setDeafened(true);client.dispatch("VOICE_CHANNEL_EFFECT_SEND",event);voice.setDeafened(false);}
  else if(action=="leave")voice.leave();else client.logout();
  QTest::qWait(300);QVERIFY(board.m_received.isEmpty());for(int i=0;i<20;++i)voice.processFixturePlayback();QVERIFY(voice.takeFixtureAudio().isEmpty());QCOMPARE(voice.fixtureCounters().value("receivedSoundboards").toInt(),0);QCOMPARE(cdn.requests.size(),1);voice.leave();
 }

 void rapidPrivacyChangesKeepNewEffectsAndRejectOldPlaybackGenerations_data(){QTest::addColumn<bool>("micTest");QTest::newRow("mic-test")<<true;QTest::newRow("deafen")<<false;}
 void rapidPrivacyChangesKeepNewEffectsAndRejectOldPlaybackGenerations(){
  QFETCH(bool,micTest);const auto ffmpeg=QStandardPaths::findExecutable("ffmpeg");if(ffmpeg.isEmpty())QSKIP("FFmpeg fixture generator unavailable");
  QTemporaryDir directory;const auto path=directory.filePath("fresh.wav");QProcess generator;generator.start(ffmpeg,{"-hide_banner","-loglevel","error","-f","lavfi","-i","sine=frequency=440:sample_rate=48000","-t","0.12","-y",path});QVERIFY(generator.waitForFinished(5000));QCOMPARE(generator.exitCode(),0);QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();
  AppSettings prefs(directory.filePath("prefs.ini"));RelationshipFixture cdn;QVERIFY(cdn.listen(QHostAddress::LocalHost));cdn.responses.enqueue({200,bytes,200});cdn.responses.enqueue({200,bytes});LocalVoiceFixture gateway;QVERIFY(gateway.start());VoiceEngine voice;QVERIFY(voice.configureLocalFixture(gateway.url()));QVERIFY(gateway.connectVoice(voice));QTRY_VERIFY(voice.connected());
  DiscordClient client;client.m_token="fake-token";client.m_currentUserId="42";client.m_connected=true;client.m_guildObjects.insert("100",{{"id","100"}});client.m_voiceStates["100"]["42"]={{"channel_id","200"}};
  GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);board.m_cdnBase=cdn.base()+"/";
  QJsonObject event{{"guild_id","100"},{"channel_id","200"},{"user_id","43"},{"sound_id","123"},{"sound_volume",1.}};client.dispatch("VOICE_CHANNEL_EFFECT_SEND",event);QTRY_COMPARE(cdn.requests.size(),1);
  if(micTest){voice.startMicTest();QVERIFY(voice.micTesting());voice.stopMicTest();}else{voice.setDeafened(true);voice.setDeafened(false);}
  // New input arrives before the GUI drains queued notifications from the
  // completed privacy transition. An unconditional reset signal loses it.
  event.insert("sound_id","124");client.dispatch("VOICE_CHANNEL_EFFECT_SEND",event);
  QTRY_VERIFY_WITH_TIMEOUT(board.m_received.isEmpty(),3000);for(int i=0;i<20;++i)voice.processFixturePlayback();const auto output=voice.takeFixtureAudio();QVERIFY(!output.isEmpty());bool heard=false;for(int at=0;at+1<output.size();at+=2){qint16 sample;memcpy(&sample,output.constData()+at,2);heard|=sample!=0;}QVERIFY(heard);QCOMPARE(cdn.requests.size(),2);QTest::qWait(250);for(int i=0;i<20;++i)voice.processFixturePlayback();QVERIFY(voice.takeFixtureAudio().isEmpty());voice.leave();
 }

 void favoritesPersistPerAccountAndNeverStartPlayback(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("prefs.ini"));DiscordClient client;client.m_currentUserId="42";
  VoiceEngine voice;GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);
  board.add(QJsonArray{QJsonObject{{"sound_id","123"},{"name","Saved fixture"},{"available",true}}},"100");
  QVERIFY(board.setFavorite("123",true));QCOMPARE(board.favorites().size(),1);QVERIFY(board.frequent().isEmpty());QVERIFY(!board.previewLoading());QVERIFY(board.m_previewData.data().isEmpty());
  Soundboard restored(&client,&voice,&permissions,&prefs);QCOMPARE(restored.favorites().size(),1);QVERIFY(restored.favorites().first().toMap().value("needsReload").toBool());QVERIFY(!restored.favorites().first().toMap().value("available").toBool());
  client.m_currentUserId="43";emit client.stateChanged();QVERIFY(board.favorites().isEmpty());
  client.m_currentUserId="42";emit client.stateChanged();QCOMPARE(board.favorites().size(),1);
  QVERIFY(board.setFavorite("123",false));QVERIFY(board.favorites().isEmpty());
 }
 void savedSoundsRefetchAndCannotUnlockRemovedOrUnavailableSounds(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("prefs.ini"));RelationshipFixture fixture;QVERIFY(fixture.listen(QHostAddress::LocalHost));
  fixture.responses.enqueue({200,"[]"});fixture.responses.enqueue({200,R"({"items":[{"sound_id":"123","name":"Locked fixture","available":false}]})"});
  DiscordClient client;client.m_currentUserId="42";client.m_connected=true;client.m_apiBase=fixture.base();client.m_token="fixture";client.m_guildObjects.insert("100",{{"id","100"}});
  VoiceEngine voice;GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);
  board.add(QJsonArray{QJsonObject{{"sound_id","123"},{"name","Previously available"},{"available",true}}},"100");QVERIFY(board.setFavorite("123",true));board.reset();
  QVERIFY(board.activate("123",true));QTRY_VERIFY(!board.busy());QCOMPARE(fixture.requests.size(),2);QVERIFY(!board.error().isEmpty());QVERIFY(!board.previewLoading());QVERIFY(board.m_previewData.data().isEmpty());QVERIFY(!board.activate("123",true));QCOMPARE(fixture.requests.size(),2);
 }
 void closingPreviewCancelsSavedSoundActivation(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("prefs.ini"));RelationshipFixture fixture;QVERIFY(fixture.listen(QHostAddress::LocalHost));
  fixture.responses.enqueue({200,"[]",80});fixture.responses.enqueue({200,R"({"items":[{"sound_id":"123","name":"Fixture","available":true}]})",80});
  DiscordClient client;client.m_currentUserId="42";client.m_connected=true;client.m_apiBase=fixture.base();client.m_token="fixture";client.m_guildObjects.insert("100",{{"id","100"}});
  VoiceEngine voice;GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);
  board.add(QJsonArray{QJsonObject{{"sound_id","123"},{"name","Fixture"}}},"100");QVERIFY(board.setFavorite("123",true));board.reset();QVERIFY(board.activate("123",true));board.stopPreview();QTRY_VERIFY(!board.busy());QVERIFY(board.m_pendingActivation.isEmpty());QVERIFY(!board.previewLoading());QCOMPARE(fixture.requests.size(),2);QVERIFY(board.m_previewData.data().isEmpty());
 }
 void frequentSoundsAreRankedBoundedAndPersistedSeparatelyFromFavorites(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("prefs.ini"));DiscordClient client;client.m_currentUserId="42";VoiceEngine voice;GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);
  board.add(QJsonArray{QJsonObject{{"sound_id","123"},{"name","First"}},QJsonObject{{"sound_id","124"},{"name","Second"}}},{});
  board.rememberUse("123");board.rememberUse("124");board.rememberUse("124");QCOMPARE(board.frequent().size(),2);QCOMPARE(board.frequent().first().toMap().value("id").toString(),QString("124"));QVERIFY(board.favorites().isEmpty());
  Soundboard restored(&client,&voice,&permissions,&prefs);QCOMPARE(restored.frequent().size(),2);QVERIFY(restored.setFavorite("124",true));QCOMPARE(restored.favorites().size(),1);QVERIFY(!restored.previewLoading());
 }
 void extensionlessServerClipsDecodeFromACompleteSeekableBuffer_data(){QTest::addColumn<QString>("extension");QTest::newRow("wav")<<QString("wav");QTest::newRow("ogg")<<QString("ogg");QTest::newRow("mp3")<<QString("mp3");}
 void extensionlessServerClipsDecodeFromACompleteSeekableBuffer(){
  QFETCH(QString,extension);const auto ffmpeg=QStandardPaths::findExecutable("ffmpeg");if(ffmpeg.isEmpty())QSKIP("FFmpeg fixture generator unavailable");
  QTemporaryDir directory;const auto path=directory.filePath("clip."+extension);QProcess generator;generator.start(ffmpeg,{"-hide_banner","-loglevel","error","-f","lavfi","-i","sine=frequency=440:sample_rate=48000","-t","0.15","-y",path});QVERIFY(generator.waitForFinished(5000));QCOMPARE(generator.exitCode(),0);
  QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto bytes=file.readAll();QVERIFY(!bytes.isEmpty());
  AppSettings prefs(directory.filePath("prefs.ini"));RelationshipFixture fixture;QVERIFY(fixture.listen(QHostAddress::LocalHost));fixture.responses.enqueue({200,bytes});
  DiscordClient client;VoiceEngine voice;GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);board.m_cdnBase=fixture.base()+"/";board.m_sounds.append(QVariantMap{{"id","123"},{"name","Fixture"},{"volume",1.}});
  board.m_preview.setAudioOutput(nullptr);QAudioBufferOutput output;board.m_preview.setAudioBufferOutput(&output);QSignalSpy buffers(&output,&QAudioBufferOutput::audioBufferReceived);
  QVERIFY(board.preview("123"));QTRY_VERIFY(!board.previewLoading());QVERIFY2(board.error().isEmpty(),qPrintable(board.error()));QCOMPARE(board.m_previewData.data(),bytes);QTRY_VERIFY_WITH_TIMEOUT(buffers.count()>0,3000);
  board.stopPreview();QVERIFY(board.preview("123"));QCOMPARE(fixture.requests.size(),1);QVERIFY(!board.previewLoading());board.stopPreview();
 }
 void cancelledPreviewCannotReplaceTheNextClip(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("prefs.ini"));RelationshipFixture fixture;QVERIFY(fixture.listen(QHostAddress::LocalHost));fixture.responses.enqueue({404,"unavailable",80});
  DiscordClient client;VoiceEngine voice;GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);board.m_cdnBase=fixture.base()+"/";board.m_sounds.append(QVariantMap{{"id","123"},{"name","Fixture"}});
  QVERIFY(board.preview("123"));QTRY_COMPARE(fixture.requests.size(),1);board.stopPreview();QTest::qWait(120);QVERIFY(!board.previewLoading());QVERIFY(board.error().isEmpty());QVERIFY(board.m_previewData.data().isEmpty());
 }
 void explicitServerPlayUsesTheCurrentVoiceChannelAndHonorsDeafen(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("prefs.ini"));RelationshipFixture fixture;QVERIFY(fixture.listen(QHostAddress::LocalHost));
  fixture.responses.enqueue({200,"[]"});fixture.responses.enqueue({200,R"({"items":[{"sound_id":"123","name":"Sound","available":true}]})"});fixture.responses.enqueue({204,{}});
  QWebSocketServer gateway("soundboard voice fixture",QWebSocketServer::NonSecureMode);QVERIFY(gateway.listen(QHostAddress::LocalHost));QUdpSocket udp;QVERIFY(udp.bind(QHostAddress::LocalHost,0));
  connect(&udp,&QUdpSocket::readyRead,this,[&]{while(udp.hasPendingDatagrams()){QByteArray packet(2048,Qt::Uninitialized);QHostAddress address;quint16 port;const auto size=udp.readDatagram(packet.data(),packet.size(),&address,&port);if(size!=74)continue;QByteArray response(74,'\0');qToBigEndian<quint16>(2,response.data());qToBigEndian<quint16>(70,response.data()+2);qToBigEndian<quint32>(101,response.data()+4);memcpy(response.data()+8,"127.0.0.1",9);qToBigEndian(port,response.data()+72);udp.writeDatagram(response,address,port);}});
  connect(&gateway,&QWebSocketServer::newConnection,this,[&]{auto *socket=gateway.nextPendingConnection();connect(socket,&QWebSocket::disconnected,socket,&QObject::deleteLater);connect(socket,&QWebSocket::textMessageReceived,this,[&,socket](const QString &message){const auto root=QJsonDocument::fromJson(message.toUtf8()).object();const auto data=root.value("d").toObject();QJsonObject reply;int opcode=-1;
    switch(root.value("op").toInt()){
     case 0:opcode=2;reply={{"ssrc",101},{"ip","127.0.0.1"},{"port",int(udp.localPort())},{"modes",QJsonArray{"aead_xchacha20_poly1305_rtpsize"}}};break;
     case 1:{opcode=4;QJsonArray key;for(int i=0;i<32;++i)key.append(1);reply={{"mode","aead_xchacha20_poly1305_rtpsize"},{"secret_key",key},{"dave_protocol_version",0}};break;}
     case 3:opcode=6;reply=data;break;
    }
    if(opcode>=0)socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"op",opcode},{"d",reply}}).toJson(QJsonDocument::Compact)));
   });socket->sendTextMessage(R"({"op":8,"d":{"heartbeat_interval":1000}})");});
  VoiceEngine voice;if(!voice.available())QSKIP("DAVE is unavailable");QVERIFY(voice.configureLocalFixture(QUrl("ws://127.0.0.1:"+QString::number(gateway.serverPort()))));voice.join("100","200","Fixture","42");voice.onVoiceState({{"guild_id","100"},{"channel_id","200"},{"user_id","42"},{"session_id","fake-session"}});voice.onVoiceServer({{"guild_id","100"},{"token","fake-voice-token"},{"endpoint","fixture.discord.media"}});QTRY_VERIFY(voice.connected());
  DiscordClient client;client.m_apiBase=fixture.base();client.m_token="fake-soundboard-token";client.m_currentUserId="42";client.m_connected=true;client.m_guildObjects.insert("100",{{"id","100"},{"owner_id","42"}});client.m_channelCache.insert("100",QJsonArray{QJsonObject{{"id","200"},{"type",2}}});client.m_voiceStates["100"]["42"]={{"channel_id","200"}};
  GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);board.load("100");QTRY_VERIFY(!board.busy());QVERIFY(board.canPlay());QVERIFY(board.play("123"));QTRY_VERIFY(!board.sending());QCOMPARE(fixture.requests.size(),3);QCOMPARE(board.frequent().size(),1);QCOMPARE(board.frequent().first().toMap().value("id").toString(),QString("123"));
  const auto request=fixture.requests.last();QVERIFY(request.startsWith("POST /channels/200/send-soundboard-sound "));const auto body=QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n")+4)).object();QCOMPARE(body.value("sound_id").toString(),QString("123"));QCOMPARE(body.value("source_guild_id").toString(),QString("100"));
  voice.setMuted(true);QVERIFY(board.canPlay());voice.setDeafened(true);QVERIFY(!board.canPlay());QVERIFY(!board.play("123"));QCOMPARE(fixture.requests.size(),3);voice.leave();
 }
 void loadsDefaultAndServerSoundsAndNeverSendsWithoutAnExplicitEligibleAction(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("prefs.ini"));
  RelationshipFixture fixture;QVERIFY(fixture.listen(QHostAddress::LocalHost));
  fixture.responses.enqueue({200,R"([{"sound_id":"1","name":"Quack","volume":0.5,"emoji_name":"🦆","available":true}])"});
  fixture.responses.enqueue({200,R"({"items":[{"sound_id":"123","name":"Server sound","volume":1,"available":true},{"sound_id":"../bad","name":"bad"}]})"});
  DiscordClient client;client.m_apiBase=fixture.base();client.m_token="fake-soundboard-token";client.m_currentUserId="42";client.m_connected=true;client.m_guildObjects.insert("100",{{"id","100"},{"name","Fixture"},{"owner_id","42"}});
  VoiceEngine voice;GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);
  board.load("100");QTRY_VERIFY(!board.busy());QCOMPARE(board.sounds().size(),2);QCOMPARE(fixture.requests.size(),2);
  QVERIFY(fixture.requests[0].startsWith("GET /soundboard-default-sounds "));QVERIFY(fixture.requests[1].startsWith("GET /guilds/100/soundboard-sounds "));
  QVERIFY(!board.canPlay());QVERIFY(!board.play("123"));QCOMPARE(fixture.requests.size(),2);
  const auto generation=client.m_generation;++client.m_generation;emit client.stateChanged();QVERIFY(board.sounds().isEmpty());QVERIFY(!board.sending());QVERIFY(client.m_generation>generation);
 }
 void pendingReadsAreDiscardedAfterLogout(){
  QTemporaryDir directory;AppSettings prefs(directory.filePath("prefs.ini"));RelationshipFixture fixture;QVERIFY(fixture.listen(QHostAddress::LocalHost));fixture.responses.enqueue({200,R"([{"sound_id":"1","name":"Old sound"}])",80});
  DiscordClient client;client.m_apiBase=fixture.base();client.m_token="fake-token";client.m_currentUserId="42";client.m_connected=true;
  VoiceEngine voice;GuildManagement permissions(&client);Soundboard board(&client,&voice,&permissions,&prefs);board.load("");QTRY_COMPARE(fixture.requests.size(),1);
  client.logout();QTest::qWait(120);QVERIFY(board.sounds().isEmpty());QVERIFY(!board.busy());
 }
};
QTEST_GUILESS_MAIN(SoundboardTest)
#include "soundboard_test.moc"
