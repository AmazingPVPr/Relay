import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
    id: page
    objectName: "languageSettings"
    function jumpToSection(section) {
        const target = section === "time" ? timeSection : languageSection
        if (!contentItem) return
        Qt.callLater(() => contentItem.contentY = Math.max(0, Math.min(contentItem.contentHeight - contentItem.height, target.mapToItem(settingsColumn, 0, 0).y)))
    }
    ColumnLayout {
        id: settingsColumn
        width: page.availableWidth
        spacing: 16
        Text { id: languageSection; objectName: "settingsSection_language"; Layout.fillWidth: true; text: "Select a Language"; color: Theme.text; font.pixelSize: Math.round(23 * preferences.fontScale); font.weight: Font.Medium; wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; text: "Relay is available in English. Select the regional date and time conventions you prefer."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        ComboBox {
            objectName: "languageCombo"
            Layout.fillWidth: true; textRole: "label"; valueRole: "value"
            model: [{value:"en_US",label:"English, US"}, {value:"en_GB",label:"English, UK"}]
            currentIndex: preferences.language === "en_GB" ? 1 : 0
            onActivated: index => preferences.language = model[index].value
            Accessible.name: "Language and regional format"
        }
        Text { id: timeSection; objectName: "settingsSection_time"; Layout.fillWidth: true; Layout.topMargin: 24; text: "Time Format"; color: Theme.text; font.pixelSize: Math.round(23 * preferences.fontScale); font.weight: Font.Medium; wrapMode: Text.WordWrap }
        Repeater {
            model: [{value:"auto",label:"Auto"}, {value:"12",label:"12-hour"}, {value:"24",label:"24-hour"}]
            delegate: RadioButton {
                required property var modelData
                objectName: "timeFormat_" + modelData.value
                text: modelData.label; checked: preferences.timeFormat === modelData.value
                onClicked: preferences.timeFormat = modelData.value
                palette.windowText: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale)
                Accessible.name: "Time format: " + modelData.label
            }
        }
        Rectangle {
            Layout.fillWidth: true; Layout.topMargin: 12; implicitHeight: sample.implicitHeight + 32
            color: Theme.surface; radius: 7; border.color: Theme.border
            ColumnLayout {
                id: sample; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 16; spacing: 8
                Text { text: "Preview"; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale) }
                Text {
                    objectName: "languageTimePreview"
                    Layout.fillWidth: true; wrapMode: Text.WordWrap; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale)
                    text: { const language = preferences.language, format = preferences.timeFormat; return preferences.formatDateTime("2026-10-19T18:27:00") }
                }
            }
        }
        Item { Layout.fillWidth: true; implicitHeight: 12 }
    }
}
