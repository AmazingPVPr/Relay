import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

ColumnLayout {
    id: page
    signal openAddon(string id)
        spacing: 18
        Text { Layout.fillWidth: true; text: "Install add-ons shared with you, then enable them here. Native add-ons run with Relay’s access to your computer; only install ones you trust."; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
        RowLayout {
            Layout.fillWidth: true
            ActionButton { text: "Open add-ons folder"; quiet: true; onClicked: addons.openLocalDirectory() }
            ActionButton { text: "Refresh"; quiet: true; onClicked: addons.rescan() }
        }
        ListView {
            boundsBehavior: Flickable.StopAtBounds
            boundsMovement: Flickable.StopAtBounds
            id: addonList
            Layout.fillWidth: true; Layout.fillHeight: true
            model: addons.items
            spacing: 12; clip: true
            delegate: Rectangle {
                required property var modelData
                width: addonList.width
                implicitHeight: addonContent.implicitHeight + 28
                color: Theme.sidebar
                radius: 6
                border.color: Theme.border
                ColumnLayout {
                    id: addonContent
                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 14; anchors.verticalCenter: parent.verticalCenter
                    spacing: 9
                    RowLayout {
                        Layout.fillWidth: true
                        Text { Layout.fillWidth: true; text: modelData.name; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(15 * preferences.fontScale); font.weight: Font.Medium; elide: Text.ElideRight }
                        AppSwitch { checked: !!modelData.enabled; enabled: !!modelData.available; onToggled: addons.setEnabled(modelData.id, checked); Accessible.name: modelData.name + " enabled" }
                    }
                    Text { Layout.fillWidth: true; text: modelData.description; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
                    RowLayout {
                        Layout.fillWidth: true
                        Text { Layout.fillWidth: true; text: modelData.status || (modelData.enabled ? "Enabled" : "Disabled"); textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap }
                        ActionButton { text: "Open"; enabled: !!modelData.enabled && !!modelData.available; onClicked: page.openAddon(modelData.id) }
                    }
                }
            }
            ScrollBoost { flickable: addonList }
            ScrollBar.vertical: ScrollBar { width: 5 }
            Text { anchors.centerIn: parent; visible: addonList.count === 0; text: "No add-ons are installed."; color: Theme.muted; font.pixelSize: Math.round(13 * preferences.fontScale) }
        }
    }
