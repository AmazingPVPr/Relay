#include "ImageEditor.h"
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QImageReader>
#include <QBuffer>
#include <QPainter>
#include <QGuiApplication>
#include <QClipboard>
#include <QSaveFile>
#include <QFileInfo>
#include <QTransform>
#include <QUuid>
#include <cmath>
namespace {constexpr qint64 MaxBytes=32*1024*1024; constexpr qint64 MaxPixels=16*1024*1024;}
ImageEditor::ImageEditor(QQuickItem *parent):QQuickPaintedItem(parent) {setAntialiasing(true);}
bool ImageEditor::allowedSource(const QUrl &url) {
    const auto host=url.host().toLower();
    return url.isValid() && url.scheme()=="https" && url.userInfo().isEmpty() && url.port(443)==443
        && (host=="cdn.discordapp.com" || host=="media.discordapp.net" || host=="images-ext-1.discordapp.net" || host=="images-ext-2.discordapp.net");
}
QImage ImageEditor::decode(const QByteArray &bytes) {
    if(bytes.isEmpty() || bytes.size()>MaxBytes)return {};
    QBuffer buffer;buffer.setData(bytes);buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);reader.setAutoTransform(true);
    const auto size=reader.size();
    if(!size.isValid() || size.width()>16384 || size.height()>16384 || qint64(size.width())*size.height()>MaxPixels)return {};
    return reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
}
void ImageEditor::setSource(const QUrl &source) {
    if(m_source==source)return;
    if(m_reply){m_reply->disconnect(this);m_reply->abort();m_reply->deleteLater();m_reply=nullptr;}
    m_source=source;m_image={};m_original={};m_before={};m_undo.clear();m_redo.clear();
    if(source.isEmpty()){refresh();return;}
    if(!allowedSource(source)){refresh("This image source cannot be edited.");return;}
    refresh("Loading full-size image…");
    QNetworkRequest request(source);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    auto *reply=m_network.get(request);m_reply=reply;
    connect(reply,&QNetworkReply::readyRead,this,[this,reply]{if(reply->bytesAvailable()>MaxBytes)reply->abort();});
    connect(reply,&QNetworkReply::metaDataChanged,this,[reply]{if(reply->header(QNetworkRequest::ContentLengthHeader).toLongLong()>MaxBytes)reply->abort();});
    connect(reply,&QNetworkReply::finished,this,[this,reply]{
        const bool ok=reply->error()==QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200;
        if(ok)m_image=decode(reply->read(MaxBytes+1));
        m_original=m_image;m_reply=nullptr;reply->deleteLater();
        refresh(ready()?"Drag on the image to draw. Crop: drag to select an area.":"Cannot edit this image. Try a raster image up to 16 megapixels and 32 MB.");
    });
}
QRectF ImageEditor::imageRect() const {
    if(!ready())return {};
    auto fitted=QSizeF(m_image.size());fitted.scale(QSizeF(width(),height()),Qt::KeepAspectRatio);
    return QRectF(QPointF((width()-fitted.width())/2,(height()-fitted.height())/2),fitted);
}
void ImageEditor::geometryChange(const QRectF &a,const QRectF &b){QQuickPaintedItem::geometryChange(a,b);emit changed();}
void ImageEditor::paint(QPainter *painter){if(ready()){painter->setRenderHint(QPainter::SmoothPixmapTransform);painter->drawImage(imageRect(),m_image);}}
void ImageEditor::refresh(QString status){m_status=status;update();emit changed();}
QPointF ImageEditor::imagePoint(QPointF p) const {
    const auto rect=imageRect();
    return {qBound(0.,(p.x()-rect.x())*m_image.width()/rect.width(),double(m_image.width())),qBound(0.,(p.y()-rect.y())*m_image.height()/rect.height(),double(m_image.height()))};
}
void ImageEditor::begin(QPointF p,QString tool,QColor color,qreal width) {
    if(!ready() || !imageRect().contains(p) || !std::isfinite(width))return;
    cancelStroke();m_before=m_image;m_tool=tool;m_color=color.isValid()?color:QColor(Qt::red);m_penWidth=qBound(1.,width,100.);
    m_start=m_last=imagePoint(p);move(p);
}
void ImageEditor::move(QPointF p) {
    if(m_before.isNull() || !std::isfinite(p.x()) || !std::isfinite(p.y()))return;
    const auto end=imagePoint(p);
    if(m_tool=="crop"){m_last=end;return;}
    if(m_tool!="pen")m_image=m_before;
    {QPainter painter(&m_image);painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(m_color,m_penWidth,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
    if(m_tool=="pen") {if(end==m_last)painter.drawPoint(end);else painter.drawLine(m_last,end);}
    else if(m_tool=="line")painter.drawLine(m_start,end);
    else if(m_tool=="rectangle")painter.drawRect(QRectF(m_start,end).normalized());
    else if(m_tool=="ellipse")painter.drawEllipse(QRectF(m_start,end).normalized());}
    m_last=end;update();
}
void ImageEditor::finish(){
    if(m_before.isNull())return;
    if(m_tool=="crop") {
        const auto rect=QRectF(m_start,m_last).normalized().toAlignedRect().intersected(m_image.rect());
        if(rect.width()>=2 && rect.height()>=2)m_image=m_image.copy(rect);
    }
    auto previous=m_before;m_before={};if(m_image!=previous)commit(previous);else refresh();
}
void ImageEditor::cancelStroke(){if(!m_before.isNull()){m_image=m_before;m_before={};refresh();}}
void ImageEditor::trimHistory(){
    qint64 bytes=0;for(const auto &image:m_undo)bytes+=image.sizeInBytes();
    while(m_undo.size()>1 && (bytes>128*1024*1024 || m_undo.size()>30)){bytes-=m_undo.first().sizeInBytes();m_undo.removeFirst();}
}
void ImageEditor::commit(const QImage &previous){m_undo.append(previous);m_redo.clear();trimHistory();refresh();}
void ImageEditor::undo(){cancelStroke();if(canUndo()){m_redo.append(m_image);m_image=m_undo.takeLast();refresh();}}
void ImageEditor::redo(){cancelStroke();if(canRedo()){m_undo.append(m_image);m_image=m_redo.takeLast();trimHistory();refresh();}}
void ImageEditor::reset(){cancelStroke();if(ready() && modified()){auto previous=m_image;m_image=m_original;commit(previous);}}
void ImageEditor::rotate(){cancelStroke();if(ready()){auto previous=m_image;m_image=m_image.transformed(QTransform().rotate(90));commit(previous);}}
void ImageEditor::flip(){cancelStroke();if(ready()){auto previous=m_image;m_image=m_image.mirrored(true,false);commit(previous);}}
bool ImageEditor::copy(){if(!ready())return false;QGuiApplication::clipboard()->setImage(m_image);refresh("Edited image copied to clipboard.");return true;}
bool ImageEditor::saveAs(QUrl destination){
    if(!ready() || !destination.isLocalFile())return false;
    QString path=destination.toLocalFile();if(!path.endsWith(".png",Qt::CaseInsensitive))path+=".png";
    QSaveFile file(path);
    const bool ok=file.open(QIODevice::WriteOnly) && m_image.save(&file,"PNG") && file.commit();
    refresh(ok?"Edited image saved.":"Could not save the image. Choose another location.");return ok;
}
QUrl ImageEditor::stage(){
    if(!ready() || !m_staging.isValid())return {};
    const auto path=m_staging.filePath("edited-image.png");
    if(!m_image.save(path,"PNG")){refresh("Could not prepare edited image.");return {};}
    return QUrl::fromLocalFile(path);
}
