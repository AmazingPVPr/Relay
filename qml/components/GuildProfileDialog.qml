import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: dialog
    parent: Overlay.overlay
    property string guildId: ""
    property bool submitted: false
    readonly property var guild: { const revision = contextActions.revision; return contextActions.guildInfo(guildId) }
    title: "Edit Per-server Profile"
    width: Math.min(460, parent.width - 32)
    x: (parent.width - width) / 2
    y: Math.max(16, (parent.height - height) / 2)
    modal: true
    closePolicy: contextActions.busy && submitted ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
    function openFor(id) {
        guildId = id; submitted = false
        nickname.text = String(contextActions.guildInfo(id).nickname || "")
        contextActions.clearError(); open()
    }
    contentItem: ColumnLayout {
        spacing: 14
        Text { Layout.fillWidth: true; text: dialog.guild.name || "Server profile"; textFormat: Text.PlainText; color: Theme.text; font.weight: Font.DemiBold; wrapMode: Text.Wrap }
        Text { Layout.fillWidth: true; text: "Choose how your name appears in this server. Leave it blank to use your display name."; color: Theme.secondary; wrapMode: Text.Wrap; font.pixelSize: Math.round(13 * preferences.fontScale) }
        Label { text: "Server Nickname"; color: Theme.text }
        TextField { id: nickname; Layout.fillWidth: true; maximumLength: 32; placeholderText: discord.currentUserName; enabled: !contextActions.busy; selectByMouse: true }
        Text { Layout.fillWidth: true; visible: contextActions.error.length > 0; text: contextActions.error; textFormat: Text.PlainText; color: Theme.danger; wrapMode: Text.Wrap }
    }
    footer: DialogButtonBox {
        Button { text: "Cancel"; enabled: !dialog.submitted || !contextActions.busy; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
        Button { text: dialog.submitted && contextActions.busy ? "Saving…" : "Save Changes"; enabled: !contextActions.busy; DialogButtonBox.buttonRole: DialogButtonBox.ActionRole; onClicked: dialog.submitted = contextActions.setGuildNickname(dialog.guildId,nickname.text) }
        onRejected: dialog.close()
    }
    Connections {
        target: contextActions
        function onChanged() {
            if (dialog.submitted && !contextActions.busy) {
                dialog.submitted = false
                if (!contextActions.error.length) dialog.close()
            }
        }
    }
}
