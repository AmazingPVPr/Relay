import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
    id: page
    signal navigate(string page, string section)
    readonly property var shortcuts: [
        {title:"Messages", rows:[{label:"Send Message",keys:"Enter"},{label:"New Line",keys:"Shift + Enter"},{label:"Focus text area / Close menu",keys:"Escape"}]},
        {title:"Navigation",rows:[{label:"Navigate between servers",keys:"Ctrl + Alt + ↑ / ↓"},{label:"Navigate between channels",keys:"Alt + ↑ / ↓"},{label:"Toggle Quick Switcher",keys:"Ctrl + K"},{label:"Open Settings",keys:"Ctrl + ,"}]},
        {title:"Drag and Drop",rows:[{label:"Move server or folder",keys:"Ctrl + ↑ / ↓"},{label:"Move server out of folder",keys:"Ctrl + ←"},{label:"Open item menu",keys:"Shift + F10"},{label:"Cancel drag",keys:"Escape"}]},
        {title:"Chat",rows:[{label:"Search",keys:"Ctrl + F"},{label:"Toggle Pins",keys:"Ctrl + P"},{label:"Toggle Member List",keys:"Ctrl + U"},{label:"Toggle Emoji Picker",keys:"Ctrl + E"},{label:"Toggle GIF Picker",keys:"Ctrl + G"}]},
        {title:"Voice and Video",rows:[{label:"Toggle Microphone",keys:shortcutSettings.muteShortcut || "Not assigned"},{label:"Toggle Deafen",keys:shortcutSettings.deafenShortcut || "Not assigned"},{label:"Disconnect Voice",keys:shortcutSettings.disconnectShortcut || "Not assigned"},{label:"Toggle Soundboard",keys:"Ctrl + Shift + B"}]}
    ]
    ColumnLayout {
        width: page.availableWidth; spacing: 24
        SettingsHeading { objectName: "settingsSection_custom"; text: "Custom Keybinds"; description: "Create keyboard shortcuts for actions you use often." }
        Repeater {
            model: shortcutSettings.bindings
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true; spacing: 12
                Text { Layout.preferredWidth: Math.min(180, page.availableWidth * 0.28); text: modelData.label; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 14 * preferences.fontScale; wrapMode: Text.WordWrap }
                AppField { id: sequence; Layout.fillWidth: true; text: modelData.sequence; placeholderText: "No shortcut"; onAccepted: shortcutSettings.setBinding(modelData.action,text); Accessible.name: modelData.label + " shortcut" }
                ActionButton { text: "Save"; onClicked: shortcutSettings.setBinding(modelData.action,sequence.text) }
            }
        }
        Text { Layout.fillWidth: true; visible: !!shortcutSettings.error; text: shortcutSettings.error; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; text: "Use Ctrl, Alt, or Meta with a key, or choose a function key. Leave a field blank to disable that shortcut."; color: Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        RowLayout {
            Layout.fillWidth: true
            ActionButton { text: "Reset defaults"; quiet: true; onClicked: shortcutSettings.reset() }
            Item { Layout.fillWidth: true }
            ActionButton { text: globalShortcuts.busy ? "Waiting for your desktop…" : "Configure system shortcuts"; enabled: !globalShortcuts.busy; onClicked: globalShortcuts.enable(shortcutSettings.bindings) }
        }
        ActionButton { visible: globalShortcuts.active; text: "Disable system shortcuts"; quiet: true; onClicked: globalShortcuts.disable() }
        Text { Layout.fillWidth: true; text: globalShortcuts.error || globalShortcuts.status; textFormat: Text.PlainText; color: globalShortcuts.error.length ? Theme.danger : Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        SettingsDivider {}
        SettingsHeading { objectName: "settingsSection_default"; text: "Default Keybinds"; description: "Shortcuts available in Relay. Server drag shortcuts work while a server or folder has keyboard focus." }
        Repeater {
            model: page.shortcuts
            delegate: ColumnLayout {
                id: group
                required property var modelData
                Layout.fillWidth: true; spacing: 12
                Text { Layout.fillWidth: true; text: group.modelData.title; color: Theme.text; font.pixelSize: 18 * preferences.fontScale; font.weight: Font.Medium; Layout.topMargin: 16 }
                Rectangle {
                    Layout.fillWidth: true; radius: 8; color: Theme.raised
                    implicitHeight: keyRows.implicitHeight
                    ColumnLayout {
                        id: keyRows
                        anchors.left: parent.left; anchors.right: parent.right; spacing: 0
                        Repeater {
                            model: group.modelData.rows
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true; spacing: 0
                                RowLayout {
                                    Layout.fillWidth: true; Layout.margins: 16; spacing: 18
                                    Text { Layout.fillWidth: true; text: modelData.label; color: Theme.secondary; font.pixelSize: 13 * preferences.fontScale; wrapMode: Text.WordWrap }
                                    Rectangle { radius: 3; color: Theme.border; implicitWidth: keys.implicitWidth + 10; implicitHeight: keys.implicitHeight + 6; Text { id: keys; anchors.centerIn: parent; text: modelData.keys; color: Theme.text; font.pixelSize: 11 * preferences.fontScale; font.weight: Font.DemiBold } }
                                }
                                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                            }
                        }
                    }
                }
            }
        }
        SettingsDivider {}
        SettingsHeading { objectName: "settingsSection_performance"; text: "Performance" }
        SettingsToggle { Layout.fillWidth: true; title: "Hardware Acceleration"; description: "Use your GPU to draw the interface. Restart Relay to apply changes."; checked: preferences.hardwareAcceleration; onToggled: value => preferences.hardwareAcceleration=value }
        Text { Layout.fillWidth: true; visible: preferences.graphicsRestartRequired; text: "Restart Relay to apply your graphics setting."; color: Theme.warning; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        Item { Layout.preferredHeight: 40 }
    }
}
