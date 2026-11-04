import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Popup {
    id: card
    property var guild: ({})
    property string tag: ""
    parent: Overlay.overlay
    width: Math.min(340, parent.width - 32)
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.rail; radius: 8; border.color: Theme.border }
    function showGuild(value) { tag = value.tag || ""; guild = discord.guildInfo(value.id); open() }
    contentItem: ColumnLayout {
        spacing: 14
        RowLayout {
            Rectangle { width: 64; height: 64; radius: 16; color: Theme.avatar
                Text { anchors.centerIn: parent; visible: !avatar.ready; text: card.tag; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 18 }
                RoundedImage { id: avatar; anchors.fill: parent; source: card.guild.iconUrl || ""; radius: 16 }
            }
            Item { Layout.fillWidth: true }
            ActionButton { iconName: "close"; hint: "Close server profile"; quiet: true; onClicked: card.close() }
        }
        Text { Layout.fillWidth: true; text: card.guild.name || card.tag; color: Theme.text; font.pixelSize: 20; font.weight: Font.DemiBold; textFormat: Text.PlainText; wrapMode: Text.Wrap }
        Text { Layout.fillWidth: true; visible: !!card.guild.countKnown; text: Number(card.guild.memberCount || 0).toLocaleString(Qt.locale(), 'f', 0) + " members" + (card.guild.onlineCountKnown ? " · " + Number(card.guild.onlineCount).toLocaleString(Qt.locale(), 'f', 0) + " online" : ""); color: Theme.secondary; font.pixelSize: 12; wrapMode: Text.Wrap }
        Text { Layout.fillWidth: true; visible: !!card.guild.description; text: card.guild.description || ""; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: 13; wrapMode: Text.Wrap }
        Text { Layout.fillWidth: true; visible: !card.guild.joined; text: "This person's server tag. Server details are available for servers you belong to."; color: Theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap }
        ActionButton { Layout.fillWidth: true; visible: !!card.guild.joined; text: "Go to server"; onClicked: { discord.navigateGuild(card.guild.id); card.close() } }
    }
}
