import QtQuick
import QtQuick.Controls

// Keep text editing native. Only explicit paste in this composer asks the
// upload service whether the clipboard contains an attachment to stage.
TextArea {
    id: editor
    required property var attachmentService
    property bool attachmentPasteEnabled: true
    signal editingKeyPressed(var event)

    textFormat: TextEdit.PlainText
    selectByMouse: true

    function consumeAttachmentPaste() {
        return enabled && visible && !readOnly && attachmentPasteEnabled
                && attachmentService && attachmentService.pasteFromClipboard()
    }
    function pasteFromMenu() {
        if (!enabled || !visible || readOnly) return
        if (!consumeAttachmentPaste()) editor.paste()
    }

    Keys.priority: Keys.BeforeItem
    Keys.onPressed: event => {
        event.accepted = false
        if (event.matches(StandardKey.Paste) && activeFocus) {
            event.accepted = consumeAttachmentPaste()
            return
        }
        if (activeFocus && (event.key === Qt.Key_Menu
                || (event.key === Qt.Key_F10 && event.modifiers === Qt.ShiftModifier))) {
            editingMenu.popup(Math.min(width, cursorRectangle.x),
                              Math.min(height, cursorRectangle.y + cursorRectangle.height))
            event.accepted = true
            return
        }
        editingKeyPressed(event)
    }

    // Qt's standard Paste action invokes TextEdit.paste() directly, so provide
    // the familiar editing actions with the same attachment-aware paste route.
    // Paste stays available for image-only clipboards, where canPaste is false.
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: point => editingMenu.popup(point.position.x, point.position.y)
    }
    Menu {
        id: editingMenu
        parent: editor
        objectName: "composerEditingMenu"
        MenuItem { text: qsTr("Undo"); icon.name: "edit-undo"; enabled: editor.canUndo && !editor.readOnly; onTriggered: editor.undo() }
        MenuItem { text: qsTr("Redo"); icon.name: "edit-redo"; enabled: editor.canRedo && !editor.readOnly; onTriggered: editor.redo() }
        MenuSeparator {}
        MenuItem { text: qsTr("Cut"); icon.name: "edit-cut"; enabled: editor.selectedText.length > 0 && !editor.readOnly; onTriggered: editor.cut() }
        MenuItem { text: qsTr("Copy"); icon.name: "edit-copy"; enabled: editor.selectedText.length > 0; onTriggered: editor.copy() }
        MenuItem { objectName: "composerPasteAction"; text: qsTr("Paste"); icon.name: "edit-paste"; enabled: editor.enabled && !editor.readOnly; onTriggered: editor.pasteFromMenu() }
        MenuItem { text: qsTr("Delete"); icon.name: "edit-delete"; enabled: editor.selectedText.length > 0 && !editor.readOnly; onTriggered: editor.remove(editor.selectionStart, editor.selectionEnd) }
        MenuSeparator {}
        MenuItem { text: qsTr("Select All"); icon.name: "edit-select-all"; enabled: editor.length > 0; onTriggered: editor.selectAll() }
    }
}
