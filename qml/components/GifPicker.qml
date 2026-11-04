import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Popup {
    id: picker
    parent: Overlay.overlay
    width: Math.min(440, parent.width - 32)
    height: Math.min(520, parent.height - 32)
    padding: 14; focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    property bool showingCategories: true
    signal insertText(string text)
    function showNear(item) {
        const position = item.mapToItem(parent, 0, 0)
        x = Math.max(16, Math.min(parent.width - width - 16, position.x + item.width - width))
        y = Math.max(16, Math.min(parent.height - height - 16, position.y - height - 8))
        open()
    }
    function runSearch(query) {
        searchDelay.stop()
        showingCategories = false
        if (query.trim()) gifSearch.search(query.trim())
        else gifSearch.trending()
    }
    onOpened: { showingCategories = true; search.clear(); search.forceActiveFocus(); gifSearch.loadCategories() }
    onClosed: { searchDelay.stop(); gifSearch.cancel() }
    background: Rectangle { radius: 8; color: Theme.surface; border.color: Theme.border }
    Timer { id: searchDelay; interval: 350; onTriggered: picker.runSearch(search.text) }
    contentItem: ColumnLayout {
        spacing: 11
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "GIFs"; color: Theme.text; font.pixelSize: Math.round(16 * preferences.fontScale); font.weight: Font.DemiBold }
            ActionButton { iconName: "close"; quiet: true; hint: "Close GIFs"; implicitWidth: 26; implicitHeight: 26; onClicked: picker.close() }
        }
        TextField {
            id: search
            Layout.fillWidth: true
            placeholderText: "Search GIFs"; selectByMouse: true; color: Theme.text
            font.pixelSize: Math.round(13 * preferences.fontScale)
            onTextEdited: searchDelay.restart()
            onAccepted: picker.runSearch(text)
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 6
            ActionButton { text: "Categories"; quiet: true; onClicked: { picker.showingCategories = true; searchDelay.stop() } }
            ActionButton { text: "Trending"; quiet: true; onClicked: { picker.showingCategories = false; searchDelay.stop(); gifSearch.trending() } }
            ActionButton { text: "Favorites"; quiet: true; onClicked: { picker.showingCategories = false; searchDelay.stop(); gifSearch.showFavorites() } }
        }
        Text { Layout.fillWidth: true; visible: gifSearch.error.length > 0; text: gifSearch.error; textFormat: Text.PlainText; color: Theme.warning; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap }
        GridView {
            id: grid
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            cellWidth: Math.floor(width / 2); cellHeight: picker.showingCategories ? 116 : 148
            boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
            model: picker.showingCategories ? (gifSearch.categories.length ? gifSearch.categories : [{name: "hello"}, {name: "lol"}, {name: "love"}, {name: "happy birthday"}]) : gifSearch.results
            delegate: Item {
                id: tile
                required property var modelData
                required property int index
                width: grid.cellWidth; height: grid.cellHeight
                readonly property bool inViewport: y + height >= grid.contentY && y <= grid.contentY + grid.height
                Button {
                    id: imageButton
                    anchors.fill: parent; anchors.margins: 4
                    hoverEnabled: true
                    background: Rectangle { color: imageButton.hovered ? Theme.hover : Theme.canvas; radius: 6; border.width: imageButton.hovered ? 1 : 0; border.color: Theme.focus }
                    contentItem: Item {
                        clip: true
                        RelayAnimatedImage {
                            anchors.fill: parent; anchors.bottomMargin: 28
                            source: picker.visible && tile.inViewport ? (modelData.previewUrl || "") : ""
                            // Network replies are sequential; QMovie needs cached frames to loop safely.
                            asynchronous: true; cache: true; playbackRequested: picker.visible && tile.inViewport && preferences.animateImages && !preferences.reducedMotion
                            fillMode: Image.PreserveAspectCrop
                        }
                        Text { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 7; text: picker.showingCategories ? modelData.name : modelData.title || "GIF"; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(12 * preferences.fontScale); elide: Text.ElideRight }
                    }
                    onClicked: {
                        if (picker.showingCategories) { search.text = modelData.name; picker.runSearch(modelData.name) }
                        else { picker.insertText(modelData.url + " "); picker.close() }
                    }
                }
                ActionButton {
                    anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8
                    visible: !picker.showingCategories
                    text: { const favorites = gifSearch.favorites; return gifSearch.isFavorite(modelData.url || "") ? "Saved" : "Save" }
                    implicitHeight: 24; leftPadding: 7; rightPadding: 7
                    onClicked: gifSearch.setFavorite(modelData, !gifSearch.isFavorite(modelData.url || ""))
                }
            }
            ScrollBoost { flickable: grid }
            ScrollBar.vertical: ScrollBar { width: 5 }
            Text { anchors.centerIn: parent; visible: !gifSearch.busy && !grid.count; text: "No GIFs here yet"; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale) }
            BusyIndicator { anchors.centerIn: parent; running: gifSearch.busy; visible: running; width: 36; height: 36 }
        }
    }
}
