#include "CrispImage.h"
#include <QBuffer>
#include <QCache>
#include <QDateTime>
#include <QPointer>
#include <QQueue>
#include <QFutureWatcher>
#include <QImageReader>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QThreadPool>
#include <QtConcurrentRun>
#include <QUrlQuery>
#include <cmath>

bool CrispImage::allowedSource(const QString &source) {
    const QUrl url(source, QUrl::StrictMode);
    const QStringList hosts{"cdn.discordapp.com", "media.discordapp.net", "images-ext-1.discordapp.net", "images-ext-2.discordapp.net"};
    return source.size() <= 4096 && url.isValid() && url.scheme() == "https" && url.userInfo().isEmpty() && hosts.contains(url.host()) && (url.port(-1) == -1 || url.port() == 443);
}
QImage CrispImage::decodeForDisplay(QByteArray bytes, int size) {
    QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer); reader.setAutoTransform(true);
    const auto natural = reader.size();
    if (natural.isEmpty() || natural.width() > 8192 || natural.height() > 8192 || qint64(natural.width())*natural.height() > 16777216) return {};
    size = qBound(32, size, 1024);
    reader.setScaledSize(natural.scaled(size,size,Qt::KeepAspectRatio));
    return reader.read();
}
// Shared compressed/decoded caches are bounded; visible items hold only display-sized images.
class AvatarCache final : public QObject {
    Q_OBJECT
    static constexpr qint64 MaximumDownload = 16*1024*1024;
    static constexpr int MaximumRequests = 4, MaximumQueuedRequests = 64, MaximumDecodes = 2;
    QCache<QString,QByteArray> encoded{64*1024};
    QCache<QString,QImage> decoded{32*1024};
    QCache<QString,qint64> failures{512};
    QNetworkAccessManager network;
    QThreadPool pool;
    QSet<QString> requests, queued, decoding;
    QQueue<QString> queue;
    bool pumping = false;
    bool recentlyFailed(const QString &key) {
        const auto failed = failures.object(key);
        return failed && QDateTime::currentMSecsSinceEpoch() - *failed < 30000;
    }
    void failed(const QString &key) { failures.insert(key, new qint64(QDateTime::currentMSecsSinceEpoch())); }
    void decode(const QString &source, int size, const QByteArray &bytes) {
        const auto key = source + "|" + QString::number(size);
        // Never enqueue compressed image copies in an unbounded worker queue.
        // Waiting items retry from the bounded encoded cache when capacity returns.
        if (decoding.contains(key) || decoding.size() >= MaximumDecodes || recentlyFailed(key)) return;
        decoding.insert(key);
        auto watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this,watcher,key] {
            const auto image = watcher->result(); decoding.remove(key);
            if (!image.isNull()) decoded.insert(key, new QImage(image), qMax(1,int((image.sizeInBytes()+1023)/1024)));
            else failed(key);
            emit imageReady(key,image); watcher->deleteLater();
            emit capacityAvailable(); pump();
        });
        watcher->setFuture(QtConcurrent::run(&pool, CrispImage::decodeForDisplay, bytes, size));
    }
    void pump() {
        if (pumping) return;
        pumping = true;
        while (!queue.isEmpty() && requests.size() < MaximumRequests && decoding.size() < MaximumDecodes) {
            const auto source = queue.dequeue(); queued.remove(source);
            if (encoded.contains(source) || recentlyFailed(source)) continue;
            requests.insert(source);
            QNetworkRequest request{QUrl(source)};
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
            request.setTransferTimeout(15000);
            auto reply = network.get(request);
            reply->setReadBufferSize(MaximumDownload+1);
            connect(reply,&QNetworkReply::readyRead,reply,[reply] { if (reply->bytesAvailable() > MaximumDownload) reply->abort(); });
            connect(reply,&QNetworkReply::downloadProgress,reply,[reply](qint64 received,qint64 total) {
                if (received > MaximumDownload || total > MaximumDownload) reply->abort();
            });
            connect(reply,&QNetworkReply::finished,this,[this,reply,source] {
                requests.remove(source);
                const auto bytes = reply->read(MaximumDownload+1);
                if (reply->error()==QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200 && !bytes.isEmpty() && bytes.size()<=MaximumDownload) {
                    encoded.insert(source,new QByteArray(bytes),qMax(1,int((bytes.size()+1023)/1024)));
                    emit sourceReady(source);
                } else failed(source);
                reply->deleteLater(); emit capacityAvailable(); pump();
            });
        }
        pumping = false;
    }
