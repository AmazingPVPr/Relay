import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

BoundedMenu {
    id: serverMenu
    property string guildId: ""
    property string guildName: ""
    readonly property var allowed: guildManagement.guildId === guildId ? guildManagement.permissions : ({})
    readonly property var state: { const revision = contextActions.revision; return contextActions.guildInfo(guildId) }
    property var folders: []
    signal channelsRolesRequested(string guildId)
    signal settingsRequested(string guildId, string page)
    signal accountSettingsRequested(string page)
    signal profileEditRequested(string guildId)
    signal createRequested(string guildId, string parentId)
    signal confirmationRequested(string title, string description, string label, var payload, bool reason)
    function openForGuild(id, name) {
        guildId = id; guildName = name || "Server"; guildManagement.selectGuild(id)
        const found = []; for (let i=0; i<serverLayout.count; ++i) { const row = serverLayout.get(i); if (row.kind === "folder") found.push(row) } folders = found
        popup()
    }
    MenuItem { text: "Mark As Read"; enabled: !contextActions.busy; onTriggered: contextActions.markGuildRead(serverMenu.guildId) }
    MenuSeparator {}
    MenuItem { text: "Invite to Server"; onTriggered: serverMenu.inviteDialog.openFor(serverMenu.guildId,serverMenu.guildName,"") }
    MenuItem { text: "Channels & Roles"; onTriggered: serverMenu.channelsRolesRequested(serverMenu.guildId) }
    MenuItem { text: "Events"; onTriggered: serverMenu.settingsRequested(serverMenu.guildId, "events") }
    MenuItem { text: "Server Boost"; onTriggered: serverMenu.settingsRequested(serverMenu.guildId, "boosts") }
    MenuItem { text: "Server Settings"; visible: !!serverMenu.allowed.manageGuild || !!serverMenu.allowed.manageRoles || !!serverMenu.allowed.manageChannels; height: visible ? implicitHeight : 0; onTriggered: serverMenu.settingsRequested(serverMenu.guildId, "overview") }
    MenuItem { text: "Create Channel"; visible: !!serverMenu.allowed.manageChannels; height: visible ? implicitHeight : 0; onTriggered: serverMenu.createRequested(serverMenu.guildId, "") }
    MenuSeparator {}
    MenuItem { text: serverMenu.state.muted ? "Unmute Server" : "Mute Server"; enabled: !contextActions.busy; onTriggered: contextActions.muteGuild(serverMenu.guildId,serverMenu.state.muted ? -1 : 0) }
    Menu {
        title: "Notification Settings"
        Repeater { model: [{label:"All Messages",value:0},{label:"Only @mentions",value:1},{label:"Nothing",value:2}]; delegate: MenuItem { required property var modelData; text: modelData.label; checkable: true; checked: Number(serverMenu.state.notificationLevel) === modelData.value; enabled: !contextActions.busy; onTriggered: contextActions.setGuildNotificationLevel(serverMenu.guildId,modelData.value) } }
        MenuSeparator {}
        Menu {
            title: "Mute Server for…"
            Repeater { model: [{label:"15 Minutes",seconds:900},{label:"1 Hour",seconds:3600},{label:"3 Hours",seconds:10800},{label:"8 Hours",seconds:28800},{label:"24 Hours",seconds:86400},{label:"Until I turn it back on",seconds:0}]; delegate: MenuItem { required property var modelData; text: modelData.label; enabled: !contextActions.busy; onTriggered: contextActions.muteGuild(serverMenu.guildId,modelData.seconds) } }
        }
    }
    MenuItem { text: "Hide Muted Channels"; checkable: true; checked: !!serverMenu.state.hideMuted; onTriggered: contextActions.setGuildHideMuted(serverMenu.guildId,!serverMenu.state.hideMuted) }
    MenuItem {
        text: "Show All Channels"; checkable: true; checked: !!serverMenu.state.showAll; enabled: !!serverMenu.state.showAllAvailable
        Accessible.description: enabled ? "Show every accessible channel" : "Relay already shows all channels that your account can access."
        onTriggered: contextActions.setGuildShowAll(serverMenu.guildId,!serverMenu.state.showAll)
    }
    MenuSeparator {}
    MenuItem { text: "Privacy Settings"; onTriggered: serverMenu.accountSettingsRequested("messaging") }
    MenuItem { text: "Edit Per-server Profile"; onTriggered: serverMenu.profileEditRequested(serverMenu.guildId) }
    MenuSeparator {}
    Menu {
        title: "Move to…"
        MenuItem { text: "Create Server Folder"; onTriggered: serverLayout.createFolder(serverMenu.guildId) }
        MenuItem { text: "Remove from Folder"; visible: !!serverLayout.folderForGuild(serverMenu.guildId); height: visible ? implicitHeight : 0; onTriggered: serverLayout.drop(serverMenu.guildId,serverLayout.folderForGuild(serverMenu.guildId),"after") }
        MenuSeparator { visible: serverMenu.folders.length > 0; height: visible ? implicitHeight : 0 }
        Repeater { model: serverMenu.folders; delegate: MenuItem { required property var modelData; text: modelData.name || "Server Folder"; enabled: modelData.id !== serverLayout.folderForGuild(serverMenu.guildId); onTriggered: serverLayout.drop(serverMenu.guildId,modelData.id,"into") } }
    }
    MenuSeparator {}
    MenuItem {
        id: leave; objectName: "leaveServer"; text: "Leave Server"
        enabled: !!serverMenu.allowed.leaveGuild && !guildManagement.mutating
        Accessible.description: serverMenu.allowed.leaveReason || "Leave this server. You will need another invitation to rejoin."
        contentItem: Column {
            spacing: 4
            Text { width: parent.width; text: leave.text; font: leave.font; color: Theme.danger; wrapMode: Text.WordWrap }
            Text { width: parent.width; visible: !!serverMenu.allowed.owner; text: "Transfer ownership before leaving."; font.pixelSize: Math.round(11 * preferences.fontScale); color: Theme.muted; wrapMode: Text.WordWrap }
        }
        onTriggered: { if (serverMenu.allowed.leaveGuild) serverMenu.confirmationRequested("Leave " + serverMenu.guildName + "?", "Are you sure you want to leave this server? You will need another invitation to rejoin.", "Leave Server", {service:"guild",action:"leaveGuild",guildId:serverMenu.guildId,targetId:serverMenu.guildId},false) }
    }
    MenuSeparator {}
    Menu {
        title: "Copy Server Info"
        MenuItem { text: "Copy Server ID"; onTriggered: memberActions.copyId(serverMenu.guildId) }
        MenuItem { text: "Copy Server Name"; onTriggered: chatText.copy(serverMenu.guildName) }
        MenuItem { text: "Copy Server URL"; onTriggered: chatText.copy("https://discord.com/channels/"+serverMenu.guildId) }
    }
    property GuildInviteDialog inviteDialog: GuildInviteDialog {}
}
