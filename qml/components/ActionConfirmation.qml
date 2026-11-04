import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: dialog
    parent: Overlay.overlay
    property string heading: "Confirm action"
    property string description: ""
    property string buttonLabel: "Confirm"
    property var payload: ({})
    property bool showReason: false
    property string accountId: ""
    signal confirmed(var payload, string reason)
    width: Math.min(460, parent.width - 40)
    x: (parent.width - width) / 2; y: Math.max(16, (parent.height - height) / 2)
    modal: true; focus: true; padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    function ask(title, details, label, value, reason) { heading = title; description = details; buttonLabel = label; payload = value; showReason = !!reason; accountId = discord.currentUserId; reasonField.clear(); open() }
    Connections { target: discord; function onStateChanged() { if (dialog.visible && dialog.accountId !== discord.currentUserId) dialog.close() } }
    background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 16
        Text { Layout.fillWidth: true; text: dialog.heading; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.text; font.pixelSize: Math.round(20 * preferences.fontScale); font.weight: Font.DemiBold }
        Text { Layout.fillWidth: true; text: dialog.description; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale) }
        Text { visible: dialog.showReason; text: "Reason (optional)"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
        TextField { id: reasonField; Layout.fillWidth: true; visible: dialog.showReason; maximumLength: 512; selectByMouse: true; placeholderText: "Add a reason for the audit log" }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ActionButton { text: "Cancel"; quiet: true; onClicked: dialog.close() }
            ActionButton { text: dialog.buttonLabel; destructive: true; enabled: dialog.accountId.length > 0 && dialog.accountId === discord.currentUserId; onClicked: { if (dialog.accountId !== discord.currentUserId) { dialog.close(); return } const value = dialog.payload; const reason = reasonField.text.trim(); dialog.close(); dialog.confirmed(value, reason) } }
        }
    }
}
