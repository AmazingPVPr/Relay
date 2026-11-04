#include "NativeMediaSource.h"
#include <QBuffer>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace {
QByteArray master(const QString &base="https://manifest.googlevideo.com"){
    QByteArray bytes="#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-INDEPENDENT-SEGMENTS\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"English, original\",LANGUAGE=\"en\",DEFAULT=YES,AUTOSELECT=YES,URI=\""+base.toUtf8()+"/audio.m3u8\"\n";
    for(const auto &size:QList<QSize>{{3840,2160},{1920,1080},{1280,720}})bytes+="#EXT-X-STREAM-INF:BANDWIDTH=1000000,CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION="+QByteArray::number(size.width())+"x"+QByteArray::number(size.height())+",FRAME-RATE=60,AUDIO=\"audio\"\n"+base.toUtf8()+"/"+QByteArray::number(size.height())+".m3u8\n";
    return bytes;
}
class ManifestPeer final:public QTcpServer {
public:
    QByteArray body;int status=200,delay=0;QList<QByteArray> requests;
    ManifestPeer(){connect(this,&QTcpServer::newConnection,this,[this]{while(auto *socket=nextPendingConnection()){
        connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        connect(socket,&QTcpSocket::readyRead,this,[this,socket]{auto bytes=socket->property("request").toByteArray()+socket->readAll();socket->setProperty("request",bytes);if(socket->property("done").toBool() || !bytes.contains("\r\n\r\n"))return;socket->setProperty("done",true);requests.append(bytes);QTimer::singleShot(delay,socket,[this,socket]{if(socket->state()!=QAbstractSocket::ConnectedState)return;socket->write("HTTP/1.1 "+QByteArray::number(status)+" Generated\r\nContent-Type: application/vnd.apple.mpegurl\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);socket->disconnectFromHost();});});
    }});}
    QString base()const{return "http://127.0.0.1:"+QString::number(serverPort());}
};
}
class NativeMediaSourceTest final:public QObject {
    Q_OBJECT
private slots:
    void parsesOneVariantWithOriginalAudioAndRetainsAllQualityChoices(){
        NativeMediaSource source;source.m_source=QUrl("https://manifest.googlevideo.com/api/manifest/hls_variant/generated/index.m3u8");
        QVERIFY(source.parse(master()));QCOMPARE(source.qualityOptions().size(),3);QCOMPARE(source.activeQuality(),1);QCOMPARE(source.qualityOptions()[1].toMap().value("height").toInt(),1080);
        const auto filtered=source.manifestFor(1);QCOMPARE(filtered.count("#EXT-X-STREAM-INF:"),1);QVERIFY(filtered.contains("RESOLUTION=1920x1080"));QVERIFY(!filtered.contains("3840x2160"));QVERIFY(filtered.contains("TYPE=AUDIO"));QVERIFY(filtered.contains("English, original"));QVERIFY(filtered.contains("/audio.m3u8"));
        QVERIFY(source.manifestFor(0).contains("3840x2160"));QVERIFY(source.manifestFor(3).isEmpty());
    }
    void malformedUntrustedOversizedAndUnknownUriDirectivesUseNormalPath(){
        NativeMediaSource source;source.m_source=QUrl("https://manifest.googlevideo.com/api/manifest/hls_variant/generated/index.m3u8");
        QVERIFY(!source.parse("not a playlist"));QVERIFY(!source.parse(QByteArray(1024*1024+1,'x')));
        auto invalid=master();invalid.replace("https://manifest.googlevideo.com/audio.m3u8","https://evil.invalid/audio.m3u8");QVERIFY(!source.parse(invalid));
        invalid=master();invalid.replace("https://manifest.googlevideo.com/1080.m3u8","file:///tmp/no-file.m3u8");QVERIFY(!source.parse(invalid));
        invalid=master();invalid+="#EXT-X-SESSION-DATA:DATA-ID=\"unknown\",URI=\"https://manifest.googlevideo.com/x\"\n";QVERIFY(!source.parse(invalid));
        QVERIFY(NativeMediaSource::attributes("CODECS=\"avc1,mp4a\",AUDIO=\"group\"").contains("AUDIO"));QVERIFY(NativeMediaSource::attributes("AUDIO=\"x\",AUDIO=\"y\"").isEmpty());
        for(const auto &url:QStringList{"http://manifest.googlevideo.com/x","https://googlevideo.com.evil.invalid/x","https://u:p@manifest.googlevideo.com/x","https://manifest.googlevideo.com:8443/x"})QVERIFY(!source.trusted(QUrl(url)));
    }
    void directSourcesNeverStartHelperRequests(){
        NativeMediaSource source;QMediaPlayer player;QVERIFY(!source.open(&player,"https://cdn.discordapp.com/attachments/1/2/generated.mp4"));QVERIFY(!source.open(&player,"https://127.0.0.1/generated.mp4"));QVERIFY(!source.open(nullptr,"https://manifest.googlevideo.com/api/manifest/hls_variant/generated/index.m3u8"));QVERIFY(!source.active());QVERIFY(player.audioOutput()==nullptr);QVERIFY(player.source().isEmpty());
    }
    void generatedNetworkManifestUsesQBufferWithNoAccountHeadersAndBoundsDevices(){
        ManifestPeer peer;QVERIFY(peer.listen(QHostAddress::LocalHost));peer.body=master(peer.base());
        NativeMediaSource source;source.m_fixtureOrigin=QUrl(peer.base());QMediaPlayer player;QVERIFY(source.open(&player,peer.base()+"/master.m3u8"));QTRY_VERIFY(source.active());
        QCOMPARE(source.qualityOptions().size(),3);QCOMPARE(source.activeQuality(),1);QVERIFY(player.sourceDevice());QVERIFY(player.audioOutput()==nullptr);QVERIFY(!peer.requests.first().toLower().contains("authorization:"));QVERIFY(!peer.requests.first().toLower().contains("cookie:"));
        for(int i=0;i<30;++i)QVERIFY(source.selectQuality(i%3));QVERIFY(player.findChildren<QBuffer*>().size()<=3);QVERIFY(!source.selectQuality(30));source.cancel();player.setSource({});
    }
    void cancelledOrDestroyedFetchNeverSetsPlayerSourceLater(){
        ManifestPeer peer;QVERIFY(peer.listen(QHostAddress::LocalHost));peer.body=master(peer.base());peer.delay=80;
        QMediaPlayer player;{NativeMediaSource source;source.m_fixtureOrigin=QUrl(peer.base());QVERIFY(source.open(&player,peer.base()+"/master.m3u8"));source.cancel();QTest::qWait(120);QVERIFY(player.source().isEmpty());QVERIFY(!source.active());}
        {auto *source=new NativeMediaSource;source->m_fixtureOrigin=QUrl(peer.base());QVERIFY(source->open(&player,peer.base()+"/master.m3u8"));delete source;}QTest::qWait(120);QVERIFY(player.source().isEmpty());
    }
    void failedPreparationFallsBackToOriginalMaster(){
        ManifestPeer peer;QVERIFY(peer.listen(QHostAddress::LocalHost));peer.body="Generated invalid playlist";
        NativeMediaSource source;source.m_fixtureOrigin=QUrl(peer.base());QMediaPlayer player;const auto url=QUrl(peer.base()+"/master.m3u8");QVERIFY(source.open(&player,url.toString()));QTRY_COMPARE(player.source(),url);QVERIFY(!source.active());QVERIFY(source.qualityOptions().isEmpty());QVERIFY(player.sourceDevice()==nullptr);player.setSource({});
    }
};
QTEST_GUILESS_MAIN(NativeMediaSourceTest)
#include "native_media_source_test.moc"
