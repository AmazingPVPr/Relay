import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

ColumnLayout {
    id: email
    objectName: "emailNotificationSettings"
    readonly property var service: typeof accountSettings !== "undefined" ? accountSettings : null
    property bool requested: false
    readonly property bool modalOpen: unsubscribeDialog.visible
    readonly property bool ready: !!service && service.page === "email" && !!service.capabilities.emailPreferences
    readonly property var categories: ready && service.details.categories ? service.details.categories : ({})
    spacing: 18
    function load(force) { if (!service || !force && requested && service.page === "email") return; requested = true; service.loadPage("email") }
    function save(key, value) { const changes = ({}); changes[key] = value; service.execute("update-email-preferences", "", changes) }
    Text { Layout.fillWidth: true; text: "Choose which emails Discord sends to your account. These preferences apply across your Discord apps."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
    ActionButton { objectName: "loadEmailPreferences"; visible: !email.requested; text: "Load Email Preferences"; enabled: !!email.service && !email.service.mutating; onClicked: email.load() }
    AccountSettingsStatus { service: email.service; visible: email.requested; refreshAction: () => email.load(true) }
    Text { Layout.fillWidth: true; visible: email.ready && email.service.details.initialized === false; text: "Discord has not saved email preferences yet. Your choices below will initialize them."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
    Repeater {
        model: [
            {key:"communication",title:"Communication",description:"Account and service communications."},
            {key:"social",title:"Social",description:"Updates about activity involving your friends and communities."},
            {key:"recommendations_and_events",title:"Recommendations & Events",description:"Recommended communities, events, and things to try."},
            {key:"tips",title:"Tips",description:"Tips for getting more out of Discord."},
            {key:"updates_and_announcements",title:"Updates & Announcements",description:"Product news, new features, and announcements."},
            {key:"family_center_digest",title:"Family Center Digest",description:"Email summaries from your Family Center connections."}]
        delegate: SettingsToggle {
            required property var modelData
            objectName: "emailCategory_" + modelData.key
            Layout.fillWidth: true; visible: email.ready && email.categories[modelData.key] !== undefined
            title: modelData.title; description: modelData.description
            checked: !!email.categories[modelData.key]; enabled: email.ready && !email.service.mutating
            onToggled: value => email.save(modelData.key, value)
        }
    }
    ActionButton { objectName: "unsubscribeMarketingEmails"; visible: email.ready; text: "Unsubscribe from Marketing Emails"; quiet: true; enabled: email.ready && !email.service.mutating; onClicked: unsubscribeDialog.open() }
    Text { Layout.fillWidth: true; visible: email.ready; text: "Essential security and account emails may still be sent."; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap }
    Dialog {
        id: unsubscribeDialog; objectName: "unsubscribeMarketingDialog"
        parent: Overlay.overlay; anchors.centerIn: parent; width: Math.min(450, parent ? parent.width - 32 : 450)
        title: "Unsubscribe from Marketing Emails?"; modal: true
        closePolicy: email.service && email.service.mutating ? Popup.NoAutoClose : Popup.CloseOnEscape
        contentItem: ColumnLayout { Text { Layout.fillWidth: true; text: "Turn off Tips, Recommendations & Events, and Updates & Announcements emails? Your other email preferences will stay the same."; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap } }
        footer: DialogButtonBox {
            Button { text: "Cancel"; enabled: email.ready && !email.service.mutating; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole; onClicked: unsubscribeDialog.close() }
            Button { objectName: "confirmUnsubscribeMarketing"; text: email.service && email.service.mutating ? "Saving…" : "Unsubscribe"; enabled: email.ready && !email.service.mutating; DialogButtonBox.buttonRole: DialogButtonBox.ActionRole; onClicked: { if (email.service.execute("update-email-preferences", "", {tips:false,recommendations_and_events:false,updates_and_announcements:false}) && !email.service.mutating) unsubscribeDialog.close() } }
        }
    }
    Connections { target: email.service; ignoreUnknownSignals: true; function onActionCompleted(action) { if (action === "update-email-preferences" && unsubscribeDialog.visible) unsubscribeDialog.close() } }
}
