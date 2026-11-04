import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "MediaUrls.js" as MediaUrls
import "Theme.js" as Theme

Rectangle {
    id: root
    required property var embed
    property bool playbackEnabled: true
    property var metadata: ({})
    signal userRequested(string userId)
    readonly property bool videoEmbed: embed.kind === "video" || embed.kind === "gifv" || String(embed.videoUrl || "").length > 0
    readonly property string authorIcon: MediaUrls.cdnImage(embed.authorIconUrl || "")
    readonly property string footerIcon: MediaUrls.cdnImage(embed.footerIconUrl || "")
    readonly property string timestampLabel: {
        const value = String(embed.timestamp || "")
        const language = preferences.language, format = preferences.timeFormat
        return value.length ? preferences.formatDateTime(value) : ""
    }
    signal imageRequested(string source, string originalUrl, string title)
    implicitWidth: 520
    implicitHeight: content.implicitHeight + 24
    radius: 4
    color: Theme.surface
    border.width: 1
    border.color: Theme.border

    function isLink(value) { return /^https?:\/\//i.test(String(value || "")) }
    function openLink(value) { if (isLink(value)) discord.openLink(String(value)) }
    function fieldRows() {
        const rows = [], fields = embed.fields || []
        const columns = content.width < 360 ? 2 : 3
        let inlineFields = []
        for (const field of fields) {
            if (field.inline) {
                inlineFields.push(field)
                if (inlineFields.length === columns) { rows.push(inlineFields); inlineFields = [] }
            } else {
                if (inlineFields.length) { rows.push(inlineFields); inlineFields = [] }
                rows.push([field])
            }
        }
        if (inlineFields.length) rows.push(inlineFields)
        return rows
    }
    Rectangle {
        objectName: "embedAccent"
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
        width: 4; radius: 2
        color: /^#[0-9a-f]{6}$/i.test(String(root.embed.accentColor || "")) ? root.embed.accentColor : Theme.border
    }
    ColumnLayout {
        id: content
        anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: 16; anchors.rightMargin: 14
        y: 12
        spacing: 8
        Text {
            objectName: "embedProvider"
            text: root.embed.provider || ""; textFormat: Text.PlainText
            visible: text.length > 0 && !root.embed.rich
            color: Theme.muted; font.pixelSize: Math.round(10 * preferences.fontScale)
        }
        RowLayout {
            objectName: "embedAuthorRow"
            Layout.fillWidth: true
            visible: String(root.embed.author || "").length > 0 || root.authorIcon.length > 0
            spacing: 8
            RoundedImage {
                objectName: "embedAuthorIcon"
                Layout.preferredWidth: 24; Layout.preferredHeight: 24
                visible: source.length > 0
                source: root.authorIcon
                radius: 12; decodeSize: 48
                MouseArea { anchors.fill: parent; enabled: root.isLink(root.embed.authorUrl); cursorShape: Qt.PointingHandCursor; onClicked: root.openLink(root.embed.authorUrl) }
            }
            Text {
                objectName: "embedAuthor"
                Layout.fillWidth: true
                text: root.embed.author || ""; textFormat: Text.PlainText
                wrapMode: Text.WordWrap; color: Theme.text
                font.pixelSize: Math.round(preferences.chatFontSize * 12 / 14); font.weight: Font.DemiBold
                MouseArea { objectName: "embedAuthorLink"; anchors.fill: parent; enabled: root.isLink(root.embed.authorUrl); cursorShape: Qt.PointingHandCursor; onClicked: root.openLink(root.embed.authorUrl) }
            }
        }
        Text {
            objectName: "embedTitle"
            Layout.fillWidth: true; text: root.embed.title || ""; textFormat: Text.PlainText; visible: text.length > 0
            wrapMode: Text.WordWrap; color: root.isLink(root.embed.url) ? preferences.highContrast ? "#9bd7ff" : "#83bcf1" : preferences.highContrast ? "#ffffff" : Theme.text
            font.underline: root.isLink(root.embed.url) && preferences.underlineLinks
            font.pixelSize: Math.round(preferences.chatFontSize); font.weight: Font.DemiBold
            MouseArea { objectName: "embedTitleLink"; anchors.fill: parent; enabled: root.isLink(root.embed.url); cursorShape: Qt.PointingHandCursor; onClicked: root.openLink(root.embed.url) }
        }
        LinkText {
            objectName: "embedDescription"
            metadata: root.metadata
            onUserRequested: id => root.userRequested(id)
            Layout.fillWidth: true
            rawText: root.embed.description || ""; visible: rawText.length > 0
            wrapMode: TextEdit.Wrap; color: preferences.highContrast ? "#ffffff" : Theme.secondary; font.pixelSize: Math.round(preferences.chatFontSize * 13 / 14)
            readOnly: true; selectByMouse: true; selectionColor: Theme.selected
        }
        Repeater {
            model: root.fieldRows()
            delegate: RowLayout {
                id: fieldRow
                required property var modelData
                Layout.fillWidth: true
                spacing: 12
                Repeater {
                    model: fieldRow.modelData
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true; Layout.preferredWidth: 0; Layout.minimumWidth: 0
                        Layout.alignment: Qt.AlignTop
                        spacing: 4
                        LinkText { objectName: "embedFieldName"; Layout.fillWidth: true; rawText: modelData.name || ""; metadata: root.metadata; onUserRequested: id => root.userRequested(id); wrapMode: TextEdit.Wrap; color: preferences.highContrast ? "#ffffff" : Theme.text; font.pixelSize: Math.round(preferences.chatFontSize * 12 / 14); font.weight: Font.DemiBold; readOnly: true; selectByMouse: true; selectionColor: Theme.selected }
                        LinkText { objectName: "embedFieldValue"; Layout.fillWidth: true; rawText: modelData.value || ""; metadata: root.metadata; onUserRequested: id => root.userRequested(id); wrapMode: TextEdit.Wrap; color: preferences.highContrast ? "#ffffff" : Theme.secondary; font.pixelSize: Math.round(preferences.chatFontSize * 12 / 14); readOnly: true; selectByMouse: true; selectionColor: Theme.selected }
                    }
                }
            }
        }
        ImagePreview {
            Layout.fillWidth: true
            Layout.maximumWidth: root.embed.imageUrl ? 480 : 120
            visible: !root.videoEmbed && source.length > 0
            playbackEnabled: root.playbackEnabled
            animated: !!root.embed.gif || MediaUrls.isGif(source)
            source: MediaUrls.cdnImage(root.embed.imageUrl || root.embed.thumbnailUrl || "")
            originalUrl: source
            title: root.embed.title || "Image"
            mediaWidth: Number(root.embed.imageUrl ? root.embed.imageWidth : root.embed.thumbnailWidth) || 0
            mediaHeight: Number(root.embed.imageUrl ? root.embed.imageHeight : root.embed.thumbnailHeight) || 0
            maximumHeight: 340
            onActivated: (source, originalUrl, title) => root.imageRequested(source, originalUrl, title)
        }
        Loader {
            objectName: "embedVideoLoader"
            Layout.fillWidth: true
            Layout.maximumWidth: item ? item.implicitWidth : 460
            Layout.alignment: Qt.AlignLeft
            active: root.videoEmbed
            visible: active
            sourceComponent: InlineVideoPlayer {
                playbackEnabled: root.playbackEnabled && visible
                sourceUrl: String(root.embed.videoUrl || "")
                pageUrl: String(root.embed.pageUrl || root.embed.url || "")
                posterUrl: root.embed.posterUrl || root.embed.imageUrl || root.embed.thumbnailUrl || ""
                title: root.embed.title || "Video"
                mediaWidth: Number(root.embed.videoWidth || root.embed.imageWidth || root.embed.thumbnailWidth) || 0
                mediaHeight: Number(root.embed.videoHeight || root.embed.imageHeight || root.embed.thumbnailHeight) || 0
                maximumHeight: 340
                gif: !!root.embed.gif
                startSeconds: Number(root.embed.startSeconds) || 0
            }
        }
        RowLayout {
            objectName: "embedFooterRow"
            Layout.fillWidth: true
            visible: root.footerIcon.length > 0 || footerText.text.length > 0
            spacing: 6
            RoundedImage {
                objectName: "embedFooterIcon"
                Layout.preferredWidth: 18; Layout.preferredHeight: 18
                visible: source.length > 0
                source: root.footerIcon
                radius: 0; decodeSize: 36
            }
            Text {
                id: footerText
                objectName: "embedFooterText"
                Layout.fillWidth: true
                text: [String(root.embed.footer || ""), root.timestampLabel].filter(value => value.length > 0).join(" • ")
                textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.secondary
                font.pixelSize: Math.round(10 * preferences.fontScale)
            }
        }
    }
}
