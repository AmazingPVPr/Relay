import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
    id: page
    signal navigate(string page, string section)
    property bool advancedVoice: false
    property bool moreSounds: false
    function deviceIndex(devices, id) { for (let i = 0; i < devices.length; ++i) if (devices[i].id === id) return i; return 0 }
    function resetVoice() {
        voice.stopMicTest()
        voice.setInputDeviceId(""); voice.setOutputDeviceId("")
        preferences.inputDeviceId = ""; preferences.outputDeviceId = ""
        preferences.inputVolume = 1; preferences.outputVolume = 1
        preferences.voiceLimiterEnabled = false; preferences.voiceLimiterCeiling = 0.5
        preferences.noiseSuppression = false; preferences.echoCancellation = false; preferences.automaticGainControl = false
        preferences.inputSensitivityDb = -48; preferences.soundEffectsVolume = 0.5; preferences.soundboardVolume = 0.5
        preferences.activitySoundsEnabled = true; preferences.voiceActivitySounds = true; preferences.streamActivitySounds = true
        for (const cue of ["mute","unmute","deafen","undeafen","join","leave","user-join","user-leave","stream-start","stream-stop","watch-start","watch-stop","message"]) preferences.setSoundEnabled(cue, true)
    }
    Component.onDestruction: { voice.stopMicTest(); soundboard.stopPreview() }
    onVisibleChanged: if (!visible) { voice.stopMicTest(); soundboard.stopPreview() }
    ColumnLayout {
        width: page.availableWidth; spacing: 22
        SettingsHeading { objectName: "settingsSection_input"; text: "Voice" }
        GridLayout {
            Layout.fillWidth: true; columns: page.availableWidth < 500 ? 1 : 2; columnSpacing: 16; rowSpacing: 18
            ColumnLayout {
                Layout.fillWidth: true; spacing: 12
                Text { text: "Microphone"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale }
                ComboBox { Layout.fillWidth: true; model: voice.inputDevices; textRole: "name"; currentIndex: page.deviceIndex(voice.inputDevices, voice.inputDeviceId); onActivated: index => voice.setInputDeviceId(voice.inputDevices[index].id); Accessible.name: "Microphone" }
                Text { text: "Microphone Volume"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale }
                AppSlider { Layout.fillWidth: true; from: 0; to: 2; stepSize: 0.01; value: preferences.inputVolume; onMoved: preferences.inputVolume = value; Accessible.name: "Microphone Volume" }
            }
            ColumnLayout {
                objectName: "settingsSection_output"
                Layout.fillWidth: true; spacing: 12
                Text { text: "Speaker"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale }
                ComboBox { Layout.fillWidth: true; model: voice.outputDevices; textRole: "name"; currentIndex: page.deviceIndex(voice.outputDevices, voice.outputDeviceId); onActivated: index => voice.setOutputDeviceId(voice.outputDevices[index].id); Accessible.name: "Speaker" }
                Text { text: "Speaker Volume"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale }
                AppSlider { Layout.fillWidth: true; from: 0; to: 2; stepSize: 0.01; value: preferences.outputVolume; onMoved: preferences.outputVolume = value; Accessible.name: "Speaker Volume" }
            }
        }
        SettingsToggle { objectName: "voiceLimiterToggle"; Layout.fillWidth: true; title: "Limit loud voices"; description: "Reduce sudden loud speech from each person. This only affects what you hear."; checked: preferences.voiceLimiterEnabled; onToggled: value => preferences.voiceLimiterEnabled = value }
        RowLayout {
            visible: preferences.voiceLimiterEnabled; Layout.fillWidth: true
            Text { text: "Voice limit"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale }
            AppSlider { objectName: "voiceLimiterSlider"; Layout.fillWidth: true; from: 0.1; to: 1; stepSize: 0.01; value: preferences.voiceLimiterCeiling; onMoved: preferences.voiceLimiterCeiling = value; Accessible.name: "Voice limit" }
            NumericValue { value: preferences.voiceLimiterCeiling * 100; minimum: 10; maximum: 100; onValueEdited: value => preferences.voiceLimiterCeiling = value / 100; Accessible.name: "Voice limit percent" }
        }
        Text { visible: preferences.voiceLimiterEnabled; Layout.fillWidth: true; text: "Lower values reduce loud voices more. Speaker volume still controls the overall level."; color: Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        RowLayout {
            Layout.fillWidth: true; spacing: 18
            ActionButton { objectName: "microphoneTestButton"; text: voice.micTesting ? "Stop Test" : "Mic Test"; primary: true; onClicked: voice.micTesting ? voice.stopMicTest() : voice.startMicTest() }
            Row {
                Layout.fillWidth: true; spacing: 3
                Repeater { model: Math.max(1, Math.floor((parent.width + 3) / 7)); Rectangle { required property int index; width: 4; height: 22; radius: 2; color: index / Math.max(1, parent.children.length) < voice.inputLevel ? Theme.success : Theme.border } }
            }
        }
        Text {
            objectName: "microphoneTestStatus"
            Layout.fillWidth: true
            text: voice.micTesting
                ? (voice.channelId.length ? "Mic test active · You are muted and deafened in the voice channel. Only you hear this test. Stream audio is paused during the test. Your previous mic and speaker state will return when you stop." : "Mic test active · Only you hear your microphone. Stream audio is paused during the test. Stop the test when you're finished.")
                : (voice.channelId.length ? "Hear your microphone through the selected speaker. Relay temporarily mutes and deafens you in the voice channel, then restores your previous state." : "Hear your microphone through the selected speaker. Audio stays on this device.")
            color: voice.micTesting ? Theme.warning : Theme.muted
            font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
        SettingsDivider {}
        ColumnLayout {
            objectName: "settingsSection_processing"; Layout.fillWidth: true; spacing: 14
            Text { text: "Input Profile"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale }
            RadioButton { text: "Voice Isolation"; checked: preferences.noiseSuppression && preferences.echoCancellation && preferences.automaticGainControl; onClicked: { preferences.noiseSuppression=true; preferences.echoCancellation=true; preferences.automaticGainControl=true } }
            Text { Layout.leftMargin: 28; Layout.fillWidth: true; text: "Reduce background noise with local RNNoise processing."; color: Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
            RadioButton { text: "Studio"; checked: !preferences.noiseSuppression && !preferences.echoCancellation && !preferences.automaticGainControl; onClicked: { preferences.noiseSuppression=false; preferences.echoCancellation=false; preferences.automaticGainControl=false } }
            Text { Layout.leftMargin: 28; Layout.fillWidth: true; text: "Turn off noise suppression, echo cancellation, and automatic gain."; color: Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
            RadioButton { text: "Custom"; checked: page.advancedVoice || !((preferences.noiseSuppression && preferences.echoCancellation && preferences.automaticGainControl) || (!preferences.noiseSuppression && !preferences.echoCancellation && !preferences.automaticGainControl)); onClicked: page.advancedVoice = true }
            Text { Layout.fillWidth: true; text: "Input Sensitivity"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale; Layout.topMargin: 12 }
            Text { Layout.fillWidth: true; text: "Controls how much sound Relay transmits from your mic. Lower values pick up quieter speech."; color: Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
            AppSlider { Layout.fillWidth: true; from: -70; to: -10; stepSize: 1; value: preferences.inputSensitivityDb; onMoved: preferences.inputSensitivityDb=value; Accessible.name: "Input Sensitivity" }
            RowLayout {
                Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: "Noise Suppression"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale }
                ComboBox { model: ["RNNoise", "None"]; currentIndex: preferences.noiseSuppression ? 0 : 1; onActivated: index => preferences.noiseSuppression=index===0; Accessible.name: "Noise Suppression" }
            }
            SettingsToggle { Layout.fillWidth: true; title: "Echo Cancellation"; checked: preferences.echoCancellation; onToggled: value => preferences.echoCancellation=value }
            ActionButton { Layout.fillWidth: true; text: page.advancedVoice ? "Hide Advanced Voice Settings" : "Show Advanced Voice Settings"; quiet: true; iconName: "down"; onClicked: page.advancedVoice=!page.advancedVoice }
            SettingsToggle { Layout.fillWidth: true; visible: page.advancedVoice; title: "Automatic Gain Control"; description: "Automatically balance your microphone level."; checked: preferences.automaticGainControl; onToggled: value => preferences.automaticGainControl=value }
        }
        SettingsDivider {}
        SettingsHeading { objectName: "settingsSection_camera"; text: "Camera" }
        CameraSettingsPreview { Layout.fillWidth: true }
        SettingsDivider {}
        SettingsHeading { objectName: "settingsSection_streaming"; text: "Streaming" }
        SettingsToggle { Layout.fillWidth: true; title: "Crop Stream Preview"; description: "Fit stream playback to your chosen aspect ratio."; checked: preferences.streamCropEnabled; onToggled: value => preferences.streamCropEnabled=value }
        ComboBox { Layout.fillWidth: true; enabled: preferences.streamCropEnabled; model: ["16:9", "16:10", "21:9", "32:9", "4:3", "1:1", "custom"]; currentIndex: Math.max(0, model.indexOf(preferences.streamCropPreset)); onActivated: index => preferences.streamCropPreset=model[index]; Accessible.name: "Stream aspect ratio" }
        Text { Layout.fillWidth: true; text: "Choose source quality and frame rate when starting a stream. OBS and RTMP sources are available from Add-ons."; color: Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        ActionButton { text: "OBS and RTMP settings"; quiet: true; onClicked: page.navigate("addons", "") }
        SettingsDivider {}
        SettingsHeading { objectName: "settingsSection_sounds"; text: "Sounds" }
        SettingsToggle { Layout.fillWidth: true; title: "Enable Voice & Video Sounds"; checked: preferences.activitySoundsEnabled; onToggled: value => preferences.activitySoundsEnabled=value }
        AppSlider { Layout.fillWidth: true; from: 0; to: 1; value: preferences.soundEffectsVolume; stepSize: 0.01; onMoved: preferences.soundEffectsVolume=value; Accessible.name: "Sound Effects Volume" }
        SoundSettingsList { Layout.fillWidth: true; cues: [{id:"deafen",label:"Deafen"},{id:"undeafen",label:"Undeafen"},{id:"mute",label:"Mute"},{id:"unmute",label:"Unmute"}] }
        ActionButton { Layout.fillWidth: true; text: page.moreSounds ? "Show fewer sounds" : "Show 8 more sounds"; quiet: true; iconName: "down"; onClicked: page.moreSounds=!page.moreSounds }
        SoundSettingsList { Layout.fillWidth: true; visible: page.moreSounds; cues: [{id:"join",label:"Voice Connected"},{id:"leave",label:"Voice Disconnected"},{id:"user-join",label:"User Join"},{id:"user-leave",label:"User Leave"},{id:"stream-start",label:"Stream Started"},{id:"stream-stop",label:"Stream Stopped"},{id:"watch-start",label:"Started Watching"},{id:"watch-stop",label:"Stopped Watching"}] }
        ActionButton { text: "Notification sound settings"; quiet: true; onClicked: page.navigate("notifications", "sounds") }
        SettingsDivider {}
        SettingsHeading { objectName: "settingsSection_soundboard"; text: "Soundboard" }
        Text { Layout.fillWidth: true; text: "Soundboard listening volume"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale }
        Text { Layout.fillWidth: true; text: "Control the server soundboard sounds you hear, including other people’s sounds and your previews. This only changes your listening volume."; color: Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        RowLayout {
            Layout.fillWidth: true
            AppSlider { objectName: "soundboardListeningVolume"; Layout.fillWidth: true; from: 0; to: 1; stepSize: 0.01; value: preferences.soundboardVolume; onMoved: preferences.soundboardVolume=value; Accessible.name: "Soundboard listening volume" }
            NumericValue { objectName: "soundboardListeningPercent"; value: preferences.soundboardVolume * 100; minimum: 0; maximum: 100; onValueEdited: value => preferences.soundboardVolume = value / 100; Accessible.name: "Soundboard listening volume percent" }
        }
        Text { Layout.fillWidth: true; text: "To adjust one person’s soundboard sounds, right-click them and use Soundboard volume."; color: Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        ActionButton { text: "Custom soundboard"; quiet: true; onClicked: page.navigate("addons", "") }
        SettingsDivider {}
        SettingsHeading { objectName: "settingsSection_advanced"; text: "Advanced" }
        Text { Layout.fillWidth: true; visible: voice.error.length > 0; text: voice.error; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        RowLayout { Layout.fillWidth: true; Text { Layout.fillWidth: true; text: "Reset all Voice & Video settings"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale; wrapMode: Text.WordWrap } ActionButton { text: "Reset"; destructive: true; onClicked: resetConfirmation.open() } }
        Item { Layout.preferredHeight: 40 }
    }
    Dialog {
        id: resetConfirmation
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(440, parent ? parent.width - 32 : 440)
        title: "Reset Voice & Video?"; modal: true
        standardButtons: Dialog.Cancel | Dialog.Ok
        onAccepted: page.resetVoice()
        contentItem: ColumnLayout { Label { Layout.fillWidth: true; text: "This restores device, processing, volume, and sound preferences."; wrapMode: Text.WordWrap } }
    }
}
