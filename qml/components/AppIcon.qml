import QtQuick
import QtQuick.Window
import "Theme.js" as Theme
import "IconPaths.js" as Icons

Item {
    id: icon
    property string name: ""
    property color color: Theme.secondary
    implicitWidth: 20
    implicitHeight: 20
    Image {
    anchors.fill: parent
    source: !Icons.path(icon.name).length ? "" : "data:image/svg+xml," + encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="' + icon.color + '" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="' + Icons.path(icon.name) + '"/></svg>')
    sourceSize: Qt.size(Math.max(1, Math.ceil(width * Screen.devicePixelRatio)), Math.max(1, Math.ceil(height * Screen.devicePixelRatio)))
    fillMode: Image.PreserveAspectFit
    smooth: true
    antialiasing: true
}
}
