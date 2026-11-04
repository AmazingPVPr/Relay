import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Button {
    id: tag
    property var guild: ({})
    visible: String(guild.tag || "").length > 0
    implicitWidth: visible ? contents.implicitWidth + 9 : 0
    implicitHeight: 20
    padding: 4
    hoverEnabled: true
    background: Rectangle { radius: 4; color: tag.hovered ? Theme.hover : Theme.selected }
    contentItem: RowLayout {
        id: contents
        spacing: 4
        RoundedImage { source: tag.guild.badgeUrl || ""; radius: 0; Layout.preferredWidth: 14; Layout.preferredHeight: 14; visible: source.length > 0 }
        Text { text: tag.guild.tag || ""; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 11; font.weight: Font.Medium }
    }
    ToolTip.visible: hovered
    ToolTip.delay: 600
    ToolTip.text: guild.name || "View server tag"
    onClicked: {
        serverCard.active = true
        serverCard.item.showGuild(guild)
    }
    Loader { id: serverCard; active: false; sourceComponent: GuildCard {} }
}
