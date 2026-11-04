import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: panel
    parent: Overlay.overlay
    property bool pinsMode: false
    property string channelId: ""
    readonly property bool busy: pinsMode ? channelTools.pinsBusy : channelTools.searchBusy
    readonly property bool more: pinsMode ? channelTools.pinsHasMore : channelTools.searchHasMore
    signal imageRequested(string source, string originalUrl, string title)
    signal userRequested(string userId, var fallback)
    signal translationRequested(string text)
    signal contextRequested(string userId, var fallback, string guildId)
    width: Math.min(740, parent.width - 40)
    height: Math.min(680, parent.height - 40)
    x: (parent.width - width) / 2; y: (parent.height - height) / 2
    padding: 16; modal: false; focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    function openPins(id) { pinsMode = true; channelId = id; open(); channelTools.readPins(id) }
    function openSearch(query) { pinsMode = false; queryField.text = query || ""; open(); queryField.forceActiveFocus(); if (queryField.text.trim()) channelTools.searchMessages(queryField.text.trim()) }
    background: Rectangle { radius: 8; color: Theme.canvas; border.color: Theme.border }
    contentItem: ColumnLayout {
        spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: panel.pinsMode ? "Pinned messages" : "Search messages"; color: Theme.text; font.pixelSize: Math.round(19 * preferences.fontScale); font.weight: Font.DemiBold }
            ActionButton { iconName: "close"; quiet: true; hint: "Close"; onClicked: panel.close() }
        }
        RowLayout {
            visible: !panel.pinsMode
            Layout.fillWidth: true
            MessageSearchField { id: queryField; Layout.fillWidth: true; placeholderText: "Search messages"; selectByMouse: true; onAccepted: if (text.trim()) channelTools.searchMessages(text.trim()); font.pixelSize: Math.round(13 * preferences.fontScale) }
            ActionButton { iconName: "search"; hint: "Search"; enabled: queryField.text.trim().length > 0 && !panel.busy; onClicked: channelTools.searchMessages(queryField.text.trim()) }
        }
        Text { Layout.fillWidth: true; visible: !panel.pinsMode && channelTools.totalResults > 0; text: channelTools.totalResults + " results"; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale) }
        Text { Layout.fillWidth: true; visible: !panel.pinsMode && text.length > 0; text: channelTools.searchStatus || channelTools.searchScope; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale); wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; visible: text.length > 0; text: (panel.pinsMode ? channelTools.pinsError : channelTools.searchError) || channelTools.actionError; textFormat: Text.PlainText; color: Theme.warning; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        ListView {
            id: results
            Layout.fillWidth: true; Layout.fillHeight: true
            model: panel.pinsMode ? channelTools.pins : channelTools.searchResults
            clip: true; reuseItems: true; spacing: 10
            boundsBehavior: Flickable.StopAtBounds; boundsMovement: Flickable.StopAtBounds
            delegate: Rectangle {
                id: result
                required property var model
                property bool mediaActive: true
                ListView.onPooled: mediaActive = false
                ListView.onReused: mediaActive = true
                width: results.width - 8
                height: resultMessage.height + 44
                radius: 5; color: Theme.sidebar
                MessageItem {
                    id: resultMessage; width: parent.width; message: result.model; compact: false; mediaActive: result.mediaActive && panel.visible
                    onImageRequested: (source, originalUrl, title) => panel.imageRequested(source, originalUrl, title)
                    onUserRequested: (userId, fallback) => panel.userRequested(userId, fallback)
                    onTranslationRequested: text => panel.translationRequested(text)
                    onContextRequested: (userId, fallback) => panel.contextRequested(userId, fallback, result.model.guildId || discord.currentGuildId)
                }
                ActionButton {
                    anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 7
                    implicitHeight: 29; topPadding: 4; bottomPadding: 4
                    text: "Jump to message"; quiet: true; enabled: !channelTools.jumpBusy
                    onClicked: channelTools.openMessage(result.model.channelId || panel.channelId || discord.currentChannelId, result.model.recordId)
                }
            }
            footer: Item {
                width: results.width; height: panel.more ? 44 : 0
                ActionButton { anchors.centerIn: parent; visible: panel.more; text: panel.busy ? "Loading…" : "Load more"; enabled: !panel.busy; onClicked: panel.pinsMode ? channelTools.loadMorePins() : channelTools.loadMoreSearch() }
            }
            Text { anchors.centerIn: parent; width: parent.width - 40; visible: !panel.busy && !results.count; text: panel.pinsMode ? "No pinned messages." : "No matching messages."; color: Theme.muted; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter }
            BusyIndicator { anchors.centerIn: parent; visible: panel.busy && !results.count; running: visible; width: 40; height: 40 }
            ScrollBoost { flickable: results }
            ScrollBar.vertical: ScrollBar { width: 6 }
        }
    }
    Connections { target: channelTools; function onMessageOpened() { panel.close() } }
}
