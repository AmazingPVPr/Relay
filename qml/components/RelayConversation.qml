import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Pane {
    id: panel
    padding: 0
    background: Rectangle { color: Theme.canvas }
    palette.window: Theme.canvas
    palette.windowText: Theme.text
    palette.text: Theme.text
    palette.buttonText: Theme.text
    palette.button: Theme.surface
    palette.base: Theme.input
    palette.highlight: Theme.avatar
    palette.highlightedText: "#ffffff"
    palette.placeholderText: Theme.muted
    property var service: relayPeers
    property var connectionService: typeof relayConnections !== "undefined" ? relayConnections : null
    property var streamService: typeof relayStreams !== "undefined" ? relayStreams : null
    readonly property bool hasRelayStream: {
        const rows = streamService ? streamService.streams : []
        for (let i = 0; i < rows.length; ++i) if (rows[i].peer === currentPeer) return true
        return false
    }
    property string bindingKey: ""
    property string channelId: ""
    property string contactName: ""
    property bool setupOpen: false
    property int connectionRevision: 0
    property var drafts: ({})
    readonly property var connection: {
        const revision = connectionRevision
        return connectionService && bindingKey.length ? connectionService.conversation(bindingKey) : ({})
    }
    readonly property string currentPeer: {
        const rows = service.contacts
        return bindingKey.length ? service.peerForBinding(bindingKey) : service.selectedPeer
    }
    readonly property var currentContact: {
        const rows = service.contacts
        for (let i = 0; i < rows.length; ++i) if (rows[i].key === currentPeer) return rows[i]
        return ({})
    }
    readonly property var savedMessages: {
        const revision = service.contacts
        return currentPeer.length ? service.messagesForPeer(currentPeer) : []
    }
    readonly property bool canSend: currentPeer.length > 0 && currentContact.active !== false
        && (!bindingKey.length || !connectionService || !!connection.enabled) && !connection.identityChanged
    Connections { target: panel.connectionService; function onChanged() { panel.connectionRevision++ } }
    onCurrentPeerChanged: draft.text = drafts[currentPeer] || ""
    onSetupOpenChanged: { if (setupOpen) details.open(); else details.close() }
    function send() {
        if (canSend && service.sendText(currentPeer, draft.text)) {
            draft.clear(); drafts[currentPeer] = ""; chat.positionViewAtEnd()
        }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 16; spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Label { text: "Relay · encrypted text"; font.bold: true; color: Theme.text }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "watchRelayStream"; visible: panel.hasRelayStream && panel.canSend
                text: panel.streamService && panel.streamService.watchingPeer === panel.currentPeer ? "Watching via Relay" : "Watch via Relay"
                onClicked: panel.streamService.watch(panel.currentPeer)
            }
            Button { objectName: "relayDetailsButton"; text: "Details"; onClicked: panel.setupOpen = true }
        }
        CheckBox {
            objectName: "enableRelayConversation"
            visible: panel.bindingKey.length > 0 && panel.connectionService !== null
            text: "Enable Relay chat with this person"; checked: !!panel.connection.enabled
            onToggled: { panel.connectionService.setConversationEnabled(panel.bindingKey, panel.channelId, checked); panel.connectionRevision++ }
        }
        Label {
            objectName: "relayConnectionStatus"
            Layout.fillWidth: true; wrapMode: Text.Wrap; textFormat: Text.PlainText; color: Theme.muted
            text: panel.currentContact.connected ? "Connected directly · messages are encrypted"
                : panel.connection.status || (panel.currentPeer.length ? panel.currentContact.state || "Offline" : "Turn on Relay chat to invite " + (panel.contactName || "this person") + ". They can accept in their Relay app.")
        }
        RowLayout {
            Layout.fillWidth: true
            CheckBox { objectName: "enableRelayMcp"; text: "Enable Relay MCP"; checked: panel.service.mcpEnabled; onToggled: panel.service.mcpEnabled = checked }
            Item { Layout.fillWidth: true }
            Button {
                visible: !!panel.connection.enabled || (!panel.bindingKey.length && panel.currentPeer.length > 0)
                text: "Reconnect"
                onClicked: panel.connectionService && panel.bindingKey.length ? panel.connectionService.reconnect(panel.bindingKey) : panel.service.reconnect(panel.currentPeer)
            }
        }
        ListView {
            id: chat; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 10
            model: panel.savedMessages
            onCountChanged: Qt.callLater(positionViewAtEnd)
            delegate: ColumnLayout {
                required property var modelData
                width: chat.width; spacing: 3
                Label { Layout.fillWidth: true; textFormat: Text.PlainText; color: Theme.muted; text: (modelData.own ? "You" : panel.currentContact.name) + " · " + modelData.time + (modelData.pending ? " · Pending delivery" : modelData.own ? " · Saved by peer" : "") }
                TextArea {
                    Layout.fillWidth: true; readOnly: true; selectByMouse: true; wrapMode: TextEdit.WrapAnywhere; textFormat: TextEdit.PlainText
                    color: Theme.text; text: modelData.text; padding: 8
                    background: Rectangle { radius: 6; color: Theme.surface }
                }
            }
            ScrollBar.vertical: ScrollBar {}
            Label {
                anchors.centerIn: parent; width: Math.min(parent.width, 420); horizontalAlignment: Text.AlignHCenter
                visible: panel.savedMessages.length === 0; wrapMode: Text.Wrap; color: Theme.muted
                text: panel.connection.pending ? "They want to connect with you on Relay. Enable Relay chat above to accept."
                    : panel.connection.enabled ? "Your Relay conversation will appear here. Both people need to enable Relay chat."
                    : "Your private Relay conversation will appear here."
            }
        }
        Label { Layout.fillWidth: true; visible: panel.savedMessages.length >= 500; text: "Showing the latest 500 messages."; color: Theme.muted }
        Label { Layout.fillWidth: true; visible: panel.service.error.length > 0; textFormat: Text.PlainText; wrapMode: Text.Wrap; text: panel.service.error; color: Theme.danger }
        Label { Layout.fillWidth: true; visible: panel.canSend && !panel.currentContact.connected; wrapMode: Text.Wrap; color: Theme.muted; text: "Messages wait on your computer until both clients connect." }
        RowLayout {
            Layout.fillWidth: true; visible: panel.canSend
            ScrollView {
                Layout.fillWidth: true; Layout.preferredHeight: 78; clip: true
                TextArea {
                    id: draft; placeholderText: "Message via Relay · text only"; selectByMouse: true; textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap; onTextChanged: panel.drafts[panel.currentPeer] = text
                    Keys.onPressed: event => { if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) { panel.send(); event.accepted = true } }
                }
            }
            Button { text: "Send"; enabled: draft.text.trim().length > 0; onClicked: panel.send() }
        }
    }
    Dialog {
        id: details; objectName: "relayDetailsDialog"
        title: "Relay chat details"; modal: true; anchors.centerIn: parent
        palette: panel.palette
        background: Rectangle { color: Theme.rail; border.color: Theme.border; radius: 6 }
        width: Math.min(620, Math.max(280, panel.width - 32)); height: Math.min(690, Math.max(220, panel.height - 32))
        standardButtons: Dialog.Close
        onClosed: panel.setupOpen = false
        contentItem: ScrollView {
            id: detailsScroll; contentWidth: availableWidth; clip: true
            ColumnLayout {
                width: detailsScroll.availableWidth; spacing: 12
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.secondary; text: "Discord exchanges the invitation and encrypted connection setup. Relay chat text travels directly between the apps. The separate setup service is not used for these conversations. STUN helps find a direct route; some networks cannot connect directly." }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted; text: "The first Relay identity is trusted through this Discord account. A changed identity needs your review. You can compare the identities with your friend for extra assurance." }
                Label { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText; text: "Your identity: " + panel.service.identity; color: Theme.text }
                Label { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText; text: "Saved peer identity: " + (panel.currentPeer || "Not connected yet"); color: Theme.text }
                Label { Layout.fillWidth: true; visible: !!panel.connection.identityChanged; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText; text: "New invitation identity: " + (panel.connection.pendingKey || ""); color: Theme.warning }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "AI access is off for each conversation until you grant it. Authorized AI apps can read and send Relay text. Incoming text does not automatically run commands."; color: Theme.muted }
                CheckBox { visible: panel.currentPeer.length > 0 && panel.service.mcpEnabled; text: "Allow AI in this conversation"; checked: !!panel.currentContact.ai; enabled: panel.canSend; onToggled: panel.service.setAiAccess(panel.currentPeer, checked) }
                Label { text: "MCP configuration"; color: Theme.text }
                TextArea { objectName: "relayMcpConfiguration"; Layout.fillWidth: true; color: Theme.text; background: Rectangle { color: Theme.input; radius: 4 } readOnly: true; selectByMouse: true; wrapMode: TextEdit.WrapAnywhere; textFormat: TextEdit.PlainText; text: panel.service.mcpConfiguration }
                Label { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; textFormat: Text.PlainText; text: "Encrypted local saves: " + panel.service.storagePath; color: Theme.muted }
                Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "Your local identity key unlocks these saves. Protect your computer and backups. Clearing history affects only your copy."; color: Theme.muted }
                Button { visible: panel.currentPeer.length > 0; text: "Clear my saved history…"; onClicked: { details.close(); clearConfirmation.open() } }
                Button { visible: panel.bindingKey.length > 0 && panel.connectionService !== null; text: "Unlink trusted identity…"; onClicked: { details.close(); resetConfirmation.open() } }
                Label { visible: !panel.bindingKey.length; text: "Manual connections"; font.bold: true; color: Theme.text }
                ColumnLayout {
                    visible: !panel.bindingKey.length; Layout.fillWidth: true
                    CheckBox { text: "Online"; checked: panel.service.enabled; onToggled: panel.service.enabled = checked }
                    Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: "For a manual connection, exchange contact cards and use the same setup service. Both people must add each other."; color: Theme.muted }
                    TextArea { Layout.fillWidth: true; readOnly: true; selectByMouse: true; wrapMode: TextEdit.WrapAnywhere; textFormat: TextEdit.PlainText; text: panel.service.contactCard }
                    TextField { id: friendName; Layout.fillWidth: true; placeholderText: "Friend's display name"; maximumLength: 80 }
                    TextArea { id: friendCard; Layout.fillWidth: true; placeholderText: "Friend's Relay contact card"; textFormat: TextEdit.PlainText; wrapMode: TextEdit.WrapAnywhere; selectByMouse: true }
                    Button { text: "Add contact"; onClicked: { if (panel.service.addContact(friendCard.text.trim(), friendName.text).length) friendCard.clear() } }
                    ComboBox { Layout.fillWidth: true; model: panel.service.contacts; textRole: "name"; valueRole: "key"; onActivated: panel.service.selectedPeer = currentValue }
                    TextField { id: setupAddress; Layout.fillWidth: true; text: panel.service.rendezvousUrl; enabled: !panel.service.enabled; selectByMouse: true; placeholderText: "Manual setup service" }
                    Button { text: "Save manual setup service"; enabled: !panel.service.enabled; onClicked: panel.service.rendezvousUrl = setupAddress.text }
                }
            }
        }
    }
    Dialog {
        id: clearConfirmation; palette: panel.palette
        background: Rectangle { color: Theme.rail; border.color: Theme.border; radius: 6 }
        width: Math.min(430, panel.width - 32); title: "Clear local Relay history?"; modal: true; anchors.centerIn: parent; standardButtons: Dialog.Ok | Dialog.Cancel
        Label { width: parent.width; wrapMode: Text.Wrap; text: "This deletes your saved and unsent messages here. It cannot delete the other person's copy." }
        onAccepted: panel.service.clearHistory(panel.currentPeer)
    }
    Dialog {
        id: resetConfirmation; palette: panel.palette
        background: Rectangle { color: Theme.rail; border.color: Theme.border; radius: 6 }
        width: Math.min(430, panel.width - 32); title: "Unlink this Relay identity?"; modal: true; anchors.centerIn: parent; standardButtons: Dialog.Ok | Dialog.Cancel
        Label { width: parent.width; wrapMode: Text.Wrap; text: "Relay chat and AI access will turn off. Saved history stays with the old identity. Verify the new invitation with your friend before enabling chat again." }
        onAccepted: panel.connectionService.resetIdentity(panel.bindingKey)
    }
}
