#include "ImageEditor.h"
#include "AppSettings.h"
#include <QtTest>
#include <QBuffer>
#include <QPainter>
#include <QQmlPropertyMap>
#include <QFile>
#include <QImageReader>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QGuiApplication>
#include <QClipboard>
class GeneratedImageUpload : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy MEMBER busy NOTIFY changed)
    Q_PROPERTY(QString error MEMBER error NOTIFY changed)
    Q_PROPERTY(QString status MEMBER status NOTIFY changed)
public:
    bool busy=false;QString error,status;QUrl file;int sent=0,cleared=0;
    Q_INVOKABLE void clearDraft(){++cleared;}
    Q_INVOKABLE bool addFiles(const QVariantList &urls){file=urls.value(0).toUrl();return file.isLocalFile();}
    Q_INVOKABLE bool send(QString content){if(!content.isEmpty())return false;++sent;status="Attachments sent";emit changed();return true;}
signals:
    void changed();
};
class ImageEditorTest : public QObject {
    Q_OBJECT
    void fixture(ImageEditor &e) {
        e.setWidth(400);e.setHeight(300);
        e.m_image=QImage(200,100,QImage::Format_ARGB32_Premultiplied);
        e.m_image.fill(Qt::white);e.m_original=e.m_image;e.refresh();
    }
    QByteArray png(QImage image) {QByteArray bytes;QBuffer b(&bytes);b.open(QIODevice::WriteOnly);image.save(&b,"PNG");return bytes;}
private slots:
    void validatesDownloadsAndDecodeBounds() {
        QVERIFY(ImageEditor::allowedSource(QUrl("https://cdn.discordapp.com/attachments/1/image.png")));
        for(const auto &s:{"file:///etc/passwd","http://cdn.discordapp.com/a","https://cdn.discordapp.com.evil.test/a","https://x@cdn.discordapp.com/a","https://cdn.discordapp.com:444/a"})
            QVERIFY(!ImageEditor::allowedSource(QUrl(s)));
        QVERIFY(ImageEditor::decode("bad").isNull());
        QImage large(16385,1,QImage::Format_RGB32);large.fill(Qt::red);
        QVERIFY(ImageEditor::decode(png(large)).isNull());
        QImage sample(200,100,QImage::Format_ARGB32);sample.fill(Qt::red);
        QCOMPARE(ImageEditor::decode(png(sample)).pixelColor(0,0),QColor(Qt::red));
        ImageEditor e;e.setSource(QUrl("file:///tmp/private.png"));QVERIFY(!e.ready());
    }
    void drawingMappingUndoAndExport() {
        ImageEditor e;fixture(e);
        QCOMPARE(e.imageRect(),QRectF(0,50,400,200));
        e.begin({40,90},"pen",Qt::red,6);e.move({160,90});e.finish();
        QCOMPARE(e.m_image.pixelColor(40,20),QColor(Qt::red));QVERIFY(e.modified());
        e.undo();QCOMPARE(e.m_image,e.m_original);QVERIFY(e.canRedo());
        e.redo();QCOMPARE(e.m_image.pixelColor(40,20),QColor(Qt::red));
        const auto staged=e.stage();QVERIFY(staged.isLocalFile());
        QCOMPARE(QImage(staged.toLocalFile()).pixelColor(40,20),QColor(Qt::red));
        QTemporaryDir temp;QVERIFY(e.saveAs(QUrl::fromLocalFile(temp.filePath("edited.png"))));
        QCOMPARE(QImage(temp.filePath("edited.png")),e.m_image.convertToFormat(QImage::Format_ARGB32));
        // This fixture clipboard exists only in the isolated build container.
        QVERIFY(e.copy());QCOMPARE(QGuiApplication::clipboard()->image().pixelColor(40,20),QColor(Qt::red));
        e.reset();QCOMPARE(e.m_image,e.m_original);e.undo();QVERIFY(e.modified());
    }
    void croppingTransformsAndCancel() {
        ImageEditor e;fixture(e);
        e.begin({0,50},"crop",Qt::black,4);e.move({400,250});e.finish();
        QCOMPARE(e.imageSize(),QSize(200,100));QVERIFY(!e.modified());
        e.begin({100,100},"crop",Qt::black,4);e.move({300,200});e.finish();
        QCOMPARE(e.imageSize(),QSize(100,50));
        e.rotate();QCOMPARE(e.imageSize(),QSize(50,100));
        e.undo();QCOMPARE(e.imageSize(),QSize(100,50));
        e.undo();QCOMPARE(e.imageSize(),QSize(200,100));
        e.begin({20,80},"line",Qt::black,4);e.move({250,180});e.cancelStroke();
        QCOMPARE(e.m_image,e.m_original);
        e.m_image.setPixelColor(0,0,Qt::red);e.flip();QCOMPARE(e.m_image.pixelColor(199,0),QColor(Qt::red));
    }
    void createsEditorUiAndRendersGeneratedImage() {
        qmlRegisterType<ImageEditor>("Relay.Native",1,0,"ImageEditor");
        QTemporaryDir temp;AppSettings preferences(temp.filePath("settings.ini"));
        GeneratedImageUpload upload;
        QQmlPropertyMap discord;discord.insert("currentChannelName","Generated test conversation");
        QQmlEngine engine;engine.rootContext()->setContextProperty("preferences",&preferences);
        engine.rootContext()->setContextProperty("discord",&discord);
        engine.rootContext()->setContextProperty("imageUploads",&upload);
        QStringList warnings;connect(&engine,&QQmlEngine::warnings,this,[&](const QList<QQmlError> &errors){for(const auto &error:errors)warnings<<error.toString();});
        QQmlComponent component(&engine,QUrl::fromLocalFile(QStringLiteral(RELAY_IMAGE_EDITOR_QML)));
        QVERIFY2(!component.isError(),qPrintable(component.errorString()));
        std::unique_ptr<QObject> panel(component.create());
        QVERIFY2(panel,qPrintable(component.errorString()));
        auto *item=qobject_cast<QQuickItem*>(panel.get());QVERIFY(item);
        QQuickWindow window;window.resize(1000,700);
        item->setParentItem(window.contentItem());item->setSize({1000,700});
        auto *editor=panel->findChild<ImageEditor*>("imageEditorCanvas");QVERIFY(editor);
        fixture(*editor);
        // Generated quadrants reveal aspect ratio, crop and pen mapping.
        {QPainter p(&editor->m_image);p.fillRect(0,0,100,50,QColor("#264c73"));p.fillRect(100,0,100,50,QColor("#ca7458"));p.fillRect(0,50,100,50,QColor("#d2b65d"));p.fillRect(100,50,100,50,QColor("#579e89"));}
        editor->m_original=editor->m_image;editor->refresh();
        window.show();QTest::qWait(100);
        editor->begin(editor->imageRect().topLeft()+QPointF(100,100),"pen",Qt::white,4);
        editor->move(editor->imageRect().center());editor->finish();
        QTest::qWait(50);
        QVERIFY(window.grabWindow().save("/workspace/work/image-editor-generated.png"));
        auto *repost=panel->findChild<QObject*>("imageEditorRepost");QVERIFY(repost);
        QVERIFY(QMetaObject::invokeMethod(repost,"clicked"));
        QCOMPARE(upload.sent,1);QCOMPARE(upload.cleared,1);
        QVERIFY(QFile::exists(upload.file.toLocalFile()));
        QCOMPARE(QImage(upload.file.toLocalFile()).pixelColor(0,0),QColor("#264c73"));
        QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join("\n")));
    }
};
QTEST_MAIN(ImageEditorTest)
#include "image_editor_test.moc"
