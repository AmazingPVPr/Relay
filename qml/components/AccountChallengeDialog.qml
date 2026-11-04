import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: dialog
    objectName: "accountChallengeDialog"
    property var service
    property bool windowActive: true
    readonly property var methods: service && service.challengeMethods ? service.challengeMethods : []
    readonly property string method: methods[methodSelector.currentIndex] || ""
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(450, parent ? parent.width - 32 : 450)
    title: "Verify Your Account"; modal: true; closePolicy: service && service.mutating ? Popup.NoAutoClose : Popup.CloseOnEscape
    function synchronize() {
        if (windowActive && methods.length && !visible) open()
        else if (!methods.length && visible) close()
    }
    function submit() {
        const value = verification.text
        verification.clear()
        service.confirmMfa(method, value)
    }
    onOpened: { methodSelector.currentIndex = 0; verification.forceActiveFocus() }
    onClosed: { verification.clear(); if (methods.length && service) service.cancelMfa() }
    onWindowActiveChanged: if (!windowActive) close()
    Connections { target: dialog.service; ignoreUnknownSignals: true; function onChanged() { dialog.synchronize() } }
    contentItem: ColumnLayout {
        spacing: 15
        Text { Layout.fillWidth: true; text: "Discord requires additional verification before completing the change you requested."; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
        ComboBox {
            id: methodSelector; objectName: "accountChallengeMethod"; Layout.fillWidth: true
            model: dialog.methods.map(method => ({key:method,label:({totp:"Authenticator code",backup:"Backup code",password:"Account password"})[method] || method}))
            textRole: "label"; visible: count > 1
            onActivated: verification.clear()
            Accessible.name: "Account verification method"
        }
        Label { Layout.fillWidth: true; text: ({totp:"Enter a code from your authenticator",backup:"Enter an unused backup code",password:"Enter your current password"})[dialog.method] || "Verification"; wrapMode: Text.WordWrap }
        TextField {
            id: verification; objectName: "accountChallengeValue"; Layout.fillWidth: true
            echoMode: dialog.method === "password" ? TextInput.Password : TextInput.Normal
            maximumLength: 254; inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
            onAccepted: if (text.length && !!dialog.service && !dialog.service.mutating) dialog.submit()
            Accessible.name: "Account verification value"
        }
        Text { Layout.fillWidth: true; text: dialog.service ? dialog.service.error : ""; visible: text.length > 0; color: Theme.danger; textFormat: Text.PlainText; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
    }
    footer: DialogButtonBox {
        Button { text: "Cancel"; enabled: !!dialog.service && !dialog.service.mutating; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole; onClicked: dialog.close() }
        Button { objectName: "accountChallengeConfirm"; highlighted: true; text: dialog.service && dialog.service.mutating ? "Verifying…" : "Verify"; enabled: verification.text.length > 0 && !!dialog.service && !dialog.service.mutating; DialogButtonBox.buttonRole: DialogButtonBox.ActionRole; onClicked: dialog.submit() }
    }
}
