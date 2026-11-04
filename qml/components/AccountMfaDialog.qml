import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: dialog
    objectName: "accountMfaDialog"
    property var service
    property string mode: "enroll"
    property bool sessionOpen: false
    readonly property var details: service ? service.details : ({})
    readonly property var codes: details.backupCodes || []
    readonly property bool showingCodes: codes.length > 0
    readonly property bool emailStage: details.backupStage === "email"
    readonly property bool working: !!service && service.mutating
    parent: Overlay.overlay; anchors.centerIn: parent
    width: Math.min(490, parent ? parent.width - 32 : 490)
    height: Math.min(implicitHeight, parent ? parent.height - 40 : implicitHeight)
    modal: true; closePolicy: working ? Popup.NoAutoClose : Popup.CloseOnEscape
    title: showingCodes ? "Your Backup Codes" : mode === "enroll" ? "Enable an Authenticator" : "View Backup Codes"
    function openMode(value) {
        if (!service) return
        mode = value; verificationCode.clear(); backupPassword.clear(); emailKey.clear()
        if (mode === "enroll" && !service.execute("mfa-begin", "", {})) return
        sessionOpen = true; open()
    }
    function confirmEnrollment() {
        const code = verificationCode.text
        verificationCode.clear()
        service.execute("mfa-confirm", "", {code: code})
    }
    function sendBackupVerification() {
        const password = backupPassword.text
        backupPassword.clear()
        service.execute("mfa-backup-send", "", {currentPassword: password})
    }
    function confirmBackupVerification() {
        const key = emailKey.text
        emailKey.clear()
        service.execute("mfa-backup-confirm", "", {key: key, regenerate: false})
    }
    function clearSession() {
        if (working) return
        verificationCode.clear(); backupPassword.clear(); emailKey.clear()
        if (sessionOpen && service) service.execute("mfa-cancel", "", {})
        sessionOpen = false
    }
    onClosed: clearSession()
    Component.onDestruction: clearSession()
    contentItem: HardScrollView {
        id: scroll
        implicitHeight: contentColumn.implicitHeight
        contentWidth: availableWidth
        ColumnLayout {
            id: contentColumn
            width: scroll.availableWidth; spacing: 16
            Text { Layout.fillWidth: true; visible: dialog.mode === "enroll" && !dialog.showingCodes; text: "Scan this code with your authenticator app, then enter the six-digit code it generates."; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
            Rectangle {
                Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 190; Layout.preferredHeight: 190
                color: "white"; radius: 5
                visible: dialog.mode === "enroll" && !dialog.showingCodes && !!dialog.details.mfaQrImage
                Image { anchors.fill: parent; anchors.margins: 10; source: dialog.details.mfaQrImage || ""; fillMode: Image.PreserveAspectFit; smooth: false }
            }
            Label { visible: dialog.mode === "enroll" && !dialog.showingCodes; text: "Manual setup key" }
            TextArea {
                objectName: "mfaManualKey"; Layout.fillWidth: true
                visible: dialog.mode === "enroll" && !dialog.showingCodes
                text: dialog.details.mfaSecret || ""; readOnly: true; selectByMouse: true; wrapMode: TextEdit.WrapAnywhere
                font.family: "monospace"; inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                Accessible.name: "Authenticator manual setup key"
            }
            TextField {
                id: verificationCode; objectName: "mfaVerificationCode"; Layout.fillWidth: true
                visible: dialog.mode === "enroll" && !dialog.showingCodes
                placeholderText: "Six-digit authentication code"; maximumLength: 6
                validator: RegularExpressionValidator { regularExpression: /^[0-9]{0,6}$/ }
                inputMethodHints: Qt.ImhDigitsOnly | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                Accessible.name: "Six-digit authentication code"
            }
            Text { Layout.fillWidth: true; visible: dialog.mode === "backup" && !dialog.emailStage && !dialog.showingCodes; text: "Enter your current password to send a verification code to your account email."; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
            TextField {
                id: backupPassword; objectName: "mfaBackupPassword"; Layout.fillWidth: true
                visible: dialog.mode === "backup" && !dialog.emailStage && !dialog.showingCodes
                placeholderText: "Current password"; echoMode: TextInput.Password; maximumLength: 254
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText; Accessible.name: "Current password for backup codes"
            }
            Text { Layout.fillWidth: true; visible: dialog.mode === "backup" && dialog.emailStage && !dialog.showingCodes; text: "Check your email and enter its verification code to view your backup codes."; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
            TextField {
                id: emailKey; objectName: "mfaBackupEmailKey"; Layout.fillWidth: true
                visible: dialog.mode === "backup" && dialog.emailStage && !dialog.showingCodes
                placeholderText: "Email verification code"; maximumLength: 128
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText; Accessible.name: "Email verification code for backup codes"
            }
            Text { Layout.fillWidth: true; visible: dialog.showingCodes; text: "Each backup code can be used once. Store these somewhere private. They disappear from Relay when you close this window."; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
            TextArea {
                objectName: "mfaBackupCodes"; Layout.fillWidth: true; visible: dialog.showingCodes
                text: dialog.codes.map(code => code.code + (code.consumed ? "  (used)" : "")).join("\n")
                readOnly: true; selectByMouse: true; wrapMode: TextEdit.NoWrap; font.family: "monospace"
                Accessible.name: "Single-use backup codes"
            }
            RowLayout {
                Layout.fillWidth: true; visible: dialog.working
                BusyIndicator { running: visible; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
                Text { Layout.fillWidth: true; text: "Verifying…"; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale) }
            }
            Text { Layout.fillWidth: true; text: dialog.service ? dialog.service.error : ""; visible: text.length > 0; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        }
    }
    footer: DialogButtonBox {
        Button { text: dialog.showingCodes ? "Done" : "Cancel"; enabled: !dialog.working; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole; onClicked: dialog.close() }
        Button {
            objectName: "mfaConfirmButton"
            highlighted: true
            visible: !dialog.showingCodes
            text: dialog.mode === "enroll" ? "Enable Authenticator" : dialog.emailStage ? "View Backup Codes" : "Send Verification Email"
            enabled: !!dialog.service && !dialog.working && (dialog.mode === "enroll" ? verificationCode.acceptableInput && verificationCode.text.length === 6 : dialog.emailStage ? emailKey.text.length > 0 : backupPassword.text.length > 0)
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: { if (dialog.mode === "enroll") dialog.confirmEnrollment(); else if (dialog.emailStage) dialog.confirmBackupVerification(); else dialog.sendBackupVerification() }
        }
    }
}
