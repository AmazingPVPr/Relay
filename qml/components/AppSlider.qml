import QtQuick
import QtQuick.Controls
import "Theme.js" as Theme

Slider {
    id: control
    implicitHeight: 24
    leftPadding: 7; rightPadding: 7
    opacity: enabled ? 1 : 0.4
    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 4
        radius: 2
        color: Theme.border
        Rectangle { width: control.visualPosition * parent.width; height: parent.height; radius: 2; color: Theme.accent }
    }
    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: 14; height: 14; radius: 7
        color: Theme.text
        border.width: control.activeFocus ? 2 : 0
        border.color: Theme.focus
    }
}
