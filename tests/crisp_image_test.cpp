#include "CrispImage.h"
#include <QBuffer>
#include <QPainter>
#include <QUrlQuery>
#include <QtTest>
#include <limits>

class CrispImageTest final : public QObject {
    Q_OBJECT
    static QByteArray png(const QImage &image) {
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG"); return bytes;
    }
private slots:
    void acceptsOnlyNativeDiscordImageEndpoints() {
        QVERIFY(CrispImage::allowedSource("https://cdn.discordapp.com/avatars/42/hash.png?size=4096"));
        QVERIFY(CrispImage::allowedSource("https://images-ext-1.discordapp.net/external/example"));
        for (const auto &url : {"http://cdn.discordapp.com/a.png", "https://cdn.discordapp.com.evil.test/a.png", "https://user@cdn.discordapp.com/a.png", "https://cdn.discordapp.com:8443/a.png", "file:///tmp/a.png", "data:image/png;base64,a"})
            QVERIFY2(!CrispImage::allowedSource(url), url);
    }
    void decodeRespectsPhysicalPixelsAndRejectsOversizedImages() {
        QImage input(1200,600,QImage::Format_ARGB32); input.fill(Qt::red);
        const auto image=CrispImage::decodeForDisplay(png(input),256);
        QCOMPARE(image.size(),QSize(256,128)); QCOMPARE(image.pixelColor(128,64),QColor(Qt::red));
        QVERIFY(CrispImage::decodeForDisplay("not an image",256).isNull());
        QImage oversized(8193,1,QImage::Format_RGB32); oversized.fill(Qt::black);
        QVERIFY(CrispImage::decodeForDisplay(png(oversized),256).isNull());
    }
    void dprChangesSelectSharpBoundedDecodeBuckets() {
        QCOMPARE(CrispImage::displayPixels(72,1.,32),128);
        QCOMPARE(CrispImage::displayPixels(72,2.,32),256);
        QCOMPARE(CrispImage::displayPixels(40,1.5,32),64);
        QCOMPARE(CrispImage::displayPixels(64,1.,256),256);
        QCOMPARE(CrispImage::displayPixels(1e20,2.,32),1024);
        QCOMPARE(CrispImage::displayPixels(std::numeric_limits<double>::infinity(),2.,32),1024);
    }
    void avatarDownloadsMatchTheDisplaySizeWithoutChangingAttachments(){
        const auto avatar=CrispImage::requestUrl("https://cdn.discordapp.com/avatars/42/hash.png?size=4096",64);
        QCOMPARE(QUrlQuery(QUrl(avatar)).queryItemValue("size"),QString("64"));
        const QString attachment="https://cdn.discordapp.com/attachments/1/2/image.png?signature=fixture";
        QCOMPARE(CrispImage::requestUrl(attachment,64),attachment);
    }
    void paintingUsesLogicalCoordinatesOnHighDprSurface() {
        CrispImage item; item.setWidth(32); item.setHeight(32); item.setRadius(0);
        item.m_image=QImage(64,64,QImage::Format_ARGB32); item.m_image.fill(Qt::green);
        QImage canvas(64,64,QImage::Format_ARGB32_Premultiplied); canvas.setDevicePixelRatio(2); canvas.fill(Qt::transparent);
        QPainter painter(&canvas); item.paint(&painter); painter.end();
        QCOMPARE(canvas.pixelColor(0,0),QColor(Qt::green));
        QCOMPARE(canvas.pixelColor(63,63),QColor(Qt::green));
        item.setRadius(16); canvas.fill(Qt::transparent);
        QPainter round(&canvas); item.paint(&round); round.end();
        QCOMPARE(canvas.pixelColor(0,0).alpha(),0); QCOMPARE(canvas.pixelColor(32,32),QColor(Qt::green));
    }
};
QTEST_MAIN(CrispImageTest)
#include "crisp_image_test.moc"
