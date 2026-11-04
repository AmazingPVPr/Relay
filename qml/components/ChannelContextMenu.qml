import QtQuick
import QtQuick.Controls

BoundedMenu {
    id: menu
    property string guildId: ""
    property var channel: ({})
    readonly property var allowed: { const changed = guildManagement.permissions; return guildId && channel.recordId ? guildManagement.channelPermissions(guildId, channel.recordId) : ({}) }
    signal editRequested(string guildId, var channel)
    signal createRequested(string guildId, string parentId)
    signal joinRequested(string guildId, string channelId, string channelName)
    signal inviteRequested(string channelId, string channelName)
    signal confirmationRequested(string title, string description, string label, var payload, bool reason)
    function openForChannel(guild, value) { guildId = guild; channel = value; guildManagement.selectGuild(guild); popup() }
    MenuItem { text: menu.channel.kind === "voice" ? "Open voice chat" : "Open channel"; visible: menu.channel.kind !== "category"; height: visible ? implicitHeight : 0; onTriggered: menu.channel.kind === "voice" ? discord.openVoiceChat(menu.channel.recordId) : discord.selectChannel(menu.channel.recordId) }
    MenuItem { text: voice.channelId === menu.channel.recordId ? "Disconnect" : "Join voice"; visible: menu.channel.kind === "voice" && !menu.channel.hidden; height: visible ? implicitHeight : 0; enabled: voice.channelId === menu.channel.recordId || (voice.available && discord.connected && !!menu.allowed.connect); onTriggered: voice.channelId === menu.channel.recordId ? voice.leave() : menu.joinRequested(menu.guildId, menu.channel.recordId, menu.channel.name) }
    MenuItem { text: "Invite to voice"; visible: menu.channel.kind === "voice" && !menu.channel.hidden && !!menu.allowed.createInvite; height: visible ? implicitHeight : 0; onTriggered: menu.inviteRequested(menu.channel.recordId, menu.channel.name) }
    MenuSeparator { visible: !!menu.allowed.manage || !!guildManagement.permissions.manageChannels; height: visible ? implicitHeight : 0 }
    MenuItem { text: "Edit channel"; visible: !!menu.allowed.manage; height: visible ? implicitHeight : 0; onTriggered: menu.editRequested(menu.guildId, menu.channel) }
    MenuItem { text: "Duplicate channel"; visible: !!menu.allowed.manage && !!guildManagement.permissions.manageChannels; height: visible ? implicitHeight : 0; enabled: !guildManagement.mutating; onTriggered: { guildManagement.selectGuild(menu.guildId); guildManagement.execute("duplicateChannel", menu.channel.recordId, {}) } }
    MenuItem { text: "Create channel"; visible: !!guildManagement.permissions.manageChannels; height: visible ? implicitHeight : 0; onTriggered: menu.createRequested(menu.guildId, menu.channel.kind === "category" ? menu.channel.recordId : menu.channel.parentId || "") }
    MenuItem {
        text: "Delete channel"; visible: !!menu.allowed.manage; height: visible ? implicitHeight : 0; enabled: !guildManagement.mutating
        onTriggered: menu.confirmationRequested("Delete “" + menu.channel.name + "”?", "This permanently deletes the channel and its messages. This cannot be undone.", "Delete channel", {service: "guild", guildId: menu.guildId, action: "deleteChannel", targetId: menu.channel.recordId, values: {}}, false)
    }
    MenuSeparator {}
    MenuItem { text: "Copy channel link"; onTriggered: chatText.copy("https://discord.com/channels/" + menu.guildId + "/" + menu.channel.recordId) }
    MenuItem { text: "Copy channel ID"; onTriggered: memberActions.copyId(menu.channel.recordId) }
}
