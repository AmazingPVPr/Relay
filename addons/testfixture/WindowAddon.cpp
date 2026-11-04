#include "addons/AddonInterface.h"
#include "addons/WindowAddonInterface.h"
#include <QCoreApplication>
#include <QVariant>
class WindowFixture final : public QObject, public RelayAddonInterface, public RelayWindowAddonInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID RelayAddonInterface_iid FILE "window-metadata.json")
    Q_INTERFACES(RelayAddonInterface RelayWindowAddonInterface)
public:
    QString id() const override { return "org.relay.windowfixture"; }
    QObject *createService(QObject *owner) override {auto *s=new QObject(owner);s->setProperty("windows",0);return s;}
    QUrl settingsUrl() const override {return {};}
    bool openWindow(QObject *service) override {service->setProperty("windows",service->property("windows").toInt()+1);return true;}
    void stopService(QObject *service) override {service->setProperty("windows",0);}
};
#include "WindowAddon.moc"
