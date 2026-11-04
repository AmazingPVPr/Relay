import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
    id: page
    objectName: "accessibilitySettings"
    function jumpToSection(section) {
        const target = ({"text-readability": readability, "visual-density": density, "color-contrast": contrast, "reduced-motion": motion, "audio-screen-reader": audio})[section]
        if (!target || !contentItem) return
        Qt.callLater(() => contentItem.contentY = Math.max(0, Math.min(contentItem.contentHeight - contentItem.height, target.mapToItem(settingsColumn, 0, 0).y)))
    }
    component SectionTitle: Text {
        Layout.fillWidth: true; Layout.topMargin: 24
        color: Theme.text; font.pixelSize: Math.round(23 * preferences.fontScale); font.weight: Font.Medium
        wrapMode: Text.WordWrap; textFormat: Text.PlainText
    }
    component Description: Text {
        Layout.fillWidth: true; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale)
        wrapMode: Text.WordWrap; textFormat: Text.PlainText
    }
    ColumnLayout {
        id: settingsColumn
        width: page.availableWidth
        spacing: 16
        Text { text: "Preview"; color: Theme.muted; font.pixelSize: Math.round(13 * preferences.fontScale) }
        Rectangle {
            Layout.fillWidth: true; implicitHeight: previewColumn.implicitHeight + 32
            radius: 7; color: preferences.highContrast ? "#000000" : Theme.canvas
            border.width: 1; border.color: preferences.highContrast ? "#ffffff" : Theme.border
            ColumnLayout {
                id: previewColumn
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                anchors.margins: 16; spacing: preferences.messageGroupGap
                Repeater {
                    model: 2
                    delegate: RowLayout {
                        required property int index
                        Layout.fillWidth: true; spacing: 12
                        Rectangle {
                            Layout.alignment: Qt.AlignTop; visible: !preferences.compactMessages
                            implicitWidth: 36; implicitHeight: 36; radius: 18; color: "#5865f2"
                            Text { anchors.centerIn: parent; text: "R"; color: "white"; font.pixelSize: 18; font.bold: true }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 4
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                Rectangle { visible: preferences.roleColorsMode === "dots"; implicitWidth: 8; implicitHeight: 8; radius: 4; color: "#b18af5" }
                                Text { text: "Relay friend"; color: preferences.roleColorsMode === "names" ? "#b18af5" : preferences.highContrast ? "white" : Theme.text; textFormat: Text.PlainText; font.pixelSize: preferences.chatFontSize; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                Text { text: "9:05 AM"; color: preferences.highContrast ? "#dddddd" : Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale) }
                            }
                            Text {
                                Layout.fillWidth: true; text: index === 0 ? "A comfortable place to catch up." : "Here’s an example link."
                                color: index === 1 ? preferences.highContrast ? "#9bd7ff" : "#83bcf1" : preferences.highContrast ? "white" : Theme.text
                                font.pixelSize: preferences.chatFontSize; font.underline: index === 1 && preferences.underlineLinks
                                wrapMode: Text.WordWrap; textFormat: Text.PlainText
                            }
                        }
                    }
                }
            }
        }
        SectionTitle { id: readability; objectName: "settingsSection_text-readability"; text: "Text Readability" }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Text size in chat"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); wrapMode: Text.WordWrap }
            NumericValue { value: preferences.chatFontSize; minimum: 12; maximum: 24; unit: " px"; onValueEdited: number => preferences.chatFontSize = number }
        }
        Description { text: "Adjust message text independently of the interface scale." }
        AppSlider { objectName: "chatFontSizeSlider"; Layout.fillWidth: true; from: 12; to: 24; stepSize: 1; value: preferences.chatFontSize; onMoved: preferences.chatFontSize = value; Accessible.name: "Text size in chat" }
        SettingsToggle { objectName: "underlineLinksToggle"; Layout.fillWidth: true; title: "Always underline links"; description: "Make links stand out in messages and embeds."; checked: preferences.underlineLinks; onToggled: value => preferences.underlineLinks = value }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; Layout.topMargin: 12; color: Theme.border }
        SectionTitle { id: density; objectName: "settingsSection_visual-density"; text: "Visual Density" }
        Text { text: "UI Density"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
        Description { text: "Adjust spacing in server, channel, and member lists." }
        Repeater {
            model: [{value:"compact", label:"Compact"}, {value:"default", label:"Default"}, {value:"spacious", label:"Spacious"}]
            delegate: RadioButton {
                required property var modelData
                objectName: "density_" + modelData.value
                text: modelData.label; checked: preferences.uiDensity === modelData.value; onClicked: preferences.uiDensity = modelData.value
                palette.windowText: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); Accessible.name: "UI density: " + modelData.label
            }
        }
        SettingsToggle { Layout.fillWidth: true; title: "Compact messages"; description: "Use less space and hide message avatars."; checked: preferences.compactMessages; onToggled: value => preferences.compactMessages = value }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Space between messages"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); wrapMode: Text.WordWrap }
            NumericValue { value: preferences.messageGroupGap; minimum: 0; maximum: 24; unit: " px"; onValueEdited: number => preferences.messageGroupGap = number }
        }
        AppSlider { objectName: "messageGapSlider"; Layout.fillWidth: true; from: 0; to: 24; stepSize: 1; value: preferences.messageGroupGap; onMoved: preferences.messageGroupGap = value; Accessible.name: "Space between messages" }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; Layout.topMargin: 12; color: Theme.border }
        SectionTitle { id: contrast; objectName: "settingsSection_color-contrast"; text: "Color & Contrast" }
        SettingsToggle { objectName: "highContrastToggle"; Layout.fillWidth: true; title: "Enable High Contrast Mode"; description: "Increase text, border, and background contrast."; checked: preferences.highContrast; onToggled: value => preferences.highContrast = value }
        Text { text: "Role Colors"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale) }
        Description { text: "Choose how server role colors appear in messages and member lists." }
        ComboBox {
            objectName: "roleColorsCombo"
            Layout.fillWidth: true; textRole: "label"; valueRole: "value"
            model: [{value:"names",label:"In names"}, {value:"dots",label:"As color dots"}, {value:"none",label:"Do not show role colors"}]
            currentIndex: Math.max(0, model.findIndex(row => row.value === preferences.roleColorsMode))
            onActivated: index => preferences.roleColorsMode = model[index].value
            Accessible.name: "Role colors"
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; Layout.topMargin: 12; color: Theme.border }
        SectionTitle { id: motion; objectName: "settingsSection_reduced-motion"; text: "Reduced Motion" }
        SettingsToggle { Layout.fillWidth: true; title: "Enable Reduced Motion"; description: "Reduce interface movement and stop GIF playback."; checked: preferences.reducedMotion; onToggled: value => preferences.reducedMotion = value }
        SettingsToggle { objectName: "animateImagesToggle"; Layout.fillWidth: true; title: "Play animated GIFs"; description: "Animate GIF attachments and previews. Reduced Motion takes priority."; checked: preferences.animateImages; onToggled: value => preferences.animateImages = value }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Minimum notification duration"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); wrapMode: Text.WordWrap }
            NumericValue { value: preferences.notificationDuration; minimum: 3; maximum: 20; unit: " s"; onValueEdited: number => preferences.notificationDuration = Math.round(number) }
        }
        Description { text: "Ask your desktop to keep notifications visible for this long. Your desktop may apply its own notification policy." }
        AppSlider { Layout.fillWidth: true; from: 3; to: 20; stepSize: 1; value: preferences.notificationDuration; onMoved: preferences.notificationDuration = value; Accessible.name: "Minimum notification duration" }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; Layout.topMargin: 12; color: Theme.border }
        SectionTitle { id: audio; objectName: "settingsSection_audio-screen-reader"; text: "Audio & Screen Reader" }
        Description { text: "Buttons, menus, sliders, and message text expose accessible names and support keyboard navigation. Configure your screen reader through your desktop accessibility settings." }
        Description { text: "Text-to-speech, generated image descriptions, and animated custom emoji are not available in this version of Relay. Custom emoji are shown as still images." }
        Description { text: "Manage individual notification and voice sounds under Notifications and Voice & Video." }
        Item { Layout.fillWidth: true; implicitHeight: 12 }
    }
}
