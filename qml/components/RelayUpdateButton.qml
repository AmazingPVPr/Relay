import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Button {
    id: control
    objectName: "relayUpdateButton"
    property var service: typeof relayUpdater !== "undefined" ? relayUpdater : null
    text: service && service.busy ? "Updating…" : service && service.available ? "Update and restart" : "Check for updates"
    enabled: service !== null && service.supported && !service.busy
    hoverEnabled: true
    ToolTip.visible: hovered
    ToolTip.text: service ? service.status : "The updater is unavailable in this preview."
    onClicked: {
        if (service.available) restartConfirmation.open()
        else service.check()
    }
    Dialog {
        id: restartConfirmation
        objectName: "relayUpdateConfirmation"
        parent: Overlay.overlay
        title: "Update and restart Relay?"; modal: true
        x: parent ? (parent.width - width) / 2 : 0; y: parent ? (parent.height - height) / 2 : 0
        width: parent ? Math.min(460, parent.width - 48) : 460
        height: 240
        standardButtons: Dialog.Yes | Dialog.No
        background: Rectangle { color: Theme.surface; border.color: Theme.border; radius: 8 }
        contentItem: Label {
            wrapMode: Text.Wrap; color: Theme.text
            text: "Relay will download and verify the update, then restart. This disconnects calls and stops your streams."
        }
        onAccepted: if (control.service) control.service.install()
    }
}
