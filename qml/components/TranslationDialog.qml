import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: dialog
    parent: Overlay.overlay
    width: Math.min(680, parent.width - 48)
    height: Math.min(720, parent.height - 48)
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    modal: true
    padding: 24
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    property bool allowInsert: true
    property bool copied: false
    readonly property string languageCode: language.currentIndex >= 0 ? translation.languages[language.currentIndex].code : "en"
    readonly property bool currentResult: original.text === translation.originalText && languageCode === translation.targetLanguage && translation.translatedText.length > 0
    signal insertRequested(string text)
    function openText(text, canInsert) {
        translation.clear()
        allowInsert = canInsert === undefined ? true : canInsert
        copied = false
        original.text = text
        const values = translation.languages
        for (let i = 0; i < values.length; ++i) {
            if (values[i].code === translation.targetLanguage) { language.currentIndex = i; break }
        }
        open()
    }
    onClosed: {
        translation.clear()
        original.clear()
        copied = false
    }
    background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 16
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Translate"; color: Theme.text; font.pixelSize: Math.round(23 * preferences.fontScale); font.weight: Font.DemiBold }
            ActionButton { iconName: "close"; hint: "Close translation"; quiet: true; onClicked: dialog.close() }
        }
        Text {
            Layout.fillWidth: true
            text: "When you choose Translate, the text below is sent to Google Translate. Nothing is sent to the chat."
            textFormat: Text.PlainText
            color: Theme.secondary
            font.pixelSize: Math.round(13 * preferences.fontScale)
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            Text { text: "Translate to"; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale) }
            ComboBox {
                id: language
                Layout.fillWidth: true
                model: translation.languages
                textRole: "name"
                Accessible.name: "Translation language"
                onCurrentIndexChanged: { translation.clear(); dialog.copied = false }
            }
            ActionButton {
                text: translation.busy ? "Cancel" : "Translate"
                primary: !translation.busy
                enabled: translation.busy || (original.text.trim().length > 0 && original.text.length <= 4000)
                onClicked: {
                    dialog.copied = false
                    if (translation.busy) translation.cancel()
                    else translation.translate(original.text, dialog.languageCode)
                }
            }
        }
        HardScrollView {
            id: scroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            ColumnLayout {
                width: scroll.availableWidth
                spacing: 12
                RowLayout {
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: "Original · language detected automatically"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
                    Text { text: original.text.length + " / 4,000"; color: original.text.length > 4000 ? Theme.danger : Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale) }
                }
                TextArea {
                    id: original
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(120, implicitHeight)
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.PlainText
                    color: Theme.text
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.onAccent
                    placeholderText: "Text to translate"
                    placeholderTextColor: Theme.muted
                    font.pixelSize: Math.round(14 * preferences.fontScale)
                    padding: 12
                    Accessible.name: "Original text"
                    background: Rectangle { color: Theme.canvas; border.color: original.activeFocus ? Theme.focus : Theme.border; radius: 4 }
                    onTextChanged: { translation.clear(); dialog.copied = false }
                }
                Text { text: "Translation"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
                TextArea {
                    id: result
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(120, implicitHeight)
                    text: dialog.currentResult ? translation.translatedText : ""
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.PlainText
                    color: Theme.text
                    selectionColor: Theme.accent
                    selectedTextColor: Theme.onAccent
                    placeholderText: translation.busy ? "Translating…" : "Your translation will appear here"
                    placeholderTextColor: Theme.muted
                    font.pixelSize: Math.round(14 * preferences.fontScale)
                    padding: 12
                    Accessible.name: "Translated text"
                    background: Rectangle { color: Theme.canvas; border.color: Theme.border; radius: 4 }
                }
                Text { Layout.fillWidth: true; visible: translation.error.length > 0; text: translation.error; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Translations may miss context. Review before using."; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap }
            ActionButton { text: dialog.copied ? "Copied" : "Copy"; enabled: dialog.currentResult && !translation.busy; onClicked: dialog.copied = translation.copyResult() }
            ActionButton {
                text: "Use in draft"
                primary: true
                visible: dialog.allowInsert
                enabled: dialog.currentResult && !translation.busy
                onClicked: {
                    const translated = translation.translatedText
                    dialog.insertRequested(translated)
                    dialog.close()
                }
            }
        }
    }
}
