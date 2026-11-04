import QtQuick
import QtQuick.Controls
import "Theme.js" as Theme

Switch {
    id: control
    implicitWidth: 44
    implicitHeight: 28
    padding: 0
    opacity: enabled ? 1 : 0.4
    indicator: Rectangle {
        anchors.centerIn: parent
        width: 42; height: 24; radius: 12
        color: control.checked ? Theme.accent : Theme.border
        border.width: control.activeFocus ? 1 : 0
        border.color: Theme.focus
        Rectangle {
            width: 18; height: 18; radius: 9
            x: control.checked ? 21 : 3
            y: 3
            color: "#ffffff"
            Behavior on x { NumberAnimation { duration: preferences.reducedMotion ? 0 : 100 } }
        }
    }
}
