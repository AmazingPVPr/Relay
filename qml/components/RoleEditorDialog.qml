import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: editor
    objectName: "roleEditor"
    property var role: ({})
    property string serverId: ""
    property var selectedKeys: []
    property var permissionOptions: []
    readonly property bool editable: (!role.id || !!role.editable) && !!guildManagement.capabilities.manageRoles
    modal: true
    width: Math.min(560, parent ? parent.width - 32 : 560)
    height: Math.min(700, parent ? parent.height - 32 : 700)
    anchors.centerIn: parent
    title: role.id ? "Edit role" : "Create role"
    padding: 22
    background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
    function showRole(guildId, value) {
        serverId = guildId; role = value || {}
        nameField.text = role.name || "New role"
        colorField.text = role.color || "#000000"
        hoist.checked = !!role.hoist; mentionable.checked = !!role.mentionable
        permissionOptions = role.permissionOptions || (guildManagement.roles.length ? guildManagement.roles[0].permissionOptions || [] : [])
        selectedKeys = role.id ? permissionOptions.filter(p => p.enabled).map(p => p.key) : []
        open()
    }
    function toggleKey(key, selected) {
        let keys = selectedKeys.slice()
        if (selected && keys.indexOf(key) < 0) keys.push(key)
        if (!selected) keys = keys.filter(k => k !== key)
        selectedKeys = keys
    }
    contentItem: HardScrollView {
        id: scroll
        ColumnLayout {
            width: scroll.availableWidth
            spacing: 14
            Label { text: "Role name"; color: Theme.text }
            TextField { id: nameField; Layout.fillWidth: true; maximumLength: 100; selectByMouse: true; readOnly: !editor.editable }
            Label { text: "Role color"; color: Theme.text }
            RowLayout {
                Rectangle { width: 28; height: 28; radius: 4; color: /^#[0-9a-fA-F]{6}$/.test(colorField.text) ? colorField.text : Theme.border; border.color: Theme.secondary }
                TextField { id: colorField; Layout.fillWidth: true; maximumLength: 7; placeholderText: "#5865f2"; selectByMouse: true; readOnly: !editor.editable }
            }
            CheckBox { id: hoist; text: "Display role members separately"; Layout.fillWidth: true; enabled: editor.editable }
            CheckBox { id: mentionable; text: "Allow anyone to mention this role"; Layout.fillWidth: true; enabled: editor.editable }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
            Label { text: "Permissions"; color: Theme.text; font.bold: true }
            Repeater {
                model: editor.permissionOptions
                delegate: CheckBox {
                    required property var modelData
                    Layout.fillWidth: true
                    text: modelData.label
                    enabled: editor.editable
                    checked: editor.selectedKeys.indexOf(modelData.key) >= 0
                    onToggled: editor.toggleKey(modelData.key, checked)
                    contentItem: Text { text: parent.text; color: Theme.text; wrapMode: Text.Wrap; leftPadding: parent.indicator.width + parent.spacing; verticalAlignment: Text.AlignVCenter; font: parent.font }
                }
            }
            Label { Layout.fillWidth: true; visible: guildManagement.error.length > 0; text: guildManagement.error; wrapMode: Text.Wrap; color: Theme.danger; textFormat: Text.PlainText }
        }
    }
    footer: DialogButtonBox {
        Button { text: "Cancel"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole; onClicked: editor.close() }
        Button {
            text: guildManagement.mutating ? "Saving…" : "Save role"
            enabled: editor.editable && !guildManagement.mutating && guildManagement.guildId === editor.serverId && nameField.text.trim().length > 0 && /^#[0-9a-fA-F]{6}$/.test(colorField.text)
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: guildManagement.execute(editor.role.id ? "editRole" : "createRole", editor.role.id || "", {name:nameField.text.trim(),color:parseInt(colorField.text.substring(1),16),hoist:hoist.checked,mentionable:mentionable.checked,permissionKeys:editor.selectedKeys})
        }
    }
    Connections {
        target: guildManagement
        function onActionCompleted(action, id, result) {
            if (editor.visible && (action === "editRole" || action === "createRole")) editor.close()
        }
    }
}
