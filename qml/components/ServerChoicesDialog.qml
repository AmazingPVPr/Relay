import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: root
    property var service
    modal: true
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(680, parent.width - 32)
    height: Math.min(720, parent.height - 48)
    padding: 20
    closePolicy: Popup.CloseOnEscape
    title: service && service.guildId.length ? service.guildName + " — Channels & Roles" : "Channel unavailable"
    onClosed: if (service) service.close()
    background: Rectangle { color: Theme.canvas; radius: 10; border.color: Theme.border }
    header: ColumnLayout {
        spacing: 12
        RowLayout {
            Layout.fillWidth: true; Layout.margins: 20; Layout.bottomMargin: 0
            Label { Layout.fillWidth: true; text: root.title; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: 19; wrapMode: Text.Wrap; font.weight: Font.DemiBold }
            ActionButton { objectName: "serverChoicesClose"; iconName: "close"; hint: "Close"; quiet: true; onClicked: root.close() }
        }
        TabBar {
            visible: root.service && root.service.guildId.length > 0
            Layout.fillWidth: true; Layout.leftMargin: 20; Layout.rightMargin: 20
            currentIndex: root.service && root.service.section === "browse" ? 1 : 0
            TabButton {
                text: "Customize"; implicitHeight: 36
                contentItem: Text { text: parent.text; color: parent.checked ? Theme.text : Theme.muted; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: parent.checked ? Theme.selected : parent.hovered ? Theme.hover : Theme.sidebar; radius: 5 }
                onClicked: root.service.openGuild(root.service.guildId, "customize")
            }
            TabButton {
                text: "Browse Channels"; implicitHeight: 36
                contentItem: Text { text: parent.text; color: parent.checked ? Theme.text : Theme.muted; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { color: parent.checked ? Theme.selected : parent.hovered ? Theme.hover : Theme.sidebar; radius: 5 }
                onClicked: root.service.openGuild(root.service.guildId, "browse")
            }
        }
    }
    contentItem: ColumnLayout {
        spacing: 12
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.service ? root.service.error : ""
            textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.danger
        }
        Label {
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.service ? root.service.status : ""
            textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.success
        }
        BusyIndicator { Layout.alignment: Qt.AlignHCenter; visible: root.service && root.service.loading; running: visible }
        ScrollView {
            objectName: "serverChoicesQuestions"
            visible: root.service && root.service.guildId.length > 0 && root.service.section === "customize"
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                width: parent.width
                spacing: 20
                visible: root.service && root.service.section === "customize"
                Label {
                    Layout.fillWidth: true
                    text: "Choose the server’s interests, roles, and channel options, then save your choices."
                    textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.secondary
                }
                Repeater {
                    model: root.service ? root.service.prompts : []
                    delegate: ColumnLayout {
                        id: prompt
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            Layout.fillWidth: true
                            text: prompt.modelData.title + (prompt.modelData.required ? " *" : "")
                            textFormat: Text.PlainText; color: Theme.text; wrapMode: Text.Wrap; font.weight: Font.DemiBold
                        }
                        Repeater {
                            model: prompt.modelData.options
                            delegate: CheckBox {
                                id: option
                                required property var modelData
                                objectName: "serverChoiceOption_" + modelData.id
                                Layout.fillWidth: true
                                enabled: root.service && !root.service.loading && !root.service.saving
                                checked: modelData.selected
                                Accessible.name: modelData.title
                                onClicked: root.service.choose(prompt.modelData.id, modelData.id, checked)
                                leftPadding: 32; rightPadding: 8; topPadding: 8; bottomPadding: 8
                                indicator: Rectangle {
                                    x: 2; y: (option.height - height) / 2
                                    width: 20; height: 20
                                    radius: prompt.modelData.single ? 10 : 4
                                    border.width: 1; border.color: option.checked ? Theme.accent : Theme.muted
                                    color: option.checked ? Theme.accent : "transparent"
                                    Text { anchors.centerIn: parent; visible: option.checked; text: prompt.modelData.single ? "●" : "✓"; color: Theme.onAccent; font.pixelSize: 14 }
                                }
                                background: Rectangle { color: option.hovered ? Theme.hover : "transparent"; radius: 5 }
                                contentItem: ColumnLayout {
                                    spacing: 3
                                    Label { Layout.fillWidth: true; text: option.modelData.title; textFormat: Text.PlainText; color: Theme.text; wrapMode: Text.Wrap }
                                    Label { Layout.fillWidth: true; visible: text.length > 0; text: option.modelData.description; textFormat: Text.PlainText; color: Theme.muted; wrapMode: Text.Wrap; font.pixelSize: 12 }
                                }
                            }
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.service && !root.service.loading && !root.service.error.length && !root.service.prompts.length
                    text: "This server has no customization questions available."
                    textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.muted
                }
            }
        }
        ListView {
            id: browseChannels
            objectName: "serverChoicesChannels"
            visible: root.service && root.service.section === "browse"
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; reuseItems: true; spacing: 6
            boundsBehavior: Flickable.StopAtBounds
            model: visible && root.service ? root.service.channels : []
            ScrollBar.vertical: ScrollBar {}
            delegate: Button {
                id: channel
                required property var modelData
                objectName: "serverChoiceChannel_" + modelData.id
                width: browseChannels.width
                text: (modelData.kind === "voice" || modelData.kind === "stage" ? "🔊 " : "# ") + modelData.name
                Accessible.name: modelData.name
                background: Rectangle { color: channel.hovered ? Theme.hover : Theme.surface; radius: 5 }
                contentItem: ColumnLayout {
                    Label { Layout.fillWidth: true; text: channel.text; textFormat: Text.PlainText; color: Theme.text; wrapMode: Text.Wrap }
                    Label { Layout.fillWidth: true; visible: text.length > 0; text: channel.modelData.category; textFormat: Text.PlainText; color: Theme.muted; font.pixelSize: 11 }
                }
                onClicked: if (root.service.activate("relay-channel:" + modelData.id)) root.close()
            }
        }
    }
    footer: RowLayout {
        spacing: 10
        Item { Layout.fillWidth: true }
        Button {
            text: "Retry"
            visible: root.service && root.service.guildId.length > 0 && root.service.section === "customize"
            enabled: root.service && !root.service.loading && !root.service.saving && !root.service.dirty
            implicitHeight: 34
            contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.muted; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: parent.hovered ? Theme.hover : Theme.surface; radius: 5 }
            onClicked: root.service.refresh()
        }
        Button {
            objectName: "serverChoicesSave"
            text: root.service && root.service.saving ? "Saving…" : "Save Changes"
            visible: root.service && root.service.guildId.length > 0 && root.service.section === "customize"
            enabled: root.service && root.service.dirty && !root.service.loading && !root.service.saving
            implicitHeight: 34; leftPadding: 16; rightPadding: 16
            contentItem: Text { text: parent.text; color: Theme.onAccent; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { color: parent.enabled ? parent.hovered ? Theme.accentHover : Theme.accent : Theme.selected; radius: 5 }
            onClicked: root.service.save()
        }
        Item { implicitWidth: 10 }
    }
}
