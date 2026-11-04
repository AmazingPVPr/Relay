#pragma once
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

// Shared, side-effect-free policy for message metadata and click-to-play input.
namespace InlineMedia {
inline QUrl httpsUrl(const QString &raw) {
    if(raw.isEmpty() || raw.size()>16384 || raw.contains(QRegularExpression("[\\x00-\\x20\\x7f]")))return {};
    const QUrl url(raw,QUrl::StrictMode);
    if(!url.isValid() || url.scheme()!="https" || url.host().isEmpty() || !url.userInfo().isEmpty() || (url.port()!=-1 && url.port()!=443))return {};
    return url;
}
inline bool discordHost(const QString &host) {
    return host=="cdn.discordapp.com" || host=="media.discordapp.net" || host=="images-ext-1.discordapp.net" || host=="images-ext-2.discordapp.net";
}
inline QString discordMedia(const QString &raw) {
    const auto url=httpsUrl(raw);return !url.isEmpty() && discordHost(url.host().toLower())?url.toString(QUrl::FullyEncoded):QString{};
}
inline QString directMedia(const QString &raw,bool resolvedYouTube=false) {
    const auto url=httpsUrl(raw);if(url.isEmpty())return {};
    const auto host=url.host().toLower();
    const bool provider=host=="media.tenor.com" || host=="c.tenor.com" || host=="media.giphy.com" || host=="i.giphy.com" || host=="static.klipy.com" || host=="static1.klipy.com" || host=="static2.klipy.com" || QRegularExpression("^media[0-4]\\.giphy\\.com$").match(host).hasMatch();
    const bool youtube=resolvedYouTube && host.endsWith(".googlevideo.com") && (url.path()=="/videoplayback" || (host=="manifest.googlevideo.com" && url.path().startsWith("/api/manifest/hls_variant/")));
    return discordHost(host) || provider || youtube?url.toString(QUrl::FullyEncoded):QString{};
}
inline int startSeconds(const QString &raw) {
    const auto url=httpsUrl(raw);if(url.isEmpty())return 0;
    const QUrlQuery query(url);QString value=query.queryItemValue("t");
    if(value.isEmpty())value=query.queryItemValue("start");
    if(value.isEmpty() && url.fragment().startsWith("t="))value=url.fragment().mid(2);
    bool ok=false;const auto seconds=value.toInt(&ok);if(ok)return qBound(0,seconds,604800);
    const auto match=QRegularExpression("^(?:(\\d{1,5})h)?(?:(\\d{1,5})m)?(?:(\\d{1,6})s)?$").match(value);
    return match.hasMatch()?qBound(0,match.captured(1).toInt()*3600+match.captured(2).toInt()*60+match.captured(3).toInt(),604800):0;
}
inline QString youtubePage(const QString &raw) {
    const auto url=httpsUrl(raw);if(url.isEmpty())return {};
    const auto host=url.host().toLower();QString id;
    if(host=="youtu.be")id=url.path().mid(1);
    else if(host=="youtube.com" || host=="www.youtube.com" || host=="m.youtube.com" || host=="music.youtube.com" || host=="www.youtube-nocookie.com" || host=="youtube-nocookie.com") {
        if(url.path()=="/watch")id=QUrlQuery(url).queryItemValue("v");
        else {
            const auto parts=url.path().split('/',Qt::SkipEmptyParts);
            if(parts.size()==2 && QStringList{"embed","shorts","live","v"}.contains(parts[0]))id=parts[1];
        }
    }
    if(!QRegularExpression("^[A-Za-z0-9_-]{11}$").match(id).hasMatch())return {};
    const int offset=startSeconds(raw);
    return "https://www.youtube.com/watch?v="+id+(offset?"&t="+QString::number(offset):QString{});
}
inline QString youtubeId(const QString &raw) {
    const auto page=youtubePage(raw);
    return page.isEmpty()?QString{}:QUrlQuery(QUrl(page)).queryItemValue("v");
}
inline bool videoFile(const QString &raw,const QString &mime={}) {
    if(mime.startsWith("video/",Qt::CaseInsensitive))return true;
    return QStringList{"mp4","webm","mov","mkv","m4v"}.contains(QUrl(raw).fileName().section('.',-1).toLower());
}
inline bool gifFile(const QString &raw,const QString &mime={}) {
    return mime.compare("image/gif",Qt::CaseInsensitive)==0 || QUrl(raw).fileName().endsWith(".gif",Qt::CaseInsensitive);
}
}
