#include "GifSearch.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QRegularExpression>
namespace {
bool safeHost(const QString &value,const QStringList &hosts) {
    const QUrl url(value,QUrl::StrictMode);
    return value.size()<=4096 && url.isValid() && url.scheme()=="https" && url.userInfo().isEmpty() && (url.port(-1)==-1 || url.port()==443) && hosts.contains(url.host()) && !QUrl::fromPercentEncoding(value.toUtf8()).contains(QRegularExpression("[\\x00-\\x1f\\x7f]"));
}
}
GifSearch::GifSearch(const QString &file,QObject *parent):QObject(parent),m_settings(file.isEmpty()?std::make_unique<QSettings>():std::make_unique<QSettings>(file,QSettings::IniFormat)) {
    for(const auto &value:m_settings->value("gifs/favorites").toList()) {
        const auto record=validatedRecord(value.toMap()); if(!record.isEmpty() && m_favorites.size()<200 && !isFavorite(record.value("url").toString()))m_favorites.append(record);
    }
}
bool GifSearch::allowedMedia(const QString &value) {
    return safeHost(value,{"static.klipy.com","media.tenor.com","c.tenor.com","media.giphy.com","i.giphy.com","cdn.discordapp.com","media.discordapp.net","images-ext-1.discordapp.net","images-ext-2.discordapp.net"});
}
QVariantMap GifSearch::validatedRecord(const QVariantMap &input) {
    const auto url=input.value("url").toString();
    if(!safeHost(url,{"klipy.com","www.klipy.com","tenor.com","www.tenor.com","giphy.com","www.giphy.com"}))return {};
    const auto preview=input.value("previewUrl",input.value("preview",input.value("src"))).toString();
    if(!allowedMedia(preview))return {};
    return {{"id",input.value("id").toString().left(128)},{"title",input.value("title").toString().left(180)},{"url",url},{"previewUrl",preview},
        {"width",qBound(1,input.value("width",320).toInt(),8192)},{"height",qBound(1,input.value("height",240).toInt(),8192)}};
}
void GifSearch::cancel(){++m_generation;if(m_reply){m_reply->abort();m_reply=nullptr;}m_busy=false;emit changed();}
void GifSearch::fetch(const QString &path,bool categories) {
    cancel();m_error.clear();m_busy=true;emit changed();const auto generation=m_generation;
    QNetworkRequest request{QUrl("https://discord.com/api/v9/gifs/"+path)};
    request.setRawHeader("User-Agent","Relay/0.1");request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);request.setTransferTimeout(10000);
    auto reply=m_network.get(request);m_reply=reply;reply->setReadBufferSize(1024*1024+1);
    auto bytes=std::make_shared<QByteArray>();
    connect(reply,&QIODevice::readyRead,reply,[reply,bytes]{*bytes+=reply->readAll();if(bytes->size()>1024*1024)reply->abort();});
    connect(reply,&QNetworkReply::finished,this,[this,reply,bytes,generation,categories] {
        reply->deleteLater();if(generation!=m_generation)return;
        m_reply=nullptr;m_busy=false;*bytes+=reply->readAll();
        const int status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if(reply->error()!=QNetworkReply::NoError || status!=200 || bytes->size()>1024*1024) {m_error=status==429?"GIF search is busy. Try again shortly.":"GIF search could not load. Try again.";emit changed();return;}
        QJsonParseError parse;const auto doc=QJsonDocument::fromJson(*bytes,&parse);
        if(parse.error!=QJsonParseError::NoError){m_error="GIF search returned an unreadable response.";emit changed();return;}
        if(categories){
            m_categories.clear();
            for(const auto &value:doc.object().value("categories").toArray()){
                if(m_categories.size()>=30)break;
                const auto item=value.toObject();const auto preview=item.value("src").toString();
                if(allowedMedia(preview))m_categories.append(QVariantMap{{"name",item.value("name").toString().left(80)},{"previewUrl",preview}});
            }
        }else{
            m_results.clear();for(const auto &value:doc.array()){if(m_results.size()>=40)break;const auto item=validatedRecord(value.toObject().toVariantMap());if(!item.isEmpty())m_results.append(item);}
        }
        emit changed();
    });
}
void GifSearch::search(const QString &query){const auto clean=query.trimmed().left(100);if(clean.isEmpty()){loadCategories();return;}QUrlQuery args;args.addQueryItem("q",clean);args.addQueryItem("media_format","gif");args.addQueryItem("provider","klipy");args.addQueryItem("limit","20");fetch("search?"+args.query(QUrl::FullyEncoded),false);}
void GifSearch::trending(){fetch("trending-gifs?media_format=gif&provider=klipy&limit=20",false);}
void GifSearch::loadCategories(){if(m_categories.isEmpty())fetch("trending?media_format=gif&provider=klipy",true);}
void GifSearch::showFavorites(){cancel();m_error.clear();m_results=m_favorites;emit changed();}
bool GifSearch::isFavorite(const QString &url)const{for(const auto &value:m_favorites)if(value.toMap().value("url").toString()==url)return true;return false;}
void GifSearch::setFavorite(const QVariantMap &input,bool favorite){
    const auto record=validatedRecord(input);if(record.isEmpty())return;const auto url=record.value("url").toString();
    for(int i=m_favorites.size()-1;i>=0;--i)if(m_favorites[i].toMap().value("url").toString()==url)m_favorites.removeAt(i);
    if(favorite){m_favorites.prepend(record);while(m_favorites.size()>200)m_favorites.removeLast();}
    m_settings->setValue("gifs/favorites",m_favorites);m_settings->sync();emit changed();
}
