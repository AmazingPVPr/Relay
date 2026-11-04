#pragma once
#include <QObject>
#include <QtPlugin>
// Modules own their native windows. stopService closes them synchronously.
// No Discord credential is supplied by this interface.
class RelayWindowAddonInterface {
public:
    virtual ~RelayWindowAddonInterface() = default;
    virtual bool openWindow(QObject *service) = 0;
};
#define RelayWindowAddonInterface_iid "org.relayclient.WindowAddon/1.0"
Q_DECLARE_INTERFACE(RelayWindowAddonInterface, RelayWindowAddonInterface_iid)
