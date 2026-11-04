import QtQuick
import "qrc:/Relay/components/Theme.js" as Theme
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import Relay

Dialog {
    id: dialog
    objectName: "obsStreamingDialog"
    required property var service
    readonly property bool remoteInput: !!service && service.inputMode === 1
    readonly property var streamState: service ? service.broadcastContext : ({})
    parent: Overlay.overlay
    width: Math.min(840, parent.width - 64)
    height: Math.min(790, parent.height - 48)
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    modal: true
    padding: 26
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle { color: Theme.canvas; radius: 8; border.color: Theme.rail }
    onClosed: if (service && !dialog.streamState.active) service.stop()
    contentItem: ColumnLayout {
      spacing: 12
      HardScrollView {
      id: scroll
      Layout.fillWidth: true; Layout.fillHeight: true
      clip: true
      contentWidth: availableWidth
      ColumnLayout {
        width: scroll.availableWidth
        spacing: dialog.height < 700 ? 12 : 18
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                spacing: 5
                Text { text: "OBS streaming"; color: Theme.text; font.pixelSize: Math.round(24 * preferences.fontScale); font.weight: Font.DemiBold }
                Text { visible: dialog.height >= 700; text: "Preview your feed and stream it to your voice channel."; color: Theme.secondary; font.pixelSize: Math.round(12 * preferences.fontScale) }
            }
            Item { Layout.fillWidth: true }
            ActionButton { text: "Close"; quiet: true; onClicked: dialog.close() }
        }
        Input {
            Layout.fillWidth: true
            service: dialog.service
            hostHeight: dialog.height
            previewEnabled: dialog.visible
        }
      }
      }
      Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
      Text {
          Layout.fillWidth: true
          text: dialog.streamState.active ? (dialog.streamState.sharing ? "Live in " + (dialog.streamState.channelName || "voice") : "Starting stream…")
              : dialog.streamState.busy ? "Stop your current stream to share OBS."
              : !dialog.streamState.voiceAvailable ? "Join a server voice channel to stream."
              : !service || !service.receiving ? "Connect the input and wait for video."
              : "Share to " + (dialog.streamState.channelName || "your voice channel")
          textFormat: Text.PlainText; wrapMode: Text.WordWrap
          color: dialog.streamState.sharing ? Theme.success : Theme.secondary
          font.pixelSize: Math.round(12 * preferences.fontScale)
      }
      RowLayout {
          Layout.fillWidth: true; spacing: 10
          ComboBox {
              id: quality; objectName: "obsShareQuality"
              Layout.preferredWidth: 136; model: ["720p", "1080p", "1440p", "Source quality", "4K"]; currentIndex: 3
              enabled: !dialog.streamState.active; Accessible.name: "Stream resolution"
          }
          ComboBox {
              id: rate; objectName: "obsShareRate"
              Layout.preferredWidth: 116; model: ["15 fps", "30 fps", "60 fps", "Source fps"]; currentIndex: 3
              enabled: !dialog.streamState.active; Accessible.name: "Stream frame rate"
          }
          CheckBox {
              id: streamAudio; objectName: "obsShareAudio"
              text: "Stream audio"; checked: true
              enabled: !dialog.streamState.active && !!service && service.hasAudio
              font.pixelSize: Math.round(12 * preferences.fontScale)
          }
          Item { Layout.fillWidth: true }
          ActionButton {
              objectName: "obsShareButton"
              text: dialog.streamState.active ? "Stop sharing" : "Start sharing"
              primary: !dialog.streamState.active; destructive: !!dialog.streamState.active
              enabled: !!dialog.streamState.active || (!!service && service.receiving && service.broadcastAvailable && !!dialog.streamState.voiceAvailable && !dialog.streamState.busy)
              onClicked: if (service) {
                  if (dialog.streamState.active) service.stopBroadcast()
                  else service.requestBroadcast([720,1080,1440,0,2160][quality.currentIndex], [15,30,60,0][rate.currentIndex], streamAudio.checked && service.hasAudio)
              }
          }
      }
      Text {
          Layout.fillWidth: true
          visible: text.length > 0; text: dialog.streamState.quality || ""
          wrapMode: Text.WordWrap; color: Theme.secondary
          font.pixelSize: Math.round(12 * preferences.fontScale)
      }
      Text {
          Layout.fillWidth: true
          visible: text.length > 0; text: dialog.streamState.error || ""
          textFormat: Text.PlainText; wrapMode: Text.WordWrap; color: Theme.danger
          font.pixelSize: Math.round(12 * preferences.fontScale)
      }
    }
}
