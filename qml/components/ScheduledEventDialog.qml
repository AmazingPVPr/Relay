import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: editor
    objectName: "eventEditor"
    property var event: ({})
    property string serverId: ""
    modal: true
    width: Math.min(560, parent ? parent.width - 32 : 560)
    height: Math.min(670, parent ? parent.height - 32 : 670)
    anchors.centerIn: parent
    title: event.id ? "Edit event" : "Create event"
    padding: 22
    background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
    function showEvent(guildId, value) {
        serverId = guildId; event = value || {}
        nameField.text = event.name || ""
        descriptionField.text = event.description || ""
        kind.currentIndex = event.entityType === 2 ? 1 : event.entityType === 1 ? 2 : 0
        channelChoice.currentIndex = channelChoice.indexOfValue(event.channelId || "")
        locationField.text = event.location || ""
        startField.setDate(event.scheduledStartTime || new Date(Date.now()+3600000))
        endField.setDate(event.scheduledEndTime || new Date(Date.now()+7200000))
        open()
    }
    contentItem: HardScrollView {
        id: scroll
        ColumnLayout {
            width: scroll.availableWidth; spacing: 12
            Label { text: "Event name"; color: Theme.text }
            TextField { id: nameField; Layout.fillWidth: true; maximumLength: 100; selectByMouse: true }
            Label { text: "Description"; color: Theme.text }
            TextArea { id: descriptionField; Layout.fillWidth: true; Layout.preferredHeight: 90; wrapMode: TextEdit.Wrap; selectByMouse: true; placeholderText: "What's happening?" }
            Label { text: "Location"; color: Theme.text }
            ComboBox { id: kind; Layout.fillWidth: true; model: ["Somewhere else", "Voice channel", "Stage channel"] }
            TextField { id: locationField; Layout.fillWidth: true; visible: kind.currentIndex === 0; maximumLength: 100; placeholderText: "Location or event link"; selectByMouse: true }
            ComboBox { id: channelChoice; Layout.fillWidth: true; visible: kind.currentIndex !== 0; model: (guildManagement.channels || []).filter(c => c.type === (kind.currentIndex === 1 ? 2 : 13)); textRole: "name"; valueRole: "id"; displayText: currentIndex < 0 ? "Choose a channel" : currentText }
            Label { text: "Start · your local time"; color: Theme.text }
            LocalDateTimeField { id: startField; Layout.fillWidth: true }
            Label { text: "End · your local time"; color: Theme.text }
            LocalDateTimeField { id: endField; Layout.fillWidth: true }
            Label { Layout.fillWidth: true; text: "Everyone sees the event in their own time zone."; wrapMode: Text.Wrap; color: Theme.muted; font.pixelSize: 12 }
            Label { Layout.fillWidth: true; visible: guildManagement.error.length > 0; text: guildManagement.error; wrapMode: Text.Wrap; color: Theme.danger; textFormat: Text.PlainText }
        }
    }
    footer: DialogButtonBox {
        Button { text: "Cancel"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole; onClicked: editor.close() }
        Button {
            text: guildManagement.mutating ? "Saving…" : "Save event"
            enabled: !guildManagement.mutating && guildManagement.guildId === editor.serverId && nameField.text.trim().length > 0 && descriptionField.text.length <= 1000 && startField.value !== null && endField.value !== null && endField.value > startField.value && (kind.currentIndex === 0 ? locationField.text.trim().length > 0 : channelChoice.currentIndex >= 0)
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: guildManagement.execute(editor.event.id ? "editEvent" : "createEvent", editor.event.id || "", {name:nameField.text.trim(),description:descriptionField.text,entityType:kind.currentIndex === 0 ? 3 : kind.currentIndex === 1 ? 2 : 1,channelId:kind.currentIndex === 0 ? "" : channelChoice.currentValue,location:locationField.text.trim(),scheduledStartTime:startField.value.toISOString(),scheduledEndTime:endField.value.toISOString()})
        }
    }
    Connections {
        target: guildManagement
        function onActionCompleted(action, id, result) { if (editor.visible && (action === "createEvent" || action === "editEvent")) editor.close() }
    }
}
