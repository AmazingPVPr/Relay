import QtQuick
import QtQuick.Controls
import Relay
Dialog {
    id: dialog
    required property var service
    parent: Overlay.overlay
    width: Math.min(660,parent.width-48); height: Math.min(650,parent.height-48)
    x:(parent.width-width)/2; y:(parent.height-height)/2; modal:true
    onClosed: if(service)service.cancelDraft()
    contentItem: Board { service:dialog.service }
}
