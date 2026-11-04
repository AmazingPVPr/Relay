#include "ImageEditor.h"
#include "AppSettings.h"
#include <QtTest>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QQmlComponent>
#include "NativeMediaSource.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

namespace {
constexpr auto imageUrl = "https://cdn.discordapp.com/relay-generated-fixture.gif";

QByteArray generatedGif(const QString &loop) {
    // Two generated 16x8 red/blue frames, 40 ms apart. No downloaded assets.
    QByteArray bytes = QByteArray::fromHex(
        "47494638396110000800810000ff0000000000000000000000");
    if (loop != "none") {
        bytes += QByteArray::fromHex("21ff0b4e45545343415045322e300301");
        bytes += loop == "infinite" ? QByteArray::fromHex("000000") : QByteArray::fromHex("010000");
    }
    bytes += QByteArray::fromHex(
        "21f90400040000002c00000000100008000008150001081c48b0a0c18308132a5cc8b0a1c38708030200");
    if (loop != "static") {
        bytes += QByteArray::fromHex(
            "21f90401040001002c0000000010000800810000ff00000000000000000008150001081c48b0a0c18308132a5cc8b0a1c38708030200");
    }
    bytes += char(0x3b);
    return bytes;
}

struct NetworkCounts {
    std::atomic_int created = 0, destroyed = 0, unexpected = 0;
};

class GeneratedReply final : public QNetworkReply {
public:
    GeneratedReply(const QNetworkRequest &request, QObject *parent, QByteArray bytes,
                   std::shared_ptr<NetworkCounts> counts)
        : QNetworkReply(parent), m_bytes(std::move(bytes)), m_counts(std::move(counts)) {
        ++m_counts->created;
        setRequest(request); setUrl(request.url()); setOperation(QNetworkAccessManager::GetOperation);
        setHeader(QNetworkRequest::ContentTypeHeader, "image/gif");
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] { emit readyRead(); setFinished(true); emit finished(); });
    }
    ~GeneratedReply() override { ++m_counts->destroyed; }
    void abort() override {}
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return m_bytes.size() - m_offset + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *destination, qint64 maximum) override {
        const auto amount = std::min<qint64>(m_bytes.size() - m_offset, maximum);
        if (!amount) return -1;
        std::memcpy(destination, m_bytes.constData() + m_offset, amount);
        m_offset += amount;
        return amount;
    }
private:
    QByteArray m_bytes;
    qint64 m_offset = 0;
    std::shared_ptr<NetworkCounts> m_counts;
};

class GeneratedNetwork final : public QNetworkAccessManager {
public:
    GeneratedNetwork(QObject *parent, QByteArray bytes, std::shared_ptr<NetworkCounts> counts)
        : QNetworkAccessManager(parent), m_bytes(std::move(bytes)), m_counts(std::move(counts)) {}
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
        // Never delegate to the real network, including on an unexpected URL.
        // Production buttons render their own inline SVGs. They are generated
        // local data too; no request is allowed to reach the network backend.
        if (request.url().scheme() == "data") return QNetworkAccessManager::createRequest(GetOperation, request, nullptr);
        if (request.url() != QUrl(imageUrl)) ++m_counts->unexpected;
        return new GeneratedReply(request, this, m_bytes, m_counts);
    }
private:
    QByteArray m_bytes;
    std::shared_ptr<NetworkCounts> m_counts;
};

class GeneratedNetworkFactory final : public QQmlNetworkAccessManagerFactory {
public:
    GeneratedNetworkFactory(QByteArray bytes, std::shared_ptr<NetworkCounts> counts)
        : m_bytes(std::move(bytes)), m_counts(std::move(counts)) {}
    QNetworkAccessManager *create(QObject *parent) override { return new GeneratedNetwork(parent, m_bytes, m_counts); }
private:
    QByteArray m_bytes;
    std::shared_ptr<NetworkCounts> m_counts;
};
}

class GeneratedImageBackend final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentUserId READ currentUserId CONSTANT)
    Q_PROPERTY(QString currentChannelId READ currentChannelId CONSTANT)
    Q_PROPERTY(QString error READ error CONSTANT)
    Q_PROPERTY(QVariantList categories READ categories CONSTANT)
    Q_PROPERTY(QVariantList results READ categories CONSTANT)
    Q_PROPERTY(QVariantList favorites READ favorites CONSTANT)
    Q_PROPERTY(bool busy READ busy CONSTANT)
