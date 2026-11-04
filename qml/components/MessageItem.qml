import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "MediaUrls.js" as MediaUrls
import "Theme.js" as Theme

Item {
    id: root
    required property var message
    property bool compact: false
    property bool mediaActive: true
    property bool highlighted: false
    readonly property bool hasReply: String(message.replyId || "").length > 0 || !!message.replyMissing
    readonly property bool canJumpReply: /^[0-9]{1,22}$/.test(String(message.replyId || "")) && !message.replyDeleted
    ListView.onPooled: { mediaActive = false; messageMenu.close(); reactionPicker.close(); actionDialog.close(); cancelEdit() }
    ListView.onReused: mediaActive = true
    readonly property bool isSystem: !!message.systemMessage
    signal userRequested(string userId, var fallback)
    signal contextRequested(string userId, var fallback)
    signal translationRequested(string text)
    signal replyRequested(string channelId, string messageId)
    signal imageRequested(string source, string originalUrl, string title)
    function authorInfo() { return {name:message.author,avatarUrl:message.avatarUrl,guildId:message.guildId || "",roleColor:message.roleColor || "",bot:!!message.bot,verifiedBot:!!message.verifiedBot} }
    implicitHeight: body.y + body.implicitHeight + (compact || isSystem ? 6 : preferences.messageGroupGap)
    function timeLabel(value) {
        const locale = preferences.language, format = preferences.timeFormat
        return preferences.formatMessageTime(value)
    }
    readonly property color readableText: preferences.highContrast ? "#ffffff" : Theme.text
    readonly property color readableMuted: preferences.highContrast ? "#d2d2db" : Theme.muted
    Rectangle { anchors.fill: parent; color: root.highlighted ? "#2b3a4c" : root.message.currentUserMentioned ? "#302b21" : hover.hovered ? Theme.hover : "transparent"; border.width: preferences.highContrast && hover.hovered ? 1 : 0; border.color: "#a5a5b1" }
    Rectangle { width: 3; anchors.top: parent.top; anchors.bottom: parent.bottom; visible: root.highlighted || !!root.message.currentUserMentioned; color: root.highlighted ? "#83bcf1" : "#d1a95c" }
    function isAuthorPoint(point) {
        return (avatarBox.visible && avatarBox.contains(avatarBox.mapFromItem(root,point.x,point.y)))
            || (!root.isSystem && authorLabel.contains(authorLabel.mapFromItem(root,point.x,point.y)))
    }
    readonly property bool actionable: !discord.demoMode && discord.connected && !channelTools.actionBusy && /^[0-9]{1,22}$/.test(String(message.recordId || "")) && !message.pending && !message.failed
    property var actionTarget: ({})
    property string actionMode: ""
    property bool editing: false
    property bool editSubmitted: false
    property string editError: ""
    readonly property string messageIdentity: String(message.channelId || "") + "/" + String(message.recordId || "")
    onMessageIdentityChanged: cancelEdit()
    function cancelEdit() {
        editing = false
        editSubmitted = false
        editError = ""
        if (inlineText) inlineText.text = ""
    }
    function beginEdit() {
        if (!actionable || !message.own || isSystem) return
        captureTarget()
        actionMode = "edit"
        editError = ""
        editSubmitted = false
        inlineText.text = actionTarget.content
        editing = true
        channelTools.clearActionError()
        inlineText.forceActiveFocus()
        inlineText.cursorPosition = inlineText.length
    }
    function saveEdit() {
        if (!editing || editSubmitted || !actionable || !inlineText.text.trim().length || inlineText.length > 2000) return
        if (inlineText.text === actionTarget.content) { cancelEdit(); return }
        editSubmitted = true
        editError = ""
        if (!channelTools.messageAction("edit", actionTarget.channel, actionTarget.id, inlineText.text, "")) {
            editSubmitted = false
            editError = channelTools.actionError
        }
    }
    function captureTarget() {
        actionTarget = {id: String(message.recordId || ""), channel: String(message.channelId || discord.currentChannelId),
            guild: String(message.guildId || ""), account: String(discord.currentUserId || ""), own: !!message.own, content: String(message.rawContent || message.content || "")}
    }
    function react(emoji, remove) {
        channelTools.messageAction(remove ? "unreact" : "react", actionTarget.channel, actionTarget.id, emoji)
    }
    function showAction(mode) {
        if (mode === "edit") { beginEdit(); return }
        actionMode = mode
        actionText.text = ""
        destination.currentIndex = -1
        actionDialog.open()
        if (mode === "reply") actionText.forceActiveFocus()
    }
    TapHandler {
        enabled: !root.editing
        acceptedButtons: Qt.RightButton
        onTapped: (eventPoint, button) => {
            if (!root.isAuthorPoint(eventPoint.position)) {
                root.captureTarget()
                messageMenu.popup(root, eventPoint.position.x, eventPoint.position.y)
            }
        }
    }
    BoundedMenu {
        id: messageMenu
        objectName: "messageContextMenu"
        palette.text: Theme.text
        palette.buttonText: Theme.text
        MenuItem {
            height: 54
            enabled: root.actionable
            contentItem: Row {
                spacing: 5
                Repeater {
                    model: ["👍", "😂", "🔥", "❤️"]
                    delegate: Button {
                        required property string modelData
                        width: 48; height: 42
                        text: modelData
                        hoverEnabled: true
                        background: Rectangle { radius: 7; color: parent.hovered ? Theme.hover : Theme.input }
                        contentItem: Text { text: parent.text; font.family: "Noto Color Emoji"; font.pixelSize: 23; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        Accessible.name: "React with " + modelData
                        onClicked: { messageMenu.close(); root.react(modelData, false) }
                    }
                }
            }
        }
        MenuItem { text: "Add Reaction…"; enabled: root.actionable && !root.isSystem; onTriggered: reactionPicker.showNear(root) }
        MenuSeparator {}
        MenuItem { objectName: "editMessageAction"; text: "Edit Message"; visible: !!root.message.own && !root.isSystem; height: visible ? implicitHeight : 0; enabled: root.actionable; onTriggered: root.showAction("edit") }
        MenuItem { text: "Reply"; enabled: root.actionable && !root.isSystem; onTriggered: root.showAction("reply") }
        MenuItem { text: "Forward"; enabled: root.actionable && !root.isSystem; onTriggered: root.showAction("forward") }
        MenuSeparator {}
        MenuItem { text: "Copy Text"; enabled: String(root.actionTarget.content || "").length > 0; onTriggered: chatText.copy(root.actionTarget.content) }
        MenuItem { text: root.message.pinned ? "Unpin Message" : "Pin Message"; enabled: root.actionable; onTriggered: channelTools.setPinned(root.actionTarget.channel, root.actionTarget.id, !root.message.pinned) }
        MenuItem { text: "Copy Message Link"; onTriggered: chatText.copy("https://discord.com/channels/" + (root.actionTarget.guild || "@me") + "/" + root.actionTarget.channel + "/" + root.actionTarget.id) }
        MenuItem { text: "Translate…"; enabled: String(root.actionTarget.content || "").length > 0; onTriggered: root.translationRequested(root.actionTarget.content) }
        MenuSeparator { visible: !!root.message.own; height: visible ? implicitHeight : 0 }
        MenuItem {
            text: "Delete Message"
            objectName: "deleteMessageAction"
            visible: !!root.message.own; height: visible ? implicitHeight : 0
            enabled: root.actionable
            palette.text: Theme.danger; palette.windowText: Theme.danger
            onTriggered: root.showAction("delete")
        }
        MenuSeparator {}
        MenuItem { text: "Copy Message ID"; onTriggered: chatText.copy(root.actionTarget.id) }
    }
    EmojiPicker { id: reactionPicker; onEmojiSelected: text => root.react(text, false) }
    Dialog {
        id: actionDialog
        objectName: "messageActionDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(520, parent.width - 32)
        modal: true
        closePolicy: channelTools.actionBusy ? Popup.NoAutoClose : Popup.CloseOnEscape
        title: root.actionMode === "delete" ? "Delete Message?" : root.actionMode === "forward" ? "Forward Message" : "Reply"
        background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
        contentItem: ColumnLayout {
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: root.actionMode === "delete" ? "This message will be permanently deleted." : root.actionMode === "forward" ? "Choose a destination for this message." : "Mentions in this action will not send notifications."
                color: Theme.text; wrapMode: Text.WordWrap; textFormat: Text.PlainText
            }
            Text {
                Layout.fillWidth: true
                visible: root.actionMode === "delete" || root.actionMode === "reply" || root.actionMode === "forward"
                text: root.actionTarget.content || "Attachment or message"
                color: Theme.muted; maximumLineCount: 4; elide: Text.ElideRight; wrapMode: Text.Wrap; textFormat: Text.PlainText
            }
            ScrollView {
                Layout.fillWidth: true; Layout.preferredHeight: 130
                visible: root.actionMode === "reply"
                TextArea { id: actionText; enabled: !channelTools.actionBusy; wrapMode: TextEdit.Wrap; selectByMouse: true; textFormat: TextEdit.PlainText; color: Theme.text; placeholderText: "Message"; background: Rectangle { color: Theme.input; radius: 5 } }
            }
            ComboBox {
                id: destination
                Layout.fillWidth: true
                visible: root.actionMode === "forward"
                enabled: !channelTools.actionBusy
                textRole: "name"; valueRole: "recordId"
                model: {
                    let rows = []
                    for (let i = 0; i < discord.channels.count; ++i) {
                        const channel = discord.channels.get(i)
                        if (channel.kind === "text" || channel.kind === "thread") rows.push(channel)
                    }
                    for (let j = 0; j < discord.dms.count; ++j) rows.push(discord.dms.get(j))
                    return rows
                }
                displayText: currentIndex < 0 ? "Choose channel or conversation" : currentText
            }
            Text { Layout.fillWidth: true; visible: channelTools.actionError.length > 0; text: channelTools.actionError; color: Theme.danger; wrapMode: Text.Wrap; textFormat: Text.PlainText }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button { text: "Cancel"; enabled: !channelTools.actionBusy; onClicked: actionDialog.close() }
                Button {
                    text: channelTools.actionBusy ? "Working…" : root.actionMode === "delete" ? "Delete" : "Send"
                    enabled: !channelTools.actionBusy && (root.actionMode === "delete" || (root.actionMode === "forward" ? destination.currentIndex >= 0 : actionText.text.trim().length > 0 && actionText.text.length <= 2000))
                    onClicked: channelTools.messageAction(root.actionMode, root.actionTarget.channel, root.actionTarget.id, actionText.text, String(destination.currentValue || ""))
                }
            }
        }
        onOpened: channelTools.clearActionError()
    }
    Connections {
        target: channelTools
        function onMessageActionFinished(action, messageId) {
            if (messageId !== root.actionTarget.id || action !== root.actionMode) return
            if (action === "edit") root.cancelEdit()
            else actionDialog.close()
        }
        function onChanged() {
            if (root.editing && root.editSubmitted && !channelTools.actionBusy && channelTools.actionError.length > 0) {
                root.editSubmitted = false
                root.editError = channelTools.actionError
            }
        }
    }
    Connections {
        target: discord
        function onStateChanged() {
            if (root.editing && (String(discord.currentChannelId) !== root.actionTarget.channel
                    || String(discord.currentUserId || "") !== root.actionTarget.account))
                root.cancelEdit()
        }
    }
    HoverHandler { id: hover }
    Rectangle {
        id: avatarBox
        x: 16; y: 11; width: 40; height: 40; radius: 20
        color: Theme.avatar
        visible: !root.compact && !root.isSystem
        Text { anchors.centerIn: parent; visible: !avatarImage.ready; text: (root.message.author || "?").substring(0, 2).toUpperCase(); textFormat: Text.PlainText; font.pixelSize: Math.round(12 * preferences.fontScale); font.weight: Font.DemiBold; color: Theme.text }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; acceptedButtons: Qt.LeftButton | Qt.RightButton; onClicked: mouse => mouse.button === Qt.RightButton ? root.contextRequested(root.message.authorId || "", root.authorInfo()) : root.userRequested(root.message.authorId || "", root.authorInfo()) }
        RoundedImage { id: avatarImage; anchors.fill: parent; source: root.message.avatarUrl || ""; radius: 20 }
    }
    AppIcon { x: 25; y: 9; width: 20; height: 20; visible: root.isSystem; name: "chevron"; rotation: -90; color: Theme.muted }
    ColumnLayout {
        id: body
        anchors.left: parent.left; anchors.leftMargin: root.compact && !root.isSystem ? 16 : 72
        anchors.right: parent.right; anchors.rightMargin: 20
        y: root.compact || root.isSystem ? 6 : 10
        spacing: root.compact ? 3 : 6
        RowLayout {
            visible: !root.isSystem
            spacing: 10
            Rectangle { objectName: "messageRoleDot"; visible: preferences.roleColorsMode === "dots" && !!root.message.roleColor; implicitWidth: 8; implicitHeight: 8; radius: 4; color: root.message.roleColor || root.readableText; Accessible.name: "Role color" }
            Text { id: authorLabel; objectName: "messageAuthor"; text: root.message.author || "Unknown user"; color: preferences.roleColorsMode === "names" && root.message.roleColor ? root.message.roleColor : root.readableText; font.pixelSize: Math.round(preferences.chatFontSize); font.weight: Font.DemiBold; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.maximumWidth: Math.max(60, body.width - timestamp.width - authorTag.implicitWidth - appBadge.implicitWidth - 48); MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; acceptedButtons: Qt.LeftButton | Qt.RightButton; onClicked: mouse => mouse.button === Qt.RightButton ? root.contextRequested(root.message.authorId || "", root.authorInfo()) : root.userRequested(root.message.authorId || "", root.authorInfo()) } }
            AppBadge { id: appBadge; visible: !!root.message.bot; verified: !!root.message.verifiedBot }
            GuildTag { id: authorTag; guild: root.message.primaryGuild || ({}) }
            Text { id: timestamp; objectName: "messageTimestamp"; text: root.timeLabel(root.message.timestamp); color: root.readableMuted; font.pixelSize: Math.round(11 * preferences.fontScale); textFormat: Text.PlainText; Layout.maximumWidth: Math.max(85,body.width * .45); elide: Text.ElideRight; HoverHandler { id: timeHover } ToolTip.visible: timeHover.hovered; ToolTip.delay: 500; ToolTip.text: { const language = preferences.language, format = preferences.timeFormat; return preferences.formatDateTime(root.message.timestamp) } }
            Item { Layout.fillWidth: true }
        }
        Button {
            id: replyButton
            objectName: "messageReplyLink"
            Layout.fillWidth: true
            implicitHeight: replyLabel.implicitHeight + 4
            visible: root.hasReply
            enabled: root.canJumpReply
            padding: 2; leftPadding: 0; rightPadding: 3
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            text: root.message.replyDeleted ? "Original message was deleted" : root.message.replyMissing ? "Reply to an unavailable message" : "Replying to " + String(root.message.replyAuthor || "Unknown user") + ": " + String(root.message.replyContent || "Message").replace(/\s+/g," ")
            background: Rectangle { radius: 3; color: replyButton.hovered || replyButton.activeFocus ? Theme.hover : "transparent"; border.width: replyButton.activeFocus ? 1 : 0; border.color: Theme.focus }
            contentItem: Text { id: replyLabel; text: replyButton.text; textFormat: Text.PlainText; color: replyButton.enabled && replyButton.hovered ? Theme.text : Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); elide: Text.ElideRight }
            HoverHandler { cursorShape: replyButton.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
            Accessible.name: text + (enabled ? ". Jump to original message" : ". Unavailable")
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: enabled ? "Jump to original message" : "Original message is unavailable"
            onClicked: root.replyRequested(String(root.message.replyChannelId || root.message.channelId || discord.currentChannelId), String(root.message.replyId))
            Keys.onReturnPressed: event => { if (enabled) clicked(); event.accepted = true }
            Keys.onEnterPressed: event => { if (enabled) clicked(); event.accepted = true }
        }
        Text { visible: !!root.message.forwarded; text: "Forwarded"; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); font.italic: true }
        LinkText {
            objectName: "messageBody"
            Layout.fillWidth: true
            rawText: root.message.rawContent || root.message.content || ""
            metadata: root.message
            onUserRequested: id => {
                const users = root.message.mentionUsers || []
                root.userRequested(id, users.find(user => user.id === id) || {})
            }
            color: root.message.failed ? Theme.danger : root.isSystem ? root.readableMuted : root.readableText
            font.pixelSize: Math.round(root.isSystem ? preferences.chatFontSize * 13 / 14 : preferences.chatFontSize)
            wrapMode: TextEdit.Wrap
            readOnly: true; selectByMouse: true
            selectionColor: Theme.selected; selectedTextColor: Theme.text
            visible: !root.editing && text.length > 0
        }
        ColumnLayout {
            objectName: "inlineMessageEditor"
            Layout.fillWidth: true
            visible: root.editing
            spacing: 6
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(48, Math.min(280, inlineText.implicitHeight))
                clip: true
                TextArea {
                    id: inlineText
                    objectName: "inlineMessageEditText"
                    enabled: !root.editSubmitted
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    textFormat: TextEdit.PlainText
                    color: root.readableText
                    selectionColor: Theme.selected
                    selectedTextColor: Theme.text
                    font.pixelSize: Math.round(preferences.chatFontSize)
                    padding: 10
                    background: Rectangle { color: Theme.input; radius: 5; border.color: Theme.focus; border.width: 1 }
                    Accessible.name: "Edit message"
                    Keys.priority: Keys.BeforeItem
                    Keys.onPressed: event => {
                        event.accepted = false
                        if (event.key === Qt.Key_Escape) {
                            if (!root.editSubmitted) root.cancelEdit()
                            event.accepted = true
                        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                   && !(event.modifiers & Qt.ShiftModifier)) {
                            root.saveEdit()
                            event.accepted = true
                        }
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                visible: root.editError.length > 0
                text: root.editError
                textFormat: Text.PlainText
                color: Theme.danger
                wrapMode: Text.Wrap
                Accessible.role: Accessible.AlertMessage
            }
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: root.editSubmitted ? "Saving…" : "Escape to cancel · Enter to save · Shift+Enter for a new line"
                    textFormat: Text.PlainText
                    color: Theme.muted
                    font.pixelSize: Math.round(11 * preferences.fontScale)
                    wrapMode: Text.Wrap
                }
                ActionButton { objectName: "inlineMessageCancel"; text: "Cancel"; quiet: true; enabled: !root.editSubmitted; onClicked: root.cancelEdit() }
                ActionButton {
                    objectName: "inlineMessageSave"
                    text: "Save"
                    primary: true
                    enabled: root.actionable && !root.editSubmitted && inlineText.text.trim().length > 0 && inlineText.length <= 2000
                    onClicked: root.saveEdit()
                }
            }
        }
        MessageComponents {
            Layout.fillWidth: true
            Layout.maximumWidth: 600
            visible: components.length > 0
            components: root.message.components || []
            messageId: root.message.recordId || ""
            metadata: root.message
            playbackEnabled: root.mediaActive
            onImageRequested:(source,originalUrl,title)=>root.imageRequested(source,originalUrl,title)
            onUserRequested:id=>root.userRequested(id,{})
        }
        Repeater {
            model: root.message.attachments || []
            delegate: AttachmentItem {
                required property var modelData
                Layout.fillWidth: true
                Layout.maximumWidth: 460
                attachment: modelData
                playbackEnabled: root.mediaActive
                onImageRequested: (source, originalUrl, title) => root.imageRequested(source, originalUrl, title)
            }
        }
        Repeater {
            model: root.message.embeds || []
            delegate: EmbedCard {
                required property var modelData
                Layout.fillWidth: true
                Layout.maximumWidth: 510
                embed: modelData
                metadata: root.message
                onUserRequested: id => root.userRequested(id, {})
                playbackEnabled: root.mediaActive
                onImageRequested: (source, originalUrl, title) => root.imageRequested(source, originalUrl, title)
            }
        }
        Repeater {
            model: root.message.stickers || []
            delegate: ColumnLayout {
                required property var modelData
                Layout.fillWidth: true
                Layout.maximumWidth: 180
                spacing: 4
                ImagePreview {
                    Layout.fillWidth: true
                    Layout.maximumWidth: 160
                    visible: source.length > 0
                    source: MediaUrls.cdnImage(modelData.url || "")
                    originalUrl: source
                    title: modelData.name || "Sticker"
                    mediaWidth: 160; mediaHeight: 160; maximumHeight: 160
                    playbackEnabled: root.mediaActive
                    onActivated: (source, originalUrl, title) => root.imageRequested(source, originalUrl, title)
                }
                Text { Layout.fillWidth: true; text: modelData.name ? "Sticker · " + modelData.name : "Sticker"; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(10 * preferences.fontScale); elide: Text.ElideRight }
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: 5
            visible: reactionRepeater.count > 0
            Repeater {
                id: reactionRepeater
                model: root.message.reactions || []
                delegate: Button {
                    required property var modelData
                    height: 30; implicitWidth: reactionRow.implicitWidth + 16
                    enabled: root.actionable
                    hoverEnabled: true
                    background: Rectangle { radius: 6; color: modelData.me ? Theme.selected : parent.hovered ? Theme.hover : Theme.input; border.width: modelData.me ? 1 : 0; border.color: Theme.focus }
                    contentItem: Row {
                        id: reactionRow; spacing: 5
                        Image { width: 21; height: 21; visible: !!modelData.customId; source: modelData.customId ? "https://cdn.discordapp.com/emojis/" + modelData.customId + ".png?size=32" : "" }
                        Text { visible: !modelData.customId; text: modelData.label; font.family: "Noto Color Emoji"; font.pixelSize: 18 }
                        Text { text: modelData.count; color: Theme.text; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                    }
                    Accessible.name: modelData.label + ", " + modelData.count + (modelData.me ? ", remove your reaction" : ", add reaction")
                    ToolTip.visible: hovered; ToolTip.text: modelData.me ? "Remove your reaction" : "Add reaction"
                    onClicked: { root.captureTarget(); root.react(modelData.emoji, modelData.me) }
                }
            }
        }
        Text {
            visible: !!root.message.edited || !!root.message.pending || !!root.message.failed
            text: root.message.failed ? "Could not send" : root.message.pending ? "Sending…" : "Edited"
            color: root.message.failed ? Theme.danger : Theme.muted
            font.pixelSize: Math.round(11 * preferences.fontScale)
        }
    }
}
