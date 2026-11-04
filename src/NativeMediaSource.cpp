#include "NativeMediaSource.h"
#include "InlineMedia.h"
#include <QBuffer>
#include <QMediaPlayer>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>
#include <cmath>
namespace {constexpr qsizetype MaximumManifest=1024*1024;}
NativeMediaSource::NativeMediaSource(QObject *parent):QObject(parent),m_network(this){}
NativeMediaSource::~NativeMediaSource(){cancel();}
bool NativeMediaSource::trusted(const QUrl &url) const {
    if(!m_fixtureOrigin.isEmpty())return url.scheme()==m_fixtureOrigin.scheme() && url.host()=="127.0.0.1" && url.port()==m_fixtureOrigin.port() && url.userInfo().isEmpty();
    return !InlineMedia::httpsUrl(url.toString()).isEmpty() && url.host().endsWith(".googlevideo.com");
}
QHash<QString,QString> NativeMediaSource::attributes(const QString &text){
    QHash<QString,QString> result;qsizetype at=0;
    while(at<text.size() && result.size()<64){
        while(at<text.size() && (text[at]==',' || text[at].isSpace()))++at;
        const auto start=at;while(at<text.size() && (text[at].isUpper() || text[at].isDigit() || text[at]=='-'))++at;
        if(at==start || at>=text.size() || text[at]!='=')return {};
        const auto key=text.mid(start,at-start);++at;QString value;
        if(at<text.size() && text[at]=='"'){++at;const auto end=text.indexOf('"',at);if(end<0)return {};value=text.mid(at,end-at);at=end+1;}
        else {auto end=text.indexOf(',',at);if(end<0)end=text.size();value=text.mid(at,end-at);at=end;}
        if(result.contains(key) || (at<text.size() && text[at]!=','))return {};
        result.insert(key,value);
    }
    return at>=text.size()?result:QHash<QString,QString>{};
}
bool NativeMediaSource::parse(const QByteArray &bytes){
    m_common.clear();m_variants.clear();
    if(bytes.isEmpty() || bytes.size()>MaximumManifest || !bytes.startsWith("#EXTM3U"))return false;
    const auto lines=QString::fromUtf8(bytes).split('\n');
    if(lines.size()>4096)return false;
    const QRegularExpression dimensions("^(\\d{1,5})x(\\d{1,5})$");
    for(qsizetype i=0;i<lines.size();++i){
        auto line=lines[i].trimmed();if(line.isEmpty())continue;if(line.size()>16384)return false;
        if(line.startsWith("#EXT-X-STREAM-INF:")){
            if(m_variants.size()>=64 || i+1>=lines.size())return false;
            const auto attrs=attributes(line.mid(18));const auto match=dimensions.match(attrs.value("RESOLUTION"));
            const int width=match.captured(1).toInt(),height=match.captured(2).toInt();
            const auto uri=m_source.resolved(QUrl(lines[++i].trimmed()));
            if(!trusted(uri) || width<1 || width>8192 || height<1 || height>4320)return false;
            const auto codec=attrs.value("CODECS");
            if(!codec.contains("avc1") && !codec.contains("vp09") && !codec.contains("vp9") && !codec.contains("av01"))continue;
            bool okay=false;double fps=attrs.value("FRAME-RATE").toDouble(&okay);if(!okay || !std::isfinite(fps) || fps<0 || fps>240)fps=0;
            m_variants.append({line,uri.toString(QUrl::FullyEncoded),codec,width,height,fps});
        }else if(line.startsWith("#EXT-X-I-FRAME-STREAM-INF:"))continue;
        else if(line.startsWith("#EXT-X-MEDIA:")){
            auto attrs=attributes(line.mid(13));if(attrs.isEmpty())return false;
            if(attrs.contains("URI")){
                const auto raw=attrs.value("URI");const auto uri=m_source.resolved(QUrl(raw));if(!trusted(uri))return false;
                line.replace("URI=\""+raw+"\"","URI=\""+uri.toString(QUrl::FullyEncoded)+"\"");
            }
            m_common.append(line);
        }else if(line.startsWith("#")){
            // Unknown manifest directives with additional URI references need
            // the backend's normal parser, rather than a partial rewrite.
            if(line.contains("URI="))return false;
            m_common.append(line);
        }else return false;
    }
    if(m_variants.isEmpty() || m_common.join('\n').size()>65536)return false;
    std::stable_sort(m_variants.begin(),m_variants.end(),[](const Variant&a,const Variant&b){return qint64(a.width)*a.height>qint64(b.width)*b.height || (qint64(a.width)*a.height==qint64(b.width)*b.height && a.fps>b.fps);});
    m_selected=-1;
    for(int i=0;i<m_variants.size();++i)if(m_variants[i].height<=1080){m_selected=i;break;}
    if(m_selected<0)m_selected=m_variants.size()-1;
    return true;
}
QVariantList NativeMediaSource::qualityOptions() const {
    QVariantList result;for(int i=0;i<m_variants.size();++i){const auto &variant=m_variants[i];auto codec=variant.codec.section(',',0,0);const auto label=QString::number(variant.height)+"p"+(variant.fps>30?" "+QString::number(qRound(variant.fps)):QString{})+(codec.isEmpty()?QString{}:" · "+codec);result.append(QVariantMap{{"index",i},{"width",variant.width},{"height",variant.height},{"fps",variant.fps},{"label",label}});}return result;
}
QByteArray NativeMediaSource::manifestFor(int index) const {if(index<0 || index>=m_variants.size())return {};const auto &variant=m_variants[index];return (m_common.join('\n')+'\n'+variant.stream+'\n'+variant.uri+'\n').toUtf8();}
bool NativeMediaSource::open(QObject *object,const QString &source){
    auto *player=qobject_cast<QMediaPlayer*>(object);const auto url=QUrl(source);
    if(!player || !trusted(url) || (m_fixtureOrigin.isEmpty() && (url.host()!="manifest.googlevideo.com" || !url.path().startsWith("/api/manifest/hls_variant/"))))return false;
    // Each playback session owns one helper. Reopening another source uses the
    // ordinary path, so a retained QIODevice can never be mutated under a reader.
    if(!m_source.isEmpty())return false;
    cancel();m_player=player;m_source=url;const auto generation=++m_generation;
    QNetworkRequest request(url);request.setTransferTimeout(2000);request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,QNetworkRequest::Manual);request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,QNetworkRequest::Manual);request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,QNetworkRequest::Manual);
    auto *reply=m_network.get(request);m_reply=reply;reply->setReadBufferSize(MaximumManifest+1);
    connect(reply,&QNetworkReply::downloadProgress,this,[reply](qint64 received,qint64 total){if(received>MaximumManifest || total>MaximumManifest){reply->setProperty("tooLarge",true);reply->abort();}});
    connect(reply,&QNetworkReply::finished,this,[this,reply,generation]{
        reply->deleteLater();if(m_reply==reply)m_reply.clear();if(generation!=m_generation || !m_player)return;
        const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();const auto bytes=reply->read(MaximumManifest+1);
        if(status!=200 || reply->error()!=QNetworkReply::NoError || reply->property("tooLarge").toBool() || !parse(bytes)){fallback(generation);return;}
        m_active=true;const auto index=m_selected;m_selected=-1;selectQuality(index);
    });
    QTimer::singleShot(2500,this,[this,generation]{if(generation==m_generation && m_reply)fallback(generation);});return true;
}
bool NativeMediaSource::selectQuality(int index){
    if(!m_active || !m_player || index<0 || index>=m_variants.size())return false;
    if(m_selected==index && m_buffers.value(index))return true;
    auto *buffer=m_buffers.value(index).data();
    if(!buffer){buffer=new QBuffer(m_player);buffer->setData(manifestFor(index));buffer->open(QIODevice::ReadOnly);m_buffers.insert(index,buffer);}
    else buffer->seek(0);
    m_selected=index;m_player->setSourceDevice(buffer,m_source);emit changed();return true;
}
void NativeMediaSource::fallback(quint64 generation){
    if(generation!=m_generation || !m_player)return;
    if(m_reply){auto *reply=m_reply.data();m_reply.clear();reply->disconnect(this);reply->abort();reply->deleteLater();}
    m_active=false;m_variants.clear();m_common.clear();m_selected=-1;emit changed();m_player->setSource(m_source);
}
void NativeMediaSource::cancel(){
    ++m_generation;if(m_reply){auto *reply=m_reply.data();m_reply.clear();reply->disconnect(this);reply->abort();reply->deleteLater();}
}
