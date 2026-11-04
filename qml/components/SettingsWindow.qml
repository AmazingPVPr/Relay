import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme
import "ApplicationLifecycle.js" as ApplicationLifecycle

ApplicationWindow {
    id: settingsWindow
    objectName: "settingsWindow"
    width: 1160; height: 820; minimumWidth: 850; minimumHeight: 600
    title: "Settings · Relay"; color: (preferences.highContrast ? "#101014" : Theme.canvas)
    font.family: "Inter"; font.pixelSize: Math.round(14 * preferences.fontScale)
    palette.window: (preferences.highContrast ? "#101014" : Theme.canvas); palette.windowText: (preferences.highContrast ? "#ffffff" : Theme.text); palette.base: (preferences.highContrast ? "#0b0b0e" : Theme.rail); palette.text: (preferences.highContrast ? "#ffffff" : Theme.text)
    palette.button: (preferences.highContrast ? "#85858f" : Theme.border); palette.buttonText: (preferences.highContrast ? "#ffffff" : Theme.text); palette.highlight: Theme.avatar; palette.highlightedText: (preferences.highContrast ? "#ffffff" : Theme.text); palette.placeholderText: (preferences.highContrast ? "#c7c7cf" : Theme.muted)
    property string currentPage: "account"
    property string currentSection: ""
    property string searchText: ""
    readonly property bool accountSaving: typeof accountSettings !== "undefined" && !!accountSettings.mutating
    readonly property var navigation: [
        {title:"",pages:[
            {key:"account",label:"Account",icon:"members",sections:[{key:"account-info",label:"Account Info"},{key:"security",label:"Password & Security"},{key:"account-status",label:"Account Status"},{key:"family",label:"Family Center"}]},
            {key:"privacy",label:"Data & Privacy",icon:"lock",sections:[]},
            {key:"messaging",label:"Messaging Permissions",icon:"chat",sections:[{key:"content-filters",label:"Content Filters"},{key:"spam-filters",label:"Spam Filters"},{key:"direct-messages",label:"Direct Messages"},{key:"friend-requests",label:"Friend Requests"},{key:"connected-games",label:"Connected Games"},{key:"ignore-block",label:"Ignore & Block"}]},
            {key:"notifications",label:"Notifications",icon:"soundboard",sections:[{key:"overview",label:"Overview"},{key:"sounds",label:"Sounds"},{key:"email",label:"Email"},{key:"advanced",label:"Advanced"}]}]},
        {title:"Relay Settings",pages:[{key:"addons",label:"Add-ons",icon:"settings",sections:[]},{key:"logs",label:"Logs",icon:"settings",sections:[]},{key:"updates",label:"Updates",icon:"refresh",sections:[]},{key:"keybinds",label:"Keybinds",icon:"soundboard",sections:[]}]},
        {title:"Billing",pages:[{key:"nitro",label:"Nitro",icon:"emoji",sections:[]},{key:"boosts",label:"Server Boost",icon:"voice",sections:[]},{key:"subscriptions",label:"Subscriptions",icon:"calendar",sections:[]},{key:"gifts",label:"Gift Inventory",icon:"emoji",sections:[]},{key:"billing",label:"Billing",icon:"settings",sections:[]}]},
        {title:"Experience",pages:[
            {key:"voice",label:"Voice & Video",icon:"mic",sections:[{key:"input",label:"Microphone"},{key:"output",label:"Output"},{key:"processing",label:"Voice Processing"},{key:"camera",label:"Camera"},{key:"streaming",label:"Streaming"},{key:"sounds",label:"Sounds"},{key:"soundboard",label:"Soundboard"},{key:"advanced",label:"Advanced"}]},
            {key:"appearance",label:"Appearance",icon:"emoji",sections:[{key:"theme",label:"Theme"},{key:"layout",label:"Layout"}]},
            {key:"accessibility",label:"Accessibility",icon:"members",sections:[{key:"text-readability",label:"Text & Readability"},{key:"visual-density",label:"Visual Density"},{key:"color-contrast",label:"Color & Contrast"},{key:"reduced-motion",label:"Reduced Motion"},{key:"audio-screen-reader",label:"Audio & Screen Reader"}]},
            {key:"system",label:"System",icon:"desktop",sections:[{key:"custom",label:"Custom Keybinds"},{key:"default",label:"Default Keybinds"},{key:"performance",label:"Performance"}]},
            {key:"language",label:"Language & Time",icon:"translate",sections:[{key:"language",label:"Language"},{key:"time",label:"Time"}]}]},
        {title:"Games & Apps",pages:[{key:"activity",label:"Activity Privacy",icon:"voice",sections:[]},{key:"apps",label:"Connected Apps",icon:"link",sections:[]},{key:"connections",label:"Connections",icon:"web",sections:[]}]},
        {title:"",pages:[{key:"developer",label:"Developer",icon:"settings",sections:[]}]}]
    signal openAddon(string id)
    signal signOutRequested()
    function pageInfo(key) {
        for (const group of navigation) for (const entry of group.pages) if (entry.key === key) return entry
        return ({key:key,label:({family:"Family Center",sessions:"Logged-in Devices"})[key] || "Settings",sections:[]})
    }
    function matches(entry) {
        const term = searchText.trim().toLowerCase()
        return !term || entry.label.toLowerCase().indexOf(term) >= 0 || entry.sections.some(section => section.label.toLowerCase().indexOf(term) >= 0)
    }
    function visiblePages(group) { return group.pages.filter(entry => matches(entry)) }
    function visibleSections(entry) {
        const term = searchText.trim().toLowerCase()
        return !term ? currentPage === entry.key ? entry.sections : [] : entry.sections.filter(section => section.label.toLowerCase().indexOf(term) >= 0)
    }
    function hasSearchResult() { return navigation.some(group => visiblePages(group).length > 0) }
    function navigate(page, section) {
        if (accountSaving) return
        const aliases = {audio:"voice",keybindings:"keybinds",permissions:"messaging"}
        currentSection = section || ""; currentPage = aliases[page] || page || "account"
        Qt.callLater(applyNavigation)
    }
    function openPage(page, section) { if (ApplicationLifecycle.closing()) return; navigate(page, section); show(); raise(); requestActivate() }
    function findSection(item, key) {
        if (!item) return null
        if (item.objectName === "settingsSection_" + key) return item
        for (const child of item.children || []) { const result = findSection(child, key); if (result) return result }
        return null
    }
    function applyNavigation() {
        const item = pageLoader.item
        if (!item) return
        if (item.settingsPage !== undefined) item.settingsPage = currentPage
        if (!currentSection) return
        if (typeof item.jumpToSection === "function") { item.jumpToSection(currentSection); return }
        const target = findSection(item, currentSection), view = item.contentItem
        if (target && view && view.contentY !== undefined) {
            if (item.wheelBooster) item.wheelBooster.cancel()
            const y = target.mapToItem(view.contentItem, 0, 0).y - 10
            view.contentY = Math.max(view.originY - view.topMargin, Math.min(Math.max(0, view.contentHeight - view.height), y))
        }
    }
    onCurrentPageChanged: { voice.stopMicTest(); Qt.callLater(applyNavigation) }
    onVisibleChanged: if (!visible) { voice.stopMicTest(); if (typeof accountSettings !== "undefined") accountSettings.close() }
    onClosing: event => { voice.stopMicTest(); if (accountSaving && !ApplicationLifecycle.closing()) event.accepted = false }
    Shortcut { sequence: "Escape"; enabled: !settingsWindow.accountSaving && !accountChallenge.visible && !(pageLoader.item && pageLoader.item.modalOpen === true); context: Qt.WindowShortcut; onActivated: settingsWindow.close() }
    Shortcut { sequence: "Ctrl+F"; enabled: !settingsWindow.accountSaving; context: Qt.WindowShortcut; onActivated: settingsSearch.forceActiveFocus() }
    AccountChallengeDialog { id: accountChallenge; service: typeof accountSettings !== "undefined" ? accountSettings : null; windowActive: settingsWindow.visible }
    RowLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.preferredWidth: Math.round(240 * Math.min(preferences.fontScale, 1.12)); Layout.fillHeight: true; color: (preferences.highContrast ? "#151519" : Theme.sidebar)
            ColumnLayout {
                enabled: !settingsWindow.accountSaving
                anchors.fill: parent; anchors.margins: 14; spacing: 14
                Button {
                    Layout.fillWidth: true; Layout.topMargin: 4; implicitHeight: 58; padding: 7; hoverEnabled: true
                    background: Rectangle { color: parent.hovered ? Theme.hover : "transparent"; radius: 6 }
                    contentItem: RowLayout {
                        spacing: 11
                        Rectangle {
                            Layout.preferredWidth: 42; Layout.preferredHeight: 42; radius: 21; color: Theme.avatar
                            Text { anchors.centerIn: parent; visible: !accountAvatar.ready; text: (discord.currentUserName || "?").substring(0,1); color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: 18 }
                            RoundedImage { id: accountAvatar; anchors.fill: parent; source: discord.currentUserAvatarUrl || ""; radius: 21 }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 3
                            Text { Layout.fillWidth: true; text: discord.currentUserName || "Your account"; textFormat: Text.PlainText; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(13 * preferences.fontScale); font.weight: Font.DemiBold; elide: Text.ElideRight }
                            Text { Layout.fillWidth: true; text: "Account settings"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(11 * preferences.fontScale); elide: Text.ElideRight }
                        }
                    }
                    onClicked: settingsWindow.navigate("account", "account-info")
                    Accessible.name: "Your account settings"
                }
                TextField {
                    id: settingsSearch; objectName: "settingsSearch"
                    Layout.fillWidth: true; implicitHeight: 38; leftPadding: 34; rightPadding: 10; placeholderText: "Search settings"; selectByMouse: true
                    background: Rectangle { radius: 6; color: Theme.rail; border.width: settingsSearch.activeFocus ? 2 : 1; border.color: settingsSearch.activeFocus ? Theme.accent : Theme.border }
                    onTextChanged: settingsWindow.searchText = text
                    AppIcon { x: 10; anchors.verticalCenter: parent.verticalCenter; width: 16; height: 16; name: "search"; color: (preferences.highContrast ? "#e4e4e8" : Theme.secondary) }
                    onAccepted: {
                        for (const group of settingsWindow.navigation) {
                            const results = settingsWindow.visiblePages(group)
                            if (!results.length) continue
                            const sections = settingsWindow.visibleSections(results[0])
                            settingsWindow.navigate(results[0].key, sections.length ? sections[0].key : ""); return
                        }
                    }
                    Accessible.name: "Search settings"
                }
                HardScrollView {
                    id: navigationScroll
                    Layout.fillWidth: true; Layout.fillHeight: true; rightPadding: 0
                    ColumnLayout {
                        width: navigationScroll.availableWidth; spacing: 10
                        Repeater {
                            model: settingsWindow.navigation
                            delegate: ColumnLayout {
                                required property var modelData
                                required property int index
                                Layout.fillWidth: true; spacing: 5; visible: settingsWindow.visiblePages(modelData).length > 0
                                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: (preferences.highContrast ? "#85858f" : Theme.border); visible: index > 0 }
                                Text { Layout.fillWidth: true; Layout.leftMargin: 8; Layout.topMargin: 9; Layout.bottomMargin: 5; visible: modelData.title.length > 0; text: modelData.title; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(11 * preferences.fontScale) }
                                Repeater {
                                    model: settingsWindow.visiblePages(modelData)
                                    delegate: ColumnLayout {
                                        id: navPage
                                        required property var modelData
                                        Layout.fillWidth: true; spacing: 0
                                        Button {
                                            id: navButton; objectName: "settingsNav_" + navPage.modelData.key
                                            Layout.fillWidth: true; implicitHeight: Math.max(Math.round(35 * Math.min(preferences.fontScale, 1.14)), contentItem.implicitHeight + 10); leftPadding: 8; rightPadding: 7; hoverEnabled: true
                                            background: Rectangle { radius: 6; color: settingsWindow.currentPage === navPage.modelData.key ? Theme.selected : navButton.hovered ? Theme.hover : "transparent"; border.width: navButton.activeFocus ? 1 : 0; border.color: Theme.focus }
                                            contentItem: RowLayout {
                                                spacing: 10
                                                AppIcon { name: navPage.modelData.icon; color: settingsWindow.currentPage === navPage.modelData.key ? (preferences.highContrast ? "#ffffff" : Theme.text) : (preferences.highContrast ? "#c7c7cf" : Theme.muted); Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
                                                Text { Layout.fillWidth: true; text: navPage.modelData.label; color: settingsWindow.currentPage === navPage.modelData.key ? (preferences.highContrast ? "#ffffff" : Theme.text) : (preferences.highContrast ? "#e4e4e8" : Theme.secondary); font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
                                            }
                                            onClicked: settingsWindow.navigate(navPage.modelData.key, "")
                                            Accessible.name: navPage.modelData.label + " settings"
                                        }
                                        Repeater {
                                            model: settingsWindow.visibleSections(navPage.modelData)
                                            delegate: Button {
                                                id: sectionButton
                                                required property var modelData
                                                Layout.fillWidth: true; implicitHeight: Math.round(31 * Math.min(preferences.fontScale, 1.14)); leftPadding: 29; rightPadding: 7; hoverEnabled: true
                                                background: Item {
                                                    Rectangle { x: 18; width: settingsWindow.currentSection === sectionButton.modelData.key ? 2 : 1; height: parent.height; color: settingsWindow.currentSection === sectionButton.modelData.key ? (preferences.highContrast ? "#ffffff" : Theme.text) : (preferences.highContrast ? "#85858f" : Theme.border) }
                                                    Rectangle { anchors.fill: parent; anchors.leftMargin: 23; radius: 4; color: sectionButton.hovered ? Theme.hover : "transparent" }
                                                }
                                                contentItem: Text { text: sectionButton.modelData.label; color: settingsWindow.currentSection === sectionButton.modelData.key ? (preferences.highContrast ? "#ffffff" : Theme.text) : (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(12 * preferences.fontScale); verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
                                                onClicked: settingsWindow.navigate(navPage.modelData.key, modelData.key)
                                                Accessible.name: modelData.label
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        Text { Layout.fillWidth: true; visible: !settingsWindow.hasSearchResult(); text: "No matching settings"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
                        ActionButton { Layout.fillWidth: true; text: "Log Out"; iconName: "logout"; destructive: true; quiet: true; enabled: !accountSettings.mutating && !(typeof accounts !== "undefined" && accounts.sessionChangeBlocked); onClicked: { settingsWindow.close(); settingsWindow.signOutRequested() } }
                        Text { Layout.leftMargin: 8; Layout.topMargin: 12; Layout.bottomMargin: 14; text: "Relay"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(10 * preferences.fontScale) }
                    }
                }
            }
        }
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            Rectangle {
                id: pageHeader
                anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right; height: 48; color: (preferences.highContrast ? "#101014" : Theme.canvas)
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: (preferences.highContrast ? "#85858f" : Theme.border); opacity: .5 }
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 18; anchors.rightMargin: 12
                    Text { Layout.fillWidth: true; text: settingsWindow.pageInfo(settingsWindow.currentPage).label; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(13 * preferences.fontScale); elide: Text.ElideRight }
                    BusyIndicator { visible: settingsWindow.accountSaving; running: visible; Layout.preferredWidth: 20; Layout.preferredHeight: 20 }
                    Text { visible: settingsWindow.accountSaving; text: "Saving account changes…"; color: Theme.secondary; font.pixelSize: Math.round(11 * preferences.fontScale) }
                    ActionButton { iconName: "close"; hint: "Close settings"; enabled: !settingsWindow.accountSaving; quiet: true; implicitWidth: 32; implicitHeight: 32; onClicked: settingsWindow.close() }
                }
            }
            Loader {
                id: pageLoader; objectName: "settingsPageLoader"
                anchors.top: pageHeader.bottom; anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; anchors.topMargin: 40; anchors.bottomMargin: 28
                width: Math.min(700, parent.width - 56)
                source: ({account:"AccountSettings.qml",messaging:"MessagingPermissionsSettings.qml",appearance:"AppearanceSettings.qml",voice:"AudioSettings.qml",keybinds:"KeybindingsSettings.qml",notifications:"NotificationsSettings.qml",addons:"AddonsSettings.qml",accessibility:"AccessibilitySettings.qml",language:"LanguageSettings.qml",system:"SystemSettings.qml",logs:"LogsSettings.qml",updates:"UpdatesSettings.qml"})[settingsWindow.currentPage] || "AccountDataSettings.qml"
                active: settingsWindow.visible
                onLoaded: Qt.callLater(settingsWindow.applyNavigation)
            }
            Connections {
                target: pageLoader.item; ignoreUnknownSignals: true
                function onNavigate(page, section) { settingsWindow.navigate(page, section) }
                function onOpenAddon(id) { settingsWindow.close(); settingsWindow.openAddon(id) }
                function onSignOutRequested() { settingsWindow.close(); settingsWindow.signOutRequested() }
            }
        }
    }
}
