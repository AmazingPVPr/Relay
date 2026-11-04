import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "MediaUrls.js" as MediaUrls
import "Theme.js" as Theme

ColumnLayout {
    id: root
    required property var attachment
    property bool playbackEnabled: true
    property bool revealed: false
    readonly property bool hiddenSpoiler: !!attachment.spoiler && !revealed
    readonly property string previewSource: hiddenSpoiler ? "" : MediaUrls.cdnImage(attachment.previewUrl || "")
    readonly property bool videoAttachment: !!attachment.isVideo || MediaUrls.isVideo(attachment.url, attachment.contentType) || (!!attachment.isGif && !MediaUrls.cdnImage(attachment.url))
    signal imageRequested(string source, string originalUrl, string title)
    spacing: 5
    onAttachmentChanged: revealed = false
    Rectangle {
        Layout.fillWidth: true
        Layout.maximumWidth: 460
        implicitHeight: 55
        visible: root.hiddenSpoiler
        color: Theme.rail
        radius: 4
        ActionButton { anchors.centerIn: parent; text: "Reveal spoiler attachment"; quiet: true; onClicked: root.revealed = true }
    }
    ImagePreview {
        Layout.fillWidth: true
        Layout.maximumWidth: Math.min(460, Number(root.attachment.width) || 460)
        visible: !root.hiddenSpoiler && !root.videoAttachment && root.previewSource.length > 0
        playbackEnabled: root.playbackEnabled
        animated: !!root.attachment.isGif || MediaUrls.isGif(source)
        source: root.previewSource
        originalUrl: root.attachment.url || ""
        title: root.attachment.filename || "Image"
        mediaWidth: Number(root.attachment.width) || 0
        mediaHeight: Number(root.attachment.height) || 0
        onActivated: (source, originalUrl, title) => root.imageRequested(source, originalUrl, title)
    }
    Loader {
        Layout.fillWidth: true; Layout.maximumWidth: 460
        active: !root.hiddenSpoiler && root.videoAttachment
        visible: active
        sourceComponent: InlineVideoPlayer {
            playbackEnabled: root.playbackEnabled && !root.hiddenSpoiler
            sourceUrl: root.videoAttachment ? String(root.attachment.videoUrl || root.attachment.url || "") : ""
            posterUrl: root.attachment.posterUrl || root.previewSource
            title: root.attachment.filename || "Video"
            mediaWidth: Number(root.attachment.width) || 0; mediaHeight: Number(root.attachment.height) || 0
            gif: !!root.attachment.isGif
        }
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.maximumWidth: 460
        visible: !root.hiddenSpoiler && !root.videoAttachment && root.previewSource.length === 0
        implicitHeight: attachmentRow.implicitHeight + 18
        color: Theme.surface
        border.color: Theme.border
        radius: 4
        RowLayout {
            id: attachmentRow
            anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 10; anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            AppIcon { name: root.attachment.isVideo ? "video" : "copy"; color: Theme.secondary; Layout.preferredWidth: 22; Layout.preferredHeight: 22 }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 3
                Text { Layout.fillWidth: true; text: root.attachment.filename || "Attachment"; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale); elide: Text.ElideMiddle }
                Text { text: root.attachment.size ? (Number(root.attachment.size) >= 1048576 ? (Number(root.attachment.size)/1048576).toFixed(1) + " MB" : Math.ceil(Number(root.attachment.size)/1024) + " KB") : ""; visible: text.length > 0; color: Theme.muted; font.pixelSize: Math.round(10 * preferences.fontScale) }
            }
            ActionButton { text: "Open"; quiet: true; implicitHeight: 29; enabled: String(root.attachment.url || "").length > 0; onClicked: discord.openAttachment(root.attachment.url) }
        }
    }
}
