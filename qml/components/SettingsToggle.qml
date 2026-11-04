import QtQuick
import QtQuick.Layouts
import "Theme.js" as Theme

RowLayout {
    id: row
    property string title: ""
    property string description: ""
    property alias checked: toggle.checked
    signal toggled(bool value)
    spacing: 20
    opacity: enabled ? 1 : 0.5
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 5
        Text { Layout.fillWidth: true; text: row.title; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(14 * preferences.fontScale); wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; visible: text.length > 0; text: row.description; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
    }
    AppSwitch { id: toggle; onToggled: row.toggled(checked); Accessible.name: row.title }
}
