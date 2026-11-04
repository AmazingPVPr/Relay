import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

// Only AttachmentTray's validated, staged local images enter this viewer.
// Remote message media continues to use MediaViewer's Discord-host allowlist.
Dialog {
    id: dialog
    objectName: "draftImagePreview"
    property string imageSource: ""
    property string imageTitle: "Image"
    parent: Overlay.overlay
    width: Math.max(240, Math.min(1100, parent.width - 48))
    height: Math.max(200, Math.min(820, parent.height - 48))
    anchors.centerIn: parent
    modal: true
    padding: 16
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.rail; radius: 6; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: dialog.imageTitle
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Math.round(14 * preferences.fontScale)
                elide: Text.ElideMiddle
            }
            ActionButton { objectName: "draftImageClose"; iconName: "close"; hint: "Close image"; quiet: true; onClicked: dialog.close() }
        }
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Image {
                id: image
                objectName: "draftPreviewImage"
                anchors.fill: parent
                source: dialog.visible ? dialog.imageSource : ""
                asynchronous: true
                cache: false
                sourceSize: Qt.size(4096, 4096)
                fillMode: Image.PreserveAspectFit
                autoTransform: true
            }
            BusyIndicator { anchors.centerIn: parent; running: dialog.visible && image.status === Image.Loading; visible: running }
            Text { anchors.centerIn: parent; visible: image.status === Image.Error; text: "This image could not be previewed."; textFormat: Text.PlainText; color: Theme.secondary }
        }
    }
}
