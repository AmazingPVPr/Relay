#pragma once
#include <QObject>
#include <QString>
#include <QUrl>
#include <QtPlugin>

// Versioned native ABI. The base interface supplies a QObject owner. Optional
// identity authorization uses a narrow host broker, never a reusable credential,
// Discord client or voice engine. Rebuild add-ons against the host's Qt ABI.
class RelayAddonInterface {
public:
    virtual ~RelayAddonInterface() = default;
    virtual QString id() const = 0;
    virtual QObject *createService(QObject *owner) = 0;
    // Stop all work and release devices synchronously. Must be safe when idle.
    virtual void stopService(QObject *service) = 0;
    // A resource compiled into this module, rooted under /relay/addons/<id>/.
    // The settings Dialog declares: required property var service.
    // Window-only modules implementing RelayWindowAddonInterface may return empty.
    virtual QUrl settingsUrl() const = 0;
};

#define RelayAddonInterface_iid "org.relayclient.Addon/1.0"
Q_DECLARE_INTERFACE(RelayAddonInterface, RelayAddonInterface_iid)
