import QtQuick
import QtQuick.Layouts
import "Theme.js" as Theme

ColumnLayout {
    id: heading
    property string section: ""
    property string title: ""
    property string description: ""
    property bool divider: false
    objectName: "settingsSection_" + section
    Layout.fillWidth: true
    spacing: 9
    Rectangle { visible: heading.divider; Layout.fillWidth: true; Layout.topMargin: 22; Layout.bottomMargin: 25; implicitHeight: 1; color: Theme.border }
    Text { Layout.fillWidth: true; text: heading.title; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(22 * preferences.fontScale); font.weight: Font.Medium; wrapMode: Text.WordWrap }
    Text { Layout.fillWidth: true; visible: text.length > 0; text: heading.description; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
}
