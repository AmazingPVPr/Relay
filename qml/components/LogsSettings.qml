import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

ColumnLayout {
    spacing: 14
    Text { text: "Logs"; color: Theme.text; font.pixelSize: Math.round(22 * preferences.fontScale); font.weight: Font.DemiBold }
    Text { Layout.fillWidth: true; text: "Recent Relay errors and stream events are saved on this device. Review them before sharing; they may contain private information. Older entries are removed as the log fills."; color: Theme.secondary; wrapMode: Text.WordWrap; font.pixelSize: Math.round(12 * preferences.fontScale) }
    RowLayout {
        ActionButton { text: "Copy logs"; enabled: diagnosticLog.text.length > 0; onClicked: diagnosticLog.copy() }
        ActionButton { text: "Clear logs"; quiet: true; enabled: diagnosticLog.text.length > 0; onClicked: confirmClear.open() }
        Item { Layout.fillWidth: true }
    }
    Dialog {
        id: confirmClear; title: "Clear logs?"; modal: true; standardButtons: Dialog.Yes | Dialog.No
        onAccepted: diagnosticLog.clear()
        Label { text: "Remove the saved diagnostic history from this device?"; wrapMode: Text.WordWrap }
    }
    ScrollView {
        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
        TextArea { objectName: "diagnosticLogText"; text: diagnosticLog.text || "No diagnostic events yet."; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap; color: Theme.text; font.family: "monospace"; font.pixelSize: Math.round(12 * preferences.fontScale); background: Rectangle { color: Theme.rail; radius: 6 } }
    }
}
