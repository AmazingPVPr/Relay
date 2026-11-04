import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

ColumnLayout {
    id: statusBar
    property var service
    property var refreshAction: null
    Layout.fillWidth: true
    spacing: 9
    RowLayout {
        Layout.fillWidth: true
        BusyIndicator { visible: !!statusBar.service && (statusBar.service.busy || statusBar.service.mutating); running: visible; Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
        Text { Layout.fillWidth: true; text: !statusBar.service ? "Account services are unavailable." : statusBar.service.busy ? "Loading account settings…" : statusBar.service.mutating ? "Saving changes…" : statusBar.service.error || statusBar.service.status || ""; color: statusBar.service && statusBar.service.error ? Theme.danger : Theme.muted; textFormat: Text.PlainText; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        ActionButton { text: "Refresh"; quiet: true; enabled: !!statusBar.service && !statusBar.service.busy && !statusBar.service.mutating; onClicked: { if (statusBar.refreshAction) statusBar.refreshAction(); else statusBar.service.refresh() } }
    }
}
