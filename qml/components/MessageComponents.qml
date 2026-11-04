import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "MediaUrls.js" as MediaUrls
import "Theme.js" as Theme

ColumnLayout {
    id: root
    property var components: []
    property string messageId: ""
    property var metadata: ({})
    property bool playbackEnabled: true
    property string actionError: ""
    property bool actionBusy: false
    function activate(customId,type,values) {
        actionError = ""
        actionBusy = true
        if (!discord.executeComponent(messageId,customId,type,values)) actionBusy = false
    }
    Connections {
        target: discord
        function onComponentError(id,reason) { if (id === root.messageId) { root.actionError = reason; root.actionBusy = false } }
        function onComponentFinished(id,customId) { if (id === root.messageId) root.actionBusy = false }
    }
    signal imageRequested(string source, string originalUrl, string title)
    signal userRequested(string userId)
    spacing: 8
    function cards(values) {
        let output = []
        let budget = 100
        function blocks(items, depth) {
            let result = []
            if (depth > 8) return result
            for (let item of items || []) {
                if (--budget < 0) break
                if (item.spoiler) { result.push({kind:"spoiler",text:"Spoiler content"}); continue }
                if (item.type === 10) result.push({kind:"text",text:item.content || ""})
                else if (item.type === 14) result.push({kind:"separator",divider:item.divider !== false})
                else if (item.type === 1) {
                    const buttons = (item.components || []).filter(c => c.type === 2)
                    if (buttons.length) result.push({kind:"buttons",buttons:buttons})
                    for (let select of item.components || []) if (select.type === 3) result.push({kind:"select",select:select})
                } else if (item.type === 2) result.push({kind:"buttons",buttons:[item]})
                else if (item.type === 12) for (let media of (item.items || []).slice(0,10)) {
                    if (media.spoiler) result.push({kind:"spoiler",text:"Spoiler image"})
                    else result.push({kind:"image",media:media.media || {},title:media.description || "Image"})
                } else if (item.type === 13) result.push({kind:"file",file:item.file || {},name:item.name || item.file?.filename || "Attachment"})
                else if (item.type === 11) result.push({kind:"image",media:item.media || {},title:item.description || "Image"})
                else if (item.components) result = result.concat(blocks(item.components,depth+1))
                if (item.accessory) result = result.concat(blocks([item.accessory],depth+1))
            }
            return result
        }
        for (let item of values || []) {
            if (item.type === 17) output.push({container:true,accent:item.accent_color,blocks:item.spoiler?[{kind:"spoiler",text:"Spoiler content"}]:blocks(item.components,0)})
            else output.push({container:false,blocks:blocks([item],0)})
        }
        return output
    }
    Repeater {
        model: root.cards(root.components)
        delegate: Rectangle {
            id: card
            required property var modelData
            Layout.fillWidth: true
            implicitHeight: stack.implicitHeight + (modelData.container ? 24 : 0)
            color: modelData.container ? Theme.surface : "transparent"
            border.width: modelData.container ? 1 : 0
            border.color: Theme.border
            radius: 6
            Rectangle {
                visible: card.modelData.accent !== undefined && card.modelData.accent !== null
                width: 4; anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
                color: Qt.rgba(((Number(card.modelData.accent)>>16)&255)/255,((Number(card.modelData.accent)>>8)&255)/255,(Number(card.modelData.accent)&255)/255,1)
            }
            ColumnLayout {
                id: stack
                anchors.left: parent.left; anchors.right: parent.right
                anchors.margins: card.modelData.container ? 14 : 0
                y: card.modelData.container ? 12 : 0
                spacing: 10
                Repeater {
                    model: card.modelData.blocks
                    delegate: Loader {
                        id: block
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.maximumWidth: modelData.kind === "image" || modelData.kind === "file" ? 460 : Number.POSITIVE_INFINITY
                        sourceComponent: modelData.kind === "text" ? textBlock : modelData.kind === "buttons" ? buttonBlock : modelData.kind === "select" ? selectBlock : modelData.kind === "image" ? imageBlock : modelData.kind === "file" ? fileBlock : modelData.kind === "separator" ? separatorBlock : spoilerBlock
                        Component {
                            id: textBlock
                            LinkText { rawText: block.modelData.text; metadata: root.metadata; font.pixelSize: Math.round(preferences.chatFontSize); onUserRequested:id=>root.userRequested(id) }
                        }
                        Component {
                            id: buttonBlock
                            Flow {
                                spacing: 7
                                Repeater {
                                    model: block.modelData.buttons
                                    delegate: Button {
                                        id: button
                                        required property var modelData
                                        readonly property var emoji: modelData.emoji || ({})
                                        implicitHeight: 32
                                        implicitWidth: Math.min(parent.width, label.implicitWidth + 24)
                                        enabled: !root.actionBusy && !modelData.disabled && (modelData.style === 5 || String(modelData.custom_id || "").length > 0)
                                        hoverEnabled: true
                                        padding: 10; topPadding: 5; bottomPadding: 5
                                        background: Rectangle { radius: 5; color: button.modelData.style === 1 ? button.hovered ? "#6466eb" : "#5456d6" : button.modelData.style === 3 ? "#287d50" : button.modelData.style === 4 ? "#a63b42" : button.hovered ? Theme.selected : Theme.border; opacity: button.enabled ? 1 : .45 }
                                        contentItem: RowLayout {
                                            id: label
                                            spacing: 6
                                            Image { visible: !!button.emoji.id; source: button.emoji.id ? "https://cdn.discordapp.com/emojis/" + button.emoji.id + ".png?size=128" : ""; asynchronous: true; Layout.preferredWidth: 18; Layout.preferredHeight: 18; sourceSize: Qt.size(64,64); fillMode: Image.PreserveAspectFit }
                                            Text { visible: !button.emoji.id && !!button.emoji.name; text: button.emoji.name || ""; color: Theme.text; font.pixelSize: 16; textFormat: Text.PlainText }
                                            Text { text: button.modelData.label || ""; visible: text.length > 0; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(12 * preferences.fontScale); elide: Text.ElideRight }
                                            AppIcon { visible: button.modelData.style === 5; name: "popout"; width: 14; height: 14; color: Theme.text }
                                        }
                                        onClicked: button.modelData.style === 5 ? discord.openLink(button.modelData.url || "") : root.activate(button.modelData.custom_id, 2, [])
                                        ToolTip.visible: hovered && modelData.style === 5
                                        ToolTip.text: modelData.url || ""
                                        ToolTip.delay: 600
                                    }
                                }
                            }
                        }
                        Component {
                            id: selectBlock
                            ColumnLayout {
                                ComboBox {
                                    id: choice
                                    Layout.fillWidth: true
                                    model: block.modelData.select.options || []
                                    textRole: "label"; valueRole: "value"
                                    currentIndex: -1
                                    displayText: currentIndex < 0 ? (block.modelData.select.placeholder || "Choose an option") : currentText
                                    enabled: !root.actionBusy && !block.modelData.select.disabled && Number(block.modelData.select.max_values || 1) === 1
                                    onActivated: root.activate(block.modelData.select.custom_id,3,[currentValue])
                                }
                                Text { visible: !choice.enabled && !block.modelData.select.disabled; text: "This control requires multiple selections."; color: Theme.muted; font.pixelSize: 12 }
                            }
                        }
                        Component {
                            id: imageBlock
                            Loader {
                                readonly property var media: block.modelData.media || ({})
                                sourceComponent: media.isVideo || MediaUrls.isVideo(media.videoUrl || media.url || media.proxy_url, media.content_type) ? videoMedia : imageMedia
                                Component {
                                    id: videoMedia
                                    InlineVideoPlayer {
                                        sourceUrl: block.modelData.media.videoUrl || block.modelData.media.url || block.modelData.media.proxy_url || ""
                                        posterUrl: block.modelData.media.posterUrl || ""
                                        title: block.modelData.title || "Video"
                                        mediaWidth: Number(block.modelData.media.width) || 0; mediaHeight: Number(block.modelData.media.height) || 0
                                        gif: !!block.modelData.media.isGif
                                        playbackEnabled: root.playbackEnabled
                                    }
                                }
                                Component {
                                    id: imageMedia
                                    ImagePreview {
                                        source: MediaUrls.cdnImage(block.modelData.media.proxy_url || block.modelData.media.url || "")
                                        originalUrl: source; title: block.modelData.title
                                        mediaWidth: Number(block.modelData.media.width) || 0; mediaHeight: Number(block.modelData.media.height) || 0
                                        animated: !!block.modelData.media.isGif || MediaUrls.isGif(source)
                                        playbackEnabled: root.playbackEnabled
                                        onActivated: (source, originalUrl, title) => root.imageRequested(source, originalUrl, title)
                                    }
                                }
                            }
                        }
                        Component {
                            id: fileBlock
                            AttachmentItem {
                                playbackEnabled: root.playbackEnabled
                                attachment: Object.assign({}, block.modelData.file, {filename:block.modelData.name,url:block.modelData.file.url || block.modelData.file.proxy_url || "",previewUrl:block.modelData.file.posterUrl || ""})
                                onImageRequested: (source, originalUrl, title) => root.imageRequested(source, originalUrl, title)
                            }
                        }
                        Component { id: separatorBlock; Rectangle { implicitHeight: block.modelData.divider ? 1 : 8; color: block.modelData.divider ? Theme.border : "transparent" } }
                        Component { id: spoilerBlock; Text { text: block.modelData.text + " hidden"; color: Theme.muted; font.pixelSize: 12 } }
                    }
                }
            }
        }
    }
    Text { Layout.fillWidth: true; visible: root.actionError.length > 0; text: root.actionError; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: 12; wrapMode: Text.Wrap }
}
