import QtQuick
import QtQuick.Layouts
import "Theme.js" as Theme

DropArea {
    id: root
    property var service: typeof uploads !== "undefined" ? uploads : null
    property bool showOverlay: true
    enabled: !!service && !service.busy
    keys: ["text/uri-list"]

    function localFiles(urls) {
        if (!urls || !urls.length) return []
        let files = []
        for (let i = 0; i < urls.length; ++i) {
            const value = String(urls[i])
            // Only local file URLs enter the draft. The service validates each path.
            if (!/^file:(?:\/[^\/]|\/\/\/|\/\/localhost\/)/i.test(value)) return []
            files.push(value)
        }
        return files
    }

    onEntered: drag => {
        drag.accepted = root.enabled && root.localFiles(drag.urls).length > 0
    }
    onDropped: drop => {
        const files = root.localFiles(drop.urls)
        if (!root.enabled || !files.length) return
        root.service.addFiles(files)
        drop.acceptProposedAction()
    }

    Rectangle {
        anchors.fill: parent
        visible: root.containsDrag && root.showOverlay
        color: Theme.canvas
        opacity: 0.96
        radius: 8
        border.width: 2
        border.color: Theme.focus
        ColumnLayout {
            anchors.centerIn: parent
            width: Math.max(1, Math.min(360, parent.width - 32))
            spacing: 8
            AppIcon { name: "copy"; color: Theme.focus; Layout.preferredWidth: 36; Layout.preferredHeight: 36; Layout.alignment: Qt.AlignHCenter }
            Text {
                Layout.fillWidth: true
                text: "Drop files to attach"
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: Math.round(18 * preferences.fontScale)
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: "Review your attachments before sending."
                textFormat: Text.PlainText
                color: Theme.secondary
                font.pixelSize: Math.round(12 * preferences.fontScale)
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }
}
