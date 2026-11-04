import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
    id: page
    objectName: "accountDataSettings"
    property string settingsPage: ""
    readonly property var service: typeof accountSettings !== "undefined" ? accountSettings : null
    readonly property bool pageMatches: !!service && service.page === (settingsPage === "activity" ? "privacy" : settingsPage)
    readonly property var details: pageMatches ? service.details : ({})
    readonly property var capabilities: pageMatches ? service.capabilities : ({})
    readonly property var preferenceValues: details.preferences || ({})
    readonly property var pageMetadata: ({
        nitro:{title:"Nitro",description:"Your Nitro membership and subscriptions."},boosts:{title:"Server Boost",description:"Your available boosts and the servers using them."},
        subscriptions:{title:"Subscriptions",description:"Your active and past Discord subscriptions."},gifts:{title:"Gift Inventory",description:"Gifts available on your account."},
        billing:{title:"Billing",description:"Your saved payment methods and payment activity."},apps:{title:"Connected Apps",description:"Apps you have authorized to access your Discord account."},
        connections:{title:"Connections",description:"Accounts and services connected to Discord."},family:{title:"Family Center",description:"Family connections and information shared with you."},
        sessions:{title:"Logged-in Devices",description:"Devices and sessions signed in to your Discord account."},privacy:{title:"Data & Privacy",description:"Privacy preferences saved to your Discord account."},
        activity:{title:"Activity Privacy",description:"Choose how friends can join your activities."},developer:{title:"Developer",description:"Developer options available in Relay."}
    })[settingsPage] || ({title:"Account settings",description:""})
    readonly property var preferenceRows: settingsPage === "activity" ? [{key:"activityJoinFriends",title:"Allow friends to join",description:"Let friends join activities that support joining."},{key:"activityJoinVoice",title:"Allow voice participants to join",description:"Let people in your voice channel join supported activities."}]
        : settingsPage === "privacy" ? [{key:"contactSyncEnabled",title:"Contact syncing",description:"Allow Discord to sync your contacts."},{key:"nonSpamRetrainingOptIn",title:"Use messages to improve spam detection",description:"Allow eligible messages to help improve Discord's spam filters."}] : []
    signal navigate(string page, string section)
    contentWidth: availableWidth
    function load() {
        if (!service || !settingsPage) return
        if (settingsPage === "developer") { service.close(); return }
        service.loadPage(settingsPage === "activity" ? "privacy" : settingsPage)
    }
    function updatePreference(key, value) { const changes = ({}); changes[key] = value; service.execute("update-preferences", "", changes) }
    onSettingsPageChanged: load()
    ColumnLayout {
        width: page.availableWidth; spacing: 22
        SettingsSectionTitle { title: page.pageMetadata.title; description: page.pageMetadata.description }
        AccountSettingsStatus { service: page.service; visible: page.settingsPage !== "developer" }
        Repeater {
            model: page.preferenceRows
            delegate: SettingsToggle {
                required property var modelData
                Layout.fillWidth: true; visible: page.preferenceValues[modelData.key] !== undefined
                title: modelData.title; description: modelData.description
                checked: !!page.preferenceValues[modelData.key]
                enabled: !!page.capabilities.preferences && !page.service.mutating
                onToggled: value => page.updatePreference(modelData.key, value)
            }
        }
        Text {
            Layout.fillWidth: true; visible: page.settingsPage === "nitro" && page.details.premiumType !== undefined
            text: ({0:"No Nitro membership",1:"Nitro Classic",2:"Nitro",3:"Nitro Basic"})[Number(page.details.premiumType)] || "Nitro membership"
            color: Theme.text; textFormat: Text.PlainText; font.pixelSize: Math.round(16 * preferences.fontScale); wrapMode: Text.WordWrap
        }
        Repeater {
            model: page.pageMatches && page.settingsPage !== "privacy" && page.settingsPage !== "activity" && page.settingsPage !== "developer" ? page.service.items : []
            delegate: Rectangle {
                required property var modelData
                Layout.fillWidth: true; implicitHeight: recordColumn.implicitHeight + 28; radius: 7; color: Theme.sidebar; border.color: Theme.border
                ColumnLayout {
                    id: recordColumn
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14; spacing: 11
                    RowLayout {
                        Layout.fillWidth: true; spacing: 12
                        RoundedImage { Layout.preferredWidth: 36; Layout.preferredHeight: 36; visible: !!modelData.iconUrl; source: modelData.iconUrl || ""; radius: 18 }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 5
                            Text { Layout.fillWidth: true; text: modelData.title || "Account record"; color: Theme.text; textFormat: Text.PlainText; font.pixelSize: Math.round(14 * preferences.fontScale); font.weight: Font.Medium; wrapMode: Text.WordWrap }
                            Text { Layout.fillWidth: true; text: modelData.subtitle || ""; visible: text.length > 0; color: Theme.muted; textFormat: Text.PlainText; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
                        }
                        Text { text: modelData.status || ""; visible: text.length > 0; Layout.maximumWidth: 140; color: Theme.secondary; textFormat: Text.PlainText; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
                        ActionButton { text: modelData.actionLabel || "Manage"; visible: !!modelData.action; enabled: !!page.service && !page.service.mutating; onClicked: page.service.execute(modelData.action, modelData.id || "", {}) }
                    }
                    Repeater {
                        model: modelData.fields || []
                        delegate: AccountSettingRow { required property var modelData; title: modelData.label || ""; value: modelData.value || "" }
                    }
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: page.settingsPage === "developer" || !!page.service && !page.service.busy && !page.service.error && page.service.items.length === 0
            text: page.details.message || (page.settingsPage === "developer" ? "Developer account controls are not available in Relay." : page.preferenceRows.length ? "Only settings currently supported by Discord's account connection appear here." : "No records are available for this account.")
            textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap
        }
    }
}
