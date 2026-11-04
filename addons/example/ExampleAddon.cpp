#include "AddonInterface.h"
#include <QTimer>

class ExampleService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int seconds READ seconds NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
public:
    explicit ExampleService(QObject *owner) : QObject(owner) {
        timer.setInterval(1000);
        connect(&timer, &QTimer::timeout, this, [this] { ++elapsed; emit changed(); });
    }
    int seconds() const { return elapsed; }
    bool running() const { return timer.isActive(); }
    Q_INVOKABLE void start() { timer.start(); emit changed(); }
    Q_INVOKABLE void stop() { timer.stop(); emit changed(); }
signals:
    void changed();
private:
    QTimer timer;
    int elapsed = 0;
};

class ExampleAddon final : public QObject, public RelayAddonInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID RelayAddonInterface_iid FILE "metadata.json")
    Q_INTERFACES(RelayAddonInterface)
public:
    QString id() const override { return "org.example.timer"; }
    QObject *createService(QObject *owner) override { return new ExampleService(owner); }
    void stopService(QObject *service) override {
        if (auto example = qobject_cast<ExampleService *>(service)) example->stop();
    }
    QUrl settingsUrl() const override { return QUrl("qrc:/relay/addons/org.example.timer/Settings.qml"); }
};
#include "ExampleAddon.moc"
