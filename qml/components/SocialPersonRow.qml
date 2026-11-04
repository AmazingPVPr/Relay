import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Rectangle {
    id: row
    objectName: "socialPersonRow"
    property var person: ({})
    property string mode: "friend"
    property bool busy: false
    property bool selected: false
    signal activated()
    signal contextRequested()
    signal accepted()
    signal dismissed()
    implicitHeight: Math.max(68, 52 * preferences.fontScale)
    color: selected || rowHover.hovered ? Theme.hover : "transparent"
    radius: 5
    HoverHandler { id: rowHover }
    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border; opacity: rowHover.hovered || row.selected ? 0 : 0.5 }
    RowLayout {
        anchors.fill: parent; anchors.leftMargin: 9; anchors.rightMargin: 9; spacing: 8
        Button {
            id: identity
            Layout.fillWidth: true; Layout.fillHeight: true
            padding: 0; leftPadding: 0; rightPadding: 0
            hoverEnabled: true; background: Item {}
            Accessible.name: (row.person.name || row.person.username || "User") + (row.mode === "request" ? ", view message request" : ", open conversation")
            contentItem: RowLayout {
                spacing: 12
                SocialAvatar { Layout.preferredWidth: 38; Layout.preferredHeight: 38; user: row.person; showPresence: row.mode !== "request"; surroundingColor: row.selected || rowHover.hovered ? Theme.hover : Theme.canvas }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 3
                    RowLayout {
                        Layout.fillWidth: true; spacing: 5
                        Text { Layout.maximumWidth: Math.max(30, identity.width - 130); text: row.person.name || row.person.username || "Unknown user"; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); font.weight: Font.DemiBold; elide: Text.ElideRight }
                        GuildTag { visible: !!row.person.primaryGuild && !!row.person.primaryGuild.tag; guild: row.person.primaryGuild || ({}) }
                        AppBadge { visible: !!row.person.bot; verified: !!row.person.verifiedBot }
                        PresenceIcons { visible: row.mode === "friend"; user: row.person }
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 4
                        AppIcon { visible: row.mode === "friend" && !!row.person.inVoice; name: "voice"; color: Theme.success; Layout.preferredWidth: 13; Layout.preferredHeight: 13 }
                        Text {
                            Layout.fillWidth: true
                            text: row.mode === "pending" ? (row.person.direction === "incoming" ? "Incoming Friend Request" : "Outgoing Friend Request") : row.mode === "request" ? (row.person.preview || row.person.username || "Message request") : row.person.inVoice ? "In voice" : row.person.activity || ({online:"Online",idle:"Idle",dnd:"Do Not Disturb",offline:"Offline"})[row.person.status] || "Offline"
                            textFormat: Text.PlainText; elide: Text.ElideRight
                            color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale)
                        }
                    }
                }
            }
            onClicked: row.activated()
        }
        Button {
            visible: row.mode === "pending" && row.person.direction === "incoming"
            enabled: !row.busy; implicitWidth: 34; implicitHeight: 34; padding: 7; hoverEnabled: true
            Accessible.name: "Accept friend request"
            background: Rectangle { radius: 17; color: parent.hovered ? Theme.selected : Theme.surface }
            contentItem: SocialIcon { name: "check"; color: parent.hovered ? Theme.success : Theme.secondary }
            onClicked: row.accepted()
            ToolTip.visible: hovered; ToolTip.text: "Accept"; ToolTip.delay: 450
        }
        Button {
            visible: row.mode === "pending"
            enabled: !row.busy; implicitWidth: 34; implicitHeight: 34; padding: 7; hoverEnabled: true
            Accessible.name: row.person.direction === "incoming" ? "Ignore friend request" : "Cancel friend request"
            background: Rectangle { radius: 17; color: parent.hovered ? Theme.selected : Theme.surface }
            contentItem: SocialIcon { name: "close"; color: parent.hovered ? Theme.danger : Theme.secondary }
            onClicked: row.dismissed()
            ToolTip.visible: hovered; ToolTip.text: row.person.direction === "incoming" ? "Ignore" : "Cancel"; ToolTip.delay: 450
        }
        ActionButton { visible: row.mode === "friend"; iconName: "chat"; hint: "Message"; quiet: true; implicitWidth: 34; implicitHeight: 34; padding: 7; onClicked: row.activated() }
        Button {
            visible: row.mode !== "pending"
            implicitWidth: 30; implicitHeight: 34; padding: 7; hoverEnabled: true
            Accessible.name: "More options"
            background: Rectangle { radius: 17; color: parent.hovered ? Theme.selected : "transparent" }
            contentItem: SocialIcon { name: "more" }
            onClicked: row.contextRequested()
            ToolTip.visible: hovered; ToolTip.text: "More"; ToolTip.delay: 450
        }
    }
    TapHandler { acceptedButtons: Qt.RightButton; onTapped: row.contextRequested() }
}
