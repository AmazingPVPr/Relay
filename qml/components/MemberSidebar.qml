import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme
import "MemberGroups.js" as MemberGroups

Rectangle {
    id: sidebar
    signal userRequested(string id, var user)
    signal contextRequested(string id, var user)
    property var groupedMembers: []
    property var groupLabels: ({})
    property int onlineCount: 0
    property int offlineCount: 0
    property int voiceCount: 0
    function rebuild() {
        const members = []
        for (let i = 0; i < discord.members.count; ++i) members.push(discord.members.get(i))
        const grouped = MemberGroups.groupMembers(members)
        groupLabels = grouped.labels
        groupedMembers = grouped.members
        onlineCount = grouped.onlineCount; offlineCount = grouped.offlineCount; voiceCount = grouped.voiceCount
    }
    Component.onCompleted: rebuild()
    Connections { target: discord.members; function onRowsInserted() { Qt.callLater(sidebar.rebuild) } function onRowsRemoved() { Qt.callLater(sidebar.rebuild) } function onRowsMoved() { Qt.callLater(sidebar.rebuild) } function onModelReset() { Qt.callLater(sidebar.rebuild) } function onDataChanged() { Qt.callLater(sidebar.rebuild) } function onCountChanged() { Qt.callLater(sidebar.rebuild) } }
    color: (preferences.highContrast ? "#151519" : Theme.sidebar)
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            height: 48
            color: (preferences.highContrast ? "#151519" : Theme.sidebar)
            Text {
                anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16
                verticalAlignment: Text.AlignVCenter
                text: "Members" + (discord.memberCount > 0 ? " — " + discord.memberCount : "")
                color: (preferences.highContrast ? "#e4e4e8" : Theme.secondary); font.pixelSize: Math.round(12 * preferences.fontScale); font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.surface }
        }
        Text {
            Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; Layout.topMargin: 10
            visible: discord.members.count > 0
            text: (discord.membersPartial ? "Loaded: " : "") + sidebar.onlineCount + " online · " + sidebar.offlineCount + " offline · " + sidebar.voiceCount + " in voice"
            color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(10 * preferences.fontScale); wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; Layout.topMargin: visible ? 10 : 0; Layout.bottomMargin: visible ? 6 : 0
            visible: discord.membersLoading || discord.membersPartial || discord.members.count === 0
            text: discord.membersStatus || (discord.membersLoading ? "Loading members…" : "No members available.")
            textFormat: Text.PlainText; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap
        }
        ListView {
            boundsBehavior: Flickable.StopAtBounds
            boundsMovement: Flickable.StopAtBounds
            id: memberList
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.leftMargin: 8; Layout.rightMargin: 8
            clip: true; reuseItems: true
            model: sidebar.groupedMembers
            section.property: "groupKey"
            section.delegate: Text {
                required property string section
                width: memberList.width
                height: 32
                leftPadding: 6; topPadding: 13
                text: sidebar.groupLabels[section] || "Members"
                textFormat: Text.PlainText; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(10 * preferences.fontScale); font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            delegate: Button {
                id: member
                required property var modelData
                readonly property var model: modelData
                opacity: model.offline ? 0.5 : 1
                width: memberList.width
                height: Math.max(preferences.uiDensity === "compact" ? 37 : preferences.uiDensity === "spacious" ? 49 : 43, Math.round(35 * preferences.fontScale))
                leftPadding: 6; rightPadding: 6
                hoverEnabled: true
                background: Rectangle { radius: 4; color: member.hovered ? Theme.hover : "transparent" }
                contentItem: RowLayout {
                    spacing: 5
                    Rectangle {
                        width: 30; height: 30; radius: 15; color: Theme.avatar
                        border.color: Theme.success; border.width: model.speaking ? 2 : 0
                        Text { anchors.centerIn: parent; visible: !avatar.ready; text: model.initials || String(model.name || "?").substring(0, 1); color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(11 * preferences.fontScale); textFormat: Text.PlainText }
                        RoundedImage { id: avatar; anchors.fill: parent; anchors.margins: model.speaking ? 2 : 0; radius: 15; source: model.avatarUrl || "" }
                        Rectangle {
                            visible: model.status && model.status !== "unknown"
                            anchors.right: parent.right; anchors.bottom: parent.bottom
                            width: 10; height: 10; radius: 5
                            color: model.status === "online" ? Theme.success : model.status === "idle" ? Theme.warning : model.status === "dnd" ? Theme.danger : (preferences.highContrast ? "#c7c7cf" : Theme.muted)
                            border.width: 2; border.color: (preferences.highContrast ? "#151519" : Theme.sidebar)
                        }
                    }
                    Text { Layout.fillWidth: true; text: model.name; textFormat: Text.PlainText; color: (preferences.roleColorsMode === "names" ? model.roleColor : "") || (model.status === "offline" ? (preferences.highContrast ? "#c7c7cf" : Theme.muted) : (preferences.highContrast ? "#e4e4e8" : Theme.secondary)); font.pixelSize: Math.round(13 * preferences.fontScale); elide: Text.ElideRight }
                    Rectangle { visible: preferences.roleColorsMode === "dots" && !!model.roleColor; width: 7; height: 7; radius: 3.5; color: model.roleColor || (preferences.highContrast ? "#c7c7cf" : Theme.muted) }
                    GuildTag { guild: model.primaryGuild || ({}) }
                    AppBadge { visible: !!model.bot; verified: !!model.verifiedBot }
                    AppIcon { visible: !!model.streaming || !!model.channelId; name: model.streaming ? "video" : "voice"; color: model.streaming ? Theme.danger : Theme.success; Layout.preferredWidth: 16; Layout.preferredHeight: 16 }
                    AppIcon { visible: !!model.muted; name: "micOff"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                    AppIcon { visible: !!model.deafened; name: "deafened"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                }
                onClicked: sidebar.userRequested(model.recordId, model)
                TapHandler { acceptedButtons: Qt.RightButton; onTapped: sidebar.contextRequested(member.model.recordId, member.model) }
                ToolTip.visible: hovered; ToolTip.delay: 700
                ToolTip.text: model.name + (model.streaming ? " · Streaming" : model.channelId ? " · In voice" : "")
            }
            ScrollBoost { flickable: memberList }
            ScrollBar.vertical: ScrollBar { width: 5 }
        }
    }
}
