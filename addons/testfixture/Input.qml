import QtQuick
import QtQuick.Controls
Column {
    objectName: "fixtureShareEditor"
    required property var service
    Component.onDestruction: if (service && !service.broadcastContext.active) service.stop()
    Text { text: "Generated input" }
    Button { objectName: "fixtureConnectInput"; text: "Connect generated input"; onClicked: service.start() }
}
