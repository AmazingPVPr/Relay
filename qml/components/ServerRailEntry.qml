import QtQuick
import QtQuick.Controls
import "Theme.js" as Theme

Item {
    id: row
    required property var entry
    property string currentGuildId: ""
    property bool beingDragged: false
    readonly property bool folder: entry.kind === "folder"
    readonly property var guild: entry.guild || ({})
    readonly property var folderGuilds: entry.children || []
    readonly property bool current: folder ? !entry.expanded && containsCurrent() : entry.guildId === currentGuildId
    readonly property bool unread: folder ? anyFlag("unread") : !!guild.unread
    readonly property bool voiceActive: folder ? anyFlag("voiceActive") : !!guild.voiceActive
    readonly property bool streaming: folder ? anyFlag("streaming") : !!guild.streaming
    readonly property bool hovered: iconButton.hovered
    signal activated()
    signal contextRequested()
    signal arrangementRequested()
    signal reorderRequested(int direction)
    signal moveOutRequested()
    signal hoverChanged(bool hovered, var anchor)
    signal dragStarted(real sceneX, real sceneY)
    signal dragMoved(real sceneX, real sceneY)
    signal dragFinished()
    signal dragCancelled()
    objectName: "railEntry_" + entry.id
    implicitHeight: preferences.uiDensity === "compact" ? 52 : preferences.uiDensity === "spacious" ? 64 : 56

    function containsCurrent() {
        for (let i = 0; i < folderGuilds.length; ++i)
            if ((folderGuilds[i].recordId || folderGuilds[i].id) === currentGuildId) return true
        return false
    }
    function anyFlag(flag) {
        for (let i = 0; i < folderGuilds.length; ++i) if (folderGuilds[i][flag]) return true
        return false
    }
    Rectangle {
        objectName: "folderChildren_" + row.entry.folderId
        visible: !row.folder && !!row.entry.folderId || row.folder && !!row.entry.expanded
        anchors.horizontalCenter: parent.horizontalCenter
        y: 0; width: 56; height: parent.height; radius: row.folder ? 16 : 0
        color: row.entry.color || "#5865f2"; opacity: .16
    }
    Rectangle {
        objectName: "railIndicator_" + row.entry.id
        x: 0; anchors.verticalCenter: parent.verticalCenter
        width: 4; height: row.current ? 36 : iconButton.hovered ? 20 : 8
        radius: 2; color: Theme.text
        visible: row.current || iconButton.hovered || row.unread
    }
    Button {
        id: iconButton
        objectName: (row.folder ? "folderToggle_" : "railButton_") + row.entry.id
        anchors.centerIn: parent
        width: 48; height: 48; padding: 0
        opacity: row.beingDragged ? .35 : 1
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        background: Rectangle {
            radius: row.folder ? 14 : 12
            color: row.folder ? row.entry.color || "#5865f2" : row.current || iconButton.hovered ? Theme.avatar : Theme.canvas
            opacity: row.folder ? row.entry.expanded || iconButton.hovered ? .5 : .26 : 1
            border.width: iconButton.activeFocus ? 2 : 0; border.color: Theme.focus
        }
        contentItem: Item {
            Text {
                anchors.fill: parent
                visible: !row.folder && !guildImage.ready
                text: row.guild.initials || (row.entry.name || "?").substring(0, 2)
                textFormat: Text.PlainText; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                color: Theme.text; font.pixelSize: Math.round(16 * preferences.fontScale); font.weight: Font.Medium
            }
            RoundedImage { id: guildImage; anchors.fill: parent; source: row.folder ? "" : row.guild.iconUrl || ""; radius: 12 }
            Grid {
                visible: row.folder && !row.entry.expanded
                anchors.centerIn: parent; columns: 2; spacing: 3
                Repeater {
                    model: row.folderGuilds.slice(0, 4)
                    delegate: Rectangle {
                        required property var modelData
                        width: 17; height: 17; radius: 5; color: Theme.canvas
                        Text { anchors.centerIn: parent; visible: !miniImage.ready; text: modelData.initials || (modelData.name || "?").substring(0, 1); color: Theme.text; textFormat: Text.PlainText; font.pixelSize: 9 }
                        RoundedImage { id: miniImage; anchors.fill: parent; source: modelData.iconUrl || ""; radius: 5 }
                    }
                }
            }
            Item {
                anchors.centerIn: parent; visible: row.folder && !!row.entry.expanded; width: 27; height: 23
                Rectangle { x: 1; y: 0; width: 12; height: 7; radius: 3; color: row.entry.color || "#5865f2" }
                Rectangle { x: 0; y: 4; width: 27; height: 19; radius: 4; color: row.entry.color || "#5865f2" }
            }
            Rectangle {
                objectName: "railVoiceBadge_" + row.entry.id
                visible: row.voiceActive || row.streaming
                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.rightMargin: -3; anchors.bottomMargin: -3
                width: 21; height: 21; radius: 10.5; color: row.streaming ? Theme.danger : Theme.success
                border.width: 2; border.color: Theme.rail
                AppIcon { anchors.centerIn: parent; width: 13; height: 13; name: row.streaming ? "video" : "voice"; color: "white" }
            }
            Rectangle {
                objectName: "railUnreadBadge_" + row.entry.id
                visible: row.unread && !row.current
                anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.leftMargin: -2; anchors.bottomMargin: -2
                width: 10; height: 10; radius: 5; color: Theme.danger; border.width: 2; border.color: Theme.rail
            }
        }
        onClicked: row.activated()
        onHoveredChanged: row.hoverChanged(hovered, iconButton)
        TapHandler { acceptedButtons: Qt.RightButton; onTapped: row.contextRequested() }
        DragHandler {
            id: drag
            target: null
            acceptedButtons: Qt.LeftButton
            onActiveChanged: {
                if (active) row.dragStarted(centroid.scenePosition.x, centroid.scenePosition.y)
                else row.dragFinished()
            }
            onCentroidChanged: if (active) row.dragMoved(centroid.scenePosition.x, centroid.scenePosition.y)
            onCanceled: row.dragCancelled()
        }
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Menu || event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier)) {
                row.arrangementRequested(); event.accepted = true
            } else if ((event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                row.reorderRequested(event.key === Qt.Key_Up ? -1 : 1); event.accepted = true
            } else if ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_Left) {
                row.moveOutRequested(); event.accepted = true
            } else if (row.folder && (event.key === Qt.Key_Left && row.entry.expanded || event.key === Qt.Key_Right && !row.entry.expanded)) {
                row.activated(); event.accepted = true
            }
        }
        ToolTip.visible: row.folder && hovered && !row.beingDragged
        ToolTip.text: row.entry.name + " · " + row.folderGuilds.length + " servers"
        ToolTip.delay: 500
        Accessible.name: row.entry.name || "Server"
        Accessible.description: row.folder ? (row.entry.expanded ? "Expanded server folder. " : "Collapsed server folder. ") + "Press Menu for folder options. Control Up or Down changes its order."
                                            : "Press Menu for server arrangement options. Control Up or Down changes its order. Control Left moves it out of a folder."
    }
    Component.onDestruction: if (drag.active) row.dragCancelled()
}
