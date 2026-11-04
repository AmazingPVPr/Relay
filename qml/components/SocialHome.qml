import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Item {
    id: home
    required property var service
    property string page: "friends"
    property string friendTab: "online"
    property bool spamTab: false
    property bool accountNoticeVisible: true
    property string query: ""
    property var menuPerson: ({})
    property string confirmationAction: ""
    property var confirmationPerson: ({})
    readonly property bool requestsPage: page === "requests"
    readonly property bool previewing: requestsPage && !!service.selectedMessageRequest && !!service.selectedMessageRequest.recordId
    readonly property var rows: {
        const source = requestsPage ? (spamTab ? service.spamRequests : service.messageRequests) : friendTab === "pending" ? service.pendingFriends : service.friends
        const term = query.trim().toLocaleLowerCase()
        return (source || []).filter(person => {
            if (!requestsPage && friendTab === "online" && (["online", "idle", "dnd"].indexOf(person.status) === -1)) return false
            return !term.length || [person.name, person.username].join(" ").toLocaleLowerCase().indexOf(term) !== -1
        })
    }
    signal messageRequested(string userId)
    signal profileRequested(string userId, var fallback)
    signal settingsRequested(string page)
    signal blockRequested(string userId, var fallback)
    onPageChanged: { query = ""; service.clearSocialFeedback() }
    onFriendTabChanged: { query = ""; service.clearSocialFeedback() }
    onSpamTabChanged: { query = ""; service.previewMessageRequest("") }
    function userId(person) { return String(person.userId || person.recordId || "") }
    function openMenu(person) { menuPerson = person; personOptions.popup() }
    function ask(action, person) { confirmationAction = action; confirmationPerson = person; confirmAction.open() }
    function label() { return requestsPage ? (spamTab ? "Spam" : "Message Requests") : friendTab === "pending" ? "Pending" : friendTab === "all" ? "All Friends" : "Online" }
    function emptyTitle() {
        if (query.length) return "No results found"
        if (requestsPage) return spamTab ? "No spam requests" : "No message requests"
        if (friendTab === "pending") return "No pending friend requests"
        if (friendTab === "all") return "Your friends will appear here"
        return "No friends online right now"
    }
    function emptyDescription() {
        if (query.length) return "Try another name or username."
        if (requestsPage) return spamTab ? "Requests marked as spam will appear here." : "When someone who isn’t your friend sends you a request, you can review it here."
        if (friendTab === "pending") return "Incoming and outgoing friend requests will appear here."
        if (friendTab === "all") return "Use Add Friend to send someone a friend request."
        return "Choose All to see all of your friends."
    }

    ColumnLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 48
            color: Theme.canvas; clip: true
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 18; anchors.rightMargin: 12; spacing: 10
                SocialIcon { name: home.requestsPage ? "mail" : "members"; Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
                Text { visible: home.width > 600; text: home.requestsPage ? "Message Requests" : "Friends"; color: Theme.text; font.pixelSize: Math.round(15 * preferences.fontScale); font.weight: Font.DemiBold }
                Rectangle { visible: home.width > 600; implicitWidth: 1; implicitHeight: 23; color: Theme.border }
                Flickable {
                    id: tabStrip
                    Layout.fillWidth: true; Layout.fillHeight: true
                    contentWidth: headerTabs.implicitWidth; contentHeight: height
                    boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds; clip: true
                    RowLayout {
                        id: headerTabs; height: tabStrip.height; spacing: 6
                        Repeater {
                            model: home.requestsPage ? [{id:"requests",title:"Requests" + (home.service.messageRequests.length ? " (" + home.service.messageRequests.length + ")" : "")}, {id:"spam",title:"Spam" + (home.service.spamRequests.length ? " (" + home.service.spamRequests.length + ")" : "")}] : [{id:"online",title:"Online"},{id:"all",title:"All"},{id:"pending",title:"Pending" + (home.service.pendingFriends.length ? " (" + home.service.pendingFriends.length + ")" : "")},{id:"add",title:"Add Friend"}]
                            delegate: Button {
                                required property var modelData
                                readonly property bool selected: home.requestsPage ? (home.spamTab ? modelData.id === "spam" : modelData.id === "requests") : home.friendTab === modelData.id
                                implicitHeight: 30; implicitWidth: label.implicitWidth + 20
                                leftPadding: 10; rightPadding: 10; hoverEnabled: true
                                Accessible.name: modelData.title
                                Accessible.role: Accessible.PageTab
                                Accessible.selected: selected
                                background: Rectangle { radius: 5; color: modelData.id === "add" ? Theme.accent : parent.selected ? Theme.selected : parent.hovered ? Theme.hover : "transparent"; border.width: parent.activeFocus ? 1 : 0; border.color: Theme.focus }
                                contentItem: Text { id: label; text: modelData.title; color: parent.selected || modelData.id === "add" ? Theme.text : Theme.secondary; font.pixelSize: Math.round(14 * preferences.fontScale); horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                onClicked: home.requestsPage ? home.spamTab = modelData.id === "spam" : home.friendTab = modelData.id
                            }
                        }
                    }
                }
                ActionButton { iconName: "refresh"; hint: "Refresh friends and requests"; quiet: true; implicitWidth: 28; implicitHeight: 30; padding: 4; enabled: !home.service.socialBusy && !home.service.requestPreviewLoading; onClicked: home.service.refreshSocial() }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.surface }
        }
        Rectangle {
            Layout.fillWidth: true; implicitHeight: feedback.implicitHeight + 20
            visible: home.service.socialError.length > 0 || home.service.socialStatus.length > 0
            color: home.service.socialError.length ? Theme.errorSurface : Theme.surface
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 20; anchors.rightMargin: 10; spacing: 10
                Text { id: feedback; Layout.fillWidth: true; text: home.service.socialError || home.service.socialStatus; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: home.service.socialError.length ? Theme.danger : Theme.success; font.pixelSize: Math.round(12 * preferences.fontScale) }
                ActionButton { iconName: "close"; hint: "Dismiss"; quiet: true; padding: 4; implicitWidth: 26; implicitHeight: 26; onClicked: home.service.clearSocialFeedback() }
            }
        }
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 24; spacing: 18
                visible: !home.previewing && (home.requestsPage || home.friendTab !== "add")
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: noticeRow.implicitHeight + 20
                    visible: !home.requestsPage && home.accountNoticeVisible
                    color: Theme.rail; radius: 5
                    RowLayout {
                        id: noticeRow; anchors.fill: parent; anchors.margins: 10; spacing: 10
                        AppIcon { name: "blocked"; Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
                        Text { Layout.fillWidth: true; text: "Looking for accounts you’ve blocked or ignored?"; color: Theme.secondary; wrapMode: Text.WordWrap; font.pixelSize: Math.round(12 * preferences.fontScale) }
                        ActionButton { text: "Go to settings"; quiet: true; implicitHeight: 28; padding: 3; onClicked: home.settingsRequested("messaging") }
                        ActionButton { iconName: "close"; hint: "Dismiss notice"; quiet: true; implicitWidth: 25; implicitHeight: 25; padding: 4; onClicked: home.accountNoticeVisible = false }
                    }
                }
                TextField {
                    id: search; objectName: "socialSearch"
                    Layout.fillWidth: true; implicitHeight: 40
                    placeholderText: home.requestsPage ? "Search requests" : "Search"; selectByMouse: true
                    leftPadding: 37; rightPadding: 12; color: Theme.text; placeholderTextColor: Theme.muted
                    font.pixelSize: Math.round(14 * preferences.fontScale)
                    text: home.query
                    onTextEdited: home.query = text
                    background: Rectangle { radius: 5; color: Theme.rail; border.width: search.activeFocus ? 1 : 0; border.color: Theme.focus }
                    AppIcon { x: 12; anchors.verticalCenter: parent.verticalCenter; width: 17; height: 17; name: "search" }
                }
                Text { text: home.label() + " — " + home.rows.length; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); font.weight: Font.DemiBold; visible: home.rows.length > 0 }
                Item {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    ListView {
                        id: people; objectName: "socialPeople"
                        anchors.fill: parent
                        model: home.rows; spacing: 1; clip: true; reuseItems: true
                        boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
                        delegate: SocialPersonRow {
                            required property var modelData
                            width: people.width
                            person: modelData
                            mode: home.requestsPage ? "request" : home.friendTab === "pending" ? "pending" : "friend"
                            busy: home.service.socialBusy
                            onActivated: home.requestsPage ? home.service.previewMessageRequest(String(modelData.recordId)) : home.friendTab === "pending" ? home.profileRequested(home.userId(modelData), modelData) : home.messageRequested(home.userId(modelData))
                            onContextRequested: home.openMenu(modelData)
                            onAccepted: home.service.acceptFriendRequest(home.userId(modelData))
                            onDismissed: home.service.dismissFriendRequest(home.userId(modelData))
                        }
                        ScrollBoost { flickable: people }
                        ScrollBar.vertical: ScrollBar { width: 7 }
                    }
                    ColumnLayout {
                        anchors.centerIn: parent; width: Math.min(390, parent.width - 32); spacing: 14
                        visible: home.rows.length === 0 && !home.service.socialBusy
                        Rectangle {
                            Layout.alignment: Qt.AlignHCenter; implicitWidth: 104; implicitHeight: 88; radius: 24; color: Theme.surface
                            SocialIcon { anchors.centerIn: parent; width: 46; height: 46; name: home.requestsPage ? "mail" : home.friendTab === "pending" ? "userPlus" : "members"; color: Theme.muted }
                        }
                        Text { Layout.fillWidth: true; text: home.emptyTitle(); color: Theme.text; font.pixelSize: Math.round(20 * preferences.fontScale); font.weight: Font.DemiBold; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap }
                        Text { Layout.fillWidth: true; text: home.emptyDescription(); color: Theme.muted; font.pixelSize: Math.round(13 * preferences.fontScale); horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap }
                    }
                    BusyIndicator { anchors.centerIn: parent; width: 34; height: 34; visible: home.rows.length === 0 && home.service.socialBusy; running: visible }
                }
            }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 30; spacing: 18
                visible: !home.requestsPage && home.friendTab === "add"
                Text { text: "Add Friend"; color: Theme.text; font.pixelSize: Math.round(20 * preferences.fontScale); font.weight: Font.DemiBold }
                Text { Layout.fillWidth: true; text: "You can add friends with their Discord username."; color: Theme.muted; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: addRow.implicitHeight + 16
                    radius: 6; color: Theme.rail; border.width: addUsername.activeFocus ? 1 : 0; border.color: Theme.focus
                    RowLayout {
                        id: addRow; anchors.fill: parent; anchors.margins: 8; spacing: 10
                        TextField { id: addUsername; objectName: "friendUsername"; Layout.fillWidth: true; placeholderText: "Enter a username"; selectByMouse: true; color: Theme.text; placeholderTextColor: Theme.muted; font.pixelSize: Math.round(14 * preferences.fontScale); background: Item {} enabled: !home.service.socialBusy; onAccepted: if (text.trim().length && !home.service.socialBusy) home.service.sendFriendRequest(text.trim()) }
                        ActionButton { objectName: "sendFriendRequest"; text: "Send Friend Request"; primary: true; enabled: addUsername.text.trim().length > 0 && !home.service.socialBusy; onClicked: home.service.sendFriendRequest(addUsername.text.trim()) }
                    }
                }
                Item { Layout.fillHeight: true }
            }
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                visible: home.previewing
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: 72
                    color: Theme.sidebar
                    RowLayout {
                        anchors.fill: parent; anchors.margins: 15; spacing: 12
                        ActionButton { iconName: "back"; hint: "Back to requests"; quiet: true; implicitWidth: 30; padding: 4; onClicked: home.service.previewMessageRequest("") }
                        SocialAvatar { Layout.preferredWidth: 38; Layout.preferredHeight: 38; user: home.service.selectedMessageRequest; showPresence: false }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 3
                            Text { Layout.fillWidth: true; text: home.service.selectedMessageRequest.name || "Message request"; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(15 * preferences.fontScale); font.weight: Font.DemiBold; elide: Text.ElideRight }
                            Text { Layout.fillWidth: true; text: home.service.selectedMessageRequest.username || ""; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); elide: Text.ElideRight }
                        }
                        ActionButton { text: "Profile"; quiet: true; onClicked: home.profileRequested(home.userId(home.service.selectedMessageRequest), home.service.selectedMessageRequest) }
                    }
                }
                Text { Layout.fillWidth: true; Layout.margins: 16; text: "Preview this conversation before accepting. Accept the request to reply."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
                Item {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    ListView {
                        id: preview; objectName: "requestPreview"
                        anchors.fill: parent; anchors.leftMargin: 18; anchors.rightMargin: 18
                        model: home.service.requestPreviewMessages
                        clip: true; reuseItems: true; spacing: 18
                        boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
                        delegate: ColumnLayout {
                            required property var modelData
                            width: preview.width; spacing: 6
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                Text { text: modelData.author || "Unknown user"; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale); font.weight: Font.DemiBold }
                                Text { text: modelData.timestamp ? preferences.formatMessageTime(modelData.timestamp) : ""; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale) }
                            }
                            LinkText { Layout.fillWidth: true; rawText: modelData.rawContent || modelData.content || ""; metadata: modelData; color: Theme.text; font.pixelSize: Math.round(preferences.chatFontSize); wrapMode: TextEdit.Wrap; readOnly: true; selectByMouse: true; selectionColor: Theme.selected; selectedTextColor: Theme.text; onUserRequested: id => home.profileRequested(id, {}) }
                            Repeater { model: modelData.attachments || []; delegate: Text { required property var modelData; Layout.fillWidth: true; text: "Attachment · " + String(modelData.filename || modelData.name || "File"); textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap } }
                            Repeater { model: modelData.embeds || []; delegate: Text { required property var modelData; Layout.fillWidth: true; text: String(modelData.title || modelData.description || "Link preview"); textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap } }
                        }
                        ScrollBoost { flickable: preview }
                        ScrollBar.vertical: ScrollBar { width: 7 }
                    }
                    BusyIndicator { anchors.centerIn: parent; visible: home.service.requestPreviewLoading; running: visible; width: 34; height: 34 }
                    Text { anchors.centerIn: parent; width: Math.min(340, parent.width - 30); text: "There are no messages to preview."; color: Theme.muted; font.pixelSize: Math.round(13 * preferences.fontScale); horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; visible: !home.service.requestPreviewLoading && !preview.count }
                }
                Rectangle {
                    Layout.fillWidth: true; implicitHeight: actionsRow.implicitHeight + 24; color: Theme.sidebar
                    RowLayout {
                        id: actionsRow; anchors.fill: parent; anchors.margins: 12; spacing: 10
                        ActionButton { text: "Ignore"; quiet: true; enabled: !home.service.socialBusy; onClicked: home.ask("request", home.service.selectedMessageRequest) }
                        ActionButton { text: "Block"; destructive: true; quiet: true; enabled: !home.service.socialBusy; onClicked: home.blockRequested(home.userId(home.service.selectedMessageRequest), home.service.selectedMessageRequest) }
                        Item { Layout.fillWidth: true }
                        ActionButton { objectName: "acceptMessageRequest"; text: "Accept Message Request"; primary: true; enabled: !home.service.socialBusy; onClicked: home.service.acceptMessageRequest(String(home.service.selectedMessageRequest.recordId)) }
                    }
                }
            }
        }
    }
    BoundedMenu {
        id: personOptions
        MenuItem { text: "Profile"; onTriggered: home.profileRequested(home.userId(home.menuPerson), home.menuPerson) }
        MenuItem { text: home.requestsPage ? "View message request" : "Message"; visible: home.requestsPage || home.friendTab !== "pending"; height: visible ? implicitHeight : 0; onTriggered: home.requestsPage ? home.service.previewMessageRequest(String(home.menuPerson.recordId)) : home.messageRequested(home.userId(home.menuPerson)) }
        MenuItem { text: "Accept Friend Request"; visible: !home.requestsPage && home.friendTab === "pending" && home.menuPerson.direction === "incoming"; height: visible ? implicitHeight : 0; enabled: !home.service.socialBusy; onTriggered: home.service.acceptFriendRequest(home.userId(home.menuPerson)) }
        MenuSeparator {}
        MenuItem { text: home.requestsPage ? "Ignore request" : home.friendTab === "pending" ? (home.menuPerson.direction === "incoming" ? "Ignore Friend Request" : "Cancel Friend Request") : "Remove Friend"; enabled: !home.service.socialBusy; onTriggered: !home.requestsPage && home.friendTab === "pending" ? home.service.dismissFriendRequest(home.userId(home.menuPerson)) : home.ask(home.requestsPage ? "request" : "friend", home.menuPerson) }
        MenuItem { text: "Block"; enabled: !home.service.socialBusy; onTriggered: home.blockRequested(home.userId(home.menuPerson), home.menuPerson) }
    }
    Dialog {
        id: confirmAction
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(420, home.width - 30); modal: true; focus: true
        title: home.confirmationAction === "friend" ? "Remove Friend" : "Ignore Message Request"
        background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 20
            Text { Layout.fillWidth: true; text: home.confirmationAction === "friend" ? "Remove " + String(home.confirmationPerson.name || "this person") + " from your friends?" : "Ignore the message request from " + String(home.confirmationPerson.name || "this person") + "?"; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); wrapMode: Text.WordWrap }
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                Item { Layout.fillWidth: true }
                ActionButton { text: "Cancel"; quiet: true; onClicked: confirmAction.close() }
                ActionButton { text: home.confirmationAction === "friend" ? "Remove Friend" : "Ignore Request"; destructive: true; enabled: !home.service.socialBusy; onClicked: { if (home.confirmationAction === "friend") home.service.removeFriend(home.userId(home.confirmationPerson)); else home.service.dismissMessageRequest(String(home.confirmationPerson.recordId)); confirmAction.close() } }
            }
        }
    }
}
