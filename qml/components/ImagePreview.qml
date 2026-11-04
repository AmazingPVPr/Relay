import QtQuick
import QtQuick.Controls
import QtQuick.Window
import "MediaUrls.js" as MediaUrls
import "Theme.js" as Theme

Item {
    id: root
    property string source: ""
    property string originalUrl: ""
    property string title: "Image"
    property int mediaWidth: 0
    property int mediaHeight: 0
    property int maximumHeight: 320
    property bool playbackEnabled: true
    property bool animated: MediaUrls.isGif(source)
    signal activated(string source, string originalUrl, string title)
    readonly property real naturalWidth: mediaWidth > 0 && mediaHeight > 0 ? mediaWidth : preview.item && preview.item.implicitWidth > 0 ? preview.item.implicitWidth : 320
    readonly property real naturalHeight: mediaWidth > 0 && mediaHeight > 0 ? mediaHeight : preview.item && preview.item.implicitHeight > 0 ? preview.item.implicitHeight : 240
    readonly property var fitted: MediaUrls.imageSize(naturalWidth, naturalHeight, Math.max(1, width), maximumHeight)
    implicitWidth: Math.min(460, naturalWidth, maximumHeight * naturalWidth / naturalHeight)
    implicitHeight: fitted.height
    Rectangle {
        width: root.fitted.width
        height: root.fitted.height
        color: Theme.surface
        radius: 4
        clip: true
        Loader {
            id: preview
            active: root.visible && root.playbackEnabled
            anchors.fill: parent
            sourceComponent: root.animated ? animatedPreview : stillPreview
        }
        Component {
            id: stillPreview
            Image {
                source: MediaUrls.cdnImage(root.source)
                asynchronous: true; cache: true
                sourceSize: Qt.size(Math.min(1024,Math.ceil(root.fitted.width * Screen.devicePixelRatio / 64) * 64), Math.min(1024,Math.ceil(root.fitted.height * Screen.devicePixelRatio / 64) * 64))
                fillMode: Image.PreserveAspectFit
            }
        }
        Component {
            id: animatedPreview
            RelayAnimatedImage {
                source: MediaUrls.cdnImage(root.source)
                // Network replies are sequential; QMovie needs cached frames to loop safely.
                asynchronous: true; cache: true
                sourceSize: Qt.size(Math.max(1,Math.ceil(root.fitted.width * Screen.devicePixelRatio)), Math.max(1,Math.ceil(root.fitted.height * Screen.devicePixelRatio)))
                fillMode: Image.PreserveAspectFit
                playbackRequested: root.visible && root.playbackEnabled && !preferences.reducedMotion && preferences.animateImages
            }
        }
        Text {
            anchors.centerIn: parent
            visible: !preview.item || preview.item.status !== Image.Ready
            text: preview.item && preview.item.status === Image.Error ? "Image unavailable" : "Loading image…"
            font.pixelSize: Math.round(12 * preferences.fontScale)
            color: Theme.muted
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: if (MediaUrls.cdnImage(root.source)) root.activated(root.source, root.originalUrl, root.title)
        }
    }
    Accessible.role: Accessible.Button
    Accessible.name: "View " + title
}
