import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: dialog
    property var accountSwitcher
    property string requestedId: ""
    parent: Overlay.overlay
    title: "Switch Accounts"
    width: Math.min(560, parent.width - 32)
    height: Math.min(content.implicitHeight + topPadding + bottomPadding + 58, parent.height - 48)
    x: (parent.width - width) / 2; y: (parent.height - height) / 2
    padding: 20; modal: true
    palette.window: Theme.surface
    palette.windowText: Theme.text
    palette.buttonText: Theme.text
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.surface; radius: 10; border.color: Theme.border }
    onOpened: { requestedId = ""; if (accountSwitcher) accountSwitcher.refreshAccounts() }
    Connections {
        target: dialog.accountSwitcher
        function onChanged() {
            if (dialog.requestedId && dialog.accountSwitcher.currentAccountId === dialog.requestedId) dialog.close()
        }
    }
    contentItem: HardScrollView {
        id: scroll
        contentWidth: availableWidth
        ColumnLayout {
            id: content
            width: scroll.availableWidth
            spacing: 12
            Text { Layout.fillWidth: true; text: "Your sign-ins stay in your desktop wallet."; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: 13; wrapMode: Text.WordWrap }
            Repeater {
                model: dialog.accountSwitcher ? dialog.accountSwitcher.accounts : []
                delegate: RowLayout {
                    id: accountRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Button {
                        Layout.fillWidth: true; implicitHeight: 60; padding: 10
                        enabled: !dialog.accountSwitcher.busy && !dialog.accountSwitcher.sessionChangeBlocked && !accountRow.modelData.current && !!accountRow.modelData.remembered
                        background: Rectangle { color: parent.hovered ? Theme.hover : Theme.rail; radius: 6; border.color: accountRow.modelData.current ? Theme.avatar : "transparent"; border.width: 1 }
                        contentItem: RowLayout {
                            spacing: 10
                            Rectangle {
                                width: 36; height: 36; radius: 18; color: Theme.avatar
                                Text { anchors.centerIn: parent; text: String(accountRow.modelData.name || "?").slice(0, 2).toUpperCase(); visible: !accountAvatar.ready; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 14 }
                                RoundedImage { id: accountAvatar; anchors.fill: parent; source: accountRow.modelData.avatarUrl || ""; radius: 18 }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 3
                                Text { Layout.fillWidth: true; text: accountRow.modelData.name || "Saved account"; textFormat: Text.PlainText; elide: Text.ElideRight; color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold }
                                Text { text: accountRow.modelData.current ? "Currently signed in" : accountRow.modelData.remembered ? "Switch to this account" : "Sign in again to remember this account"; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: 11 }
                            }
                        }
                        onClicked: {
                            dialog.requestedId = accountRow.modelData.id
                            if (!dialog.accountSwitcher.switchAccount(accountRow.modelData.id)) dialog.requestedId = ""
                        }
                        Accessible.name: "Switch to " + String(accountRow.modelData.name || "saved account")
                    }
                    Button {
                        text: "New Window"
                        enabled: !dialog.accountSwitcher.busy && !!accountRow.modelData.remembered
                        Accessible.name: "Open " + String(accountRow.modelData.name || "saved account") + " in a new window"
                        onClicked: { if (dialog.accountSwitcher.openAccountWindow(accountRow.modelData.id)) dialog.close() }
                        ToolTip.visible: hovered
                        ToolTip.text: "Open in New Window"
                    }
                    ActionButton {
                        iconName: "close"; quiet: true; hint: "Remove saved sign-in"; enabled: !dialog.accountSwitcher.busy && !dialog.accountSwitcher.sessionChangeBlocked
                        onClicked: { removeConfirm.accountId = accountRow.modelData.id; removeConfirm.accountName = accountRow.modelData.name || "this account"; removeConfirm.open() }
                    }
                }
            }
            Text { Layout.fillWidth: true; visible: dialog.accountSwitcher && !dialog.accountSwitcher.accounts.length; text: "Add an account to switch between saved sign-ins."; textFormat: Text.PlainText; color: Theme.muted; wrapMode: Text.WordWrap; font.pixelSize: 13 }
            RowLayout {
                Layout.fillWidth: true
                BusyIndicator { visible: dialog.accountSwitcher && dialog.accountSwitcher.busy; running: visible; Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
                Text { Layout.fillWidth: true; text: dialog.accountSwitcher ? dialog.accountSwitcher.sessionChangeBlocked ? "Your account changes are being saved. Please wait before changing accounts." : dialog.accountSwitcher.error || dialog.accountSwitcher.status : ""; textFormat: Text.PlainText; color: dialog.accountSwitcher && dialog.accountSwitcher.error ? Theme.danger : Theme.secondary; font.pixelSize: 12; wrapMode: Text.WordWrap }
            }
            ActionButton { Layout.fillWidth: true; text: "Add Account"; primary: true; enabled: !!dialog.accountSwitcher && dialog.accountSwitcher.canAdd; onClicked: { dialog.close(); dialog.accountSwitcher.addAccount() } }
            ActionButton { Layout.fillWidth: true; text: dialog.accountSwitcher && dialog.accountSwitcher.busy ? "Cancel" : "Close"; quiet: true; onClicked: { if (dialog.accountSwitcher && dialog.accountSwitcher.busy) dialog.accountSwitcher.cancelPending(); dialog.close() } }
        }
    }
    Dialog {
        id: removeConfirm
        parent: Overlay.overlay
        property string accountId: ""
        property string accountName: ""
        title: "Remove Account"
        modal: true; width: Math.min(380, parent.width - 32)
        x: (parent.width - width) / 2; y: (parent.height - height) / 2
        standardButtons: Dialog.Cancel | Dialog.Ok
        onOpened: standardButton(Dialog.Ok).enabled = Qt.binding(function() { return !dialog.accountSwitcher.busy && !dialog.accountSwitcher.sessionChangeBlocked })
        height: Math.min(180, parent.height - 48)
        contentItem: Text { text: "Remove the saved sign-in for " + removeConfirm.accountName + " from Relay?"; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.text; font.pixelSize: 14 }
        onAccepted: dialog.accountSwitcher.removeAccount(accountId)
    }
}
