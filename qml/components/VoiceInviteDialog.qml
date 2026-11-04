import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: dialog
    parent: Overlay.overlay
    width: Math.min(480, parent.width - 48)
    x: (parent.width - width) / 2; y: (parent.height - height) / 2
    modal: true; padding: 24
    property string channelId: ""
    property string channelName: ""
    property string createdUrl: ""
    property bool requested: false
    function openForChannel(id, label) { channelId = id; channelName = label; createdUrl = ""; requested = false; open() }
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
    Connections { target: discord; function onInviteReady(url) { if (dialog.visible && dialog.requested) dialog.createdUrl = url } }
    contentItem: ColumnLayout {
        spacing: 16
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Invite to " + dialog.channelName; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.text; font.pixelSize: Math.round(20 * preferences.fontScale); font.weight: Font.DemiBold }
            ActionButton { iconName: "close"; hint: "Close invitation"; quiet: true; onClicked: dialog.close() }
        }
        Text { Layout.fillWidth: true; text: "Create a link that expires in 24 hours."; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
        AppField { Layout.fillWidth: true; visible: dialog.createdUrl.length > 0; text: dialog.createdUrl; readOnly: true; selectByMouse: true; Accessible.name: "Invite link" }
        Text { Layout.fillWidth: true; visible: text.length > 0; text: dialog.requested ? discord.inviteError : ""; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        ActionButton {
            Layout.fillWidth: true
            text: dialog.createdUrl.length ? "Copy invite link" : discord.inviteBusy ? "Creating invite…" : "Create invite link"
            primary: true
            enabled: dialog.createdUrl.length > 0 || (!discord.inviteBusy && discord.connected && !discord.demoMode)
            onClicked: {
                if (dialog.createdUrl.length) discord.copyInvite()
                else { dialog.requested = true; discord.createVoiceInvite(dialog.channelId) }
            }
        }
    }
}
