#pragma once
#include <QQuickPaintedItem>
#include <QImage>

class CrispImage : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)
    Q_PROPERTY(int decodeSize READ decodeSize WRITE setDecodeSize NOTIFY decodeSizeChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
public:
    explicit CrispImage(QQuickItem *parent = nullptr);
    QString source() const { return m_source; }
    qreal radius() const { return m_radius; }
    int decodeSize() const { return m_decodeSize; }
    bool ready() const { return !m_image.isNull(); }
    void setSource(const QString &value);
    void setRadius(qreal value);
    void setDecodeSize(int value);
    void paint(QPainter *painter) override;
signals:
    void sourceChanged(); void radiusChanged(); void decodeSizeChanged(); void readyChanged();
protected:
    void itemChange(ItemChange change, const ItemChangeData &data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
private:
    friend class AvatarCache;
    friend class CrispImageTest;
    static bool allowedSource(const QString &source);
    static QImage decodeForDisplay(QByteArray bytes, int size);
    static int displayPixels(qreal logicalSize, qreal dpr, int minimum);
    static QString requestUrl(const QString &source,int size);
    void load();
    QString m_source, m_requestSource, m_key, m_loadedKey;
    qreal m_radius = 24;
    int m_decodeSize = 256;
    QImage m_image;
};
