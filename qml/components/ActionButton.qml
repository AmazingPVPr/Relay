import QtQuick
import "Theme.js" as Theme
import QtQuick.Controls
import QtQuick.Layouts

Button {
    id: control
    property bool primary: false
    property bool quiet: false
    property bool destructive: false
    property string hint: ""
    property string iconName: ""
    implicitHeight: 36
    implicitWidth: Math.max(32, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: text.length > 0 ? 14 : 8
    rightPadding: leftPadding
    topPadding: 7
    bottomPadding: 7
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    opacity: enabled ? 1 : 0.45
    Accessible.name: hint.length ? hint : text
    background: Rectangle {
        radius: 4
        color: control.primary ? (control.down ? Theme.accentDown : control.hovered ? Theme.accentHover : Theme.accent)
                               : control.down ? Theme.selected : control.hovered ? Theme.selected : control.quiet ? "transparent" : (preferences.highContrast ? "#85858f" : Theme.border)
        border.width: control.activeFocus ? 2 : 0
        border.color: Theme.focus
    }
    contentItem: RowLayout {
      spacing: 8
      AppIcon {
        visible: control.iconName.length > 0
        name: control.iconName
        color: control.destructive ? Theme.danger : control.primary ? Theme.onAccent : (preferences.highContrast ? "#e4e4e8" : Theme.secondary)
        Layout.preferredWidth: 20; Layout.preferredHeight: 20
        Layout.alignment: Qt.AlignHCenter
      }
      Text {
        visible: control.text.length > 0
        text: control.text
        font.family: control.font.family
        font.pixelSize: Math.round(13 * preferences.fontScale)
        font.weight: Font.Medium
        color: control.destructive ? Theme.danger : control.primary ? Theme.onAccent : (preferences.highContrast ? "#ffffff" : Theme.text)
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        textFormat: Text.PlainText
        elide: Text.ElideRight
        Layout.fillWidth: true
      }
    }
    ToolTip.visible: hovered && hint.length > 0
    ToolTip.text: hint
    ToolTip.delay: 600
}
