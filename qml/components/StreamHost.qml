import QtQuick
import "Theme.js" as Theme

Item {
    id: host
    property bool expanded: false
    property real floatingWidth: 400
    property real horizontalPosition: 1
    property real verticalPosition: 1
    readonly property real margin: 10
    readonly property real minimumWidth: Math.min(320, Math.max(1, width - margin * 2))
    readonly property real maximumWidth: Math.max(minimumWidth, width - margin * 2)
    property alias contentItem: panel

    function moveBy(dx, dy) {
        if (expanded) return
        const travelX = Math.max(0, width - panel.width - margin * 2)
        const travelY = Math.max(0, height - panel.height - margin * 2)
        horizontalPosition = travelX > 0 ? Math.max(0, Math.min(1, (panel.x + dx - margin) / travelX)) : 0
        verticalPosition = travelY > 0 ? Math.max(0, Math.min(1, (panel.y + dy - margin) / travelY)) : 0
    }
    function resizeBy(dx, dy) {
        if (expanded) return
        const change = Math.abs(dx) >= Math.abs(dy) ? dx : dy * 16 / 9
        floatingWidth = Math.max(minimumWidth, Math.min(maximumWidth, panel.width + change))
    }

    Rectangle {
        id: panel
        objectName: "streamPanel"
        width: host.expanded ? host.width : Math.max(host.minimumWidth, Math.min(host.maximumWidth, host.floatingWidth))
        height: host.expanded ? host.height : Math.min(Math.max(180, width * 9 / 16), Math.max(1, host.height - host.margin * 2))
        x: host.expanded ? 0 : host.margin + Math.max(0, host.width - width - host.margin * 2) * host.horizontalPosition
        y: host.expanded ? 0 : host.margin + Math.max(0, host.height - height - host.margin * 2) * host.verticalPosition
        color: "#0a0a0a"
        radius: host.expanded ? 0 : 6
        border.color: Theme.border
        border.width: host.expanded ? 0 : 1
        clip: true
    }
    MouseArea {
        objectName: "streamResizeHandle"
        visible: !host.expanded
        x: panel.x + panel.width - width
        y: panel.y + panel.height - height
        width: 20; height: 20; z: 5
        cursorShape: Qt.SizeFDiagCursor
        property point previousPosition
        onPressed: mouse => previousPosition = mapToItem(null, mouse.x, mouse.y)
        onPositionChanged: mouse => {
            if (!pressed) return
            const position = mapToItem(null, mouse.x, mouse.y)
            host.resizeBy(position.x - previousPosition.x, position.y - previousPosition.y)
            previousPosition = position
        }
        Canvas {
            anchors.fill: parent
            onPaint: {
                const context = getContext("2d")
                context.clearRect(0, 0, width, height)
                context.strokeStyle = Theme.secondary
                context.lineWidth = 1.5
                context.beginPath()
                context.moveTo(7, 16); context.lineTo(16, 7)
                context.moveTo(12, 16); context.lineTo(16, 12)
                context.stroke()
            }
        }
        Accessible.name: "Resize stream"
    }
}
