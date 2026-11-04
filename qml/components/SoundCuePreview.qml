import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

ColumnLayout {
    spacing: 8
    RowLayout {
        Layout.fillWidth: true
        ComboBox {
            id: selectedCue
            objectName: "soundCueSelector"
            Layout.fillWidth: true
            textRole: "label"
            model: [
                { label: "Mute", cue: "mute" }, { label: "Unmute", cue: "unmute" },
                { label: "Deafen", cue: "deafen" }, { label: "Undeafen", cue: "undeafen" },
                { label: "Join voice", cue: "join" }, { label: "Leave voice", cue: "leave" },
                { label: "Someone joins voice", cue: "user-join" }, { label: "Someone leaves voice", cue: "user-leave" },
                { label: "Stream starts", cue: "stream-start" }, { label: "Stream stops", cue: "stream-stop" },
                { label: "Start watching", cue: "watch-start" }, { label: "Stop watching", cue: "watch-stop" },
                { label: "Message", cue: "message" }
            ]
            Accessible.name: "Sound effect to preview"
        }
        ActionButton {
            objectName: "previewSoundCue"
            text: "Preview"
            quiet: true
            readonly property string cue: selectedCue.model[selectedCue.currentIndex].cue
            enabled: preferences.activitySoundsEnabled && preferences.soundEffectsVolume > 0
                && (!(cue.startsWith("stream-") || cue.startsWith("watch-")) || preferences.streamActivitySounds)
                && (!cue.startsWith("user-") || preferences.voiceActivitySounds)
                && (cue !== "message" || preferences.notificationSound)
            onClicked: soundEffects.play(cue)
        }
    }
    Text {
        Layout.fillWidth: true
        text: preferences.activitySoundsEnabled && preferences.soundEffectsVolume > 0
            ? "Preview through your selected output device. This doesn't change your microphone or call."
            : "Enable activity sounds and turn up sound effects volume to hear a preview."
        color: Theme.muted
        font.pixelSize: Math.round(11 * preferences.fontScale)
        wrapMode: Text.WordWrap
    }
    Text {
        Layout.fillWidth: true
        visible: soundEffects.error.length > 0
        text: soundEffects.error
        textFormat: Text.PlainText
        color: Theme.danger
        font.pixelSize: Math.round(12 * preferences.fontScale)
        wrapMode: Text.WordWrap
    }
}
