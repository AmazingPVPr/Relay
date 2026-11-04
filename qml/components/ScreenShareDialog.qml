import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Theme.js" as Theme

Dialog {
    id: dialog
    objectName: "screenShareDialog"
    property var streamService: typeof relayStreams !== "undefined" ? relayStreams : null
    parent: Overlay.overlay
    width: Math.min(700, parent.width - 64)
    height: Math.min(broadcast.active ? 380 : addonSelected ? 740 : Math.ceil((broadcast.usesSystemPicker ? 545 : 510) * preferences.fontScale), parent.height - 64)
    x: (parent.width - width) / 2; y: (parent.height - height) / 2
    modal: true; padding: 24
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    property string selectedToken: ""
    property var sourceConfiguration: ({})
    property string editorError: ""
    readonly property var sourceChoices: {
        var result = []
        var screens = broadcast.sources
        for (var i = 0; i < screens.length; ++i)
            result.push({ token: "screen:" + screens[i].id, id: screens[i].id, name: screens[i].name, addon: false })
        var contributed = addons.broadcastSources
        for (var j = 0; j < contributed.length; ++j)
            result.push({ token: "addon:" + contributed[j].id, id: contributed[j].id, name: contributed[j].name, addon: true })
        return result
    }
    readonly property var selectedSource: {
        for (var i = 0; i < sourceChoices.length; ++i)
            if (sourceChoices[i].token === selectedToken) return sourceChoices[i]
        return ({})
    }
    readonly property bool addonSelected: !!selectedSource.addon
    readonly property var addonService: sourceConfiguration.service || null
    readonly property var addonState: addonService ? addonService.broadcastContext : ({})
    readonly property bool sourceReady: addonSelected
        ? !!addonService && addonService.running && addonService.receiving && sourceEditor.status === Loader.Ready
        : !!selectedSource.id
    readonly property var qualityLabels: addonSelected ? ["Source quality", "720p", "1080p", "1440p", "4K"] : ["720p", "1080p", "1440p", "4K"]
    readonly property var qualityValues: addonSelected ? [0, 720, 1080, 1440, 2160] : [720, 1080, 1440, 2160]
    readonly property var rateLabels: addonSelected ? ["Source fps", "15 fps", "30 fps", "60 fps"] : ["15 fps", "30 fps", "60 fps"]
    readonly property var rateValues: addonSelected ? [0, 15, 30, 60] : [15, 30, 60]
    property int selectedHeight: 2160
    property int selectedRate: 60
    function selectSource(token) {
        editorError = ""
        selectedToken = token
        sourceConfiguration = ({})
        if (addonSelected) sourceConfiguration = addons.openBroadcastSource(selectedSource.id)
        selectedHeight = addonSelected ? 0 : 2160
        selectedRate = addonSelected ? 0 : 60
        computerAudio.checked = addonSelected
    }
    function updateEditor() {
        if (!sourceEditor) return
        // Read the changed map directly: dependent readonly bindings can still
        // hold their previous value during this property-change handler.
        if (sourceConfiguration.editorUrl && sourceConfiguration.service)
            sourceEditor.setSource(sourceConfiguration.editorUrl, { service: sourceConfiguration.service })
        else sourceEditor.source = ""
    }
    onSourceConfigurationChanged: updateEditor()
    function ensureSelection() {
        if (!selectedSource.token) selectSource(sourceChoices.length ? sourceChoices[0].token : "")
    }
    background: Rectangle { color: Theme.surface; radius: 8; border.color: Theme.border }
    onOpened: {
        broadcast.refreshSources()
        ensureSelection()
        if (addonSelected && !addonService) sourceConfiguration = addons.openBroadcastSource(selectedSource.id)
    }
    onClosed: sourceConfiguration = ({})
    Connections { target: addons; function onItemsChanged() { dialog.ensureSelection() } }
    Connections { target: broadcast; function onSourcesChanged() { dialog.ensureSelection() } }
    contentItem: ColumnLayout {
        spacing: dialog.height < 650 ? 14 : 18
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: broadcast.active ? "You’re sharing a stream" : "Share your screen"; color: Theme.text; font.pixelSize: Math.round(23 * preferences.fontScale); font.weight: Font.DemiBold; wrapMode: Text.WordWrap }
            ActionButton { iconName: "close"; quiet: true; hint: "Close screen-sharing settings"; onClicked: dialog.close() }
        }
        Text { Layout.fillWidth: true; visible: broadcast.active || !dialog.addonSelected; text: broadcast.active ? broadcast.status : "People in your voice channel can watch your screen, a window, or an enabled add-on source."; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
        ColumnLayout {
            Layout.fillWidth: true; visible: !broadcast.active; spacing: 8
            Text { text: "What to share"; color: Theme.text; font.pixelSize: Math.round(14 * preferences.fontScale); font.weight: Font.Medium }
            ComboBox {
                id: source; objectName: "screenShareSource"
                Layout.fillWidth: true; model: dialog.sourceChoices; textRole: "name"
                currentIndex: {
                    for (var i = 0; i < dialog.sourceChoices.length; ++i)
                        if (dialog.sourceChoices[i].token === dialog.selectedToken) return i
                    return -1
                }
                onActivated: index => dialog.selectSource(dialog.sourceChoices[index].token)
                Accessible.name: "What to share"
            }
            Text { Layout.fillWidth: true; visible: !dialog.addonSelected && broadcast.usesSystemPicker; text: "Your desktop will ask you to choose a screen or window when you start."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        }
        HardScrollView {
            id: scroll
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.minimumHeight: visible ? 90 : 0
            visible: !broadcast.active && dialog.addonSelected
            Loader {
                id: sourceEditor; objectName: "screenShareSourceEditor"
                width: scroll.availableWidth
                active: dialog.visible && dialog.addonSelected && !!dialog.sourceConfiguration.editorUrl && !broadcast.active
                onLoaded: if (item && item.hostHeight !== undefined) item.hostHeight = Qt.binding(function() { return Math.min(dialog.height, 640) })
                onStatusChanged: if (status === Loader.Error) dialog.editorError = "The add-on source editor could not be opened."
            }
        }
        Text { Layout.fillWidth: true; visible: !!dialog.sourceConfiguration.error || dialog.editorError.length > 0; text: dialog.sourceConfiguration.error || dialog.editorError; textFormat: Text.PlainText; color: Theme.danger; wrapMode: Text.WordWrap; font.pixelSize: Math.round(12 * preferences.fontScale) }
        ColumnLayout {
            Layout.fillWidth: true; visible: !broadcast.active; spacing: 10
            RowLayout {
                Layout.fillWidth: true; spacing: 16
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 4
                    Text { text: "Resolution"; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale) }
                    ComboBox { id: resolution; objectName: "screenShareResolution"; Layout.fillWidth: true; model: dialog.qualityLabels; currentIndex: dialog.qualityValues.indexOf(dialog.selectedHeight); onActivated: index => dialog.selectedHeight = dialog.qualityValues[index] }
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 4
                    Text { text: "Frame rate"; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale) }
                    ComboBox { id: rate; objectName: "screenShareFrameRate"; Layout.fillWidth: true; model: dialog.rateLabels; currentIndex: dialog.rateValues.indexOf(dialog.selectedRate); onActivated: index => dialog.selectedRate = dialog.rateValues[index] }
                }
                ColumnLayout {
                    Layout.preferredWidth: 190; spacing: 4
                    Text { text: "Audio"; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale) }
                    CheckBox { id: computerAudio; objectName: "screenShareAudio"; text: dialog.addonSelected ? "Share source audio" : "Share computer audio"; checked: false; font.pixelSize: Math.round(12 * preferences.fontScale); Accessible.name: text }
                }
            }
            Text { Layout.fillWidth: true; text: dialog.addonSelected ? "Source quality preserves compatible original video. Audio comes from the input; your microphone stays separate." : "Higher quality uses more upload bandwidth. Computer audio includes all output, which may include voice-channel audio."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        }
        ColumnLayout {
            Layout.fillWidth: true; spacing: 4; visible: dialog.streamService !== null
            RowLayout {
                Layout.fillWidth: true
                CheckBox {
                    objectName: "relayStreamEnabled"; text: "Also share directly with Relay contacts"
                    checked: dialog.streamService ? dialog.streamService.publishEnabled : false
                    onToggled: if (dialog.streamService) dialog.streamService.publishEnabled = checked
                }
                Item { Layout.fillWidth: true }
                Label { text: "Relay Mbps"; color: Theme.secondary }
                SpinBox {
                    objectName: "relayStreamBitrate"; from: 1; to: 40
                    value: dialog.streamService ? dialog.streamService.bitrateMbps : 20
                    onValueModified: if (dialog.streamService) dialog.streamService.bitrateMbps = value
                }
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale)
                text: "Discord viewers use the normal stream. Connected Relay contacts can watch via Relay chat. Each direct viewer uses your upload at the Relay bitrate; compatible OBS video uses its original bitrate. Direct viewers can see your network address."
            }
            Text {
                visible: broadcast.active; color: Theme.secondary
                text: dialog.streamService ? "Relay viewers: " + dialog.streamService.viewerCount : ""
            }
        }
        Text { Layout.fillWidth: true; visible: broadcast.active && broadcast.sourceName.length > 0; text: broadcast.sourceName; textFormat: Text.PlainText; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale); wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; visible: broadcast.active && broadcast.qualitySummary.length > 0; text: broadcast.qualitySummary; textFormat: Text.PlainText; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; visible: dialog.addonSelected && !!dialog.addonState.error; text: dialog.addonState.error || ""; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; visible: broadcast.error.length > 0; text: broadcast.error; textFormat: Text.PlainText; color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap }
        Text { Layout.fillWidth: true; visible: !voice.connected; text: "Join a voice channel to start sharing."; color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale) }
        Item { Layout.fillHeight: true; visible: !dialog.addonSelected || broadcast.active }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ActionButton { text: "Cancel"; quiet: true; onClicked: dialog.close() }
            ActionButton {
                objectName: "screenShareStart"
                text: broadcast.active ? "Stop sharing" : "Start sharing"
                primary: !broadcast.active; destructive: broadcast.active
                enabled: broadcast.active || (voice.connected && !discord.isDirectCall(voice.currentGuildId) && broadcast.available && dialog.sourceReady)
                onClicked: {
                    if (broadcast.active) broadcast.stop()
                    else if (dialog.addonSelected) addonBroadcast.startSource(dialog.selectedSource.id, dialog.selectedHeight, dialog.selectedRate, computerAudio.checked)
                    else broadcast.start(voice.currentGuildId, voice.channelId, dialog.selectedSource.id, dialog.selectedHeight, dialog.selectedRate, computerAudio.checked)
                    if (broadcast.active) dialog.close()
                }
            }
        }
    }
}
