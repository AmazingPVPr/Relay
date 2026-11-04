import QtQuick
import QtQuick.Controls
import "Theme.js" as Theme

TextField {
    id: field
    selectByMouse: true
    placeholderText: "Search messages"
    property var suggestions: []
    property int selectedSuggestion: -1
    property bool dismissed: false
    function refreshSuggestions() {
        suggestions = activeFocus && !dismissed ? channelTools.searchSuggestions(text, cursorPosition) : []
        selectedSuggestion = -1
    }
    function complete(index) {
        if (index < 0 || index >= suggestions.length) return
        const choice = suggestions[index]
        text = choice.text
        cursorPosition = choice.cursor
        dismissed = false
        forceActiveFocus()
        refreshSuggestions()
    }
    onTextEdited: { dismissed = false; refreshSuggestions(); memberLookup.restart() }
    onCursorPositionChanged: refreshSuggestions()
    onActiveFocusChanged: { dismissed = false; refreshSuggestions() }
    Keys.priority: Keys.BeforeItem
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape && completions.visible) {
            dismissed = true; suggestions = []; event.accepted = true
        } else if ((event.key === Qt.Key_Down || event.key === Qt.Key_Up) && suggestions.length) {
            selectedSuggestion = (selectedSuggestion + (event.key === Qt.Key_Down ? 1 : suggestions.length - 1) + suggestions.length) % suggestions.length
            choices.positionViewAtIndex(selectedSuggestion, ListView.Contain)
            event.accepted = true
        } else if ((event.key === Qt.Key_Tab || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && suggestions.length && (selectedSuggestion >= 0 || event.key === Qt.Key_Tab)) {
            complete(selectedSuggestion < 0 ? 0 : selectedSuggestion); event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            dismissed = true; suggestions = []
        }
    }
    Timer {
        id: memberLookup
        interval: 300
        onTriggered: if (field.activeFocus && !field.dismissed) channelTools.requestSearchSuggestions(field.text, field.cursorPosition)
    }
    Connections {
        target: channelTools
        function onSearchSuggestionsChanged() { field.refreshSuggestions() }
    }
    Popup {
        id: completions
        parent: field
        y: field.height + 5
        width: Math.max(field.width, 320)
        height: Math.min(360, choices.contentHeight + 42)
        visible: field.activeFocus && field.suggestions.length > 0
        focus: false
        padding: 6
        closePolicy: Popup.NoAutoClose
        background: Rectangle { color: Theme.sidebar; radius: 6; border.color: Theme.border }
        contentItem: Column {
            spacing: 4
            Text { text: "SEARCH OPTIONS  ·  ↑↓ choose · Tab complete"; color: Theme.muted; font.pixelSize: 10; height: 24; verticalAlignment: Text.AlignVCenter }
            ListView {
                id: choices
                width: parent.width
                height: Math.min(310, contentHeight)
                clip: true
                model: field.suggestions
                delegate: ItemDelegate {
                    required property var modelData
                    required property int index
                    width: choices.width; height: 42
                    highlighted: index === field.selectedSuggestion
                    background: Rectangle { radius: 4; color: parent.highlighted || parent.hovered ? Theme.surface : "transparent" }
                    contentItem: Column {
                        Text { text: modelData.label; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 13; width: parent.width; elide: Text.ElideRight }
                        Text { text: modelData.detail; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: 10; width: parent.width; elide: Text.ElideRight }
                    }
                    onClicked: field.complete(index)
                }
                ScrollBar.vertical: ScrollBar {}
            }
        }
    }
}
