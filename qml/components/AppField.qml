import QtQuick
import "Theme.js" as Theme
import QtQuick.Controls

TextField {
    id: control
    implicitHeight: 44
    color: (preferences.highContrast ? "#ffffff" : Theme.text)
    placeholderTextColor: (preferences.highContrast ? "#c7c7cf" : Theme.muted)
    selectionColor: Theme.selected
    selectedTextColor: "#ffffff"
    font.pixelSize: Math.round(14 * preferences.fontScale)
    leftPadding: 14
    rightPadding: 14
    selectByMouse: true
    background: Rectangle {
        radius: 4
        color: (preferences.highContrast ? "#0b0b0e" : Theme.rail)
        border.color: control.activeFocus ? Theme.focus : "transparent"
        border.width: 1
    }
}
