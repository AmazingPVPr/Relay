import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
    id: page
    readonly property bool modalOpen: emailSettings.modalOpen
    signal navigate(string page, string section)
    function jumpToSection(section) {
        if (section === "email") emailSettings.load()
        const target = ({overview: overviewHeading, sounds: soundsHeading, email: emailHeading, advanced: advancedHeading})[section]
        if (!target || !contentItem || contentItem.contentY === undefined) return
        Qt.callLater(() => {
            page.wheelBooster.cancel()
            const view = page.contentItem
            const y = target.mapToItem(view.contentItem, 0, 0).y - 10
            view.contentY = Math.max(view.originY - view.topMargin, Math.min(Math.max(0, view.contentHeight - view.height), y))
        })
    }
    ColumnLayout {
        width: page.availableWidth; spacing: 24
        SettingsHeading { id: overviewHeading; objectName: "settingsSection_overview"; text: "Overview" }
        SettingsToggle { Layout.fillWidth: true; title: "Enable Desktop Notifications"; description: "For individual servers and channels, use their notification settings."; checked: preferences.notificationsEnabled; onToggled: value => preferences.notificationsEnabled=value }
        Text { text: "Notify me when…"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale; font.weight: Font.Medium }
        SettingsToggle { Layout.fillWidth: true; title: "I receive a direct message"; checked: preferences.notifyDirectMessages; onToggled: value => preferences.notifyDirectMessages=value }
        SettingsToggle { Layout.fillWidth: true; title: "Someone mentions me"; checked: preferences.notifyMentions; onToggled: value => preferences.notifyMentions=value }
        SettingsToggle { Layout.fillWidth: true; title: "New messages arrive in server channels"; description: "Includes other server messages Relay receives."; checked: preferences.notifyAllMessages; onToggled: value => preferences.notifyAllMessages=value }
        SettingsDivider {}
        SettingsHeading { id: soundsHeading; objectName: "settingsSection_sounds"; text: "Sounds" }
        SoundSettingsList { Layout.fillWidth: true; cues: [{id:"message",label:"New Message"}] }
        SettingsToggle { Layout.fillWidth: true; title: "Play Notification Sounds"; checked: preferences.notificationSound; onToggled: value => preferences.notificationSound=value }
        SettingsToggle { Layout.fillWidth: true; title: "Disable All Notification Sounds"; description: "Includes Voice & Video sound effects. Individual sound preferences are kept."; checked: !preferences.activitySoundsEnabled; onToggled: value => preferences.activitySoundsEnabled=!value }
        ActionButton { text: "Voice & Video"; iconName: "mic"; quiet: true; onClicked: page.navigate("voice", "sounds") }
        SettingsDivider {}
        SettingsHeading { id: emailHeading; objectName: "settingsSection_email"; text: "Email" }
        EmailNotificationSettings { id: emailSettings; Layout.fillWidth: true }
        SettingsDivider {}
        SettingsHeading { id: advancedHeading; objectName: "settingsSection_advanced"; text: "Advanced" }
        SettingsToggle { Layout.fillWidth: true; title: "Show Message Previews"; description: "Include message text in desktop notifications."; checked: preferences.notificationPreviews; onToggled: value => preferences.notificationPreviews=value }
        SettingsToggle { Layout.fillWidth: true; title: "Notify While Relay Is Focused"; checked: preferences.notifyWhenFocused; onToggled: value => preferences.notifyWhenFocused=value }
        Text { Layout.fillWidth: true; text: "Minimum notification duration"; color: Theme.text; font.pixelSize: 14 * preferences.fontScale }
        RowLayout {
            Layout.fillWidth: true
            AppSlider { Layout.fillWidth: true; from: 3; to: 20; stepSize: 1; value: preferences.notificationDuration; onMoved: preferences.notificationDuration=Math.round(value); Accessible.name: "Minimum notification duration" }
            Text { text: preferences.notificationDuration + "s"; color: Theme.secondary; font.pixelSize: 13 * preferences.fontScale }
        }
        Text { Layout.fillWidth: true; text: "Your desktop controls the final appearance and display time of notifications."; color: Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        ActionButton { text: "Send a test notification"; primary: true; enabled: notifications.available; onClicked: notifications.sendTest() }
        Text { Layout.fillWidth: true; text: notifications.error || notifications.status; textFormat: Text.PlainText; color: notifications.error.length ? Theme.danger : Theme.muted; font.pixelSize: 12 * preferences.fontScale; wrapMode: Text.WordWrap }
        Item { Layout.preferredHeight: 40 }
    }
}
