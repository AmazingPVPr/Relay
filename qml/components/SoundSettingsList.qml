import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

ColumnLayout {
    id: list
    property var cues: []
    property int soundRevision: 0
    spacing: 0
    Connections { target: preferences; function onChanged() { list.soundRevision++ } }
    Repeater {
        model: list.cues
        delegate: ColumnLayout {
            id: cue
            required property var modelData
            Layout.fillWidth: true
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 15; Layout.bottomMargin: 15
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 5
                    Text { Layout.fillWidth: true; text: cue.modelData.label; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); wrapMode: Text.WordWrap }
                    Button {
                        text: "Preview Sound"; flat: true; padding: 0; implicitHeight: 20; hoverEnabled: true
                        background: Item {}
                        contentItem: Text { text: parent.text; color: "#5fa8ed"; font.pixelSize: Math.round(12 * preferences.fontScale); font.underline: parent.hovered }
                        onClicked: soundEffects.preview(cue.modelData.id)
                    }
                }
                AppSwitch {
                    checked: { const revision = list.soundRevision; return preferences.soundEnabled(cue.modelData.id) }
                    onToggled: preferences.setSoundEnabled(cue.modelData.id, checked)
                    Accessible.name: cue.modelData.label + " sound"
                }
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
        }
    }
}
