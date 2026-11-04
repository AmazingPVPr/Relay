import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
    property bool portalAvailable: typeof globalShortcuts !== "undefined"
      id: page
      contentWidth: availableWidth
      clip: true
      ColumnLayout {
        id: settingsColumn
        width: page.availableWidth
        spacing: 18
        Text { Layout.fillWidth: true; text: "Enter a shortcut such as Ctrl+Shift+M. Leave it blank to disable it."; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
        Repeater {
            model: shortcutSettings.bindings
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 12
                Text { Layout.preferredWidth: 145; text: modelData.label; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
                AppField { id: sequence; Layout.fillWidth: true; text: modelData.sequence; placeholderText: "No shortcut"; onAccepted: shortcutSettings.setBinding(modelData.action, text); Accessible.name: modelData.label + " shortcut" }
                ActionButton { text: "Save"; onClicked: shortcutSettings.setBinding(modelData.action, sequence.text) }
            }
        }
        Text { Layout.fillWidth: true; visible: text.length > 0; text: shortcutSettings.error; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; text: "These shortcuts work while Relay is focused. To use them from another app, enable system shortcuts below."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        ActionButton { text: "Reset defaults"; quiet: true; onClicked: shortcutSettings.reset() }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border; visible: page.portalAvailable }
        Text { text: "System shortcuts"; color: Theme.text; font.pixelSize: Math.round(15 * preferences.fontScale); font.weight: Font.Medium; visible: page.portalAvailable }
        Text { Layout.fillWidth: true; text: "Your desktop will ask which shortcuts to allow."; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap; visible: page.portalAvailable }
        RowLayout {
            visible: page.portalAvailable
            ActionButton {
                text: page.portalAvailable && globalShortcuts.busy ? "Waiting for your desktop…" : page.portalAvailable && globalShortcuts.active ? "Configure system shortcuts" : "Enable system shortcuts"
                enabled: page.portalAvailable && !globalShortcuts.busy
                onClicked: globalShortcuts.enable(shortcutSettings.bindings)
            }
            ActionButton { visible: page.portalAvailable && globalShortcuts.active; text: "Disable"; quiet: true; onClicked: globalShortcuts.disable() }
        }
        Repeater {
            model: page.portalAvailable && globalShortcuts.active ? globalShortcuts.bindings : []
            delegate: Text {
                required property var modelData
                Layout.fillWidth: true
                text: modelData.label + ": " + (modelData.trigger || "Not assigned")
                textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap
            }
        }
        Text { Layout.fillWidth: true; visible: text.length > 0; text: page.portalAvailable ? globalShortcuts.error || globalShortcuts.status : ""; textFormat: Text.PlainText; color: page.portalAvailable && globalShortcuts.error.length ? Theme.danger : Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
      }
    }
