import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
    id: view
    property var channel: ({})
    ColumnLayout {
        width: view.availableWidth
        spacing: 20
        Item { Layout.preferredHeight: 60 }
        AppIcon { Layout.alignment: Qt.AlignHCenter; width: 48; height: 48; name: "lock"; color: Theme.muted }
        Text { Layout.fillWidth: true; text: "You don’t have access to this channel"; color: Theme.text; font.pixelSize: 23 * preferences.fontScale; font.weight: Font.DemiBold; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; Layout.leftMargin: 30; Layout.rightMargin: 30; text: "This is metadata Discord already sent to Relay. Messages and voice are available only when the server gives you permission."; color: Theme.secondary; font.pixelSize: 14 * preferences.fontScale; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter }
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Math.min(500, parent.width - 40)
            implicitHeight: metadata.implicitHeight + 40
            radius: 8; color: Theme.surface; border.color: Theme.border
            ColumnLayout {
                id: metadata
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 20
                spacing: 16
                Text { Layout.fillWidth: true; text: channel.name || "Hidden channel"; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 18 * preferences.fontScale; font.weight: Font.DemiBold; wrapMode: Text.WordWrap }
                Text { Layout.fillWidth: true; text: "Type: " + ({text: "Text",voice: "Voice",stage: "Stage",forum: "Forum",category: "Category",thread: "Thread"})[channel.kind]; color: Theme.secondary; font.pixelSize: 13 * preferences.fontScale }
                Text { Layout.fillWidth: true; visible: !!channel.parentId; text: "Category: " + (discord.channels.get(discord.channels.indexOf(channel.parentId)).name || channel.parentId || ""); textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: 13 * preferences.fontScale; wrapMode: Text.WordWrap }
                Text { Layout.fillWidth: true; visible: !!channel.topic; text: channel.topic || ""; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: 14 * preferences.fontScale; wrapMode: Text.WordWrap }
                Text { Layout.fillWidth: true; visible: Number(channel.bitrate) > 0; text: "Bitrate: " + Math.round(Number(channel.bitrate) / 1000) + " kbps"; color: Theme.secondary; font.pixelSize: 13 * preferences.fontScale }
                Text { Layout.fillWidth: true; visible: Number(channel.userLimit) > 0; text: "User limit: " + channel.userLimit; color: Theme.secondary; font.pixelSize: 13 * preferences.fontScale }
                ActionButton { text: "Copy channel ID"; quiet: true; onClicked: memberActions.copyId(channel.recordId) }
            }
        }
    }
}
