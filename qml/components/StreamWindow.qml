import QtQuick
import QtQuick.Controls
import "Theme.js" as Theme
import "ApplicationLifecycle.js" as ApplicationLifecycle

ApplicationWindow {
    id: viewer
    objectName: "streamWindow"
    width: 1120; height: 740
    minimumWidth: 640; minimumHeight: 440
    visible: false
    flags: Qt.Window
    transientParent: null
    property var presentedPlayer: null
    title: (presentedPlayer && presentedPlayer.playback ? (presentedPlayer.playback.title || (presentedPlayer.cameraMode ? "Camera" : "Stream")) : "Stream") + " — Relay"
    color: Theme.rail
    font.family: "Inter"
    font.pixelSize: Math.round(13 * preferences.fontScale)
    palette.windowText: Theme.text; palette.text: Theme.text
    palette.buttonText: Theme.text; palette.button: Theme.raised
    palette.highlight: Theme.avatar
    signal returnToAppRequested()
    function presentPlayer(player, fullscreen) {
        if (ApplicationLifecycle.closing()) return
        presentedPlayer = player
        const origin = player.Window.window
        if (!visible && origin && origin !== viewer && origin.screen) {
            screen = origin.screen
            if (fullscreen) {
                x = screen.virtualX; y = screen.virtualY
            } else {
                x = screen.virtualX + Math.max(0, (screen.width - width) / 2)
                y = screen.virtualY + Math.max(0, (screen.height - height) / 2)
            }
        }
        player.parent = contentItem
        player.poppedOut = true
        if (fullscreen) showFullScreen()
        else show()
        // This runs only after the user explicitly chooses Pop out/Fullscreen.
        raise()
        requestActivate()
        player.showControls()
        player.forceActiveFocus()
    }
    function toggleFullscreen() {
        if (visibility === Window.FullScreen) showNormal()
        else showFullScreen()
    }
    onClosing: close => { if (!ApplicationLifecycle.closing()) { close.accepted = false; returnToAppRequested() } }
}
