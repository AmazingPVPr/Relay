import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import "Theme.js" as Theme

Popup {
    id: panel
    parent: Overlay.overlay
    width: Math.min(360, parent ? parent.width - 32 : 360)
    padding: 18
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
    readonly property var samples: voice.latencyHistory
    readonly property bool measured: voice.latencyMs >= 0
    readonly property real chartMaximum: {
        let highest = 1
        for (const sample of samples) highest = Math.max(highest, Number(sample.ms))
        return Math.max(10, Math.ceil(highest / 10) * 10)
    }
    function showNear(item) {
        const position = item.mapToItem(parent, 0, 0)
        x = Math.max(16, Math.min(parent.width - width - 16, position.x))
        y = Math.max(16, Math.min(parent.height - height - 16, position.y - height - 12))
        open()
    }
    function latencyText(value) { return value >= 0 ? Math.round(value) + " ms" : "Waiting…" }
    function chartPoints() {
        const points = []
        const count = samples.length
        if (!count) return points
        const firstTime = Number(samples[0].timestamp)
        const duration = Number(samples[count - 1].timestamp) - firstTime
        for (let i = 0; i < count; ++i) {
            const position = duration > 0 ? (Number(samples[i].timestamp) - firstTime) / duration : (count > 1 ? i / (count - 1) : 1)
            points.push(Qt.point(position * history.width, history.height - 3 - Math.max(0, Number(samples[i].ms)) / chartMaximum * (history.height - 6)))
        }
        return points
    }
    contentItem: ColumnLayout {
        spacing: 14
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            AppIcon { name: "voice"; color: voice.connected ? Theme.success : Theme.warning; Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
            Text { Layout.fillWidth: true; text: "Voice connection"; color: Theme.text; font.pixelSize: Math.round(15 * preferences.fontScale); font.weight: Font.DemiBold }
            ActionButton { iconName: "close"; quiet: true; hint: "Close"; implicitWidth: 26; implicitHeight: 26; onClicked: panel.close() }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 20
            Repeater {
                model: [{label: "Last ping", value: voice.latencyMs}, {label: "Average", value: voice.averageLatencyMs}]
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true; Layout.preferredWidth: (panel.availableWidth - 20) / 2; spacing: 4
                    Text { text: modelData.label; color: Theme.secondary; font.pixelSize: Math.round(11 * preferences.fontScale) }
                    Text { text: panel.latencyText(modelData.value); color: Theme.text; font.pixelSize: Math.round(19 * preferences.fontScale); font.weight: Font.Medium }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 92
            radius: 5; color: Theme.canvas
            Item {
                id: history
                anchors.fill: parent; anchors.margins: 10
                Rectangle { width: parent.width; height: 1; y: parent.height - 1; color: Theme.border }
                Shape {
                    anchors.fill: parent
                    visible: panel.samples.length > 1
                    ShapePath {
                        strokeColor: Theme.success; strokeWidth: 2; fillColor: "transparent"
                        capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
                        PathPolyline { path: panel.chartPoints() }
                    }
                }
                Rectangle {
                    visible: panel.samples.length === 1
                    width: 5; height: 5; radius: 2.5; color: Theme.success
                    x: history.width - width
                    y: panel.samples.length ? history.height - 3 - Number(panel.samples[0].ms) / panel.chartMaximum * (history.height - 6) - height / 2 : 0
                }
                Text { anchors.centerIn: parent; visible: !panel.samples.length; text: "Waiting for a ping measurement"; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale) }
            }
        }
        Text { Layout.fillWidth: true; text: "Voice gateway round-trip time"; color: Theme.muted; font.pixelSize: Math.round(10 * preferences.fontScale) }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
        ColumnLayout {
            Layout.fillWidth: true; spacing: 5
            Text { text: "Server"; color: Theme.secondary; font.pixelSize: Math.round(11 * preferences.fontScale) }
            Text { Layout.fillWidth: true; text: voice.voiceServerLabel || "Not connected"; textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere; color: Theme.text; font.pixelSize: Math.round(12 * preferences.fontScale) }
        }
        Text {
            Layout.fillWidth: true
            text: voice.encryptedReady ? "End-to-end encryption active" : voice.connected ? "Waiting for end-to-end encryption" : "Voice disconnected"
            wrapMode: Text.WordWrap
            color: voice.encryptedReady ? Theme.success : Theme.secondary
            font.pixelSize: Math.round(12 * preferences.fontScale)
        }
    }
}
