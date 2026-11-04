import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Rectangle {
    id: rail
    objectName: "serverRail"
    required property var layoutModel
    property var unreadDmModel: null
    property string currentGuildId: ""
    property string currentChannelId: ""
    readonly property int unreadDmCount: unreadDmModel ? Number(unreadDmModel.count) || 0 : 0
    readonly property bool dragging: dragSourceId.length > 0
    property string dragSourceId: ""
    property var dragSource: ({})
    property point dragPoint: Qt.point(0, 0)
    property string dropTargetId: ""
    property string dropPlacement: ""
    property string centerCandidate: ""
    property bool centerArmed: false
    property real dropLineY: -1
    property int dragGeneration: 0
    signal guildActivated(string guildId)
    signal guildContextRequested(string guildId, string name)
    signal hoverPrepared(string guildId, var anchor)
    signal hoverDismissed(string guildId)
    signal homeActivated()
    signal dmActivated(string channelId)
    signal dmContextRequested(string channelId)
    readonly property real rowHeight: preferences.uiDensity === "compact" ? 52 : preferences.uiDensity === "spacious" ? 64 : 56
    implicitWidth: 72
    color: Theme.rail

    function rowAt(index) {
        if (!layoutModel || index < 0 || index >= layoutModel.count) return ({})
        const value = layoutModel.get(index)
        return value.entry || value
    }
    function findEntry(id) {
        for (let i = 0; layoutModel && i < layoutModel.count; ++i) {
            const value = rowAt(i)
            if (value.id === id) return value
        }
        return ({})
    }
    function parentOf(entry) { return entry.kind === "folder" ? "" : entry.folderId || "" }
    function focusUnreadDm(index) {
        if (index < 0) { homeButton.forceActiveFocus(); return }
        if (index >= unreadDmCount) { homeButton.forceActiveFocus(); return }
        unreadDms.currentIndex = index
        unreadDms.positionViewAtIndex(index, ListView.Contain)
        unreadDms.forceLayout()
        Qt.callLater(() => {
            const row = unreadDms.itemAtIndex(index)
            if (row && row.focusTarget) row.focusTarget.forceActiveFocus()
        })
    }
    function dropAt(sourceId, targetId, placement) {
        if (!layoutModel || !sourceId || sourceId === targetId) return false
        return layoutModel.drop(sourceId, targetId, placement)
    }
    function moveEntry(id, direction) {
        const source = findEntry(id)
        if (!source.id) return false
        let index = -1
        for (let i = 0; i < layoutModel.count; ++i) if (rowAt(i).id === id) { index = i; break }
        for (let i = index + direction; i >= 0 && i < layoutModel.count; i += direction) {
            const candidate = rowAt(i)
            if (parentOf(candidate) === parentOf(source)) return dropAt(id, candidate.id, direction < 0 ? "before" : "after")
        }
        return false
    }
    function moveOut(id) {
        const entry = findEntry(id)
        return entry.kind === "guild" && entry.folderId ? dropAt(id, entry.folderId, "after") : false
    }
    function beginDrag(entry, sceneX, sceneY) {
        cancelDrag()
        dragSourceId = entry.id; dragSource = entry
        hoverDismissed(entry.guildId || "")
        wheel.cancel(); guildList.cancelFlick()
        forceActiveFocus()
        moveDrag(sceneX, sceneY)
    }
    function moveDrag(sceneX, sceneY) {
        if (!dragging) return
        dragPoint = mapFromItem(null, sceneX, sceneY)
        updateTarget()
    }
    function updateTarget() {
        if (!dragging) return
        const point = guildList.contentItem.mapFromItem(rail, dragPoint.x, dragPoint.y)
        const index = guildList.indexAt(point.x, point.y)
        const target = rowAt(index)
        const item = index >= 0 ? guildList.itemAtIndex(index) : null
        dropTargetId = ""; dropPlacement = ""; dropLineY = -1
        if (dragPoint.x < 0 || dragPoint.x > width || dragPoint.y < guildList.y || dragPoint.y > guildList.y + guildList.height) {
            centerCandidate = ""; centerArmed = false; dwell.stop(); return
        }
        if (!target.id || !item) {
            centerCandidate = ""; centerArmed = false; dwell.stop()
            if (point.y >= guildList.contentHeight) { dropPlacement = "after"; dropLineY = guildList.y + Math.min(guildList.height - 2, guildList.contentHeight - guildList.contentY) }
            return
        }
        if (target.id === dragSourceId) { centerCandidate = ""; centerArmed = false; dwell.stop(); return }
        const local = item.mapFromItem(rail, dragPoint.x, dragPoint.y)
        const relative = local.y / item.height
        const canGroup = dragSource.kind !== "folder" && relative > .25 && relative < .75
        if (canGroup) {
            if (target.kind === "folder") {
                centerCandidate = target.id; centerArmed = true; dwell.stop()
            } else if (centerCandidate !== target.id) {
                centerCandidate = target.id; centerArmed = false; dwell.restart()
            }
        } else { centerCandidate = ""; centerArmed = false; dwell.stop() }
        dropTargetId = target.id
        dropPlacement = canGroup && centerArmed ? "into" : relative < .5 ? "before" : "after"
        const top = item.mapToItem(rail, 0, 0).y
        if (dropPlacement !== "into") dropLineY = top + (dropPlacement === "after" ? item.height : 0)
    }
    function finishDrag() {
        if (!dragging) return
        const source = dragSourceId, target = dropTargetId, placement = dropPlacement
        cancelDrag()
        const generation = dragGeneration, account = layoutModel.accountKey
        // Mutating the model only after the pointer gesture finishes keeps the
        // pressed delegate alive while it owns the mouse grab.
        if (placement) Qt.callLater(() => {
            if (rail.dragGeneration === generation && rail.layoutModel.accountKey === account) rail.dropAt(source, target, placement)
        })
    }
    function cancelDrag() {
        ++dragGeneration
        dragSourceId = ""; dragSource = ({}); dropTargetId = ""; dropPlacement = ""
        centerCandidate = ""; centerArmed = false; dropLineY = -1; dwell.stop()
    }
    function openArrangement(entry) {
        arrangeMenu.entryId = entry.id
        arrangeMenu.folder = entry.kind === "folder"
        arrangeMenu.parentFolderId = parentOf(entry)
        arrangeMenu.popup()
    }
    function editFolder(id) {
        const entry = findEntry(id)
        if (entry.kind !== "folder") return
        folderEditor.folderId = id; folderName.text = entry.name || "Server folder"
        folderEditor.folderColor = entry.color || "#5865f2"
        folderEditor.open()
    }
    ColumnLayout {
        anchors.fill: parent; anchors.topMargin: 10; anchors.bottomMargin: 10; spacing: 10
        ListView {
            id: unreadDms
            objectName: "railUnreadDms"
            visible: rail.unreadDmCount > 0
            Layout.fillWidth: true
            Layout.minimumHeight: 0
            Layout.preferredHeight: Math.min(contentHeight, Math.max(rail.rowHeight, Math.min(210, rail.height * .30)))
            model: rail.unreadDmModel
            clip: true; reuseItems: true
            boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
            interactive: !rail.dragging
            delegate: Item {
                id: dmRow
                required property var model
                required property int index
                readonly property string channelId: String(model.recordId || "")
                readonly property string title: String(model.name || "Direct message")
                readonly property int unread: Math.max(0, Number(model.unread) || 0)
                readonly property bool current: rail.currentChannelId === channelId
                readonly property Item focusTarget: dmButton
                objectName: "railDmEntry_" + channelId
                width: unreadDms.width; height: rail.rowHeight
                Rectangle {
                    objectName: "railDmIndicator_" + dmRow.channelId
                    x: 0; anchors.verticalCenter: parent.verticalCenter
                    width: 4; height: dmRow.current ? 36 : dmButton.hovered ? 20 : 8
                    radius: 2; color: Theme.text
                    visible: dmRow.current || dmButton.hovered || dmRow.unread > 0
                }
                Button {
                    id: dmButton
                    objectName: "railDmButton_" + dmRow.channelId
                    anchors.centerIn: parent; width: 48; height: 48; padding: 0
                    hoverEnabled: true; focusPolicy: Qt.StrongFocus
                    background: Rectangle {
                        radius: 24; color: dmRow.current || dmButton.hovered ? Theme.avatar : Theme.canvas
                        border.width: dmButton.activeFocus ? 2 : 0; border.color: Theme.focus
                    }
                    contentItem: Item {
                        Text {
                            objectName: "railDmInitials_" + dmRow.channelId
                            anchors.fill: parent; visible: !dmAvatar.ready
                            text: dmRow.model.initials || dmRow.title.substring(0, 2)
                            textFormat: Text.PlainText; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                            color: Theme.text; font.pixelSize: Math.round(16 * preferences.fontScale); font.weight: Font.Medium
                        }
                        RoundedImage { id: dmAvatar; anchors.fill: parent; source: dmRow.model.avatarUrl || dmRow.model.iconUrl || ""; radius: 24 }
                        Rectangle {
                            objectName: "railDmUnreadBadge_" + dmRow.channelId
                            visible: dmRow.unread > 0
                            anchors.right: parent.right; anchors.bottom: parent.bottom
                            anchors.rightMargin: -3; anchors.bottomMargin: -3
                            width: Math.max(22, unreadCount.implicitWidth + 10); height: 22; radius: 11
                            color: "#ed4245"; border.width: 3; border.color: Theme.rail
                            Text {
                                id: unreadCount; objectName: "railDmUnreadCount_" + dmRow.channelId
                                anchors.centerIn: parent; text: dmRow.unread > 99 ? "99+" : String(dmRow.unread)
                                textFormat: Text.PlainText; color: "#ffffff"; font.pixelSize: Math.round(11 * Math.min(preferences.fontScale, 1.2)); font.weight: Font.Bold
                            }
                        }
                    }
                    onClicked: { homeButton.forceActiveFocus(); rail.dmActivated(dmRow.channelId) }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: { dmTooltip.close(); rail.dmContextRequested(dmRow.channelId) }
                    }
                    Keys.onPressed: event => {
                        if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))) {
                            dmTooltip.close()
                            rail.dmContextRequested(dmRow.channelId)
                            event.accepted = true
                        }
                    }
                    Keys.onReturnPressed: event => { dmButton.clicked(); event.accepted = true }
                    Keys.onEnterPressed: event => { dmButton.clicked(); event.accepted = true }
                    Keys.onUpPressed: event => { rail.focusUnreadDm(dmRow.index - 1); event.accepted = true }
                    Keys.onDownPressed: event => { rail.focusUnreadDm(dmRow.index + 1); event.accepted = true }
                    Accessible.name: dmRow.title + ", " + dmRow.unread + (dmRow.unread === 1 ? " unread message" : " unread messages")
                    Accessible.description: "Open conversation and mark it as read."
                    ToolTip {
                        id: dmTooltip
                        objectName: "railDmTooltip_" + dmRow.channelId
                        visible: dmButton.hovered && !rail.dragging; delay: 500
                        width: Math.min(320, Math.max(100, rail.Window.window ? rail.Window.window.width - 24 : 320))
                        x: dmButton.width + 12; margins: 8
                        text: dmRow.title + " · " + dmRow.unread + (dmRow.unread === 1 ? " unread message" : " unread messages")
                        contentItem: Text { objectName: "railDmTooltipText_" + dmRow.channelId; text: dmTooltip.text; textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.text; font.pixelSize: Math.round(12 * preferences.fontScale) }
                        background: Rectangle { color: Theme.surface; radius: 6; border.color: Theme.border }
                    }
                }
            }
            ScrollBoost { flickable: unreadDms; enabled: !rail.dragging }
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
        }
        Button {
            id: homeButton
            objectName: "railHome"
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 48; implicitHeight: 48
            hoverEnabled: true
            background: Rectangle { radius: 12; color: !rail.currentGuildId || homeButton.hovered ? Theme.avatar : Theme.canvas }
            Rectangle { x: -homeButton.x; anchors.verticalCenter: parent.verticalCenter; width: 4; height: !rail.currentGuildId ? 36 : 20; radius: 2; color: Theme.text; visible: !rail.currentGuildId || homeButton.hovered }
            contentItem: Item { AppIcon { anchors.centerIn: parent; name: "chat"; color: Theme.text; width: 25; height: 25 } }
            onClicked: rail.homeActivated()
            ToolTip.visible: hovered; ToolTip.text: "Direct messages"; ToolTip.delay: 500
            Accessible.name: "Direct messages"
        }
        Rectangle { Layout.alignment: Qt.AlignHCenter; width: 32; height: 2; radius: 1; color: Theme.border }
        ListView {
            id: guildList
            objectName: "serverRailList"
            Layout.fillWidth: true; Layout.fillHeight: true
            boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
            model: rail.layoutModel
            clip: true; reuseItems: true
            interactive: !rail.dragging
            delegate: ServerRailEntry {
                width: guildList.width; height: rail.rowHeight
                currentGuildId: rail.currentGuildId
                beingDragged: rail.dragSourceId === entry.id
                onActivated: {
                    if (rail.dragging) return
                    if (entry.kind === "folder") rail.layoutModel.toggleFolder(entry.id)
                    else { rail.hoverDismissed(entry.guildId); rail.guildActivated(entry.guildId) }
                }
                onContextRequested: {
                    if (entry.kind === "folder") rail.openArrangement(entry)
                    else rail.guildContextRequested(entry.guildId, entry.name)
                }
                onArrangementRequested: rail.openArrangement(entry)
                onReorderRequested: direction => rail.moveEntry(entry.id, direction)
                onMoveOutRequested: rail.moveOut(entry.id)
                onHoverChanged: (hovered, anchor) => {
                    if (entry.kind !== "guild") return
                    if (hovered && !rail.dragging) rail.hoverPrepared(entry.guildId, anchor)
                    else rail.hoverDismissed(entry.guildId)
                }
                onDragStarted: (sceneX, sceneY) => rail.beginDrag(entry, sceneX, sceneY)
                onDragMoved: (sceneX, sceneY) => rail.moveDrag(sceneX, sceneY)
                onDragFinished: rail.finishDrag()
                onDragCancelled: rail.cancelDrag()
            }
            ScrollBoost { id: wheel; flickable: guildList; enabled: !rail.dragging }
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
        }
    }
    Rectangle {
        objectName: "railDropLine"
        visible: rail.dragging && rail.dropLineY >= 0
        x: 10; y: rail.dropLineY - 2; width: parent.width - 20; height: 4; radius: 2; color: "#8891ff"; z: 20
    }
    Rectangle {
        objectName: "railDropGroup"
        visible: rail.dragging && rail.dropPlacement === "into"
        x: (rail.width - 54) / 2; width: 54; height: 54; radius: 15; color: "transparent"; border.width: 3; border.color: "#8891ff"; z: 20
        y: {
            for (let i = 0; rail.layoutModel && i < rail.layoutModel.count; ++i) {
                if (rail.rowAt(i).id === rail.dropTargetId) {
                    const item = guildList.itemAtIndex(i)
                    if (item) return item.mapToItem(rail, 0, 0).y + 1
                }
            }
            return -100
        }
    }
    Rectangle {
        id: ghost
        objectName: "serverDragGhost"
        visible: rail.dragging
        x: rail.dragPoint.x - 24; y: rail.dragPoint.y - 24; width: 48; height: 48; radius: 12
        color: rail.dragSource.kind === "folder" ? rail.dragSource.color || "#5865f2" : Theme.avatar
        opacity: .85; border.width: 2; border.color: Theme.focus; z: 30
        Text { anchors.centerIn: parent; visible: !ghostImage.ready; text: (rail.dragSource.name || "?").substring(0, 2); color: Theme.text; font.pixelSize: 15; textFormat: Text.PlainText }
        RoundedImage { id: ghostImage; anchors.fill: parent; source: rail.dragSource.guild ? rail.dragSource.guild.iconUrl || "" : ""; radius: 12 }
    }
    Timer {
        id: dwell; interval: 350
        onTriggered: {
            if (!rail.dragging || !rail.centerCandidate) return
            rail.centerArmed = true
            rail.updateTarget()
        }
    }
    Timer {
        interval: 16; repeat: true; running: rail.dragging
        onTriggered: {
            const y = rail.dragPoint.y - guildList.y, edge = 34
            let delta = y < edge ? -Math.min(12, (edge - y) / 3) : y > guildList.height - edge ? Math.min(12, (y - guildList.height + edge) / 3) : 0
            if (rail.dragPoint.x < 0 || rail.dragPoint.x > rail.width || y < -20 || y > guildList.height + 20) delta = 0
            if (!delta) return
            const top = guildList.originY - guildList.topMargin
            const bottom = Math.max(top, guildList.originY + guildList.contentHeight + guildList.bottomMargin - guildList.height)
            guildList.contentY = Math.max(top, Math.min(bottom, guildList.contentY + delta))
            rail.updateTarget()
        }
    }
    Shortcut { sequence: "Escape"; enabled: rail.dragging; context: Qt.ApplicationShortcut; onActivated: rail.cancelDrag() }
    Connections { target: rail.layoutModel; ignoreUnknownSignals: true; function onAccountKeyChanged() { rail.cancelDrag(); arrangeMenu.close(); folderEditor.close() } }
    BoundedMenu {
        id: arrangeMenu
        objectName: "serverArrangementMenu"
        property string entryId: ""
        property bool folder: false
        property string parentFolderId: ""
        MenuItem { text: "Open server options"; visible: !arrangeMenu.folder; height: visible ? implicitHeight : 0; onTriggered: { const entry = rail.findEntry(arrangeMenu.entryId); rail.guildContextRequested(entry.guildId, entry.name) } }
        MenuItem { text: "Folder settings"; visible: arrangeMenu.folder; height: visible ? implicitHeight : 0; onTriggered: rail.editFolder(arrangeMenu.entryId) }
        MenuItem { text: "Create server folder"; visible: !arrangeMenu.folder && !arrangeMenu.parentFolderId; height: visible ? implicitHeight : 0; onTriggered: { const id = rail.layoutModel.createFolder(arrangeMenu.entryId); if (id) rail.editFolder(id) } }
        MenuSeparator {}
        MenuItem { text: "Move up"; onTriggered: rail.moveEntry(arrangeMenu.entryId, -1) }
        MenuItem { text: "Move down"; onTriggered: rail.moveEntry(arrangeMenu.entryId, 1) }
        MenuItem { text: "Move out of folder"; visible: !!arrangeMenu.parentFolderId; height: visible ? implicitHeight : 0; onTriggered: rail.moveOut(arrangeMenu.entryId) }
        MenuItem { text: "Ungroup servers"; visible: arrangeMenu.folder; height: visible ? implicitHeight : 0; onTriggered: rail.layoutModel.ungroupFolder(arrangeMenu.entryId) }
    }
    Dialog {
        id: folderEditor
        objectName: "folderEditor"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(400, parent ? parent.width - 32 : 400)
        title: "Folder settings"; modal: true
        standardButtons: Dialog.Save | Dialog.Cancel
        property string folderId: ""
        property string folderColor: "#5865f2"
        onAccepted: rail.layoutModel.updateFolder(folderId, folderName.text.trim() || "Server folder", folderColor)
        onOpened: { folderName.forceActiveFocus(); folderName.selectAll() }
        contentItem: ColumnLayout {
            spacing: 14
            Label { text: "Folder name"; color: Theme.secondary }
            TextField { id: folderName; objectName: "folderName"; Layout.fillWidth: true; maximumLength: 100; selectByMouse: true; Accessible.name: "Folder name" }
            Label { text: "Folder color"; color: Theme.secondary }
            Flow {
                Layout.fillWidth: true; spacing: 8
                Repeater {
                    model: [{color: "#5865f2", name: "Indigo"}, {color: "#4bad7b", name: "Green"}, {color: "#c6a96d", name: "Gold"}, {color: "#ed4245", name: "Red"},
                            {color: "#eb459e", name: "Pink"}, {color: "#9b59b6", name: "Purple"}, {color: "#3498db", name: "Blue"}, {color: "#747f8d", name: "Gray"}]
                    delegate: Button {
                        required property var modelData
                        objectName: "folderColor_" + modelData.color.substring(1)
                        width: 34; height: 34
                        hoverEnabled: true
                        background: Rectangle { radius: 17; color: modelData.color; border.width: folderEditor.folderColor === modelData.color ? 3 : 0; border.color: Theme.text }
                        onClicked: folderEditor.folderColor = modelData.color
                        ToolTip.visible: hovered; ToolTip.text: modelData.name; ToolTip.delay: 500
                        Accessible.name: "Use " + modelData.name.toLowerCase() + " folder color"
                    }
                }
            }
        }
    }
}
