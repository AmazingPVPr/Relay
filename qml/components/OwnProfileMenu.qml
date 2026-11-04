import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Popup {
    id: ownProfile
    property var accountSwitcher
    readonly property bool accountSwitcherVisible: switcher.visible
    property var user: ({})
    property real anchorX: 12
    property real anchorY: 12
    property string userName: discord.currentUserName
    property string avatarUrl: discord.currentUserAvatarUrl
    property string presenceStatus: discord.currentPresenceStatus
    readonly property string statusLabel: presenceStatus === "idle" ? "Idle" : presenceStatus === "dnd" ? "Do Not Disturb" : presenceStatus === "invisible" ? "Invisible" : "Online"
    readonly property color statusColor: presenceStatus === "idle" ? Theme.warning : presenceStatus === "dnd" ? Theme.danger : presenceStatus === "invisible" ? Theme.muted : Theme.success
    readonly property var voiceGuild: voice.connected ? discord.guildInfo(voice.currentGuildId) : ({})
    signal editProfileRequested()
    signal viewProfileRequested()
    signal logoutRequested()
    parent: Overlay.overlay
    width: Math.min(340, parent.width - 24)
    height: Math.min(content.implicitHeight + 2, parent.height - 24)
    padding: 1
    x: Math.max(12, Math.min(parent.width - width - 12, anchorX))
    y: Math.max(12, Math.min(parent.height - height - 12, anchorY - height - 8))
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.surface; radius: 9; border.color: Theme.border }
    function refreshProfile() { user = discord.memberInfoForGuild(discord.currentUserId, "") || {} }
    function openFor(anchor) {
        refreshProfile()
        const position = anchor.mapToItem(parent, 0, 0)
        anchorX = position.x
        anchorY = position.y
        open()
        if (!discord.demoMode) discord.fetchProfileForGuild(discord.currentUserId, "")
    }
    Connections {
        target: discord
        function onProfileUpdated(id) { if (ownProfile.visible && id === discord.currentUserId) ownProfile.refreshProfile() }
        function onStateChanged() { if (ownProfile.visible) ownProfile.refreshProfile() }
    }
    component ProfileRow: Button {
        id: row
        property string iconName: ""
        property bool arrow: false
        property color iconColor: Theme.secondary
        implicitHeight: 46; padding: 12
        font.pixelSize: Math.round(13 * preferences.fontScale)
        background: Rectangle { radius: 5; color: row.hovered ? Theme.selected : "transparent" }
        contentItem: RowLayout {
            spacing: 10
            AppIcon { name: row.iconName; color: row.iconColor; Layout.preferredWidth: 18; Layout.preferredHeight: 18; visible: row.iconName.length > 0 }
            Text { Layout.fillWidth: true; text: row.text; textFormat: Text.PlainText; color: row.enabled ? Theme.secondary : Theme.muted; font: row.font; elide: Text.ElideRight }
            AppIcon { name: "chevron"; rotation: -90; visible: row.arrow; Layout.preferredWidth: 16; Layout.preferredHeight: 16 }
        }
    }
    contentItem: HardScrollView {
        id: scroll
        rightPadding: 0
        contentWidth: availableWidth
        ColumnLayout {
            id: content
            width: scroll.availableWidth
            spacing: 0
            Item {
                Layout.fillWidth: true; implicitHeight: 162
                Rectangle { width: parent.width; height: 116; color: Theme.accent; radius: 8 }
                Image { width: parent.width; height: 116; source: ownProfile.user.bannerUrl || ""; asynchronous: true; sourceSize: Qt.size(680,232); fillMode: Image.PreserveAspectCrop; clip: true }
                Rectangle {
                    x: 16; y: 72; width: 92; height: 92; radius: 46; color: Theme.surface
                    Rectangle {
                        anchors.centerIn: parent; width: 80; height: 80; radius: 40; color: Theme.avatar
                        Text { anchors.centerIn: parent; visible: !ownAvatar.ready; text: String(ownProfile.userName || "?").slice(0,2).toUpperCase(); textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 25 }
                        RoundedImage { id: ownAvatar; anchors.fill: parent; source: ownProfile.avatarUrl; radius: 40 }
                        Rectangle { anchors.right: parent.right; anchors.bottom: parent.bottom; width: 22; height: 22; radius: 11; border.color: Theme.surface; border.width: 4; color: ownProfile.statusColor }
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.margins: 16; Layout.topMargin: 10
                spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 4
                    Text { Layout.fillWidth: true; text: ownProfile.userName || "Your account"; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.text; font.pixelSize: Math.round(21 * preferences.fontScale); font.weight: Font.DemiBold }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 7
                        Text { Layout.fillWidth: true; text: ownProfile.user.username || ""; visible: text.length > 0; textFormat: Text.PlainText; elide: Text.ElideRight; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale) }
                        GuildTag { guild: ownProfile.user.primaryGuild || ({}) }
                        Item { Layout.fillWidth: true; visible: !ownProfile.user.username }
                    }
                    Text { Layout.fillWidth: true; visible: !!ownProfile.user.pronouns; text: ownProfile.user.pronouns || ""; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale) }
                }
                Text { Layout.fillWidth: true; visible: !!ownProfile.user.bio; text: ownProfile.user.bio || ""; textFormat: Text.PlainText; wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale) }
                ActionButton { visible: !!ownProfile.user.bio; text: "View Full Bio"; quiet: true; leftPadding: 0; rightPadding: 0; implicitHeight: 24; onClicked: { ownProfile.close(); ownProfile.viewProfileRequested() } }
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: voiceInfo.implicitHeight + 20; visible: voice.connected
                    radius: 7; color: Theme.raised
                    ColumnLayout {
                        id: voiceInfo
                        x: 10; y: 10; width: parent.width - 20; spacing: 10
                        Text { text: "In voice"; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
                        RowLayout {
                            Layout.fillWidth: true; spacing: 10
                            Rectangle {
                                width: 44; height: 44; radius: 8; color: Theme.surface
                                Text { anchors.centerIn: parent; visible: !guildAvatar.ready; text: String(ownProfile.voiceGuild.name || "?").slice(0,2).toUpperCase(); textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 15 }
                                RoundedImage { id: guildAvatar; anchors.fill: parent; source: ownProfile.voiceGuild.iconUrl || ""; radius: 8 }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 3
                                RowLayout { Layout.fillWidth: true; spacing: 5; AppIcon { name: "voice"; Layout.preferredWidth: 15; Layout.preferredHeight: 15 } Text { Layout.fillWidth: true; text: voice.channelName; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale); font.weight: Font.DemiBold; elide: Text.ElideRight } }
                                Text { Layout.fillWidth: true; text: ownProfile.voiceGuild.name ? "in " + ownProfile.voiceGuild.name : "Voice connected"; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); elide: Text.ElideRight }
                            }
                        }
                        ActionButton { Layout.fillWidth: true; text: "Open Voice"; onClicked: { discord.activateChannel(voice.channelId); ownProfile.close() } }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: profileRows.implicitHeight + 8; radius: 7; color: Theme.raised
                    ColumnLayout {
                        id: profileRows
                        x: 6; y: 4; width: parent.width - 12; spacing: 0
                        ProfileRow { Layout.fillWidth: true; text: "Edit Profile"; iconName: "edit"; onClicked: { ownProfile.close(); ownProfile.editProfileRequested() } }
                        Rectangle { Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; implicitHeight: 1; color: Theme.border }
                        ProfileRow {
                            id: statusRow
                            Layout.fillWidth: true; text: ownProfile.statusLabel; arrow: true; enabled: discord.connected && !discord.demoMode
                            leftPadding: 40
                            Rectangle { x: 14; anchors.verticalCenter: parent.verticalCenter; width: 12; height: 12; radius: 6; color: ownProfile.statusColor }
                            onClicked: presenceMenu.popup(statusRow, statusRow.width, 0)
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: accountRows.implicitHeight + 8; radius: 7; color: Theme.raised
                    ColumnLayout {
                        id: accountRows
                        x: 6; y: 4; width: parent.width - 12; spacing: 0
                        ProfileRow { Layout.fillWidth: true; text: "Switch Accounts"; iconName: "members"; arrow: true; enabled: !!ownProfile.accountSwitcher && !ownProfile.accountSwitcher.sessionChangeBlocked; onClicked: { ownProfile.close(); switcher.open() } }
                        Rectangle { Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; implicitHeight: 1; color: Theme.border }
                        ProfileRow { Layout.fillWidth: true; text: "Copy User ID"; iconName: "copy"; enabled: discord.currentUserId.length > 0; onClicked: { memberActions.copyId(discord.currentUserId); ownProfile.close() } }
                    }
                }
                Text { Layout.fillWidth: true; visible: !!ownProfile.accountSwitcher && ownProfile.accountSwitcher.sessionChangeBlocked; text: "Your account changes are being saved. Please wait before changing accounts."; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
            }
        }
    }
    property Menu presenceMenu: BoundedMenu {
        Repeater {
            model: [{key:"online",label:"Online"}, {key:"idle",label:"Idle"}, {key:"dnd",label:"Do Not Disturb"}, {key:"invisible",label:"Invisible"}]
            delegate: MenuItem {
                required property var modelData
                text: modelData.label; checkable: true; checked: ownProfile.presenceStatus === modelData.key
                onTriggered: if (discord.setPresenceStatus(modelData.key)) ownProfile.close()
            }
        }
    }
    AccountSwitcherDialog { id: switcher; accountSwitcher: ownProfile.accountSwitcher }
}
