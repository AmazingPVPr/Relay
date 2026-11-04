import QtQuick
import QtQuick.Controls
import "Theme.js" as Theme
Dialog {
    id: dialog
    parent: Overlay.overlay
    width: Math.min(680, parent.width - 64)
    height: Math.min(820, parent.height - 64)
    x: (parent.width - width) / 2; y: (parent.height - height) / 2
    modal: true; padding: 24
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
    contentItem: AppearanceSettings { }
    ActionButton { anchors.right: parent.right; anchors.top: parent.top; iconName: "close"; quiet: true; hint: "Close settings"; onClicked: dialog.close() }

}
