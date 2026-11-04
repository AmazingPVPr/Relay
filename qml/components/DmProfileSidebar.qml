import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Rectangle {
    id: sidebar
    color: Theme.sidebar
    property string userId: ""
    property var user: ({})
    property var recipients: []
    signal userRequested(string id, var user)
    signal contextRequested(string id, var user)
    function refresh() {
        if (!visible || discord.currentChannelKind !== "dm") return
        const people = []
        for (let i = 0; i < discord.members.count; ++i) {
            const member = discord.members.get(i)
            if (member.recordId !== discord.currentUserId) people.push(member)
        }
        recipients = people
        let selected = people.find(person => person.recordId === userId)
        if (!selected) selected = people.length ? people[0] : null
        const nextId = selected ? selected.recordId : ""
        const changed = userId !== nextId
        userId = nextId
        user = nextId ? Object.assign({}, selected, discord.memberInfo(nextId)) : ({})
        if (changed && nextId && !discord.demoMode) discord.fetchProfile(nextId)
    }
    function memberDate() {
        let date
        if (user.createdAt) date = new Date(user.createdAt)
        else if (/^\d{15,21}$/.test(userId)) date = new Date(Math.floor(Number(userId) / 4194304) + 1420070400000)
        return date && Number.isFinite(date.getTime()) ? Qt.formatDate(date, "MMM d, yyyy") : ""
    }
    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) Qt.callLater(refresh)
    Connections {
        target: discord
        function onStateChanged() { Qt.callLater(sidebar.refresh) }
        function onProfileUpdated(id) { if (id === sidebar.userId) sidebar.refresh() }
    }
    Connections {
        target: discord.members
        function onRowsInserted() { Qt.callLater(sidebar.refresh) }
        function onRowsRemoved() { Qt.callLater(sidebar.refresh) }
        function onRowsMoved() { Qt.callLater(sidebar.refresh) }
        function onModelReset() { Qt.callLater(sidebar.refresh) }
        function onDataChanged() { Qt.callLater(sidebar.refresh) }
        function onCountChanged() { Qt.callLater(sidebar.refresh) }
    }
    HardScrollView {
        id: profileScroll
        anchors.fill: parent; anchors.leftMargin: 14; anchors.topMargin: 16; anchors.bottomMargin: 16
        ColumnLayout {
            width: profileScroll.availableWidth
            spacing: 14
            ComboBox {
                Layout.fillWidth: true
                visible: sidebar.recipients.length > 1
                model: sidebar.recipients; textRole: "name"
                onActivated: index => { sidebar.userId = sidebar.recipients[index].recordId; sidebar.refresh(); if (!discord.demoMode) discord.fetchProfile(sidebar.userId) }
            }
            Image {
                Layout.fillWidth: true; Layout.preferredHeight: Math.min(120, width * 0.5)
                visible: source.toString().length > 0
                source: sidebar.user.bannerUrl || ""; asynchronous: true
                sourceSize: Qt.size(1024, 512); fillMode: Image.PreserveAspectCrop; clip: true
            }
            Rectangle {
                visible: sidebar.userId.length > 0
                width: 76; height: 76; radius: 38; color: Theme.avatar
                Text { anchors.centerIn: parent; visible: !avatar.ready; text: String(sidebar.user.name || "?").substring(0, 2).toUpperCase(); textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 26 }
                RoundedImage { id: avatar; anchors.fill: parent; source: sidebar.user.avatarUrl || ""; radius: 38 }
                Rectangle {
                    width: 18; height: 18; radius: 9; anchors.right: parent.right; anchors.bottom: parent.bottom
                    visible: !!sidebar.user.status && sidebar.user.status !== "unknown"
                    color: sidebar.user.status === "online" ? Theme.success : sidebar.user.status === "idle" ? Theme.warning : sidebar.user.status === "dnd" ? Theme.danger : Theme.muted
                    border.width: 3; border.color: Theme.sidebar
                }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; acceptedButtons: Qt.LeftButton | Qt.RightButton; onClicked: mouse => mouse.button === Qt.RightButton ? sidebar.contextRequested(sidebar.userId, sidebar.user) : sidebar.userRequested(sidebar.userId, sidebar.user) }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 5
                Text { Layout.fillWidth: true; text: sidebar.user.name || "Profile"; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.text; font.pixelSize: Math.round(20 * preferences.fontScale); font.weight: Font.DemiBold }
                Text { Layout.fillWidth: true; visible: text.length > 0; text: sidebar.user.username || ""; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
                Text { Layout.fillWidth: true; visible: text.length > 0; text: sidebar.user.pronouns || ""; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale) }
                RowLayout {
                    Layout.fillWidth: true; spacing: 6
                    GuildTag { guild: sidebar.user.primaryGuild || ({}) }
                    AppBadge { visible: !!sidebar.user.bot; verified: !!sidebar.user.verifiedBot }
                    Item { Layout.fillWidth: true }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; visible: sidebar.userId.length > 0 }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 7; visible: !!sidebar.user.bio
                Text { text: "About me"; color: Theme.text; font.pixelSize: Math.round(11 * preferences.fontScale); font.weight: Font.DemiBold }
                LinkText { Layout.fillWidth: true; rawText: sidebar.user.bio || ""; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 7; visible: sidebar.memberDate().length > 0
                Text { text: "Discord member since"; color: Theme.text; font.pixelSize: Math.round(11 * preferences.fontScale); font.weight: Font.DemiBold }
                Text { text: sidebar.memberDate(); color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
            }
            Text { Layout.fillWidth: true; visible: sidebar.user.mutualGuildCount !== undefined; text: (sidebar.user.mutualGuildCount || 0) + " mutual servers"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
            Text { Layout.fillWidth: true; visible: sidebar.user.mutualFriendCount !== undefined; text: (sidebar.user.mutualFriendCount || 0) + " mutual friends"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
            Text { Layout.fillWidth: true; visible: !!sidebar.user.profileLoading || !!sidebar.user.profileError; text: sidebar.user.profileLoading ? "Loading profile…" : sidebar.user.profileError || ""; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap }
            ActionButton { Layout.fillWidth: true; visible: sidebar.userId.length > 0; text: "View full profile"; quiet: true; onClicked: sidebar.userRequested(sidebar.userId, sidebar.user) }
            Text { Layout.fillWidth: true; visible: !sidebar.userId.length; text: "Loading profile…"; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        }
    }
}
