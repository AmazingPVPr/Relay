import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

BoundedMenu {
    id: contextMenu
    property string userId: ""
    property string guildId: ""
    property var user: ({})
    property bool allowMention: false
    readonly property bool dmMode: !guildId.length && !ownUser
    readonly property var relationship: { const users = discord.friends; return discord.relationshipInfo(userId) }
    readonly property string dmChannelId: { const revision = contextActions.revision; return contextActions.dmChannelForUser(userId) }
    readonly property var dmState: { const revision = contextActions.revision; return contextActions.dmInfo(dmChannelId) }
    property var inviteGuilds: []
    property bool editDirty: false
    property bool editSaving: false
    property string editingUserId: ""
    property string editingMode: "note"
    readonly property bool ownUser: userId === discord.currentUserId
    readonly property var localState: { const revision = memberActions.revision; return memberActions.state(userId) }
    readonly property var allowed: { const state = guildManagement.permissions; return guildId ? guildManagement.userPermissions(guildId, userId) : ({}) }
    property var voiceChannels: []
    property bool dmMenusShown: false
    property bool moveShown: false
    property bool rolesShown: false
    property bool timeoutShown: false
    readonly property bool modalDialogOpen: editDialog.visible || removeFriendDialog.visible || inviteDialog.visible
    signal profileRequested(string userId, var fallback)
    signal mentionRequested(string userId)
    signal confirmationRequested(string title, string description, string label, var payload, bool reason)
    function openForUser(id, fallback, guild) {
        if (!id) return
        userId = id
        guildId = guild || ""
        user = Object.assign({}, fallback || {}, discord.memberInfoForGuild(id, guildId))
        if (guildId) guildManagement.selectGuild(guildId)
        const servers = []; for (let i=0; i<discord.guilds.count; ++i) servers.push(discord.guilds.get(i)); inviteGuilds = servers
        syncSubmenus()
        popup()
    }
    function syncSubmenus() {
        if (dmMode !== dmMenusShown) {
            if (dmMode) {
                for (const pair of [{anchor:appsPosition, menu:appsMenu},{anchor:invitePosition, menu:inviteMenu},{anchor:mutePosition, menu:dmMuteMenu}]) {
                    for (let i=0; i<count; ++i) if (itemAt(i) === pair.anchor) { insertMenu(i+1,pair.menu); break }
                }
            } else { removeMenu(appsMenu); removeMenu(inviteMenu); removeMenu(dmMuteMenu) }
            dmMenusShown = dmMode
        }
        voiceChannels = guildId && guildManagement.guildId === guildId ? guildManagement.channels.filter(channel => Number(channel.type) === 2 && guildManagement.channelPermissions(guildId, channel.id).view).map(channel => ({recordId:channel.id,name:channel.name})) : []
        const move = !!allowed.move && !!user.channelId, roles = !!allowed.manageRoles, timeout = !!allowed.timeout
        if (move !== moveShown) { if (move) addMenu(moveMenu); else removeMenu(moveMenu); moveShown = move }
        if (roles !== rolesShown) { if (roles) addMenu(rolesMenu); else removeMenu(rolesMenu); rolesShown = roles }
        if (timeout !== timeoutShown) { if (timeout) addMenu(timeoutMenu); else removeMenu(timeoutMenu); timeoutShown = timeout }
    }
    Connections { target: guildManagement; function onChanged() { if (contextMenu.visible) Qt.callLater(contextMenu.syncSubmenus) } }
    function execute(action, values) { guildManagement.selectGuild(guildId); guildManagement.execute(action, userId, values || {}) }
    function confirm(action, title, description, label, values, reason) {
        confirmationRequested(title, description, label, {service: "guild", guildId: guildId, action: action, targetId: userId, values: values || {}}, reason)
    }
    MenuItem { text: "Mark As Read"; visible: contextMenu.dmMode; height: visible ? implicitHeight : 0; enabled: !!contextMenu.dmState.canMarkRead && !contextActions.busy; onTriggered: contextActions.markDmRead(contextMenu.dmChannelId) }
    MenuSeparator { visible: contextMenu.dmMode; height: visible ? implicitHeight : 0 }
    MenuItem { text: contextMenu.dmState.pinned ? "Unpin" : "Pin"; visible: contextMenu.dmMode; height: visible ? implicitHeight : 0; enabled: !!contextMenu.dmState.available; onTriggered: contextActions.setDmPinned(contextMenu.dmChannelId,!contextMenu.dmState.pinned) }
    MenuItem { text: contextMenu.dmState.favorite ? "Remove from Favorites" : "Add to Favorites"; visible: contextMenu.dmMode; height: visible ? implicitHeight : 0; enabled: !!contextMenu.dmState.available; onTriggered: contextActions.setDmFavorite(contextMenu.dmChannelId,!contextMenu.dmState.favorite) }
    MenuSeparator { visible: contextMenu.dmMode; height: visible ? implicitHeight : 0 }
    MenuItem { text: "Profile"; onTriggered: contextMenu.profileRequested(contextMenu.userId, contextMenu.user) }
    MenuItem { text: "Mention"; visible: contextMenu.allowMention; height: visible ? implicitHeight : 0; onTriggered: contextMenu.mentionRequested(contextMenu.userId) }
    MenuItem { text: "Message"; visible: !contextMenu.ownUser; height: visible ? implicitHeight : 0; onTriggered: discord.openDirectMessage(contextMenu.userId) }
    MenuItem {
        id: startCall; text: "Start a Call"; visible: contextMenu.dmMode; height: visible ? implicitHeight : 0
        enabled: typeof discord.startDirectCall === "function" && discord.connected && !discord.demoMode && !discord.directCallStarting && !contextMenu.user.bot && !contextMenu.relationship.blocked && !contextMenu.relationship.ignored
        Accessible.description: enabled ? "Call this person in Relay" : "Direct calls are not available for this conversation."
        onTriggered: if (typeof discord.startDirectCall === "function") discord.startDirectCall(contextMenu.userId)
    }
    MenuItem {
        text: "Add Note"; visible: contextMenu.dmMode; height: visible ? implicitHeight : 0; enabled: !discord.socialBusy
        onTriggered: { contextMenu.editingUserId = contextMenu.userId; contextMenu.editingMode = "note"; contextMenu.editDirty = false; contextMenu.editSaving = false; discord.clearSocialFeedback(); editText.text = discord.userNote(contextMenu.userId); editDialog.open(); discord.fetchUserNote(contextMenu.userId) }
    }
    MenuItem {
        text: "Change Friend Nickname"; visible: contextMenu.dmMode && !!contextMenu.relationship.friend; height: visible ? implicitHeight : 0; enabled: !discord.socialBusy
        onTriggered: { contextMenu.editingUserId = contextMenu.userId; contextMenu.editingMode = "nickname"; contextMenu.editDirty = false; contextMenu.editSaving = false; discord.clearSocialFeedback(); editText.text = contextMenu.relationship.nickname || ""; editDialog.open() }
    }
    MenuItem { text: "Close DM"; visible: contextMenu.dmMode; height: visible ? implicitHeight : 0; enabled: !!contextMenu.dmState.available && !contextActions.busy; onTriggered: contextActions.closeDm(contextMenu.dmChannelId) }
    MenuItem { text: "Mute microphone"; visible: contextMenu.ownUser; height: visible ? implicitHeight : 0; checkable: true; checked: voice.muted; onTriggered: voice.setMuted(!voice.muted) }
    MenuItem { text: "Deafen"; visible: contextMenu.ownUser; height: visible ? implicitHeight : 0; checkable: true; checked: voice.deafened; onTriggered: voice.setDeafened(!voice.deafened) }
    MenuSeparator { visible: !contextMenu.ownUser; height: visible ? implicitHeight : 0 }
    MenuItem { text: "Mute locally"; visible: !contextMenu.ownUser && !contextMenu.dmMode; height: visible ? implicitHeight : 0; checkable: true; checked: !!contextMenu.localState.muted; onTriggered: memberActions.setLocalMuted(contextMenu.userId, !contextMenu.localState.muted) }
    MenuItem {
        visible: !contextMenu.ownUser && (!contextMenu.dmMode || !!contextMenu.user.inVoice); height: visible ? 86 : 0
        contentItem: ColumnLayout {
            spacing: 5
            RowLayout {
                Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: "Voice volume"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
                NumericValue { objectName: "personVoiceVolume"; value: Number(contextMenu.localState.volume === undefined ? 1 : contextMenu.localState.volume) * 100; minimum: 0; maximum: 200; onValueEdited: value => memberActions.setLocalVolume(contextMenu.userId, value / 100) }
            }
            AppSlider { objectName: "personVoiceVolumeSlider"; Layout.fillWidth: true; from: 0; to: 2; stepSize: 0.05; value: Number(contextMenu.localState.volume === undefined ? 1 : contextMenu.localState.volume); onMoved: memberActions.setLocalVolume(contextMenu.userId, value); Accessible.name: "Voice volume"; Accessible.description: "This person’s speech volume, only for you" }
        }
    }
    MenuItem {
        visible: !contextMenu.ownUser && (!contextMenu.dmMode || !!contextMenu.user.inVoice); height: visible ? 86 : 0
        contentItem: ColumnLayout {
            spacing: 5
            RowLayout {
                Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: "Soundboard volume"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
                NumericValue { objectName: "personSoundboardVolume"; value: Number(contextMenu.localState.soundboardVolume === undefined ? 1 : contextMenu.localState.soundboardVolume) * 100; minimum: 0; maximum: 200; onValueEdited: value => memberActions.setLocalSoundboardVolume(contextMenu.userId, value / 100); Accessible.name: "Soundboard volume percent" }
            }
            AppSlider { objectName: "personSoundboardVolumeSlider"; Layout.fillWidth: true; from: 0; to: 2; stepSize: 0.05; value: Number(contextMenu.localState.soundboardVolume === undefined ? 1 : contextMenu.localState.soundboardVolume); onMoved: memberActions.setLocalSoundboardVolume(contextMenu.userId, value); Accessible.name: "Soundboard volume"; Accessible.description: "This person’s soundboard sounds, only for you" }
        }
    }
    MenuItem { id: appsPosition; visible: false; height: 0 }
    property Menu appsMenu: BoundedMenu {
        title: "Apps"
        MenuItem { text: "No apps available in this conversation"; enabled: false }
    }
    MenuItem { id: invitePosition; visible: false; height: 0 }
    property Menu inviteMenu: BoundedMenu {
        title: "Invite to Server"
        Repeater { model: contextMenu.inviteGuilds; delegate: MenuItem { required property var modelData; text: modelData.name; onTriggered: contextMenu.inviteDialog.openFor(modelData.recordId,modelData.name,contextMenu.user.name || "this person") } }
        MenuItem { text: "No servers available"; visible: !contextMenu.inviteGuilds.length; height: visible ? implicitHeight : 0; enabled: false }
    }
    MenuItem { text: "Remove Friend"; visible: !contextMenu.ownUser && !!contextMenu.relationship.friend; height: visible ? implicitHeight : 0; enabled: !discord.socialBusy; onTriggered: removeFriendDialog.open() }
    MenuItem { text: contextMenu.relationship.outgoing ? "Cancel Friend Request" : contextMenu.relationship.incoming ? "Accept Friend Request" : "Add Friend"; visible: !contextMenu.ownUser && !contextMenu.relationship.friend && !contextMenu.user.bot && !contextMenu.relationship.blocked; height: visible ? implicitHeight : 0; enabled: !discord.socialBusy; onTriggered: contextMenu.relationship.outgoing ? discord.dismissFriendRequest(contextMenu.userId) : contextMenu.relationship.incoming ? discord.acceptFriendRequest(contextMenu.userId) : discord.addFriend(contextMenu.userId) }
    MenuItem { text: contextMenu.relationship.ignored ? "Unignore" : "Ignore"; visible: !contextMenu.ownUser; height: visible ? implicitHeight : 0; enabled: !discord.socialBusy; onTriggered: discord.setUserIgnored(contextMenu.userId,!contextMenu.relationship.ignored) }
    MenuItem {
        id: blockAction
        text: contextMenu.localState.blocked ? "Unblock" : "Block"
        contentItem: Text { text: blockAction.text; font: blockAction.font; color: contextMenu.localState.blocked ? Theme.text : Theme.danger; elide: Text.ElideRight }
        visible: !contextMenu.ownUser; height: visible ? implicitHeight : 0; enabled: !memberActions.busy
        onTriggered: {
            if (contextMenu.localState.blocked) memberActions.setBlocked(contextMenu.userId, false)
            else contextMenu.confirmationRequested("Block " + (contextMenu.user.name || "user") + "?", "This updates your Discord block list.", "Block", {service: "member", action: "block", targetId: contextMenu.userId}, false)
        }
    }
    MenuSeparator { visible: contextMenu.dmMode; height: visible ? implicitHeight : 0 }
    MenuItem { id: mutePosition; visible: false; height: 0 }
    property Menu dmMuteMenu: BoundedMenu {
        title: contextMenu.dmState.muted ? "Unmute @" + (contextMenu.user.name || "user") : "Mute @" + (contextMenu.user.name || "user")
        enabled: !!contextMenu.dmState.available && !contextActions.busy
        MenuItem { text: "Unmute"; visible: !!contextMenu.dmState.muted; height: visible ? implicitHeight : 0; onTriggered: contextActions.muteDm(contextMenu.dmChannelId,-1) }
        Repeater { model: [{label:"15 Minutes",seconds:900},{label:"1 Hour",seconds:3600},{label:"3 Hours",seconds:10800},{label:"8 Hours",seconds:28800},{label:"24 Hours",seconds:86400},{label:"Until I turn it back on",seconds:0}]; delegate: MenuItem { required property var modelData; text: modelData.label; onTriggered: contextActions.muteDm(contextMenu.dmChannelId,modelData.seconds) } }
    }
    MenuSeparator { visible: !!contextMenu.allowed.mute || !!contextMenu.allowed.deafen || !!contextMenu.allowed.move || !!contextMenu.allowed.manageRoles || !!contextMenu.allowed.kick || !!contextMenu.allowed.ban || !!contextMenu.allowed.timeout; height: visible ? implicitHeight : 0 }
    MenuItem { text: "Server mute"; visible: !!contextMenu.allowed.mute && !!contextMenu.user.channelId; height: visible ? implicitHeight : 0; checkable: true; checked: !!contextMenu.allowed.serverMuted; enabled: !guildManagement.mutating; onTriggered: contextMenu.execute("serverMute", {enabled: !contextMenu.allowed.serverMuted}) }
    MenuItem { text: "Server deafen"; visible: !!contextMenu.allowed.deafen && !!contextMenu.user.channelId; height: visible ? implicitHeight : 0; checkable: true; checked: !!contextMenu.allowed.serverDeafened; enabled: !guildManagement.mutating; onTriggered: contextMenu.execute("serverDeafen", {enabled: !contextMenu.allowed.serverDeafened}) }
    property Menu moveMenu: BoundedMenu {
        title: "Move to"
        enabled: !guildManagement.mutating
        Repeater {
            model: contextMenu.voiceChannels
            delegate: MenuItem { required property var modelData; text: modelData.name; enabled: modelData.recordId !== contextMenu.user.channelId; onTriggered: contextMenu.execute("moveMember", {channelId: modelData.recordId}) }
        }
        MenuSeparator {}
        MenuItem { text: "Disconnect from voice"; onTriggered: contextMenu.confirm("moveMember", "Disconnect " + (contextMenu.user.name || "member") + "?", "They will be removed from their voice channel.", "Disconnect", {channelId: ""}, false) }
    }
    property Menu rolesMenu: BoundedMenu {
        title: "Roles"
        enabled: !guildManagement.mutating
        Repeater {
            model: guildManagement.roles
            delegate: MenuItem {
                required property var modelData
                text: modelData.name
                visible: !!modelData.assignable; height: visible ? implicitHeight : 0
                checkable: true
                checked: (contextMenu.allowed.roleIds || contextMenu.user.roleIds || []).indexOf(modelData.id) !== -1
                onTriggered: contextMenu.execute(checked ? "assignRole" : "removeRole", {roleId: modelData.id})
            }
        }
    }
    property Menu timeoutMenu: BoundedMenu {
        title: "Timeout"
        enabled: !guildManagement.mutating
        Repeater {
            model: [{label:"1 minute",seconds:60},{label:"5 minutes",seconds:300},{label:"10 minutes",seconds:600},{label:"1 hour",seconds:3600},{label:"1 day",seconds:86400},{label:"1 week",seconds:604800}]
            delegate: MenuItem {
                required property var modelData
                text: modelData.label
                onTriggered: contextMenu.confirm("timeoutMember", "Timeout " + (contextMenu.user.name || "member") + "?", "They will be timed out for " + modelData.label + ".", "Timeout", {until: new Date(Date.now() + modelData.seconds * 1000).toISOString()}, true)
            }
        }
        MenuSeparator {}
        MenuItem { text: "Remove timeout"; onTriggered: contextMenu.execute("timeoutMember", {until: ""}) }
    }
    MenuItem { text: "Kick from server"; visible: !!contextMenu.allowed.kick; height: visible ? implicitHeight : 0; enabled: !guildManagement.mutating; onTriggered: contextMenu.confirm("kickMember", "Kick " + (contextMenu.user.name || "member") + "?", "They will be removed from this server and can return with an invitation.", "Kick", {}, true) }
    MenuItem { text: "Ban from server"; visible: !!contextMenu.allowed.ban; height: visible ? implicitHeight : 0; enabled: !guildManagement.mutating; onTriggered: contextMenu.confirm("banMember", "Ban " + (contextMenu.user.name || "member") + "?", "They will be banned from this server. Their existing messages will be kept.", "Ban", {deleteMessageSeconds: 0}, true) }
    MenuSeparator {}
    MenuItem { text: "Copy User ID"; onTriggered: memberActions.copyId(contextMenu.userId) }
    MenuItem { text: "Copy Channel ID"; visible: contextMenu.dmMode; height: visible ? implicitHeight : 0; enabled: contextMenu.dmChannelId.length > 0; onTriggered: memberActions.copyId(contextMenu.dmChannelId) }
    MenuItem { text: "Copy User URL"; onTriggered: chatText.copy("https://discord.com/users/"+contextMenu.userId) }
    property GuildInviteDialog inviteDialog: GuildInviteDialog {}
    property Dialog editDialog: Dialog {
        parent: Overlay.overlay; anchors.centerIn: parent; width: Math.min(440,parent ? parent.width - 32 : 440)
        modal: true; focus: true; title: contextMenu.editingMode === "note" ? "Add Note" : "Change Friend Nickname"
        background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 14
            Text { Layout.fillWidth: true; text: contextMenu.editingMode === "note" ? "Notes are only visible to you." : "Choose the name you see for this friend. Leave it empty to reset it."; wrapMode: Text.WordWrap; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale) }
            TextArea { id: editText; Layout.fillWidth: true; Layout.preferredHeight: contextMenu.editingMode === "note" ? 120 : 46; wrapMode: TextEdit.Wrap; selectByMouse: true; color: Theme.text; enabled: !discord.socialBusy; background: Rectangle { radius: 5; color: Theme.rail } onTextChanged: if (editDialog.visible) contextMenu.editDirty = true }
            Text { Layout.fillWidth: true; text: discord.socialError; visible: text.length > 0; color: Theme.danger; wrapMode: Text.WordWrap; font.pixelSize: Math.round(12 * preferences.fontScale) }
            RowLayout {
                Layout.fillWidth: true
                ActionButton { text: "Cancel"; quiet: true; onClicked: editDialog.close() }
                Item { Layout.fillWidth: true }
                ActionButton { text: "Save"; primary: true; enabled: !discord.socialBusy && editText.text.length <= (contextMenu.editingMode === "note" ? 256 : 32); onClicked: { const accepted = contextMenu.editingMode === "note" ? discord.setUserNote(contextMenu.editingUserId,editText.text) : discord.setFriendNickname(contextMenu.editingUserId,editText.text); if (accepted) { contextMenu.editSaving = true; if (!discord.socialBusy && !discord.socialError.length) editDialog.close() } } }
            }
        }
    }
    property Dialog removeFriendDialog: Dialog {
        parent: Overlay.overlay; anchors.centerIn: parent; width: Math.min(420,parent ? parent.width - 32 : 420)
        modal: true; focus: true; title: "Remove Friend"
        background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 18
            Text { Layout.fillWidth: true; text: "Remove " + String(contextMenu.user.name || "this person") + " from your friends?"; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
            RowLayout { Layout.fillWidth: true; Item { Layout.fillWidth: true } ActionButton { text: "Cancel"; quiet: true; onClicked: removeFriendDialog.close() } ActionButton { text: "Remove Friend"; destructive: true; enabled: !discord.socialBusy; onClicked: { if (discord.removeFriend(contextMenu.userId)) removeFriendDialog.close() } } }
        }
    }
    Connections { target: discord; function onSocialChanged() { if (contextMenu.editSaving && !discord.socialBusy) { contextMenu.editSaving = false; if (!discord.socialError.length) editDialog.close() } } function onUserNoteLoaded(id,note) { if (editDialog.visible && contextMenu.editingMode === "note" && contextMenu.editingUserId === id && !contextMenu.editDirty) { editText.text = note; contextMenu.editDirty = false } } }
}
