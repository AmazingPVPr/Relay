#include "addons/AddonInterface.h"
#include "addons/BroadcastSourceInterface.h"
#include "addons/ShareSourceInterface.h"
#include <QCoreApplication>
#include <QVariant>

class DummyService final : public QObject, public RelayBroadcastSourceInterface {
    Q_OBJECT
    Q_INTERFACES(RelayBroadcastSourceInterface)
    Q_PROPERTY(bool receiving READ running NOTIFY changed)
    Q_PROPERTY(bool capturing MEMBER capturing)
    Q_PROPERTY(QVariantMap broadcastContext READ broadcastContext NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool stopped READ stopped NOTIFY changed)
public:
    explicit DummyService(QObject *owner) : QObject(owner) {
        qApp->setProperty("addonFixtureServicesCreated", qApp->property("addonFixtureServicesCreated").toInt() + 1);
    }
    QVariantMap broadcastContext() const { return context; }
    void setBroadcastContext(const QVariantMap &value) override { context = value; emit changed(); }
    void setBroadcastCaptureEnabled(bool enabled, int, int, bool) override { capturing = enabled; emit changed(); }
    RelayBroadcastMedia takeBroadcastMedia() override { return {}; }
    bool capturing = false;
    bool running() const { return active; }
    bool stopped() const { return wasStopped; }
    Q_INVOKABLE void start() { active = true; emit changed(); }
    Q_INVOKABLE void stop() { active = false; capturing = false; wasStopped = true; emit changed(); }
signals:
    void changed();
private:
    bool active = false, wasStopped = false;
    QVariantMap context;
};
class DummyAddon final : public QObject, public RelayAddonInterface, public RelayShareSourceInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID RelayAddonInterface_iid FILE "metadata.json")
    Q_INTERFACES(RelayAddonInterface RelayShareSourceInterface)
public:
    QString id() const override { return "org.relay.fixture"; }
    QObject *createService(QObject *owner) override { return new DummyService(owner); }
    void stopService(QObject *service) override {
        if (auto dummy = qobject_cast<DummyService *>(service)) dummy->stop();
    }
    QUrl shareSourceUrl() const override {
        const auto overrideUrl = qApp->property("addonFixtureShareUrl").toUrl();
        return overrideUrl.isEmpty() ? QUrl("qrc:/relay/addons/org.relay.fixture/Input.qml") : overrideUrl;
    }
    QUrl settingsUrl() const override { return QUrl("qrc:/relay/addons/org.relay.fixture/Settings.qml"); }
};
#include "DummyAddon.moc"
