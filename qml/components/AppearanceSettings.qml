import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
      id: page
      contentWidth: availableWidth
      clip: true
      ColumnLayout {
        id: settingsColumn
        width: page.availableWidth
        spacing: 18
        SettingsHeading { objectName: "settingsSection_theme"; text: "Appearance" }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Interface size"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
            NumericValue { value: preferences.fontScale * 100; minimum: 80; maximum: 140; unit: "%"; onValueEdited: number => preferences.fontScale = number / 100 }
        }
        AppSlider { Layout.fillWidth: true; from: 0.8; to: 1.4; stepSize: 0.05; value: preferences.fontScale; onMoved: preferences.fontScale = value; Accessible.name: "Text size" }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true; spacing: 4
                Text { text: "Compact messages"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
                Text { Layout.fillWidth: true; text: "Use less space and hide message avatars."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
            }
            AppSwitch { checked: preferences.compactMessages; onToggled: preferences.compactMessages = checked; Accessible.name: "Compact messages" }
        }
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true; spacing: 4
                Text { text: "Reduce motion"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
                Text { Layout.fillWidth: true; text: "Turn off interface transitions."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
            }
            AppSwitch { checked: preferences.reducedMotion; onToggled: preferences.reducedMotion = checked; Accessible.name: "Reduce motion" }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                Layout.fillWidth: true; spacing: 4
                Text { text: "Graphics acceleration"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
                Text { Layout.fillWidth: true; text: "Use your GPU to draw the interface. Restart Relay to apply changes."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
            }
            AppSwitch { checked: preferences.hardwareAcceleration; onToggled: preferences.hardwareAcceleration = checked; Accessible.name: "Graphics acceleration" }
        }
        Text { Layout.fillWidth: true; visible: preferences.graphicsRestartRequired; text: "Restart Relay to apply your graphics setting."; color: Theme.warning; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        Text { objectName: "settingsSection_layout"; text: "Scrolling & layout"; color: Theme.text; font.pixelSize: Math.round(16 * preferences.fontScale); font.weight: Font.Medium }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Mouse wheel speed"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
            NumericValue { value: preferences.scrollMultiplier; minimum: 0.5; maximum: 16; unit: "×"; onValueEdited: number => preferences.scrollMultiplier = number }
        }
        AppSlider { Layout.fillWidth: true; from: 0.5; to: 16; stepSize: 0.5; value: preferences.scrollMultiplier; onMoved: preferences.scrollMultiplier = value; Accessible.name: "Mouse wheel speed" }
        Text { Layout.fillWidth: true; text: Math.round(preferences.scrollMultiplier * 160) + " pixels per wheel step · trackpad speed follows your system"; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        SettingsToggle { Layout.fillWidth: true; title: "Show member list"; checked: preferences.memberListVisible; onToggled: value => preferences.memberListVisible = value }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Channel sidebar width"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
            NumericValue { value: preferences.channelSidebarWidth; minimum: 180; maximum: 520; unit: " px"; onValueEdited: number => preferences.channelSidebarWidth = number }
        }
        AppSlider { Layout.fillWidth: true; from: 180; to: 520; stepSize: 10; value: preferences.channelSidebarWidth; onMoved: preferences.channelSidebarWidth = value; Accessible.name: "Channel sidebar width" }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Member sidebar width"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
            NumericValue { value: preferences.memberSidebarWidth; minimum: 180; maximum: 480; unit: " px"; onValueEdited: number => preferences.memberSidebarWidth = number }
        }
        AppSlider { Layout.fillWidth: true; from: 180; to: 480; stepSize: 10; value: preferences.memberSidebarWidth; onMoved: preferences.memberSidebarWidth = value; Accessible.name: "Member sidebar width" }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Server bar width"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
            NumericValue { value: preferences.serverRailWidth; minimum: 56; maximum: 104; unit: " px"; onValueEdited: number => preferences.serverRailWidth = number }
        }
        AppSlider { Layout.fillWidth: true; from: 56; to: 104; stepSize: 4; value: preferences.serverRailWidth; onMoved: preferences.serverRailWidth = value; Accessible.name: "Server bar width" }
        ActionButton { text: "Reset layout"; quiet: true; onClicked: { preferences.scrollMultiplier = 1; preferences.channelSidebarWidth = 240; preferences.memberSidebarWidth = 240; preferences.memberListVisible = true; preferences.serverRailWidth = 72 } }

      }
    }
