import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: root
    objectName: "forumPostDialog"
    property var service: forumTools
    property var selectedTags: []
    title: "New post in " + (service ? service.channelName : "forum")
    modal: true
    width: Math.min(620, parent ? parent.width - 32 : 620)
    height: Math.min(650, parent ? parent.height - 32 : 650)
    anchors.centerIn: parent
    closePolicy: service && service.posting ? Popup.NoAutoClose : Popup.CloseOnEscape
    onOpened: { postTitle.clear(); postBody.clear(); selectedTags = []; service.clearPostError(); postTitle.forceActiveFocus() }
    function toggleTag(id, checked) {
        const values = selectedTags.slice()
        const at = values.indexOf(id)
        if (checked && at < 0 && values.length < 5) values.push(id)
        else if (!checked && at >= 0) values.splice(at, 1)
        selectedTags = values
    }
    Connections { target: root.service; function onPostCreated(id) { root.close() } }
    contentItem: ColumnLayout {
        spacing: 12
        HardScrollView {
            id: formScroll
            objectName: "forumPostFormScroll"
            Layout.fillWidth: true; Layout.fillHeight: true
            ColumnLayout {
                width: formScroll.availableWidth; spacing: 12
                Label { text: "Title"; font.bold: true }
                TextField { id: postTitle; objectName: "forumPostTitle"; Layout.fillWidth: true; maximumLength: 100; placeholderText: "Give your discussion a title"; enabled: !root.service.posting }
                Label { text: "Message"; font.bold: true }
                TextArea {
                    id: postBody; objectName: "forumPostBody"
                    Layout.fillWidth: true; Layout.preferredHeight: Math.max(160, implicitHeight)
                    textFormat: TextEdit.PlainText; wrapMode: TextEdit.Wrap; placeholderText: "Start the conversation…"; enabled: !root.service.posting; selectByMouse: true
                }
                Label { Layout.alignment: Qt.AlignRight; text: postBody.length + " / 2,000"; color: postBody.length > 2000 ? Theme.danger : Theme.muted }
                Label { text: root.service.tagRequired ? "Tags · choose at least one" : "Tags · choose up to five"; font.bold: true; visible: root.service.tags.length > 0 }
                Flow {
                    Layout.fillWidth: true; spacing: 5; visible: root.service.tags.length > 0
                    Repeater {
                        model: root.service.tags
                        delegate: CheckBox {
                            id: tagCheck
                            required property var modelData
                            text: (modelData.emoji ? modelData.emoji + " " : "") + modelData.name
                            checked: root.selectedTags.indexOf(modelData.id) >= 0
                            enabled: modelData.selectable && !root.service.posting && (checked || root.selectedTags.length < 5)
                            onToggled: root.toggleTag(modelData.id, checked)
                            contentItem: Label { text: tagCheck.text; textFormat: Text.PlainText; font: tagCheck.font; leftPadding: tagCheck.indicator ? tagCheck.indicator.width + tagCheck.spacing : 0; verticalAlignment: Text.AlignVCenter; color: tagCheck.enabled ? Theme.text : Theme.muted }
                            ToolTip.visible: hovered && !modelData.selectable
                            ToolTip.text: "Only moderators can apply this tag"
                        }
                    }
                }
                Label { Layout.fillWidth: true; visible: text.length > 0; text: root.service.postError || root.service.postRestriction; color: Theme.danger; wrapMode: Text.Wrap; textFormat: Text.PlainText }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button { text: "Cancel"; enabled: !root.service.posting; onClicked: root.close() }
            Button {
                objectName: "forumCreatePost"
                text: root.service.posting ? "Creating…" : "Create post"
                highlighted: true
                enabled: root.service.canPost && !root.service.posting && postTitle.text.trim().length > 0 && postBody.text.trim().length > 0 && postBody.length <= 2000 && (!root.service.tagRequired || root.selectedTags.length > 0)
                onClicked: root.service.createPost(postTitle.text, postBody.text, root.selectedTags)
            }
        }
    }
}
