import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: invite
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(480, parent ? parent.width - 32 : 480)
    modal: true; focus: true
    property string guildId: ""
    property string guildName: ""
    property string recipientName: ""
    property string inviteUrl: ""
    property bool waiting: false
    readonly property var channels: guildManagement.guildId === guildId ? guildManagement.channels.filter(channel => [0,2,5].indexOf(Number(channel.type)) !== -1 && guildManagement.channelPermissions(guildId, channel.id).createInvite) : []
    title: "Invite to " + (guildName || "Server")
    function openFor(id, name, recipient) { guildId = id; guildName = name; recipientName = recipient || ""; inviteUrl = ""; waiting = false; guildManagement.selectGuild(id); open() }
    background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 14
        Text { Layout.fillWidth: true; text: invite.recipientName ? "Create an invite link to share with " + invite.recipientName + "." : "Create an invite link to share with a friend."; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale) }
        ComboBox { id: channel; Layout.fillWidth: true; model: invite.channels; textRole: "name"; valueRole: "id"; enabled: !invite.waiting && count > 0; displayText: count ? currentText : "No channels available for invitations" }
        Text { Layout.fillWidth: true; visible: guildManagement.error.length > 0; text: guildManagement.error; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale) }
        TextField { Layout.fillWidth: true; visible: invite.inviteUrl.length > 0; text: invite.inviteUrl; readOnly: true; selectByMouse: true; color: Theme.text; background: Rectangle { radius: 4; color: Theme.rail } }
        Text { Layout.fillWidth: true; text: "This invite expires in 24 hours."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale) }
        RowLayout {
            Layout.fillWidth: true
            ActionButton { text: "Close"; quiet: true; onClicked: invite.close() }
            Item { Layout.fillWidth: true }
            ActionButton { text: invite.inviteUrl.length ? "Copy Invite Link" : invite.waiting ? "Creating…" : "Create Invite"; primary: true; enabled: !invite.waiting && !guildManagement.mutating && channel.count > 0; onClicked: { if (invite.inviteUrl.length) chatText.copy(invite.inviteUrl); else { guildManagement.selectGuild(invite.guildId); invite.waiting = guildManagement.execute("createInvite", String(channel.currentValue), {maxAge:86400,maxUses:0,temporary:false}); } } }
        }
    }
    Connections {
        target: guildManagement
        function onChanged() { if (!guildManagement.mutating && invite.waiting && guildManagement.error.length) invite.waiting = false }
        function onActionCompleted(action, id, result) { if (invite.visible && invite.waiting && action === "createInvite" && guildManagement.guildId === invite.guildId) { invite.waiting = false; const code = result.code || result.id; if (code) invite.inviteUrl = "https://discord.gg/" + code } }
    }
}
