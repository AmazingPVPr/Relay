import QtQuick
import QtQuick.Layouts
import "Theme.js" as Theme

RowLayout {
    id: row
    property string title: ""
    property string description: ""
    property string value: ""
    property string actionText: ""
    property string secondaryText: ""
    property bool actionEnabled: true
    signal triggered()
    signal secondaryTriggered()
    Layout.fillWidth: true
    spacing: 12
    ColumnLayout {
        Layout.fillWidth: true; Layout.minimumWidth: 90; spacing: 5
        Text { Layout.fillWidth: true; text: row.title; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; text: row.description; visible: text.length > 0; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap }
    }
    Text { Layout.preferredWidth: Math.round(160 * Math.min(preferences.fontScale, 1.12)); visible: row.value.length > 0; text: row.value; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale); horizontalAlignment: Text.AlignRight; elide: Text.ElideMiddle }
    ActionButton { visible: row.secondaryText.length > 0; text: row.secondaryText; quiet: true; onClicked: row.secondaryTriggered() }
    ActionButton { visible: row.actionText.length > 0; text: row.actionText; enabled: row.actionEnabled; onClicked: row.triggered() }
}
