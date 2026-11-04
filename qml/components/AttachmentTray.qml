import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Rectangle {
    id: root
    property var service: typeof uploads !== "undefined" ? uploads : null
    readonly property int attachmentCount: service ? service.count : 0
    readonly property bool uploading: !!service && service.busy
    readonly property string errorText: service ? String(service.error || "") : ""
    readonly property real cardHeight: Math.round(147 * preferences.fontScale)
    signal addRequested()
    property string previewAttachmentId: ""
    readonly property var previewAttachment: {
        const files = root.service ? root.service.attachments : []
        return files.find(file => String(file.id) === root.previewAttachmentId) || ({})
    }
    readonly property string selectedPreviewSource: root.previewSource(previewAttachment)
    onSelectedPreviewSourceChanged: if (draftPreview.visible && !selectedPreviewSource.length) draftPreview.close()
    function openPreview(attachment) {
        if (!previewSource(attachment).length) return
        previewAttachmentId = String(attachment.id)
        draftPreview.open()
    }
    DraftImagePreview {
        id: draftPreview
        imageSource: root.selectedPreviewSource
        imageTitle: String(root.previewAttachment.name || "Image")
        onClosed: root.previewAttachmentId = ""
    }
    visible: attachmentCount > 0 || uploading || errorText.length > 0
    implicitWidth: 480
    implicitHeight: visible ? content.implicitHeight + 24 : 0
    color: Theme.sidebar
    radius: 6
    border.color: Theme.border

    function typeLabel(attachment) {
        const mime = String(attachment.mime || "")
        if (mime.startsWith("image/")) return "Image"
        if (mime.startsWith("video/")) return "Video"
        if (mime.startsWith("audio/")) return "Audio"
        if (mime === "application/pdf") return "PDF"
        const name = String(attachment.name || "")
        const dot = name.lastIndexOf(".")
        return dot > 0 && dot < name.length - 1 ? name.substring(dot + 1).toUpperCase().substring(0, 12) + " file" : "File"
    }
    function previewSource(attachment) {
        const source = String(attachment.previewUrl || "")
        const size = Number(attachment.size || 0)
        const mime = String(attachment.mime || "").toLowerCase()
        // The service also bounds image dimensions before exposing previewUrl.
        return /^file:(?:\/[^\/]|\/\/\/|\/\/localhost\/)/i.test(source) && size > 0 && size <= 8 * 1024 * 1024
                && /^(image\/png|image\/jpeg|image\/webp|image\/avif|image\/gif|image\/bmp)$/.test(mime) ? source : ""
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 9

        RowLayout {
            Layout.fillWidth: true
            visible: root.attachmentCount > 0
            spacing: 8
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    Layout.fillWidth: true
                    text: "Attachments · " + root.attachmentCount + "/10"
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: Math.round(12 * preferences.fontScale)
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: root.service ? String(root.service.limitText || "") : ""
                    textFormat: Text.PlainText
                    color: Theme.muted
                    font.pixelSize: Math.round(10 * preferences.fontScale)
                    elide: Text.ElideRight
                }
            }
            ActionButton {
                objectName: "attachmentAddFiles"
                text: "Add files"
                quiet: true
                implicitHeight: 28
                enabled: !root.uploading && root.attachmentCount < 10
                onClicked: root.addRequested()
            }
            ActionButton {
                objectName: "attachmentClearAll"
                text: "Clear all"
                quiet: true
                implicitHeight: 28
                enabled: !root.uploading
                onClicked: root.service.clearDraft()
            }
        }

        ListView {
            id: cards
            Layout.fillWidth: true
            Layout.preferredHeight: root.cardHeight + (contentWidth > width ? 8 : 0)
            visible: root.attachmentCount > 0
            orientation: ListView.Horizontal
            spacing: 8
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            boundsMovement: Flickable.StopAtBounds
            model: root.service ? root.service.attachments : []
            ScrollBar.horizontal: ScrollBar { height: 5; policy: ScrollBar.AsNeeded }
            delegate: Rectangle {
                id: card
                required property var modelData
                readonly property string preview: root.previewSource(modelData)
                width: Math.min(194 * preferences.fontScale, cards.width)
                height: root.cardHeight
                color: Theme.surface
                radius: 5
                border.color: Theme.border
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4
                    Button {
                        id: previewButton
                        objectName: "attachmentPreview_" + String(card.modelData.id)
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 24
                        padding: 0
                        enabled: card.preview.length > 0
                        hoverEnabled: true
                        HoverHandler { cursorShape: previewButton.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
                        Accessible.name: "Preview " + String(card.modelData.name || "image")
                        onClicked: root.openPreview(card.modelData)
                        ToolTip.visible: hovered && enabled
                        ToolTip.delay: 500
                        ToolTip.text: "Preview image"
                        background: Rectangle {
                            color: Theme.canvas
                            radius: 3
                            border.width: previewButton.activeFocus || previewButton.hovered && previewButton.enabled ? 1 : 0
                            border.color: Theme.focus
                        }
                        contentItem: Item {
                        clip: true
                        Image {
                            id: thumbnail
                            anchors.fill: parent
                            anchors.margins: 3
                            source: card.visible && !card.modelData.spoiler ? card.preview : ""
                            sourceSize: Qt.size(128, 128)
                            asynchronous: true
                            cache: false
                            fillMode: Image.PreserveAspectFit
                        }
                        AppIcon {
                            anchors.centerIn: parent
                            width: 27; height: 27
                            name: card.modelData.spoiler ? "lock" : String(card.modelData.mime || "").startsWith("video/") ? "video" : "copy"
                            visible: card.modelData.spoiler || thumbnail.status !== Image.Ready
                            color: Theme.secondary
                        }
                        }
                    }
                    Text {
                        id: fileName
                        Layout.fillWidth: true
                        text: String(card.modelData.name || "Attachment")
                        textFormat: Text.PlainText
                        color: Theme.text
                        font.pixelSize: Math.round(12 * preferences.fontScale)
                        elide: Text.ElideMiddle
                        HoverHandler { id: nameHover }
                        ToolTip {
                            id: nameTip
                            visible: nameHover.hovered && fileName.truncated
                            delay: 500
                            contentItem: Text {
                                text: fileName.text
                                textFormat: Text.PlainText
                                font: nameTip.font
                                color: nameTip.palette.toolTipText
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.typeLabel(card.modelData) + (card.modelData.sizeLabel ? " · " + String(card.modelData.sizeLabel) : "")
                        textFormat: Text.PlainText
                        color: Theme.muted
                        font.pixelSize: Math.round(10 * preferences.fontScale)
                        elide: Text.ElideRight
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        ActionButton {
                            objectName: "attachmentSpoiler_" + String(card.modelData.id)
                            text: card.modelData.spoiler ? "Spoiler on" : "Spoiler"
                            iconName: "lock"
                            hint: card.modelData.spoiler ? "Remove spoiler" : "Mark as spoiler"
                            Accessible.description: String(card.modelData.name || "Attachment")
                            primary: !!card.modelData.spoiler
                            quiet: !card.modelData.spoiler
                            implicitHeight: 25
                            topPadding: 3; bottomPadding: 3; leftPadding: 6; rightPadding: 6
                            Layout.fillWidth: true
                            enabled: !root.uploading
                            onClicked: root.service.setSpoiler(card.modelData.id, !card.modelData.spoiler)
                        }
                        ActionButton {
                            objectName: "attachmentRemove_" + String(card.modelData.id)
                            iconName: "close"
                            hint: "Remove attachment"
                            Accessible.description: String(card.modelData.name || "Attachment")
                            quiet: true
                            implicitWidth: 25; implicitHeight: 25
                            topPadding: 3; bottomPadding: 3; leftPadding: 3; rightPadding: 3
                            enabled: !root.uploading
                            onClicked: root.service.removeAttachment(card.modelData.id)
                        }
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: root.uploading
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: root.service ? String(root.service.status || "Uploading attachments…") : ""
                    textFormat: Text.PlainText
                    color: Theme.secondary
                    font.pixelSize: Math.round(11 * preferences.fontScale)
                    wrapMode: Text.WordWrap
                }
                ActionButton {
                    objectName: "attachmentCancel"
                    text: "Cancel"
                    quiet: true
                    implicitHeight: 27
                    visible: root.service && root.service.cancelable
                    onClicked: root.service.cancel()
                }
            }
            ProgressBar {
                id: progress
                Layout.fillWidth: true
                from: 0; to: 1
                value: root.service ? Math.max(0, Math.min(1, Number(root.service.progress || 0))) : 0
                implicitHeight: 4
                background: Rectangle { radius: 2; color: Theme.border }
                contentItem: Item {
                    Rectangle { width: progress.visualPosition * parent.width; height: parent.height; radius: 2; color: Theme.accent }
                }
                Accessible.name: "Attachment upload progress"
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: errorRow.implicitHeight + 12
            visible: root.errorText.length > 0
            radius: 4
            color: Theme.errorSurface
            RowLayout {
                id: errorRow
                anchors.left: parent.left; anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 6
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: root.errorText
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: Theme.danger
                    font.pixelSize: Math.round(11 * preferences.fontScale)
                    Accessible.role: Accessible.AlertMessage
                }
                ActionButton {
                    objectName: "attachmentDismissError"
                    iconName: "close"
                    quiet: true
                    hint: "Dismiss attachment error"
                    implicitHeight: 24; implicitWidth: 24
                    topPadding: 3; bottomPadding: 3; leftPadding: 3; rightPadding: 3
                    onClicked: root.service.clearError()
                }
            }
        }
    }
}
