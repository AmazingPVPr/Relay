import QtQuick
import "Theme.js" as Theme

Rectangle {
    id: avatar
    property var user: ({})
    property bool showPresence: true
    property color surroundingColor: Theme.canvas
    implicitWidth: 40
    implicitHeight: 40
    radius: width / 2
    color: Theme.avatar
    Text { anchors.centerIn: parent; visible: !picture.ready; text: avatar.user.initials || String(avatar.user.name || "?").substring(0, 1); textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); font.weight: Font.DemiBold }
    RoundedImage { id: picture; anchors.fill: parent; source: avatar.user.avatarUrl || ""; radius: width / 2; decodeSize: 48 }
    Rectangle {
        anchors.right: parent.right; anchors.bottom: parent.bottom
        width: 14; height: 14; radius: 7
        visible: avatar.showPresence
        color: avatar.user.status === "online" ? Theme.success : avatar.user.status === "idle" ? Theme.warning : avatar.user.status === "dnd" ? Theme.danger : Theme.muted
        border.width: 3; border.color: avatar.surroundingColor
        Rectangle { anchors.centerIn: parent; width: 6; height: avatar.user.status === "dnd" ? 2 : 6; radius: height / 2; color: avatar.surroundingColor; visible: avatar.user.status === "dnd" || !avatar.user.status || avatar.user.status === "offline" || avatar.user.status === "unknown" }
        Rectangle { x: 2; y: 2; width: 7; height: 7; radius: 4; color: avatar.surroundingColor; visible: avatar.user.status === "idle" }
    }
}
