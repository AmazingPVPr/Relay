import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    required property var service
    title: "Example timer"
    modal: true
    width: 360
    standardButtons: Dialog.Close
    contentItem: ColumnLayout {
        Label { text: service ? "Elapsed: " + service.seconds + " seconds" : "Disabled" }
        Button {
            text: service && service.running ? "Stop" : "Start"
            enabled: service !== null
            onClicked: service.running ? service.stop() : service.start()
        }
    }
}