public:
    QString currentUserId() const { return "fixture-account"; }
    QString currentChannelId() const { return "fixture-channel"; }
    QString error() const { return {}; }
    bool busy() const { return false; }
    QVariantList categories() const { return {QVariantMap{{"name", "Generated GIF"}, {"previewUrl", imageUrl}, {"url", imageUrl}}}; }
    QVariantList favorites() const { return {}; }
    Q_INVOKABLE void loadCategories() {}
    Q_INVOKABLE void cancel() {}
    Q_INVOKABLE bool isFavorite(const QString &) const { return false; }
signals:
    void stateChanged();
};

namespace {
bool waitUntil(const std::function<bool()> &condition, int milliseconds = 1500) {
    QElapsedTimer elapsed; elapsed.start();
    while (!condition() && elapsed.elapsed() < milliseconds) QTest::qWait(5);
    return condition();
}
QObject *animationIn(QObject *root, QSet<QObject *> &seen) {
    if (!root || seen.contains(root)) return nullptr;
    seen.insert(root);
    if (root->inherits("QQuickAnimatedImage")) return root;
    if (auto *window = qobject_cast<QQuickWindow *>(root)) if (auto *found = animationIn(window->contentItem(), seen)) return found;
    if (auto *item = qobject_cast<QQuickItem *>(root))
        for (auto *child : item->childItems()) if (auto *found = animationIn(child, seen)) return found;
    for (auto *child : root->children()) if (auto *found = animationIn(child, seen)) return found;
    return nullptr;
}
int runGeneratedComponent(const QString &name, const QString &loop) {
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 10;
    AppSettings preferences(temporary.filePath("settings.ini"));
    preferences.setReducedMotion(false); preferences.setAnimateImages(true);
    GeneratedImageBackend backend;
    const auto counts = std::make_shared<NetworkCounts>();
    GeneratedNetworkFactory factory(generatedGif(loop), counts);
    QStringList warnings;
    {
        QQmlEngine engine;
        engine.setNetworkAccessManagerFactory(&factory);
        engine.rootContext()->setContextProperty("preferences", &preferences);
        engine.rootContext()->setContextProperty("discord", &backend);
        engine.rootContext()->setContextProperty("gifSearch", &backend);
        QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const auto &errors) {
            for (const auto &error : errors) warnings << error.toString();
        });
        const QString sourcePath = QFINDTESTDATA("../qml/components/ImagePreview.qml");
        if (sourcePath.isEmpty()) return 11;
        QByteArray contents = "import QtQuick\nimport QtQuick.Controls\nApplicationWindow { width:640; height:480; visible:true; ";
        contents += name.toUtf8() + " { id:fixture; objectName:\"fixture\"; ";
        if (name == "ImagePreview") contents += "width:320; mediaWidth:16; mediaHeight:8; source:\"" + QByteArray(imageUrl) + "\";";
        else if (name == "MediaViewer") contents += "imageSource:\"" + QByteArray(imageUrl) + "\";";
        contents += " } ";
        if (name != "ImagePreview") contents += "Component.onCompleted: fixture.open(); ";
        contents += "}";
        QQmlComponent component(&engine);
        component.setData(contents, QUrl::fromLocalFile(QFileInfo(sourcePath).absolutePath() + "/GeneratedAnimationHarness.qml"));
        if (!waitUntil([&] { return component.status() != QQmlComponent::Loading; })) return 12;
        std::unique_ptr<QObject> window(component.create());
        if (!window) { std::fprintf(stderr,"Component: %s\n",qPrintable(component.errorString())); return 12; }
        QObject *animation = nullptr;
        if (!waitUntil([&] { QSet<QObject *> seen; animation = animationIn(window.get(), seen); return animation && animation->property("status").toInt() == 1; })) {
            std::fprintf(stderr,"Animation notready found%d status%d requests%d warnings%s\n",!!animation,animation?animation->property("status").toInt():-1,counts->created.load(),qPrintable(warnings.join('\n')));return 13;
        }
        const int frames = loop == "static" ? 1 : 2;
        if (!waitUntil([&] { return animation->property("frameCount").toInt() == frames; })) {
            std::fprintf(stderr,"Frames expected%d actual%d status%d playing%d cache%d current%d class%s source%s warnings%s\n",frames,animation->property("frameCount").toInt(),animation->property("status").toInt(),animation->property("playing").toBool(),animation->property("cache").toBool(),animation->property("currentFrame").toInt(),animation->metaObject()->className(),qPrintable(animation->property("source").toString()),qPrintable(warnings.join('\n')));return 14;
        }
        QSignalSpy frameChanges(animation, SIGNAL(currentFrameChanged()));
        if (!frameChanges.isValid()) return 15;
        if (frames > 1) {
            // Observe several complete loops, including QMovie's finite-loop finish.
            if (!waitUntil([&] { return frameChanges.count() >= 8; })) return 16;
            QObject *fixture = window->findChild<QObject *>("fixture");
            if (!fixture) return 17;
            if (name == "MediaViewer") fixture->setProperty("animationPaused", true);
            else preferences.setAnimateImages(false);
            QCoreApplication::processEvents();
            const int paused = frameChanges.count();
            QTest::qWait(150);
            if (frameChanges.count() != paused || animation->property("playing").toBool()) return 18;
            if (name == "MediaViewer") fixture->setProperty("animationPaused", false);
            else preferences.setAnimateImages(true);
            if (!waitUntil([&] { return frameChanges.count() >= paused + 4; })) return 19;
        } else QTest::qWait(250);
        if (animation->property("status").toInt() != 1) return 20;
        const auto imageWidth = animation->property("implicitWidth").toDouble();
        const auto imageHeight = animation->property("implicitHeight").toDouble();
        if (imageHeight <= 0 || std::abs(imageWidth / imageHeight - 2.) > .01) return 23;
        // Destroy a running production image/popup, then flush deferred QMovie/reply cleanup.
        window.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    if (counts->created == 0 || counts->created != counts->destroyed || counts->unexpected) {std::fprintf(stderr,"Network created%d destroyed%d unexpected%d\n",counts->created.load(),counts->destroyed.load(),counts->unexpected.load());return 21;}
    if (!warnings.isEmpty()) { std::fprintf(stderr,"Warnings: %s\n",qPrintable(warnings.join('\n'))); return 22; }
    std::puts("Generated animation completed loops, pause/resume and disposal safely");
    return 0;
}
}

