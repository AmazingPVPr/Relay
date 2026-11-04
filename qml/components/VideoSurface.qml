import QtQuick
import QtMultimedia
import Relay.Native 1.0

Item {
    readonly property var videoSink: presentation.item ? presentation.item.videoSink : null
    Loader {
        id: presentation
        anchors.fill: parent
        sourceComponent: softwareRendering ? softwareVideo : hardwareVideo
    }
    Component { id: hardwareVideo; VideoOutput { fillMode: VideoOutput.PreserveAspectFit } }
    Component { id: softwareVideo; SoftwareVideo {} }
}
