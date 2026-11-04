import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Popup {
    id: card
    signal watchRequested(string key, string title)
    signal messageRequested(string userId)
    property var user: ({})
    property string userId: ""
    property var contextUser: ({})
    readonly property bool ownProfile: userId.length > 0 && userId === discord.currentUserId
    parent: Overlay.overlay
    width: Math.min(340, parent.width - 32)
    height: Math.min(content.implicitHeight + 32, parent.height - 40)
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    padding: 16
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.rail; radius: 8; border.color: Theme.border }
    function showUser(id, fallback) {
        userId = id
        contextUser = fallback || {}
        refreshUser()
        open()
        if (id.length && !discord.demoMode) {
            if (contextUser.guildId !== undefined) discord.fetchProfileForGuild(id,contextUser.guildId)
            else discord.fetchProfile(id)
        }
    }
    function refreshUser() {
        const known = (contextUser.guildId !== undefined ? discord.memberInfoForGuild(userId,contextUser.guildId) : discord.memberInfo(userId)) || {}
        const value = Object.assign({}, contextUser, known)
        if (contextUser.guildId !== undefined && contextUser.guildId !== known.guildId) {
            for (let key of ["guildId","name","avatarUrl","roleColor","roleName","roleNames"]) {
                if (contextUser[key] !== undefined) value[key] = contextUser[key]
                else if (key === "roleNames") value[key] = []
                else if (key === "roleName" || key === "roleColor") value[key] = ""
            }
        }
        user = value
    }
    Connections { target: discord; function onProfileUpdated(id) { if (card.visible && id === card.userId) card.refreshUser() } }
    contentItem: HardScrollView {
        id: profileScroll
        contentWidth: availableWidth
        ColumnLayout {
            id: content
            width: profileScroll.availableWidth
            spacing: 12
            Image { Layout.fillWidth: true; Layout.preferredHeight: 96; visible: source.toString().length > 0; source: card.user.bannerUrl || ""; asynchronous: true; sourceSize: Qt.size(1024,512); fillMode: Image.PreserveAspectCrop; clip: true }
            RowLayout {
                Rectangle {
                    width: 72; height: 72; radius: 36; color: Theme.avatar
                    Text { anchors.centerIn: parent; visible: !avatar.ready; text: String(card.user.name || "?").substring(0,2).toUpperCase(); textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 24 }
                    RoundedImage { id: avatar; anchors.fill: parent; source: card.user.avatarUrl || "" }
                    Rectangle { width: 16; height: 16; radius: 8; anchors.right: parent.right; anchors.bottom: parent.bottom; border.color: Theme.rail; border.width: 3; visible: !!card.user.status && card.user.status !== "unknown"; color: card.user.status === "online" ? Theme.success : card.user.status === "idle" ? Theme.warning : card.user.status === "dnd" ? Theme.danger : Theme.muted }
                }
                Item { Layout.fillWidth: true }
                ActionButton { iconName: "close"; hint: "Close profile"; quiet: true; onClicked: card.close() }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: card.user.name || "Unknown user"; color: card.user.roleColor || Theme.text; font.pixelSize: 20; font.weight: Font.DemiBold; textFormat: Text.PlainText; wrapMode: Text.Wrap }
                AppBadge { visible: !!card.user.bot; verified: !!card.user.verifiedBot }
                GuildTag { guild: card.user.primaryGuild || ({}) }
            }
            Text { Layout.fillWidth: true; visible: !!card.user.username; text: card.user.username || ""; color: Theme.secondary; font.pixelSize: 13; textFormat: Text.PlainText; elide: Text.ElideRight }
            Text { visible: !!card.user.status && card.user.status !== "unknown"; text: card.user.status === "dnd" ? "Do not disturb" : card.user.status === "idle" ? "Idle" : card.user.status === "online" ? "Online" : "Offline"; color: Theme.muted; font.pixelSize: 12 }
            Text { Layout.fillWidth: true; visible: !!card.user.pronouns; text: card.user.pronouns || ""; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: 12 }
            Text { Layout.fillWidth: true; visible: !!card.user.createdAt; text: card.user.createdAt ? "Member since " + Qt.formatDateTime(new Date(card.user.createdAt), "MMM d, yyyy") : ""; color: Theme.muted; font.pixelSize: 12 }
            Text { Layout.fillWidth: true; visible: card.user.mutualGuildCount !== undefined || card.user.mutualFriendCount !== undefined; text: (card.user.mutualFriendCount !== undefined ? card.user.mutualFriendCount + " mutual friends" : "") + (card.user.mutualFriendCount !== undefined && card.user.mutualGuildCount !== undefined ? " · " : "") + (card.user.mutualGuildCount !== undefined ? card.user.mutualGuildCount + " mutual servers" : ""); color: Theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap }
            LinkText { Layout.fillWidth: true; rawText: card.user.bio || ""; color: Theme.secondary; font.pixelSize: 13 }
            Text { Layout.fillWidth: true; visible: (card.user.roleNames || []).length > 0; text: (card.user.roleNames || []).join(" · "); textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: 12; wrapMode: Text.Wrap }
            Rectangle {
                Layout.fillWidth: true
                visible: !!card.user.channelName
                implicitHeight: voiceInfo.implicitHeight + 24
                radius: 6; color: Theme.surface
                ColumnLayout {
                    id: voiceInfo
                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12; y: 12; spacing: 8
                    Text { Layout.fillWidth: true; text: (card.user.streaming ? "Streaming · " : "In voice · ") + String(card.user.channelName || ""); textFormat: Text.PlainText; color: card.user.streaming ? Theme.success : Theme.text; font.pixelSize: 13; wrapMode: Text.Wrap }
                    ActionButton { Layout.fillWidth: true; visible: !card.ownProfile && !!card.user.streaming; enabled: voice.connected && voice.channelId === card.user.channelId; text: enabled ? "Watch stream" : "Join their voice channel to watch"; onClicked: { card.watchRequested(card.user.streamKey, card.user.name + "’s stream"); card.close() } }
                }
            }
            ActionButton { Layout.fillWidth: true; visible: !card.ownProfile && card.userId.length > 0; text: "Message"; onClicked: { discord.openDirectMessage(card.userId); card.messageRequested(card.userId); card.close() } }
        }
    }
}
