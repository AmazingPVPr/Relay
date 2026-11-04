import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "qrc:/Relay/components"
import "qrc:/Relay/components/Theme.js" as Theme

ColumnLayout {
    id: editor
    objectName: "obsShareSourceEditor"
    required property var service
    property real hostHeight: 720
    property bool previewEnabled: true
    function updatePreview() {
        if (!service || !preview || !preview.videoSink) return
        if (previewEnabled) service.videoSink = preview.videoSink
        else service.releasePreview(preview.videoSink)
    }
    onPreviewEnabledChanged: updatePreview()
    readonly property bool remoteInput: !!service && service.inputMode === 1
    readonly property var streamState: service ? service.broadcastContext : ({})
    spacing: hostHeight < 700 ? 12 : 18
    Component.onCompleted: updatePreview()
    Component.onDestruction: {
        // Releasing this particular preview preserves a pending/active stream.
        // The add-on releases input after sharing ends unless a new editor has
        // attached another sink in the meantime.
        if (service) service.releasePreview(preview.videoSink)
    }
        RowLayout {
            Layout.fillWidth: true
            Text { text: "Input"; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale) }
            ComboBox {
                id: inputMode
                objectName: "obsInputMode"
                Layout.fillWidth: true
                model: ["Direct from OBS", "Existing RTMP stream"]
                currentIndex: service ? service.inputMode : 0
                enabled: !!service && !service.running
                font.pixelSize: Math.round(13 * preferences.fontScale)
                indicator: AppIcon {
                    x: inputMode.width - width - 12; y: (inputMode.height - height) / 2
                    width: 16; height: 16; name: "down"; color: Theme.secondary
                }
                background: Rectangle {
                    radius: 4; color: Theme.rail
                    border.width: 1; border.color: inputMode.activeFocus ? Theme.focus : inputMode.hovered ? Theme.border : "transparent"
                }
                onActivated: index => { if (service) service.inputMode = index }
                Accessible.name: "OBS input source"
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: editor.remoteInput
            spacing: 9
            Text { text: "RTMP server or full stream URL"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
            AppField {
                objectName: "obsRemoteServer"
                Layout.fillWidth: true
                text: service ? service.remoteServer : ""
                placeholderText: "rtmp://server:1935/live"
                enabled: !!service && !service.running
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                font.pixelSize: Math.round(12 * preferences.fontScale)
                onTextEdited: if (service) service.remoteServer = text
                Accessible.name: "RTMP server or full stream URL"
            }
            Text { text: "Stream key"; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
            RowLayout {
                Layout.fillWidth: true
                AppField {
                    id: remoteKey
                    objectName: "obsRemoteStreamKey"
                    Layout.fillWidth: true
                    text: service ? service.remoteStreamKey : ""
                    placeholderText: "Leave blank if the URL already includes it"
                    echoMode: showKey.checked ? TextInput.Normal : TextInput.Password
                    enabled: !!service && !service.running
                    inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                    font.pixelSize: Math.round(12 * preferences.fontScale)
                    onTextEdited: if (service) service.remoteStreamKey = text
                    Accessible.name: "RTMP stream key"
                }
                ActionButton { id: showKey; text: checked ? "Hide" : "Show"; checkable: true; quiet: true; Accessible.name: checked ? "Hide stream key" : "Show stream key" }
            }
            Text {
                Layout.fillWidth: true
                text: "Use the same server and stream key as OBS. Your server can keep feeding your other PC while Relay connects as another viewer."
                wrapMode: Text.WordWrap; color: Theme.secondary; font.pixelSize: Math.round(11 * preferences.fontScale)
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: editor.hostHeight < 700 ? 150 : Math.min(290, editor.width * 0.37)
            color: Theme.rail
            radius: 10
            border.color: Theme.surface
            clip: true
            VideoSurface {
                id: preview
                anchors.fill: parent
                onVideoSinkChanged: editor.updatePreview()
            }
            ColumnLayout {
                anchors.centerIn: parent
                spacing: 11
                visible: !service || !service.receiving
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    width: 46; height: 36; radius: 7
                    color: Theme.sidebar
                    border.color: Theme.border
                    AppIcon { anchors.centerIn: parent; name: "video"; color: Theme.secondary; width: 23; height: 23 }
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: service && service.running ? (editor.remoteInput ? "Connecting to stream" : "Waiting for OBS") : "Your scene will appear here"
                    color: Theme.text; font.pixelSize: Math.round(15 * preferences.fontScale)
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: editor.remoteInput ? "Keep OBS streaming to your RTMP server." : service && service.running ? "Start streaming in OBS to connect." : "Start the preview, then connect OBS."
                    Layout.maximumWidth: preview.width - 32
                    wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter
                    color: Theme.muted; font.pixelSize: Math.round(12 * preferences.fontScale)
                }
            }
            Rectangle {
                anchors.top: parent.top; anchors.left: parent.left; anchors.margins: 12
                width: receivingLabel.implicitWidth + 20; height: 28; radius: 14
                color: "#dc1e1f22"
                visible: !!service && service.receiving
                Text { id: receivingLabel; anchors.centerIn: parent; text: editor.streamState.sharing ? "LIVE" : "INPUT PREVIEW"; font.pixelSize: Math.round(10 * preferences.fontScale); font.weight: Font.Bold; color: Theme.success }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: service ? service.status + (service.receiving && service.resolution ? "  ·  " + service.resolution : "") : "Add-on disabled"
                textFormat: Text.PlainText
                color: service && service.receiving ? Theme.success : Theme.secondary
                font.pixelSize: Math.round(12 * preferences.fontScale)
                elide: Text.ElideRight
            }
            Text {
                visible: !!service && service.receiving
                text: service && service.hasAudio ? "Audio detected" : "No audio detected"
                color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale)
            }
            ActionButton {
                objectName: "obsStartPreview"
                text: service && service.running ? "Stop input" : editor.remoteInput ? "Connect" : "Start preview"
                primary: !service || !service.running
                onClicked: if (service) service.running ? service.stop() : service.start()
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }
        Text { visible: !editor.remoteInput; text: "CONNECT OBS"; color: Theme.secondary; font.pixelSize: Math.round(10 * preferences.fontScale); font.weight: Font.Bold; font.letterSpacing: 1.3 }
        Text {
            visible: !editor.remoteInput
            text: "In OBS, open Settings → Stream and choose Custom. Use H.264 video and AAC audio."
            Layout.fillWidth: true
            wrapMode: Text.WordWrap; color: Theme.text; font.pixelSize: Math.round(13 * preferences.fontScale)
        }
        GridLayout {
            visible: !editor.remoteInput
            columns: 3
            columnSpacing: 12
            rowSpacing: 10
            Layout.fillWidth: true
            Text { text: "Server"; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale) }
            AppField { Layout.fillWidth: true; text: service ? service.obsServer : ""; readOnly: true; font.pixelSize: Math.round(12 * preferences.fontScale); implicitHeight: 37 }
            ActionButton { text: "Copy"; implicitHeight: 37; onClicked: if (service) service.copyObsServer() }
            Text { text: "Stream key"; color: Theme.secondary; font.pixelSize: Math.round(13 * preferences.fontScale) }
            AppField { Layout.fillWidth: true; text: service ? service.streamKey : ""; readOnly: true; font.pixelSize: Math.round(12 * preferences.fontScale); implicitHeight: 37 }
            ActionButton { text: "Copy"; implicitHeight: 37; onClicked: if (service) service.copyStreamKey() }
        }
        Text {
            visible: editor.remoteInput
            Layout.fillWidth: true
            text: "The server must allow playback of this stream. Connecting and sharing always start when you choose."
            wrapMode: Text.WordWrap; color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale)
        }
        RowLayout {
            Layout.fillWidth: true
            CheckBox {
                id: rememberInput; objectName: "obsRememberInput"
                text: "Remember this input"; checked: !!service && service.rememberInput
                enabled: !!service
                onClicked: if (service) service.rememberInput = checked
                Accessible.name: "Remember OBS input securely"
            }
            Item { Layout.fillWidth: true }
            ActionButton {
                objectName: "obsForgetInput"; text: "Forget saved input"; quiet: true
                enabled: !!service && (service.rememberInput || service.inputSaved) && !service.inputStorageBusy
                onClicked: if (service) service.forgetSavedInput()
            }
        }
        Text {
            objectName: "obsInputStorageStatus"; Layout.fillWidth: true
            text: service ? service.inputStorageStatus : ""; visible: text.length > 0
            textFormat: Text.PlainText; wrapMode: Text.WordWrap
            color: Theme.muted; font.pixelSize: Math.round(11 * preferences.fontScale)
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !!service && service.inputStorageError.length > 0
            Text {
                objectName: "obsInputStorageError"; Layout.fillWidth: true
                text: service ? service.inputStorageError : ""; textFormat: Text.PlainText
                wrapMode: Text.WordWrap; color: Theme.danger; font.pixelSize: Math.round(11 * preferences.fontScale)
            }
            ActionButton {
                objectName: "obsRetryInputSave"; text: "Save again"; quiet: true
                enabled: !!service && service.rememberInput && !service.inputStorageBusy
                onClicked: if (service) service.retryInputSave()
            }
        }
        Text {
            visible: !!service && service.error.length > 0
            text: service ? service.error : ""
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.danger; font.pixelSize: Math.round(12 * preferences.fontScale)
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: note.implicitHeight + 24
            color: Theme.sidebar
            radius: 9
            Text {
                id: note
                anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "Source quality keeps the original video up to 4K at 60 fps. Compatible H.264 feeds are sent without compressing the picture again. Stream audio comes from the feed; your microphone stays separate. Closing this window keeps an active stream running."
                color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale); wrapMode: Text.WordWrap
            }
        }

}
