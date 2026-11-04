import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Window {
    id: viewer
    objectName: "relayStreamWindow"
    width: 1000; height: 650; minimumWidth: 480; minimumHeight: 300
    visible: true; color: "#08090b"; title: "Relay direct stream"
    onClosing: relayStreams.stopWatching()
    ColumnLayout {
        anchors.fill: parent; spacing: 0
        RowLayout {
            Layout.fillWidth: true; Layout.margins: 12
            Label { Layout.fillWidth: true; text: relayStreams.status; textFormat: Text.PlainText; color: Theme.text; wrapMode: Text.Wrap }
            CheckBox { palette.windowText: Theme.text; palette.text: Theme.text; text: "Mute stream"; checked: relayStreams.audioMuted; onToggled: relayStreams.audioMuted = checked }
            Button { text: viewer.visibility === Window.FullScreen ? "Window" : "Full screen"; onClicked: viewer.visibility === Window.FullScreen ? viewer.showNormal() : viewer.showFullScreen() }
            Button { text: "Stop watching"; onClicked: relayStreams.stopWatching() }
        }
        VideoSurface {
            id: surface; Layout.fillWidth: true; Layout.fillHeight: true
            onVideoSinkChanged: if (videoSink) relayStreams.videoSink = videoSink
            Component.onCompleted: if (videoSink) relayStreams.videoSink = videoSink
            Component.onDestruction: if (relayStreams.videoSink === videoSink) relayStreams.videoSink = null
        }
    }
}
