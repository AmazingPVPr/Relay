import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "Theme.js" as Theme

Item {
    id: viewer
    anchors.fill: parent
    focus: true
    property bool poppedOut: false
    property bool floating: false
    property var playback: stream
    readonly property bool cameraMode: playback !== stream
    signal popOutRequested()
    signal returnToAppRequested()
    signal expandRequested()
    signal minimizeRequested()
    signal moveRequested(real dx, real dy)
    signal stopRequested()
    signal fullscreenRequested()
    readonly property bool cropEnabled: preferences.streamCropEnabled
    readonly property string cropPreset: preferences.streamCropPreset
    readonly property string cropLabel: cropPreset === "custom" ? preferences.streamCropWidth + ":" + preferences.streamCropHeight : cropPreset
    readonly property real cropAspect: {
        const parts = cropPreset.split(":")
        const ratio = cropPreset === "custom" ? preferences.streamCropWidth / preferences.streamCropHeight : Number(parts[0]) / Number(parts[1])
        return Number.isFinite(ratio) && ratio >= 0.1 && ratio <= 10 ? ratio : 16 / 9
    }
    property bool chromeShown: true
    readonly property bool fullscreen: Window.window && Window.window.visibility === Window.FullScreen
    property real zoom: 1
    property real panX: 0
    property real panY: 0
    property real sourceWidth: playback.videoWidth > 0 ? playback.videoWidth : 16
    property real sourceHeight: playback.videoHeight > 0 ? playback.videoHeight : 9
    property real baseScale: cropEnabled
                             ? Math.max(viewport.width / sourceWidth, viewport.height / sourceHeight)
                             : Math.min(viewport.width / sourceWidth, viewport.height / sourceHeight)
    property real maxPanX: Math.max(0, (video.width - viewport.width) / 2)
    property real maxPanY: Math.max(0, (video.height - viewport.height) / 2)

    function clampPan() {
        panX = Math.max(-maxPanX, Math.min(maxPanX, panX))
        panY = Math.max(-maxPanY, Math.min(maxPanY, panY))
    }
    function resetView() { zoom = 1; panX = 0; panY = 0 }
    function chooseCrop(preset) { preferences.streamCropPreset = preset; preferences.streamCropEnabled = true }
    function availableCropPresets() {
        if (playback.frameCount <= 0 || playback.videoWidth <= 0 || playback.videoHeight <= 0) return []
        const sourceAspect = playback.videoWidth / playback.videoHeight
        return ["16:9", "21:9", "32:9", "16:10", "4:3", "1:1"].filter(preset => {
            const parts = preset.split(":")
            return Number(parts[0]) / Number(parts[1]) <= sourceAspect + 0.01
        })
    }
    function beginWatch(key, label, backend) {
        const next = backend || stream
        const newWatch = playback !== next || next.streamKey !== key || !next.active
        if (newWatch) preferences.streamCropEnabled = false
        if (playback !== next) {
            playback.videoSink = null
            playback.stop()
            playback = next
            resetView()
        }
        playback.videoSink = video.videoSink
        if (playback.streamKey !== key || !playback.active) {
            resetView()
            playback.watch(key, label)
        }
        showControls()
        forceActiveFocus()
    }
    function showControls() { chromeShown = true; if (playback.frameCount > 0) hideControls.restart() }
    onZoomChanged: clampPan()
    onMaxPanXChanged: clampPan()
    onMaxPanYChanged: clampPan()
    onCropEnabledChanged: Qt.callLater(clampPan)
    onCropAspectChanged: Qt.callLater(clampPan)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_F && event.modifiers === Qt.NoModifier) { fullscreenRequested(); event.accepted = true }
        else if (event.key === Qt.Key_Escape && fullscreen) { fullscreenRequested(); event.accepted = true }
    }
    HoverHandler { onPointChanged: viewer.showControls() }
    Timer {
        id: hideControls
        interval: 2000
        running: viewer.visible && viewer.chromeShown && playback.frameCount > 0
        onTriggered: {
            if (headerHover.hovered || footerHover.hovered || videoDrag.pressed || cropMenu.visible || customCrop.visible) restart()
            else viewer.chromeShown = false
        }
    }

    Item {
        anchors.fill: parent
        Rectangle {
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            height: viewer.floating ? 36 : 52
            z: 2
            visible: viewer.chromeShown
            color: Theme.sidebar
            HoverHandler { id: headerHover }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: viewer.floating ? 9 : 16; anchors.rightMargin: viewer.floating ? 5 : 12
                spacing: viewer.floating ? 3 : 10
                Item {
                    Layout.fillWidth: true; Layout.minimumWidth: 0; Layout.fillHeight: true
                    RowLayout {
                        anchors.fill: parent; spacing: 7
                        AppIcon { name: "video"; color: Theme.muted; Layout.preferredWidth: 17; Layout.preferredHeight: 17 }
                        Text { Layout.fillWidth: true; text: playback.title || (viewer.cameraMode ? "Camera" : "Stream"); textFormat: Text.PlainText; elide: Text.ElideRight; color: Theme.text; font.pixelSize: Math.round((viewer.floating ? 11 : 14) * preferences.fontScale); font.weight: Font.DemiBold }
                        Text { visible: !viewer.floating; text: playback.receiving ? "LIVE" : playback.connected ? "CONNECTED" : ""; color: playback.receiving ? Theme.success : Theme.muted; font.pixelSize: Math.round(10 * preferences.fontScale); font.weight: Font.DemiBold }
                    }
                    MouseArea {
                        objectName: "streamMoveHandle"
                        anchors.fill: parent
                        enabled: viewer.floating
                        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                        property point previousPosition
                        onPressed: mouse => previousPosition = mapToItem(null, mouse.x, mouse.y)
                        onPositionChanged: mouse => {
                            if (!pressed) return
                            const position = mapToItem(null, mouse.x, mouse.y)
                            viewer.moveRequested(position.x - previousPosition.x, position.y - previousPosition.y)
                            previousPosition = position
                        }
                        onDoubleClicked: viewer.expandRequested()
                    }
                }
                ActionButton { objectName: "streamLayoutButton"; visible: !viewer.poppedOut; iconName: viewer.floating ? "expand" : "minimize"; hint: viewer.floating ? (viewer.cameraMode ? "Expand camera" : "Expand stream") : (viewer.cameraMode ? "Show chat · Float camera" : "Show chat · Float stream"); quiet: true; implicitWidth: viewer.floating ? 26 : 32; implicitHeight: viewer.floating ? 28 : 32; padding: 5; onClicked: viewer.floating ? viewer.expandRequested() : viewer.minimizeRequested() }
                ActionButton { text: viewer.width > 650 ? (viewer.poppedOut ? "Return to app" : "Pop out") : ""; iconName: viewer.poppedOut ? "returnToApp" : "popout"; hint: viewer.poppedOut ? "Return to app" : "Pop out"; quiet: true; onClicked: viewer.poppedOut ? viewer.returnToAppRequested() : viewer.popOutRequested() }
                ActionButton { visible: !viewer.floating; iconName: "fullscreen"; hint: viewer.fullscreen ? "Exit fullscreen · F" : "Fullscreen · F"; quiet: true; onClicked: viewer.fullscreenRequested() }
                ActionButton { iconName: "close"; hint: "Stop watching"; quiet: true; onClicked: viewer.stopRequested() }
            }
        }
        Rectangle {
            id: stage
            anchors.fill: parent
            z: 0
            color: "#0a0a0a"
            Rectangle {
                id: viewport
                anchors.centerIn: parent
                width: viewer.cropEnabled ? Math.min(parent.width, parent.height * viewer.cropAspect) : parent.width
                height: viewer.cropEnabled ? width / viewer.cropAspect : parent.height
                color: "#000000"
                clip: true
                VideoSurface {
                    id: video
                    objectName: "streamVideo"
                    width: viewer.sourceWidth * viewer.baseScale * viewer.zoom
                    height: viewer.sourceHeight * viewer.baseScale * viewer.zoom
                    x: (parent.width - width) / 2 + viewer.panX
                    y: (parent.height - height) / 2 + viewer.panY
                    visible: playback.frameCount > 0
                }
                MouseArea {
                    id: videoDrag
                    anchors.fill: parent
                    enabled: playback.frameCount > 0
                    cursorShape: pressed ? Qt.ClosedHandCursor : viewer.maxPanX > 0 || viewer.maxPanY > 0 ? Qt.OpenHandCursor : Qt.ArrowCursor
                    property real startX: 0
                    property real startY: 0
                    property real startPanX: 0
                    property real startPanY: 0
                    onPressed: mouse => { viewer.forceActiveFocus(); startX = mouse.x; startY = mouse.y; startPanX = viewer.panX; startPanY = viewer.panY }
                    onPositionChanged: mouse => {
                        if (pressed) {
                            viewer.panX = startPanX + mouse.x - startX
                            viewer.panY = startPanY + mouse.y - startY
                            viewer.clampPan()
                        }
                    }
                    onWheel: wheel => { viewer.zoom = Math.max(1, Math.min(3, viewer.zoom + wheel.angleDelta.y / 1200)); wheel.accepted = true }
                    onDoubleClicked: viewer.resetView()
                }
                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(380, parent.width - 48)
                    visible: playback.frameCount === 0
                    spacing: 14
                    BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: playback.active && !playback.error.length; visible: running; implicitWidth: 36; implicitHeight: 36 }
                    AppIcon { Layout.alignment: Qt.AlignHCenter; visible: !playback.active || playback.error.length > 0; name: "video"; color: Theme.muted; width: 34; height: 34 }
                    Text { Layout.fillWidth: true; text: playback.error || playback.status || (viewer.cameraMode ? "Choose a live camera to watch." : "Choose a live stream to watch."); textFormat: Text.PlainText; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: playback.error.length ? Theme.danger : Theme.secondary; font.pixelSize: Math.round(14 * preferences.fontScale) }
                }
            }
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                anchors.bottomMargin: footer.visible ? footer.height : 0
                visible: playback.frameCount > 0 && playback.error.length > 0
                height: playbackError.implicitHeight + 20
                color: Theme.errorSurface
                Text { id: playbackError; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 16; anchors.verticalCenter: parent.verticalCenter; text: playback.error; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale) }
            }
        }
        Rectangle {
            id: footer
            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            z: 2
            visible: viewer.chromeShown
            height: controlsColumn.implicitHeight + (viewer.floating ? 12 : 24)
            color: Theme.sidebar
            HoverHandler { id: footerHover }
            ColumnLayout {
                id: controlsColumn
                anchors.left: parent.left; anchors.right: parent.right; anchors.leftMargin: viewer.floating ? 8 : 16; anchors.rightMargin: viewer.floating ? 24 : 16; anchors.verticalCenter: parent.verticalCenter
                spacing: 9
                RowLayout {
                    Layout.fillWidth: true
                    spacing: viewer.floating ? 4 : 10
                    ActionButton { text: "Fit"; leftPadding: viewer.floating ? 8 : 14; rightPadding: leftPadding; primary: !viewer.cropEnabled; onClicked: preferences.streamCropEnabled = false }
                    ActionButton {
                        text: !viewer.floating && viewer.cropEnabled ? "Crop " + viewer.cropLabel : "Crop"
                        primary: viewer.cropEnabled
                        leftPadding: viewer.floating ? 8 : 14; rightPadding: leftPadding
                        hint: viewer.cropEnabled ? "Crop " + viewer.cropLabel + " · Choose a crop ratio" : "Choose a crop ratio"
                        onClicked: cropMenu.popup()
                        Menu {
                            id: cropMenu
                            Repeater {
                                model: viewer.availableCropPresets()
                                delegate: MenuItem {
                                    required property string modelData
                                    text: modelData
                                    checkable: true
                                    checked: viewer.cropEnabled && viewer.cropPreset === modelData
                                    onTriggered: viewer.chooseCrop(modelData)
                                }
                            }
                            MenuSeparator {}
                            MenuItem { text: "Custom ratio…"; onTriggered: customCrop.open() }
                        }
                    }
                    ActionButton { iconName: "refresh"; hint: "Reset view"; quiet: true; onClicked: viewer.resetView() }
                    Item { Layout.fillWidth: true }
                    Text { objectName: "floatingCameraAudioLabel"; visible: viewer.floating && viewer.cameraMode; text: "Voice audio"; color: Theme.muted; font.pixelSize: Math.round(10 * preferences.fontScale) }
                    ActionButton { objectName: "floatingStreamAudioButton"; visible: viewer.floating && !viewer.cameraMode; iconName: !viewer.cameraMode && playback.audioMuted ? "volumeOff" : "voice"; hint: !viewer.cameraMode && playback.audioMuted ? "Unmute stream" : "Mute stream"; quiet: true; destructive: !viewer.cameraMode && playback.audioMuted; onClicked: playback.audioMuted = !playback.audioMuted }
                    AppSlider { visible: viewer.floating && !viewer.cameraMode; Layout.preferredWidth: 60; from: 0; to: 1; value: viewer.cameraMode ? 0 : playback.volume; onMoved: playback.volume = value; Accessible.name: "Stream volume" }
                }
                RowLayout {
                    visible: !viewer.floating
                    Layout.fillWidth: true
                    spacing: 8
                    Text { text: "Zoom"; color: Theme.secondary; font.pixelSize: Math.round(11 * preferences.fontScale) }
                    AppSlider { Layout.fillWidth: true; from: 1; to: 3; stepSize: 0.05; value: viewer.zoom; onMoved: viewer.zoom = value; Accessible.name: viewer.cameraMode ? "Zoom camera" : "Zoom stream" }
                    NumericValue { value: viewer.zoom * 100; minimum: 100; maximum: 300; onValueEdited: number => viewer.zoom = number / 100; Accessible.name: viewer.cameraMode ? "Camera zoom" : "Stream zoom" }
                    ActionButton { objectName: "streamAudioButton"; visible: !viewer.cameraMode; iconName: !viewer.cameraMode && playback.audioMuted ? "volumeOff" : "voice"; hint: !viewer.cameraMode && playback.audioMuted ? "Unmute stream" : "Mute stream"; quiet: true; destructive: !viewer.cameraMode && playback.audioMuted; onClicked: playback.audioMuted = !playback.audioMuted }
                    AppSlider { visible: !viewer.cameraMode; Layout.preferredWidth: Math.min(90, viewer.width / 5); from: 0; to: 1; value: viewer.cameraMode ? 0 : playback.volume; onMoved: playback.volume = value; Accessible.name: "Stream volume" }
                }
                Text { objectName: "cameraAudioLabel"; visible: !viewer.floating && viewer.cameraMode; Layout.fillWidth: true; text: "Camera audio uses your voice channel controls."; color: Theme.secondary; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap }
                Text { visible: !viewer.floating; Layout.fillWidth: true; text: viewer.cropEnabled || viewer.zoom > 1 ? "Only your view changes. Drag to pan, scroll to zoom, or double-click to reset." : "Crop and zoom change only your view."; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap }
            }
        }
    }
    Dialog {
        id: customCrop
        parent: Overlay.overlay
        width: Math.min(420, parent.width - 48)
        x: (parent.width - width) / 2; y: (parent.height - height) / 2
        padding: 24; modal: true
        property string validationError: ""
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
        onOpened: { ratioWidth.text = String(preferences.streamCropWidth); ratioHeight.text = String(preferences.streamCropHeight); validationError = ""; ratioWidth.forceActiveFocus() }
        function applyRatio() {
            const w = Number(ratioWidth.text), h = Number(ratioHeight.text)
            if (!Number.isFinite(w) || !Number.isFinite(h) || w < 0.1 || h < 0.1 || w > 10000 || h > 10000 || w / h < 0.1 || w / h > 10) {
                validationError = "Enter positive dimensions with a ratio between 1:10 and 10:1."
                return
            }
            if (!preferences.setStreamCropRatio(w, h)) { validationError = "That crop ratio could not be saved."; return }
            viewer.chooseCrop("custom")
            close()
        }
        contentItem: ColumnLayout {
            spacing: 16
            Text { text: "Custom crop ratio"; color: Theme.text; font.pixelSize: Math.round(20 * preferences.fontScale); font.weight: Font.DemiBold }
            Text { Layout.fillWidth: true; text: "Width : height"; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale) }
            RowLayout {
                Layout.fillWidth: true
                AppField { id: ratioWidth; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly; Accessible.name: "Crop width"; onAccepted: customCrop.applyRatio() }
                Text { text: ":"; color: Theme.text }
                AppField { id: ratioHeight; Layout.fillWidth: true; inputMethodHints: Qt.ImhFormattedNumbersOnly; Accessible.name: "Crop height"; onAccepted: customCrop.applyRatio() }
            }
            Text { Layout.fillWidth: true; visible: text.length > 0; text: customCrop.validationError; color: Theme.danger; wrapMode: Text.WordWrap; font.pixelSize: Math.round(12 * preferences.fontScale) }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ActionButton { text: "Cancel"; quiet: true; onClicked: customCrop.close() }
                ActionButton { text: "Apply"; primary: true; onClicked: customCrop.applyRatio() }
            }
        }
    }
}
