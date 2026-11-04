import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Pane {
    id: page
    property var service: typeof relayUpdater !== "undefined" ? relayUpdater : null
    background: Rectangle { color: Theme.canvas }
    ColumnLayout {
        anchors.fill: parent; spacing: 16
        Label { text: "Relay updates"; color: Theme.text; font.pixelSize: 24; font.bold: true }
        Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondary
            text: "Relay checks for compatible updates when you launch it. You can also check here without closing the app."
        }
        Label { objectName: "relayInstalledVersion"; text: "Running build: " + (page.service && page.service.currentVersion ? page.service.currentVersion : "Development build"); color: Theme.text; textFormat: Text.PlainText }
        Label { visible: page.service !== null && page.service.available; text: "Available build: " + (page.service ? page.service.availableVersion : ""); color: Theme.text; textFormat: Text.PlainText }
        RowLayout {
            RelayUpdateButton { service: page.service }
            BusyIndicator { visible: page.service !== null && page.service.busy; running: visible; Layout.preferredWidth: 28; Layout.preferredHeight: 28 }
        }
        Label {
            objectName: "relayUpdateStatus"; Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText
            color: page.service && page.service.error ? Theme.danger : Theme.secondary
            text: page.service ? page.service.status : "Updates are unavailable in this preview."
        }
        Label { visible: page.service !== null && page.service.lastChecked.length > 0; text: "Last checked: " + (page.service ? page.service.lastChecked : ""); color: Theme.muted; textFormat: Text.PlainText }
        Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted; text: "Only published packages for your platform count as updates. Your account settings and conversations are kept when Relay updates." }
        Item { Layout.fillHeight: true }
    }
}
