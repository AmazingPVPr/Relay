import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

HardScrollView {
    id: page
    objectName: "messagingPermissionsSettings"
    readonly property var service: typeof accountSettings !== "undefined" ? accountSettings : null
    readonly property var values: service && service.details.preferences ? service.details.preferences : ({})
    readonly property bool canEdit: !!service && !!service.capabilities.preferences && !service.busy && !service.mutating
    property bool showAllBlocked: false
    property bool showAllIgnored: false
    signal navigate(string page, string section)
    contentWidth: availableWidth
    function save(key, value) { const changes = ({}); changes[key] = value; service.execute("update-preferences", "", changes) }
    function accounts(action) { return service ? service.items.filter(item => item.action === action) : [] }
    Component.onCompleted: if (service) service.loadPage("messaging")
    ColumnLayout {
        width: page.availableWidth; spacing: 22
        SettingsSectionTitle { section: "content-filters"; title: "Content Filters"; description: "Choose how sensitive media is handled in your conversations." }
        AccountSettingsStatus { service: page.service }
        Text { Layout.fillWidth: true; text: "Discord's sensitive-media filters are not available through Relay's account connection yet."; color: Theme.muted; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
        ActionButton { text: "Related settings: Appearance"; quiet: true; onClicked: page.navigate("appearance", "layout") }
        SettingsSectionTitle { section: "spam-filters"; title: "Spam Filters"; divider: true; description: "Discord manages suspected spam and your Spam inbox." }
        Text { Layout.fillWidth: true; text: "Spam filter choices are not available through Relay's account connection yet."; color: Theme.muted; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
        SettingsSectionTitle { section: "direct-messages"; title: "Direct Message (DM) Permissions"; divider: true; description: "Defaults for messages from other server members. Individual servers can have their own overrides." }
        SettingsToggle {
            Layout.fillWidth: true; visible: page.values.defaultDmsAllowed !== undefined
            title: "Direct messages"; description: "Allow DMs from other server members by default."
            checked: !!page.values.defaultDmsAllowed; enabled: page.canEdit
            onToggled: value => page.save("defaultDmsAllowed", value)
        }
        SettingsToggle {
            Layout.fillWidth: true; visible: page.values.messageRequestsAllowed !== undefined
            title: "Message requests"; description: "Filter messages from server members you may not know."
            checked: !!page.values.messageRequestsAllowed; enabled: page.canEdit
            onToggled: value => page.save("messageRequestsAllowed", value)
        }
        SettingsSectionTitle { section: "friend-requests"; title: "Friend Request Permissions"; divider: true; description: "Choose who can send you friend requests." }
        Repeater {
            model: [{key:"friendRequestsEveryone",label:"Everyone"},{key:"friendRequestsMutualFriends",label:"Friends of friends"},{key:"friendRequestsMutualServers",label:"Server members"}]
            delegate: SettingsToggle {
                required property var modelData
                Layout.fillWidth: true; visible: page.values[modelData.key] !== undefined
                title: modelData.label; checked: !!page.values[modelData.key]; enabled: page.canEdit
                onToggled: value => page.save(modelData.key, value)
            }
        }
        SettingsSectionTitle { section: "connected-games"; title: "Messaging in Connected Games"; divider: true; description: "Manage the apps and games authorized to use your Discord account." }
        ActionButton { text: "View connected apps"; iconName: "link"; onClicked: page.navigate("apps", "") }
        SettingsSectionTitle { section: "ignore-block"; title: "Ignore & Block"; divider: true; description: "Manage people whose messages you have blocked or ignored." }
        Repeater {
            model: [{action:"unblock-user",title:"Blocked accounts",expand:"blocked"},{action:"unignore-user",title:"Ignored accounts",expand:"ignored"}]
            delegate: Rectangle {
                id: accountGroup
                required property var modelData
                readonly property var records: page.accounts(modelData.action)
                readonly property bool expanded: modelData.expand === "blocked" ? page.showAllBlocked : page.showAllIgnored
                Layout.fillWidth: true; implicitHeight: groupContents.implicitHeight + 28; radius: 7; color: Theme.sidebar; border.color: Theme.border
                ColumnLayout {
                    id: groupContents
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14; spacing: 16
                    Text { Layout.fillWidth: true; text: accountGroup.modelData.title + " · " + accountGroup.records.length; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); font.weight: Font.Medium; wrapMode: Text.WordWrap }
                    Repeater {
                        model: accountGroup.expanded ? accountGroup.records : accountGroup.records.slice(0, 5)
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true; spacing: 12
                            Rectangle {
                                Layout.preferredWidth: 34; Layout.preferredHeight: 34; radius: 17; color: Theme.avatar
                                Text { anchors.centerIn: parent; text: (modelData.title || "?").substring(0,1); color: Theme.text; visible: !personImage.ready; font.pixelSize: 14 }
                                RoundedImage { id: personImage; anchors.fill: parent; source: modelData.iconUrl || ""; radius: 17 }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 3
                                Text { Layout.fillWidth: true; text: modelData.title || "Discord user"; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale); textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere }
                                Text { Layout.fillWidth: true; visible: text.length > 0; text: modelData.subtitle || ""; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); textFormat: Text.PlainText; wrapMode: Text.WrapAnywhere }
                            }
                            ActionButton { objectName: "messagingAction_" + modelData.action + "_" + modelData.id; text: modelData.actionLabel || "Remove"; enabled: !!page.service && !page.service.mutating; onClicked: page.service.execute(modelData.action, modelData.id, {}) }
                        }
                    }
                    Text { Layout.fillWidth: true; visible: accountGroup.records.length === 0 && !!page.service && !page.service.busy && !page.service.error; text: "No " + accountGroup.modelData.title.toLowerCase() + "."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale) }
                    ActionButton { Layout.alignment: Qt.AlignHCenter; visible: !accountGroup.expanded && accountGroup.records.length > 5; text: "Show " + (accountGroup.records.length - 5) + " more"; quiet: true; onClicked: { if (accountGroup.modelData.expand === "blocked") page.showAllBlocked = true; else page.showAllIgnored = true } }
                }
            }
        }
    }
}
