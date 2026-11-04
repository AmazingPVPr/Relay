import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog
    required property var service
    title: "Show hidden channels"
    modal: true
    width: Math.min(470, parent ? parent.width - 40 : 470)
    standardButtons: Dialog.Close
    contentItem: ColumnLayout {
        spacing: 16
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: "Hidden channels appear dimmed with a lock in the channel list. Select one to see the name, category, topic, and other metadata Discord already sent to Relay." }
        Label { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: "Messages, members, and voice access still require permission from the server. Turning off this add-on hides those entries again."; opacity: 0.7 }
    }
}
