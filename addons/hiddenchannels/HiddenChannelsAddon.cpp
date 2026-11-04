#include "addons/AddonInterface.h"

class HiddenChannelsAddon final : public QObject, public RelayAddonInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID RelayAddonInterface_iid FILE "metadata.json")
    Q_INTERFACES(RelayAddonInterface)
public:
    QString id() const override { return "org.relay.hiddenchannels"; }
    QObject *createService(QObject *owner) override { return new QObject(owner); }
    void stopService(QObject *) override {}
    QUrl settingsUrl() const override { return QUrl("qrc:/relay/addons/org.relay.hiddenchannels/Settings.qml"); }
};
#include "HiddenChannelsAddon.moc"
