#pragma once
#include <QQuickPaintedItem>
#include <QImage>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTemporaryDir>
#include <QColor>
class QNetworkReply;

// A bounded, local raster edit session. Downloads never carry account credentials.
class ImageEditor : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(bool modified READ modified NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY changed)
    Q_PROPERTY(QRectF imageRect READ imageRect NOTIFY changed)
public:
    explicit ImageEditor(QQuickItem *parent=nullptr);
    QUrl source() const {return m_source;}
    void setSource(const QUrl &source);
    bool ready() const {return !m_image.isNull();}
    bool canUndo() const {return !m_undo.isEmpty();}
    bool canRedo() const {return !m_redo.isEmpty();}
    bool modified() const {return m_image!=m_original;}
    QString status() const {return m_status;}
    QSize imageSize() const {return m_image.size();}
    QRectF imageRect() const;
    void paint(QPainter *painter) override;
    Q_INVOKABLE void begin(QPointF position,QString tool,QColor color,qreal width);
    Q_INVOKABLE void move(QPointF position);
    Q_INVOKABLE void finish();
    Q_INVOKABLE void cancelStroke();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void rotate();
    Q_INVOKABLE void flip();
    Q_INVOKABLE bool copy();
    Q_INVOKABLE bool saveAs(QUrl destination);
    Q_INVOKABLE QUrl stage();
    static bool allowedSource(const QUrl &url);
    static QImage decode(const QByteArray &bytes);
signals:
    void changed();
protected:
    void geometryChange(const QRectF &newGeometry,const QRectF &oldGeometry) override;
private:
    friend class ImageEditorTest;
    QPointF imagePoint(QPointF point) const;
    void commit(const QImage &previous);
    void refresh(QString status={});
    void trimHistory();
    QUrl m_source;
    QImage m_image,m_original,m_before;
    QList<QImage> m_undo,m_redo;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTemporaryDir m_staging;
    QString m_status,m_tool;
    QPointF m_start,m_last;
    QColor m_color;
    qreal m_penWidth=4;
};
