import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Relay.Native 1.0
import "Theme.js" as Theme
import "MediaUrls.js" as MediaUrls

ColumnLayout {
    id: root
    property string source: ""
    property string imageTitle: "Image"
    property string tool: "pen"
    property color ink: "#ff4055"
    property string feedback: ""
    readonly property bool modified: editor.modified
    readonly property bool posting: typeof imageUploads !== "undefined" && imageUploads.busy
    spacing: 8
    Flow {
        Layout.fillWidth: true
        spacing: 4
        Repeater {
            model: [{key:"pen",label:"Pen"},{key:"line",label:"Line"},{key:"rectangle",label:"Rectangle"},{key:"ellipse",label:"Ellipse"},{key:"crop",label:"Crop"}]
            ActionButton { required property var modelData; text: modelData.label; primary: root.tool === modelData.key; enabled: editor.ready; onClicked: root.tool = modelData.key }
        }
        Repeater {
            model: ["#ff4055", "#ffcf40", "#45d483", "#4897ff", "#ffffff", "#111111"]
            Button {
                required property string modelData
                width: 30; height: 36
                Accessible.name: "Ink " + modelData
                onClicked: root.ink = modelData
                background: Rectangle { anchors.centerIn: parent; width: 22; height: 22; radius: 11; color: parent.modelData; border.width: root.ink.toString() === parent.modelData ? 3 : 1; border.color: root.ink.toString() === parent.modelData ? Theme.focus : Theme.muted }
            }
        }
        Label { text: "Width"; color: Theme.text; height: 36; verticalAlignment: Text.AlignVCenter }
        SpinBox { id: penWidth; from: 1; to: 100; value: 6; editable: true; width: 100; Accessible.name: "Stroke width in image pixels" }
    }
    Label {
        Layout.fillWidth: true
        visible: MediaUrls.isGif(root.source)
        text: "GIF edits save and repost a still PNG image."
        color: Theme.muted
        wrapMode: Text.Wrap
    }
    Flow {
        Layout.fillWidth: true; spacing: 4
        ActionButton { text: "Undo"; enabled: editor.canUndo; onClicked: editor.undo() }
        ActionButton { text: "Redo"; enabled: editor.canRedo; onClicked: editor.redo() }
        ActionButton { text: "Rotate"; enabled: editor.ready; onClicked: editor.rotate() }
        ActionButton { text: "Flip"; enabled: editor.ready; onClicked: editor.flip() }
        ActionButton { text: "Reset"; enabled: editor.modified; onClicked: editor.reset() }
        Label { text: editor.ready ? editor.imageSize.width + " × " + editor.imageSize.height : ""; color: Theme.muted; height: 36; verticalAlignment: Text.AlignVCenter }
    }
    Rectangle {
        Layout.fillWidth: true; Layout.fillHeight: true
        color: Theme.surface; clip: true
        ImageEditor {
            id: editor
            objectName: "imageEditorCanvas"
            anchors.fill: parent
            source: root.source
            onSourceChanged: root.feedback = ""
        }
        MouseArea {
            id: drawing
            anchors.fill: parent
            enabled: editor.ready && !root.posting
            cursorShape: Qt.CrossCursor
            property point start: Qt.point(0,0)
            property point end: Qt.point(0,0)
            property bool selecting: false
            function boundedPoint(x,y) {
                const r = editor.imageRect
                return Qt.point(Math.max(r.x,Math.min(r.x+r.width,x)),Math.max(r.y,Math.min(r.y+r.height,y)))
            }
            onPressed: mouse => {
                const r = editor.imageRect
                if (mouse.x < r.x || mouse.y < r.y || mouse.x > r.x+r.width || mouse.y > r.y+r.height) { mouse.accepted = false; return }
                root.feedback = ""
                start = boundedPoint(mouse.x,mouse.y); end = start
                selecting = root.tool === "crop"
                editor.begin(start,root.tool,root.ink,penWidth.value)
            }
            onPositionChanged: mouse => { if (pressed) { end = boundedPoint(mouse.x,mouse.y); editor.move(end) } }
            onReleased: { editor.finish(); selecting = false }
            onCanceled: { editor.cancelStroke(); selecting = false }
        }
        Rectangle {
            visible: drawing.selecting
            x: Math.min(drawing.start.x,drawing.end.x); y: Math.min(drawing.start.y,drawing.end.y)
            width: Math.abs(drawing.start.x-drawing.end.x); height: Math.abs(drawing.start.y-drawing.end.y)
            color: "#304897ff"; border.color: "white"; border.width: 2
        }
        Label { anchors.centerIn: parent; width: parent.width - 30; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; visible: !editor.ready; text: editor.status; color: Theme.text }
    }
    Label {
        Layout.fillWidth: true
        text: root.feedback || (root.posting ? imageUploads.status : editor.status) || (root.tool === "crop" ? "Drag to crop. Undo restores the previous image." : "Drag to draw. Save and copy create a PNG with your edits.")
        textFormat: Text.PlainText; color: Theme.muted; wrapMode: Text.Wrap
    }
    Flow {
        Layout.fillWidth: true; spacing: 6
        ActionButton { text: "Save as…"; enabled: editor.ready && !root.posting; onClicked: saveDialog.open() }
        ActionButton { text: "Copy image"; enabled: editor.ready && !root.posting; onClicked: { root.feedback = ""; editor.copy() } }
        ActionButton {
            objectName: "imageEditorRepost"
            text: "Save & repost"; primary: true
            hint: "Send this edited image as a new message in " + discord.currentChannelName
            enabled: editor.ready && !root.posting && typeof imageUploads !== "undefined"
            onClicked: {
                root.feedback = ""
                const url = editor.stage()
                if (!url.toString().length) return
                imageUploads.clearDraft()
                if (imageUploads.addFiles([url]) && !imageUploads.send("")) root.feedback = imageUploads.error || "Wait for the current message to finish sending, then try again."
                if (imageUploads.error.length) root.feedback = imageUploads.error
            }
        }
    }
    Connections {
        target: typeof imageUploads !== "undefined" ? imageUploads : null
        function onChanged() {
            if (!imageUploads.busy) root.feedback = imageUploads.error || imageUploads.status
        }
    }
    FileDialog {
        id: saveDialog
        title: "Save edited image"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: ["PNG images (*.png)"]
        selectedNameFilter.index: 0
        onAccepted: { root.feedback = ""; editor.saveAs(selectedFile) }
    }
}
