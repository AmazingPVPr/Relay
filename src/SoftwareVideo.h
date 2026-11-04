#pragma once

#include <QMutex>
#include <QQuickPaintedItem>
#include <QVideoFrame>
#include <QVideoSink>
#include <atomic>

// Qt Multimedia's scene graph node requires a graphics backend. This sink
// keeps video available when the user chooses the software scene graph.
class SoftwareVideo : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QVideoSink *videoSink READ videoSink CONSTANT)
public:
    explicit SoftwareVideo(QQuickItem *parent = nullptr);
    QVideoSink *videoSink() { return &m_sink; }
    void paint(QPainter *painter) override;

private:
    QVideoSink m_sink;
    QMutex m_frameMutex;
    QVideoFrame m_latestFrame;
    std::atomic_bool m_updateQueued{false};
};
