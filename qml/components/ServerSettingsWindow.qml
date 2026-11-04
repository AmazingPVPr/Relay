import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "Theme.js" as Theme
import "ApplicationLifecycle.js" as ApplicationLifecycle

ApplicationWindow {
    id: window
    width: 1060; height: 790
    minimumWidth: 820; minimumHeight: 580
    title: (guildManagement.guild.name || "Server") + " · Server settings"
    color: Theme.canvas
    font.family: "Inter"; font.pixelSize: Math.round(14 * preferences.fontScale)
    palette.window: Theme.canvas; palette.windowText: Theme.text
    palette.base: Theme.rail; palette.text: Theme.text
    palette.button: Theme.border; palette.buttonText: Theme.text
    palette.highlight: Theme.avatar; palette.highlightedText: Theme.text
    palette.placeholderText: Theme.muted
    property string serverId: ""
    property string currentPage: "overview"
    property string localError: ""
    readonly property bool correctServer: serverId === guildManagement.guildId
    readonly property var caps: correctServer ? guildManagement.capabilities : ({})
    readonly property var server: correctServer ? guildManagement.guild : ({})
    signal userRequested(string userId, var fallback)
    signal contextRequested(string userId, var fallback)
    signal mentionRequested(string userId)
    signal watchRequested(string key, string title)
    onUserRequested: (id, fallback) => { profileLoader.active = true; profileLoader.item.showUser(id,Object.assign({},fallback,{guildId:window.serverId})) }
    onContextRequested: (id, fallback) => { personMenuLoader.active = true; personMenuLoader.item.openForUser(id,fallback,window.serverId) }
    function openGuild(id, page) {
        if (ApplicationLifecycle.closing()) return
        if (serverId !== id) { confirmation.close(); personConfirmation.close(); imagePicker.close(); roleEditor.close(); eventEditor.close(); if(personMenuLoader.item) personMenuLoader.item.close(); if(profileLoader.item) profileLoader.item.close() }
        serverId = id; currentPage = page || "overview"; localError = ""
        guildManagement.selectGuild(id); guildManagement.loadSection(currentPage)
        pageLoader.active = false; pageLoader.active = true
        show(); raise(); requestActivate()
    }
    function changePage(page) { currentPage = page; localError = ""; guildManagement.loadSection(page) }
    function execute(action, target, values) {
        if (!correctServer) { localError = "Select this server again before making changes."; return false }
        return guildManagement.execute(action, target || "", values || {})
    }
    function confirm(action, target, values, title, message) {
        confirmation.serverId = serverId
        confirmation.action = action; confirmation.targetId = target; confirmation.values = values
        confirmation.title = title; confirmation.message = message; confirmation.open()
    }
    function chooseImage(kind) { imagePicker.serverId = serverId; imagePicker.kind = kind; imagePicker.open() }
    function dateLabel(value) { const d = new Date(value); return isNaN(d.getTime()) ? "" : d.toLocaleString(Qt.locale(), Locale.ShortFormat) }
    function itemName(row) { return row.displayName || row.nick || row.name || row.username || row.user?.global_name || row.user?.username || row.userId || row.id || "Unknown" }
    Shortcut { sequence: "Escape"; context: Qt.WindowShortcut; onActivated: window.close() }
    RowLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.preferredWidth: 220; Layout.fillHeight: true; color: Theme.sidebar
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 18; spacing: 6
                Text { Layout.fillWidth: true; Layout.topMargin: 18; Layout.bottomMargin: 20; text: window.server.name || "Server settings"; color: Theme.text; font.bold: true; wrapMode: Text.Wrap; textFormat: Text.PlainText }
                Repeater {
                    model: [{key:"overview",label:"Server Profile"},{key:"boosts",label:"Boosts"},{key:"events",label:"Events"},{key:"roles",label:"Roles"},{key:"members",label:"Members"},{key:"invites",label:"Invites",cap:"manageInvites"},{key:"bans",label:"Bans",cap:"banMembers"},{key:"audit",label:"Audit Log",cap:"viewAuditLog"}]
                    delegate: Button {
                        id: nav
                        required property var modelData
                        visible: !modelData.cap || !!window.caps[modelData.cap]
                        Layout.fillWidth: true; implicitHeight: 40; hoverEnabled: true
                        onClicked: window.changePage(modelData.key)
                        background: Rectangle { radius: 4; color: window.currentPage === nav.modelData.key ? Theme.selected : nav.hovered ? Theme.hover : "transparent" }
                        contentItem: Text { text: nav.modelData.label; color: window.currentPage === nav.modelData.key ? Theme.text : Theme.secondary; font: nav.font; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; leftPadding: 10 }
                    }
                }
                Item { Layout.fillHeight: true }
                Text { Layout.fillWidth: true; text: "Options follow your server permissions."; color: Theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap }
            }
        }
        Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: Theme.border }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 28; spacing: 16
            RowLayout {
                Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: ({overview:"Server Profile",boosts:"Server Boosts",roles:"Roles",members:"Members",invites:"Invites",bans:"Bans",audit:"Audit Log",events:"Events"})[window.currentPage] || "Server"; color: Theme.text; font.pixelSize: Math.round(24 * preferences.fontScale); font.bold: true }
                BusyIndicator { running: guildManagement.busy; visible: running; Layout.preferredWidth: 24; Layout.preferredHeight: 24 }
                ActionButton { iconName: "refresh"; hint: "Refresh"; quiet: true; enabled: window.correctServer && !guildManagement.busy; onClicked: guildManagement.refresh() }
                ActionButton { iconName: "close"; hint: "Close server settings"; quiet: true; onClicked: window.close() }
            }
            Label { Layout.fillWidth: true; visible: (window.localError || guildManagement.error).length > 0; text: window.localError || guildManagement.error; color: Theme.danger; wrapMode: Text.Wrap; textFormat: Text.PlainText }
            Label { Layout.fillWidth: true; visible: guildManagement.status.length > 0; text: guildManagement.status; color: Theme.muted; wrapMode: Text.Wrap; textFormat: Text.PlainText }
            ActionButton { visible: !window.correctServer; text: "Return to this server"; onClicked: window.openGuild(window.serverId, window.currentPage) }
            Loader {
                id: pageLoader
                Layout.fillWidth: true; Layout.fillHeight: true
                active: window.visible
                enabled: window.correctServer
                sourceComponent: window.currentPage === "overview" ? profilePage : window.currentPage === "boosts" ? boostsPage : window.currentPage === "roles" ? rolesPage : window.currentPage === "events" ? eventsPage : recordsPage
            }
        }
    }
    Component {
        id: profilePage
        HardScrollView {
            id: profileScroll
            ColumnLayout {
                width: profileScroll.availableWidth; spacing: 18
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: 150; color: Theme.surface; radius: 8; clip: true
                    Image { anchors.fill: parent; source: window.server.bannerUrl || ""; fillMode: Image.PreserveAspectCrop; asynchronous: true; sourceSize.width: 2048; opacity: .5 }
                    RowLayout {
                        anchors.fill: parent; anchors.margins: 24; spacing: 20
                        RoundedImage { source: window.server.iconUrl || ""; radius: 20; Layout.preferredWidth: 88; Layout.preferredHeight: 88; decodeSize: 512 }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Text { Layout.fillWidth: true; text: window.server.name || "Server"; color: Theme.text; font.pixelSize: 22; font.bold: true; textFormat: Text.PlainText; wrapMode: Text.Wrap }
                            Text { text: window.server.memberCount === undefined ? "" : window.server.memberCount + " members"; color: Theme.secondary }
                        }
                    }
                }
                Label { text: "Name"; color: Theme.text }
                TextField { id: nameField; Layout.fillWidth: true; text: window.server.name || ""; maximumLength: 100; selectByMouse: true; readOnly: !window.caps.manageGuild }
                Label { text: "Description"; color: Theme.text }
                TextArea { id: descriptionField; Layout.fillWidth: true; Layout.preferredHeight: 110; text: window.server.description || ""; selectByMouse: true; wrapMode: TextEdit.Wrap; readOnly: !window.caps.manageGuild; placeholderText: "Tell people about your server" }
                Label { Layout.alignment: Qt.AlignRight; text: descriptionField.text.length + "/120"; color: descriptionField.text.length > 120 ? Theme.danger : Theme.muted; font.pixelSize: 12 }
                RowLayout {
                    visible: !!window.caps.manageGuild; Layout.fillWidth: true
                    ActionButton { text: "Save changes"; primary: true; enabled: !guildManagement.mutating && nameField.text.trim().length > 0 && descriptionField.text.length <= 120; onClicked: window.execute("editGuild", window.serverId, {name:nameField.text.trim(),description:descriptionField.text}) }
                    ActionButton { text: "Reset"; quiet: true; onClicked: { nameField.text = window.server.name || ""; descriptionField.text = window.server.description || "" } }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
                Label { text: "Server images"; color: Theme.text; font.bold: true }
                Flow {
                    Layout.fillWidth: true; spacing: 8; visible: !!window.caps.manageGuild
                    ActionButton { text: "Change icon"; enabled: !guildManagement.mutating; onClicked: window.chooseImage("icon") }
                    ActionButton { text: "Remove icon"; destructive: true; enabled: !guildManagement.mutating && !!window.server.iconUrl; onClicked: window.confirm("editGuild",window.serverId,{iconPath:""},"Remove server icon?","The server will use its default icon.") }
                    ActionButton { text: "Change banner"; visible: (window.server.features || []).indexOf("BANNER") >= 0; enabled: !guildManagement.mutating; onClicked: window.chooseImage("banner") }
                    ActionButton { text: "Remove banner"; visible: !!window.server.bannerUrl; destructive: true; enabled: !guildManagement.mutating; onClicked: window.confirm("editGuild",window.serverId,{bannerPath:""},"Remove server banner?","The current banner image will be removed.") }
                }
                Label { Layout.fillWidth: true; text: window.caps.manageGuild ? "Choose a PNG, JPEG, WebP, or GIF image. A server icon of at least 512 × 512 looks best." : "You need Manage Server permission to edit this profile."; color: Theme.muted; wrapMode: Text.Wrap; font.pixelSize: 12 }
            }
        }
    }
    Component {
        id: boostsPage
        HardScrollView {
            id: boostsScroll
            ColumnLayout {
                width: boostsScroll.availableWidth; spacing: 20
                Label { text: "Level " + (window.server.premiumTier || 0); color: Theme.text; font.pixelSize: 32; font.bold: true }
                Label { text: window.server.premiumSubscriptionCount === undefined ? "Boost count unavailable" : window.server.premiumSubscriptionCount + " server boosts"; color: Theme.secondary; font.pixelSize: 18 }
                Label { text: "Available server features"; color: Theme.text; font.bold: true }
                Flow {
                    Layout.fillWidth: true; spacing: 8
                    Repeater { model: window.server.features || []; delegate: Label { required property string modelData; text: modelData.replace(/_/g," ").toLowerCase(); padding: 9; color: Theme.secondary; background: Rectangle { color: Theme.surface; radius: 5 } } }
                }
                Label { Layout.fillWidth: true; text: "Boost purchases and subscription changes are managed in Discord."; color: Theme.muted; wrapMode: Text.Wrap }
            }
        }
    }
    Component {
        id: rolesPage
        ColumnLayout {
            spacing: 12
            ActionButton { text: "Create role"; primary: true; visible: !!window.caps.manageRoles; enabled: !guildManagement.mutating; onClicked: roleEditor.showRole(window.serverId, {}) }
            HardScrollView {
                id: rolesScroll; Layout.fillWidth: true; Layout.fillHeight: true
                ColumnLayout {
                    width: rolesScroll.availableWidth; spacing: 8
                    Repeater {
                        model: guildManagement.roles
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true; implicitHeight: roleRow.implicitHeight + 24; radius: 6; color: Theme.surface
                            RowLayout {
                                id: roleRow; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12; y: 12; spacing: 12
                                Rectangle { width: 12; height: 12; radius: 6; color: modelData.color || Theme.muted }
                                Label { Layout.fillWidth: true; text: modelData.name; color: Theme.text; textFormat: Text.PlainText; elide: Text.ElideRight }
                                Label { visible: !!modelData.managed; text: "Managed"; color: Theme.muted; font.pixelSize: 12 }
                                ActionButton { text: modelData.editable ? "Edit" : "View"; quiet: true; onClicked: roleEditor.showRole(window.serverId, modelData) }
                                ActionButton { text: "Delete"; destructive: true; visible: !!modelData.editable && modelData.id !== window.serverId; enabled: !guildManagement.mutating; onClicked: window.confirm("deleteRole", modelData.id, {}, "Delete role?", "Delete “" + modelData.name + "” and remove it from all members? This cannot be undone.") }
                            }
                        }
                    }
                }
            }
        }
    }
    Component {
        id: eventsPage
        ColumnLayout {
            spacing: 12
            ActionButton { text: "Create event"; primary: true; visible: !!window.caps.manageEvents || !!window.caps.createEvents; enabled: !guildManagement.mutating; onClicked: eventEditor.showEvent(window.serverId,{}) }
            HardScrollView {
                id: eventScroll; Layout.fillWidth: true; Layout.fillHeight: true
                ColumnLayout {
                    width: eventScroll.availableWidth; spacing: 12
                    Label { visible: !guildManagement.busy && guildManagement.items.length === 0; text: "No scheduled events."; color: Theme.muted }
                    Repeater {
                        model: guildManagement.items
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true; implicitHeight: eventBody.implicitHeight + 32; radius: 8; color: Theme.surface
                            ColumnLayout {
                                id: eventBody; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 16; y: 16; spacing: 10
                                Label { Layout.fillWidth: true; text: window.dateLabel(modelData.scheduledStartTime); color: Theme.success; textFormat: Text.PlainText }
                                Label { Layout.fillWidth: true; text: modelData.name; color: Theme.text; font.pixelSize: 19; font.bold: true; wrapMode: Text.Wrap; textFormat: Text.PlainText }
                                Label { Layout.fillWidth: true; visible: !!modelData.description; text: modelData.description || ""; color: Theme.secondary; wrapMode: Text.Wrap; textFormat: Text.PlainText }
                                Label { Layout.fillWidth: true; text: modelData.location || (modelData.entityType === 2 ? "Voice channel event" : "Stage channel event"); color: Theme.muted; wrapMode: Text.Wrap; textFormat: Text.PlainText }
                                Label { text: Number(modelData.userCount || 0) + " interested"; color: Theme.muted }
                                Flow {
                                    Layout.fillWidth: true; spacing: 8
                                    ActionButton { text: modelData.currentUserInterested ? "Not interested" : modelData.interestKnown ? "Interested" : "Mark interested"; enabled: !guildManagement.mutating; onClicked: window.execute("rsvpEvent", modelData.id,{enabled:!modelData.currentUserInterested}) }
                                    ActionButton { text: "Copy link"; quiet: true; onClicked: chatText.copy("https://discord.com/events/"+window.serverId+"/"+modelData.id) }
                                    ActionButton { text: "Edit"; quiet: true; visible: !!modelData.canEdit; onClicked: eventEditor.showEvent(window.serverId,modelData) }
                                    ActionButton { text: "Delete"; destructive: true; visible: !!modelData.canDelete; enabled: !guildManagement.mutating; onClicked: window.confirm("deleteEvent",modelData.id,{},"Delete event?","Delete “"+modelData.name+"”? People will no longer be able to join this scheduled event.") }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    Component {
        id: recordsPage
        ColumnLayout {
            TextField { id: filter; Layout.fillWidth: true; placeholderText: "Filter loaded " + (window.currentPage === "audit" ? "entries" : window.currentPage); selectByMouse: true }
            Label { Layout.fillWidth: true; visible: window.currentPage === "members"; text: "Right-click a member for roles and moderation."; color: Theme.muted; font.pixelSize: 12 }
            HardScrollView {
                id: recordsScroll; Layout.fillWidth: true; Layout.fillHeight: true
                ColumnLayout {
                    width: recordsScroll.availableWidth; spacing: 8
                    Label { text: guildManagement.busy ? "Loading…" : "No entries to show."; visible: guildManagement.items.length === 0; color: Theme.muted }
                    Repeater {
                        model: guildManagement.items
                        delegate: Rectangle {
                            id: entry
                            required property var modelData
                            readonly property string name: window.currentPage === "invites" ? (modelData.code || modelData.id || "Invite") : window.currentPage === "audit" ? (modelData.actionName || modelData.action || "Server change") : window.itemName(modelData)
                            visible: !filter.text || (name + " " + (modelData.reason || "")).toLowerCase().indexOf(filter.text.toLowerCase()) >= 0
                            Layout.fillWidth: true; implicitHeight: visible ? entryBody.implicitHeight + 24 : 0; color: Theme.surface; radius: 6
                            ColumnLayout {
                                id: entryBody; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12; y: 12; spacing: 6
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label {
                                        Layout.fillWidth: true; text: entry.name; color: Theme.text; textFormat: Text.PlainText; elide: Text.ElideRight
                                        MouseArea { anchors.fill: parent; enabled: window.currentPage === "members"; acceptedButtons: Qt.LeftButton | Qt.RightButton; cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor; onClicked: mouse => mouse.button === Qt.RightButton ? window.contextRequested(entry.modelData.userId || entry.modelData.id,entry.modelData) : window.userRequested(entry.modelData.userId || entry.modelData.id,entry.modelData) }
                                    }
                                    ActionButton { text: "Options"; quiet: true; visible: window.currentPage === "members"; onClicked: window.contextRequested(entry.modelData.userId || entry.modelData.id, entry.modelData) }
                                    ActionButton { text: "Copy link"; quiet: true; visible: window.currentPage === "invites"; onClicked: chatText.copy("https://discord.gg/" + (entry.modelData.code || entry.modelData.id)) }
                                    ActionButton { text: window.currentPage === "invites" ? "Revoke" : "Unban"; destructive: window.currentPage === "invites"; visible: window.currentPage === "invites" || window.currentPage === "bans"; enabled: !guildManagement.mutating; onClicked: window.confirm(window.currentPage === "invites" ? "deleteInvite" : "unbanMember", entry.modelData.code || entry.modelData.userId || entry.modelData.id, {}, window.currentPage === "invites" ? "Revoke invite?" : "Unban member?", window.currentPage === "invites" ? "This invite link will stop working." : "This person will be allowed to join again using an invite.") }
                                }
                                Label { Layout.fillWidth: true; visible: !!entry.modelData.reason; text: entry.modelData.reason || ""; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.secondary }
                                Label { Layout.fillWidth: true; visible: window.currentPage === "invites"; text: String(entry.modelData.uses || 0) + " uses" + (entry.modelData.maxUses ? " of " + entry.modelData.maxUses : "") + (entry.modelData.channelName ? " · #" + entry.modelData.channelName : ""); color: Theme.muted; textFormat: Text.PlainText; wrapMode: Text.Wrap }
                                Label { Layout.fillWidth: true; visible: window.currentPage === "audit"; text: [entry.modelData.actorName || entry.modelData.userId || "",entry.modelData.targetName || entry.modelData.targetId || "",window.dateLabel(entry.modelData.createdAt)].filter(Boolean).join(" · "); color: Theme.muted; textFormat: Text.PlainText; wrapMode: Text.Wrap }
                                CheckBox { id: expandChanges; text: "Show changes"; visible: window.currentPage === "audit" && (entry.modelData.changes || []).length > 0 }
                                Repeater {
                                    model: expandChanges.checked ? entry.modelData.changes || [] : []
                                    delegate: ColumnLayout {
                                        required property var modelData
                                        Layout.fillWidth: true; spacing: 3
                                        Label { text: modelData.key.replace(/_/g," "); color: Theme.text; font.bold: true; textFormat: Text.PlainText }
                                        Label { Layout.fillWidth: true; text: "Before: " + (modelData.before || "—"); color: Theme.muted; textFormat: Text.PlainText; wrapMode: Text.Wrap }
                                        Label { Layout.fillWidth: true; text: "After: " + (modelData.after || "—"); color: Theme.secondary; textFormat: Text.PlainText; wrapMode: Text.Wrap }
                                    }
                                }
                            }
                        }
                    }
                    ActionButton { text: "Load more"; visible: guildManagement.hasMore; enabled: !guildManagement.busy; onClicked: guildManagement.loadMore() }
                }
            }
        }
    }
    FileDialog {
        id: imagePicker
        property string serverId: ""
        property string kind: "icon"
        title: kind === "icon" ? "Choose server icon" : "Choose server banner"
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp *.gif)"]
        onAccepted: {
            if (serverId !== window.serverId || serverId !== guildManagement.guildId) { window.localError = "The selected server changed. Choose the image again."; return }
            let values = {}; values[kind === "icon" ? "iconPath" : "bannerPath"] = selectedFile.toString()
            window.execute("editGuild", window.serverId, values)
        }
    }
    Dialog {
        id: confirmation
        parent: Overlay.overlay; anchors.centerIn: parent
        property string serverId: ""; property string action: ""; property string targetId: ""; property var values: ({}); property string message: ""
        modal: true; width: Math.min(440,parent.width - 40); padding: 22
        standardButtons: Dialog.Cancel | Dialog.Ok
        contentItem: Label { text: confirmation.message; color: Theme.text; wrapMode: Text.Wrap; textFormat: Text.PlainText }
        background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
        onAccepted: {
            if (serverId !== window.serverId || serverId !== guildManagement.guildId) { window.localError = "The selected server changed. Review the action again."; return }
            window.execute(action,targetId,values)
        }
    }
    RoleEditorDialog { id: roleEditor; parent: Overlay.overlay }
    ScheduledEventDialog { id: eventEditor; parent: Overlay.overlay }
    Loader { id: profileLoader; active: false; sourceComponent: UserCard { onWatchRequested:(key,title)=>{window.watchRequested(key,title);window.close()}; onMessageRequested:window.close() } }
    Loader {
        id: personMenuLoader; active: false
        sourceComponent: PersonContextMenu {
            onProfileRequested:(id,fallback)=>window.userRequested(id,fallback)
            onConfirmationRequested:(title,description,label,payload,reason)=>personConfirmation.ask(title,description,label,payload,reason)
        }
    }
    Connections { target: personMenuLoader.item; ignoreUnknownSignals: true; function onMentionRequested(id) { window.mentionRequested(id); window.close() } }
    ActionConfirmation {
        id: personConfirmation
        onConfirmed:(payload,reason)=> {
            if (payload.service === "member") { memberActions.setBlocked(payload.targetId,true); return }
            if (payload.guildId !== window.serverId || payload.guildId !== guildManagement.guildId) { window.localError = "The selected server changed. Review the action again."; return }
            const values = Object.assign({},payload.values || {})
            if (reason) values.reason = reason
            window.execute(payload.action,payload.targetId,values)
        }
    }
}
