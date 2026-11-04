#include "SoftwareVideo.h"
#include <QMutexLocker>
#include <QPainter>

SoftwareVideo::SoftwareVideo(QQuickItem *parent)
    : QQuickPaintedItem(parent), m_sink(this) {
    setFillColor(Qt::black);
    setOpaquePainting(true);
    connect(&m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) {
        {
            const QMutexLocker lock(&m_frameMutex);
            m_latestFrame = frame;
        }
        // Retain only the latest frame and at most one pending GUI update.
        if (m_updateQueued.exchange(true)) return;
        QMetaObject::invokeMethod(this, [this] {
            m_updateQueued.store(false);
            update();
        }, Qt::QueuedConnection);
    }, Qt::DirectConnection);
}

void SoftwareVideo::paint(QPainter *painter) {
    QVideoFrame frame;
    {
        const QMutexLocker lock(&m_frameMutex);
        frame = m_latestFrame;
    }
    if (!frame.isValid()) return;
    const QImage image = frame.toImage();
    if (image.isNull()) return;
    // toImage() converts the pixels, but intentionally does not apply the
    // frame's display rotation/mirroring. Match the hardware VideoOutput path
    // without allocating a second rotated full-size image for every frame.
    const int rotation = int(frame.rotation());
    const bool portrait = rotation == 90 || rotation == 270;
    QSize displayed = image.size();
    if (portrait) displayed.transpose();
    const QSizeF size = displayed.scaled(boundingRect().size().toSize(), Qt::KeepAspectRatio);
    const QSizeF unrotated = portrait ? QSizeF(size.height(), size.width()) : size;
    painter->save();
    painter->translate(width() / 2, height() / 2);
    // QPainter composes these operations so pixels rotate before mirroring,
    // as required by QVideoFrame and 3GPP camera orientation.
    if (frame.mirrored()) painter->scale(-1, 1);
    painter->rotate(rotation);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    painter->drawImage(QRectF(-unrotated.width() / 2, -unrotated.height() / 2,
                              unrotated.width(), unrotated.height()), image);
    painter->restore();
}
