import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "MediaUrls.js" as MediaUrls
import "Theme.js" as Theme

Dialog {
    id: dialog
    property bool editing: false
    property string imageSource: ""
    property string originalUrl: ""
    property string imageTitle: "Image"
    property bool animationPaused: preferences.reducedMotion
    property string accountId: ""
    property string channelId: ""
    readonly property bool animated: MediaUrls.isGif(imageSource) || MediaUrls.isGif(originalUrl)
    readonly property bool video: MediaUrls.isVideo(imageSource) || MediaUrls.isVideo(originalUrl)
    parent: Overlay.overlay
    width: Math.min(1100, parent.width - 70)
    height: Math.min(820, parent.height - 70)
    x: (parent.width - width) / 2; y: (parent.height - height) / 2
    modal: true; padding: 16
    closePolicy: dialog.editing ? Popup.NoAutoClose : Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.rail; radius: 6; border.color: Theme.border }
    onClosed: { imageSource = ""; editing = false }
    onOpened: { accountId = discord.currentUserId; channelId = discord.currentChannelId; animationPaused = preferences.reducedMotion; editing = false }
    Connections { target: discord; function onStateChanged() { if (dialog.visible && (dialog.accountId !== discord.currentUserId || dialog.channelId !== discord.currentChannelId)) dialog.close() } }
    Dialog {
        id: discardDialog
        property bool closeViewer: false
        anchors.centerIn: parent
        title: "Discard edits?"
        modal: true
        standardButtons: Dialog.Discard | Dialog.Cancel
        Label { text: "Save or copy your image before discarding edits."; color: Theme.text }
        onDiscarded: { if (closeViewer) dialog.close(); else dialog.editing = false; closeViewer = false }
        onRejected: closeViewer = false
    }
    contentItem: ColumnLayout {
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: dialog.imageTitle; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); elide: Text.ElideMiddle }
            ActionButton { visible: dialog.animated && !dialog.video && !dialog.editing; iconName: dialog.animationPaused ? "play" : "pause"; hint: dialog.animationPaused ? "Play GIF" : "Pause GIF"; quiet: true; onClicked: dialog.animationPaused = !dialog.animationPaused }
            ActionButton { text: dialog.editing ? "View image" : "Edit image"; visible: !dialog.video; quiet: true; enabled: !(dialog.editing && viewer.item && viewer.item.posting); onClicked: { if (dialog.editing && viewer.item && viewer.item.modified) discardDialog.open(); else dialog.editing = !dialog.editing } }
            ActionButton { text: "Open original"; quiet: true; enabled: dialog.originalUrl.length > 0; onClicked: discord.openAttachment(dialog.originalUrl) }
            ActionButton { iconName: "close"; hint: "Close image"; quiet: true; enabled: !(dialog.editing && viewer.item && viewer.item.posting); onClicked: { if (dialog.editing && viewer.item && viewer.item.modified) { discardDialog.closeViewer = true; discardDialog.open() } else dialog.close() } }
        }
        Loader {
            id: viewer
            Layout.fillWidth: true; Layout.fillHeight: true
            active: dialog.visible && dialog.imageSource.length > 0
            sourceComponent: dialog.editing ? editorViewer : dialog.video ? videoViewer : dialog.animated ? animatedViewer : stillViewer
        }
        Component {
            id: editorViewer
            ImageEditPanel { source: MediaUrls.cdnImage(dialog.originalUrl) || MediaUrls.cdnImage(dialog.imageSource); imageTitle: dialog.imageTitle }
        }
        Component {
            id: stillViewer
            Image { source: MediaUrls.cdnImage(dialog.imageSource); asynchronous: true; cache: true; sourceSize: Qt.size(4096, 4096); fillMode: Image.PreserveAspectFit }
        }
        Component {
            id: animatedViewer
            RelayAnimatedImage {
                objectName: "mediaViewerAnimation"
                source: MediaUrls.cdnImage(dialog.imageSource)
                // Network replies are sequential; QMovie needs cached frames to loop safely.
                asynchronous: true; cache: true
                fillMode: Image.PreserveAspectFit
                playbackRequested: dialog.visible && !dialog.animationPaused
            }
        }
        Component {
            id: videoViewer
            InlineVideoPlayer {
                sourceUrl: dialog.originalUrl || dialog.imageSource
                title: dialog.imageTitle
                maximumHeight: viewer.height
                playbackEnabled: dialog.visible
            }
        }
    }
}
