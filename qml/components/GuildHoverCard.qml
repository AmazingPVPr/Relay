import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Popup {
    id: card
    parent: Overlay.overlay
    width: Math.min(280, parent.width - 24)
    padding: 14
    modal: false; focus: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    property string guildId: ""
    property Item anchorItem
    property var info: ({})
    function prepare(id, anchor) { close(); guildId = id; anchorItem = anchor; delay.restart() }
    function dismiss(id) { if (guildId === id) { delay.stop(); close() } }
    function present() {
        if (!anchorItem || !anchorItem.hovered) return
        info = discord.guildHoverInfo(guildId)
        if (!info.name) return
        const point = anchorItem.mapToItem(parent, anchorItem.width + 12, 0)
        x = Math.min(parent.width - width - 8, point.x)
        y = Math.max(8, Math.min(parent.height - implicitHeight - 8, point.y))
        open()
    }
    Timer { id: delay; interval: 350; onTriggered: card.present() }
    Timer { id: refresh; interval: 100; onTriggered: if (card.visible) card.info = discord.guildHoverInfo(card.guildId) }
    Connections { target: discord; function onStateChanged() { if (card.visible && !refresh.running) refresh.start() } }
    background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 10
        Text { Layout.fillWidth: true; text: card.info.name || ""; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(14*preferences.fontScale); font.weight: Font.DemiBold; wrapMode: Text.WordWrap }
        Repeater {
            model: card.info.voiceChannels || []
            delegate: ColumnLayout {
                required property var modelData
                Layout.fillWidth: true; spacing: 5
                Text { Layout.fillWidth: true; text: modelData.name; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(10*preferences.fontScale); elide: Text.ElideRight }
                RowLayout {
                    spacing: 5
                    AppIcon { name: "voice"; color: Theme.secondary; Layout.preferredWidth: 17; Layout.preferredHeight: 17 }
                    Repeater {
                        model: modelData.people
                        delegate: Item {
                            required property var modelData
                            width: 26; height: 26
                            Rectangle { anchors.fill: parent; radius: 13; color: Theme.avatar }
                            Text { anchors.centerIn: parent; text: (modelData.name || "?").substring(0,1); textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 12 }
                            RoundedImage { anchors.fill: parent; source: modelData.avatarUrl || ""; radius: 13 }
                            AppIcon { visible: !!modelData.streaming; anchors.right: parent.right; anchors.bottom: parent.bottom; width: 12; height: 12; name: "video"; color: Theme.success }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }
        Text { visible: !!card.info.muted; text: "Muted"; color: Theme.muted; font.pixelSize: Math.round(11*preferences.fontScale) }
        RowLayout {
            spacing: 12
            RowLayout {
                visible: !!card.info.onlineCountKnown; spacing: 5
                Rectangle { width: 8; height: 8; radius: 4; color: Theme.success }
                Text { text: card.info.onlineCount || 0; color: Theme.success; font.pixelSize: Math.round(12*preferences.fontScale) }
            }
            RowLayout {
                visible: !!card.info.countKnown; spacing: 5
                AppIcon { visible: !card.info.onlineCountKnown; name: "members"; width: 13; height: 13; color: Theme.muted }
                Rectangle { visible: !!card.info.onlineCountKnown; width: 8; height: 8; radius: 4; color: "transparent"; border.width: 2; border.color: Theme.muted }
                Text { text: card.info.onlineCountKnown ? Math.max(0,Number(card.info.memberCount)-Number(card.info.onlineCount)) : card.info.memberCount || 0; color: Theme.secondary; font.pixelSize: Math.round(12*preferences.fontScale) }
            }
            RowLayout {
                visible: Number(card.info.voiceCount)>0; spacing: 5
                AppIcon { name: "voice"; width: 13; height: 13; color: Theme.muted }
                Text { text: card.info.voiceCount || 0; color: Theme.secondary; font.pixelSize: Math.round(12*preferences.fontScale) }
            }
            Item { Layout.fillWidth: true }
        }
    }
}
