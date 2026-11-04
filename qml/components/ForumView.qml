import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Item {
    id: root
    objectName: "forumView"
    property var service: forumTools
    readonly property var tagOptions: {
        const values = [{id:"", name:"All tags"}]
        if (service) for (const tag of service.tags) values.push({id:tag.id, name:(tag.emoji ? tag.emoji + " " : "") + tag.name})
        return values
    }
    function activityLabel(value) {
        const date = new Date(value)
        return isNaN(date.getTime()) ? "" : date.toLocaleDateString(Qt.locale(), Locale.ShortFormat) + " · " + date.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 20; spacing: 12
        RowLayout {
            Layout.fillWidth: true
            Label { text: "Posts"; font.pixelSize: 22 * preferences.fontScale; font.bold: true; Layout.fillWidth: true; Layout.minimumWidth: 0; elide: Text.ElideRight }
            Button { text: "Refresh"; enabled: !root.service.loading; onClicked: root.service.refresh() }
            Button {
                objectName: "forumNewPost"
                text: "New post"; highlighted: true; enabled: root.service.canPost && !root.service.posting
                onClicked: newPost.open()
                ToolTip.visible: hovered && !enabled; ToolTip.text: root.service.postRestriction
            }
        }
        GridLayout {
            Layout.fillWidth: true; columns: root.width < 520 ? 1 : 2
            TextField { Layout.fillWidth: true; Layout.minimumWidth: 0; placeholderText: "Search loaded posts"; text: root.service.searchText; onTextEdited: root.service.searchText = text }
            ComboBox { Layout.fillWidth: root.width < 520; Layout.preferredWidth: 170 * preferences.fontScale; model: ["Recent activity", "Newest first"]; currentIndex: root.service.sortOrder === "newest" ? 1 : 0; onActivated: root.service.sortOrder = currentIndex === 1 ? "newest" : "activity" }
        }
        RowLayout {
            Layout.fillWidth: true; visible: root.service.tags.length > 0
            Label { text: "Tag"; textFormat: Text.PlainText }
            ComboBox {
                id: tagSelector
                Layout.fillWidth: true; Layout.minimumWidth: 0
                model: root.tagOptions; textRole: "name"
                currentIndex: Math.max(0, root.tagOptions.findIndex(option => option.id === root.service.filterTag))
                onActivated: root.service.filterTag = root.tagOptions[currentIndex].id
                contentItem: Label { text: tagSelector.displayText; textFormat: Text.PlainText; font: tagSelector.font; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter; leftPadding: tagSelector.leftPadding; rightPadding: tagSelector.indicator ? tagSelector.indicator.width + tagSelector.spacing : tagSelector.rightPadding }
                delegate: ItemDelegate {
                    required property var modelData
                    width: tagSelector.width; text: modelData.name
                    contentItem: Label { text: parent.text; textFormat: Text.PlainText; font: tagSelector.font; elide: Text.ElideRight }
                }
            }
        }
        Label { Layout.fillWidth: true; text: root.service.error; visible: text.length > 0; color: "#f08080"; wrapMode: Text.Wrap; textFormat: Text.PlainText }
        ListView {
            id: list
            objectName: "forumPosts"
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            model: root.service.posts; spacing: 8; reuseItems: true; cacheBuffer: 300
            ScrollBar.vertical: ScrollBar {}
            ScrollBoost { flickable: list }
            delegate: Rectangle {
                id: card
                required property string recordId
                required property string name
                required property string author
                required property string timestamp
                required property var forumTags
                required property int replyCount
                required property bool archived
                required property bool locked
                required property bool pinned
                width: list.width
                implicitHeight: details.implicitHeight + 28
                radius: 8; color: hover.hovered ? Theme.hover : Theme.surface
                border.width: 1; border.color: hover.hovered ? Theme.focus : Theme.border
                Accessible.role: Accessible.Button; Accessible.name: name; activeFocusOnTab: true
                Keys.onReturnPressed: root.service.openPost(recordId)
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: root.service.openPost(card.recordId) }
                ColumnLayout {
                    id: details
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 14; spacing: 8
                    Label { Layout.fillWidth: true; text: (card.pinned ? "Pinned · " : "") + card.name; textFormat: Text.PlainText; font.bold: true; font.pixelSize: 16 * preferences.fontScale; wrapMode: Text.Wrap }
                    Flow {
                        Layout.fillWidth: true; spacing: 6; visible: card.forumTags.length > 0
                        Repeater { model: card.forumTags; delegate: Label { required property var modelData; text: "# " + modelData.name; textFormat: Text.PlainText; color: "#c4c7f5"; font.pixelSize: 12 * preferences.fontScale } }
                    }
                    Label { Layout.fillWidth: true; text: card.author + " · " + card.replyCount + (card.replyCount === 1 ? " reply" : " replies") + (card.locked ? " · Locked" : card.archived ? " · Archived" : ""); textFormat: Text.PlainText; color: "#b4b5be"; wrapMode: Text.Wrap }
                    Label { Layout.fillWidth: true; text: root.activityLabel(card.timestamp); color: "#999ba8"; font.pixelSize: 11 * preferences.fontScale; textFormat: Text.PlainText }
                }
            }
            footer: Item {
                width: list.width; height: root.service.loading || root.service.hasMore ? 56 : 0
                BusyIndicator { anchors.centerIn: parent; running: root.service.loading; visible: running; width: 32; height: 32 }
                Button { anchors.centerIn: parent; text: "More posts"; visible: root.service.hasMore && !root.service.loading; onClicked: root.service.loadMore() }
            }
            Label {
                anchors.centerIn: parent; width: Math.min(parent.width - 30, 480)
                visible: list.count === 0 && !root.service.loading
                text: root.service.searchText || root.service.filterTag ? "No loaded posts match. Clear the filters or load more posts." : root.service.error ? "The forum could not be loaded. Use Refresh to try again." : "No posts yet. Start a discussion with New post."
                textFormat: Text.PlainText; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; color: "#b4b5be"
            }
        }
    }
    ForumPostDialog { id: newPost; parent: Overlay.overlay; service: root.service }
}
