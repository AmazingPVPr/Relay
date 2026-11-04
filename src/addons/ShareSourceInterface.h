#pragma once
#include <QUrl>
#include <QtPlugin>

// Optional plug-in UI contribution to the regular Share Screen dialog. The
// editor is an Item with required property var service. Opening the editor
// configures or previews input only; the host owns explicit publish consent.
// The service also implements BroadcastSource/1.0 and exposes notifying bool
// properties running and receiving, and a notifying QVariantMap broadcastContext
// exposing the context supplied by BroadcastSource/1.0 (including errors). Both
// readiness booleans must be true before explicit publish.
// Its editor releases idle previews on destruction, but preserves the input
// while the supplied broadcast context has active=true (including allocation).
class RelayShareSourceInterface {
public:
    virtual ~RelayShareSourceInterface() = default;
    virtual QUrl shareSourceUrl() const = 0;
};
#define RelayShareSourceInterface_iid "org.relayclient.ShareSource/1.0"
Q_DECLARE_INTERFACE(RelayShareSourceInterface, RelayShareSourceInterface_iid)
