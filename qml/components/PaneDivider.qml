import QtQuick
import "Theme.js" as Theme

Rectangle {
    id: divider
    property real value: 240
    property real minimum: 180
    property real maximum: 520
    property real direction: 1
    readonly property bool dragging: drag.pressed
    signal resizeRequested(real size)
    signal resizeFinished(real size)
    implicitWidth: 5
    color: drag.containsMouse || drag.pressed ? Theme.border : Theme.rail
    MouseArea {
        id: drag
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.SplitHCursor
        property real startPosition: 0
        property real startValue: 0
        property real requested: 0
        onPressed: mouse => {
            startPosition = mapToItem(null, mouse.x, mouse.y).x
            startValue = divider.value
            requested = startValue
        }
        onPositionChanged: mouse => {
            if (!pressed) return
            const delta = mapToItem(null, mouse.x, mouse.y).x - startPosition
            requested = Math.max(divider.minimum, Math.min(divider.maximum, startValue + delta * divider.direction))
            divider.resizeRequested(requested)
        }
        onReleased: divider.resizeFinished(requested)
        onCanceled: divider.resizeFinished(requested)
    }
}
