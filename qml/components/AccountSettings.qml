import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
    id: page
    objectName: "accountSettingsPage"
    readonly property var service: typeof accountSettings !== "undefined" ? accountSettings : null
    readonly property var account: service ? service.account : ({})
    readonly property var capabilities: service ? service.capabilities : ({})
    property bool revealEmail: false
    property bool revealPhone: false
    readonly property bool modalOpen: editDialog.visible || mfaDialog.visible || disableMfaDialog.visible
    signal signOutRequested()
    signal navigate(string page, string section)
    contentWidth: availableWidth
    function maskedEmail() {
        const value = account.email || ""
        return !value ? "Not provided" : revealEmail ? value : value.indexOf("@") >= 0 ? "••••••" + value.substring(value.indexOf("@")) : "••••••"
    }
    function maskedPhone() { const value = account.phone || ""; return !value ? "Not provided" : revealPhone ? value : "••••••" + value.slice(-4) }
    function edit(field, title, value) {
        editDialog.field = field; editDialog.title = title; editValue.text = value || ""
        currentPassword.clear(); passwordConfirmation.clear(); editDialog.open()
    }
    Component.onCompleted: if (service) service.loadPage("account")
    ColumnLayout {
        width: page.availableWidth; spacing: 27
        SettingsSectionTitle { section: "account-info"; title: "Account Info" }
        AccountSettingsStatus { service: page.service }
        AccountSettingRow {
            title: "Display Name"; value: page.account.globalName || discord.currentUserName || ""
            actionText: page.capabilities.updateAccount ? "Edit" : ""; actionEnabled: !!page.service && !page.service.busy && !page.service.mutating
            onTriggered: page.edit("globalName", "Edit Display Name", page.account.globalName || "")
        }
        AccountSettingRow {
            title: "Username"; value: page.account.username || ""
            actionText: page.capabilities.updateAccount ? "Edit" : ""; actionEnabled: !!page.service && !page.service.busy && !page.service.mutating
            onTriggered: page.edit("username", "Edit Username", page.account.username || "")
        }
        AccountSettingRow {
            title: "Email"; value: page.maskedEmail(); secondaryText: page.account.email ? page.revealEmail ? "Hide" : "Reveal" : ""
            actionText: page.capabilities.updateAccount ? "Edit" : ""; actionEnabled: !!page.service && !page.service.busy && !page.service.mutating
            onSecondaryTriggered: page.revealEmail = !page.revealEmail
            onTriggered: page.edit("email", "Edit Email", page.account.email || "")
        }
        AccountSettingRow {
            title: "Phone Number"; value: page.maskedPhone(); secondaryText: page.account.phone ? page.revealPhone ? "Hide" : "Reveal" : ""
            description: "Phone changes are not available in Relay."
            onSecondaryTriggered: page.revealPhone = !page.revealPhone
        }
        SettingsSectionTitle { section: "security"; title: "Password & Security"; divider: true }
        AccountSettingRow {
            title: "Password"; actionText: page.capabilities.updateAccount ? "Change Password" : ""
            actionEnabled: !!page.service && !page.service.busy && !page.service.mutating
            onTriggered: page.edit("newPassword", "Change Password", "")
        }
        AccountSettingRow {
            title: "Multi-Factor Authentication"; value: page.account.mfaEnabled === undefined ? "Unavailable" : page.account.mfaEnabled ? "Enabled" : "Not enabled"
            description: page.capabilities.mfaEnroll || page.capabilities.mfaDisable || page.capabilities.backupCodes ? "" : "Authentication setup is not available through this account connection."
            actionText: page.account.mfaEnabled ? page.capabilities.mfaDisable ? "Disable Authenticator" : "" : page.capabilities.mfaEnroll ? "Enable Authenticator" : ""
            actionEnabled: !!page.service && !page.service.busy && !page.service.mutating
            onTriggered: { if (page.account.mfaEnabled) disableMfaDialog.open(); else mfaDialog.openMode("enroll") }
        }
        AccountSettingRow {
            visible: !!page.account.mfaEnabled && !!page.capabilities.backupCodes
            title: "Backup Codes"; description: "Keep a private copy in case you lose access to your authenticator."
            actionText: "View Backup Codes"; actionEnabled: !!page.service && !page.service.busy && !page.service.mutating
            onTriggered: mfaDialog.openMode("backup")
        }
        AccountSettingRow { title: "Logged-in Devices"; actionText: "View Devices"; onTriggered: page.navigate("sessions", "") }
        SettingsSectionTitle { section: "account-status"; title: "Account Status"; divider: true }
        AccountSettingRow { title: "Email verification"; value: page.account.verified === undefined ? "Unavailable" : page.account.verified ? "Verified" : "Not verified" }
        Text { Layout.fillWidth: true; text: "Discord has not provided account standing or age-group details through this connection. Account deletion and deactivation are not available in Relay."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        SettingsSectionTitle { section: "family"; title: "Family Center"; divider: true; description: "View the family connections and information available to your account." }
        AccountSettingRow { title: "Family Center"; actionText: "View Family Center"; onTriggered: page.navigate("family", "") }
        Rectangle { Layout.fillWidth: true; Layout.topMargin: 16; implicitHeight: 1; color: Theme.border }
        ActionButton { text: discord.demoMode ? "Exit preview" : "Log Out"; destructive: true; quiet: true; onClicked: page.signOutRequested() }
    }
    Dialog {
        id: editDialog
        objectName: "accountEditDialog"
        parent: Overlay.overlay; anchors.centerIn: parent; width: Math.min(460, parent ? parent.width - 32 : 460)
        modal: true; closePolicy: page.service && page.service.mutating ? Popup.NoAutoClose : Popup.CloseOnEscape
        property string field: ""
        readonly property bool passwordMode: field === "newPassword"
        function submit() {
            const values = ({})
            values[field] = editValue.text
            if (currentPassword.text) values.currentPassword = currentPassword.text
            const accepted = page.service.execute("update-account", "", values)
            currentPassword.clear(); passwordConfirmation.clear()
            if (passwordMode) editValue.clear()
            if (accepted && !page.service.mutating) close()
        }
        onOpened: editValue.forceActiveFocus()
        onClosed: { currentPassword.clear(); editValue.clear(); passwordConfirmation.clear() }
        contentItem: ColumnLayout {
            spacing: 14
            Label { Layout.fillWidth: true; text: editDialog.passwordMode ? "New password" : editDialog.field === "globalName" ? "Display name" : editDialog.field === "email" ? "Email" : "Username"; wrapMode: Text.WordWrap }
            TextField {
                id: editValue; objectName: "accountEditValue"; Layout.fillWidth: true
                enabled: !!page.service && !page.service.mutating
                echoMode: editDialog.passwordMode ? TextInput.Password : TextInput.Normal
                maximumLength: editDialog.passwordMode || editDialog.field === "email" ? 254 : 32
                selectByMouse: !editDialog.passwordMode; inputMethodHints: editDialog.passwordMode ? Qt.ImhSensitiveData | Qt.ImhNoPredictiveText : editDialog.field === "email" ? Qt.ImhEmailCharactersOnly : Qt.ImhNone
                Accessible.name: editDialog.passwordMode ? "New password" : "New account value"
            }
            Label { visible: editDialog.passwordMode; text: "Confirm new password" }
            TextField { id: passwordConfirmation; visible: editDialog.passwordMode; enabled: !!page.service && !page.service.mutating; Layout.fillWidth: true; echoMode: TextInput.Password; maximumLength: 254; inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText; Accessible.name: "Confirm new password" }
            Label { visible: editDialog.field !== "globalName"; text: "Current password" }
            TextField { id: currentPassword; objectName: "accountCurrentPassword"; visible: editDialog.field !== "globalName"; enabled: !!page.service && !page.service.mutating; Layout.fillWidth: true; echoMode: TextInput.Password; maximumLength: 254; inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText; Accessible.name: "Current password" }
            Text { Layout.fillWidth: true; visible: !!page.service && page.service.mutating; text: "Saving account changes…"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
            Text { Layout.fillWidth: true; text: page.service ? page.service.error : "Account services are unavailable."; visible: text.length > 0; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        }
        footer: DialogButtonBox {
            Button { text: "Cancel"; enabled: !!page.service && !page.service.mutating; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole; onClicked: editDialog.close() }
            Button {
                text: "Save"; highlighted: true; DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
                enabled: !!page.service && !page.service.mutating && (editDialog.field === "globalName" || currentPassword.text.length > 0) && (!editDialog.passwordMode || editValue.text.length >= 8 && editValue.text === passwordConfirmation.text)
                onClicked: editDialog.submit()
            }
        }
    }
    AccountMfaDialog { id: mfaDialog; service: page.service }
    Connections {
        target: page.service; ignoreUnknownSignals: true
        function onActionCompleted(action) {
            if (action === "update-account" && editDialog.visible) editDialog.close()
            if (action === "mfa-disable" && disableMfaDialog.visible) disableMfaDialog.close()
        }
    }
    Dialog {
        id: disableMfaDialog; objectName: "disableMfaDialog"
        parent: Overlay.overlay; anchors.centerIn: parent; width: Math.min(450, parent ? parent.width - 32 : 450)
        title: "Disable Authenticator?"; modal: true
        closePolicy: page.service && page.service.mutating ? Popup.NoAutoClose : Popup.CloseOnEscape
        contentItem: ColumnLayout { Text { Layout.fillWidth: true; text: "Your account will lose this extra sign-in protection. Discord may ask you to verify before continuing."; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap } }
        footer: DialogButtonBox {
            Button { text: "Cancel"; enabled: !!page.service && !page.service.mutating; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole; onClicked: disableMfaDialog.close() }
            Button { text: page.service && page.service.mutating ? "Saving…" : "Disable Authenticator"; enabled: !!page.service && !page.service.mutating; DialogButtonBox.buttonRole: DialogButtonBox.ActionRole; onClicked: { if (page.service.execute("mfa-disable", "", {}) && !page.service.mutating) disableMfaDialog.close() } }
        }
    }
}
