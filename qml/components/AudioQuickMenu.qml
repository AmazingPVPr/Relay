import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Popup {
    id: menu
    parent: Overlay.overlay
    property bool inputMode: true
    readonly property var devices: inputMode ? voice.inputDevices : voice.outputDevices
    readonly property string deviceId: inputMode ? voice.inputDeviceId : voice.outputDeviceId
    signal settingsRequested()
    width: Math.min(340, parent.width - 32)
    padding: 16; focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    function openFor(item, input) {
        inputMode = input
        const position = item.mapToItem(parent, 0, 0)
        x = Math.max(16, Math.min(parent.width - width - 16, position.x - width / 2))
        y = Math.max(16, position.y - height - 10)
        open()
    }
    function selectedDevice() {
        for (let i = 0; i < devices.length; ++i) if (devices[i].id === deviceId) return i
        return 0
    }
    function selectedProfile() {
        if (!preferences.noiseSuppression && !preferences.echoCancellation && !preferences.automaticGainControl) return 1
        if (preferences.noiseSuppression && !preferences.echoCancellation && !preferences.automaticGainControl) return 2
        if (preferences.noiseSuppression && preferences.echoCancellation && preferences.automaticGainControl) return 3
        return 0
    }
    background: Rectangle { radius: 7; color: Theme.surface; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 12
        Text { text: menu.inputMode ? "Microphone" : "Output device"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); font.weight: Font.DemiBold }
        ComboBox {
            Layout.fillWidth: true; model: menu.devices; textRole: "name"
            currentIndex: menu.selectedDevice()
            onActivated: index => { if (menu.inputMode) voice.setInputDeviceId(menu.devices[index].id); else voice.setOutputDeviceId(menu.devices[index].id) }
        }
        Text { visible: menu.inputMode; text: "Voice processing"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
        ComboBox {
            Layout.fillWidth: true; visible: menu.inputMode
            model: ["Custom", "Unprocessed", "Headset · noise suppression", "Speakers · noise + echo control"]
            currentIndex: menu.selectedProfile()
            onActivated: index => {
                if (index === 0) return
                preferences.noiseSuppression = index >= 2
                preferences.echoCancellation = index === 3
                preferences.automaticGainControl = index === 3
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: menu.inputMode ? "Input volume" : "Output volume"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
            NumericValue { value: (menu.inputMode ? preferences.inputVolume : preferences.outputVolume) * 100; minimum: 0; maximum: 200; unit: "%"; onValueEdited: number => { if (menu.inputMode) preferences.inputVolume = number / 100; else preferences.outputVolume = number / 100 } }
        }
        AppSlider { Layout.fillWidth: true; from: 0; to: 2; stepSize: 0.05; value: menu.inputMode ? preferences.inputVolume : preferences.outputVolume; onMoved: { if (menu.inputMode) preferences.inputVolume = value; else preferences.outputVolume = value } }
        Text { visible: menu.inputMode; text: voice.connected || voice.micTesting ? "Input level" : "Microphone inactive"; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale) }
        Rectangle {
            visible: menu.inputMode; Layout.fillWidth: true; height: 6; radius: 3; color: Theme.rail
            Rectangle { height: parent.height; width: (voice.connected || voice.micTesting ? Math.max(0, Math.min(1, voice.inputLevel)) : 0) * parent.width; radius: 3; color: voice.inputLevel > 0.9 ? Theme.warning : Theme.success }
        }
        Text {
            objectName: "quickMicrophoneTestStatus"
            Layout.fillWidth: true; visible: menu.inputMode && voice.micTesting
            text: voice.channelId.length ? "Mic test active · muted and deafened in voice. Only you hear this test." : "Mic test active · only you hear this test."
            color: Theme.warning; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap
        }
        ActionButton {
            objectName: "quickMicrophoneTestButton"
            Layout.fillWidth: true; visible: menu.inputMode
            text: voice.micTesting ? "Stop Mic Test" : "Test microphone"
            quiet: true
            onClicked: voice.micTesting ? voice.stopMicTest() : voice.startMicTest()
        }
        ActionButton { Layout.fillWidth: true; text: "Voice settings"; quiet: true; onClicked: { menu.close(); menu.settingsRequested() } }
    }
}
