import QtQuick
import QtQuick.Window
import "components/Theme.js" as Theme
import "components/ApplicationLifecycle.js" as ApplicationLifecycle
import QtQuick.Controls
import QtQuick.Layouts
import "components"

ApplicationWindow {
    id: window
    visible: true
    property bool applicationClosing: false
    onClosing: event => {
        if (applicationClosing) return
        applicationClosing = true
        ApplicationLifecycle.beginShutdown()
        Qt.quit()
    }
    width: 1280
    height: 800
    minimumWidth: 960
    minimumHeight: 640
    title: discord.currentUserName ? "Relay — " + discord.currentUserName : "Relay"
    color: (preferences.highContrast ? "#101014" : Theme.canvas)
    font.family: "Inter"
    font.pixelSize: Math.round(14 * preferences.fontScale)
    palette.window: (preferences.highContrast ? "#101014" : Theme.canvas)
    palette.windowText: (preferences.highContrast ? "#ffffff" : Theme.text)
    palette.base: (preferences.highContrast ? "#0b0b0e" : Theme.rail)
    palette.alternateBase: (preferences.highContrast ? "#151519" : Theme.sidebar)
    palette.text: (preferences.highContrast ? "#ffffff" : Theme.text)
    palette.button: (preferences.highContrast ? "#85858f" : Theme.border)
    palette.buttonText: (preferences.highContrast ? "#ffffff" : Theme.text)
    palette.highlight: Theme.avatar
    palette.highlightedText: "#ffffff"
    palette.placeholderText: (preferences.highContrast ? "#c7c7cf" : Theme.muted)

    property bool workspaceVisible: discord.currentUserId.length > 0 || discord.demoMode
    readonly property bool conversationModalOpen: ownProfileMenu.accountSwitcherVisible || personMenu.modalDialogOpen
                                                  || channelEditor.visible || actionConfirmation.visible || guildProfileDialog.visible
                                                  || voiceInvite.visible || serverMenu.inviteDialog.visible
                                                  || !!(screenShareLoader.item && screenShareLoader.item.visible)
    readonly property bool readingConversation: window.active && window.visible && window.visibility !== Window.Minimized
                                               && window.workspaceVisible && messages.visible && !window.socialVisible && !window.streamCoveringChat
                                               && !mediaViewer.visible && !switcher.visible && !searchPins.visible && !window.conversationModalOpen
    onReadingConversationChanged: discord.setReadingActive(readingConversation)
    Component.onCompleted: discord.setReadingActive(readingConversation)
    property string homePage: "friends"
    readonly property bool socialVisible: !discord.currentGuildId.length && homePage.length > 0
    property var drafts: ({})
    property string lastChannel: ""
    property string lastUserId: ""
    property string pendingSendChannel: ""
    property bool relayMode: false
    readonly property bool relayConversationActive: relayMode && !socialVisible && discord.currentChannelKind === "dm"
    readonly property string relayBinding: {
        const row = discord.channels.get(discord.channels.indexOf(discord.currentChannelId))
        return row.recipientId ? discord.currentUserId + ":" + row.recipientId : ""
    }
    function openRelayWindow() { relayPrivateWindow.show(); relayPrivateWindow.raise(); relayPrivateWindow.requestActivate() }
    Window {
        id: relayPrivateWindow
        title: "Relay · private text"; width: 860; height: 800; minimumWidth: 640; minimumHeight: 600
        transientParent: window
        RelayConversation { anchors.fill: parent }
    }
    Shortcut { sequence: "Ctrl+Alt+R"; onActivated: window.openRelayWindow() }
    property bool followMessages: true
    onFollowMessagesChanged: discord.setFollowingMessages(followMessages)
    property bool loadingOlder: false
    property string pendingReplyChannel: ""
    property string pendingReplyId: ""
    property string highlightedMessageId: ""
    property string replyJumpError: ""
    property string pendingStreamKey: ""
    property string pendingStreamTitle: ""
    property string watchedCameraChannel: ""
    property var streamPlayer: null
    property bool streamPoppedOut: false
    property bool streamExpanded: false
    property bool pendingStreamFullscreen: false
    property bool insertingMention: false
    property int mentionStart: -1
    property real voiceStartedAt: 0
    property int voiceElapsedSeconds: 0
    property real currentTimeMs: Date.now()
    readonly property bool streamEmbedded: streamPlayer !== null && !streamPoppedOut
    readonly property bool streamCoveringChat: streamEmbedded && streamExpanded
    readonly property bool directVoiceCall: voice.channelId.length > 0 && discord.isDirectCall(voice.currentGuildId)
    readonly property bool hiddenChannelSelected: discord.currentChannelKind === "hidden"
    readonly property bool forumSelected: discord.currentChannelKind === "forum"
    property string pendingSettingsPage: "account"
    property string pendingTranslationText: ""
    property string pendingTranslationChannel: ""
    property bool pendingTranslationInsert: false
    property string pendingServerGuild: ""
    property string pendingServerPage: "overview"
    readonly property real densityAdjustment: preferences.uiDensity === "compact" ? -4 : preferences.uiDensity === "spacious" ? 6 : 0
    property real channelWidth: preferences.channelSidebarWidth
    property real memberWidth: preferences.memberSidebarWidth
    readonly property bool showMembers: !socialVisible && !hiddenChannelSelected && preferences.memberListVisible && (discord.currentGuildId.length > 0 || discord.currentChannelKind === "dm")
    readonly property real paneBudget: Math.max(360, width - preferences.serverRailWidth - 380 - (showMembers ? 10 : 5))
    readonly property real effectiveChannelWidth: Math.max(180, Math.min(channelWidth, paneBudget - (showMembers ? 180 : 0)))
    readonly property real effectiveMemberWidth: Math.max(180, Math.min(memberWidth, paneBudget - effectiveChannelWidth))
    property bool canCompose: workspaceVisible && !relayConversationActive && !socialVisible && discord.currentChannelId.length > 0
                              && ["text", "dm", "thread", "voice"].indexOf(discord.currentChannelKind) !== -1
                              && (!forumTools.currentThread || forumTools.canReply)
    property bool canSend: canCompose && (discord.connected || discord.demoMode)

    function sendDraft() {
        const text = composer.text
        if (!canSend || discord.sendingMessage || uploads.busy || (!text.trim() && uploads.count === 0) || text.length > 2000) return
        pendingSendChannel = discord.currentChannelId
        if (uploads.count > 0) uploads.send(text)
        else discord.sendMessage(text)
    }
    function insertComposerText(text) {
        if (!canCompose) return
        const position = composer.cursorPosition
        composer.insert(position, text)
        composer.cursorPosition = position + text.length
        composer.forceActiveFocus()
    }
    function updateMentions() {
        if (insertingMention) return
        if (!canCompose || streamCoveringChat) { mentionPopup.close(); return }
        const prefix = composer.text.substring(0, composer.cursorPosition)
        const match = /(?:^|[\s(])@([^\s@<>]{0,64})$/.exec(prefix)
        if (!match) { mentionPopup.close(); return }
        mentionStart = prefix.length - match[1].length - 1
        mentionPopup.candidates = discord.mentionCandidates(match[1])
        if (!mentionPopup.candidates.length) { mentionPopup.close(); return }
        mentionList.currentIndex = 0
        const position = composerFrame.mapToItem(Overlay.overlay, 0, 0)
        mentionPopup.x = position.x
        mentionPopup.y = Math.max(24, position.y - mentionPopup.height - 6)
        mentionPopup.open()
    }
    function insertMention(index) {
        if (index < 0 || index >= mentionPopup.candidates.length || mentionStart < 0) return
        const user = mentionPopup.candidates[index]
        const token = "<@" + user.id + "> "
        insertingMention = true
        const start = mentionStart
        composer.remove(start, composer.cursorPosition)
        composer.insert(start, token)
        composer.cursorPosition = start + token.length
        mentionPopup.close()
        composer.forceActiveFocus()
        insertingMention = false
    }
    function clearReplyJump() {
        pendingReplyChannel = ""
        pendingReplyId = ""
        highlightedMessageId = ""
        replyJumpError = ""
        replyHighlight.stop()
    }
    function jumpToReply(channelId, messageId) {
        const channel = String(channelId || discord.currentChannelId)
        const target = String(messageId || "")
        const previousFollow = followMessages
        if (!/^[0-9]{1,22}$/.test(channel) || !/^[0-9]{1,22}$/.test(target)) return
        messageScroll.cancel()
        messages.cancelFlick()
        messageHistory.reset()
        loadingOlder = false
        if (channel !== discord.currentChannelId) selectChannel(channel)
        if (channel !== discord.currentChannelId) {
            clearReplyJump()
            replyJumpError = "This reply is outside the current conversation."
            followMessages = previousFollow
            return
        }
        pendingReplyChannel = channel
        pendingReplyId = target
        highlightedMessageId = ""
        replyJumpError = ""
        streamExpanded = false
        followMessages = false
        discord.jumpToMessage(channel, target)
    }
    function replyJumpReady(channelId, messageId) {
        if (channelId !== pendingReplyChannel || messageId !== pendingReplyId || channelId !== discord.currentChannelId) return
        Qt.callLater(window.finishReplyJump, channelId, messageId)
    }
    function finishReplyJump(channelId, messageId) {
        if (channelId !== pendingReplyChannel || messageId !== pendingReplyId || channelId !== discord.currentChannelId) return
        let target = -1
        for (let i = 0; i < discord.messages.count; ++i) {
            if (String(discord.messages.get(i).recordId) === messageId) { target = i; break }
        }
        if (target < 0) { replyJumpFailed(channelId, messageId, "The original message is no longer available."); return }
        messageScroll.cancel()
        messages.cancelFlick()
        messages.forceLayout()
        messages.positionViewAtIndex(target, ListView.Center)
        messages.forceLayout()
        messages.positionViewAtIndex(target, ListView.Center)
        followMessages = false
        highlightedMessageId = messageId
        pendingReplyChannel = ""
        pendingReplyId = ""
        replyJumpError = ""
        replyHighlight.restart()
        scheduleHistoryLoad()
    }
    function replyJumpFailed(channelId, messageId, reason) {
        if (channelId !== pendingReplyChannel || messageId !== pendingReplyId || channelId !== discord.currentChannelId) return
        pendingReplyChannel = ""
        pendingReplyId = ""
        replyJumpError = reason || "The original message is unavailable."
    }
    Timer { id: replyHighlight; interval: 1500; onTriggered: window.highlightedMessageId = "" }
    function positionAtEndIfFollowing() {
        if (window.followMessages && !window.loadingOlder && !discord.hasNewerMessages) messages.positionViewAtEnd()
    }
    function scheduleHistoryLoad() { messageHistory.schedule() }
    function loadVisibleHistory() { messageHistory.loadVisible() }
    HistoryViewport {
        id: messageHistory
        view: messages; records: discord.messages; backend: discord; wheel: messageScroll
        followTail: window.followMessages
        suspended: window.relayConversationActive || window.pendingReplyId.length > 0 || !window.workspaceVisible || window.socialVisible || window.streamCoveringChat || window.forumSelected
        scrollbarPressed: messageScrollbar.pressed
        onTailRequested: follow => window.followMessages = follow
        onOlderRequested: { window.loadingOlder = true; discord.loadOlderMessages() }
        onNewerRequested: discord.loadNewerMessages()
    }
    ReadReceiptTracker {
        view: messages; records: discord.messages; backend: discord
        active: window.readingConversation && !window.pendingReplyId.length && !window.loadingOlder
                && !window.forumSelected && !window.hiddenChannelSelected
        following: window.followMessages
    }
    function showHome(page) {
        drafts[discord.currentChannelId] = composer.text
        streamExpanded = false
        clearReplyJump()
        discord.selectHome()
        discord.previewMessageRequest("")
        homePage = page || "friends"
    }
    function openSocialDm(userId) {
        discord.openDirectMessage(userId)
    }
    ServerChoicesDialog { id: serverChoices; service: messageReferences }
    Connections {
        target: messageReferences
        function onPanelRequested() { serverChoices.open() }
        function onChannelRequested(id) {
            drafts[discord.currentChannelId] = composer.text
            homePage = ""
            streamExpanded = false
            clearReplyJump()
            discord.activateChannel(id)
            followMessages = true
        }
        function onChanged() { if (!messageReferences.guildId.length && !messageReferences.error.length) serverChoices.close() }
    }
    function focusDmComposer() {
        Qt.callLater(() => {
            if (discord.currentChannelKind === "dm" && window.canCompose && composerFrame.visible)
                composer.forceActiveFocus()
        })
    }
    function selectChannel(id) {
        homePage = ""
        streamExpanded = false
        if (id === discord.currentChannelId) { focusDmComposer(); return }
        clearReplyJump()
        drafts[discord.currentChannelId] = composer.text
        discord.selectChannel(id)
        followMessages = true
    }
    function syncChannel() {
        if (lastUserId !== discord.currentUserId) {
            stopWatching()
            clearReplyJump()
            serverChoices.close(); personMenu.close(); channelMenu.close(); serverMenu.close(); dmContext.close()
            channelEditor.close(); actionConfirmation.close(); voiceInvite.close(); userCard.close(); mediaViewer.close(); searchPins.close(); guildProfileDialog.close(); ownProfileMenu.close()
            // Recreate account-scoped settings so drafts and revealed fields cannot cross accounts.
            settingsLoader.active = false
            serverSettingsLoader.active = false
            pendingSettingsPage = "account"
            pendingServerGuild = ""
            pendingServerPage = "overview"
            if (translationLoader.item) translationLoader.item.close()
            pendingTranslationChannel = ""
            drafts = ({})
            composer.clear()
            lastChannel = ""
            pendingSendChannel = ""
            lastUserId = discord.currentUserId
            homePage = "friends"
        }
        if (lastChannel !== discord.currentChannelId) {
            if (discord.currentChannelId.length) homePage = ""
            // Selecting another conversation keeps playback in a floating view.
            streamExpanded = false
            drafts[lastChannel] = composer.text
            lastChannel = discord.currentChannelId
            composer.text = drafts[lastChannel] || ""
            if (pendingReplyChannel.length && pendingReplyChannel !== lastChannel) clearReplyJump()
            followMessages = !(pendingReplyChannel === lastChannel && pendingReplyId.length)
            Qt.callLater(window.positionAtEndIfFollowing)
            focusDmComposer()
        }
        if (!discord.currentGuildId.length && !discord.currentChannelId.length && !homePage.length) homePage = "friends"
        if (loadingOlder && !discord.loadingMessages) {
            loadingOlder = false
            if (followMessages) Qt.callLater(window.positionAtEndIfFollowing)
        }
        scheduleHistoryLoad()
    }
    function joinChannel(id, name) {
        if (discord.connected && !discord.demoMode && voice.available && voice.channelId !== id)
            voice.join(discord.currentGuildId, id, name, discord.currentUserId)
    }
    function openSwitcher() {
        if (workspaceVisible) { switcher.open(); channelSearch.forceActiveFocus() }
    }
    function openAddon(id) {
        if (applicationClosing) return
        if (addons.activeId === id && addonSettings.item) addonSettings.item.open()
        else addons.openSettings(id)
    }
    function showStudio() { openAddon("org.relay.obs") }
    function showSettings(page) {
        if (applicationClosing) return
        pendingSettingsPage = page || "account"
        if (settingsLoader.item) settingsLoader.item.openPage(pendingSettingsPage)
        else settingsLoader.active = true
    }
    function showAudio() { showSettings("voice") }
    function showAddons() { showSettings("addons") }
    function showAppearance() { showSettings("appearance") }
    function showKeybindings() { showSettings("keybinds") }
    function showShortcuts() { showKeybindings() }
    function showServerSettings(guildId, page) {
        if (applicationClosing) return
        pendingServerGuild = guildId; pendingServerPage = page || "overview"
        if (serverSettingsLoader.item) serverSettingsLoader.item.openGuild(guildId, pendingServerPage)
        else serverSettingsLoader.active = true
    }
    function openChannelContext(channel) {
        if (channel.kind === "dm") {
            if (channel.recipientId) personMenu.openForUser(channel.recipientId, {name:channel.name,avatarUrl:channel.avatarUrl}, "")
            else { dmContext.channelId = channel.recordId; dmContext.popup() }
        } else channelMenu.openForChannel(discord.currentGuildId, channel)
    }
    function openDmContext(channelId) {
        const index = discord.dms.indexOf(channelId)
        if (index >= 0) openChannelContext(discord.dms.get(index))
    }
    function showSoundboard() { soundboardDialog.openFor(voice.connected && !window.directVoiceCall ? voice.currentGuildId : discord.currentGuildId) }
    function showScreenShare() { if (applicationClosing) return; if (screenShareLoader.item) screenShareLoader.item.open(); else screenShareLoader.active = true }
    function selectScreenShareSource(id) { showScreenShare(); if (screenShareLoader.item) screenShareLoader.item.selectSource("addon:" + id) }
    function translateDraft() {
        showTranslation(composer.text, true)
    }
    function showTranslation(text, allowInsert) {
        pendingTranslationText = text
        pendingTranslationInsert = allowInsert
        pendingTranslationChannel = allowInsert ? discord.currentChannelId : ""
        if (translationLoader.item) translationLoader.item.openText(pendingTranslationText, allowInsert)
        else translationLoader.active = true
    }
    function voiceGuildName() {
        if (voice.currentGuildId === voice.channelId && voice.channelId.length) return "Direct call"
        for (let i = 0; i < discord.guilds.count; ++i) {
            const guild = discord.guilds.get(i)
            if (guild.recordId === voice.currentGuildId) return guild.name
        }
        return ""
    }
    function voiceDuration() {
        const seconds = voiceElapsedSeconds % 60
        const minutes = Math.floor(voiceElapsedSeconds / 60) % 60
        const hours = Math.floor(voiceElapsedSeconds / 3600)
        return (hours ? hours + ":" + String(minutes).padStart(2, "0") : String(minutes)) + ":" + String(seconds).padStart(2, "0")
    }
    function channelDuration(startedAt) {
        const elapsed = Math.max(0, Math.floor((currentTimeMs - Number(startedAt)) / 1000))
        const seconds = elapsed % 60, minutes = Math.floor(elapsed / 60) % 60, hours = Math.floor(elapsed / 3600)
        return (hours ? hours + ":" + String(minutes).padStart(2, "0") : String(minutes)) + ":" + String(seconds).padStart(2, "0")
    }
    function directStreamPeer(userId) {
        const contacts = relayPeers.contacts
        const streams = relayStreams.streams
        const binding = discord.currentUserId + ":" + userId
        for (let i = 0; i < contacts.length; ++i) {
            if (contacts[i].binding !== binding || !contacts[i].active) continue
            for (let j = 0; j < streams.length; ++j) if (streams[j].peer === contacts[i].key) return contacts[i].key
        }
        return ""
    }
    function watchStream(key, label) {
        watchedCameraChannel = ""
        streamExpanded = false
        showStream()
        streamPlayer.beginWatch(key, label, stream)
    }
    function watchCamera(userId, label) {
        if (!camera.available || !voice.connected || userId === discord.currentUserId) return
        const members = voiceRoster.members(voice.channelId)
        if (!members.some(member => member.recordId === userId && !!member.camera)) return
        watchedCameraChannel = voice.channelId
        streamExpanded = false
        showStream()
        streamPlayer.beginWatch(userId, label, camera)
    }
    function closeStoppedCamera() {
        if (streamPlayer && streamPlayer.cameraMode && !camera.active && !camera.error.length) stopWatching()
    }
    function showStream() {
        if (applicationClosing) return
        if (!streamPlayer) streamPlayer = playerComponent.createObject(embeddedStreamHost.contentItem)
        if (streamPoppedOut) returnStreamToApp(false)
    }
    function popOutStream(fullscreen) {
        if (applicationClosing) return
        if (!streamPlayer) return
        pendingStreamFullscreen = !!fullscreen
        streamPoppedOut = true
        if (streamWindowLoader.item) streamWindowLoader.item.presentPlayer(streamPlayer, pendingStreamFullscreen)
        else streamWindowLoader.active = true
    }
    function returnStreamToApp(activate) {
        if (applicationClosing) return
        if (streamPlayer) { streamPlayer.parent = embeddedStreamHost.contentItem; streamPlayer.poppedOut = false; streamPlayer.showControls() }
        streamPoppedOut = false
        if (streamWindowLoader.item) streamWindowLoader.item.hide()
        if (activate) { window.show(); window.raise(); window.requestActivate() }
    }
    function stopWatching() {
        stream.stop()
        stream.videoSink = null
        camera.stop()
        camera.videoSink = null
        watchedCameraChannel = ""
        if (streamWindowLoader.item) streamWindowLoader.item.hide()
        if (streamPlayer) { streamPlayer.destroy(); streamPlayer = null }
        streamPoppedOut = false
        streamExpanded = false
    }
    function showImage(source, originalUrl, title) {
        mediaViewer.imageSource = source
        mediaViewer.originalUrl = originalUrl
        mediaViewer.imageTitle = title
        mediaViewer.open()
    }
    function signOut() {
        if (accountSettings.mutating || (typeof accounts !== "undefined" && accounts.sessionChangeBlocked)) return
        clearReplyJump(); drafts = ({}); composer.clear()
        if (!discord.demoMode && typeof accounts !== "undefined") accounts.signOutCurrent()
        else { stopWatching(); broadcast.stop(); voice.leave(); auth.cancel(); discord.logout() }
    }

    Connections {
        target: camera
        function onChanged() { Qt.callLater(window.closeStoppedCamera) }
    }
    Connections {
        target: voice
        function onChanged() {
            if (window.streamPlayer && window.streamPlayer.cameraMode && (!voice.connected || voice.channelId !== window.watchedCameraChannel)) window.stopWatching()
            if (voice.connected && !window.voiceStartedAt) window.voiceStartedAt = Date.now()
            else if (!voice.connected) { window.voiceStartedAt = 0; window.voiceElapsedSeconds = 0 }
        }
    }
    Timer { interval: 1000; running: window.workspaceVisible; repeat: true; onTriggered: { window.currentTimeMs = Date.now(); if (voice.connected) { if (!window.voiceStartedAt) window.voiceStartedAt = Date.now(); window.voiceElapsedSeconds = Math.floor((Date.now() - window.voiceStartedAt) / 1000) } } }
    Connections {
        target: preferences
        function onChanged() {
            if (!channelDivider.dragging) window.channelWidth = preferences.channelSidebarWidth
            if (!memberDivider.dragging) window.memberWidth = preferences.memberSidebarWidth
        }
    }
    Connections {
        target: discord
        function onStateChanged() { window.syncChannel() }
        function onMessageJumpReady(channelId, messageId) { window.replyJumpReady(channelId, messageId) }
        function onMessageJumpFailed(channelId, messageId, reason) { window.replyJumpFailed(channelId, messageId, reason) }
        function onMessageSent(content) {
            if (window.pendingReplyId.length) {
                window.clearReplyJump()
                discord.jumpToPresent()
            }
            if (discord.currentChannelId === pendingSendChannel && composer.text === content) composer.clear()
            if (drafts[pendingSendChannel] === content) drafts[pendingSendChannel] = ""
            window.followMessages = true
            Qt.callLater(window.positionAtEndIfFollowing)
        }
    }
    Connections {
        target: addons
        function onActiveChanged() {
            if (String(addons.activeSettingsUrl).length)
                addonSettings.setSource(addons.activeSettingsUrl, { service: addons.activeService })
            else addonSettings.source = ""
        }
    }
    function voiceShortcutUses(sequence) {
        return [shortcutSettings.muteShortcut, shortcutSettings.deafenShortcut, shortcutSettings.disconnectShortcut].indexOf(sequence) !== -1
    }
    function navigateChannels(direction) {
        const rows = []
        for (let i=0; i<discord.channels.count; ++i) { const channel=discord.channels.get(i); if (channel.kind !== "category" && !channel.hidden) rows.push(channel) }
        let index=rows.findIndex(channel => channel.recordId === discord.currentChannelId)
        index=Math.max(0,Math.min(rows.length-1,index+direction))
        if (rows[index]) window.selectChannel(rows[index].recordId)
    }
    function navigateServers(direction) {
        const ids=[]
        for (let i=0; i<serverLayout.count; ++i) { const row=serverLayout.get(i); if (row.kind === "guild") ids.push(row.guildId); else if (!row.expanded) for (const guild of row.children) ids.push(guild.recordId) }
        const index=Math.max(0,Math.min(ids.length-1,ids.indexOf(discord.currentGuildId)+direction))
        if (ids[index]) discord.selectGuild(ids[index])
    }
    Shortcut { sequence: "Alt+Up"; enabled: !window.voiceShortcutUses("Alt+Up"); onActivated: window.navigateChannels(-1) }
    Shortcut { sequence: "Alt+Down"; enabled: !window.voiceShortcutUses("Alt+Down"); onActivated: window.navigateChannels(1) }
    Shortcut { sequence: "Ctrl+Alt+Up"; enabled: !window.voiceShortcutUses("Ctrl+Alt+Up"); onActivated: window.navigateServers(-1) }
    Shortcut { sequence: "Ctrl+Alt+Down"; enabled: !window.voiceShortcutUses("Ctrl+Alt+Down"); onActivated: window.navigateServers(1) }
    Shortcut { sequence: "Ctrl+F"; enabled: !window.voiceShortcutUses("Ctrl+F") && window.canCompose; onActivated: messageSearch.forceActiveFocus() }
    Shortcut { sequence: "Ctrl+P"; enabled: !window.voiceShortcutUses("Ctrl+P") && window.canCompose && !discord.demoMode; onActivated: searchPins.openPins(discord.currentChannelId) }
    Shortcut { sequence: "Ctrl+U"; enabled: !window.voiceShortcutUses("Ctrl+U"); onActivated: preferences.memberListVisible=!preferences.memberListVisible }
    Shortcut { sequence: "Ctrl+E"; enabled: !window.voiceShortcutUses("Ctrl+E") && window.canCompose; onActivated: emojiPicker.visible ? emojiPicker.close() : emojiPicker.showNear(composerFrame) }
    Shortcut { sequence: "Ctrl+G"; enabled: !window.voiceShortcutUses("Ctrl+G") && window.canCompose; onActivated: gifPicker.visible ? gifPicker.close() : gifPicker.showNear(composerFrame) }
    Shortcut { sequence: "Ctrl+Shift+U"; enabled: !window.voiceShortcutUses("Ctrl+Shift+U") && window.canCompose && !uploads.busy; onActivated: attachmentPicker.open() }
    Shortcut { sequence: "Ctrl+Shift+B"; enabled: !window.voiceShortcutUses("Ctrl+Shift+B") && voice.connected; onActivated: window.showSoundboard() }
    Shortcut { sequence: "Ctrl+K"; enabled: !window.voiceShortcutUses("Ctrl+K"); onActivated: window.openSwitcher() }
    Shortcut { sequence: "Ctrl+,"; enabled: !window.voiceShortcutUses("Ctrl+,"); onActivated: window.showAudio() }
    Shortcut { sequence: shortcutSettings.muteShortcut; enabled: typeof globalShortcuts === "undefined" || !(globalShortcuts.active && globalShortcuts.isBound("toggle-mute")); context: Qt.ApplicationShortcut; onActivated: controls.execute("toggle-mute") }
    Shortcut { sequence: shortcutSettings.deafenShortcut; enabled: typeof globalShortcuts === "undefined" || !(globalShortcuts.active && globalShortcuts.isBound("toggle-deafen")); context: Qt.ApplicationShortcut; onActivated: controls.execute("toggle-deafen") }
    Shortcut { sequence: shortcutSettings.disconnectShortcut; enabled: typeof globalShortcuts === "undefined" || !(globalShortcuts.active && globalShortcuts.isBound("disconnect-voice")); context: Qt.ApplicationShortcut; onActivated: controls.execute("disconnect-voice") }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true
            height: 23
            color: (preferences.highContrast ? "#0b0b0e" : Theme.rail)
            Text { anchors.left: parent.left; anchors.leftMargin: 12; anchors.verticalCenter: parent.verticalCenter; text: "Relay"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(11 * preferences.fontScale); font.weight: Font.DemiBold }
        }
        Rectangle {
            Layout.fillWidth: true
            visible: discord.error.length > 0 || channelTools.actionError.length > 0 || memberActions.error.length > 0 || guildManagement.error.length > 0 || (!window.socialVisible && (contextActions.error.length > 0 || discord.socialError.length > 0 || discord.readReceiptError.length > 0))
            implicitHeight: errorRow.implicitHeight + 12
            color: Theme.errorSurface
            RowLayout {
                id: errorRow
                anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 15; anchors.verticalCenter: parent.verticalCenter
                Text { Layout.fillWidth: true; text: discord.error || channelTools.actionError || memberActions.error || guildManagement.error || contextActions.error || discord.socialError || discord.readReceiptError; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
                ActionButton { iconName: "close"; hint: "Dismiss error"; quiet: true; implicitHeight: 26; onClicked: { if (discord.error.length) discord.clearError(); else if (channelTools.actionError.length) channelTools.clearActionError(); else if (memberActions.error.length) memberActions.clearError(); else if (guildManagement.error.length) guildManagement.clearError(); else if (contextActions.error.length) contextActions.clearError(); else if (discord.socialError.length) discord.clearSocialFeedback(); else discord.clearReadReceiptError() } }
            }
        }

        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: !window.workspaceVisible
            Rectangle { anchors.fill: parent; color: Theme.surface }
            Flickable {
                anchors.fill: parent
                clip: true
                contentHeight: Math.max(height, signInPanel.height + 64)
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
            Rectangle {
                id: signInPanel
                width: Math.min(750, parent.width - 32)
                height: Math.max(360, signInContent.implicitHeight + 64)
                x: (parent.width - width) / 2
                y: Math.max(32, (parent.height - height) / 2)
                color: (preferences.highContrast ? "#101014" : Theme.canvas)
                radius: 6
                RowLayout {
                    id: signInContent
                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 32; anchors.verticalCenter: parent.verticalCenter
                    spacing: 44
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 17
                        ActionButton { text: "Open Relay private text"; onClicked: window.openRelayWindow() }
                        Text { text: "Welcome back"; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(25 * preferences.fontScale); font.weight: Font.DemiBold }
                        Text { text: "Sign in to see your servers and messages."; Layout.fillWidth: true; color: (preferences.highContrast ? "#e4e4e8" : Theme.secondary); font.pixelSize: Math.round(14 * preferences.fontScale); wrapMode: Text.WordWrap }
                        Text {
                            Layout.fillWidth: true
                            text: discord.connecting ? discord.status : auth.status
                            textFormat: Text.PlainText
                            visible: text.length > 0 && (auth.active || auth.completed || discord.connecting || bitwardenOptions.checked)
                            color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            text: auth.error
                            textFormat: Text.PlainText
                            visible: text.length > 0
                            color: Theme.danger; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: !auth.active && !discord.connecting && text.length > 0
                            text: accounts.error || (typeof credentials !== "undefined" ? credentials.error || credentials.status : "")
                            textFormat: Text.PlainText; wrapMode: Text.WordWrap
                            color: typeof credentials !== "undefined" && credentials.error ? Theme.warning : (preferences.highContrast ? "#e4e4e8" : Theme.secondary)
                            font.pixelSize: Math.round(12 * preferences.fontScale)
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: !auth.active && !discord.connecting
                            TextField { id: signInEmail; objectName: "signInEmail"; Layout.fillWidth: true; visible: !auth.mfaRequired; placeholderText: "Email or phone number"; Accessible.name: placeholderText; inputMethodHints: Qt.ImhEmailCharactersOnly }
                            TextField { id: signInPassword; objectName: "signInPassword"; Layout.fillWidth: true; visible: !auth.mfaRequired; placeholderText: "Password"; Accessible.name: placeholderText; echoMode: TextInput.Password; onAccepted: passwordSignIn.clicked() }
                            ActionButton {
                                id: passwordSignIn; objectName: "passwordSignIn"; Layout.fillWidth: true; visible: !auth.mfaRequired
                                text: "Sign in with email"; primary: true
                                onClicked: { credentials.cancelRestore(); auth.signInPassword(signInEmail.text, signInPassword.text); signInPassword.clear() }
                            }
                            TextField { id: signInCode; objectName: "signInCode"; Layout.fillWidth: true; visible: auth.mfaRequired; placeholderText: "Authenticator or backup code"; Accessible.name: placeholderText; echoMode: TextInput.Password; onAccepted: codeSignIn.clicked() }
                            CheckBox { id: useBackupCode; visible: auth.mfaRequired; text: "Use a backup code" }
                            ActionButton { id: codeSignIn; Layout.fillWidth: true; visible: auth.mfaRequired; text: "Verify code"; primary: true; onClicked: { auth.submitCode(signInCode.text, useBackupCode.checked); signInCode.clear() } }
                            ActionButton { visible: auth.mfaRequired; text: "Back to sign-in"; quiet: true; onClicked: { auth.cancel(); signInCode.clear() } }
                            CheckBox { id: bitwardenOptions; visible: !auth.mfaRequired; text: "Sign in using Bitwarden" }
                            CheckBox { id: bitwardenSetup; visible: bitwardenOptions.checked && !auth.mfaRequired; text: "Connect or change vault" }
                            ColumnLayout {
                                Layout.fillWidth: true; visible: bitwardenOptions.checked && bitwardenSetup.checked && !auth.mfaRequired
                                TextField { id: bitwardenServer; objectName: "bitwardenServer"; Layout.fillWidth: true; text: auth.bitwardenServer; placeholderText: "Server URL (cloud or self-hosted)"; Accessible.name: placeholderText }
                                Text { Layout.fillWidth: true; text: "Use https://vault.bitwarden.com, https://vault.bitwarden.eu, or your self-hosted HTTPS address."; color: Theme.secondary; wrapMode: Text.WordWrap; font.pixelSize: 12 }
                                TextField { id: bitwardenEmail; Layout.fillWidth: true; placeholderText: "Bitwarden email"; Accessible.name: placeholderText }
                                TextField { id: bitwardenSetupPassword; Layout.fillWidth: true; placeholderText: "Bitwarden master password"; Accessible.name: placeholderText; echoMode: TextInput.Password }
                                ComboBox { id: bitwardenMfaMethod; Layout.fillWidth: true; model: ["Vault 2FA: Authenticator", "Vault 2FA: Email", "Vault 2FA: YubiKey OTP"] }
                                TextField { id: bitwardenSetupCode; Layout.fillWidth: true; placeholderText: "Vault two-factor code (if enabled)"; Accessible.name: placeholderText; echoMode: TextInput.Password }
                                ActionButton { Layout.fillWidth: true; text: "Connect vault"; onClicked: { credentials.cancelRestore(); auth.connectBitwarden(bitwardenServer.text, bitwardenEmail.text, bitwardenSetupPassword.text, bitwardenSetupCode.text, [0, 1, 3][bitwardenMfaMethod.currentIndex]); bitwardenSetupPassword.clear(); bitwardenSetupCode.clear() } }
                                ActionButton { Layout.fillWidth: true; text: "Disconnect vault from Relay"; quiet: true; onClicked: auth.disconnectBitwarden() }
                            }
                            TextField { id: bitwardenItem; objectName: "bitwardenItem"; Layout.fillWidth: true; visible: bitwardenOptions.checked && !auth.mfaRequired; text: auth.bitwardenItem; placeholderText: "Discord login item ID"; Accessible.name: placeholderText }
                            CheckBox { visible: bitwardenOptions.checked && !auth.mfaRequired; text: "Remember this Discord login securely for automatic sign-in"; checked: auth.rememberLogin; onToggled: auth.rememberLogin = checked }
                            TextField { id: bitwardenPassword; objectName: "bitwardenPassword"; Layout.fillWidth: true; visible: bitwardenOptions.checked && !auth.mfaRequired; placeholderText: "Vault master password (if locked)"; Accessible.name: placeholderText; echoMode: TextInput.Password }
                            Text { Layout.fillWidth: true; visible: bitwardenOptions.checked && !auth.mfaRequired; text: "Optional: connect a Bitwarden cloud or self-hosted vault above. Relay reads the selected Discord entry and its authenticator code. Your Discord session is remembered in the desktop wallet."; color: Theme.secondary; wrapMode: Text.WordWrap; font.pixelSize: 12 }
                            ActionButton { Layout.fillWidth: true; visible: bitwardenOptions.checked && !auth.mfaRequired; text: "Sign in from Bitwarden"; onClicked: { credentials.cancelRestore(); auth.signInBitwarden(bitwardenItem.text, bitwardenPassword.text); bitwardenPassword.clear(); signInPassword.clear() } }
                        }
                        ActionButton {
                            Layout.fillWidth: true
                            Layout.topMargin: 9
                            primary: true
                            text: "Sign in with QR code"
                            enabled: !auth.active && !discord.connecting
                            onClicked: { if (typeof credentials !== "undefined") credentials.cancelRestore(); signInPassword.clear(); bitwardenPassword.clear(); signInCode.clear(); auth.start() }
                        }
                        ActionButton {
                            visible: auth.active || auth.mfaRequired
                            Layout.fillWidth: true
                            text: "Cancel"
                            quiet: true
                            onClicked: auth.cancel()
                        }
                        ActionButton {
                            text: "Preview interface"
                            quiet: true
                            Layout.alignment: Qt.AlignLeft
                            leftPadding: 0; rightPadding: 0
                            enabled: !discord.connecting
                            onClicked: { if (typeof credentials !== "undefined") credentials.cancelRestore(); auth.cancel(); discord.loadDemo() }
                        }
                    }
                    ColumnLayout {
                        Layout.preferredWidth: 224
                        spacing: 15
                        Rectangle {
                            Layout.alignment: Qt.AlignHCenter
                            width: 200; height: 200; radius: 6
                            color: auth.qrReady ? "#ffffff" : (preferences.highContrast ? "#151519" : Theme.sidebar)
                            Image {
                                anchors.fill: parent; anchors.margins: 10
                                source: auth.qrImage
                                fillMode: Image.PreserveAspectFit
                                smooth: false
                                visible: auth.qrReady
                            }
                            AppIcon { anchors.centerIn: parent; width: 44; height: 44; name: "chat"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); visible: !auth.qrReady && !auth.active }
                            BusyIndicator { anchors.centerIn: parent; width: 42; height: 42; running: auth.active && !auth.qrReady; visible: running }
                        }
                        Text { Layout.fillWidth: true; text: "Log in with QR code"; horizontalAlignment: Text.AlignHCenter; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(16 * preferences.fontScale); font.weight: Font.DemiBold }
                        Text {
                            Layout.fillWidth: true
                            text: auth.scannedUserName.length ? "Approve sign-in on your phone." : "Scan the code with the Discord app on your phone."
                            wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter
                            color: (preferences.highContrast ? "#e4e4e8" : Theme.secondary); font.pixelSize: Math.round(13 * preferences.fontScale)
                        }
                    }
                }
            }
        }

        }

        RowLayout {
            visible: window.workspaceVisible
            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: 0
            ServerRail {
                id: serverRail
                Layout.fillHeight: true
                Layout.preferredWidth: preferences.serverRailWidth
                Layout.minimumWidth: preferences.serverRailWidth
                Layout.maximumWidth: preferences.serverRailWidth
                layoutModel: serverLayout
                unreadDmModel: discord.unreadDms
                currentGuildId: discord.currentGuildId
                currentChannelId: discord.currentChannelId
                onDmActivated: id => {
                    guildHover.dismiss(guildHover.guildId)
                    window.homePage = ""
                    window.streamExpanded = false
                    window.clearReplyJump()
                    window.drafts[discord.currentChannelId] = composer.text
                    discord.activateChannel(id)
                    window.focusDmComposer()
                    messageHistory.reset()
                    messageScroll.cancel()
                    messages.cancelFlick()
                    window.followMessages = true
                    if (discord.hasNewerMessages) discord.jumpToPresent()
                    else Qt.callLater(window.positionAtEndIfFollowing)
                }
                onHomeActivated: window.showHome("friends")
                onDmContextRequested: id => window.openDmContext(id)
                onGuildActivated: id => { guildHover.dismiss(id); discord.selectGuild(id) }
                onGuildContextRequested: (id, name) => serverMenu.openForGuild(id, name)
                onHoverPrepared: (id, anchor) => guildHover.prepare(id, anchor)
                onHoverDismissed: id => guildHover.dismiss(id)
            }

            Rectangle {
                Layout.fillHeight: true; Layout.preferredWidth: window.effectiveChannelWidth
                Layout.minimumWidth: window.effectiveChannelWidth; Layout.maximumWidth: window.effectiveChannelWidth
                color: (preferences.highContrast ? "#151519" : Theme.sidebar)
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true; height: 48; color: (preferences.highContrast ? "#151519" : Theme.sidebar)
                        Text { visible: discord.currentGuildId.length > 0; anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 38; verticalAlignment: Text.AlignVCenter; text: discord.currentGuildName; textFormat: Text.PlainText; elide: Text.ElideRight; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(15 * preferences.fontScale); font.weight: Font.DemiBold }
                        Button {
                            visible: !discord.currentGuildId.length
                            anchors.fill: parent; anchors.margins: 8; padding: 7; hoverEnabled: true
                            Accessible.name: "Find or start a conversation"
                            contentItem: Text { text: "Find or start a conversation"; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
                            background: Rectangle { radius: 5; color: Theme.rail; border.width: parent.activeFocus ? 1 : 0; border.color: Theme.focus }
                            onClicked: window.openSwitcher()
                        }
                        AppIcon { visible: discord.currentGuildId.length > 0; anchors.right: parent.right; anchors.rightMargin: 14; anchors.verticalCenter: parent.verticalCenter; width: 14; height: 14; name: "down"; color: (preferences.highContrast ? "#e4e4e8" : Theme.secondary) }
                        MouseArea { anchors.fill: parent; enabled: discord.currentGuildId.length > 0; acceptedButtons: Qt.LeftButton | Qt.RightButton; cursorShape: Qt.PointingHandCursor; onClicked: serverMenu.openForGuild(discord.currentGuildId, discord.currentGuildName) }
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.surface }
                    }
                    ColumnLayout {
                        visible: !discord.currentGuildId.length
                        Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 8; Layout.topMargin: 8
                        spacing: 3
                        ActionButton { Layout.fillWidth: true; text: "Relay private text"; onClicked: window.openRelayWindow() }
                        Repeater {
                            model: [{page:"friends",label:"Friends",icon:"members",badge:discord.pendingFriends.length}, {page:"requests",label:"Message Requests",icon:"mail",badge:discord.messageRequests.length}]
                            delegate: Button {
                                id: homeNavigation
                                required property var modelData
                                Layout.fillWidth: true; implicitHeight: Math.max(40, 30 * preferences.fontScale)
                                leftPadding: 10; rightPadding: 10; hoverEnabled: true
                                Accessible.name: modelData.label + (modelData.badge ? ", " + modelData.badge + " pending" : "")
                                background: Rectangle { radius: 5; color: window.socialVisible && window.homePage === homeNavigation.modelData.page ? Theme.selected : homeNavigation.hovered ? Theme.hover : "transparent"; border.width: homeNavigation.activeFocus ? 1 : 0; border.color: Theme.focus }
                                contentItem: RowLayout {
                                    spacing: 10
                                    SocialIcon { name: homeNavigation.modelData.icon; Layout.preferredWidth: 21; Layout.preferredHeight: 21; color: window.socialVisible && window.homePage === homeNavigation.modelData.page ? Theme.text : Theme.muted }
                                    Text { Layout.fillWidth: true; text: homeNavigation.modelData.label; color: window.socialVisible && window.homePage === homeNavigation.modelData.page ? Theme.text : Theme.secondary; font.pixelSize: Math.round(14 * preferences.fontScale); elide: Text.ElideRight }
                                    Rectangle {
                                        visible: homeNavigation.modelData.badge > 0
                                        implicitWidth: Math.max(18, badgeText.implicitWidth + 10); implicitHeight: 18; radius: 9; color: Theme.danger
                                        Text { id: badgeText; anchors.centerIn: parent; text: homeNavigation.modelData.badge > 99 ? "99+" : String(homeNavigation.modelData.badge); color: Theme.rail; font.pixelSize: Math.round(10 * preferences.fontScale); font.weight: Font.Bold }
                                    }
                                }
                                onClicked: window.showHome(modelData.page)
                            }
                        }
                        Rectangle { Layout.fillWidth: true; Layout.topMargin: 10; Layout.bottomMargin: 4; implicitHeight: 1; color: Theme.border; opacity: 0.5 }
                        RowLayout {
                            Layout.fillWidth: true; Layout.leftMargin: 8; Layout.rightMargin: 3
                            Text { Layout.fillWidth: true; text: "Direct Messages"; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); font.weight: Font.DemiBold }
                            ActionButton { iconName: "userPlus"; hint: "Find or start a conversation"; quiet: true; implicitWidth: 25; implicitHeight: 25; padding: 4; onClicked: window.openSwitcher() }
                        }
                    }
                    ListView {
                        boundsBehavior: Flickable.StopAtBounds
                        boundsMovement: Flickable.StopAtBounds
                        id: channelList
                        Layout.fillWidth: true; Layout.fillHeight: true
                        Layout.leftMargin: 8; Layout.rightMargin: 8; Layout.topMargin: 8
                        model: discord.channels
                        clip: true; reuseItems: true; spacing: 2
                        delegate: Item {
                            id: channelDelegate
                            required property var model
                            required property int index
                            property bool isVoice: model.kind === "voice" || model.kind === "stage"
                            property bool isDm: model.kind === "dm"
                            property bool isCategory: model.kind === "category"
                            property real rowHeight: isDm ? Math.max(50 + window.densityAdjustment, 40 * preferences.fontScale) : Math.max(34 + window.densityAdjustment, 25 * preferences.fontScale)
                            property real memberRowHeight: Math.max(29 + window.densityAdjustment, 22 * preferences.fontScale)
                            property bool selected: (!window.socialVisible && (model.recordId === discord.currentChannelId || (model.kind === "forum" && forumTools.currentThread && model.recordId === forumTools.channelId))) || (isVoice && model.recordId === voice.channelId)
                            property var membersModel: { const revision = voiceRoster.revision; return isVoice && !model.hidden ? voiceRoster.forChannel(model.recordId) : null }
                            width: channelList.width
                            height: (isCategory ? 33 : rowHeight) + (membersModel ? membersModel.count : 0) * memberRowHeight
                            HoverHandler { id: channelHover }
                            Text { visible: channelDelegate.isCategory; anchors.left: parent.left; anchors.right: parent.right; anchors.leftMargin: 7; y: 15; text: String(model.name || "").toUpperCase(); textFormat: Text.PlainText; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(10 * preferences.fontScale); font.weight: Font.DemiBold; elide: Text.ElideRight }
                            MouseArea { visible: channelDelegate.isCategory; width: parent.width; height: 33; acceptedButtons: model.hidden ? Qt.LeftButton | Qt.RightButton : Qt.RightButton; onClicked: mouse => mouse.button === Qt.LeftButton ? window.selectChannel(model.recordId) : window.openChannelContext(discord.channels.get(channelDelegate.index)) }
                            Button {
                                id: channelButton
                                visible: !channelDelegate.isCategory
                                width: parent.width; height: channelDelegate.rowHeight
                                hoverEnabled: true
                                enabled: model.kind !== "stage" || !!model.hidden
                                opacity: !enabled || model.hidden ? 0.55 : model.conversationMuted ? 0.6 : 1
                                background: Rectangle { radius: 4; color: channelDelegate.selected ? Theme.selected : channelButton.hovered ? Theme.hover : "transparent" }
                                contentItem: RowLayout {
                                    spacing: 8
                                    Item {
                                        visible: !channelDelegate.isDm
                                        Layout.preferredWidth: 20; Layout.preferredHeight: 20
                                        AppIcon { anchors.fill: parent; name: channelDelegate.isVoice ? "voice" : model.kind === "forum" ? "chat" : "hash"; color: channelDelegate.isVoice && model.recordId === voice.channelId ? Theme.success : (preferences.highContrast ? "#c7c7cf" : Theme.muted) }
                                        Rectangle {
                                            visible: !!model.locked || !!model.hidden
                                            anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.rightMargin: -3; anchors.bottomMargin: -2
                                            width: 11; height: 11; radius: 2; color: channelDelegate.selected ? Theme.selected : (preferences.highContrast ? "#151519" : Theme.sidebar)
                                            AppIcon { anchors.fill: parent; name: "lock"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted) }
                                        }
                                    }
                                    Rectangle {
                                        visible: channelDelegate.isDm
                                        width: 32; height: 32; radius: 16; color: Theme.avatar
                                        Text { anchors.centerIn: parent; visible: !dmAvatar.ready; text: model.initials || String(model.name || "?").substring(0, 1); textFormat: Text.PlainText; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(11 * preferences.fontScale) }
                                        RoundedImage { id: dmAvatar; anchors.fill: parent; source: model.avatarUrl || model.iconUrl || ""; radius: 16 }
                                        Rectangle {
                                            visible: model.status && model.status !== "unknown"
                                            anchors.right: parent.right; anchors.bottom: parent.bottom
                                            width: 11; height: 11; radius: 5.5
                                            color: model.status === "online" ? Theme.success : model.status === "idle" ? Theme.warning : model.status === "dnd" ? Theme.danger : (preferences.highContrast ? "#c7c7cf" : Theme.muted)
                                            border.width: 2; border.color: channelDelegate.selected ? Theme.selected : (preferences.highContrast ? "#151519" : Theme.sidebar)
                                            Rectangle { anchors.centerIn: parent; width: 5; height: 2; radius: 1; color: (preferences.highContrast ? "#151519" : Theme.sidebar); visible: model.status === "dnd" }
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true; spacing: 2
                                        RowLayout {
                                            Layout.fillWidth: true; spacing: 4
                                            Text { Layout.fillWidth: true; text: model.name; textFormat: Text.PlainText; elide: Text.ElideRight; font.pixelSize: Math.round(14 * preferences.fontScale); color: channelDelegate.selected || model.unread ? (preferences.highContrast ? "#ffffff" : Theme.text) : (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.weight: channelDelegate.selected ? Font.Medium : Font.Normal }
                                            GuildTag { visible: channelDelegate.isDm && !!model.primaryGuild && !!model.primaryGuild.tag; guild: model.primaryGuild || ({}) }
                                            AppBadge { visible: channelDelegate.isDm && !!model.bot; verified: !!model.verifiedBot }
                                            PresenceIcons { visible: channelDelegate.isDm; user: model }
                                            AppIcon { visible: channelDelegate.isDm && !!model.blocked; name: "blocked"; color: Theme.danger; Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            visible: channelDelegate.isDm && text.length > 0
                                            text: model.blocked ? "Blocked" : model.streaming ? "Streaming" : model.inVoice ? "In voice" : model.activity || ({online: "Online", idle: "Idle", dnd: "Do not disturb", offline: "Offline"})[model.status] || ""
                                            textFormat: Text.PlainText; elide: Text.ElideRight; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(11 * preferences.fontScale)
                                        }
                                    }
                                    Rectangle { visible: !!model.unread && !channelDelegate.selected; width: 5; height: 5; radius: 3; color: (preferences.highContrast ? "#ffffff" : Theme.text) }
                                    Text { visible: channelDelegate.isVoice && Number(model.voiceStartedAt) > 0; text: visible ? window.channelDuration(model.voiceStartedAt) : ""; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(10 * preferences.fontScale) }
                                    AppIcon { visible: channelDelegate.isDm && (!!model.streaming || !!model.inVoice); name: model.streaming ? "video" : "voice"; color: model.streaming ? Theme.danger : Theme.success; Layout.preferredWidth: 15; Layout.preferredHeight: 15 }
                                    AppIcon { visible: channelDelegate.isDm && (!!model.muted || !!model.deafened) && !!model.inVoice; name: model.deafened ? "deafened" : "micOff"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                                    RowLayout {
                                        visible: model.kind === "voice" && !model.hidden
                                        spacing: 0
                                        opacity: channelHover.hovered || channelDelegate.selected ? 1 : 0
                                        ActionButton { iconName: "chat"; hint: "Open voice chat"; quiet: true; implicitWidth: 25; implicitHeight: 27; leftPadding: 3; rightPadding: 3; topPadding: 3; bottomPadding: 3; enabled: parent.opacity > 0; onClicked: { window.streamExpanded = false; discord.openVoiceChat(model.recordId) } }
                                        ActionButton { iconName: "userPlus"; hint: "Invite to voice"; quiet: true; implicitWidth: 25; implicitHeight: 27; leftPadding: 3; rightPadding: 3; topPadding: 3; bottomPadding: 3; enabled: parent.opacity > 0 && discord.connected && !discord.demoMode; onClicked: voiceInvite.openForChannel(model.recordId, model.name) }
                                    }
                                }
                                leftPadding: 7; rightPadding: 9
                                onClicked: channelDelegate.isVoice && !model.hidden ? window.joinChannel(model.recordId, model.name) : window.selectChannel(model.recordId)
                                ToolTip.visible: channelHover.hovered && channelHover.point.position.y < channelDelegate.rowHeight && !channelDelegate.isCategory && (model.kind !== "voice" || channelHover.point.position.x < channelButton.width - 60); ToolTip.delay: 750
                                ToolTip.text: model.hidden ? "Hidden channel · metadata only" : (model.locked ? "Private channel · " : "") + (model.kind === "stage" ? "Stage channels are not supported yet" : channelDelegate.isVoice ? (discord.demoMode ? "Voice is unavailable in the preview" : !discord.connected ? "Waiting for Discord to reconnect" : voice.available ? "Join voice channel" : "Voice is unavailable in this build") : model.kind === "forum" ? "Browse forum posts" : model.name)
                            }
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: point => { if (!channelDelegate.isCategory && point.position.y < channelDelegate.rowHeight) window.openChannelContext(discord.channels.get(channelDelegate.index)) }
                            }
                            Column {
                                y: channelDelegate.rowHeight; x: 31; width: parent.width - 39
                                Repeater {
                                    model: channelDelegate.membersModel
                                    delegate: Loader {
                                        id: participantBody
                                        required property var model
                                        required property int index
                                        width: parent.width; height: channelDelegate.memberRowHeight
                                        readonly property real rowTop: channelDelegate.y + channelDelegate.rowHeight + index * height
                                        // Keep row geometry without creating offscreen avatars and controls.
                                        active: rowTop + height >= channelList.contentY - 120
                                                && rowTop <= channelList.contentY + channelList.height + 120
                                        sourceComponent: RowLayout {
                                            readonly property var modelData: participantBody.model

                                            width: parent.width; height: channelDelegate.memberRowHeight; spacing: 7
                                            Rectangle {
                                                width: 22; height: 22; radius: 11
                                                color: Theme.avatar
                                                border.width: modelData.speaking ? 2 : 0; border.color: Theme.success
                                                Text { anchors.centerIn: parent; visible: !memberAvatar.ready; text: modelData.initials || String(modelData.name || "?").substring(0,1); textFormat: Text.PlainText; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(9 * preferences.fontScale) }
                                                Rectangle {
                                                    objectName: "streamViewerIndicator"
                                                    visible: !!modelData.watchingYourStream
                                                    anchors.right: parent.left; anchors.rightMargin: 4
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    width: 7; height: 7; radius: 4
                                                    color: preferences.highContrast ? "#ffffff" : Theme.accent
                                                    Accessible.role: Accessible.Indicator
                                                    Accessible.name: modelData.name + " is watching your stream"
                                                    HoverHandler { id: streamViewerHover }
                                                    ToolTip.visible: streamViewerHover.hovered
                                                    ToolTip.delay: 350
                                                    ToolTip.text: "Watching your stream"
                                                }
                                                RoundedImage { id: memberAvatar; anchors.fill: parent; anchors.margins: modelData.speaking ? 2 : 0; source: modelData.avatarUrl || ""; radius: width / 2 }
                                                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; acceptedButtons: Qt.LeftButton | Qt.RightButton; onClicked: mouse => mouse.button === Qt.RightButton ? personMenu.openForUser(modelData.recordId, modelData, modelData.guildId || discord.currentGuildId) : userCard.showUser(modelData.recordId, modelData) }
                                            }
                                            Text { Layout.fillWidth: true; text: modelData.name; textFormat: Text.PlainText; elide: Text.ElideRight; color: modelData.roleColor || (modelData.speaking ? (preferences.highContrast ? "#ffffff" : Theme.text) : (preferences.highContrast ? "#c7c7cf" : Theme.muted)); font.pixelSize: Math.round(12 * preferences.fontScale); MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; acceptedButtons: Qt.LeftButton | Qt.RightButton; onClicked: mouse => mouse.button === Qt.RightButton ? personMenu.openForUser(modelData.recordId, modelData, modelData.guildId || discord.currentGuildId) : userCard.showUser(modelData.recordId, modelData) } }
                                            GuildTag { guild: modelData.primaryGuild || ({}) }
                                            AppBadge { visible: !!modelData.bot; verified: !!modelData.verifiedBot }
                                            AppIcon { visible: !!modelData.muted; name: "micOff"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                                            AppIcon { visible: !!modelData.deafened; name: "deafened"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); Layout.preferredWidth: 14; Layout.preferredHeight: 14 }
                                            ActionButton {
                                                objectName: "watchCameraButton"
                                                visible: !!modelData.camera
                                                iconName: "video"
                                                quiet: true
                                                implicitWidth: 24; implicitHeight: 24
                                                leftPadding: 4; rightPadding: 4; topPadding: 4; bottomPadding: 4
                                                enabled: camera.available && voice.connected && modelData.channelId === voice.channelId && modelData.recordId !== discord.currentUserId
                                                hint: modelData.recordId === discord.currentUserId ? "Your camera" : !voice.connected || modelData.channelId !== voice.channelId ? "Join this voice channel to watch their camera" : "Watch " + modelData.name + "’s camera"
                                                onClicked: window.watchCamera(modelData.recordId, modelData.name + "’s camera")
                                            }
                                            Button {
                                                objectName: "watchDirectRelayStream"
                                                readonly property string directPeer: window.directStreamPeer(modelData.recordId)
                                                visible: directPeer.length > 0
                                                text: "Relay"; implicitHeight: 22; implicitWidth: 48
                                                leftPadding: 5; rightPadding: 5; topPadding: 2; bottomPadding: 2
                                                hoverEnabled: true
                                                ToolTip.visible: hovered; ToolTip.delay: 350
                                                ToolTip.text: "Watch " + modelData.name + " directly via Relay"
                                                onClicked: relayStreams.watch(directPeer)
                                            }
                                            Button {
                                                id: liveButton
                                                visible: !!modelData.streaming && !!modelData.streamKey
                                                text: "LIVE"
                                                implicitWidth: 40; implicitHeight: 20
                                                leftPadding: 5; rightPadding: 5; topPadding: 2; bottomPadding: 2
                                                hoverEnabled: true
                                                background: Rectangle { radius: 3; color: Theme.danger; opacity: liveButton.enabled ? liveButton.hovered ? 1 : 0.85 : 0.55 }
                                                contentItem: Text { text: "LIVE"; color: "#ffffff"; font.pixelSize: Math.round(9 * preferences.fontScale); font.weight: Font.Bold; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                                enabled: stream.available && voice.connected && modelData.channelId === voice.channelId && modelData.recordId !== discord.currentUserId
                                                ToolTip.visible: hovered; ToolTip.delay: 650
                                                ToolTip.text: modelData.recordId === discord.currentUserId ? "Your stream" : !voice.connected || modelData.channelId !== voice.channelId ? "Join this voice channel to watch" : "Watch " + modelData.name + " on Discord"
                                                onClicked: window.watchStream(modelData.streamKey, modelData.name + "’s stream")
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        ScrollBoost { flickable: channelList }
                        ScrollBar.vertical: ScrollBar { width: 4 }
                        Text { anchors.centerIn: parent; width: parent.width - 25; visible: channelList.count === 0; text: discord.currentGuildId ? "No channels available." : "No direct messages yet."; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(12 * preferences.fontScale) }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: voiceState.implicitHeight + 17
                        visible: voice.connected || voice.channelName.length > 0 || voice.error.length > 0 || voice.micTesting
                        color: Theme.surface
                        ColumnLayout {
                            id: voiceState
                            anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 8; anchors.verticalCenter: parent.verticalCenter
                            spacing: 4
                            RowLayout {
                                AppIcon { visible: voice.connected; name: "voice"; color: Theme.success; Layout.preferredWidth: 18; Layout.preferredHeight: 18 }
                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 3
                                    Text {
                                        id: voiceConnectionLabel
                                        Layout.fillWidth: true
                                        text: voice.connected ? "Voice Connected" : voice.micTesting ? "Microphone Test" : voice.status
                                        textFormat: Text.PlainText; elide: Text.ElideRight
                                        color: voice.connected ? Theme.success : Theme.warning
                                        font.pixelSize: Math.round(12 * preferences.fontScale); font.weight: Font.Medium
                                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: connectionInfo.showNear(voiceConnectionLabel) }
                                    }
                                    Text { Layout.fillWidth: true; text: voice.channelName + (window.voiceGuildName() ? " / " + window.voiceGuildName() : ""); textFormat: Text.PlainText; elide: Text.ElideRight; color: (preferences.highContrast ? "#e4e4e8" : Theme.secondary); font.pixelSize: Math.round(10 * preferences.fontScale) }
                                    Text { visible: voice.connected; text: window.voiceDuration(); color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(10 * preferences.fontScale) }
                                }
                                ActionButton { iconName: "disconnect"; hint: "Disconnect from voice"; quiet: true; implicitHeight: 30; onClicked: voice.leave() }
                            }
                            Text { Layout.fillWidth: true; text: voice.error; textFormat: Text.PlainText; visible: text.length > 0; wrapMode: Text.WordWrap; color: Theme.danger; font.pixelSize: Math.round(11 * preferences.fontScale) }
                            RowLayout {
                                visible: voice.micTesting
                                Layout.fillWidth: true
                                Text { Layout.fillWidth: true; text: voice.channelId.length ? "Mic test · muted and deafened" : "Mic test · local playback"; color: Theme.warning; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap }
                                ActionButton { text: "Stop"; hint: "Stop microphone test"; quiet: true; implicitHeight: 30; onClicked: voice.stopMicTest() }
                            }
                            RowLayout {
                                visible: voice.connected || broadcast.active
                                Layout.fillWidth: true
                                Text { visible: broadcast.sharing; text: "LIVE"; color: Theme.danger; font.pixelSize: Math.round(10 * preferences.fontScale); font.weight: Font.Bold }
                                ActionButton {
                                    Layout.fillWidth: true
                                    iconName: "video"
                                    text: broadcast.sharing ? "Stop sharing" : broadcast.active ? "Cancel sharing" : "Share screen"
                                    destructive: broadcast.active
                                    enabled: broadcast.active || (voice.connected && broadcast.available && !window.directVoiceCall)
                                    hint: window.directVoiceCall ? "Screen sharing is currently available in server voice channels." : "Share screen"
                                    onClicked: broadcast.active ? broadcast.stop() : window.showScreenShare()
                                }
                                ActionButton { iconName: "soundboard"; hint: "Soundboard"; enabled: voice.connected; quiet: true; onClicked: window.showSoundboard() }
                            }
                            Text { Layout.fillWidth: true; text: broadcast.error || (broadcast.active && !broadcast.sharing ? broadcast.status : ""); visible: text.length > 0; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: broadcast.error.length ? Theme.danger : (preferences.highContrast ? "#e4e4e8" : Theme.secondary); font.pixelSize: Math.round(11 * preferences.fontScale) }
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true; height: 54; color: Theme.surface
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 5; spacing: 1
                            Button {
                                id: accountButton
                                Layout.fillWidth: true; Layout.minimumWidth: 38; implicitHeight: 40
                                hoverEnabled: true
                                background: Rectangle { color: accountButton.hovered ? Theme.hover : "transparent"; radius: 4 }
                                contentItem: RowLayout {
                                    spacing: 7
                                    Rectangle {
                                        width: 30; height: 30; radius: 15; color: Theme.avatar
                                        Text { anchors.centerIn: parent; visible: !selfAvatar.ready; text: (discord.currentUserName || "?").substring(0,1).toUpperCase(); textFormat: Text.PlainText; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(12 * preferences.fontScale) }
                                        RoundedImage { id: selfAvatar; anchors.fill: parent; source: discord.currentUserAvatarUrl; radius: 15 }
                                        Rectangle { anchors.right: parent.right; anchors.bottom: parent.bottom; width: 10; height: 10; radius: 5; color: discord.connected && !discord.demoMode ? Theme.success : Theme.warning; border.width: 2; border.color: Theme.surface }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true; spacing: 2
                                        Text { Layout.fillWidth: true; text: discord.currentUserName || "Account"; textFormat: Text.PlainText; elide: Text.ElideRight; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(11 * preferences.fontScale); font.weight: Font.Medium }
                                        Text { text: voice.micTesting ? "Testing microphone" : discord.demoMode ? "Preview" : voice.connected ? "In voice" : discord.connected ? "Online" : "Reconnecting"; color: (preferences.highContrast ? "#e4e4e8" : Theme.secondary); font.pixelSize: Math.round(9 * preferences.fontScale) }
                                    }
                                }
                                onClicked: ownProfileMenu.openFor(this)
                            }
                            ActionButton { iconName: voice.muted ? "micOff" : "mic"; hint: voice.micTesting ? "Stop mic test and unmute" : voice.muted ? "Unmute" : "Mute"; destructive: voice.muted; quiet: true; implicitWidth: 28; implicitHeight: 32; leftPadding: 4; rightPadding: 4; onClicked: voice.setMuted(!voice.muted) }
                            Button {
                                id: microphoneOptions
                                implicitWidth: 12; implicitHeight: 32; leftPadding: 1; rightPadding: 1
                                hoverEnabled: true
                                background: Rectangle { radius: 3; color: parent.hovered ? Theme.hover : "transparent" }
                                contentItem: AppIcon { name: "down"; color: (preferences.highContrast ? "#e4e4e8" : Theme.secondary) }
                                onClicked: audioQuickMenu.openFor(microphoneOptions, true)
                                ToolTip.visible: hovered; ToolTip.delay: 650; ToolTip.text: "Microphone options"; Accessible.name: "Microphone options"
                            }
                            ActionButton { iconName: voice.deafened ? "deafened" : "headphones"; hint: voice.micTesting ? "Stop mic test and undeafen" : voice.deafened ? "Undeafen" : "Deafen"; destructive: voice.deafened; quiet: true; implicitWidth: 28; implicitHeight: 32; leftPadding: 4; rightPadding: 4; onClicked: voice.setDeafened(!voice.deafened) }
                            Button {
                                id: outputOptions
                                implicitWidth: 12; implicitHeight: 32; leftPadding: 1; rightPadding: 1
                                hoverEnabled: true
                                background: Rectangle { radius: 3; color: parent.hovered ? Theme.hover : "transparent" }
                                contentItem: AppIcon { name: "down"; color: (preferences.highContrast ? "#e4e4e8" : Theme.secondary) }
                                onClicked: audioQuickMenu.openFor(outputOptions, false)
                                ToolTip.visible: hovered; ToolTip.delay: 650; ToolTip.text: "Output options"; Accessible.name: "Output options"
                            }
                            ActionButton {
                                iconName: "settings"; hint: "Settings"; quiet: true; implicitWidth: 28; implicitHeight: 32; leftPadding: 4; rightPadding: 4
                                onClicked: window.showSettings("account")
                            }
                        }
                    }
                }
            }

            PaneDivider {
                id: channelDivider
                Layout.fillHeight: true; Layout.preferredWidth: 5
                value: window.effectiveChannelWidth
                maximum: Math.min(520, window.paneBudget - (window.showMembers ? 180 : 0))
                onResizeRequested: size => window.channelWidth = size
                onResizeFinished: size => preferences.channelSidebarWidth = size
            }
            ColumnLayout {
                id: chatColumn
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.minimumWidth: 380
                spacing: 0
                Rectangle {
                    id: chatHeader
                    visible: !window.socialVisible
                    Layout.fillWidth: true; height: 48; color: (preferences.highContrast ? "#101014" : Theme.canvas); clip: true
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 10; spacing: 11
                        ActionButton { visible: forumTools.currentThread; iconName: "back"; hint: "Back to " + forumTools.channelName; quiet: true; implicitWidth: 28; padding: 3; onClicked: { window.drafts[discord.currentChannelId] = composer.text; forumTools.returnToForum() } }
                        AppIcon { name: discord.currentChannelKind === "dm" || window.forumSelected ? "chat" : "hash"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
                        Text { text: discord.currentChannelName || "Messages"; textFormat: Text.PlainText; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(15 * preferences.fontScale); font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.minimumWidth: 0; Layout.maximumWidth: 230 }
                        Rectangle { visible: discord.currentChannelTopic.length > 0 && chatColumn.width > 600; width: 1; height: 22; color: (preferences.highContrast ? "#85858f" : Theme.border) }
                        Text {
                            id: channelTopic
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: chatColumn.width > 600 ? discord.currentChannelTopic.replace(/\s+/g, " ").trim() : ""
                            textFormat: Text.PlainText; maximumLineCount: 1; wrapMode: Text.NoWrap
                            color: (preferences.highContrast ? "#e4e4e8" : Theme.secondary); font.pixelSize: Math.round(12 * preferences.fontScale); elide: Text.ElideRight
                            HoverHandler { id: topicHover }
                            ToolTip.visible: topicHover.hovered && text.length > 0
                            ToolTip.delay: 650; ToolTip.text: discord.currentChannelTopic
                        }
                        Text { visible: discord.demoMode && chatColumn.width > 500; text: "PREVIEW"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(9 * preferences.fontScale); font.weight: Font.DemiBold }
                        Button {
                            objectName: "relayUpdateButton"
                            visible: relayUpdater.available
                            text: relayUpdater.busy ? "Updating…" : "Update"
                            enabled: !relayUpdater.busy
                            implicitHeight: 30
                            ToolTip.visible: hovered
                            ToolTip.text: relayUpdater.status
                            onClicked: relayUpdater.install()
                        }
                        ActionButton { visible: !window.relayConversationActive && chatColumn.width > 640; iconName: "refresh"; hint: window.forumSelected ? "Refresh posts" : "Refresh messages"; quiet: true; enabled: !discord.loadingMessages && !forumTools.loading && discord.currentChannelId.length > 0; onClicked: window.forumSelected ? forumTools.refresh() : discord.refresh() }
                        MessageSearchField {
                            id: messageSearch
                            visible: !window.relayConversationActive && !window.forumSelected
                            Layout.preferredWidth: chatColumn.width > 600 ? 180 : 110; Layout.maximumWidth: 180; Layout.minimumWidth: 90
                            implicitHeight: 29; leftPadding: 9; rightPadding: 9
                            placeholderText: "Search"; selectByMouse: true
                            font.pixelSize: Math.round(12 * preferences.fontScale)
                            enabled: discord.currentChannelId.length > 0 && !discord.demoMode
                            background: Rectangle { radius: 5; color: (preferences.highContrast ? "#0b0b0e" : Theme.rail) }
                            onAccepted: if (text.trim()) searchPins.openSearch(text.trim())
                        }
                        ActionButton { visible: !window.relayConversationActive && !window.forumSelected; iconName: "pin"; hint: "Pinned messages"; quiet: true; implicitWidth: 30; leftPadding: 5; rightPadding: 5; enabled: discord.currentChannelId.length > 0 && !discord.demoMode; onClicked: searchPins.openPins(discord.currentChannelId) }
                        ActionButton { visible: voice.connected || broadcast.active; iconName: "video"; hint: window.directVoiceCall ? "Screen sharing is currently available in server voice channels." : broadcast.sharing ? "Stop sharing your screen" : broadcast.active ? "Cancel screen sharing" : "Share screen"; quiet: true; destructive: broadcast.active; enabled: broadcast.active || (broadcast.available && !window.directVoiceCall); onClicked: broadcast.active ? broadcast.stop() : window.showScreenShare() }
                        ActionButton { visible: discord.currentGuildId.length > 0 || discord.currentChannelKind === "dm"; iconName: "members"; hint: discord.currentChannelKind === "dm" ? (window.showMembers ? "Hide profile" : "Show profile") : window.showMembers ? "Hide member list" : "Show member list"; quiet: true; primary: window.showMembers; onClicked: preferences.memberListVisible = !preferences.memberListVisible }
                    }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.surface }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
                    visible: relayConnections.invitations.length > 0
                    property var invitation: relayConnections.invitations.length ? relayConnections.invitations[0] : ({})
                    Label { Layout.fillWidth: true; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.text; text: (parent.invitation.name || "Someone") + " wants to connect with you on Relay." }
                    Button { text: "View invitation"; onClicked: { discord.activateChannel(parent.invitation.channel); window.homePage = ""; window.relayMode = true } }
                    Button { text: "Dismiss"; onClicked: relayConnections.dismiss(parent.invitation.binding) }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12
                    visible: !window.socialVisible && discord.currentChannelKind === "dm"
                    Button { text: "Discord"; checkable: true; checked: !window.relayMode; onClicked: window.relayMode = false }
                    Button { text: "Relay"; checkable: true; checked: window.relayMode; enabled: window.relayBinding.length > 0; onClicked: window.relayMode = true }
                    Label { Layout.fillWidth: true; text: window.relayMode ? "Direct encrypted text · separate local history" : "Discord conversation"; color: Theme.muted; wrapMode: Text.Wrap }
                }
                RelayConversation {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    visible: window.relayConversationActive
                    bindingKey: window.relayBinding
                    channelId: discord.currentChannelId
                    contactName: discord.currentChannelName
                }
                Rectangle {
                    Layout.fillWidth: true; height: 30
                    visible: !window.relayConversationActive && !discord.demoMode && !discord.connected
                    color: Theme.warningSurface
                    Text { anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 16; verticalAlignment: Text.AlignVCenter; text: discord.status + " · Your draft is kept while reconnecting."; textFormat: Text.PlainText; elide: Text.ElideRight; color: Theme.warning; font.pixelSize: Math.round(11 * preferences.fontScale) }
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: replyJumpLabel.implicitHeight + 16
                    visible: !window.socialVisible && !window.streamCoveringChat && (window.pendingReplyId.length > 0 || window.replyJumpError.length > 0)
                    color: window.replyJumpError.length ? Theme.errorSurface : Theme.surface
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 8; spacing: 8
                        Text { id: replyJumpLabel; Layout.fillWidth: true; text: window.pendingReplyId.length ? "Loading original message…" : window.replyJumpError; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: window.replyJumpError.length ? Theme.danger : (preferences.highContrast ? "#e4e4e8" : Theme.secondary); font.pixelSize: Math.round(12 * preferences.fontScale) }
                        ActionButton { visible: window.replyJumpError.length > 0; iconName: "close"; hint: "Dismiss message error"; quiet: true; implicitWidth: 26; implicitHeight: 26; padding: 4; onClicked: window.replyJumpError = "" }
                    }
                }
                Item {
                    visible: !window.relayConversationActive
                    Layout.fillWidth: true; Layout.fillHeight: true
                    ListView {
                        boundsBehavior: Flickable.StopAtBounds
                        boundsMovement: Flickable.StopAtBounds
                        id: messages
                        visible: !window.socialVisible && !window.streamCoveringChat && !window.forumSelected && !window.hiddenChannelSelected
                        anchors.fill: parent; anchors.topMargin: 10; anchors.bottomMargin: 8
                        model: discord.messages
                        clip: true; reuseItems: true; cacheBuffer: 500
                        onMovementEnded: window.followMessages = atYEnd && !discord.hasNewerMessages
                        onContentYChanged: window.scheduleHistoryLoad()
                        onContentHeightChanged: window.scheduleHistoryLoad()
                        onHeightChanged: window.scheduleHistoryLoad()
                        onCountChanged: { if (!window.loadingOlder && window.followMessages) Qt.callLater(window.positionAtEndIfFollowing) }
                        header: Item { width: messages.width; height: 8 }
                        delegate: MessageItem {
                            required property var model
                            width: messages.width; message: model; compact: preferences.compactMessages
                            onHeightChanged: if (model) messageHistory.delegateGeometryChanged(String(model.recordId || ""))
                            highlighted: String(model.recordId) === window.highlightedMessageId
                            onReplyRequested: (channelId, messageId) => window.jumpToReply(channelId, messageId)
                            onImageRequested: (source, originalUrl, title) => window.showImage(source, originalUrl, title)
                            onUserRequested: (userId, fallback) => userCard.showUser(userId, fallback)
                            onTranslationRequested: text => window.showTranslation(text, false)
                            onContextRequested: (userId, fallback) => personMenu.openForUser(userId, fallback, discord.currentGuildId)
                        }
                        ScrollBoost { id: messageScroll; flickable: messages }
                        ScrollBar.vertical: ScrollBar { id: messageScrollbar; width: 8 }
                    }
                    HiddenChannelView { anchors.fill: parent; visible: window.hiddenChannelSelected && !window.streamCoveringChat; channel: discord.channels.get(discord.channels.indexOf(discord.currentChannelId)) }
                    ForumView { anchors.fill: parent; visible: window.forumSelected && !window.streamCoveringChat }
                    BusyIndicator { anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; anchors.topMargin: 4; width: 24; height: 24; z: 4; visible: !window.socialVisible && !window.forumSelected && window.loadingOlder && discord.loadingMessages; running: visible }
                    ColumnLayout {
                        anchors.centerIn: parent; width: Math.min(parent.width - 60, 370); spacing: 12; visible: !window.socialVisible && !window.streamCoveringChat && !window.forumSelected && !window.hiddenChannelSelected && messages.count === 0
                        AppIcon { Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 42; Layout.preferredHeight: 42; name: "chat"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted) }
                        Text { Layout.fillWidth: true; text: discord.loadingMessages ? "Loading messages…" : discord.currentChannelId ? "No messages yet" : "Choose a conversation"; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(20 * preferences.fontScale); font.weight: Font.DemiBold; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap }
                        Text { Layout.fillWidth: true; text: discord.loadingMessages ? "" : discord.currentChannelId ? "Start the conversation below." : "Select a server or direct message to get started."; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(13 * preferences.fontScale); horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap }
                    }
                    ActionButton { anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter; anchors.bottomMargin: 8; iconName: "down"; text: "Jump to present"; visible: !window.socialVisible && !window.streamCoveringChat && !window.forumSelected && !window.hiddenChannelSelected && messages.count > 0 && (!messages.atYEnd || discord.hasNewerMessages) && !discord.loadingMessages; onClicked: { window.clearReplyJump(); messageHistory.reset(); messageScroll.cancel(); messages.cancelFlick(); window.followMessages = true; if (discord.hasNewerMessages) discord.jumpToPresent(); else messages.positionViewAtEnd() } }
                    SocialHome {
                        anchors.fill: parent
                        visible: window.socialVisible && !window.streamCoveringChat
                        service: discord
                        page: window.homePage
                        onMessageRequested: userId => window.openSocialDm(userId)
                        onProfileRequested: (userId, fallback) => userCard.showUser(userId, fallback)
                        onSettingsRequested: page => window.showSettings(page)
                        onBlockRequested: (userId, fallback) => actionConfirmation.ask("Block " + String(fallback.name || "user") + "?", "This updates your Discord block list.", "Block", {service:"member", action:"block", targetId:userId}, false)
                    }
                    StreamHost { id: embeddedStreamHost; anchors.fill: parent; visible: window.streamEmbedded; expanded: window.streamExpanded; z: 5 }
                    AttachmentDropArea { anchors.fill: parent; z: 10; service: uploads; enabled: window.canCompose && !window.socialVisible && !window.streamCoveringChat && !uploads.busy }
                }
                AttachmentTray {
                    Layout.fillWidth: true; Layout.leftMargin: 16; Layout.rightMargin: 16
                    service: uploads
                    visible: window.canCompose && !window.streamCoveringChat && (uploads.count > 0 || uploads.busy || uploads.error.length > 0)
                    onAddRequested: attachmentPicker.open()
                }
                Rectangle {
                    id: composerFrame
                    visible: !window.relayConversationActive && !window.socialVisible && !window.streamCoveringChat && !window.forumSelected && !window.hiddenChannelSelected
                    Layout.fillWidth: true; Layout.leftMargin: 16; Layout.rightMargin: 16; Layout.bottomMargin: 6
                    implicitHeight: composerRow.implicitHeight + 8
                    color: Theme.input; radius: 8
                    RowLayout {
                        id: composerRow
                        ActionButton { Layout.alignment: Qt.AlignBottom; iconName: "plus"; hint: "Upload a file · Ctrl+Shift+U"; quiet: true; enabled: window.canCompose && !uploads.busy; implicitWidth: 32; leftPadding: 5; rightPadding: 5; onClicked: attachmentPicker.open() }
                        anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 7; anchors.verticalCenter: parent.verticalCenter; spacing: 6
                        HardScrollView {
                            Layout.fillWidth: true; Layout.preferredHeight: Math.min(138, Math.max(38, composer.contentHeight + 18)); clip: true
                            AttachmentComposer {
                                id: composer
                                attachmentService: uploads
                                attachmentPasteEnabled: window.canCompose
                                placeholderText: forumTools.currentThread && !forumTools.canReply ? "This discussion is locked or read-only" : !window.canCompose ? "Choose chat" : width < 240 * preferences.fontScale ? "Message" : "Message " + (discord.currentChannelKind === "dm" ? "" : "#") + discord.currentChannelName
                                placeholderTextColor: (preferences.highContrast ? "#c7c7cf" : Theme.muted); color: (preferences.highContrast ? "#ffffff" : Theme.text); selectionColor: Theme.selected; selectedTextColor: "#ffffff"
                                font.pixelSize: Math.round(14 * preferences.fontScale); wrapMode: TextEdit.Wrap; textFormat: TextEdit.PlainText; selectByMouse: true; enabled: window.canCompose
                                leftPadding: 8; rightPadding: 4; topPadding: 9; bottomPadding: 8
                                background: Item {}
                                onTextChanged: Qt.callLater(window.updateMentions)
                                onCursorPositionChanged: Qt.callLater(window.updateMentions)
                                onEditingKeyPressed: event => {
                                    if (mentionPopup.visible) {
                                        if (event.key === Qt.Key_Down) { mentionList.currentIndex = Math.min(mentionList.count - 1, mentionList.currentIndex + 1); event.accepted = true; return }
                                        if (event.key === Qt.Key_Up) { mentionList.currentIndex = Math.max(0, mentionList.currentIndex - 1); event.accepted = true; return }
                                        if (event.key === Qt.Key_Escape) { mentionPopup.close(); event.accepted = true; return }
                                        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) { window.insertMention(mentionList.currentIndex); event.accepted = true; return }
                                    }
                                    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier)) { window.sendDraft(); event.accepted = true }
                                }
                            }
                            ScrollBar.vertical: ScrollBar { width: 4 }
                        }
                        ActionButton { id: emojiButton; Layout.alignment: Qt.AlignBottom; iconName: "emoji"; hint: "Emoji"; quiet: true; enabled: window.canCompose; implicitWidth: 30; leftPadding: 5; rightPadding: 5; onClicked: emojiPicker.showNear(composerFrame) }
                        ActionButton { Layout.alignment: Qt.AlignBottom; text: "GIF"; hint: "GIFs"; quiet: true; enabled: window.canCompose; implicitWidth: Math.max(32, Math.ceil(28 * preferences.fontScale) + 6); leftPadding: 3; rightPadding: 3; onClicked: gifPicker.showNear(composerFrame) }
                        ActionButton { Layout.alignment: Qt.AlignBottom; iconName: "calendar"; hint: "Insert date & time"; quiet: true; enabled: window.canCompose; implicitWidth: 30; leftPadding: 5; rightPadding: 5; onClicked: dateTimePicker.open() }
                        ActionButton { Layout.alignment: Qt.AlignBottom; iconName: "translate"; hint: "Translate draft"; quiet: true; enabled: window.canCompose && composer.text.trim().length > 0; implicitWidth: 30; leftPadding: 5; rightPadding: 5; onClicked: window.translateDraft() }
                        ActionButton { Layout.alignment: Qt.AlignBottom; iconName: "send"; hint: "Send message"; quiet: true; enabled: window.canSend && (composer.text.trim().length > 0 || uploads.count > 0) && composer.text.length <= 2000 && !discord.sendingMessage && !uploads.busy; onClicked: window.sendDraft() }
                    }
                }
                RowLayout {
                    visible: !window.socialVisible && !window.streamCoveringChat && !window.forumSelected && !window.hiddenChannelSelected
                    Layout.fillWidth: true; Layout.leftMargin: 22; Layout.rightMargin: 22; Layout.bottomMargin: 12
                    Text { text: discord.demoMode ? "Preview messages stay in this app." : discord.sendingMessage ? "Sending…" : ""; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(10 * preferences.fontScale) }
                    Item { Layout.fillWidth: true }
                    Text { visible: composer.text.length > 1800; text: composer.text.length + " / 2000"; color: composer.text.length > 2000 ? Theme.danger : (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(10 * preferences.fontScale) }
                }
            }
            PaneDivider {
                id: memberDivider
                visible: window.showMembers
                Layout.fillHeight: true; Layout.preferredWidth: 5
                value: window.effectiveMemberWidth; direction: -1
                maximum: Math.min(480, window.paneBudget - window.effectiveChannelWidth)
                onResizeRequested: size => window.memberWidth = size
                onResizeFinished: size => preferences.memberSidebarWidth = size
            }
            MemberSidebar {
                onUserRequested: (id, user) => userCard.showUser(id, user)
                onContextRequested: (id, user) => personMenu.openForUser(id, user, discord.currentGuildId)
                visible: window.showMembers && discord.currentChannelKind !== "dm"
                Layout.fillHeight: true
                Layout.preferredWidth: window.effectiveMemberWidth; Layout.minimumWidth: window.effectiveMemberWidth; Layout.maximumWidth: window.effectiveMemberWidth
            }
            DmProfileSidebar {
                onUserRequested: (id, user) => userCard.showUser(id, user)
                onContextRequested: (id, user) => personMenu.openForUser(id, user, "")
                visible: window.showMembers && discord.currentChannelKind === "dm"
                Layout.fillHeight: true
                Layout.preferredWidth: window.effectiveMemberWidth; Layout.minimumWidth: window.effectiveMemberWidth; Layout.maximumWidth: window.effectiveMemberWidth
            }
        }
    }

    Popup {
        id: mentionPopup
        parent: Overlay.overlay
        width: Math.min(440, composerFrame.width)
        height: Math.min(280, mentionList.count * 42 + 14)
        property var candidates: []
        padding: 7
        modal: false; focus: false
        closePolicy: Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.surface; radius: 6; border.color: (preferences.highContrast ? "#85858f" : Theme.border) }
        contentItem: ListView {
                        boundsBehavior: Flickable.StopAtBounds
                        boundsMovement: Flickable.StopAtBounds
            id: mentionList
            clip: true
            model: mentionPopup.candidates
            onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
            delegate: Button {
                required property var modelData
                required property int index
                width: mentionList.width; height: 42
                leftPadding: 8; rightPadding: 8
                hoverEnabled: true
                background: Rectangle { radius: 4; color: parent.hovered || index === mentionList.currentIndex ? Theme.selected : "transparent" }
                contentItem: RowLayout {
                    spacing: 10
                    Rectangle {
                        width: 26; height: 26; radius: 13; color: Theme.avatar
                        Text { anchors.centerIn: parent; visible: !mentionAvatar.ready; text: String(modelData.name || "?").substring(0, 1); textFormat: Text.PlainText; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(11 * preferences.fontScale) }
                        RoundedImage { id: mentionAvatar; anchors.fill: parent; source: modelData.avatarUrl || ""; radius: 13 }
                    }
                    Text { Layout.fillWidth: true; text: modelData.name; textFormat: Text.PlainText; color: (preferences.highContrast ? "#ffffff" : Theme.text); elide: Text.ElideRight; font.pixelSize: Math.round(13 * preferences.fontScale) }
                    AppBadge { visible: !!modelData.bot; verified: !!modelData.verifiedBot }
                }
                onClicked: window.insertMention(index)
            }
            ScrollBoost { flickable: mentionList }
            ScrollBar.vertical: ScrollBar { width: 4 }
        }
    }
    Dialog {
        id: switcher
        parent: Overlay.overlay
        width: 520; x: (parent.width - width) / 2; y: Math.max(60, (parent.height - height) / 3)
        modal: true; padding: 18
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { radius: 7; color: (preferences.highContrast ? "#101014" : Theme.canvas) }
        onOpened: { channelSearch.clear(); channelSearch.forceActiveFocus() }
        function filteredChannels() {
            const result = [], query = channelSearch.text.trim().toLowerCase()
            for (let i = 0; i < discord.channels.count; ++i) {
                const channel = discord.channels.get(i)
                if (["text", "thread", "forum", "dm"].indexOf(channel.kind) !== -1 && channel.name.toLowerCase().indexOf(query) !== -1) result.push(channel)
            }
            return result
        }
        function choose(index) {
            if (index < 0 || index >= switchResults.count) return
            window.selectChannel(switchResults.model[index].recordId); switcher.close(); composer.forceActiveFocus()
        }
        contentItem: ColumnLayout {
            spacing: 12
            Text { text: "Find a conversation"; color: (preferences.highContrast ? "#ffffff" : Theme.text); font.pixelSize: Math.round(17 * preferences.fontScale); font.weight: Font.DemiBold }
            AppField {
                id: channelSearch
                Layout.fillWidth: true; placeholderText: "Search this server"
                onTextChanged: switchResults.currentIndex = 0
                onAccepted: switcher.choose(switchResults.currentIndex)
                Keys.onDownPressed: switchResults.currentIndex = Math.min(switchResults.count - 1, switchResults.currentIndex + 1)
                Keys.onUpPressed: switchResults.currentIndex = Math.max(0, switchResults.currentIndex - 1)
            }
            ListView {
                        boundsBehavior: Flickable.StopAtBounds
                        boundsMovement: Flickable.StopAtBounds
                id: switchResults
                Layout.fillWidth: true; Layout.preferredHeight: Math.min(310, Math.max(52, count * 39))
                model: switcher.filteredChannels(); clip: true; spacing: 3
                delegate: ActionButton { required property var modelData; required property int index; width: switchResults.width; iconName: modelData.kind === "dm" ? "chat" : "hash"; text: modelData.name; primary: index === switchResults.currentIndex; quiet: true; onClicked: switcher.choose(index) }
                ScrollBoost { flickable: switchResults }
                Text { anchors.centerIn: parent; visible: switchResults.count === 0; text: "No matching conversations"; color: (preferences.highContrast ? "#c7c7cf" : Theme.muted); font.pixelSize: Math.round(13 * preferences.fontScale) }
            }
        }
    }
    Loader { id: settingsLoader; active: false; source: "components/SettingsWindow.qml"; onLoaded: item.openPage(window.pendingSettingsPage) }
    Loader { id: serverSettingsLoader; active: false; source: "components/ServerSettingsWindow.qml"; onLoaded: item.openGuild(window.pendingServerGuild, window.pendingServerPage) }
    Connections {
        target: serverSettingsLoader.item
        function onMentionRequested(id) { window.insertComposerText("<@" + id + "> ") }
        function onWatchRequested(key, title) { window.watchStream(key, title) }
    }
    Loader { active: relayStreams.watchingPeer.length > 0; source: "components/RelayStreamWindow.qml" }
    Loader { id: screenShareLoader; active: false; source: "components/ScreenShareDialog.qml"; onLoaded: item.open() }
    Loader { id: translationLoader; active: false; source: "components/TranslationDialog.qml"; onLoaded: item.openText(window.pendingTranslationText, window.pendingTranslationInsert) }
    Connections {
        target: translationLoader.item
        function onInsertRequested(text) {
            if (!window.pendingTranslationInsert || window.pendingTranslationChannel !== discord.currentChannelId || composer.text !== window.pendingTranslationText) return
            composer.text = text; composer.cursorPosition = text.length; composer.forceActiveFocus()
        }
    }
    Component {
        id: playerComponent
        StreamPlayer {
            floating: !window.streamPoppedOut && !window.streamExpanded
            onExpandRequested: window.streamExpanded = true
            onMinimizeRequested: window.streamExpanded = false
            onMoveRequested: (dx, dy) => embeddedStreamHost.moveBy(dx, dy)
            onPopOutRequested: window.popOutStream(false)
            onReturnToAppRequested: window.returnStreamToApp(true)
            onStopRequested: window.stopWatching()
            onFullscreenRequested: {
                if (window.streamPoppedOut && streamWindowLoader.item) streamWindowLoader.item.toggleFullscreen()
                else window.popOutStream(true)
            }
        }
    }
    Loader {
        id: streamWindowLoader
        active: false
        source: "components/StreamWindow.qml"
        onLoaded: if (window.streamPlayer) item.presentPlayer(window.streamPlayer, window.pendingStreamFullscreen)
    }
    Connections { target: streamWindowLoader.item; function onReturnToAppRequested() { window.returnStreamToApp(true) } }
    Connections {
        target: settingsLoader.item
        function onOpenAddon(id) { window.openAddon(id) }
        function onSignOutRequested() { window.signOut() }
    }
    Loader { id: addonSettings; onLoaded: if (item && item.open) item.open() }
    Connections { target: addonSettings.item; function onClosed() { addons.closeSettings() } }
    MediaViewer { id: mediaViewer }
    SoundboardDialog { id: soundboardDialog; onCustomRequested: { addons.setEnabled("org.relay.soundboard",true); window.openAddon("org.relay.soundboard") } }
    UserCard { id: userCard; onWatchRequested: (key, title) => window.watchStream(key, title) }
    AttachmentPicker { id: attachmentPicker; service: uploads }
    GuildProfileDialog { id: guildProfileDialog }
    OwnProfileMenu {
        id: ownProfileMenu
        accountSwitcher: accounts
        onEditProfileRequested: window.showSettings("account")
        onViewProfileRequested: userCard.showUser(discord.currentUserId, {name:discord.currentUserName,avatarUrl:discord.currentUserAvatarUrl})
        onLogoutRequested: window.signOut()
    }
    PersonContextMenu {
        id: personMenu
        allowMention: window.canCompose
        onProfileRequested: (id, user) => userCard.showUser(id, user)
        onMentionRequested: id => window.insertComposerText("<@" + id + "> ")
        onConfirmationRequested: (title, description, label, payload, reason) => actionConfirmation.ask(title, description, label, payload, reason)
    }
    ChannelContextMenu {
        id: channelMenu
        onEditRequested: (guildId, channel) => channelEditor.openEdit(guildId, channel)
        onCreateRequested: (guildId, parentId) => channelEditor.openCreate(guildId, parentId)
        onJoinRequested: (guildId, channelId, channelName) => voice.join(guildId, channelId, channelName, discord.currentUserId)
        onInviteRequested: (channelId, channelName) => voiceInvite.openForChannel(channelId, channelName)
        onConfirmationRequested: (title, description, label, payload, reason) => actionConfirmation.ask(title, description, label, payload, reason)
    }
    ServerContextMenu {
        id: serverMenu
        onChannelsRolesRequested: guildId => messageReferences.openGuild(guildId, "customize")
        onAccountSettingsRequested: page => window.showSettings(page)
        onProfileEditRequested: guildId => guildProfileDialog.openFor(guildId)
        onSettingsRequested: (guildId, page) => window.showServerSettings(guildId, page)
        onCreateRequested: (guildId, parentId) => channelEditor.openCreate(guildId, parentId)
        onConfirmationRequested: (title, description, label, payload, reason) => actionConfirmation.ask(title, description, label, payload, reason)
    }
    ChannelEditorDialog { id: channelEditor }
    ActionConfirmation {
        id: actionConfirmation
        onConfirmed: (payload, reason) => {
            if (payload.service === "member" && payload.action === "block") memberActions.setBlocked(payload.targetId, true)
            else if (payload.service === "guild") {
                const values = Object.assign({}, payload.values || {})
                if (reason) values.reason = reason
                guildManagement.selectGuild(payload.guildId)
                if (guildManagement.guildId !== payload.guildId) return
                guildManagement.execute(payload.action, payload.targetId, values)
            }
        }
    }
    BoundedMenu {
        id: dmContext
        property string channelId: ""
        MenuItem { text: "Open conversation"; onTriggered: window.selectChannel(dmContext.channelId) }
        MenuItem { text: "Copy conversation link"; onTriggered: chatText.copy("https://discord.com/channels/@me/" + dmContext.channelId) }
        MenuItem { text: "Copy channel ID"; onTriggered: memberActions.copyId(dmContext.channelId) }
    }
    ConnectionInfo { id: connectionInfo }
    EmojiPicker { id: emojiPicker; onEmojiSelected: text => window.insertComposerText(text) }
    GifPicker { id: gifPicker; onInsertText: text => window.insertComposerText(text) }
    DateTimePicker { id: dateTimePicker; onTimestampSelected: text => window.insertComposerText(text) }
    AudioQuickMenu { id: audioQuickMenu; onSettingsRequested: window.showAudio() }
    SearchPinsDialog {
        id: searchPins
        onImageRequested: (source, originalUrl, title) => window.showImage(source, originalUrl, title)
        onUserRequested: (userId, fallback) => userCard.showUser(userId, fallback)
        onTranslationRequested: text => window.showTranslation(text, false)
        onContextRequested: (userId, fallback, guildId) => personMenu.openForUser(userId, fallback, guildId)
    }
    Connections {
        target: channelTools
        function onMessageOpened(channelId, messageId, rowIndex) {
            window.followMessages = false
            if (rowIndex >= 0) Qt.callLater(() => messages.positionViewAtIndex(rowIndex, ListView.Center))
        }
    }
    GuildHoverCard { id: guildHover }
    VoiceInviteDialog { id: voiceInvite }
}