class AnimatedImageTest final : public QObject {
    Q_OBJECT
private slots:
    void sequentialProductionImages_data() {
        QTest::addColumn<QString>("component"); QTest::addColumn<QString>("loop");
        for (const auto &component : {"ImagePreview", "MediaViewer", "GifPicker"})
            for (const auto &loop : {"none", "finite", "infinite", "static"})
                QTest::newRow(qPrintable(QString(component) + '-' + loop)) << QString(component) << QString(loop);
    }
    void sequentialProductionImages() {
        QFETCH(QString, component); QFETCH(QString, loop);
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QT_QPA_PLATFORM", "offscreen"); environment.insert("QT_QUICK_BACKEND", "software");
        environment.insert("QT_AUDIO_BACKEND", "pulseaudio"); environment.insert("PULSE_SERVER", "unix:/tmp/relay-qa-no-audio.sock");
        environment.insert("PIPEWIRE_REMOTE", "relay-qa-no-pipewire");
        child.setProcessEnvironment(environment); child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {"--generated-animation", component, loop});
        QVERIFY(child.waitForStarted(3000));
        const bool completed = child.waitForFinished(6000);
        if (!completed) { child.kill(); child.waitForFinished(1000); }
        const auto output = child.readAll();
        QVERIFY2(completed, output.constData());
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, qPrintable(QString("Child exit %1: %2").arg(child.exitCode()).arg(QString::fromUtf8(output))));
        QVERIFY(output.contains("disposal safely"));
    }
};

int main(int argc, char **argv) {
    QGuiApplication application(argc, argv);
    // MediaViewer's unused video branch must resolve its native type. The GIF
    // scenarios never instantiate it or open an audio/video device.
    qmlRegisterType<QQuickItem>("Relay.Native", 1, 0, "SoftwareVideo");
    qmlRegisterType<ImageEditor>("Relay.Native", 1, 0, "ImageEditor");
    qmlRegisterType<NativeMediaSource>("Relay.Native", 1, 0, "NativeMediaSource");
    if (argc == 4 && QByteArray(argv[1]) == "--generated-animation") {
#ifdef Q_OS_UNIX
        const rlimit noCore{0,0}; setrlimit(RLIMIT_CORE,&noCore);
#endif
        return runGeneratedComponent(QString::fromUtf8(argv[2]), QString::fromUtf8(argv[3]));
    }
    AnimatedImageTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "animated_image_test.moc"
