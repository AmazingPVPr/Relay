#include "VideoResolver.h"
#include "InlineMedia.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QtTest>

namespace {
QJsonObject combined(const QString &url="https://rr1---fixture.googlevideo.com/videoplayback?expire=9999999999&sig=a%2Bb%2Fc") {
    return {{"url",url},{"title","Synthetic video"},{"width",1280},{"height",720},{"ext","mp4"},{"vcodec","avc1.64001f"},{"acodec","mp4a.40.2"},{"duration",60}};
}
QByteArray bytes(const QJsonObject &object){return QJsonDocument(object).toJson(QJsonDocument::Compact);}
QString fixture(QTemporaryDir &directory) {
    const auto path=directory.filePath("resolver.py");QFile file(path);if(!file.open(QIODevice::WriteOnly))return {};
    file.write(R"PY(#!/usr/bin/python3
import json,sys,time
from urllib.parse import urlparse,parse_qs
with open(__file__+'.args','w') as f: json.dump(sys.argv[1:],f)
video=parse_qs(urlparse(sys.argv[-1]).query).get('v',[''])[0]
with open(__file__+'.calls','a') as f: f.write(video+'\n')
if video in ('BBBBBBBBBBB','FFFFFFFFFFF'): time.sleep(2)
if video == 'CCCCCCCCCCC':
    sys.stderr.write('ERROR: Sign in to confirm your age. fixture-private-detail')
    sys.exit(1)
if video == 'DDDDDDDDDDD':
    sys.stdout.write('x'*(5*1024*1024));sys.exit(0)
if video == 'EEEEEEEEEEE':
    print(json.dumps({'url':'http://127.0.0.1/private','width':1280,'height':720,'ext':'mp4','vcodec':'avc1','acodec':'mp4a'}));sys.exit(0)
print(json.dumps({'url':'https://rr1---fixture.googlevideo.com/videoplayback?expire=9999999999&sig=a%2Bb%2Fc','width':1280,'height':720,'ext':'mp4','vcodec':'avc1','acodec':'mp4a','title':'Synthetic video'}))
)PY");file.close();if(!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner))return {};return path;
}
}
class VideoResolverTest final:public QObject {
    Q_OBJECT
private slots:
    void init() {
#ifdef Q_OS_WIN
        const QString name=QString::fromLatin1(QTest::currentTestFunction());
        if(!QStringList{"urlPolicyRejectsSpoofingAndPreservesOffsets",
            "directMediaNeverStartsAProcessAndCancellationSuppressesReply",
            "nativeHlsUsesMasterWithAudioNotVideoOnlyVariant",
            "higherQualityHlsBeatsLowerCombinedVideo"}.contains(name))
            QSKIP("This generated extractor is a Unix executable script; the portable helper is optional.");
#endif
    }
    void urlPolicyRejectsSpoofingAndPreservesOffsets() {
        QCOMPARE(InlineMedia::youtubePage("https://youtu.be/b2tjnxX8IHU?t=262"),QString("https://www.youtube.com/watch?v=b2tjnxX8IHU&t=262"));
        QCOMPARE(InlineMedia::startSeconds("https://www.youtube.com/watch?v=AAAAAAAAAAA&t=1h2m3s"),3723);
        QCOMPARE(InlineMedia::startSeconds("https://www.youtube.com/embed/AAAAAAAAAAA?start=12"),12);
        for(const auto &url:QStringList{"file:///tmp/movie.mp4","https://youtube.com.evil.invalid/watch?v=AAAAAAAAAAA","https://user:pass@youtube.com/watch?v=AAAAAAAAAAA","https://youtu.be/../bad","https://www.youtube.com/watch?v=AAAAAAAAAAA\n--cookies","https://www.youtube.com/playlist?list=AAAAAAAAAAA","http://youtu.be/AAAAAAAAAAA"})QVERIFY2(InlineMedia::youtubePage(url).isEmpty(),qPrintable(url));
        QVERIFY(InlineMedia::directMedia("https://static.klipy.com/ii/fixture.mp4?key=a%2Bb").contains("key=a%2Bb"));
        QVERIFY(InlineMedia::directMedia("https://media.tenor.com.evil.invalid/clip.mp4").isEmpty());
        QVERIFY(InlineMedia::directMedia("https://127.0.0.1/clip.mp4").isEmpty());
    }
    void directMediaNeverStartsAProcessAndCancellationSuppressesReply() {
        VideoResolver resolver(QString{});QSignalSpy success(&resolver,&VideoResolver::resolved),failure(&resolver,&VideoResolver::failed);
        const auto url=QString("https://cdn.discordapp.com/attachments/1/2/clip.webm?ex=ffff&hm=a%2Fb");
        QVERIFY(resolver.resolve("direct:1",url));QTRY_COMPARE(success.count(),1);QCOMPARE(success.first()[1].toMap().value("sourceUrl").toString(),url);QCOMPARE(success.first()[1].toMap().value("mimeType").toString(),QString("video/webm"));
        QVERIFY(resolver.resolve("cancel",url));resolver.cancel("cancel");QTest::qWait(20);QCOMPARE(success.count(),1);QVERIFY(resolver.findChildren<QProcess*>().isEmpty());
        QVERIFY(!resolver.resolve("missing","https://youtu.be/AAAAAAAAAAA"));QCOMPARE(failure.count(),1);QVERIFY(failure.first()[1].toString().contains("bundled"));
    }
    void extractorArgumentsDisableCredentialsConfigPluginsAndDownloads() {
        QTemporaryDir directory;const auto program=fixture(directory);QVERIFY(!program.isEmpty());VideoResolver resolver(program);
        QSignalSpy success(&resolver,&VideoResolver::resolved);QVERIFY(resolver.resolve("yt","https://youtu.be/AAAAAAAAAAA?t=262&list=anything"));QTRY_COMPARE_WITH_TIMEOUT(success.count(),1,3000);
        const auto media=success.first()[1].toMap();QCOMPARE(media.value("startSeconds").toInt(),262);QVERIFY(media.value("sourceUrl").toString().contains("sig=a%2Bb%2Fc"));
        QFile file(program+".args");QVERIFY(file.open(QIODevice::ReadOnly));const auto args=QJsonDocument::fromJson(file.readAll()).array().toVariantList();
        for(const auto &flag:QStringList{"--ignore-config","--no-plugin-dirs","--no-remote-components","--skip-download","--dump-single-json","--no-playlist","--no-cache-dir","--no-check-formats","--use-extractors"})QVERIFY(args.contains(flag));
        QVERIFY(args.contains("youtube:skip=dash,translated_subs;player_skip=initial_data"));
        QVERIFY(!args.contains("--cookies"));QVERIFY(!args.contains("--cookies-from-browser"));QVERIFY(!args.contains("--exec"));QCOMPARE(args.last().toString(),QString("https://www.youtube.com/watch?v=AAAAAAAAAAA"));QCOMPARE(args[args.size()-2].toString(),QString("--"));
    }
    void nativeHlsUsesMasterWithAudioNotVideoOnlyVariant() {
        auto video=combined("https://rr1---fixture.googlevideo.com/videoplayback?video-only=1");video.insert("acodec","none");video.insert("protocol","m3u8_native");video.insert("manifest_url","https://manifest.googlevideo.com/api/manifest/hls_variant/expire/9999999999/id/fixture/index.m3u8");
        auto result=VideoResolver::parseResult(bytes({{"title","Master"},{"formats",QJsonArray{video}}}));QVERIFY(!result.isEmpty());QVERIFY(result.value("adaptive").toBool());QVERIFY(result.value("hasAudio").toBool());QCOMPARE(result.value("sourceUrl").toString(),video.value("manifest_url").toString());
        video.insert("manifest_url","https://manifest.googlevideo.com.evil.invalid/api/manifest/hls_variant/fixture");QVERIFY(VideoResolver::parseResult(bytes({{"formats",QJsonArray{video}}})).isEmpty());
        auto raw=combined();raw.insert("acodec","none");QVERIFY(VideoResolver::parseResult(bytes(raw)).isEmpty());
        raw=combined("file:///tmp/unsafe.mp4");QVERIFY(VideoResolver::parseResult(bytes(raw)).isEmpty());
        raw=combined();raw.insert("height",9000);QVERIFY(VideoResolver::parseResult(bytes(raw)).isEmpty());
    }
    void higherQualityHlsBeatsLowerCombinedVideo() {
        auto metadata=combined();auto high=combined();high.insert("url","https://rr1---fixture.googlevideo.com/videoplayback?video-only");
        high.insert("width",3840);high.insert("height",2160);high.insert("acodec","none");high.insert("vcodec","vp9");
        high.insert("manifest_url","https://manifest.googlevideo.com/api/manifest/hls_variant/fixture/index.m3u8");
        metadata.insert("formats",QJsonArray{combined(),high});
        auto media=VideoResolver::parseResult(bytes(metadata));QVERIFY(media.value("adaptive").toBool());QCOMPARE(media.value("height").toInt(),2160);QCOMPARE(media.value("width").toInt(),3840);
        high.insert("height",360);high.insert("width",640);metadata.insert("formats",QJsonArray{high});
        media=VideoResolver::parseResult(bytes(metadata));QVERIFY(!media.value("adaptive").toBool());QCOMPARE(media.value("height").toInt(),720);
        high.insert("height",2160);high.remove("width");metadata.insert("formats",QJsonArray{high});
        media=VideoResolver::parseResult(bytes(metadata));QCOMPARE(media.value("height").toInt(),720);
        high.insert("width",3840);high.insert("has_drm",true);metadata.insert("formats",QJsonArray{high});
        media=VideoResolver::parseResult(bytes(metadata));QCOMPARE(media.value("height").toInt(),720);
    }
    void cancellationReuseAndConcurrencyStayIndependent() {
        QTemporaryDir directory;VideoResolver resolver(fixture(directory));QSignalSpy success(&resolver,&VideoResolver::resolved),failure(&resolver,&VideoResolver::failed);
        QVERIFY(resolver.resolve("one","https://youtu.be/BBBBBBBBBBB"));QVERIFY(resolver.resolve("two","https://youtu.be/FFFFFFFFFFF"));QVERIFY(!resolver.resolve("three","https://youtu.be/AAAAAAAAAAA"));QCOMPARE(failure.count(),1);
        resolver.cancel("one");QVERIFY(resolver.resolve("one","https://youtu.be/AAAAAAAAAAA"));QTRY_COMPARE_WITH_TIMEOUT(success.count(),1,3000);QCOMPARE(success.first()[0].toString(),QString("one"));resolver.cancelAll();QTest::qWait(40);QCOMPARE(success.count(),1);QVERIFY(resolver.m_jobs.isEmpty());
    }
    void timeoutOversizedOutputAndRestrictionHaveSafeErrors() {
        QTemporaryDir directory;VideoResolver resolver(fixture(directory));QSignalSpy failure(&resolver,&VideoResolver::failed),success(&resolver,&VideoResolver::resolved);
        resolver.m_timeoutMs=50;QVERIFY(resolver.resolve("timeout","https://youtu.be/BBBBBBBBBBB"));QTRY_COMPARE_WITH_TIMEOUT(failure.count(),1,1000);QVERIFY(failure.last()[1].toString().contains("timed out"));resolver.m_timeoutMs=3000;
        QVERIFY(resolver.resolve("restricted","https://youtu.be/CCCCCCCCCCC"));QTRY_COMPARE_WITH_TIMEOUT(failure.count(),2,2000);QVERIFY(failure.last()[1].toString().contains("verification"));QVERIFY(!failure.last()[1].toString().contains("fixture-private-detail"));
        QVERIFY(resolver.resolve("large","https://youtu.be/DDDDDDDDDDD"));QTRY_COMPARE_WITH_TIMEOUT(failure.count(),3,2000);QVERIFY(failure.last()[1].toString().contains("too much"));
        QVERIFY(resolver.resolve("unsafe","https://youtu.be/EEEEEEEEEEE"));QTRY_COMPARE_WITH_TIMEOUT(failure.count(),4,2000);QVERIFY(failure.last()[1].toString().contains("trusted"));QCOMPARE(success.count(),0);QVERIFY(resolver.m_jobs.isEmpty());
    }
    void replayReusesResolvedMediaAcrossOffsetsAndCanonicalLinksWithoutAnotherProcess() {
        QTemporaryDir directory;const auto program=fixture(directory);VideoResolver resolver(program);
        QSignalSpy success(&resolver,&VideoResolver::resolved);
        QVERIFY(resolver.resolve("first","https://youtu.be/AAAAAAAAAAA?t=14"));QTRY_COMPARE_WITH_TIMEOUT(success.count(),1,3000);
        QCOMPARE(success.first()[1].toMap().value("startSeconds").toInt(),14);QCOMPARE(resolver.m_cache.size(),1);
        QElapsedTimer replay;replay.start();
        QVERIFY(resolver.resolve("replay","https://www.youtube.com/watch?v=AAAAAAAAAAA&start=42"));QTRY_COMPARE_WITH_TIMEOUT(success.count(),2,500);
        QVERIFY(replay.elapsed()<500);QCOMPARE(success.last()[0].toString(),QString("replay"));
        QCOMPARE(success.last()[1].toMap().value("startSeconds").toInt(),42);
        QCOMPARE(success.first()[1].toMap().value("sourceUrl"),success.last()[1].toMap().value("sourceUrl"));
        QFile calls(program+".calls");QVERIFY(calls.open(QIODevice::ReadOnly));QCOMPARE(calls.readAll(),QByteArray("AAAAAAAAAAA\n"));
        QVERIFY(resolver.resolve("cancelled-cache-hit","https://youtu.be/AAAAAAAAAAA"));resolver.cancel("cancelled-cache-hit");QTest::qWait(20);QCOMPARE(success.count(),2);
        resolver.invalidate("https://youtube.com/embed/AAAAAAAAAAA?start=5");QVERIFY(resolver.m_cache.isEmpty());
        QVERIFY(resolver.resolve("refresh","https://youtu.be/AAAAAAAAAAA"));QTRY_COMPARE_WITH_TIMEOUT(success.count(),3,3000);
        calls.seek(0);QCOMPARE(calls.readAll(),QByteArray("AAAAAAAAAAA\nAAAAAAAAAAA\n"));
        resolver.cancelAll();QVERIFY(resolver.m_cache.isEmpty());QVERIFY(resolver.m_running.isEmpty());
    }
    void identicalInFlightVideosShareOneProcessAndKeepIndependentCancellationAndOffsets() {
        QTemporaryDir directory;const auto program=fixture(directory);VideoResolver resolver(program);
        QSignalSpy success(&resolver,&VideoResolver::resolved),failure(&resolver,&VideoResolver::failed);
        QVERIFY(resolver.resolve("owner","https://youtu.be/BBBBBBBBBBB?t=1"));
        QVERIFY(resolver.resolve("retained","https://m.youtube.com/watch?v=BBBBBBBBBBB&t=12"));
        QVERIFY(resolver.resolve("another","https://youtube.com/embed/BBBBBBBBBBB?start=34"));
        QCOMPARE(resolver.m_jobs.size(),3);QCOMPARE(resolver.m_running.size(),1);QCOMPARE(resolver.findChildren<QProcess*>().size(),1);
        resolver.cancel("owner");QCOMPARE(resolver.m_jobs.size(),2);QCOMPARE(resolver.m_running.size(),1);
        QTRY_COMPARE_WITH_TIMEOUT(success.count(),2,3000);QCOMPARE(failure.count(),0);
        QHash<QString,int> offsets;for(const auto &event:success)offsets.insert(event[0].toString(),event[1].toMap().value("startSeconds").toInt());
        QCOMPARE(offsets.value("retained"),12);QCOMPARE(offsets.value("another"),34);QVERIFY(!offsets.contains("owner"));
        QFile calls(program+".calls");QVERIFY(calls.open(QIODevice::ReadOnly));QCOMPARE(calls.readAll(),QByteArray("BBBBBBBBBBB\n"));
        QVERIFY(resolver.m_jobs.isEmpty());QVERIFY(resolver.m_running.isEmpty());
    }
    void cachedUrlsExpireBeforeTheirSignaturesAndTheLruAndSubscriberCountsStayBounded() {
        QTemporaryDir directory;const auto program=fixture(directory);VideoResolver resolver(program);resolver.m_cacheLimit=2;
        const auto keyA=VideoResolver::cacheKey("https://youtu.be/AAAAAAAAAAA?t=2");
        const auto keyB=VideoResolver::cacheKey("https://youtu.be/BBBBBBBBBBB");
        const auto keyC=VideoResolver::cacheKey("https://youtu.be/CCCCCCCCCCC");
        const auto media=VideoResolver::parseResult(bytes(combined()));
        resolver.remember(keyA,media);resolver.remember(keyB,media);QCOMPARE(resolver.m_cache.size(),2);
        resolver.m_cache[keyA].lastUse=++resolver.m_cacheUse;resolver.remember(keyC,media);
        QVERIFY(resolver.m_cache.contains(keyA));QVERIFY(!resolver.m_cache.contains(keyB));QVERIFY(resolver.m_cache.contains(keyC));
        auto soon=media;soon.insert("sourceUrl","https://rr1---fixture.googlevideo.com/videoplayback?expire="+QString::number(QDateTime::currentSecsSinceEpoch()+30));
        resolver.remember(keyB,soon);QVERIFY(!resolver.m_cache.contains(keyB));
        soon.insert("sourceUrl","https://manifest.googlevideo.com/api/manifest/hls_variant/expire/"+QString::number(QDateTime::currentSecsSinceEpoch()+30)+"/index.m3u8");
        resolver.remember(keyB,soon);QVERIFY(!resolver.m_cache.contains(keyB));
        resolver.m_cache[keyA].expiresAt=0;QSignalSpy success(&resolver,&VideoResolver::resolved);
        QVERIFY(resolver.resolve("expired","https://youtu.be/AAAAAAAAAAA"));QTRY_COMPARE_WITH_TIMEOUT(success.count(),1,3000);
        QFile calls(program+".calls");QVERIFY(calls.open(QIODevice::ReadOnly));QCOMPARE(calls.readAll(),QByteArray("AAAAAAAAAAA\n"));
        resolver.cancelAll();QVERIFY(resolver.m_cache.isEmpty());
        for(int i=0;i<32;++i)QVERIFY(resolver.resolve("subscriber:"+QString::number(i),"https://youtu.be/BBBBBBBBBBB"));
        QSignalSpy failure(&resolver,&VideoResolver::failed);QVERIFY(!resolver.resolve("too-many","https://youtu.be/BBBBBBBBBBB"));QCOMPARE(failure.count(),1);
        QCOMPARE(resolver.m_running.size(),1);resolver.cancelAll();QVERIFY(resolver.m_jobs.isEmpty());QVERIFY(resolver.m_running.isEmpty());
    }
    void immediateCancellationFromTheStartedSignalDoesNotLaunchAChild() {
        QTemporaryDir directory;const auto program=fixture(directory);VideoResolver resolver(program);
        connect(&resolver,&VideoResolver::resolutionStarted,&resolver,[&resolver](const QString &id){resolver.cancel(id);});
        QSignalSpy success(&resolver,&VideoResolver::resolved),failure(&resolver,&VideoResolver::failed);
        QVERIFY(resolver.resolve("cancel-at-start","https://youtu.be/AAAAAAAAAAA"));QTest::qWait(20);
        QVERIFY(resolver.findChildren<QProcess*>().isEmpty());QVERIFY(resolver.m_jobs.isEmpty());QVERIFY(resolver.m_running.isEmpty());
        QCOMPARE(success.count(),0);QCOMPARE(failure.count(),0);
    }
};
QTEST_GUILESS_MAIN(VideoResolverTest)
#include "video_resolver_test.moc"
