#include "SoftwareVideo.h"
#include <QImage>
#include <QPainter>
#include <QtTest>

class SoftwareVideoTest final : public QObject {
    Q_OBJECT
private slots:
    void cameraDisplayOrientation_data() {
        QTest::addColumn<int>("rotation");QTest::addColumn<bool>("mirror");
        for(int angle:{0,90,180,270})for(bool mirrored:{false,true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(angle).arg(mirrored?"mirror":"plain")))<<angle<<mirrored;
    }
    void cameraDisplayOrientation() {
        QFETCH(int,rotation);QFETCH(bool,mirror);
        QImage source(80,40,QImage::Format_RGB32);source.fill(Qt::black);
        QPainter pattern(&source);
        pattern.fillRect(0,0,40,20,Qt::red);pattern.fillRect(40,0,40,20,Qt::green);
        pattern.fillRect(0,20,40,20,Qt::blue);pattern.fillRect(40,20,40,20,Qt::yellow);pattern.end();
        QVideoFrame frame(source);frame.setRotation(QtVideo::Rotation(rotation));frame.setMirrored(mirror);
        SoftwareVideo item;item.setWidth(160);item.setHeight(160);item.videoSink()->setVideoFrame(frame);
        QImage rendered(160,160,QImage::Format_RGB32);rendered.fill(Qt::black);
        QPainter painter(&rendered);item.paint(&painter);painter.end();
        // Independent image transformation supplies a pixel-accurate expected
        // orientation, including asymmetric quadrants and black letterboxing.
        QImage expected=source.transformed(QTransform().rotate(rotation));
        if(mirror)expected=expected.transformed(QTransform().scale(-1,1));
        expected=expected.scaled(160,160,Qt::KeepAspectRatio,Qt::FastTransformation);
        const int left=(160-expected.width())/2,top=(160-expected.height())/2;
        for(int y=0;y<expected.height();y+=10)for(int x=0;x<expected.width();x+=10){
            const int sampleX=std::min(x+5,expected.width()-1),sampleY=std::min(y+5,expected.height()-1);
            QCOMPARE(rendered.pixelColor(left+sampleX,top+sampleY),expected.pixelColor(sampleX,sampleY));
        }
        if(left)QCOMPARE(rendered.pixelColor(0,80),QColor(Qt::black));
        if(top)QCOMPARE(rendered.pixelColor(80,0),QColor(Qt::black));
    }
};
QTEST_MAIN(SoftwareVideoTest)
#include "software_video_test.moc"
