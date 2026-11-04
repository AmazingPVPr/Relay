import QtQuick
import QtQuick.Window
import "Theme.js" as Theme

Item {
    id: icon
    property string name: ""
    property color color: Theme.secondary
    implicitWidth: 20
    implicitHeight: 20
    readonly property string vector: name === "mail" ? "M3 5H21V19H3ZM3 6L12 13 21 6" : name === "check" ? "M4 12L9 17 20 6" : name === "more" ? "M12 4V5M12 11V12M12 18V19" : ""
    AppIcon { anchors.fill: parent; name: icon.name; color: icon.color; visible: !icon.vector.length }
    Image {
        anchors.fill: parent
        visible: icon.vector.length > 0
        source: icon.vector.length ? "data:image/svg+xml," + encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="' + icon.color + '" stroke-width="' + (icon.name === "more" ? 3.2 : 1.8) + '" stroke-linecap="round" stroke-linejoin="round"><path d="' + icon.vector + '"/></svg>') : ""
        sourceSize: Qt.size(Math.max(1, Math.ceil(width * Screen.devicePixelRatio)), Math.max(1, Math.ceil(height * Screen.devicePixelRatio)))
        fillMode: Image.PreserveAspectFit
    }
}
