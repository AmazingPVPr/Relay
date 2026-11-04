import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtMultimedia
import Relay.Native
import "MediaUrls.js" as MediaUrls
import "MediaPlayback.js" as MediaPlayback
import "Theme.js" as Theme
import "ApplicationLifecycle.js" as ApplicationLifecycle

Item {
    id: root
    objectName: "inlineVideoPlayer"
    property string sourceUrl: ""
    property string pageUrl: ""
    property string posterUrl: ""
    property string title: "Video"
    property int mediaWidth: 0
    property int mediaHeight: 0
    property int maximumHeight: 320
    property bool gif: false
    property real startSeconds: 0
    property bool playbackEnabled: true
    readonly property string instanceId: MediaPlayback.createId()
    property int generation: 0
    property string requestId: ""
    property string requestChannel: ""
    property string requestAccount: ""
    property string resolvedSource: ""
    property string mediaError: ""
    property bool resolving: false
    property bool requested: false
    property bool hasFrame: false
    property bool audioMuted: gif
    property real volume: 1
    property bool fullscreen: false
    property bool controlsShown: true
    property int resolvedWidth: 0
    property int resolvedHeight: 0
    property real resolvedStart: 0
    property string bookmarkKey: ""
    property real bookmarkPosition: 0
    property bool wantPlaying: false
    property bool userPaused: false
    readonly property bool playing: session.item ? session.item.playing : false
    readonly property real position: session.item ? session.item.position : 0
    readonly property real duration: session.item ? session.item.duration : 0
    readonly property bool seekable: session.item ? session.item.seekable : false
    readonly property bool buffering: session.item ? session.item.buffering : false
    readonly property var qualityOptions: session.item ? session.item.qualityOptions : []
    readonly property int activeQuality: session.item ? session.item.activeQuality : -1
    readonly property real sourceWidth: resolvedWidth > 0 ? resolvedWidth : mediaWidth
    readonly property real sourceHeight: resolvedHeight > 0 ? resolvedHeight : mediaHeight
    readonly property var fitted: MediaUrls.imageSize(sourceWidth > 0 ? sourceWidth : 16, sourceHeight > 0 ? sourceHeight : 9, Math.max(1, width), maximumHeight)
    implicitWidth: sourceWidth > 0 && sourceHeight > 0 ? Math.min(460, sourceWidth, maximumHeight * sourceWidth / sourceHeight) : 460
    implicitHeight: sourceWidth > 0 && sourceHeight > 0 ? fitted.height : Math.min(maximumHeight, Math.max(1, width) * 9 / 16)
    Accessible.role: Accessible.Grouping
    Accessible.name: title || "Video"

    function timeLabel(milliseconds) {
        const seconds = Math.max(0, Math.floor(milliseconds / 1000))
        const minutes = Math.floor(seconds / 60)
        return (minutes >= 60 ? Math.floor(minutes / 60) + ":" + String(minutes % 60).padStart(2, "0") : String(minutes)) + ":" + String(seconds % 60).padStart(2, "0")
    }
    function showControls() { controlsShown = true; if (playing) hideControls.restart(); else hideControls.stop() }
    function requestMatches(id) {
        return id === requestId && requested && playbackEnabled
            && requestAccount === discord.currentUserId && requestChannel === discord.currentChannelId
    }
    function play() {
        if (!playbackEnabled || !visible) return
        const key = discord.currentUserId + ":" + discord.currentChannelId + ":" + (pageUrl || sourceUrl)
        if (bookmarkKey !== key) { bookmarkKey = key; bookmarkPosition = MediaPlayback.position(key) }
        if (mediaError) suspend()
        wantPlaying = true
        userPaused = false
        showControls()
        if (session.item && resolvedSource) {
            MediaPlayback.claim(root)
            session.item.resume()
            return
        }
        if (resolving) return
        const url = /^https:\/\/(?:www\.|m\.)?(?:youtube\.com|youtu\.be)\//i.test(pageUrl) ? pageUrl : sourceUrl || pageUrl
        if (!url) { mediaError = "No playable video is available."; return }
        MediaPlayback.claim(root)
        mediaError = ""; hasFrame = false; resolving = true; requested = true
        requestAccount = discord.currentUserId; requestChannel = discord.currentChannelId
        requestId = instanceId + "-" + (++generation)
        if (!videoResolver.resolve(requestId, url) && resolving) {
            resolving = false; requested = false; requestId = ""
            mediaError = "This video could not be opened. Try again."
            MediaPlayback.release(root)
        }
    }
    function togglePlayback() {
        if (wantPlaying && session.item) { wantPlaying = false; userPaused = true; session.item.pause() }
        else play()
        showControls()
    }
    function seekTo(milliseconds) { bookmarkPosition = milliseconds; if (session.item) session.item.seek(milliseconds) }
    function chooseQuality(index) { if (session.item) session.item.chooseQuality(index); showControls() }
    function stop() { stopPlayback(true) }
    function suspend() { stopPlayback(false) }
    function stopPlayback(resetPosition) {
        if (resetPosition === false) MediaPlayback.remember(bookmarkKey, bookmarkPosition)
        else MediaPlayback.forget(bookmarkKey)
        const oldId = requestId
        requestId = ""; ++generation
        if (oldId) videoResolver.cancel(oldId)
        if (session.item) session.item.stop()
        requested = false; resolving = false; resolvedSource = ""; hasFrame = false
        wantPlaying = false; userPaused = false
        if (resetPosition !== false) bookmarkPosition = 0
        resolvedWidth = 0; resolvedHeight = 0; resolvedStart = 0
        mediaError = ""; controlsShown = true
        hideControls.stop()
        leaveFullscreen()
        MediaPlayback.release(root)
    }
    function enterFullscreen() {
        if (ApplicationLifecycle.closing() || fullscreen) return
        fullscreen = true
        if (fullscreenLoader.item) fullscreenLoader.item.present()
        else fullscreenLoader.active = true
    }
    function leaveFullscreen() {
        if (!fullscreen) return
        fullscreen = false
        stage.parent = inlineHost
        if (fullscreenLoader.item) fullscreenLoader.item.hide()
        showControls()
    }
    onSourceUrlChanged: { suspend(); bookmarkPosition = 0; bookmarkKey = "" }
    onPageUrlChanged: { suspend(); bookmarkPosition = 0; bookmarkKey = "" }
    onPlaybackEnabledChanged: if (!playbackEnabled) suspend()
    onVisibleChanged: if (!visible) suspend()
    onPlayingChanged: showControls()
    Component.onDestruction: suspend()
    Connections {
        target: discord
        function onStateChanged() {
            if (root.requested && (root.requestAccount !== discord.currentUserId || root.requestChannel !== discord.currentChannelId)) root.suspend()
        }
    }
    Connections {
        target: videoResolver
        function onResolved(id, media) {
            if (!root.requestMatches(id)) return
            root.resolving = false
            root.resolvedStart = root.bookmarkPosition > 0 ? root.bookmarkPosition / 1000 : Number(media.startSeconds) > 0 ? Number(media.startSeconds) : root.startSeconds
            root.resolvedWidth = Number(media.width) || 0; root.resolvedHeight = Number(media.height) || 0
            root.resolvedSource = String(media.sourceUrl || "")
            if (!root.resolvedSource) { root.stop(); root.mediaError = "No playable video was returned."; return }
        }
        function onFailed(id, reason) {
            if (!root.requestMatches(id)) return
            root.suspend()
            root.mediaError = reason || "This video could not be opened."
        }
    }
    Loader {
        id: session
        active: root.requested && root.resolvedSource.length > 0
        sourceComponent: Item {
            id: playback
            property real pendingStart: 0
            property bool waitingToPlay: false
            property bool switchingSource: false
            property bool qualityChosen: false
            property bool audioQualityChosen: false
            property string preferredAudioLanguage: ""
            property var qualityOptions: []
            readonly property int activeQuality: nativeSource.active ? nativeSource.activeQuality : media.activeVideoTrack
            readonly property bool playing: media.playbackState === MediaPlayer.PlayingState
            readonly property real position: media.position
            readonly property real duration: media.duration
            readonly property bool seekable: media.seekable
            readonly property bool buffering: media.mediaStatus === MediaPlayer.LoadingMedia || media.mediaStatus === MediaPlayer.BufferingMedia || media.mediaStatus === MediaPlayer.StalledMedia
            function begin(url) {
                qualityChosen = false; audioQualityChosen = false; qualityOptions = []
                pendingStart = Math.max(0, root.resolvedStart * 1000); waitingToPlay = true
                // Qt 6.10+ can limit initial format probing without changing
                // normal playback buffering. Older Qt retains its defaults.
                if (typeof media.playbackOptions !== "undefined" && root.pageUrl) media.playbackOptions.probeSize = 1048576
                if (!nativeSource.open(media, url)) media.source = url
            }
            function updateTracks() {
                const options = []
                let bestVideo = -1, bestPixels = 0, bestFps = 0
                for (let i = 0; i < media.videoTracks.length; ++i) {
                    const metadata = media.videoTracks[i]
                    const resolution = metadata.value(MediaMetaData.Resolution)
                    const width = resolution && Number(resolution.width) || 0, height = resolution && Number(resolution.height) || 0
                    const fps = Number(metadata.value(MediaMetaData.VideoFrameRate)) || 0
                    const codec = metadata.stringValue(MediaMetaData.VideoCodec)
                    options.push({index:i,width:width,height:height,fps:fps,label:(height > 0 ? height + "p" : "Track " + (i + 1)) + (fps > 30 ? " " + Math.round(fps) : "") + (codec ? " · " + codec : "")})
                    // Keep inline playback light; larger tracks remain available
                    // explicitly in the quality menu, including in fullscreen.
                    if (height <= 1080 && (width * height > bestPixels || (width * height === bestPixels && fps > bestFps))) { bestVideo = i; bestPixels = width * height; bestFps = fps }
                }
                qualityOptions = nativeSource.active ? nativeSource.qualityOptions : options.sort((a,b) => b.width * b.height - a.width * a.height || b.fps - a.fps)
                if (!nativeSource.active && !qualityChosen && bestVideo >= 0) {
                    qualityChosen = true
                    media.activeVideoTrack = bestVideo
                }
                if (!audioQualityChosen && media.audioTracks.length > 0) {
                    audioQualityChosen = true
                    let bestAudio = -1, bestBitrate = -1
                    const initialAudio = Math.max(0, Math.min(media.audioTracks.length - 1, media.activeAudioTrack))
                    const language = preferredAudioLanguage || media.audioTracks[initialAudio].stringValue(MediaMetaData.Language)
                    let hasLanguages = false
                    for (const track of media.audioTracks) if (track.stringValue(MediaMetaData.Language)) hasLanguages = true
                    for (let j = 0; j < media.audioTracks.length; ++j) {
                        const candidateLanguage = media.audioTracks[j].stringValue(MediaMetaData.Language)
                        if (language ? candidateLanguage !== language : hasLanguages && j !== initialAudio) continue
                        const bitrate = Number(media.audioTracks[j].value(MediaMetaData.AudioBitRate)) || 0
                        if (bitrate > bestBitrate) { bestAudio = j; bestBitrate = bitrate }
                    }
                    if (bestAudio >= 0) { media.activeAudioTrack = bestAudio; preferredAudioLanguage = media.audioTracks[bestAudio].stringValue(MediaMetaData.Language) }
                }
                updateAspect()
            }
            function updateAspect() {
                const choice = qualityOptions.find(option => option.index === playback.activeQuality)
                if (choice && choice.width > 0 && choice.height > 0) { root.resolvedWidth = choice.width; root.resolvedHeight = choice.height }
            }
            function chooseQuality(index) {
                qualityChosen = true
                if (nativeSource.active) {
                    if (index === nativeSource.activeQuality) return
                    pendingStart = Math.max(pendingStart, media.position)
                    waitingToPlay = true
                    audioQualityChosen = false
                    // setSourceDevice stops the old source synchronously. Its
                    // LoadedMedia signal must not consume the new seek/play
                    // intent before the replacement source has begun loading.
                    switchingSource = true
                    try { nativeSource.selectQuality(index) }
                    finally { switchingSource = false }
                } else media.activeVideoTrack = index
                updateAspect()
            }
            function ready() {
                if (switchingSource) return
                if (media.mediaStatus !== MediaPlayer.LoadedMedia && media.mediaStatus !== MediaPlayer.BufferedMedia) return
                updateTracks()
                if (pendingStart > 0 && media.seekable) {
                    if (media.duration <= 0) return
                    media.position = Math.min(pendingStart, Math.max(0, media.duration - 1)); pendingStart = 0
                }
                if (waitingToPlay) { waitingToPlay = false; if (root.wantPlaying) media.play() }
            }
            function resume() {
                if (media.mediaStatus === MediaPlayer.EndOfMedia && root.bookmarkPosition >= media.duration - 1000) { root.bookmarkPosition = 0; MediaPlayback.remember(root.bookmarkKey,0); media.position = 0 }
                else if (media.seekable && root.bookmarkPosition > 0 && Math.abs(media.position - root.bookmarkPosition) > 100) media.position = root.bookmarkPosition
                media.play()
            }
            function pause() { root.bookmarkPosition = media.position; MediaPlayback.remember(root.bookmarkKey,root.bookmarkPosition); waitingToPlay = false; media.pause() }
            function stop() { nativeSource.cancel(); media.stop(); media.source = "" }
            NativeMediaSource { id: nativeSource; onChanged: playback.updateTracks() }
            function seek(milliseconds) { if (media.seekable) { pendingStart = 0; root.bookmarkPosition = Math.max(0, Math.min(media.duration, milliseconds)); media.position = root.bookmarkPosition } }
            MediaPlayer {
                id: media
                audioOutput: AudioOutput { volume: root.volume; muted: root.audioMuted }
                videoOutput: videoSurface.item ? videoSurface.item.videoSink : null
                loops: root.gif ? MediaPlayer.Infinite : 1
                onTracksChanged: playback.updateTracks()
                onActiveTracksChanged: playback.updateAspect()
                onMediaStatusChanged: { if (media.mediaStatus === MediaPlayer.EndOfMedia) root.wantPlaying = false; playback.ready() }
                onSeekableChanged: value => { if (value && playback.pendingStart > 0) playback.ready() }
                onDurationChanged: value => { if (value > 0 && playback.pendingStart > 0) playback.ready() }
                onPositionChanged: value => { if (value > 0 && !playback.waitingToPlay && root.wantPlaying && playback.playing) root.bookmarkPosition = value }
                onErrorOccurred: (error, errorString) => {
                    if (error === MediaPlayer.NoError || !root.requested) return
                    if (typeof videoResolver.invalidate === "function") videoResolver.invalidate(root.pageUrl || root.sourceUrl)
                    root.mediaError = "Playback was interrupted. Press Play to resume."
                    root.wantPlaying = false
                    root.showControls()
                }
            }
        }
        onLoaded: if (root.resolvedSource) item.begin(root.resolvedSource)
    }
    Item {
        id: inlineHost
        width: root.sourceWidth > 0 && root.sourceHeight > 0 ? root.fitted.width : root.width
        height: root.implicitHeight
        Rectangle {
            id: stage
            anchors.fill: parent
            color: "#080808"; clip: true; radius: root.fullscreen ? 0 : 5
            focus: true
            Keys.onSpacePressed: event => { root.togglePlayback(); event.accepted = true }
            Keys.onEscapePressed: event => { if (root.fullscreen) { root.leaveFullscreen(); event.accepted = true } }
            Keys.onLeftPressed: event => { if (session.item) session.item.seek(root.position - 5000); root.showControls(); event.accepted = true }
            Keys.onRightPressed: event => { if (session.item) session.item.seek(root.position + 5000); root.showControls(); event.accepted = true }
            Image {
                anchors.fill: parent
                visible: !root.hasFrame
                source: MediaUrls.cdnImage(root.posterUrl)
                sourceSize: Qt.size(1280, 720)
                asynchronous: true; fillMode: Image.PreserveAspectFit
            }
            Loader { id: videoSurface; anchors.fill: parent; active: root.requested; visible: root.hasFrame; sourceComponent: VideoSurface {} }
            Connections {
                target: videoSurface.item ? videoSurface.item.videoSink : null
                function onVideoFrameChanged() { if (root.requested && root.resolvedSource) root.hasFrame = true }
            }
            HoverHandler { onPointChanged: root.showControls() }
            TapHandler {
                onTapped: { stage.forceActiveFocus(); root.showControls() }
                onDoubleTapped: root.fullscreen ? root.leaveFullscreen() : root.enterFullscreen()
            }
            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(400, parent.width - 28)
                visible: root.resolving || root.mediaError.length > 0 || (!root.playing && !root.buffering)
                spacing: 10
                BusyIndicator { Layout.alignment: Qt.AlignHCenter; implicitWidth: 34; implicitHeight: 34; running: root.resolving; visible: running }
                Button {
                    id: playButton
                    objectName: "inlineVideoPlay"
                    Layout.alignment: Qt.AlignHCenter
                    visible: !root.resolving
                    implicitWidth: 54; implicitHeight: 54
                    enabled: root.playbackEnabled && !!(root.sourceUrl || root.pageUrl)
                    hoverEnabled: true
                    background: Rectangle { radius: 27; color: playButton.hovered ? "#ed202020" : "#c0202020"; border.width: 1; border.color: "#70ffffff" }
                    contentItem: AppIcon { name: root.mediaError ? "refresh" : "play"; color: "#ffffff" }
                    padding: 14
                    Accessible.name: root.mediaError ? "Retry video" : "Play " + root.title
                    onClicked: { root.play(); stage.forceActiveFocus() }
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.resolving || root.mediaError.length > 0
                    text: root.mediaError || "Preparing video…"
                    textFormat: Text.PlainText; color: root.mediaError ? Theme.danger : "#ffffff"
                    wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: Math.round(12 * preferences.fontScale)
                }
            }
            BusyIndicator { anchors.centerIn: parent; visible: root.buffering && !root.resolving && !root.mediaError; running: visible; implicitWidth: 34; implicitHeight: 34 }
            Rectangle {
                anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                visible: root.fullscreen && root.controlsShown
                height: 48; color: "#b5151515"
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 16; anchors.rightMargin: 8
                    Text { Layout.fillWidth: true; text: root.title || "Video"; textFormat: Text.PlainText; color: "#ffffff"; elide: Text.ElideRight; font.pixelSize: Math.round(14 * preferences.fontScale) }
                    ActionButton { iconName: "close"; hint: "Return to chat · Esc"; quiet: true; onClicked: root.leaveFullscreen() }
                }
            }
            Rectangle {
                id: footer
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                visible: root.controlsShown || !root.playing
                height: footerContent.implicitHeight + 8
                color: "#da141414"
                HoverHandler { id: footerHover }
                ColumnLayout {
                    id: footerContent
                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 7; anchors.verticalCenter: parent.verticalCenter
                    spacing: 0
                    AppSlider {
                        id: seek
                        Layout.fillWidth: true
                        implicitHeight: 20
                        enabled: root.seekable && root.duration > 0
                        from: 0; to: Math.max(1, root.duration)
                        value: root.position
                        onMoved: if (session.item) session.item.seek(value)
                        Accessible.name: "Video position"
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 4
                        ActionButton { iconName: root.wantPlaying ? "pause" : "play"; hint: root.wantPlaying ? "Pause · Space" : "Play · Space"; quiet: true; implicitWidth: 28; implicitHeight: 28; padding: 4; enabled: root.playbackEnabled && !root.resolving; onClicked: root.togglePlayback() }
                        Text { text: root.timeLabel(root.position) + (root.duration > 0 ? " / " + root.timeLabel(root.duration) : ""); color: "#ffffff"; font.pixelSize: Math.round(10 * preferences.fontScale) }
                        Item { Layout.fillWidth: true }
                        ActionButton { iconName: root.audioMuted || root.volume === 0 ? "volumeOff" : "voice"; hint: root.audioMuted ? "Unmute" : "Mute"; quiet: true; implicitWidth: 28; implicitHeight: 28; padding: 4; onClicked: root.audioMuted = !root.audioMuted }
                        AppSlider { id: volumeSlider; Layout.preferredWidth: Math.min(70, stage.width * 0.16); from: 0; to: 1; stepSize: 0.05; value: root.volume; onMoved: root.volume = value; Accessible.name: "Video volume" }
                        NumericValue { visible: stage.width > 540; value: root.volume * 100; minimum: 0; maximum: 100; onValueEdited: value => root.volume = value / 100; Accessible.name: "Video volume percentage" }
                        ActionButton {
                            visible: root.qualityOptions.length > 1
                            iconName: "settings"; hint: "Video quality"; quiet: true
                            implicitWidth: 28; implicitHeight: 28; padding: 4
                            onClicked: qualityMenu.popup()
                            BoundedMenu {
                                id: qualityMenu
                                Repeater {
                                    model: root.qualityOptions
                                    delegate: MenuItem { required property var modelData; text: modelData.label; checkable: true; checked: modelData.index === root.activeQuality; onTriggered: root.chooseQuality(modelData.index) }
                                }
                            }
                        }
                        ActionButton { iconName: "fullscreen"; hint: root.fullscreen ? "Return to chat · Esc" : "Fullscreen"; quiet: true; implicitWidth: 28; implicitHeight: 28; padding: 4; onClicked: root.fullscreen ? root.leaveFullscreen() : root.enterFullscreen() }
                        ActionButton { visible: root.resolving; iconName: "close"; hint: "Cancel loading"; quiet: true; implicitWidth: 28; implicitHeight: 28; padding: 4; onClicked: root.stop() }
                    }
                }
            }
        }
    }
    Timer {
        id: hideControls
        interval: 2200
        onTriggered: { if (root.playing && (footerHover.hovered || seek.pressed || volumeSlider.pressed || qualityMenu.visible)) restart(); else root.controlsShown = !root.playing }
    }
    Loader {
        id: fullscreenLoader
        active: false
        sourceComponent: Window {
            id: fullscreenWindow
            objectName: "inlineVideoWindow"
            title: root.title || "Video"
            transientParent: null
            flags: Qt.Window
            color: "#000000"
            function present() {
                if (ApplicationLifecycle.closing()) return
                const origin = root.Window.window
                if (origin && origin.screen) {
                    screen = origin.screen
                    // Explicit initial bounds keep native creation on this output.
                    x = screen.virtualX; y = screen.virtualY
                    width = screen.width; height = screen.height
                }
                stage.parent = contentItem
                showFullScreen(); raise(); requestActivate(); stage.forceActiveFocus(); root.showControls()
            }
            onClosing: event => { if (!ApplicationLifecycle.closing()) { event.accepted = false; root.leaveFullscreen() } }
        }
        onLoaded: if (root.fullscreen) item.present()
    }
}