public:
    explicit AvatarCache(QObject *parent) : QObject(parent) { pool.setMaxThreadCount(MaximumDecodes); }
    static AvatarCache *instance() {
        static QPointer<AvatarCache> cache;
        if (!cache) cache = new AvatarCache(qApp);
        return cache;
    }
    void request(const QString &source, int size) {
        const auto key = source + "|" + QString::number(size);
        if (auto image = decoded.object(key)) { emit imageReady(key,*image); return; }
        if (auto bytes = encoded.object(source)) { decode(source,size,*bytes); return; }
        if (requests.contains(source) || queued.contains(source) || recentlyFailed(source)) return;
        if (queue.size() >= MaximumQueuedRequests) return;
        queued.insert(source); queue.enqueue(source); pump();
    }
signals:
    void sourceReady(const QString &source);
    void imageReady(const QString &key, const QImage &image);
    void capacityAvailable();
};
CrispImage::CrispImage(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true); setMipmap(true);
    auto cache = AvatarCache::instance();
    connect(cache,&AvatarCache::sourceReady,this,[this](const QString &source){ if(source==m_requestSource) load(); });
    connect(cache,&AvatarCache::imageReady,this,[this](const QString &key,const QImage &image){
        if(key!=m_key) return;
        const bool old = ready(); m_loadedKey=key; m_image=image; update(); if(old!=ready()) emit readyChanged();
    });
    connect(cache,&AvatarCache::capacityAvailable,this,[this]{ if(m_loadedKey!=m_key) load(); });
    connect(this,&QQuickItem::windowChanged,this,[this]{ load(); });
}
void CrispImage::setSource(const QString &value) {
    const QString source=allowedSource(value) ? value : QString{};
    if(m_source==source) return;
    const bool old=ready(); m_source=source; m_image={}; m_key.clear(); m_loadedKey.clear(); update();
    emit sourceChanged(); if(old) emit readyChanged(); load();
}
void CrispImage::setRadius(qreal value) { if(!std::isfinite(value) || m_radius==value) return; m_radius=qMax(0.,value); emit radiusChanged(); update(); }
void CrispImage::setDecodeSize(int value) { value=qBound(32,value,1024); if(m_decodeSize==value) return; m_decodeSize=value; emit decodeSizeChanged(); load(); }
void CrispImage::geometryChange(const QRectF &next,const QRectF &previous) { QQuickPaintedItem::geometryChange(next,previous); if(next.size()!=previous.size()) load(); }
void CrispImage::itemChange(ItemChange change, const ItemChangeData &data) {
    QQuickPaintedItem::itemChange(change, data);
    if (change == ItemDevicePixelRatioHasChanged) load();
}
int CrispImage::displayPixels(qreal logicalSize, qreal dpr, int minimum) {
    const qreal physical = std::isfinite(logicalSize) && std::isfinite(dpr) ? qBound(0., logicalSize*dpr, 1024.) : 1024.;
    const int need = qBound(32, qMax(minimum, int(std::ceil(physical))), 1024);
    int size=32; while(size<need) size*=2;
    return size;
}
void CrispImage::load() {
    if(m_source.isEmpty()) return;
    const qreal dpr=window() ? window()->effectiveDevicePixelRatio() : 1.;
    const int size=displayPixels(qMax(width(),height()),dpr,m_decodeSize);
    m_requestSource=requestUrl(m_source,size);
    m_key=m_requestSource+"|"+QString::number(size);
    if(m_loadedKey==m_key) return;
    AvatarCache::instance()->request(m_requestSource,size);
}
QString CrispImage::requestUrl(const QString &source,int size){
    QUrl url(source);const auto path=url.path();
    if(url.host()=="cdn.discordapp.com" && (path.startsWith("/avatars/") || path.startsWith("/icons/") || (path.startsWith("/guilds/") && path.contains("/avatars/")))){
        QUrlQuery query(url);query.removeAllQueryItems("size");query.addQueryItem("size",QString::number(qBound(32,size,1024)));url.setQuery(query);
    }
    return url.toString(QUrl::FullyEncoded);
}
void CrispImage::paint(QPainter *painter) {
    if(m_image.isNull() || width()<=0 || height()<=0) return;
    painter->setRenderHint(QPainter::Antialiasing); painter->setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF target(0,0,width(),height()); const qreal radius=qBound(0.,m_radius,qMin(width(),height())/2);
    QPainterPath clip; clip.addRoundedRect(target,radius,radius); painter->setClipPath(clip);
    const qreal scale=qMax(width()/m_image.width(),height()/m_image.height());
    const QSizeF sourceSize(width()/scale,height()/scale);
    const QRectF source((m_image.width()-sourceSize.width())/2,(m_image.height()-sourceSize.height())/2,sourceSize.width(),sourceSize.height());
    painter->drawImage(target,m_image,source);
}
#include "CrispImage.moc"
