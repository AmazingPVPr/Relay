import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: dialog
    parent: Overlay.overlay
    property string guildId: ""
    property string channelId: ""
    property var channel: ({})
    property var parents: [{id:"",name:"No category"}]
    property string validationError: ""
    property bool submitted: false
    property var initial: ({})
    property string accountId: ""
    property bool canEditPrivacy: false
    property real maxBitrate: 96
    readonly property bool editing: channelId.length > 0
    readonly property var channelTypes: [{name:"Text",kind:"text",type:0},{name:"Voice",kind:"voice",type:2},{name:"Forum",kind:"forum",type:15},{name:"Category",kind:"category",type:4}]
    readonly property string channelKind: channelTypes[Math.max(0, channelType.currentIndex)].kind
    width: Math.min(520, parent.width - 40)
    height: Math.min(contentColumn.implicitHeight + 40, parent.height - 40)
    x: (parent.width - width) / 2; y: Math.max(16, (parent.height - height) / 2)
    modal: true; focus: true; padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    function prepare(guild, value, parentId) {
        guildId = guild; channel = value || {}; channelId = channel.recordId || ""
        guildManagement.selectGuild(guild)
        accountId = discord.currentUserId
        canEditPrivacy = !editing || !!guildManagement.channelPermissions(guild, channelId).managePermissions
        maxBitrate = Number(guildManagement.guild.maxBitrate || 96000) / 1000
        validationError = ""; submitted = false
        const choices = [{id:"",name:"No category"}]
        for (const entry of guildManagement.channels) if (Number(entry.type) === 4 && entry.id !== channelId) choices.push({id:entry.id,name:entry.name})
        parents = choices
        channelType.currentIndex = Math.max(0, channelTypes.findIndex(entry => entry.kind === (channel.kind || "text")))
        nameField.text = channel.name || ""
        topicField.text = channel.topic || ""
        privateChannel.checked = !!channel.locked
        category.currentIndex = Math.max(0, choices.findIndex(entry => entry.id === (parentId || channel.parentId || "")))
        bitrate.value = Number(channel.bitrate || 64000) / 1000
        userLimit.value = Number(channel.userLimit || 0)
        initial = {name:nameField.text,topic:topicField.text,private:privateChannel.checked,parentId:choices[category.currentIndex].id,bitrate:bitrate.value,userLimit:userLimit.value}
        open(); nameField.forceActiveFocus()
    }
    function openCreate(guild, parentId) { prepare(guild, {}, parentId) }
    function openEdit(guild, value) { prepare(guild, value, value.parentId || "") }
    function save() {
        if (!accountId || accountId !== discord.currentUserId) { close(); return }
        if (!nameField.text.trim()) { validationError = "Enter a channel name."; return }
        const values = {}
        if (!editing || nameField.text.trim() !== initial.name) values.name = nameField.text.trim()
        if (!editing || (canEditPrivacy && privateChannel.checked !== initial.private)) values.private = privateChannel.checked
        if (!editing) values.type = channelTypes[channelType.currentIndex].type
        if (channelKind !== "category" && (!editing || parents[category.currentIndex].id !== initial.parentId)) values.parentId = parents[category.currentIndex].id
        if ((channelKind === "text" || channelKind === "forum") && (!editing || topicField.text !== initial.topic)) values.topic = topicField.text
        if (channelKind === "voice") {
            if (!editing || bitrate.value !== initial.bitrate) values.bitrate = Math.round(bitrate.value * 1000)
            if (!editing || userLimit.value !== initial.userLimit) values.userLimit = Math.round(userLimit.value)
        }
        if (!Object.keys(values).length) { close(); return }
        guildManagement.selectGuild(guildId)
        submitted = guildManagement.execute(editing ? "editChannel" : "createChannel", channelId, values)
    }
    background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
    contentItem: HardScrollView {
        id: scroll
        ColumnLayout {
            id: contentColumn
            width: scroll.availableWidth; spacing: 14
            RowLayout {
                Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: dialog.editing ? "Edit channel" : "Create channel"; color: Theme.text; font.pixelSize: Math.round(20 * preferences.fontScale); font.weight: Font.DemiBold }
                ActionButton { iconName: "close"; hint: "Close"; quiet: true; onClicked: dialog.close() }
            }
            ComboBox { id: channelType; Layout.fillWidth: true; model: dialog.channelTypes; textRole: "name"; enabled: !dialog.editing }
            Text { text: "Channel name"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
            AppField { id: nameField; Layout.fillWidth: true; maximumLength: 100; placeholderText: "Channel name"; onAccepted: dialog.save() }
            Text { visible: dialog.channelKind !== "category"; text: "Category"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
            ComboBox { id: category; visible: dialog.channelKind !== "category"; Layout.fillWidth: true; model: dialog.parents; textRole: "name" }
            Text { visible: dialog.channelKind === "text" || dialog.channelKind === "forum"; text: "Topic"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
            AppField { id: topicField; visible: dialog.channelKind === "text" || dialog.channelKind === "forum"; Layout.fillWidth: true; maximumLength: 1024; placeholderText: "Optional topic" }
            SettingsToggle { id: privateChannel; Layout.fillWidth: true; title: "Private channel"; enabled: dialog.canEditPrivacy; description: dialog.canEditPrivacy ? "Restrict access for @everyone. Existing role permissions are preserved." : "Manage Permissions is required to change channel privacy." }
            RowLayout {
                visible: dialog.channelKind === "voice"; Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: "Bitrate"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
                NumericValue { id: bitrate; value: 64; minimum: 8; maximum: dialog.maxBitrate; unit: " kbps"; onValueEdited: number => value = number }
            }
            RowLayout {
                visible: dialog.channelKind === "voice"; Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: "User limit · 0 is unlimited"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
                NumericValue { id: userLimit; value: 0; minimum: 0; maximum: 99; unit: ""; decimals: 0; onValueEdited: number => value = number }
            }
            Text { Layout.fillWidth: true; visible: text.length > 0; text: dialog.validationError || (dialog.submitted || guildManagement.guildId === dialog.guildId ? guildManagement.error : ""); textFormat: Text.PlainText; color: Theme.warning; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ActionButton { text: "Cancel"; quiet: true; onClicked: dialog.close() }
                ActionButton { text: guildManagement.mutating ? "Saving…" : dialog.editing ? "Save changes" : "Create channel"; primary: true; enabled: !guildManagement.mutating; onClicked: dialog.save() }
            }
        }
    }
    Connections { target: guildManagement; function onActionCompleted(action, id, result) { if (dialog.visible && dialog.submitted && guildManagement.guildId === dialog.guildId && (action === "createChannel" || (action === "editChannel" && id === dialog.channelId))) { dialog.submitted = false; dialog.close() } } }
    Connections { target: discord; function onStateChanged() { if (dialog.visible && dialog.accountId !== discord.currentUserId) dialog.close() } }
}
