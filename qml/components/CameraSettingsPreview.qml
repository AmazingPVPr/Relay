import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "Theme.js" as Theme

ColumnLayout {
    id: cameraSettings
    spacing: 16
    Component.onDestruction: localCamera.stop()
    onVisibleChanged: if (!visible) localCamera.stop()
    MediaDevices { id: devices }
    Camera {
        id: localCamera
        cameraDevice: devices.videoInputs.length > 0 && deviceChoice.currentIndex >= 0 ? devices.videoInputs[deviceChoice.currentIndex] : devices.defaultVideoInput
    }
    CaptureSession { camera: localCamera; videoOutput: preview }
    Rectangle {
        Layout.fillWidth: true; implicitHeight: width * 0.32
        radius: 4; color: Theme.rail; border.color: Theme.border; clip: true
        VideoOutput { id: preview; anchors.fill: parent; visible: localCamera.active; fillMode: VideoOutput.PreserveAspectFit }
        ActionButton {
            anchors.centerIn: parent; visible: !localCamera.active
            text: "Test Video"; primary: true; enabled: devices.videoInputs.length > 0
            onClicked: localCamera.start()
        }
        ActionButton { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 10; visible: localCamera.active; text: "Stop preview"; onClicked: localCamera.stop() }
    }
    Text { text: "Camera"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale }
    ComboBox {
        id: deviceChoice
        Layout.fillWidth: true
        model: devices.videoInputs
        textRole: "description"
        enabled: devices.videoInputs.length > 0
        displayText: devices.videoInputs.length > 0 ? currentText : "No Video Devices"
        onActivated: { const active = localCamera.active; localCamera.stop(); if (active) Qt.callLater(() => localCamera.start()) }
        Accessible.name: "Camera"
    }
    Text { Layout.fillWidth: true; visible: !!localCamera.errorString; text: localCamera.errorString; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
    Text { Layout.fillWidth: true; text: "Test your camera locally. This preview starts only when you press Test Video and stops when you leave this page."; color: Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
}
