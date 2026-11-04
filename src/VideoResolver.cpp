#include "VideoResolver.h"
#include "InlineMedia.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#ifdef Q_OS_UNIX
#include <signal.h>
#endif

struct VideoResolver::Job {
    QString page;
    QHash<QString,int> requests;
    QPointer<QProcess> process;
    QPointer<QTimer> timer;
    QByteArray output,error;
};
namespace {
constexpr qsizetype OutputLimit=4*1024*1024,ErrorLimit=16*1024;
void stopProcess(QProcess *process) {
#ifdef Q_OS_UNIX
    // yt-dlp's JavaScript runtime belongs to this new session too.
    if(process->processId()>0)::kill(-pid_t(process->processId()),SIGKILL);
#endif
    process->kill();
}
QString boundedText(QString value,int limit) {value.remove(QRegularExpression("[\\x00-\\x1f\\x7f]"));return value.left(limit);}
}
QString VideoResolver::executablePath() {
    const auto configured=qEnvironmentVariable("RELAY_YTDLP_PATH");
    if(!configured.isEmpty())return configured;
    #ifdef Q_OS_WIN
    const QStringList locations{"/yt-dlp.exe","/../tools/yt-dlp.exe","/../libexec/relay/yt-dlp.exe"};
#else
    const QStringList locations{"/../tools/yt-dlp","/../libexec/relay/yt-dlp"};
#endif
    for(const auto &relative:locations) {
        const QFileInfo bundled(QCoreApplication::applicationDirPath()+relative);
        if(bundled.isFile() && bundled.isExecutable())return bundled.absoluteFilePath();
    }
    return QStandardPaths::findExecutable("yt-dlp");
}
VideoResolver::VideoResolver(QObject *parent):VideoResolver(executablePath(),parent) {}
VideoResolver::VideoResolver(const QString &executable,QObject *parent):QObject(parent) {
    const QFileInfo file(executable);
    if(file.isAbsolute() && file.isFile() && file.isExecutable())m_executable=file.absoluteFilePath();
}
VideoResolver::~VideoResolver() {
    cancelAll();
    // Reap already-killed children before QObject destroys their QProcess objects.
    for(auto *process:findChildren<QProcess*>())if(process->state()!=QProcess::NotRunning){stopProcess(process);process->waitForFinished(100);}
}
QStringList VideoResolver::arguments(const QString &page) {
    // JSON metadata only; no shell, browser cookies,
    // user config, external plugins, remote runtime components, or media download.
    // Playback uses progressive media or HLS, so DASH, translated subtitles,
    // extra initial-data requests and download-format probes only add latency.
    // Keep the normal webpage/player clients and JavaScript extraction intact.
    return {"--ignore-config","--no-plugin-dirs","--no-remote-components","--no-cache-dir","--no-playlist",
        "--skip-download","--no-progress","--no-colors","--quiet","--socket-timeout","10","--retries","0","--extractor-retries","0",
        "--no-check-formats","--use-extractors","youtube","--extractor-args","youtube:skip=dash,translated_subs;player_skip=initial_data",
        "--js-runtimes","node","--format",
        "best[height<=1080][protocol=https][ext=mp4][vcodec^=avc1][acodec!=none]/best[height<=1080][protocol=https][ext=webm][vcodec!=none][acodec!=none]/bestvideo[height<=1080]+bestaudio",
        "--dump-single-json","--",page};
}
QVariantMap VideoResolver::parseResult(const QByteArray &output) {
    if(output.isEmpty() || output.size()>OutputLimit)return {};
    QJsonParseError error;const auto value=QJsonDocument::fromJson(output.trimmed(),&error);
    if(error.error!=QJsonParseError::NoError || !value.isObject())return {};
    const auto object=value.object();
    auto selected=object;QString source;bool adaptive=false;
    auto combined=[](const QJsonObject &format){
        const auto url=InlineMedia::directMedia(format.value("url").toString(),true);
        const auto video=format.value("vcodec").toString(),audio=format.value("acodec").toString();
        const int height=format.value("height").toInt(),width=format.value("width").toInt();
        return !url.isEmpty() && QUrl(url).host().endsWith(".googlevideo.com") && !video.isEmpty() && video!="none" &&
            !audio.isEmpty() && audio!="none" && height>0 && height<=1080 && width>0 && width<=8192 &&
            !format.value("has_drm").toBool() && QStringList{"mp4","webm"}.contains(format.value("ext").toString());
    };
    if(combined(object))source=InlineMedia::directMedia(object.value("url").toString(),true);
    if(source.isEmpty()) {
        int count=0,bestHeight=0;
        for(const auto &entry:object.value("formats").toArray()) {
            if(++count>500)break;
            const auto format=entry.toObject();
            if(combined(format) && format.value("height").toInt()>bestHeight){selected=format;source=InlineMedia::directMedia(format.value("url").toString(),true);bestHeight=format.value("height").toInt();}
        }
    }
    {
        // Prefer a higher-quality HLS master over a low-resolution muxed file.
        // The master carries synchronized audio/video; format.url may not.
        int count=0,bestHeight=source.isEmpty()?0:selected.value("height").toInt();
        for(const auto &entry:object.value("formats").toArray()) {
            if(++count>500)break;
            const auto format=entry.toObject();
            const auto manifest=InlineMedia::directMedia(format.value("manifest_url").toString(),true);
            const auto url=QUrl(manifest);const int height=format.value("height").toInt(),width=format.value("width").toInt();
            if(url.host()!="manifest.googlevideo.com" || !url.path().startsWith("/api/manifest/hls_variant/") || height<1 || height>4320 || height<=bestHeight || width<1 || width>8192 || format.value("has_drm").toBool())continue;
            const auto codec=format.value("vcodec").toString();
            if(!codec.startsWith("avc1") && !codec.startsWith("vp9") && !codec.startsWith("vp09") && !codec.startsWith("av01"))continue;
            source=manifest;selected=format;adaptive=true;bestHeight=height;
        }
    }
    const int width=selected.value("width").toInt(),height=selected.value("height").toInt();
    if(source.isEmpty() || width<1 || width>8192 || height<1 || height>(adaptive?4320:1080))return {};
    return {{"sourceUrl",source},{"title",boundedText(object.value("title").toString(),256)},
        {"width",width},{"height",height},{"mimeType",adaptive?"application/vnd.apple.mpegurl":"video/"+selected.value("ext").toString()},
        {"hasAudio",true},{"adaptive",adaptive},{"duration",qBound(0.0,object.value("duration").toDouble(),604800.0)},{"posterUrl",QString{}}};
}
QString VideoResolver::failureText(const QByteArray &error) {
    const auto lower=error.toLower();
    if(lower.contains("sign in") || lower.contains("sign-in") || lower.contains("confirm your age") || lower.contains("confirm you") || lower.contains("captcha"))
        return "YouTube requires sign-in or verification for this video. Open the original link in your browser.";
    if(lower.contains("private video") || lower.contains("video unavailable") || lower.contains("not available") || lower.contains("removed") || lower.contains("copyright"))
        return "This video is unavailable or restricted. Open the original link to check its availability.";
    if(lower.contains("requested format"))return "This video has no compatible combined audio/video stream. Open the original link in your browser.";
    return "The video could not be prepared for native playback. Retry or open the original link in your browser.";
}
void VideoResolver::finish(const std::shared_ptr<Job> &job,const QVariantMap &media,const QString &error) {
    if(job->requests.isEmpty())return;
    const auto requests=job->requests;job->requests.clear();
    for(auto it=requests.cbegin();it!=requests.cend();++it)if(m_jobs.value(it.key())==job)m_jobs.remove(it.key());
    if(!job->page.isEmpty() && m_running.value(job->page)==job)m_running.remove(job->page);
    if(job->timer){job->timer->stop();job->timer->deleteLater();}
    if(job->process){
        auto *process=job->process.data();disconnect(process,nullptr,this,nullptr);
        if(process->state()!=QProcess::NotRunning){connect(process,&QProcess::finished,process,&QObject::deleteLater);stopProcess(process);}
        else process->deleteLater();
    }
    if(error.isEmpty() && !job->page.isEmpty())remember(job->page,media);
    for(auto it=requests.cbegin();it!=requests.cend();++it){
        if(!error.isEmpty())emit failed(it.key(),error);
        else {auto result=media;result.insert("startSeconds",it.value());emit resolved(it.key(),result);}
    }
}
void VideoResolver::cancel(const QString &id) {
    const auto job=m_jobs.take(id);if(!job)return;
    job->requests.remove(id);if(!job->requests.isEmpty())return;
    if(!job->page.isEmpty() && m_running.value(job->page)==job)m_running.remove(job->page);
    if(job->timer){job->timer->stop();job->timer->deleteLater();}
    if(job->process){
        auto *process=job->process.data();disconnect(process,nullptr,this,nullptr);
        if(process->state()!=QProcess::NotRunning){connect(process,&QProcess::finished,process,&QObject::deleteLater);stopProcess(process);}
        else process->deleteLater();
    }
}
void VideoResolver::cancelAll() {for(const auto &id:m_jobs.keys())cancel(id);m_cache.clear();}
QString VideoResolver::cacheKey(const QString &url){
    const auto id=InlineMedia::youtubeId(url);
    return id.isEmpty()?QString{}:"https://www.youtube.com/watch?v="+id;
}
void VideoResolver::invalidate(const QString &url){m_cache.remove(cacheKey(url));}
void VideoResolver::remember(const QString &page,const QVariantMap &media){
    const auto now=QDateTime::currentMSecsSinceEpoch();qint64 expiresAt=now+m_cacheTtlMs;
    const QUrl source(media.value("sourceUrl").toString());
    bool ok=false;auto expiry=QUrlQuery(source).queryItemValue("expire").toLongLong(&ok);
    if(!ok){
        const auto match=QRegularExpression("/expire/(\\d{1,12})(?:/|$)").match(source.path());
        expiry=match.captured(1).toLongLong(&ok);
    }
    // Signed URLs must be refreshed before their upstream expiry. Unknown
    // lifetimes get a short in-memory TTL; no URLs or titles are written to disk.
    if(ok){if(expiry<=0 || expiry>=1000000000000LL)return;expiresAt=qMin(expiresAt,expiry*1000-60000);}
    if(expiresAt<=now || media.isEmpty() || m_cacheLimit<1)return;
    for(auto it=m_cache.begin();it!=m_cache.end();)if(it->expiresAt<=now)it=m_cache.erase(it);else ++it;
    if(!m_cache.contains(page) && m_cache.size()>=m_cacheLimit){
        auto oldest=m_cache.begin();
        for(auto it=m_cache.begin();it!=m_cache.end();++it)if(it->lastUse<oldest->lastUse)oldest=it;
        m_cache.erase(oldest);
    }
    auto result=media;result.remove("startSeconds");m_cache.insert(page,{result,expiresAt,++m_cacheUse});
}
bool VideoResolver::resolve(const QString &id,const QString &raw) {
    if(!QRegularExpression("^[A-Za-z0-9_.:-]{1,128}$").match(id).hasMatch())return false;
    cancel(id);
    const auto direct=InlineMedia::directMedia(raw),page=cacheKey(raw);
    if(direct.isEmpty() && page.isEmpty()){emit failed(id,"This link is not a supported native video source.");return false;}
    if(direct.isEmpty() && !available()){emit failed(id,"YouTube playback needs the bundled video resolver. Open the original link or reinstall Relay's tools.");return false;}
    if(m_jobs.size()>=32){emit failed(id,"Too many videos are being prepared. Try again when one finishes.");return false;}
    const int offset=InlineMedia::startSeconds(raw);
    if(direct.isEmpty()){
        const auto cached=m_cache.find(page);
        if(cached!=m_cache.end()){
            if(cached->expiresAt>QDateTime::currentMSecsSinceEpoch()){
                cached->lastUse=++m_cacheUse;
                const auto media=cached->media;
                auto job=std::make_shared<Job>();job->requests.insert(id,offset);m_jobs.insert(id,job);emit resolutionStarted(id);
                QTimer::singleShot(0,this,[this,job,media]{finish(job,media,{});});return true;
            }
            m_cache.erase(cached);
        }
        if(const auto running=m_running.value(page)){
            running->requests.insert(id,offset);m_jobs.insert(id,running);emit resolutionStarted(id);return true;
        }
        if(m_running.size()>=2){emit failed(id,"Two videos are already being prepared. Try again when one finishes.");return false;}
    }
    auto job=std::make_shared<Job>();job->requests.insert(id,offset);m_jobs.insert(id,job);
    if(direct.isEmpty()){job->page=page;m_running.insert(page,job);}
    emit resolutionStarted(id);
    if(job->requests.isEmpty())return true;
    if(!direct.isEmpty()) {
        QTimer::singleShot(0,this,[this,job,direct]{
            const auto extension=QUrl(direct).fileName().section('.',-1).toLower();
            finish(job,{{"sourceUrl",direct},{"title",QString{}},{"width",0},{"height",0},{"mimeType",extension=="webm"?"video/webm":extension=="gif"?"image/gif":"video/mp4"},{"hasAudio",extension!="gif"},{"posterUrl",QString{}}},{});
        });return true;
    }
    auto *process=new QProcess(this);job->process=process;
#ifdef Q_OS_UNIX
    process->setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
#endif
    process->setProcessChannelMode(QProcess::SeparateChannels);
    auto environment=QProcessEnvironment::systemEnvironment();environment.insert("PYTHONNOUSERSITE","1");environment.remove("PYTHONPATH");process->setProcessEnvironment(environment);
    auto *timer=new QTimer(this);job->timer=timer;timer->setSingleShot(true);
    connect(timer,&QTimer::timeout,this,[this,job]{finish(job,{},"Preparing the video timed out. Retry or open the original link in your browser.");});
    connect(process,&QProcess::readyReadStandardOutput,this,[this,job]{
        if(job->requests.isEmpty() || !job->process)return;
        job->output+=job->process->readAllStandardOutput();if(job->output.size()>OutputLimit)finish(job,{},"The video resolver returned too much data.");
    });
    connect(process,&QProcess::readyReadStandardError,this,[this,job]{
        if(job->requests.isEmpty() || !job->process)return;
        const auto bytes=job->process->readAllStandardError();if(job->error.size()+bytes.size()>ErrorLimit){finish(job,{},"The video resolver returned too much diagnostic data.");return;}job->error+=bytes;
    });
    connect(process,&QProcess::errorOccurred,this,[this,job](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish(job,{},"The native video resolver could not start. Open the original link or reinstall Relay's tools.");});
    connect(process,&QProcess::finished,this,[this,job](int code,QProcess::ExitStatus status){
        if(job->requests.isEmpty())return;
        if(code!=0 || status!=QProcess::NormalExit){finish(job,{},failureText(job->error));return;}
        auto media=parseResult(job->output);finish(job,media,media.isEmpty()?"The video resolver did not return a compatible, trusted media stream.":QString{});
    });
    timer->start(m_timeoutMs);process->start(m_executable,arguments(page),QIODevice::ReadOnly);return true;
}
