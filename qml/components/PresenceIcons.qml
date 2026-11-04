import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

RowLayout {
    id: icons
    property var user: ({})
    spacing: 3
    function statusColor(status) { return status === "online" ? Theme.success : status === "idle" ? Theme.warning : status === "dnd" ? Theme.danger : Theme.muted }
    Repeater {
        model: [{name: "desktop", label: "Desktop", status: icons.user.desktopStatus || "unknown"}, {name: "mobile", label: "Mobile", status: icons.user.mobileStatus || "unknown"}, {name: "web", label: "Web", status: icons.user.webStatus || "unknown"}]
        delegate: Item {
            required property var modelData
            visible: modelData.status !== "unknown" && modelData.status !== "offline"
            Layout.preferredWidth: visible ? 13 : 0; Layout.preferredHeight: 14
            AppIcon { anchors.fill: parent; name: modelData.name; color: icons.statusColor(modelData.status) }
            HoverHandler { id: deviceHover }
            ToolTip.visible: deviceHover.hovered; ToolTip.delay: 650
            ToolTip.text: modelData.label + " · " + (modelData.status === "dnd" ? "Do not disturb" : modelData.status === "idle" ? "Idle" : "Online")
        }
    }
}
