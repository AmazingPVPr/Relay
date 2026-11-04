import QtQuick
import QtQuick.Shapes

Rectangle {
    id: badge
    property bool verified: false
    readonly property real sizeScale: Math.max(0.8, Math.min(1.4, preferences.fontScale))
    implicitWidth: label.implicitWidth + Math.round((verified ? 22 : 10) * sizeScale)
    implicitHeight: Math.round(17 * sizeScale)
    radius: 3
    color: "#5865f2"
    Accessible.role: Accessible.StaticText
    Accessible.name: verified ? "Verified app" : "App"
    Shape {
        visible: badge.verified
        anchors.left: parent.left; anchors.leftMargin: 5 * badge.sizeScale
        anchors.verticalCenter: parent.verticalCenter
        width: 10 * badge.sizeScale; height: 10 * badge.sizeScale
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: "#ffffff"; strokeWidth: 1.6 * badge.sizeScale
            fillColor: "transparent"; capStyle: ShapePath.RoundCap; joinStyle: ShapePath.RoundJoin
            startX: 1 * badge.sizeScale; startY: 5 * badge.sizeScale
            PathLine { x: 4 * badge.sizeScale; y: 8 * badge.sizeScale }
            PathLine { x: 9 * badge.sizeScale; y: 2 * badge.sizeScale }
        }
    }
    Text {
        id: label
        anchors.right: parent.right; anchors.rightMargin: 5 * badge.sizeScale
        anchors.verticalCenter: parent.verticalCenter
        text: "APP"; textFormat: Text.PlainText
        color: "#ffffff"; font.pixelSize: Math.round(9 * badge.sizeScale); font.weight: Font.Bold
    }
}
