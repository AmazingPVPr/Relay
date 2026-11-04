import QtQuick
import QtQuick.Layouts
import "Theme.js" as Theme

ColumnLayout {
    property string text: ""
    property string description: ""
    Layout.fillWidth: true
    spacing: 8
    Text { Layout.fillWidth: true; text: parent.text; color: Theme.text; font.pixelSize: Math.round(22 * preferences.fontScale); font.weight: Font.Medium; wrapMode: Text.WordWrap }
    Text { Layout.fillWidth: true; text: parent.description; visible: text.length > 0; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
}
