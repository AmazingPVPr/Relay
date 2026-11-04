#include "addons/AddonInterface.h"
#include "addons/ShareSourceInterface.h"
#include "StudioBridge.h"
#include <QCoreApplication>
#include <QStandardPaths>
extern "C" {
#include <libavutil/log.h>
}

class ObsAddon final : public QObject, public RelayAddonInterface, public RelayShareSourceInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID RelayAddonInterface_iid FILE "metadata.json")
    Q_INTERFACES(RelayAddonInterface RelayShareSourceInterface)
public:
    QString id() const override { return QStringLiteral("org.relay.obs"); }
    QObject *createService(QObject *owner) override {
        auto *studio = new StudioBridge(owner);
        if (!QStandardPaths::isTestModeEnabled() && !(QCoreApplication::instance() && QCoreApplication::instance()->property("relayPreviewMode").toBool())) studio->enableInputPersistence();
        return studio;
    }
    void stopService(QObject *service) override {
        if (auto studio = qobject_cast<StudioBridge *>(service)) {
            studio->setVideoSink(nullptr);
            studio->stop();
        }
    }
    QUrl shareSourceUrl() const override { return QUrl("qrc:/relay/addons/org.relay.obs/Input.qml"); }
    QUrl settingsUrl() const override { return QUrl("qrc:/relay/addons/org.relay.obs/Settings.qml"); }
};
#include "ObsAddon.moc"
